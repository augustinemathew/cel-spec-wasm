/**
 * Message construction — the write side of the proto host, backing the
 * `cel_make_message` / `cel_set_field` trampolines (cel_host.cc
 * `CelMakeMessageImpl` / `CelSetFieldImpl`). A CEL message literal
 * (`Customer{name: 'Ann'}`) lowers to: `cel_make_message(type_id)` →
 * allocate an empty proto of `cel.abi.types[type_id]`, then one
 * `cel_set_field(field_ref_id, value)` per field.
 *
 * `createMessage` makes a fresh, MUTABLE `ProtoMessageBacking` (protobuf-es
 * `create(desc)`); `setField` writes one field into it by reflection,
 * mapping a decoded `CelValue` back to the proto runtime value (the inverse
 * of `proto-backing.ts`'s read mapping). Scope: scalar / enum / bool /
 * string / bytes fields + `null` (clears the field). Message-valued,
 * repeated, map, and wrapper-autowrap fields aren't built yet — they throw
 * `MessageBuildError`, which the trampoline surfaces (and conformance
 * classifies as an expected skip), never a silent wrong value.
 */
import {
  ScalarType,
  create,
  type DescField,
  type DescMessage,
} from '@bufbuild/protobuf';
import { reflect } from '@bufbuild/protobuf/reflect';
import { CelKind, type CelValue } from '../celvalue.js';
import { ProtoMessageBacking } from './proto-backing.js';

/** Thrown when a field can't be built from a CelValue (unsupported field
 *  kind, or a value whose kind doesn't match the field). */
export class MessageBuildError extends Error {
  public override readonly name = 'MessageBuildError';
}

/** A fresh, mutable proto message of `desc` (all fields default/unset). */
export function createMessage(desc: DescMessage): ProtoMessageBacking {
  return new ProtoMessageBacking(desc, create(desc));
}

/** Set (or, for a `null` value, clear) field `fieldName` on `backing`. */
export function setField(
  backing: ProtoMessageBacking,
  fieldName: string,
  value: CelValue,
): void {
  const desc = backing.descriptor;
  const field = desc.fields.find((f) => f.name === fieldName);
  if (field === undefined) {
    throw new MessageBuildError(`no field '${fieldName}' on ${desc.typeName}`);
  }
  const r = reflect(desc, backing.message);
  if (value.kind === CelKind.Null) {
    r.clear(field);
    return;
  }
  r.set(field, protoValue(field, value));
}

// Map a CelValue to the protobuf-es runtime value for `field` (inverse of
// proto-backing.ts's scalarToHost / elementToHost).
function protoValue(
  field: DescField,
  value: CelValue,
): number | bigint | boolean | string | Uint8Array {
  switch (field.fieldKind) {
    case 'scalar':
      return scalarProtoValue(field.scalar, value);
    case 'enum':
      if (value.kind === CelKind.Int) {
        return Number(value.int);
      }
      throw new MessageBuildError(
        `enum field wants CEL int, got ${value.kind}`,
      );
    case 'message':
    case 'list':
    case 'map':
      throw new MessageBuildError(
        `building ${field.fieldKind} field '${field.name}' is not supported`,
      );
  }
}

function scalarProtoValue(
  type: ScalarType,
  value: CelValue,
): number | bigint | boolean | string | Uint8Array {
  switch (type) {
    case ScalarType.INT32:
    case ScalarType.SINT32:
    case ScalarType.SFIXED32:
      return Number(wantInt(value));
    case ScalarType.INT64:
    case ScalarType.SINT64:
    case ScalarType.SFIXED64:
      return wantInt(value);
    case ScalarType.UINT32:
    case ScalarType.FIXED32:
      return Number(wantUint(value));
    case ScalarType.UINT64:
    case ScalarType.FIXED64:
      return wantUint(value);
    case ScalarType.FLOAT:
    case ScalarType.DOUBLE:
      if (value.kind === CelKind.Double) return value.double;
      throw mismatch('double', value);
    case ScalarType.BOOL:
      if (value.kind === CelKind.Bool) return value.bool;
      throw mismatch('bool', value);
    case ScalarType.STRING:
      if (value.kind === CelKind.String) return value.value;
      throw mismatch('string', value);
    case ScalarType.BYTES:
      if (value.kind === CelKind.Bytes) return value.bytes;
      throw mismatch('bytes', value);
  }
}

function wantInt(value: CelValue): bigint {
  if (value.kind === CelKind.Int) return value.int;
  throw mismatch('int', value);
}
function wantUint(value: CelValue): bigint {
  if (value.kind === CelKind.Uint) return value.uint;
  throw mismatch('uint', value);
}
function mismatch(want: string, value: CelValue): MessageBuildError {
  return new MessageBuildError(
    `field wants CEL ${want}, got kind ${value.kind}`,
  );
}
