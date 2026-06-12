// The resolving CelValue codec — read/write 24-byte CelValues that may
// reference host externref backings (MESSAGE / LIST_HOST / MAP_HOST).
//
// The plain codec (`./celvalue.js`) decodes only the kinds whose bytes
// live in linear memory; it THROWS on the three externref-slot kinds
// because resolving them needs the host-side {@link ExternrefTable},
// which the codec does not own.  This module is the layer that DOES own
// the table: it wraps `readCelValue` with a resolver for the externref
// kinds, and provides the matching `writeValue` encoder that interns a
// nested JS aggregate / message back into the table.  Together those two
// methods satisfy the `AggregateContext` / `ProtoContext.codec` contracts
// the `cel_host` trampolines (`./host/*.js`) are written against.
//
// The encoder mirrors the C++ `EncodeValueToSlot` (`eval/internal/
// cel_host.h:803`): scalars stamp inline; string / bytes arena-copy their
// payload and stamp a span; a JS array / Map / message-object interns into
// the externref table and stamps the `CEL_*_HOST` / `CEL_MESSAGE` handle.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md
//       §A.4.1 (codec), §A.4.5 (externref tables).

import {
  readCelValue,
  writeScalarBool,
  writeScalarDouble,
  writeScalarInt,
  writeScalarNull,
  writeSpan,
  encodeUtf8,
  CelExternrefBoundaryError,
} from './celvalue.js';
import type { ExternrefTable } from './externref.js';
import type {
  HostListBacking,
  HostMapBacking,
  HostMapEntry,
} from './host/aggregates.js';
import { ProtoMessageBacking, messageToObject } from './proto/backing.js';
import {
  CEL_DURTS_NANOS_OFFSET,
  CEL_DURTS_SECONDS_OFFSET,
  CEL_VALUE_KIND_OFFSET,
  CEL_VALUE_PAYLOAD_OFFSET,
  CelKind,
} from './types.js';
import type { CelValue } from './types.js';

/**
 * The runtime hooks the codec needs but does not own: a fresh `DataView`
 * / `Uint8Array` over the Program's current linear memory (re-read per
 * call because wasm memory growth detaches the backing `ArrayBuffer`),
 * the per-Eval externref table, and the arena allocator that copies
 * string / bytes / aggregate payloads into linear memory.
 */
export interface CodecEnv {
  /** A `DataView` over the Program's CURRENT linear memory. */
  view(): DataView;
  /** A `Uint8Array` over the SAME current linear memory `view()` covers. */
  bytes(): Uint8Array;
  /** The three host-handle namespaces (message / map / list). */
  readonly refs: ExternrefTable;
  /**
   * Reserve `n` bytes of the Program's per-Eval arena and return the
   * linear-memory offset (the same allocator the expr module uses via
   * its `arena_alloc` export).  Returns 0 on OOM.
   */
  arenaAlloc(n: number): number;
}

/**
 * Read the CelValue at `slot`, resolving the externref kinds (MESSAGE /
 * LIST_HOST / MAP_HOST) against the externref table to their JS-natural
 * shape: a message → its decoded object (protobufjs `toObject` via
 * {@link messageToObject}), a host list → an array, a host map → a `Map`.
 * Every other kind delegates to the plain {@link readCelValue}.
 *
 * Mirrors the C++ `DecodeCelValueAt` (`eval/instance.cc`): the host
 * decode that turns a `CEL_*_HOST` slot back into an owned JS value whose
 * lifetime outlives the externref table's between-Eval reset.
 */
export function resolveCelValue(env: CodecEnv, slot: number): CelValue {
  const view = env.view();
  const kind = view.getUint32(
    slot + CEL_VALUE_KIND_OFFSET,
    /* littleEndian */ true,
  ) as CelKind;
  switch (kind) {
    case CelKind.MESSAGE:
      return resolveMessage(env, slot);
    case CelKind.LIST_HOST:
      return resolveHostList(env, slot);
    case CelKind.MAP_HOST:
      return resolveHostMap(env, slot);
    default:
      // The plain codec decodes every in-linear-memory kind; only the
      // three externref kinds above reach it as a throw, and those are
      // handled here, so a boundary error escaping is a wire-corruption
      // bug (e.g. an unexpected discriminant) and propagates.
      return readCelValue(view, slot, env.bytes());
  }
}

/** The externref `ref_slot` payload u32 of an externref-kind CelValue. */
function readRefSlot(view: DataView, slot: number): number {
  return view.getUint32(
    slot + CEL_VALUE_PAYLOAD_OFFSET,
    /* littleEndian */ true,
  );
}

