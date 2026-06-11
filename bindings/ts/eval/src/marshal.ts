// Marshal an activation's bound values into the Program's workspace
// slots before `$eval` runs.
//
// For every variable the `cel.abi` declares, the marshal writes its
// 24-byte CelValue at the variable's `slot_offset` (`VariableEntry`),
// dispatching on the variable's `repr` (compiler/ir/annotations.h: kNull=1
// … kOptional=15).  Scalars stamp inline; string / bytes payloads are
// copied into an ACTIVATION BUFFER — a region malloc'd from the dlmalloc
// heap, ABOVE the runtime's bump arena — because `$eval`'s prelude calls
// `arena_reset`, which would rewind and zero-fill any payload the marshal
// placed in the arena.  A JS array / Map / object bound to a list / map /
// message variable interns a host backing into the externref table and
// stamps the handle slot.
//
// This mirrors the C++ marshal in `eval/instance.cc`
// (`MarshalActivation` / `MarshalOneVariable` / `EncodeBoundValue`):
// same slot-offset write, same "string payloads outside the arena"
// invariant (the activation buffer at `eval/internal/instance_impl.h:82`).
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.6.

import type * as protobuf from 'protobufjs';

import { encodeUtf8 } from './celvalue.js';
import type { ExternrefTable } from './externref.js';
import type { HostListBacking, HostMapBacking } from './host/aggregates.js';
import { coerceObjectToMessage, ProtoMessageBacking } from './proto/backing.js';
import type { DescriptorSet } from './proto/descriptors.js';
import type { CodecEnv } from './resolving-codec.js';
import { encodeCelValue } from './resolving-codec.js';
import {
  CEL_VALUE_KIND_OFFSET,
  CEL_VALUE_PAYLOAD_OFFSET,
  CelKind,
} from './types.js';
import type { CelInput, CelValue, TypeEntry, VariableEntry } from './types.js';

/**
 * The `repr` discriminant of a declared variable (`compiler/ir/
 * annotations.h:17`).  Numbered independently of {@link CelKind}; the
 * marshal dispatches the bound value's encoding on it.
 */
export const enum Repr {
  UNKNOWN = 0,
  NULL = 1,
  BOOL = 2,
  INT = 3,
  UINT = 4,
  DOUBLE = 5,
  STRING = 6,
  BYTES = 7,
  LIST = 8,
  MAP = 9,
  MESSAGE = 10,
  ENUM = 11,
  DURATION = 12,
  TIMESTAMP = 13,
  TYPE = 14,
  OPTIONAL = 15,
}

/**
 * Thrown when an activation cannot be marshalled — a declared variable is
 * unbound, or a bound value's JS type does not match its declared `repr`.
 * This is API misuse (the caller passed a bad activation), so it throws
 * rather than returning a CEL_ERROR value.
 */
export class CelMarshalError extends Error {
  override readonly name = 'CelMarshalError';
}

/**
 * A linear-memory allocator for activation payloads that survive
 * `arena_reset`.  Backed by a `malloc`'d buffer above `__heap_base`
 * (mirrors the C++ activation buffer, `instance_impl.h:82`).  `alloc`
 * returns 0 on exhaustion; the caller is responsible for sizing the
 * buffer to the activation's total payload need first.
 */
export interface ActivationArena {
  /** Reserve `n` bytes; returns the linear-memory offset, or 0 on OOM. */
  alloc(n: number): number;
}

/** Everything the marshal needs, assembled once per Eval by the Instance. */
export interface MarshalEnv {
  /** A `DataView` over the Program's CURRENT linear memory. */
  view(): DataView;
  /** A `Uint8Array` over the SAME current linear memory `view()` covers. */
  bytes(): Uint8Array;
  /** The per-Eval externref table host aggregates / messages intern into. */
  readonly refs: ExternrefTable;
  /** The codec env used to encode nested aggregate elements. */
  readonly codec: CodecEnv;
  /** Allocator for string / bytes payloads (above the arena). */
  readonly activationArena: ActivationArena;
  /** Descriptors message-typed variables coerce / resolve against. */
  readonly descriptors: DescriptorSet | undefined;
  /** The ABI message-type intern table (for a message variable's type). */
  readonly types: readonly TypeEntry[];
}

/**
 * Marshal every declared variable into its workspace slot.  Throws
 * {@link CelMarshalError} if a declared variable is unbound or a bound
 * value's type does not match its declared `repr`.
 *
 * Order matches the C++ `MarshalActivation`: this runs BEFORE `$eval`,
 * whose prelude reads each slot via `local.get local_index`.
 */
