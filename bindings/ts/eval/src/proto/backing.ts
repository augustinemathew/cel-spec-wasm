// A descriptor-backed message backing over a protobufjs `Message` + `Type`.
//
// `ProtoMessageBacking` is the concrete `MessageBacking` (types.ts) the
// `cel_host` proto/WKT trampolines operate over: it reads a field by wire
// number or name, decodes the protobufjs value to the JS-natural
// `CelValue`, applies proto3 default/presence rules, and supports mutable
// construction for proto literals.  Pure protobufjs — no wasm.
//
// Decode rules (CEL semantics, doc/langdef.md):
//   - int32/sint32/sfixed32/int64/sint64/sfixed64/enum → INT  (bigint)
//   - uint32/fixed32/uint64/fixed64                    → UINT (bigint)
//   - double/float                                     → number
//   - bool/string                                      → boolean/string
//   - bytes                                            → Uint8Array
//   - repeated                                         → CelValue[]
//   - map                                              → Map<CelValue,CelValue>
//   - message (set)        → nested message-as-object  ({[k]:CelValue})
//   - message (unset)      → null
//   - WKT Timestamp/Duration/wrapper → peeled scalar / tagged record
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.6.

import * as protobuf from 'protobufjs';

import type {
  CelDuration,
  CelTimestamp,
  CelValue,
  MessageBacking,
} from '../types.js';

// ───────────────────────────────────────────────────────────────────
// Well-known type names + their field numbers (proto descriptors).
// ───────────────────────────────────────────────────────────────────
const WKT_TIMESTAMP = 'google.protobuf.Timestamp';
const WKT_DURATION = 'google.protobuf.Duration';
const WKT_WRAPPERS = new Set<string>([
  'google.protobuf.DoubleValue',
  'google.protobuf.FloatValue',
  'google.protobuf.Int64Value',
  'google.protobuf.UInt64Value',
  'google.protobuf.Int32Value',
  'google.protobuf.UInt32Value',
  'google.protobuf.BoolValue',
  'google.protobuf.StringValue',
  'google.protobuf.BytesValue',
]);

/** A `MessageBacking` over a protobufjs `Message` + its `Type`. */
export class ProtoMessageBacking implements MessageBacking {
  private readonly type: protobuf.Type;
  private readonly message: protobuf.Message;

  constructor(type: protobuf.Type, message: protobuf.Message) {
    this.type = type;
    this.message = message;
  }

  /** The fully-qualified type name, WITHOUT a leading dot (matches the ABI). */
  get typeName(): string {
    return stripDot(this.type.fullName);
  }

  /** The underlying protobufjs message (for trampoline interop). */
  get raw(): protobuf.Message {
    return this.message;
  }

  readField(field: number | string): CelValue {
    const f = resolveField(this.type, field);
    const value = fieldsOf(this.message)[f.name];
    return decodeField(f, value);
  }

  hasField(field: number | string): boolean {
    const f = resolveField(this.type, field);
    return fieldIsPresent(this.message, f);
  }

  setField(field: number | string, value: CelValue): void {
    const f = resolveField(this.type, field);
    fieldsOf(this.message)[f.name] = encodeField(f, value);
  }
}

// ───────────────────────────────────────────────────────────────────
// Standalone coercion helpers.
// ───────────────────────────────────────────────────────────────────

/**
 * Coerce a plain JS object (protobuf-JSON shape) to a protobufjs message
 * of the given type — the activation path for a message-typed variable
 * bound to a JS object (§A.4.6).  Uses protobufjs `fromObject`.
 */
export function coerceObjectToMessage(
  type: protobuf.Type,
  obj: Record<string, unknown>,
): protobuf.Message {
  return type.fromObject(obj);
}

/**
 * Convert a protobufjs message to a plain object of decoded `CelValue`s —
 * the decode path for a returned `CEL_MESSAGE` (§A.4.6).  Recurses through
 * nested messages, repeated fields, and maps, applying the same per-field
 * decode rules as {@link ProtoMessageBacking.readField}.
 */
export function messageToObject(
  msg: protobuf.Message,
): Record<string, CelValue> {
  const type = msg.$type;
  const out: Record<string, CelValue> = {};
  for (const f of type.fieldsArray) {
    const value = fieldsOf(msg)[f.name];
    out[f.name] = decodeField(f, value);
  }
  return out;
}

/**
 * Peel a `google.protobuf.Timestamp` message to a tagged {@link CelTimestamp}.
 * Throws if the message is not a Timestamp.
 */
export function peelTimestamp(msg: protobuf.Message): CelTimestamp {
  const type = msg.$type;
  if (stripDot(type.fullName) !== WKT_TIMESTAMP) {
    throw new Error(`not a Timestamp: '${type.fullName}'`);
  }
  const rec = fieldsOf(msg);
  return {
    kind: 'timestamp',
    epochSeconds: toBigInt(rec.seconds ?? 0),
    nanos: toNumber(rec.nanos ?? 0),
  };
}