/** Resolve a CEL_MESSAGE slot to its decoded plain object, or `null`. */
function resolveMessage(env: CodecEnv, slot: number): CelValue {
  const ref = readRefSlot(env.view(), slot);
  const backing = env.refs.message.lookup(ref);
  if (!(backing instanceof ProtoMessageBacking)) {
    // A wild / empty message slot decodes to null rather than crashing
    // the host (the non-throwing externref lookup contract, §A.4.5).
    return null;
  }
  return messageToObject(backing.raw);
}

/** Resolve a CEL_LIST_HOST slot to a JS array of its decoded elements. */
function resolveHostList(env: CodecEnv, slot: number): CelValue {
  const ref = readRefSlot(env.view(), slot);
  const backing = env.refs.list.lookup(ref) as HostListBacking | undefined;
  if (backing === undefined) {
    return [];
  }
  return [...backing.elements];
}

/** Resolve a CEL_MAP_HOST slot to a JS `Map` of its decoded entries. */
function resolveHostMap(env: CodecEnv, slot: number): CelValue {
  const ref = readRefSlot(env.view(), slot);
  const backing = env.refs.map.lookup(ref) as HostMapBacking | undefined;
  const out = new Map<CelValue, CelValue>();
  if (backing === undefined) {
    return out;
  }
  for (const entry of backing.entries) {
    out.set(entry.key, entry.value);
  }
  return out;
}

/**
 * Encode `value` into the 24-byte CelValue at `slot`, handling every
 * kind the trampolines / marshal produce.  Mirrors the C++
 * `EncodeValueToSlot` (`cel_host.h:803`):
 *
 *   - null / bool / int (bigint) / uint / double → stamp inline.
 *   - string / bytes → arena-copy the payload, stamp a `{ptr,len}` span.
 *   - timestamp / duration tagged records → stamp the `CelDurTs` payload.
 *   - error tagged record → stamp kind + code.
 *   - array → intern a {@link HostListBacking}, stamp a CEL_LIST_HOST slot.
 *   - Map → intern a {@link HostMapBacking}, stamp a CEL_MAP_HOST slot.
 *   - message object → coerce/intern a `ProtoMessageBacking`, stamp a
 *     CEL_MESSAGE slot.  (Decoded message OBJECTS round-trip back as host
 *     maps only when not bound to a message type; the trampolines that
 *     produce message values write the slot directly, so the object arm
 *     here is the nested-aggregate fallback.)
 *
 * `value` is an already-decoded {@link CelValue}; an `int`/`uint`
 * distinction is lost once it is a JS `bigint`, so a bigint encodes as
 * CEL_INT — the kinds that need the uint discriminant (a uint map key
 * round-tripped through a trampoline) are re-encoded from the backing's
 * own CelValue, which preserves the kind via this same path on the way in.
 */
export function encodeCelValue(
  env: CodecEnv,
  slot: number,
  value: CelValue,
): void {
  const view = env.view();
  if (value === null) {
    writeScalarNull(view, slot);
    return;
  }
  if (typeof value === 'boolean') {
    writeScalarBool(view, slot, value);
    return;
  }
  if (typeof value === 'bigint') {
    writeScalarInt(view, slot, value);
    return;
  }
  if (typeof value === 'number') {
    writeScalarDouble(view, slot, value);
    return;
  }
  if (typeof value === 'string') {
    writeStringOrBytes(env, slot, CelKind.STRING, encodeUtf8(value));
    return;
  }
  if (value instanceof Uint8Array) {
    writeStringOrBytes(env, slot, CelKind.BYTES, value);
    return;
  }
  if (Array.isArray(value)) {
    internHostList(env, slot, value);
    return;
  }
  if (value instanceof Map) {
    internHostMap(env, slot, value);
    return;
  }
  // A tagged record (timestamp / duration / error) or a plain message
  // object.
  encodeRecord(env, slot, value);
}

/** Stamp a STRING / BYTES / TYPE span, arena-copying the payload bytes first. */
function writeStringOrBytes(
  env: CodecEnv,
  slot: number,
  kind: CelKind.STRING | CelKind.BYTES | CelKind.TYPE,
  payload: Uint8Array,
): void {
  let ptr = 0;
  if (payload.length > 0) {
    ptr = env.arenaAlloc(payload.length);
    // arena_alloc may grow memory; re-read the byte view before writing.
    env.bytes().set(payload, ptr);
  }
  writeSpan(env.view(), slot, kind, ptr, payload.length);
}

/** Intern `elements` as a host list and stamp a CEL_LIST_HOST slot. */
function internHostList(
  env: CodecEnv,
  slot: number,
  elements: readonly CelValue[],
): void {
  const backing: HostListBacking = { elements: [...elements] };
  const ref = env.refs.list.intern(backing);
  writeHostHandle(env.view(), slot, CelKind.LIST_HOST, ref);
}