export function marshalActivation(
  env: MarshalEnv,
  variables: readonly VariableEntry[],
  activation: Record<string, CelInput>,
): void {
  for (const variable of variables) {
    if (
      !Object.prototype.hasOwnProperty.call(activation, variable.name) ||
      activation[variable.name] === undefined
    ) {
      throw new CelMarshalError(
        `Activation: variable '${variable.name}' declared on the Program ` +
          `but not bound in the activation`,
      );
    }
    marshalOne(env, variable, activation[variable.name] as CelInput);
  }
}

/**
 * Sum the byte sizes of every string / bytes payload the activation will
 * place in the activation arena, so the Instance can size the buffer
 * before any payload is written.  Mirrors `TotalHostStringBytes`
 * (`eval/instance.cc`).  Each payload is padded up to 8 bytes to keep the
 * arena cursor 8-aligned.
 */
export function totalActivationBufferBytes(
  variables: readonly VariableEntry[],
  activation: Record<string, CelInput>,
): number {
  let total = 0;
  for (const variable of variables) {
    if (!Object.prototype.hasOwnProperty.call(activation, variable.name)) {
      continue;
    }
    const bound = activation[variable.name];
    const repr = variable.repr as Repr;
    let bytes = 0;
    if (repr === Repr.STRING && typeof bound === 'string') {
      bytes = encodeUtf8(bound).length;
    } else if (repr === Repr.BYTES && bound instanceof Uint8Array) {
      bytes = bound.length;
    }
    total += (bytes + 7) & ~7;
  }
  return total;
}

function marshalOne(
  env: MarshalEnv,
  variable: VariableEntry,
  bound: CelInput,
): void {
  const repr = variable.repr as Repr;
  const slot = variable.slotOffset;
  switch (repr) {
    case Repr.NULL: {
      writeNull(env, slot, variable);
      return;
    }
    case Repr.BOOL: {
      writeBool(env, slot, variable, bound);
      return;
    }
    case Repr.INT: {
      writeInt(env, slot, variable, bound);
      return;
    }
    case Repr.UINT: {
      writeUint(env, slot, variable, bound);
      return;
    }
    case Repr.DOUBLE: {
      writeDouble(env, slot, variable, bound);
      return;
    }
    case Repr.STRING: {
      writeString(env, slot, variable, bound);
      return;
    }
    case Repr.BYTES: {
      writeBytes(env, slot, variable, bound);
      return;
    }
    case Repr.LIST: {
      writeList(env, slot, variable, bound);
      return;
    }
    case Repr.MAP: {
      writeMap(env, slot, variable, bound);
      return;
    }
    case Repr.MESSAGE: {
      writeMessage(env, slot, variable, bound);
      return;
    }
    case Repr.DURATION:
    case Repr.TIMESTAMP: {
      writeTimeRecord(env, slot, variable, bound, repr);
      return;
    }
    case Repr.UNKNOWN:
    case Repr.ENUM:
    case Repr.TYPE:
    case Repr.OPTIONAL:
      throw new CelMarshalError(
        `Activation['${variable.name}']: repr ${String(repr)} is not a ` +
          `marshalable activation type in this binding`,
      );
  }
}

// ── Scalar writers ──────────────────────────────────────────────────

function writeNull(env: MarshalEnv, slot: number, v: VariableEntry): void {
  void v;
  stampKind(env, slot, CelKind.NULL);
}

function writeBool(
  env: MarshalEnv,
  slot: number,
  v: VariableEntry,
  bound: CelInput,
): void {
  if (typeof bound !== 'boolean') {
    throw typeMismatch(v, 'bool', bound);
  }
  stampKind(env, slot, CelKind.BOOL);
  env.view().setInt32(slot + CEL_VALUE_PAYLOAD_OFFSET, bound ? 1 : 0, true);
}

function writeInt(
  env: MarshalEnv,
  slot: number,
  v: VariableEntry,
  bound: CelInput,
): void {
  stampKind(env, slot, CelKind.INT);
  env
    .view()
    .setBigInt64(slot + CEL_VALUE_PAYLOAD_OFFSET, asBigInt(v, bound), true);
}

function writeUint(
  env: MarshalEnv,
  slot: number,
  v: VariableEntry,
  bound: CelInput,
): void {
  stampKind(env, slot, CelKind.UINT);
  env
    .view()
    .setBigUint64(slot + CEL_VALUE_PAYLOAD_OFFSET, asBigInt(v, bound), true);
}

function writeDouble(
  env: MarshalEnv,
  slot: number,
  v: VariableEntry,
  bound: CelInput,
): void {
  let n: number;
  if (typeof bound === 'number') {
    n = bound;
  } else if (typeof bound === 'bigint') {
    n = Number(bound);
  } else {
    throw typeMismatch(v, 'double', bound);
  }
  stampKind(env, slot, CelKind.DOUBLE);
  env.view().setFloat64(slot + CEL_VALUE_PAYLOAD_OFFSET, n, true);
}

