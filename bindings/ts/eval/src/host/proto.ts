// The `cel_host.*` proto / WKT / message trampolines, in TypeScript.
//
// These are the host-import functions a compiled Program calls for every
// proto operation: field reads, presence, literal construction, WKT
// unwrap, message equality / emptiness, and `type(<message>)` FQN
// resolution.  Each is `(out, ...operands)` over i32 linear-memory slot
// offsets — it reads its operands through the codec, resolves message
// backings via the host externref table, looks field/type refs up in the
// ABI `fields[]` / `types[]` intern tables, **absorbs UNKNOWN / ERROR on
// its inputs**, computes, and **writes a CelValue** into `out`.  Spec
// errors (FIELD_NOT_FOUND, TYPE_MISMATCH, …) are CEL_ERROR *values*,
// never thrown — only an infrastructure bug throws.
//
// Authoritative signatures: `eval/internal/cel_host.h:520-790` (the C++
// trampolines these mirror byte-for-byte on the wire).  The import names
// (`cel_get_field`, `cel_has_field`, `cel_make_message`, `cel_set_field`,
// `cel_wkt_unwrap_time`, `cel_wkt_unwrap_wrapper`, `cel_message_eq`,
// `cel_message_is_zero`, `resolve_message_type_name`) are the empirically
// confirmed `cel_host` module exports.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md
//       §A.4.5 (trampolines), §A.4.6 (descriptors / backings).

import * as protobuf from 'protobufjs';

import type { ExternrefTable } from '../externref.js';
import {
  ProtoFieldRangeError,
  ProtoMessageBacking,
  isWellKnownConstructable,
  isWellKnownWrappable,
} from '../proto/backing.js';
import type { DescriptorSet } from '../proto/descriptors.js';
import { CelErrorCode, CelKind } from '../types.js';
import type { CelValue, FieldEntry, TypeEntry } from '../types.js';

// ───────────────────────────────────────────────────────────────────
// ProtoContext — everything the trampolines need, supplied by the
// assembly WI (WI-1.5) after the Program instantiates.  The trampolines
// are pure functions of this context: no module-global state, so a fresh
// context per Eval is the reset boundary.
// ───────────────────────────────────────────────────────────────────

/** Reads / writes 24-byte CelValues over the Program's linear memory. */
export interface ProtoCodec {
  /**
   * Decode the CelValue at the i32 `slot` offset to its JS-natural shape.
   * Externref kinds (MESSAGE / LIST_HOST / MAP_HOST) are NOT decoded by
   * the trampolines through this hook — they read the slot's `kind` and
   * `msg_slot` directly via {@link readKind} / {@link readMessageSlot}.
   */
  readonly readValue: (slot: number) => CelValue;

  /**
   * Encode `value` into the CelValue at the i32 `slot` offset — the full
   * encoder the assembly WI owns: scalars inline, string / bytes
   * arena-copied, nested message / list / map interned into the externref
   * table as a `CEL_*_HOST` handle.  Mirrors the C++ `EncodeValueToSlot`
   * (`cel_host.h:803`).
   */
  readonly writeValue: (slot: number, value: CelValue) => void;

  /** Read the raw `kind` u32 of the CelValue at `slot` (`cel_data.h:143`). */
  readonly readKind: (slot: number) => CelKind;

  /** Read the externref `msg_slot` payload of a CEL_MESSAGE at `slot`. */
  readonly readMessageSlot: (slot: number) => number;

  /** Copy the CelValue at `src` to `dst` verbatim (for UNKNOWN/ERROR absorb). */
  readonly copyValue: (dst: number, src: number) => void;

  /** Write a CEL_BOOL CelValue at `slot`. */
  readonly writeBool: (slot: number, value: boolean) => void;

  /** Write a CEL_ERROR CelValue with the given wire code at `slot`. */
  readonly writeError: (slot: number, code: CelErrorCode) => void;

  /**
   * Write a CEL_TYPE CelValue at `slot` carrying `fqn` — arena-copies the
   * FQN bytes and stamps the `{ptr, len}` span (`cel_host.h:712`).
   */
  readonly writeType: (slot: number, fqn: string) => void;