/**
 * Intern `backing` into the externref message table and stamp a
 * CEL_MESSAGE handle at `slot` — the message-return seam.  Mirrors the
 * C++ `HostCallContext::ReturnProto` (`eval/host_call_context.cc:549`),
 * whose `EncodeValueToSlot` interns the owned message into the per-eval
 * externref table and writes the CEL_MESSAGE kind + ref_slot payload.
 */
export function internMessageBacking(
  env: CodecEnv,
  slot: number,
  backing: ProtoMessageBacking,
): void {
  const ref = env.refs.message.intern(backing);
  writeHostHandle(env.view(), slot, CelKind.MESSAGE, ref);
}

/** Intern `map` as a host map and stamp a CEL_MAP_HOST slot. */
function internHostMap(
  env: CodecEnv,
  slot: number,
  map: ReadonlyMap<CelValue, CelValue>,
): void {
  const entries: HostMapEntry[] = [];
  for (const [key, value] of map) {
    entries.push({ key, value });
  }
  const backing: HostMapBacking = { entries };
  const ref = env.refs.map.intern(backing);
  writeHostHandle(env.view(), slot, CelKind.MAP_HOST, ref);
}

/**
 * Encode a tagged record (timestamp / duration / type / error) or a
 * plain message-shaped object.  A `{kind:
 * 'timestamp'|'duration'|'type'|'error'}` record stamps its native
 * CelValue; any other plain object is a decoded message — re-interned as
 * a host map (its decoded field object) so a nested message round-trips
 * structurally.
 */
function encodeRecord(
  env: CodecEnv,
  slot: number,
  value: Exclude<
    CelValue,
    null | boolean | bigint | number | string | Uint8Array | CelValue[]
  >,
): void {
  if (value instanceof Map) {
    internHostMap(env, slot, value);
    return;
  }
  const tag = (value as { kind?: unknown }).kind;
  if (tag === 'timestamp') {
    const ts = value as { epochSeconds: bigint; nanos: number };
    writeDurTs(env.view(), slot, CelKind.TIMESTAMP, ts.epochSeconds, ts.nanos);
    return;
  }
  if (tag === 'duration') {
    const dur = value as { seconds: bigint; nanos: number };
    writeDurTs(env.view(), slot, CelKind.DURATION, dur.seconds, dur.nanos);
    return;
  }
  if (tag === 'error') {
    writeErrorValue(env.view(), slot, (value as { code: number }).code);
    return;
  }
  if (tag === 'type') {
    // CEL_TYPE is a span over the type-name bytes (`cel_data.h:164-174`);
    // arena-copy the name so runtime equality (a memcmp on the span)
    // sees the bytes in linear memory.
    const name = (value as { name: string }).name;
    writeStringOrBytes(env, slot, CelKind.TYPE, encodeUtf8(name));
    return;
  }
  // A decoded message object: intern it as a host map of its fields so it
  // round-trips structurally without a descriptor at this layer.
  const map = new Map<CelValue, CelValue>();
  for (const [k, v] of Object.entries(value as Record<string, CelValue>)) {
    map.set(k, v);
  }
  internHostMap(env, slot, map);
}

/** Stamp a `CEL_*_HOST` / `CEL_MESSAGE` handle: kind + ref_slot payload. */
function writeHostHandle(
  view: DataView,
  slot: number,
  kind: CelKind,
  ref: number,
): void {
  view.setUint32(slot + CEL_VALUE_KIND_OFFSET, kind, /* littleEndian */ true);
  view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, ref, true);
}

/** Stamp a TIMESTAMP / DURATION `CelDurTs` payload (seconds@8, nanos@16). */
function writeDurTs(
  view: DataView,
  slot: number,
  kind: CelKind.TIMESTAMP | CelKind.DURATION,
  seconds: bigint,
  nanos: number,
): void {
  view.setUint32(slot + CEL_VALUE_KIND_OFFSET, kind, /* littleEndian */ true);
  view.setBigInt64(slot + CEL_DURTS_SECONDS_OFFSET, seconds, true);
  view.setInt32(slot + CEL_DURTS_NANOS_OFFSET, nanos, true);
}

/** Stamp a CEL_ERROR CelValue: kind + numeric code at the payload. */
function writeErrorValue(view: DataView, slot: number, code: number): void {
  view.setUint32(
    slot + CEL_VALUE_KIND_OFFSET,
    CelKind.ERROR,
    /* littleEndian */ true,
  );
  view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, code, true);
}

// Re-exported for the assembly's convenience: the boundary error type the
// plain codec throws, so a caller of {@link resolveCelValue} can narrow a
// truly-unexpected externref escape (there should be none).
export { CelExternrefBoundaryError };