function writeString(
  env: MarshalEnv,
  slot: number,
  v: VariableEntry,
  bound: CelInput,
): void {
  if (typeof bound !== 'string') {
    throw typeMismatch(v, 'string', bound);
  }
  writeSpanPayload(env, slot, CelKind.STRING, encodeUtf8(bound), v);
}

function writeBytes(
  env: MarshalEnv,
  slot: number,
  v: VariableEntry,
  bound: CelInput,
): void {
  if (!(bound instanceof Uint8Array)) {
    throw typeMismatch(v, 'bytes', bound);
  }
  writeSpanPayload(env, slot, CelKind.BYTES, bound, v);
}

/**
 * Copy a string / bytes payload into the activation arena (NOT the eval
 * arena — `$eval`'s `arena_reset` would wipe it) and stamp the span.
 */
function writeSpanPayload(
  env: MarshalEnv,
  slot: number,
  kind: CelKind.STRING | CelKind.BYTES,
  payload: Uint8Array,
  v: VariableEntry,
): void {
  let ptr = 0;
  if (payload.length > 0) {
    ptr = env.activationArena.alloc(payload.length);
    if (ptr === 0) {
      throw new CelMarshalError(
        `Activation['${v.name}']: activation buffer exhausted writing a ` +
          `${String(payload.length)}-byte payload`,
      );
    }
    env.bytes().set(payload, ptr);
  }
  const view = env.view();
  view.setUint32(slot + CEL_VALUE_KIND_OFFSET, kind, true);
  view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, ptr, true);
  view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET + 4, payload.length, true);
}

// ── Aggregate / message writers ─────────────────────────────────────

function writeList(
  env: MarshalEnv,
  slot: number,
  v: VariableEntry,
  bound: CelInput,
): void {
  if (!Array.isArray(bound)) {
    throw typeMismatch(v, 'list', bound);
  }
  const elements = bound.map((e) => inputToCelValue(e));
  const backing: HostListBacking = { elements };
  const ref = env.refs.list.intern(backing);
  stampHandle(env, slot, CelKind.LIST_HOST, ref);
}

function writeMap(
  env: MarshalEnv,
  slot: number,
  v: VariableEntry,
  bound: CelInput,
): void {
  const backing = mapBackingFrom(bound, v);
  const ref = env.refs.map.intern(backing);
  stampHandle(env, slot, CelKind.MAP_HOST, ref);
}

function mapBackingFrom(bound: CelInput, v: VariableEntry): HostMapBacking {
  if (bound instanceof Map) {
    const entries = [...bound].map(([k, val]) => ({
      key: inputToCelValue(k),
      value: inputToCelValue(val),
    }));
    return { entries };
  }
  if (isPlainObject(bound)) {
    const entries = Object.entries(bound).map(([k, val]) => ({
      key: k,
      value: inputToCelValue(val),
    }));
    return { entries };
  }
  throw typeMismatch(v, 'map', bound);
}

function writeMessage(
  env: MarshalEnv,
  slot: number,
  v: VariableEntry,
  bound: CelInput,
): void {
  const backing = messageBackingFrom(env, v, bound);
  const ref = env.refs.message.intern(backing);
  stampHandle(env, slot, CelKind.MESSAGE, ref);
}

/**
 * Build a {@link ProtoMessageBacking} for a message-typed variable from a
 * protobufjs message (used directly) or a plain JS object (coerced via
 * `fromObject` against the variable's declared descriptor, §A.4.6).
 */
function messageBackingFrom(
  env: MarshalEnv,
  v: VariableEntry,
  bound: CelInput,
): ProtoMessageBacking {
  if (env.descriptors === undefined) {
    throw new CelMarshalError(
      `Activation['${v.name}']: a message-typed variable needs descriptors; ` +
        `pass them to Engine.create({ descriptors })`,
    );
  }
  const typeName = messageTypeNameFor(env, v);
  const type = env.descriptors.messageType(typeName);
  // A protobufjs message carries a `$type`; a bare object is coerced.
  if (isProtobufMessage(bound)) {
    return new ProtoMessageBacking(type, bound as protobuf.Message);
  }
  if (isPlainObject(bound)) {
    return new ProtoMessageBacking(type, coerceObjectToMessage(type, bound));
  }
  throw typeMismatch(v, `message<${typeName}>`, bound);
}

/**
 * The fully-qualified message type a message-typed variable declares.
 * The ABI's `types[]` table interns each message literal's FQN; a single
 * message-typed variable maps to the sole declared type when present.
 */
function messageTypeNameFor(env: MarshalEnv, v: VariableEntry): string {
  const entry: TypeEntry | undefined = env.types[0];
  if (entry === undefined) {
    throw new CelMarshalError(
      `Activation['${v.name}']: no message type declared in the Program's ` +
        `cel.abi type table for a message-typed variable`,
    );
  }
  return entry.fullyQualifiedName;
}