  /**
   * Write a CEL_MESSAGE CelValue at `slot` whose payload `msg_slot` is the
   * externref `messageSlot` (`cel_host.h:697`).
   */
  readonly writeMessageSlot: (slot: number, messageSlot: number) => void;
}

/**
 * The per-Eval context the proto / WKT / message trampolines close over.
 * Supplied by the assembly WI (WI-1.5); a fresh one per Eval, so the
 * externref table's between-Eval reset is the only mutable-state boundary.
 */
export interface ProtoContext {
  /** The CelValue read/write codec over the Program's linear memory. */
  readonly codec: ProtoCodec;

  /** The three host-handle namespaces (message / map / list). */
  readonly refs: ExternrefTable;

  /** The decoded `cel.abi` field intern table (`field_ref_id` → field). */
  readonly fields: readonly FieldEntry[];

  /** The decoded `cel.abi` message-type intern table (`type_id` → type). */
  readonly types: readonly TypeEntry[];

  /** The descriptor resolver message literals are constructed against. */
  readonly descriptors: DescriptorSet;

  /**
   * Reserve `n` bytes in the Program's linear-memory arena and return the
   * wasm-side offset.  Used to arena-copy string / bytes / FQN payloads a
   * field read or `type()` produces (`cel_host.h:428` ArenaAllocator).
   */
  readonly arenaAlloc: (n: number) => number;
}

// ───────────────────────────────────────────────────────────────────
// Internal helpers shared by the trampolines.
// ───────────────────────────────────────────────────────────────────

/**
 * True iff the CelValue at `slot` is UNKNOWN or ERROR — the 3VL-absorb
 * inputs.  A trampoline copies such a value to its out slot and returns
 * without dereferencing any backing (the C++ contract, `cel_host.h:518`).
 */
function isPoison(kind: CelKind): boolean {
  return kind === CelKind.UNKNOWN || kind === CelKind.ERROR;
}

/**
 * Resolve the message backing the CEL_MESSAGE at `msgSlot` points at, or
 * `undefined` if `msgSlot` is not a CEL_MESSAGE or its externref slot is
 * empty / wild.  Non-throwing: a bad slot from a buggy guest yields
 * `undefined`, never a host crash (mirrors `ExternrefTable::Lookup`).
 */
function resolveBacking(
  ctx: ProtoContext,
  msgSlot: number,
): ProtoMessageBacking | undefined {
  if (ctx.codec.readKind(msgSlot) !== CelKind.MESSAGE) {
    return undefined;
  }
  const ref = ctx.codec.readMessageSlot(msgSlot);
  const backing = ctx.refs.message.lookup(ref);
  return backing instanceof ProtoMessageBacking ? backing : undefined;
}

/** Look a `field_ref_id` up in the ABI field intern table. */
function fieldRef(
  ctx: ProtoContext,
  fieldRefId: number,
): FieldEntry | undefined {
  return ctx.fields[fieldRefId];
}

/**
 * The key a field is resolved by on a protobufjs `Type`: the wire number
 * when known (non-zero), else the name (`field_number == 0` means
 * "resolve by name only", `cel_host.h:54`).
 */
function fieldKey(entry: FieldEntry): number | string {
  return entry.fieldNumber !== 0 ? entry.fieldNumber : entry.name;
}

/**
 * The protobufjs `Field` a `FieldEntry` names on `backing`, resolved, or
 * `undefined` if the message type has no such field.
 */
function resolveProtoField(
  backing: ProtoMessageBacking,
  entry: FieldEntry,
): protobuf.Field | undefined {
  const type = backing.raw.$type;
  const field =
    entry.fieldNumber !== 0
      ? type.fieldsById[entry.fieldNumber]
      : type.fields[entry.name];
  if (field === undefined) {
    return undefined;
  }
  field.resolve();
  return field;
}

