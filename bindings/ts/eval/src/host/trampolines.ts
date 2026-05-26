/**
 * `cel_host.*` trampolines — the TS counterpart to the Layer-2 entry
 * points in `compiler_v2/api/internal/cel_host.cc` (`CelGetFieldImpl`,
 * `CelListAtImpl`, `CelMapLookupImpl`, …). Each reads operand slots from
 * linear memory, dereferences the host backing through the externref
 * table, reads it, and encodes the result `HostValue` back into the
 * `out_slot` (scalars inline, strings/bytes via the arena, nested
 * aggregates by interning). Backing-agnostic: proto or object backings
 * behave identically (P-6/P-9).
 *
 * 3VL: an UNKNOWN / ERROR operand is copied through to `out_slot` without
 * touching the backing (matches the runtime dispatcher + the C++ Impls).
 */
import {
  CelKind,
  decodeCelValue,
  encodeInlineScalar,
  type CelValue,
} from '../celvalue.js';
import type { FieldEntry, TypeEntry } from '../abi.js';
import type { ExternrefTable } from '../externref.js';
import type { TypeRegistry } from '../type-registry.js';
import type {
  HostValue,
  ListBacking,
  MapBacking,
  MessageBacking,
} from './backing.js';
import { ProtoMessageBacking } from './proto-backing.js';
import { createMessage, setField } from './message-builder.js';

/** Wire error codes (`runtime/cel_data.h` `CEL_ERR_*`). */
const ERR = {
  TYPE_MISMATCH: 13,
  NO_SUCH_KEY: 15,
  INDEX_OUT_OF_BOUNDS: 17,
  FIELD_NOT_FOUND: 20,
  HOST_ADAPTER: 41,
} as const;

const CELVALUE_SIZE = 24;
const PAYLOAD = 8;

export class TrampolineError extends Error {
  public override readonly name = 'TrampolineError';
}

/** Per-eval state the trampolines read. Built by `Engine.plan` from the
 *  decoded `cel.abi` + the runtime's `arena_alloc` + the externref table. */
export interface TrampolineContext {
  readonly memory: WebAssembly.Memory;
  readonly refs: ExternrefTable<MessageBacking, MapBacking, ListBacking>;
  /** `field_ref_id` → field entry (from `cel.abi.fields`). */
  readonly fields: ReadonlyMap<number, FieldEntry>;
  /** `type_id` → type entry (from `cel.abi.types`), for `cel_make_message`. */
  readonly types: ReadonlyMap<number, TypeEntry>;
  /** Descriptors for message construction; absent when the embedder bound
   *  no proto types (then `cel_make_message` reports a type error). */
  readonly registry?: TypeRegistry | undefined;
  /** Runtime `arena_alloc(size) -> offset` for string/bytes results. */
  readonly arenaAlloc: (size: number) => number;
}

function view(ctx: TrampolineContext): DataView {
  return new DataView(ctx.memory.buffer);
}

function writeError(dv: DataView, slot: number, code: number): void {
  dv.setUint32(slot, CelKind.Error, true);
  dv.setUint32(slot + 4, 0, true);
  dv.setUint32(slot + PAYLOAD, code, true);
}

function writeBool(dv: DataView, slot: number, b: boolean): void {
  dv.setUint32(slot, CelKind.Bool, true);
  dv.setUint32(slot + 4, 0, true);
  dv.setInt32(slot + PAYLOAD, b ? 1 : 0, true);
}

function writeInt(dv: DataView, slot: number, n: bigint): void {
  dv.setUint32(slot, CelKind.Int, true);
  dv.setUint32(slot + 4, 0, true);
  dv.setBigInt64(slot + PAYLOAD, n, true);
}

// Write a string/bytes span: copy the bytes into a fresh arena region and
// stamp {kind, ptr, len}.
function writeSpan(
  ctx: TrampolineContext,
  slot: number,
  kind: number,
  bytes: Uint8Array,
): void {
  const ptr = ctx.arenaAlloc(Math.max(bytes.length, 1));
  new Uint8Array(ctx.memory.buffer).set(bytes, ptr);
  const dv = view(ctx);
  dv.setUint32(slot, kind, true);
  dv.setUint32(slot + 4, 0, true);
  dv.setUint32(slot + PAYLOAD, ptr, true);
  dv.setUint32(slot + PAYLOAD + 4, bytes.length, true);
}

function writeRef(dv: DataView, slot: number, kind: number, ref: number): void {
  dv.setUint32(slot, kind, true);
  dv.setUint32(slot + 4, 0, true);
  dv.setUint32(slot + PAYLOAD, ref, true);
}

