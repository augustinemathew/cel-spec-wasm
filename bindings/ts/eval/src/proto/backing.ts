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

// The three dynamic JSON-value WKTs (`google.protobuf.{Value,Struct,ListValue}`).
// A CEL scalar / map / list assigned to a field of one of these types is
// *wrapped* into the dynamic message rather than assigned verbatim — cel-cpp
// performs the JSON conversion of doc/langdef.md §"JSON Data Conversion"
// (e.g. `TestAllTypes{single_value: 'foo'}` yields `Value{string_value:'foo'}`,
// `single_struct: {'one': 1}` yields `Struct{fields{'one': number_value:1}}`).
const WKT_VALUE = 'google.protobuf.Value';
const WKT_STRUCT = 'google.protobuf.Struct';
const WKT_LIST_VALUE = 'google.protobuf.ListValue';
const WKT_DYNAMIC = new Set<string>([WKT_VALUE, WKT_STRUCT, WKT_LIST_VALUE]);

// int32 / uint32 wire domains — proto narrows int64/uint64 CEL values into
// 32-bit fields, and an out-of-domain value is a CEL "range error", not a
// silent truncation (`eval/internal/cel_host.cc` CheckInt32Range /
// CheckUint32Range).
const INT32_MIN = -2147483648n;
const INT32_MAX = 2147483647n;
const UINT32_MAX = 4294967295n;

/**
 * An out-of-range proto field assignment (int32 / uint32 / enum narrowing).
 * cel-cpp surfaces this as a CEL eval error ("range error"), not a host
 * trap — the `cel_set_field` trampoline catches this error and poisons the
 * message slot with CEL_ERROR(OVERFLOW), mirroring `CelSetFieldImpl`'s
 * kOutOfRange arm (`eval/internal/cel_host.cc`).
 */
export class ProtoFieldRangeError extends Error {
  constructor(fieldName: string, domain: string, value: bigint) {
    super(`field '${fieldName}' ${domain} range error: ${String(value)}`);
    this.name = 'ProtoFieldRangeError';
  }
}

function checkInt32Range(v: bigint, fieldName: string): bigint {
  if (v < INT32_MIN || v > INT32_MAX) {
    throw new ProtoFieldRangeError(fieldName, 'int32', v);
  }
  return v;
}

function checkUint32Range(v: bigint, fieldName: string): bigint {
  if (v < 0n || v > UINT32_MAX) {
    throw new ProtoFieldRangeError(fieldName, 'uint32', v);
  }
  return v;
}

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
 * bound to a JS object (§A.4.6).  Uses protobufjs `fromObject`, after
 * normalizing map-field values: a JSON-natural `{ K: V }` record (or a JS
 * `Map`) bound to a map field is rewritten to the shape the field's
 * descriptor source expects — a `Root.fromDescriptor` map field is a
 * synthetic repeated `{key, value}` entry field (see {@link isMapField}),
 * for which `fromObject` wants an entry array, not a record.
 */
export function coerceObjectToMessage(
  type: protobuf.Type,
  obj: Record<string, unknown>,
): protobuf.Message {
  return type.fromObject(normalizeObjectForType(type, obj));
}

/**
 * Rewrite `obj`'s map-field values into the shape `type.fromObject`
 * accepts, recursing through nested message fields (singular and
 * repeated) so a nested map normalizes too.  Non-field keys and already
 * conformant values pass through untouched.
 */
function normalizeObjectForType(
  type: protobuf.Type,
  obj: Record<string, unknown>,
): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(obj)) {
    const field: protobuf.Field | undefined = type.fields[key];
    out[key] = field === undefined ? value : normalizeFieldValue(field, value);
  }
  return out;
}

/** Normalize one bound field value (map / repeated / singular). */
function normalizeFieldValue(field: protobuf.Field, value: unknown): unknown {
  field.resolve();
  if (field instanceof protobuf.MapField) {
    // A real MapField (descriptors from .proto source) wants a `{ K: V }`
    // record; accept a JS `Map` by stringifying its keys.
    if (value instanceof Map) {
      const record: Record<string, unknown> = {};
      for (const [k, v] of value as Map<unknown, unknown>) {
        record[String(k)] = v;
      }
      return record;
    }
    return value;
  }
  if (isMapField(field)) {
    return normalizeSyntheticMapEntries(field, value);
  }
  if (field.repeated && Array.isArray(value)) {
    return value.map((element) => normalizeSingularValue(field, element));
  }
  return normalizeSingularValue(field, value);
}