/** The raw protobufjs field value off a message instance, by field name. */
function rawField(
  backing: ProtoMessageBacking,
  field: protobuf.Field,
): unknown {
  return (backing.raw as unknown as Record<string, unknown>)[field.name];
}

// ───────────────────────────────────────────────────────────────────
// cel_get_field — read a field from a message backing.
// ───────────────────────────────────────────────────────────────────

/**
 * Read the singular message-typed field `field` off `backing` and intern a
 * fresh `ProtoMessageBacking` over the nested message so `out` carries a
 * CEL_MESSAGE slot — `req.user.id` chains through a second `cel_get_field`.
 * An UNSET field reads as the DEFAULT-INSTANCE message, not null — langdef
 * §"Field Selection" / cel-cpp `ReadSingularMessageField` (reflection's
 * `GetMessage` serves the default instance; corpus
 * `proto3/empty_field/nested_message`) — except `google.protobuf.Any` and
 * the JSON WKTs (`Value` / `Struct` / `ListValue`), whose unset read stays
 * CEL_NULL.  Returns `false` when `field` is not a non-WKT singular message
 * (the caller then falls back to the decoded-value path).
 */
function tryWriteNestedMessage(
  ctx: ProtoContext,
  out: number,
  backing: ProtoMessageBacking,
  field: protobuf.Field,
): boolean {
  if (field.repeated || field.map) {
    return false;
  }
  const nestedType = field.resolvedType;
  if (!(nestedType instanceof protobuf.Type)) {
    return false;
  }
  // WKT message fields (Timestamp / Duration / wrapper) peel to scalars /
  // tagged records — let the decoded-value path (`backing.readField`)
  // handle those rather than interning a CEL_MESSAGE slot.
  if (isWellKnownWrappable(nestedType.fullName)) {
    return false;
  }
  const value = rawField(backing, field);
  if (value === null || value === undefined) {
    if (isNullOnUnset(nestedType.fullName)) {
      ctx.codec.writeValue(out, null);
      return true;
    }
    const slot = ctx.refs.message.intern(
      new ProtoMessageBacking(nestedType, nestedType.create()),
    );
    ctx.codec.writeMessageSlot(out, slot);
    return true;
  }
  const nested =
    (value as { $type?: unknown }).$type === undefined
      ? nestedType.fromObject(value as Record<string, unknown>)
      : (value as protobuf.Message);
  const slot = ctx.refs.message.intern(
    new ProtoMessageBacking(nestedType, nested),
  );
  ctx.codec.writeMessageSlot(out, slot);
  return true;
}

// Message types whose UNSET singular field reads as CEL_NULL rather than the
// default instance: `Any` (no descriptor to unpack against; cel-cpp returns
// null for the unset path) and the JSON WKTs (`Value` / `Struct` /
// `ListValue` — an unset `Value` is JSON null).
function isNullOnUnset(fqn: string): boolean {
  const name = fqn.startsWith('.') ? fqn.slice(1) : fqn;
  return name === 'google.protobuf.Any' || isWellKnownConstructable(name);
}

/**
 * `cel_host.cel_get_field(out, msg, field_ref, attr)` (`cel_host.h:520`).
 * Reads the field `field_ref` names from the message at `msg`: proto
 * presence + WKT peel, nested message → fresh interned CEL_MESSAGE slot,
 * scalar / string / bytes / list / map → CelValue via the codec
 * (string / bytes arena-copied).  Absorbs UNKNOWN / ERROR on `msg`.  An
 * unmapped message backing or unknown field → CEL_ERROR(FIELD_NOT_FOUND).
 * `attr` (the unknown-pattern attribute id) is ignored — partial eval is
 * out of scope (§A.3).
 */