/** Encode a `HostValue` into the 24-byte cell at `slot`. */
export function encodeHostValue(
  ctx: TrampolineContext,
  slot: number,
  hv: HostValue,
): void {
  switch (hv.host) {
    case 'scalar': {
      const v = hv.value;
      const utf8 = new TextEncoder();
      switch (v.kind) {
        case CelKind.String:
          writeSpan(ctx, slot, CelKind.String, utf8.encode(v.value));
          return;
        case CelKind.Bytes:
          writeSpan(ctx, slot, CelKind.Bytes, v.bytes);
          return;
        case CelKind.Error:
          writeError(view(ctx), slot, v.errorCode);
          return;
        default:
          // null / bool / int / uint / double encode inline.
          encodeInlineScalar(new Uint8Array(ctx.memory.buffer), slot, v);
          return;
      }
    }
    case 'message':
      writeRef(
        view(ctx),
        slot,
        CelKind.Message,
        ctx.refs.internMessage(hv.backing),
      );
      return;
    case 'list':
      writeRef(
        view(ctx),
        slot,
        CelKind.ListHost,
        ctx.refs.internList(hv.backing),
      );
      return;
    case 'map':
      writeRef(
        view(ctx),
        slot,
        CelKind.MapHost,
        ctx.refs.internMap(hv.backing),
      );
      return;
  }
}

// Copy a 24-byte UNKNOWN/ERROR operand straight through to `out`; returns
// true if it did (the caller then returns early). 3VL absorb.
function absorb(ctx: TrampolineContext, out: number, operand: number): boolean {
  const kind = view(ctx).getUint32(operand, true);
  if (kind === CelKind.Unknown || kind === CelKind.Error) {
    new Uint8Array(ctx.memory.buffer).copyWithin(
      out,
      operand,
      operand + CELVALUE_SIZE,
    );
    return true;
  }
  return false;
}

export function celGetField(
  ctx: TrampolineContext,
  out: number,
  msgSlot: number,
  fieldRefId: number,
): void {
  if (absorb(ctx, out, msgSlot)) return;
  const dv = view(ctx);
  const backing = ctx.refs.lookupMessage(dv.getUint32(msgSlot + PAYLOAD, true));
  const field = ctx.fields.get(fieldRefId);
  if (backing === undefined || field === undefined) {
    writeError(
      dv,
      out,
      backing === undefined ? ERR.HOST_ADAPTER : ERR.FIELD_NOT_FOUND,
    );
    return;
  }
  const hv = backing.getField(field.name);
  if (hv === undefined) {
    writeError(dv, out, ERR.FIELD_NOT_FOUND);
    return;
  }
  encodeHostValue(ctx, out, hv);
}

export function celHasField(
  ctx: TrampolineContext,
  out: number,
  msgSlot: number,
  fieldRefId: number,
): void {
  if (absorb(ctx, out, msgSlot)) return;
  const dv = view(ctx);
  const backing = ctx.refs.lookupMessage(dv.getUint32(msgSlot + PAYLOAD, true));
  const field = ctx.fields.get(fieldRefId);
  if (backing === undefined || field === undefined) {
    writeError(
      dv,
      out,
      backing === undefined ? ERR.HOST_ADAPTER : ERR.FIELD_NOT_FOUND,
    );
    return;
  }
  writeBool(dv, out, backing.hasField(field.name));
}

export function celListAt(
  ctx: TrampolineContext,
  out: number,
  listSlot: number,
  indexSlot: number,
): void {
  if (absorb(ctx, out, listSlot) || absorb(ctx, out, indexSlot)) return;
  const dv = view(ctx);
  const backing = ctx.refs.lookupList(dv.getUint32(listSlot + PAYLOAD, true));
  if (backing === undefined) {
    writeError(dv, out, ERR.HOST_ADAPTER);
    return;
  }
  if (dv.getUint32(indexSlot, true) !== CelKind.Int) {
    writeError(dv, out, ERR.TYPE_MISMATCH);
    return;
  }
  const hv = backing.at(Number(dv.getBigInt64(indexSlot + PAYLOAD, true)));
  if (hv === undefined) {
    writeError(dv, out, ERR.INDEX_OUT_OF_BOUNDS);
    return;
  }
  encodeHostValue(ctx, out, hv);
}

export function celListSize(
  ctx: TrampolineContext,
  out: number,
  listSlot: number,
): void {
  if (absorb(ctx, out, listSlot)) return;
  const dv = view(ctx);
  const backing = ctx.refs.lookupList(dv.getUint32(listSlot + PAYLOAD, true));
  if (backing === undefined) {
    writeError(dv, out, ERR.HOST_ADAPTER);
    return;
  }
  writeInt(dv, out, BigInt(backing.size));
}

export function celMapLookup(
  ctx: TrampolineContext,
  out: number,
  mapSlot: number,
  keySlot: number,
): void {
  if (absorb(ctx, out, mapSlot) || absorb(ctx, out, keySlot)) return;
  const dv = view(ctx);
  const backing = ctx.refs.lookupMap(dv.getUint32(mapSlot + PAYLOAD, true));
  if (backing === undefined) {
    writeError(dv, out, ERR.HOST_ADAPTER);
    return;
  }
  const key: CelValue = decodeCelValue(
    new Uint8Array(ctx.memory.buffer),
    keySlot,
  );
  const hv = backing.get(key);
  if (hv === undefined) {
    writeError(dv, out, ERR.NO_SUCH_KEY);
    return;
  }
  encodeHostValue(ctx, out, hv);
}