/**
 * A synthetic map-entry repeated field (`Root.fromDescriptor`): rewrite a
 * `{ K: V }` record or a JS `Map` to the `[{key, value}, …]` entry array
 * `fromObject` expects.  An already entry-shaped array passes through.
 */
function normalizeSyntheticMapEntries(
  field: protobuf.Field,
  value: unknown,
): unknown {
  const entryType = field.resolvedType;
  if (!(entryType instanceof protobuf.Type)) {
    return value;
  }
  const keyField = entryType.fields.key;
  const valueField = entryType.fields.value;
  const entries: [unknown, unknown][] | undefined =
    value instanceof Map
      ? [...(value as Map<unknown, unknown>).entries()]
      : isRecordObject(value)
        ? Object.entries(value)
        : undefined;
  if (
    entries === undefined ||
    keyField === undefined ||
    valueField === undefined
  ) {
    return value; // already entry-shaped (array) or not coercible — pass through
  }
  return entries.map(([key, entryValue]) => ({
    key: normalizeSingularValue(keyField, key),
    value: normalizeSingularValue(valueField, entryValue),
  }));
}

/**
 * Normalize one singular value: recurse into a nested message-typed
 * record, and rewrite a `bigint` to the shape protobufjs `fromObject`
 * accepts (a Number for 32-bit fields, a decimal string for 64-bit Long
 * fields — `fromObject`'s generated converters reject bigint on 32-bit
 * fields and lose uint64 unsignedness, while {@link encodeScalar} also
 * range-checks the 32-bit narrowing).  Other scalars pass through.
 */
function normalizeSingularValue(
  field: protobuf.Field,
  value: unknown,
): unknown {
  if (typeof value === 'bigint') {
    return encodeScalar(field, value);
  }
  if (
    isRecordObject(value) &&
    field.resolvedType instanceof protobuf.Type &&
    !('$type' in value)
  ) {
    return normalizeObjectForType(field.resolvedType, value);
  }
  return value;
}