/**
 * Peel a `google.protobuf.Duration` message to a tagged {@link CelDuration}.
 * Throws if the message is not a Duration.
 */
export function peelDuration(msg: protobuf.Message): CelDuration {
  const type = msg.$type;
  if (stripDot(type.fullName) !== WKT_DURATION) {
    throw new Error(`not a Duration: '${type.fullName}'`);
  }
  const rec = fieldsOf(msg);
  return {
    kind: 'duration',
    seconds: toBigInt(rec.seconds ?? 0),
    nanos: toNumber(rec.nanos ?? 0),
  };
}

/**
 * Peel a `google.protobuf.*Value` wrapper message to its scalar `CelValue`.
 * The wrapper's single `value` field (number 1) is decoded under the same
 * rules as a normal field read.  Throws if the message is not a wrapper.
 */
export function peelWrapper(msg: protobuf.Message): CelValue {
  const type = msg.$type;
  if (!WKT_WRAPPERS.has(stripDot(type.fullName))) {
    throw new Error(`not a wrapper type: '${type.fullName}'`);
  }
  const f = type.fieldsById[1];
  if (f === undefined) {
    throw new Error(`wrapper '${type.fullName}' has no field 1`);
  }
  const value = fieldsOf(msg)[f.name];
  return decodeField(f, value);
}

/** Whether `fqn` (with or without a leading dot) is a peelable WKT. */
export function isWellKnownWrappable(fqn: string): boolean {
  const name = stripDot(fqn);
  return (
    name === WKT_TIMESTAMP || name === WKT_DURATION || WKT_WRAPPERS.has(name)
  );
}

// ───────────────────────────────────────────────────────────────────
// Internal: field resolution, decode, encode, presence.
// ───────────────────────────────────────────────────────────────────

function resolveField(
  type: protobuf.Type,
  field: number | string,
): protobuf.Field {
  if (typeof field === 'number') {
    const byId = type.fieldsById[field];
    if (byId === undefined) {
      throw new Error(
        `unknown field number ${String(field)} on message '${type.fullName}'`,
      );
    }
    return byId;
  }
  const byName = type.fields[field];
  if (byName === undefined) {
    throw new Error(`unknown field '${field}' on message '${type.fullName}'`);
  }
  return byName;
}

function decodeField(field: protobuf.Field, value: unknown): CelValue {
  if (field.map) {
    return decodeMap(field, asRecord(value));
  }
  if (field.repeated) {
    return decodeRepeated(field, value);
  }
  return decodeSingular(field, value);
}

function decodeSingular(field: protobuf.Field, value: unknown): CelValue {
  field.resolve();
  // Message-typed (including WKT) fields.
  if (field.resolvedType instanceof protobuf.Type) {
    if (value === null || value === undefined) {
      return null;
    }
    return decodeMessageValue(field.resolvedType, value);
  }
  // Enum → CEL INT.
  if (field.resolvedType instanceof protobuf.Enum) {
    return toBigInt(value ?? 0);
  }
  return decodeScalar(field.type, value);
}

function decodeScalar(protoType: string, value: unknown): CelValue {
  switch (protoType) {
    case 'int32':
    case 'sint32':
    case 'sfixed32':
    case 'int64':
    case 'sint64':
    case 'sfixed64':
      return toBigInt(value ?? 0);
    case 'uint32':
    case 'fixed32':
    case 'uint64':
    case 'fixed64':
      return toBigInt(value ?? 0);
    case 'double':
    case 'float':
      return toNumber(value ?? 0);
    case 'bool':
      return value === true;
    case 'string':
      return typeof value === 'string' ? value : '';
    case 'bytes':
      return toBytes(value);
    default:
      throw new Error(`unsupported proto scalar type '${protoType}'`);
  }
}

function decodeMessageValue(type: protobuf.Type, value: unknown): CelValue {
  const msg = asMessage(value, type);
  if (isWellKnownWrappable(type.fullName)) {
    const name = stripDot(type.fullName);
    if (name === WKT_TIMESTAMP) {
      return peelTimestamp(msg);
    }
    if (name === WKT_DURATION) {
      return peelDuration(msg);
    }
    return peelWrapper(msg);
  }
  return messageToObject(msg);
}

function decodeRepeated(field: protobuf.Field, value: unknown): CelValue {
  if (!Array.isArray(value)) {
    return [];
  }
  const elementDecode = singularElementDecoder(field);
  return value.map(elementDecode);
}

function decodeMap(
  field: protobuf.Field,
  value: Record<string, unknown>,
): CelValue {
  const out = new Map<CelValue, CelValue>();
  if (!(field instanceof protobuf.MapField)) {
    throw new Error(`field '${field.name}' is not a map field`);
  }
  const keyType = field.keyType;
  const elementDecode = singularElementDecoder(field);
  for (const [rawKey, rawVal] of Object.entries(value)) {
    out.set(decodeMapKey(keyType, rawKey), elementDecode(rawVal));
  }
  return out;
}

