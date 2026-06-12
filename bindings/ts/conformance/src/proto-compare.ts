// Builds the expected proto message for an `object_value` matcher and
// decodes it to the SAME plain-object shape `instance.eval` returns for a
// constructed message, so the row's expected value and the got value can be
// deep-compared field-by-field.
//
// The got value of a proto-construction row is `messageToObject(backing.raw)`
// (`eval/src/resolving-codec.ts` → `proto/backing.ts`): every field decoded
// to a `CelValue`, defaults materialised, WKTs peeled.  To produce a
// comparable expected value, this module:
//
//   1. converts the matcher's parsed textproto body (`TextprotoMessage`) into
//      a protobufjs-`fromObject`-acceptable plain object against the field
//      descriptors,
//   2. builds the protobufjs `Message`,
//   3. decodes it through the binding's own `messageToObject` — the identical
//      decode the eval path uses — so the two trees are produced by one set
//      of rules.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7;
//       eval/src/proto/backing.ts (messageToObject).

import type { CelInput, CelValue } from '@cel-wasm/eval';
import { DescriptorSet, messageToObject } from '@cel-wasm/eval/proto';
import * as protobuf from 'protobufjs';

import type { TextprotoMessage, TextprotoValue } from './textproto.js';

/**
 * Build the decoded-object form of an `object_value` matcher's expected
 * message, against `descriptors`.  Returns the same `{ [field]: CelValue }`
 * shape `instance.eval` returns for a constructed message of type `fqn`, or
 * throws if `fqn` is not in the descriptor set / the body has a field the
 * type does not declare.
 */
export function buildExpectedMessage(
  descriptors: DescriptorSet,
  fqn: string,
  body: TextprotoMessage,
): CelValue {
  return messageToObject(buildBoundMessage(descriptors, fqn, body));
}

/**
 * Build the protobufjs `Message` for a textproto message body against
 * `descriptors` — the raw message itself (bindable into an activation as a
 * message-typed variable), as opposed to {@link buildExpectedMessage}'s
 * decoded-object form (comparable to an eval result).  Both paths share the
 * one textproto → `fromObject` conversion.  Throws if `fqn` is not in the
 * descriptor set or the body sets a field the type does not declare.
 */
export function buildBoundMessage(
  descriptors: DescriptorSet,
  fqn: string,
  body: TextprotoMessage,
): protobuf.Message {
  const type = descriptors.messageType(fqn);
  return type.fromObject(textprotoToObject(type, body));
}

/**
 * Lower an `object_value` binding to the {@link CelInput} the activation
 * marshal accepts for the variable's declared type.  The time WKTs lower to
 * the binding's duration / timestamp records (a `google.protobuf.Duration`
 * variable declares `repr=DURATION`, not `MESSAGE` — the marshal rejects a
 * protobufjs message there); every other type binds the protobufjs message
 * itself.
 */
export function buildBindingInput(
  descriptors: DescriptorSet,
  fqn: string,
  body: TextprotoMessage,
): CelInput {
  const message = buildBoundMessage(descriptors, fqn, body);
  if (fqn === 'google.protobuf.Duration') {
    return timeRecord('duration', message);
  }
  if (fqn === 'google.protobuf.Timestamp') {
    return timeRecord('timestamp', message);
  }
  return message;
}

// The CelInput duration / timestamp record off a built time-WKT message.
// `seconds` is a protobufjs Long (or number) — stringify for an exact
// bigint; `nanos` is an int32.
function timeRecord(
  kind: 'duration' | 'timestamp',
  message: protobuf.Message,
): CelInput {
  const fields = message as unknown as {
    seconds?: number | { toString(): string };
    nanos?: number;
  };
  return {
    kind,
    seconds: BigInt(fields.seconds?.toString() ?? '0'),
    nanos: Number(fields.nanos ?? 0),
  };
}

/** Load a `DescriptorSet` from a serialized `FileDescriptorSet`. */
export function loadDescriptorSet(fdsBytes: Uint8Array): DescriptorSet {
  return DescriptorSet.fromFileDescriptorSet(fdsBytes);
}

// ───────────────────────────────────────────────────────────────────
// textproto body → protobufjs `fromObject` input.
// ───────────────────────────────────────────────────────────────────

/**
 * Convert a parsed textproto message body to a plain object protobufjs
 * `fromObject` accepts, resolving each field against `type`'s descriptor.
 * Repeated fields become arrays; map fields (a protobufjs map field rendered
 * as repeated `key/value` entry submessages) become objects; nested messages
 * recurse; scalars convert per the field's proto type.
 */
function textprotoToObject(
  type: protobuf.Type,
  body: TextprotoMessage,
): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [fieldName, values] of body.fields) {
    const field = type.fields[fieldName];
    if (field === undefined) {
      throw new Error(`message '${type.fullName}' has no field '${fieldName}'`);
    }
    field.resolve();
    out[fieldName] = convertField(field, values);
  }
  return out;
}

function convertField(
  field: protobuf.Field,
  values: readonly TextprotoValue[],
): unknown {
  const entry = mapEntryType(field);
  if (entry !== undefined) {
    return convertMapEntries(field, entry, values);
  }
  if (field.repeated) {
    return values.map((v) => convertSingular(field, v));
  }
  const first = values[0];
  if (first === undefined) {
    return undefined;
  }
  return convertSingular(field, first);
}

/**
 * The synthetic map-entry message type for a map field, or `undefined` if
 * `field` is not a map field.  A `Root.fromDescriptor` map field is a repeated
 * field whose resolved type is the `*Entry` message (`map_entry` option, with
 * `key`/`value` sub-fields); a `MapField` (from `.proto` source) carries the
 * key/value types directly — synthesize the same shape so the two paths share
 * `convertMapEntries`.
 */