export function celMapSize(
  ctx: TrampolineContext,
  out: number,
  mapSlot: number,
): void {
  if (absorb(ctx, out, mapSlot)) return;
  const dv = view(ctx);
  const backing = ctx.refs.lookupMap(dv.getUint32(mapSlot + PAYLOAD, true));
  if (backing === undefined) {
    writeError(dv, out, ERR.HOST_ADAPTER);
    return;
  }
  writeInt(dv, out, BigInt(backing.size));
}

/** Allocate an empty message of `cel.abi.types[typeId]` and write a
 *  CEL_MESSAGE ref to `out`. Args are (type_id, out_slot) per the codegen
 *  call sequence. A type with no resolvable descriptor (no registry / FQN
 *  not registered) yields CEL_ERROR(type_mismatch). */
export function celMakeMessage(
  ctx: TrampolineContext,
  typeId: number,
  out: number,
): void {
  const dv = view(ctx);
  const entry = ctx.types.get(typeId);
  const desc =
    entry === undefined
      ? undefined
      : ctx.registry?.getMessage(entry.fullyQualifiedName);
  if (desc === undefined) {
    writeError(dv, out, ERR.TYPE_MISMATCH);
    return;
  }
  writeRef(
    dv,
    out,
    CelKind.Message,
    ctx.refs.internMessage(createMessage(desc)),
  );
}

/** Set field `field_ref_id` on the message at `msgSlot` to the value at
 *  `valueSlot` (mutates the interned backing in place; no out slot). The
 *  target must be a host-constructed proto message (`cel_make_message`);
 *  an unsupported field/value throws (surfaced as a trap). */
export function celSetField(
  ctx: TrampolineContext,
  msgSlot: number,
  fieldRefId: number,
  valueSlot: number,
): void {
  const dv = view(ctx);
  const backing = ctx.refs.lookupMessage(dv.getUint32(msgSlot + PAYLOAD, true));
  const field = ctx.fields.get(fieldRefId);
  if (!(backing instanceof ProtoMessageBacking) || field === undefined) {
    throw new TrampolineError(
      `cel_set_field: not a constructed message, or unknown field_ref ${fieldRefId}`,
    );
  }
  // A scalar / null value decodes self-contained; a message-valued field
  // (CEL_MESSAGE at valueSlot) makes the codec throw — not built yet.
  const value = decodeCelValue(new Uint8Array(ctx.memory.buffer), valueSlot);
  setField(backing, field.name, value);
}

/** Build the `cel_host` import namespace: the implemented read
 *  trampolines, with a trap-stub for every not-yet-implemented entry. */
export function makeCelHostImports(
  ctx: TrampolineContext,
): WebAssembly.ModuleImports {
  function notImplemented(name: string): () => never {
    return (): never => {
      throw new TrampolineError(
        `cel_host.${name} not implemented (Slice C+ trampoline)`,
      );
    };
  }
  // Every `cel_host.*` the expr/runtime modules may import (P-3 + the
  // cel_host_wasmtime.cc trampoline table). The six read trampolines are
  // implemented; the rest are explicit trap-stubs (later slices).
  return {
    cel_get_field: (out: number, msg: number, fieldRef: number): void => {
      celGetField(ctx, out, msg, fieldRef);
    },
    cel_has_field: (out: number, msg: number, fieldRef: number): void => {
      celHasField(ctx, out, msg, fieldRef);
    },
    cel_list_at: (out: number, list: number, idx: number): void => {
      celListAt(ctx, out, list, idx);
    },
    cel_list_size: (out: number, list: number): void => {
      celListSize(ctx, out, list);
    },
    cel_map_lookup: (out: number, map: number, key: number): void => {
      celMapLookup(ctx, out, map, key);
    },
    cel_map_size: (out: number, map: number): void => {
      celMapSize(ctx, out, map);
    },
    cel_map_iter_open: notImplemented('cel_map_iter_open'),
    cel_list_iter_open: notImplemented('cel_list_iter_open'),
    cel_list_in: notImplemented('cel_list_in'),
    cel_list_eq: notImplemented('cel_list_eq'),
    cel_list_concat: notImplemented('cel_list_concat'),
    cel_map_in: notImplemented('cel_map_in'),
    cel_map_eq: notImplemented('cel_map_eq'),
    cel_message_eq: notImplemented('cel_message_eq'),
    cel_make_message: (typeId: number, out: number): void => {
      celMakeMessage(ctx, typeId, out);
    },
    cel_set_field: (msg: number, fieldRef: number, value: number): void => {
      celSetField(ctx, msg, fieldRef, value);
    },
    resolve_message_type_name: notImplemented('resolve_message_type_name'),
    cel_timestamp_tz_accessor: notImplemented('cel_timestamp_tz_accessor'),
    cel_wkt_unwrap_time: notImplemented('cel_wkt_unwrap_time'),
    cel_wkt_unwrap_wrapper: notImplemented('cel_wkt_unwrap_wrapper'),
  };
}