/** A plain record object (not an array / Map / bytes / protobufjs message). */
function isRecordObject(value: unknown): value is Record<string, unknown> {
  return (
    typeof value === 'object' &&
    value !== null &&
    !Array.isArray(value) &&
    !(value instanceof Uint8Array) &&
    !(value instanceof Map)
  );
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

/**
 * Whether `fqn` names a WKT into which a CEL scalar / map / list is *wrapped*
 * on field construction: the nine `google.protobuf.*Value` wrappers plus the
 * dynamic `Value` / `Struct` / `ListValue`.  (Timestamp / Duration are NOT
 * here — those are constructed from their own `seconds`/`nanos` object shape,
 * not wrapped from a scalar.)  See {@link wrapWellKnownValue}.
 */
export function isWellKnownConstructable(fqn: string): boolean {
  const name = stripDot(fqn);
  return WKT_WRAPPERS.has(name) || WKT_DYNAMIC.has(name);
}

/**
 * Wrap a CEL value into a constructed protobufjs message of the WKT `type`, the
 * construct-direction counterpart of {@link peelWrapper}.  cel-cpp applies the
 * JSON conversion of doc/langdef.md §"JSON Data Conversion" when a scalar / map
 * / list is assigned to a WKT-typed field (e.g. `TestAllTypes{single_value:
 * 'foo'}` yields `Value{string_value:'foo'}`).  The message is built against the
 * supplied `type` (and its `Value`/`Struct`/`ListValue` siblings resolved off
 * the same `Root`) so the shape matches whatever descriptor source the caller
 * loaded — in particular `Struct.fields` arriving as a synthetic map-entry
 * `repeated` field under `Root.fromDescriptor` (see {@link isMapField}).
 *
 *   - `*Value` wrapper → `value: <scalar>` (the inner field decodes under the
 *     wrapper's own descriptor — a bigint becomes a Long for Int64Value, a
 *     number for Int32Value, etc.).
 *   - `Value` → the JSON-value oneof: null→`null_value`, bool→`bool_value`,
 *     number/bigint→`number_value` (a `double`, per langdef — `{single_value:
 *     1}` yields `number_value:1.0`), string→`string_value`, Map→`struct_value`,
 *     Array→`list_value`.
 *   - `Struct` → `fields[<k>] = Value` from a string-keyed Map.
 *   - `ListValue` → `values = [Value, …]` from an Array.
 */
export function wrapWellKnownValue(
  type: protobuf.Type,
  value: CelValue,
): protobuf.Message {
  const name = stripDot(type.fullName);
  if (WKT_WRAPPERS.has(name)) {
    return type.fromObject({ value: wrapperInner(name, value) });
  }
  if (name === WKT_VALUE) {
    return buildValue(type, value);
  }
  if (name === WKT_STRUCT) {
    return buildStruct(type, value);
  }
  // WKT_LIST_VALUE
  return buildListValue(type, value);
}

// The inner `value` field of a `*Value` wrapper.  A bigint is normalized to a
// decimal string (protobufjs accepts a string for 64-bit fields and parses it
// for 32-bit), a Uint8Array stays as-is (BytesValue); everything else passes
// through (number / string / bool).  The 32-bit wrappers range-check before
// narrowing — an out-of-domain value is a CEL range error, not a truncation
// (`eval/internal/cel_host.cc` SetWrapperInnerValue).
function wrapperInner(wrapperFqn: string, value: CelValue): unknown {
  if (typeof value === 'bigint') {
    if (wrapperFqn === 'google.protobuf.Int32Value') {
      return checkInt32Range(value, wrapperFqn).toString();
    }
    if (wrapperFqn === 'google.protobuf.UInt32Value') {
      return checkUint32Range(value, wrapperFqn).toString();
    }
    return value.toString();
  }
  return value;
}

// Resolve a sibling WKT type (`Value` / `Struct` / `ListValue`) off the same
// Root as `anchor`, so a recursively-built sub-value matches the caller's
// descriptor source.
function siblingType(anchor: protobuf.Type, fqn: string): protobuf.Type {
  const resolved = anchor.root.lookup(fqn, [protobuf.Type]);
  if (!(resolved instanceof protobuf.Type)) {
    throw new Error(`WKT '${fqn}' not in descriptor set`);
  }
  return resolved;
}

// A CEL value → a constructed `google.protobuf.Value` message (its oneof set by
// CEL kind).
function buildValue(
  valueType: protobuf.Type,
  value: CelValue,
): protobuf.Message {
  if (value === null) {
    return valueType.fromObject({ null_value: 0 });
  }
  if (typeof value === 'boolean') {
    return valueType.fromObject({ bool_value: value });
  }
  if (typeof value === 'bigint') {
    // Per langdef §"JSON Data Conversion": int/uint map to a JSON Number
    // (`double`); cel-cpp encodes `{single_value: 1}` as `number_value: 1.0`.
    return valueType.fromObject({ number_value: Number(value) });
  }
  if (typeof value === 'number') {
    return valueType.fromObject({ number_value: value });
  }
  if (typeof value === 'string') {
    return valueType.fromObject({ string_value: value });
  }
  if (value instanceof Map) {
    const struct = buildStruct(siblingType(valueType, WKT_STRUCT), value);
    return setOneof(valueType, 'struct_value', struct);
  }
  if (Array.isArray(value)) {
    const list = buildListValue(siblingType(valueType, WKT_LIST_VALUE), value);
    return setOneof(valueType, 'list_value', list);
  }
  // Bytes / timestamp / duration / type have no JSON-Value image (langdef);
  // cel-cpp raises an error.  An empty Value (no oneof set) is the closest the
  // binding can build; the construction read-back surfaces the gap.
  return valueType.create();
}

// Construct a message of `type` with a single message-typed oneof/field set to
// an already-built sub-message (bypassing `fromObject`, which would re-coerce
// the sub-message's nested map shape and re-trip the map-entry quirk).
function setOneof(
  type: protobuf.Type,
  fieldName: string,
  sub: protobuf.Message,
): protobuf.Message {
  const msg = type.create();
  (msg as unknown as Record<string, unknown>)[fieldName] = sub;
  return msg;
}

// A CEL Map → a constructed `google.protobuf.Struct` message.  The `fields` map
// is assigned in whichever shape the descriptor source exposes: a record for a
// real `MapField`, or an array of `{key, value}` entry messages for the
// synthetic map-entry `repeated` field `Root.fromDescriptor` produces.
function buildStruct(
  structType: protobuf.Type,
  value: CelValue,
): protobuf.Message {
  const fieldsField = structType.fields.fields;
  if (fieldsField === undefined) {
    throw new Error(`Struct type '${structType.fullName}' has no fields field`);
  }
  fieldsField.resolve();
  const valueType = siblingType(structType, WKT_VALUE);
  const entries = new Map<string, protobuf.Message>();
  if (value instanceof Map) {
    for (const [k, v] of value) {
      entries.set(structKey(k), buildValue(valueType, v));
    }
  }
  const struct = structType.create();
  assignStructFields(struct, fieldsField, entries);
  return struct;
}

// Assign the entry map onto a Struct's `fields`, matching the field's shape: a
// record for a real `MapField`, an array of `{key, value}` entry messages for
// the synthetic map-entry `repeated` field.
function assignStructFields(
  struct: protobuf.Message,
  fieldsField: protobuf.Field,
  entries: ReadonlyMap<string, protobuf.Message>,
): void {
  const target = struct as unknown as Record<string, unknown>;
  if (isMapField(fieldsField)) {
    const record: Record<string, protobuf.Message> = {};
    for (const [k, v] of entries) {
      record[k] = v;
    }
    target.fields = record;
    return;
  }
  const array: Record<string, unknown>[] = [];
  for (const [k, v] of entries) {
    array.push({ key: k, value: v });
  }
  target.fields = array;
}

// The string key a CEL map key spells on a `google.protobuf.Struct`.  Struct
// keys are strings (langdef §"Dynamic Values": "map with string keys"); cel-cpp
// restricts the key type at check time, so a string is the only kind that
// reaches a Struct field.  The other JS-natural scalar key shapes are spelled
// explicitly (never via `String(unknown)`) for a best-effort encodable key.
function structKey(key: CelValue): string {
  if (typeof key === 'string') {
    return key;
  }
  if (typeof key === 'bigint' || typeof key === 'number') {
    return key.toString();
  }
  if (typeof key === 'boolean') {
    return key ? 'true' : 'false';
  }
  return '';
}

// A CEL Array → a constructed `google.protobuf.ListValue` message.
function buildListValue(
  listType: protobuf.Type,
  value: CelValue,
): protobuf.Message {
  const valueType = siblingType(listType, WKT_VALUE);
  const values = Array.isArray(value)
    ? value.map((v) => buildValue(valueType, v))
    : [];
  const list = listType.create();
  (list as unknown as Record<string, unknown>).values = values;
  return list;
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
  if (isMapField(field)) {
    return decodeMap(field, value);
  }
  if (field.repeated) {
    return decodeRepeated(field, value);
  }
  return decodeSingular(field, value);
}

/**
 * True iff `field` is a protobuf map field.  A `Root` built from a
 * `FileDescriptorSet` (`Root.fromDescriptor`) does not reconstruct
 * `MapField` instances — a `map<K,V>` arrives as a `repeated` field whose
 * resolved element type is the synthetic `*Entry` message carrying the
 * `map_entry` option.  `field.map` is therefore unreliable across descriptor
 * sources; the `map_entry` option on the resolved element type is the
 * authoritative signal (and matches what `protoc` stamps).
 */
function isMapField(field: protobuf.Field): boolean {
  if (field.map) {
    return true;
  }
  if (!field.repeated) {
    return false;
  }
  field.resolve();
  const elem = field.resolvedType;
  // The `map_entry` option is necessary but not sufficient: protobufjs's
  // `Root.fromDescriptor` over-stamps it onto types that merely *contain* a
  // map field (e.g. `google.protobuf.Struct`).  A genuine synthetic map-entry
  // type also has exactly the two fields `key` and `value`; require both.
  if (
    !(elem instanceof protobuf.Type) ||
    (elem.options as { map_entry?: boolean } | undefined)?.map_entry !== true
  ) {
    return false;
  }
  const names = Object.keys(elem.fields);
  return (
    names.length === 2 &&
    elem.fields.key !== undefined &&
    elem.fields.value !== undefined
  );
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
      return toNumber(value ?? 0);
    case 'float':
      // A float field carries float32 precision: narrow on decode so a
      // double stored by construction reads back as protobuf reflection
      // would serve it (e.g. `FloatValue{value: 1.333}` reads 1.333f ≠
      // 1.333; `1e-50` rounds to 0; `1.4e55` rounds to +inf) — cel-cpp
      // parity via `static_cast<float>` in `eval/internal/cel_host.cc`.
      return Math.fround(toNumber(value ?? 0));
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

function decodeMap(field: protobuf.Field, value: unknown): CelValue {
  const out = new Map<CelValue, CelValue>();
  const { keyType, keyDecode, valueDecode } = mapDecoders(field);
  // A `MapField` stores its entries as a `{ K: V }` record; a synthetic
  // map-entry `repeated` field (descriptors from `Root.fromDescriptor`)
  // stores an array of `{ key, value }` entry messages.  Handle both.
  if (Array.isArray(value)) {
    for (const entry of value) {
      const rec = asRecord(entry);
      out.set(keyDecode(rec.key), valueDecode(rec.value));
    }
    return out;
  }
  for (const [rawKey, rawVal] of Object.entries(asRecord(value))) {
    out.set(decodeMapKey(keyType, rawKey), valueDecode(rawVal));
  }
  return out;
}

/**
 * The (keyType, value-element decoder) pair for a map field.  A `MapField`
 * (descriptors from `.proto` source / the generated pool) carries `keyType`
 * directly and its value type is the field's own resolved type.  A synthetic
 * map-entry `repeated` field (descriptors from `Root.fromDescriptor`) instead
 * exposes the entry message's `key` / `value` sub-fields — read both off the
 * entry type.
 */
function mapDecoders(field: protobuf.Field): {
  keyType: string;
  keyDecode: (raw: unknown) => CelValue;
  valueDecode: (element: unknown) => CelValue;
} {
  if (field instanceof protobuf.MapField) {
    return {
      keyType: field.keyType,
      keyDecode: (raw) => decodeMapKey(field.keyType, stringifyMapKey(raw)),
      valueDecode: singularElementDecoder(field),
    };
  }
  field.resolve();
  const entry = field.resolvedType;
  if (!(entry instanceof protobuf.Type)) {
    throw new Error(`map field '${field.name}' has no entry type`);
  }
  const keyField = entry.fields.key;
  const valueField = entry.fields.value;
  if (keyField === undefined || valueField === undefined) {
    throw new Error(`map field '${field.name}' entry lacks key/value`);
  }
  keyField.resolve();
  valueField.resolve();
  const keyType = keyField.type;
  return {
    keyType,
    keyDecode: (raw) => decodeMapKey(keyType, stringifyMapKey(raw)),
    valueDecode: singularElementDecoder(valueField),
  };
}

// A map key off a synthetic entry message is a proto scalar (string / number /
// bool / Long).  `decodeMapKey` re-parses it from a string, so normalize any
// scalar form to its string spelling without tripping a base-to-string lint on
// `unknown`.
function stringifyMapKey(raw: unknown): string {
  if (typeof raw === 'string') {
    return raw;
  }
  if (
    typeof raw === 'number' ||
    typeof raw === 'bigint' ||
    typeof raw === 'boolean'
  ) {
    return String(raw);
  }
  if (isLongLike(raw)) {
    return raw.toString();
  }
  return '';
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
  if (isMapField(field)) {
    return encodeMapField(field, value);
  }
  if (field.repeated) {
    return encodeRepeatedField(field, value);
  }
  return encodeSingular(field, value);
}

// Encode one singular value against `field`'s (element) type — shared by the
// singular set path, repeated elements, and map values (the `repeated`/`map`
// rule is stripped by the callers, mirroring `singularElementDecoder`).
function encodeSingular(field: protobuf.Field, value: CelValue): unknown {
  field.resolve();
  if (field.resolvedType instanceof protobuf.Type) {
    return encodeMessageValue(field.resolvedType, value);
  }
  if (field.resolvedType instanceof protobuf.Enum) {
    // Enum values narrow to int32 on the wire; an out-of-range assignment is
    // a CEL range error (`eval/internal/cel_host.cc` SetScalarField
    // CPPTYPE_ENUM arm), not a truncation.
    return Number(checkInt32Range(toBigInt(value ?? 0), field.name));
  }
  return encodeScalar(field, value);
}

// Encode a CEL value into a message-typed field/element of `type`:
//   - `null` clears (leaves the field unset / prunes the element) for every
//     message type EXCEPT `google.protobuf.Value`, which packs an explicit
//     `null_value` (cel-cpp `SetScalarField` CPPTYPE_MESSAGE CEL_NULL arm;
//     corpus `wrappers/*/to_null`, `dynamic/value/*`).
//   - a WKT wrapper / Value / Struct / ListValue from a non-message CEL
//     value wraps via {@link wrapWellKnownValue} (langdef §"JSON Data
//     Conversion").
//   - a tagged timestamp / duration record packs into the matching WKT
//     message (cel-cpp `MaybeSetWktMessageField`).
//   - a plain object coerces through the descriptor; an already-built
//     protobufjs message assigns verbatim.
function encodeMessageValue(type: protobuf.Type, value: CelValue): unknown {
  const name = stripDot(type.fullName);
  if (value === null && name !== WKT_VALUE) {
    return null;
  }
  if (isWellKnownConstructable(name) && !isMessageObject(value)) {
    return wrapWellKnownValue(type, value);
  }
  if (name === WKT_TIMESTAMP && isCelTimestamp(value)) {
    return type.fromObject({
      seconds: value.epochSeconds.toString(),
      nanos: value.nanos,
    });
  }
  if (name === WKT_DURATION && isCelDuration(value)) {
    return type.fromObject({
      seconds: value.seconds.toString(),
      nanos: value.nanos,
    });
  }
  if (isPlainObject(value)) {
    return coerceObjectToMessage(type, value);
  }
  return value;
}

function encodeScalar(field: protobuf.Field, value: CelValue): unknown {
  if (typeof value === 'bigint') {
    // 32-bit integer fields range-check before narrowing (cel-cpp
    // `CheckInt32Range`/`CheckUint32Range`); 64-bit fields normalize to a
    // decimal string, which protobufjs accepts for Long fields.
    switch (field.type) {
      case 'int32':
      case 'sint32':
      case 'sfixed32':
        return Number(checkInt32Range(value, field.name));
      case 'uint32':
      case 'fixed32':
        return Number(checkUint32Range(value, field.name));
      default:
        return value.toString();
    }
  }
  return value;
}

// Encode a CEL list into a repeated field: each element encodes under the
// element type; a `null` element of a message-typed repeated field is PRUNED
// (skipped, not appended) — `[timestamp(1), null]` round-trips as
// `[timestamp(1)]` (cel-cpp `AppendRepeatedFromCelValue` CPPTYPE_MESSAGE
// CEL_NULL arm; corpus `set_null/repeated_*_null_pruned`).
function encodeRepeatedField(field: protobuf.Field, value: CelValue): unknown {
  if (!Array.isArray(value)) {
    return [];
  }
  field.resolve();
  const messageElement = field.resolvedType instanceof protobuf.Type;
  const out: unknown[] = [];
  for (const element of value) {
    if (messageElement && element === null) {
      continue;
    }
    out.push(encodeSingular(field, element));
  }
  return out;
}

// Encode a CEL map into a map field, in whichever shape the descriptor
// source exposes (a `{K: V}` record for a real `MapField`, an array of
// `{key, value}` entry objects for the synthetic map-entry `repeated` field —
// see {@link isMapField}).  A `null` value of a message-typed map field
// PRUNES the whole entry (cel-cpp `InsertArenaMapEntry`; corpus
// `set_null/map_*_null_pruned`).
function encodeMapField(field: protobuf.Field, value: CelValue): unknown {
  const { valueField, asEntryArray } = mapEncodeShape(field);
  valueField.resolve();
  const messageValue = valueField.resolvedType instanceof protobuf.Type;
  const entries: { key: unknown; value: unknown }[] = [];
  if (value instanceof Map) {
    for (const [k, v] of value) {
      if (messageValue && v === null) {
        continue;
      }
      entries.push({
        key: encodeMapKey(k),
        value: encodeSingular(valueField, v),
      });
    }
  }
  if (asEntryArray) {
    return entries;
  }
  const record: Record<string, unknown> = {};
  for (const e of entries) {
    record[stringifyMapKey(e.key)] = e.value;
  }
  return record;
}

// The value sub-field + target shape for a map-field encode.  A `MapField`
// carries its value type on itself (same trick as `singularElementDecoder`);
// a synthetic map-entry `repeated` field exposes the entry message's `value`
// sub-field.
function mapEncodeShape(field: protobuf.Field): {
  valueField: protobuf.Field;
  asEntryArray: boolean;
} {
  if (field instanceof protobuf.MapField) {
    return { valueField: field, asEntryArray: false };
  }
  field.resolve();
  const entry = field.resolvedType;
  if (!(entry instanceof protobuf.Type) || entry.fields.value === undefined) {
    throw new Error(`map field '${field.name}' has no entry value type`);
  }
  return { valueField: entry.fields.value, asEntryArray: true };
}

// A CEL map key as the protobufjs-acceptable entry key (bool / string pass
// through; int/uint keys go through as decimal strings, which protobufjs
// parses for any integer key type).
function encodeMapKey(key: CelValue): unknown {
  if (typeof key === 'bigint') {
    return key.toString();
  }
  return key;
}

// proto presence, mirroring cel-cpp `ProtoBacking::HasField`
// (`eval/internal/cel_host.cc`): repeated / map fields are present iff
// non-empty (`FieldSize > 0`); singular message fields iff set (non-null);
// explicit-presence scalars (proto2, oneof members, proto3 `optional`) iff
// assigned; implicit-presence scalars (proto3) iff != the type default —
// langdef §"Field Selection" `has()` semantics.
function fieldIsPresent(
  message: protobuf.Message,
  field: protobuf.Field,
): boolean {
  const value = fieldsOf(message)[field.name];
  field.resolve();
  if (isMapField(field)) {
    return mapFieldSize(value) > 0;
  }
  if (field.repeated) {
    return Array.isArray(value) && value.length > 0;
  }
  if (field.resolvedType instanceof protobuf.Type) {
    return value !== null && value !== undefined;
  }
  if (field.hasPresence) {
    // Explicit presence: protobufjs serves unset scalars from the prototype
    // (defaults), so an own enumerable property is the assignment signal.
    return (
      Object.prototype.hasOwnProperty.call(message, field.name) &&
      value !== undefined &&
      value !== null
    );
  }
  // Implicit presence (proto3 scalars / enums): present iff the decoded
  // value differs from the field's default.
  return !isDefaultScalar(decodeField(field, value));
}

// The number of entries a map field's stored value carries — a record for a
// real `MapField`, an array of entry messages for the synthetic map-entry
// `repeated` shape (see {@link isMapField}).
function mapFieldSize(value: unknown): number {
  if (Array.isArray(value)) {
    return value.length;
  }
  if (typeof value === 'object' && value !== null) {
    return Object.keys(value).length;
  }
  return 0;
}

// Whether a decoded scalar / enum CelValue is its proto default (the
// implicit-presence "absent" probe).  Only scalar shapes reach here — the
// message / repeated / map arms are handled before the decode.
function isDefaultScalar(value: CelValue): boolean {
  if (typeof value === 'bigint') {
    return value === 0n;
  }
  if (typeof value === 'number') {
    return value === 0;
  }
  if (typeof value === 'boolean') {
    return !value;
  }
  if (typeof value === 'string') {
    return value === '';
  }
  if (value instanceof Uint8Array) {
    return value.length === 0;
  }
  return value === null;
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

function isCelTimestamp(value: CelValue): value is CelTimestamp {
  return (
    typeof value === 'object' &&
    value !== null &&
    'kind' in value &&
    (value as { kind: unknown }).kind === 'timestamp'
  );
}

function isCelDuration(value: CelValue): value is CelDuration {
  return (
    typeof value === 'object' &&
    value !== null &&
    'kind' in value &&
    (value as { kind: unknown }).kind === 'duration'
  );
}

// True iff `value` is already a constructed protobufjs message (carries a
// `$type`).  Such a value reaching the WKT-construct path is assigned verbatim,
// not re-wrapped — it is the message itself, not a scalar to wrap.
function isMessageObject(value: CelValue): boolean {
  return (
    typeof value === 'object' &&
    value !== null &&
    !Array.isArray(value) &&
    !(value instanceof Uint8Array) &&
    !(value instanceof Map) &&
    '$type' in value
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