/**
 * The decoder for one element of a repeated/map field — the field's value
 * type decoded as a singular value (the `repeated`/`map` rule is stripped).
 */
function singularElementDecoder(
  field: protobuf.Field,
): (element: unknown) => CelValue {
  field.resolve();
  if (field.resolvedType instanceof protobuf.Type) {
    const type = field.resolvedType;
    return (element) =>
      element === null || element === undefined
        ? null
        : decodeMessageValue(type, element);
  }
  if (field.resolvedType instanceof protobuf.Enum) {
    return (element) => toBigInt(element ?? 0);
  }
  const protoType = field.type;
  return (element) => decodeScalar(protoType, element);
}

function decodeMapKey(keyType: string, rawKey: string): CelValue {
  switch (keyType) {
    case 'int32':
    case 'sint32':
    case 'sfixed32':
    case 'int64':
    case 'sint64':
    case 'sfixed64':
    case 'uint32':
    case 'fixed32':
    case 'uint64':
    case 'fixed64':
      return BigInt(rawKey);
    case 'bool':
      return rawKey === 'true';
    case 'string':
      return rawKey;
    default:
      throw new Error(`unsupported proto map key type '${keyType}'`);
  }
}

function encodeField(field: protobuf.Field, value: CelValue): unknown {
  field.resolve();
  if (field.map || field.repeated) {
    // Aggregate construction is the proto-literal path; protobufjs accepts
    // the JS-natural shapes (Array / object) directly on assignment.
    return value;
  }
  if (field.resolvedType instanceof protobuf.Type) {
    if (value === null) {
      return null;
    }
    if (isPlainObject(value)) {
      return coerceObjectToMessage(field.resolvedType, value);
    }
    return value;
  }
  if (typeof value === 'bigint') {
    // protobufjs accepts a decimal string for 64-bit fields and a number
    // for 32-bit; a string is accepted by both, so normalize to string.
    return value.toString();
  }
  return value;
}

function fieldIsPresent(
  message: protobuf.Message,
  field: protobuf.Field,
): boolean {
  const value = fieldsOf(message)[field.name];
  field.resolve();
  // Message fields: present iff non-null (protobufjs sets unset messages to
  // null, so an own-property null is still "absent" by proto semantics).
  if (field.resolvedType instanceof protobuf.Type && !field.repeated) {
    return value !== null && value !== undefined;
  }
  // Everything else: present iff explicitly assigned on the instance.
  // protobufjs serves unset scalars from the prototype (defaults), so an
  // own enumerable property is the presence signal.
  return Object.prototype.hasOwnProperty.call(message, field.name);
}

// ───────────────────────────────────────────────────────────────────
// Internal: narrowing helpers (no `any`).
// ───────────────────────────────────────────────────────────────────

/**
 * View a protobufjs message as a string-keyed record so its fields can be
 * read/written by name.  protobufjs stores fields as own enumerable
 * properties on the instance (defaults served from the prototype); the
 * public `Message` type carries no index signature, so this funnel
 * is the single narrowing point (through `unknown`) the rest of the module
 * relies on.
 */
function fieldsOf(message: protobuf.Message): Record<string, unknown> {
  return message as unknown as Record<string, unknown>;
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
  throw new Error(`cannot decode integer field from ${describe(value)}`);
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
  throw new Error(`cannot decode numeric field from ${describe(value)}`);
}

function toBytes(value: unknown): Uint8Array {
  if (value instanceof Uint8Array) {
    return value;
  }
  if (Array.isArray(value)) {
    return Uint8Array.from(value as number[]);
  }
  return new Uint8Array(0);
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

function isPlainObject(value: CelValue): value is Record<string, CelValue> {
  return (
    typeof value === 'object' &&
    value !== null &&
    !Array.isArray(value) &&
    !(value instanceof Uint8Array) &&
    !(value instanceof Map) &&
    !('kind' in value)
  );
}

function asRecord(value: unknown): Record<string, unknown> {
  if (typeof value === 'object' && value !== null) {
    return value as Record<string, unknown>;
  }
  return {};
}

function asMessage(value: unknown, type: protobuf.Type): protobuf.Message {
  if (typeof value !== 'object' || value === null) {
    throw new Error(`expected a message value for '${type.fullName}'`);
  }
  // A bare protobuf-JSON object reaching here (e.g. from a hand-built
  // instance) is coerced through the type so nested decode is uniform.
  if ((value as { $type?: unknown }).$type === undefined) {
    return type.fromObject(value as Record<string, unknown>);
  }
  return value as protobuf.Message;
}

function describe(value: unknown): string {
  return value === null ? 'null' : typeof value;
}

function stripDot(name: string): string {
  return name.startsWith('.') ? name.slice(1) : name;
}