function writeTimeRecord(
  env: MarshalEnv,
  slot: number,
  v: VariableEntry,
  bound: CelInput,
  repr: Repr.DURATION | Repr.TIMESTAMP,
): void {
  if (
    typeof bound !== 'object' ||
    bound === null ||
    !('kind' in bound) ||
    (bound.kind !== 'timestamp' && bound.kind !== 'duration')
  ) {
    throw typeMismatch(
      v,
      repr === Repr.TIMESTAMP ? 'timestamp' : 'duration',
      bound,
    );
  }
  // A CelInput timestamp/duration record reuses the CelValue encoder.
  encodeCelValue(env.codec, slot, bound as CelValue);
}

// ── Shared helpers ──────────────────────────────────────────────────

function stampKind(env: MarshalEnv, slot: number, kind: CelKind): void {
  env.view().setUint32(slot + CEL_VALUE_KIND_OFFSET, kind, true);
}

function stampHandle(
  env: MarshalEnv,
  slot: number,
  kind: CelKind,
  ref: number,
): void {
  const view = env.view();
  view.setUint32(slot + CEL_VALUE_KIND_OFFSET, kind, true);
  view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, ref, true);
}

function asBigInt(v: VariableEntry, bound: CelInput): bigint {
  if (typeof bound === 'bigint') {
    return bound;
  }
  if (typeof bound === 'number' && Number.isInteger(bound)) {
    return BigInt(bound);
  }
  throw typeMismatch(v, 'int/uint', bound);
}

/**
 * Convert a JS-natural {@link CelInput} to a decoded {@link CelValue} for
 * an aggregate element.  A nested array / Map / object becomes the
 * matching CelValue shape; a protobufjs message becomes its decoded
 * object.  The narrow {@link CelInput} → {@link CelValue} mapping is the
 * identity on scalars (bigint/number/boolean/string/Uint8Array/null).
 */
function inputToCelValue(input: CelInput): CelValue {
  if (
    input === null ||
    typeof input === 'boolean' ||
    typeof input === 'bigint' ||
    typeof input === 'number' ||
    typeof input === 'string' ||
    input instanceof Uint8Array
  ) {
    return input;
  }
  if (Array.isArray(input)) {
    return input.map((e) => inputToCelValue(e));
  }
  if (input instanceof Map) {
    const out = new Map<CelValue, CelValue>();
    for (const [k, val] of input) {
      out.set(inputToCelValue(k), inputToCelValue(val));
    }
    return out;
  }
  if (isProtobufMessage(input)) {
    return messageObjectFrom(input as { $type: { toObject: unknown } });
  }
  // A plain object element decodes structurally as a field map.
  const obj: Record<string, CelValue> = {};
  for (const [k, val] of Object.entries(input)) {
    obj[k] = inputToCelValue(val as CelInput);
  }
  return obj;
}

/** Decode a protobufjs message element to its plain-object CelValue. */
function messageObjectFrom(msg: { $type: { toObject: unknown } }): CelValue {
  const out: Record<string, CelValue> = {};
  const record = msg as unknown as Record<string, unknown>;
  for (const [k, val] of Object.entries(record)) {
    if (k === '$type') {
      continue;
    }
    out[k] = inputToCelValue(val as CelInput);
  }
  return out;
}

function isPlainObject(value: CelInput): value is Record<string, CelInput> {
  return (
    typeof value === 'object' &&
    value !== null &&
    !Array.isArray(value) &&
    !(value instanceof Uint8Array) &&
    !(value instanceof Map) &&
    !isProtobufMessage(value) &&
    !('kind' in value)
  );
}

/**
 * True iff `value` is a protobufjs message instance — it carries a
 * `$type` descriptor object.  Returns a plain boolean (not a type
 * predicate) because the protobufjs `Message` type is structurally
 * incompatible with the narrow shape this checks; callers cast at the
 * use site.
 */
function isProtobufMessage(value: CelInput): boolean {
  return (
    typeof value === 'object' &&
    value !== null &&
    '$type' in value &&
    typeof (value as { $type?: unknown }).$type === 'object'
  );
}

function typeMismatch(
  v: VariableEntry,
  declared: string,
  bound: CelInput,
): CelMarshalError {
  return new CelMarshalError(
    `Activation['${v.name}']: declared ${declared} but bound a ` +
      describeInput(bound),
  );
}

function describeInput(value: CelInput): string {
  if (value === null) {
    return 'null';
  }
  if (Array.isArray(value)) {
    return 'array';
  }
  if (value instanceof Uint8Array) {
    return 'Uint8Array';
  }
  if (value instanceof Map) {
    return 'Map';
  }
  return typeof value;
}