function mapEntryType(field: protobuf.Field): protobuf.Type | undefined {
  field.resolve();
  if (field instanceof protobuf.MapField) {
    return field.resolvedType instanceof protobuf.Type
      ? field.resolvedType
      : undefined;
  }
  if (!field.repeated) {
    return undefined;
  }
  const elem = field.resolvedType;
  if (
    elem instanceof protobuf.Type &&
    (elem.options as { map_entry?: boolean } | undefined)?.map_entry === true &&
    elem.fields.key !== undefined &&
    elem.fields.value !== undefined &&
    Object.keys(elem.fields).length === 2
  ) {
    return elem;
  }
  return undefined;
}

// A textproto map renders as repeated `key: K value: V` entry submessages
// (`field { key: K value: V }`).  protobufjs `fromObject` expects the shape
// matching how the field was modelled: a `MapField` (from `.proto` source)
// wants a `{ K: V }` object; a `Root.fromDescriptor` field wants the raw
// repeated array of `{ key, value }` entry objects.  Emit whichever the
// field's own form requires.
function convertMapEntries(
  field: protobuf.Field,
  entryType: protobuf.Type,
  values: readonly TextprotoValue[],
): unknown {
  const keyField = entryType.fields.key;
  const valueField = entryType.fields.value;
  if (keyField === undefined || valueField === undefined) {
    throw new Error(`map entry '${entryType.fullName}' lacks key/value`);
  }
  keyField.resolve();
  valueField.resolve();
  const asArray = !(field instanceof protobuf.MapField);
  const obj: Record<string, unknown> = {};
  const arr: { key: unknown; value: unknown }[] = [];
  for (const entry of values) {
    if (entry.kind !== 'message') {
      continue;
    }
    const keyTok = entry.fields.get('key')?.[0];
    const valTok = entry.fields.get('value')?.[0];
    if (keyTok === undefined || valTok === undefined) {
      continue;
    }
    const key = convertScalarByProtoType(keyField.type, keyTok);
    const value = convertMapValue(valueField, valTok);
    if (asArray) {
      arr.push({ key, value });
    } else {
      obj[String(key)] = value;
    }
  }
  return asArray ? arr : obj;
}

function convertMapValue(
  valueField: protobuf.Field,
  token: TextprotoValue,
): unknown {
  if (
    valueField.resolvedType instanceof protobuf.Type &&
    token.kind === 'message'
  ) {
    return textprotoToObject(valueField.resolvedType, token);
  }
  if (valueField.resolvedType instanceof protobuf.Enum) {
    return convertEnum(valueField.resolvedType, token);
  }
  return convertScalarByProtoType(valueField.type, token);
}

function convertSingular(
  field: protobuf.Field,
  token: TextprotoValue,
): unknown {
  field.resolve();
  if (field.resolvedType instanceof protobuf.Type) {
    if (token.kind !== 'message') {
      throw new Error(`field '${field.name}' expects a message body`);
    }
    return textprotoToObject(field.resolvedType, token);
  }
  if (field.resolvedType instanceof protobuf.Enum) {
    return convertEnum(field.resolvedType, token);
  }
  return convertScalarByProtoType(field.type, token);
}

function convertEnum(en: protobuf.Enum, token: TextprotoValue): number {
  if (token.kind === 'number') {
    return Math.trunc(token.value);
  }
  if (token.kind === 'enum') {
    const byName = en.values[token.value];
    if (byName === undefined) {
      throw new Error(`enum '${en.fullName}' has no value '${token.value}'`);
    }
    return byName;
  }
  throw new Error(`field expects an enum value, got ${token.kind}`);
}

// Convert a textproto scalar to the protobufjs `fromObject` value for the
// given proto scalar type.  64-bit ints go through as decimal strings (the
// form protobufjs accepts for Long fields); bytes go through as a Uint8Array.
function convertScalarByProtoType(
  protoType: string,
  token: TextprotoValue,
): unknown {
  switch (protoType) {
    case 'int32':
    case 'sint32':
    case 'sfixed32':
    case 'uint32':
    case 'fixed32':
      return numberOf(token);
    case 'int64':
    case 'sint64':
    case 'sfixed64':
    case 'uint64':
    case 'fixed64':
      return integerStringOf(token);
    case 'double':
    case 'float':
      return numberOf(token);
    case 'bool':
      return token.kind === 'bool' ? token.value : false;
    case 'string':
      return token.kind === 'string' ? token.value : '';
    case 'bytes':
      return token.kind === 'string' ? token.bytes : new Uint8Array(0);
    default:
      throw new Error(`unsupported proto scalar type '${protoType}'`);
  }
}

function numberOf(token: TextprotoValue): number {
  if (token.kind === 'number') {
    return token.value;
  }
  throw new Error(`expected a numeric scalar, got ${token.kind}`);
}

// A 64-bit integer textproto scalar as a decimal string (protobufjs accepts a
// decimal string for Long-typed fields). The textproto `raw` carries the
// exact integer text so large magnitudes survive without a float round-trip.
function integerStringOf(token: TextprotoValue): string {
  if (token.kind !== 'number') {
    throw new Error(`expected an integer scalar, got ${token.kind}`);
  }
  return BigInt(normalizeIntRaw(token.raw)).toString();
}

// Strip a `u` suffix the corpus uses for uint literals in some contexts and
// fall back to the parsed value if the raw is not a clean integer literal.
function normalizeIntRaw(raw: string): string | number {
  const trimmed =
    raw.endsWith('u') || raw.endsWith('U') ? raw.slice(0, -1) : raw;
  if (/^[+-]?\d+$/.test(trimmed)) {
    return trimmed;
  }
  return Math.trunc(Number(trimmed));
}