export function celGetField(
  ctx: ProtoContext,
  out: number,
  msg: number,
  fieldRefId: number,
  _attr: number,
): void {
  if (isPoison(ctx.codec.readKind(msg))) {
    ctx.codec.copyValue(out, msg);
    return;
  }
  const backing = resolveBacking(ctx, msg);
  const entry = fieldRef(ctx, fieldRefId);
  if (backing === undefined || entry === undefined) {
    ctx.codec.writeError(out, CelErrorCode.FIELD_NOT_FOUND);
    return;
  }
  const field = resolveProtoField(backing, entry);
  if (field === undefined) {
    ctx.codec.writeError(out, CelErrorCode.FIELD_NOT_FOUND);
    return;
  }
  if (tryWriteNestedMessage(ctx, out, backing, field)) {
    return;
  }
  // Scalars, enums, repeated, maps, and WKT peels decode through the
  // backing; the codec encodes the JS-natural result back to the wire.
  ctx.codec.writeValue(out, backing.readField(fieldKey(entry)));
}

// ───────────────────────────────────────────────────────────────────
// cel_has_field — proto2/proto3 presence.
// ───────────────────────────────────────────────────────────────────

/**
 * `cel_host.cel_has_field(out, msg, field_ref, attr)` (`cel_host.h:526`).
 * Writes a CEL_BOOL: the proto presence of the named field on the
 * message at `msg`.  Absorbs UNKNOWN / ERROR on `msg`; an unmapped
 * backing or unknown field → CEL_ERROR(FIELD_NOT_FOUND).
 */
export function celHasField(
  ctx: ProtoContext,
  out: number,
  msg: number,
  fieldRefId: number,
  _attr: number,
): void {
  if (isPoison(ctx.codec.readKind(msg))) {
    ctx.codec.copyValue(out, msg);
    return;
  }
  const backing = resolveBacking(ctx, msg);
  const entry = fieldRef(ctx, fieldRefId);
  if (backing === undefined || entry === undefined) {
    ctx.codec.writeError(out, CelErrorCode.FIELD_NOT_FOUND);
    return;
  }
  if (resolveProtoField(backing, entry) === undefined) {
    ctx.codec.writeError(out, CelErrorCode.FIELD_NOT_FOUND);
    return;
  }
  ctx.codec.writeBool(out, backing.hasField(fieldKey(entry)));
}

// ───────────────────────────────────────────────────────────────────
// cel_make_message / cel_set_field — proto-literal construction.
// ───────────────────────────────────────────────────────────────────

/**
 * `cel_host.cel_make_message(type_id, out)` (`cel_host.h:702`).  Resolves
 * `type_id` against the ABI type table → a descriptor, constructs a
 * default-valued protobufjs message, wraps it in a `ProtoMessageBacking`,
 * interns it in the message externref table, and writes a CEL_MESSAGE
 * slot at `out`.  An unknown / unresolvable type_id →
 * CEL_ERROR(UNKNOWN_TYPE).
 */
export function celMakeMessage(
  ctx: ProtoContext,
  typeId: number,
  out: number,
): void {
  const entry: TypeEntry | undefined = ctx.types[typeId];
  if (entry === undefined) {
    ctx.codec.writeError(out, CelErrorCode.UNKNOWN_TYPE);
    return;
  }
  let type: protobuf.Type;
  try {
    type = ctx.descriptors.messageType(entry.fullyQualifiedName);
  } catch {
    ctx.codec.writeError(out, CelErrorCode.UNKNOWN_TYPE);
    return;
  }
  const slot = ctx.refs.message.intern(
    new ProtoMessageBacking(type, type.create()),
  );
  ctx.codec.writeMessageSlot(out, slot);
}

/**
 * `cel_host.cel_set_field(msg, field_ref, value)` (`cel_host.h:743`).
 * Sets the named field on the message backing at `msg` from the CelValue
 * at `value`.  No out slot — the message is mutated in place; the literal
 * is read back through `cel_get_field`.  A poisoned `value`, unmapped
 * backing, or unknown field is a silent no-op (the read-back surfaces the
 * gap as a default / error there); only an infrastructure failure would
 * throw, and there is none on this path.
 */
export function celSetField(
  ctx: ProtoContext,
  msg: number,
  fieldRefId: number,
  value: number,
): void {
  if (isPoison(ctx.codec.readKind(value))) {
    return;
  }
  const backing = resolveBacking(ctx, msg);
  const entry = fieldRef(ctx, fieldRefId);
  if (backing === undefined || entry === undefined) {
    return;
  }
  const field = resolveProtoField(backing, entry);
  if (field === undefined) {
    return;
  }
  // A message-typed value carries an externref slot the codec cannot
  // decode — resolve its backing and set the raw protobufjs message
  // directly so a constructed sub-message nests without re-coercion.
  if (
    field.resolvedType instanceof protobuf.Type &&
    !field.repeated &&
    !field.map &&
    ctx.codec.readKind(value) === CelKind.MESSAGE
  ) {
    const sub = resolveBacking(ctx, value);
    if (sub !== undefined) {
      (backing.raw as unknown as Record<string, unknown>)[field.name] = sub.raw;
    }
    return;
  }
  try {
    backing.setField(fieldKey(entry), ctx.codec.readValue(value));
  } catch (err) {
    if (err instanceof ProtoFieldRangeError) {
      // An out-of-range narrowing (int32 / uint32 / enum) is a CEL eval
      // error, not a host trap: poison the MESSAGE slot itself with
      // CEL_ERROR(OVERFLOW) so the construction's result slot carries the
      // error out — mirrors `CelSetFieldImpl`'s kOutOfRange arm
      // (`eval/internal/cel_host.cc`).  Subsequent `cel_set_field` calls on
      // the poisoned slot no-op (resolveBacking sees a non-MESSAGE kind).
      ctx.codec.writeError(msg, CelErrorCode.OVERFLOW);
      return;
    }
    throw err;
  }
}

// ───────────────────────────────────────────────────────────────────
// cel_wkt_unwrap_time / cel_wkt_unwrap_wrapper — WKT peel.
// ───────────────────────────────────────────────────────────────────

/**
 * `cel_host.cel_wkt_unwrap_time(out, msg)` (`cel_host.h:774`).  Peels a
 * `google.protobuf.Timestamp` / `Duration` message at `msg` to a tagged
 * CEL_TIMESTAMP / CEL_DURATION CelValue.  A non-WKT-time or non-message
 * operand → CEL_ERROR(TYPE_MISMATCH).  Absorbs UNKNOWN / ERROR on `msg`.
 */
export function celWktUnwrapTime(
  ctx: ProtoContext,
  out: number,
  msg: number,
): void {
  if (isPoison(ctx.codec.readKind(msg))) {
    ctx.codec.copyValue(out, msg);
    return;
  }
  const backing = resolveBacking(ctx, msg);
  if (backing === undefined) {
    ctx.codec.writeError(out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const name = backing.typeName;
  if (name === 'google.protobuf.Timestamp') {
    ctx.codec.writeValue(out, peelTimestampOf(backing));
    return;
  }
  if (name === 'google.protobuf.Duration') {
    ctx.codec.writeValue(out, peelDurationOf(backing));
    return;
  }
  ctx.codec.writeError(out, CelErrorCode.TYPE_MISMATCH);
}

/** The nine `google.protobuf.*Value` wrapper FQNs (`cel_host.h:788`). */
const WRAPPER_NAMES: ReadonlySet<string> = new Set<string>([
  'google.protobuf.BoolValue',
  'google.protobuf.Int32Value',
  'google.protobuf.Int64Value',
  'google.protobuf.UInt32Value',
  'google.protobuf.UInt64Value',
  'google.protobuf.FloatValue',
  'google.protobuf.DoubleValue',
  'google.protobuf.StringValue',
  'google.protobuf.BytesValue',
]);

/**
 * The inner CelKinds a wrapper's `value` field can decode to (1..6 per
 * `cel_data.h::CelKind`, `cel_host.h:788`) — the valid `wrapper_kind`
 * arguments.  The backing's own descriptor decides the concrete decode;
 * this only validates the discriminant is in range.
 */
const WRAPPER_INNER_KINDS: ReadonlySet<CelKind> = new Set<CelKind>([
  CelKind.BOOL,
  CelKind.INT,
  CelKind.UINT,
  CelKind.DOUBLE,
  CelKind.STRING,
  CelKind.BYTES,
]);

/**
 * `cel_host.cel_wkt_unwrap_wrapper(out, msg, wrapper_kind)`
 * (`cel_host.h:788`).  Peels one of the nine `google.protobuf.*Value`
 * wrapper messages at `msg` to its inner scalar CelValue.  `wrapper_kind`
 * is the expected inner CelKind (1..6); a non-wrapper / non-message
 * operand → CEL_ERROR(TYPE_MISMATCH).  Absorbs UNKNOWN / ERROR on `msg`.
 *
 * The 32-bit wrappers (`Int32Value` / `UInt32Value` / `FloatValue`)
 * carry the same inner CelKind as their 64-bit / double siblings, so
 * `wrapper_kind` selects the kind family; the backing's own descriptor
 * decides the concrete decode (`peelWrapper` reads the actual field).
 */
export function celWktUnwrapWrapper(
  ctx: ProtoContext,
  out: number,
  msg: number,
  wrapperKind: number,
): void {
  if (isPoison(ctx.codec.readKind(msg))) {
    ctx.codec.copyValue(out, msg);
    return;
  }
  const backing = resolveBacking(ctx, msg);
  if (
    backing === undefined ||
    !WRAPPER_INNER_KINDS.has(wrapperKind as CelKind) ||
    !WRAPPER_NAMES.has(backing.typeName)
  ) {
    ctx.codec.writeError(out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  // The wrapper's `value` field (number 1), decoded as a normal field.
  ctx.codec.writeValue(out, backing.readField(1));
}

// ───────────────────────────────────────────────────────────────────
// cel_message_eq / cel_message_is_zero — equality / emptiness.
// ───────────────────────────────────────────────────────────────────

/**
 * `cel_host.cel_message_eq(out, a, b)` (`cel_host.h:665`).  Writes a
 * CEL_BOOL: structural equality of the two message backings at `a` / `b`
 * (langdef §"Equality").  Either operand UNKNOWN / ERROR propagates 3VL;
 * an unmapped backing on either side → CEL_ERROR(TYPE_MISMATCH).
 */
export function celMessageEq(
  ctx: ProtoContext,
  out: number,
  a: number,
  b: number,
): void {
  if (isPoison(ctx.codec.readKind(a))) {
    ctx.codec.copyValue(out, a);
    return;
  }
  if (isPoison(ctx.codec.readKind(b))) {
    ctx.codec.copyValue(out, b);
    return;
  }
  const ba = resolveBacking(ctx, a);
  const bb = resolveBacking(ctx, b);
  if (ba === undefined || bb === undefined) {
    ctx.codec.writeError(out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  ctx.codec.writeBool(out, messagesEqual(ba, bb));
}

/**
 * `cel_host.cel_message_is_zero(out, msg)` (`cel_host.h:683`).  Writes a
 * CEL_BOOL: true iff the message at `msg` has no set fields (cel-cpp
 * `ParsedMessageValue::IsZeroValue()` parity).  Absorbs UNKNOWN / ERROR;
 * an unmapped backing → CEL_ERROR(HOST_ADAPTER_ERROR).
 */
export function celMessageIsZero(
  ctx: ProtoContext,
  out: number,
  msg: number,
): void {
  if (isPoison(ctx.codec.readKind(msg))) {
    ctx.codec.copyValue(out, msg);
    return;
  }
  const backing = resolveBacking(ctx, msg);
  if (backing === undefined) {
    ctx.codec.writeError(out, CelErrorCode.HOST_ADAPTER_ERROR);
    return;
  }
  ctx.codec.writeBool(out, messageIsZero(backing));
}

// ───────────────────────────────────────────────────────────────────
// resolve_message_type_name — type(<message>) → CEL_TYPE.
// ───────────────────────────────────────────────────────────────────

/**
 * `cel_host.resolve_message_type_name(out, in)` (`cel_host.h:712`).
 * Writes a CEL_TYPE CelValue carrying the message's fully-qualified
 * descriptor name (no leading dot).  Absorbs UNKNOWN / ERROR; an unmapped
 * backing → CEL_ERROR(UNKNOWN_TYPE).
 */
export function resolveMessageTypeName(
  ctx: ProtoContext,
  out: number,
  inSlot: number,
): void {
  if (isPoison(ctx.codec.readKind(inSlot))) {
    ctx.codec.copyValue(out, inSlot);
    return;
  }
  const backing = resolveBacking(ctx, inSlot);
  if (backing === undefined) {
    ctx.codec.writeError(out, CelErrorCode.UNKNOWN_TYPE);
    return;
  }
  ctx.codec.writeType(out, backing.typeName);
}

// ───────────────────────────────────────────────────────────────────
// Backing-level helpers (pure protobufjs, no wasm).
// ───────────────────────────────────────────────────────────────────

/** Peel a Timestamp backing's `(seconds, nanos)` to a tagged CelValue. */
function peelTimestampOf(backing: ProtoMessageBacking): CelValue {
  const rec = backing.raw as unknown as Record<string, unknown>;
  return {
    kind: 'timestamp',
    epochSeconds: toBigInt(rec.seconds),
    nanos: toNumber(rec.nanos),
  };
}

/** Peel a Duration backing's `(seconds, nanos)` to a tagged CelValue. */
function peelDurationOf(backing: ProtoMessageBacking): CelValue {
  const rec = backing.raw as unknown as Record<string, unknown>;
  return {
    kind: 'duration',
    seconds: toBigInt(rec.seconds),
    nanos: toNumber(rec.nanos),
  };
}

/**
 * Structural message equality.  Compares the two backings' canonical
 * protobufjs encodings (`Type.encode` over each), which is the cheapest
 * field-by-field structural compare protobufjs offers and matches the
 * langdef message-equality contract for the static subset.  Differing
 * types are unequal.  A NaN float/double field anywhere in the tree
 * forces inequality: NaN serialises to stable bytes, but field-wise
 * equality treats NaN as unequal to NaN — `MessageDifferencer::Equals`
 * parity (`CompareProtoMessages`, eval/internal/cel_host.cc) and
 * langdef §"Equality" (NaN compares unequal to itself).
 */
function messagesEqual(
  a: ProtoMessageBacking,
  b: ProtoMessageBacking,
): boolean {
  if (a.typeName !== b.typeName) {
    return false;
  }
  const type = a.raw.$type;
  if (!bytesEqual(type.encode(a.raw).finish(), type.encode(b.raw).finish())) {
    return false;
  }
  // Byte-equal: the messages are field-wise identical, so a NaN in `a`
  // pairs with the same NaN in `b` — unequal under exact float compare.
  return !messageContainsNaN(type, a.raw);
}

// Scalar field types whose values can carry NaN.
const FLOATING_FIELD_TYPES: ReadonlySet<string> = new Set(['double', 'float']);

// Walks every set field of a message (declared by `type`) — singular,
// repeated, and map values, recursing through nested messages — and
// reports whether any float/double value is NaN.  Walks the declared
// field types rather than `$type` so plain-object nested values (the
// `fromObject` / direct-assignment shapes) are covered too.
function messageContainsNaN(type: protobuf.Type, msg: unknown): boolean {
  if (typeof msg !== 'object' || msg === null) {
    return false;
  }
  const rec = msg as Record<string, unknown>;
  for (const f of type.fieldsArray) {
    const value = rec[f.name];
    if (value === undefined || value === null) {
      continue;
    }
    const values: unknown[] = f.map
      ? Object.values(value as Record<string, unknown>)
      : f.repeated
        ? (value as unknown[])
        : [value];
    if (fieldValuesContainNaN(f, values)) {
      return true;
    }
  }
  return false;
}

function fieldValuesContainNaN(f: protobuf.Field, values: unknown[]): boolean {
  if (FLOATING_FIELD_TYPES.has(f.type)) {
    return values.some((v) => typeof v === 'number' && Number.isNaN(v));
  }
  if (f.resolvedType instanceof protobuf.Type) {
    const nested = f.resolvedType;
    return values.some((v) => messageContainsNaN(nested, v));
  }
  return false;
}

/**
 * True iff the backing is a proto zero value (no set fields).  A proto3
 * message with every field at its default serialises to zero bytes, so
 * the canonical encoding's length is the zero-value probe — robust to
 * protobufjs materialising defaults as own properties on `fromObject`.
 */
function messageIsZero(backing: ProtoMessageBacking): boolean {
  return backing.raw.$type.encode(backing.raw).finish().length === 0;
}

function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) {
    return false;
  }
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) {
      return false;
    }
  }
  return true;
}

function toBigInt(value: unknown): bigint {
  if (typeof value === 'bigint') {
    return value;
  }
  if (typeof value === 'number') {
    return BigInt(Math.trunc(value));
  }
  if (typeof value === 'string') {
    return BigInt(value);
  }
  if (isLongLike(value)) {
    return BigInt(value.toString());
  }
  return 0n;
}

function toNumber(value: unknown): number {
  if (typeof value === 'number') {
    return value;
  }
  if (typeof value === 'bigint') {
    return Number(value);
  }
  if (typeof value === 'string') {
    return Number(value);
  }
  if (isLongLike(value)) {
    return Number(value.toString());
  }
  return 0;
}

interface LongLike {
  toString(): string;
}

function isLongLike(value: unknown): value is LongLike {
  return (
    typeof value === 'object' &&
    value !== null &&
    'low' in value &&
    'high' in value &&
    typeof (value as { toString: unknown }).toString === 'function'
  );
}

// ───────────────────────────────────────────────────────────────────
// makeProtoTrampolines — the `cel_host` proto / WKT / message group,
// ready to deep-merge with the aggregate trampolines + stub imports the
// assembly WI (WI-1.5) supplies before `WebAssembly.instantiate`.
// ───────────────────────────────────────────────────────────────────

/**
 * Builds the `cel_host.*` proto / WKT / message import functions over
 * `ctx`.  Each is the `(...args: number[]) => void` wasm import shape; the
 * keys are the empirically-confirmed `cel_host` import names
 * (`cel_host.h:520-790`).
 */
/** The proto/WKT/message `cel_host` import names this group provides. */
export type ProtoTrampolineName =
  | 'cel_get_field'
  | 'cel_has_field'
  | 'cel_make_message'
  | 'cel_set_field'
  | 'cel_wkt_unwrap_time'
  | 'cel_wkt_unwrap_wrapper'
  | 'cel_message_eq'
  | 'cel_message_is_zero'
  | 'resolve_message_type_name';

export function makeProtoTrampolines(
  ctx: ProtoContext,
): Record<ProtoTrampolineName, (...args: number[]) => void> {
  return {
    cel_get_field: (out, msg, fieldRefId, attr): void => {
      celGetField(ctx, out, msg, fieldRefId, attr);
    },
    cel_has_field: (out, msg, fieldRefId, attr): void => {
      celHasField(ctx, out, msg, fieldRefId, attr);
    },
    cel_make_message: (typeId, out): void => {
      celMakeMessage(ctx, typeId, out);
    },
    cel_set_field: (msg, fieldRefId, value): void => {
      celSetField(ctx, msg, fieldRefId, value);
    },
    cel_wkt_unwrap_time: (out, msg): void => {
      celWktUnwrapTime(ctx, out, msg);
    },
    cel_wkt_unwrap_wrapper: (out, msg, wrapperKind): void => {
      celWktUnwrapWrapper(ctx, out, msg, wrapperKind);
    },
    cel_message_eq: (out, a, b): void => {
      celMessageEq(ctx, out, a, b);
    },
    cel_message_is_zero: (out, msg): void => {
      celMessageIsZero(ctx, out, msg);
    },
    resolve_message_type_name: (out, inSlot): void => {
      resolveMessageTypeName(ctx, out, inSlot);
    },
  };
}
