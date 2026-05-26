/**
 * protobuf-es host backings — the real-proto path (probe P-6), the TS
 * counterpart to `compiler_v2/api/internal/cel_host.h::ProtoBacking` /
 * `ProtoList` / `ProtoMap`. Reads a `protobuf-es` `Message` through
 * descriptor reflection: a `DescField` (a discriminated union on
 * `fieldKind`) gives the CEL type, `reflect(desc,msg).get(field)` the
 * value. The `cel_host.*` trampolines treat this identically to the
 * object backing — only the field accessor differs.
 *
 * WKT messages (Timestamp/Duration/wrappers) surface here as nested
 * message backings; CEL's WKT-unwrap is a separate runtime concern
 * (handled by the `cel_wkt_unwrap_*` trampolines), out of scope for the
 * field-read backing.
 */
import {
  ScalarType,
  type DescField,
  type DescMessage,
  type Message,
} from '@bufbuild/protobuf';
import {
  reflect,
  type ReflectList,
  type ReflectMap,
  type ReflectMessage,
} from '@bufbuild/protobuf/reflect';
import { CelKind, type CelValue } from '../celvalue.js';
import {
  HostBackingError,
  listValue,
  mapValue,
  messageValue,
  scalarValue,
  type HostValue,
  type ListBacking,
  type MapBacking,
  type MessageBacking,
} from './backing.js';
import { celKeyTag } from './map-key.js';

// A protobuf scalar runtime value (what reflect().get yields for a
// scalar field): number (32-bit ints, float/double), bigint (64-bit
// ints), boolean, string, or Uint8Array.
type ScalarRuntime = number | bigint | boolean | string | Uint8Array;

function scalarToHost(type: ScalarType, raw: ScalarRuntime): HostValue {
  switch (type) {
    case ScalarType.DOUBLE:
    case ScalarType.FLOAT:
      return scalarValue({ kind: CelKind.Double, double: raw as number });
    case ScalarType.INT32:
    case ScalarType.INT64:
    case ScalarType.SINT32:
    case ScalarType.SINT64:
    case ScalarType.SFIXED32:
    case ScalarType.SFIXED64:
      return scalarValue({
        kind: CelKind.Int,
        int: BigInt(raw as number | bigint),
      });
    case ScalarType.UINT32:
    case ScalarType.UINT64:
    case ScalarType.FIXED32:
    case ScalarType.FIXED64:
      return scalarValue({
        kind: CelKind.Uint,
        uint: BigInt(raw as number | bigint),
      });
    case ScalarType.BOOL:
      return scalarValue({ kind: CelKind.Bool, bool: raw as boolean });
    case ScalarType.STRING:
      return scalarValue({ kind: CelKind.String, value: raw as string });
    case ScalarType.BYTES:
      return scalarValue({ kind: CelKind.Bytes, bytes: raw as Uint8Array });
  }
}

// A repeated/map element value (`scalar` | `enum` | `message`).
function elementToHost(
  kind: 'scalar' | 'enum' | 'message',
  scalar: ScalarType | undefined,
  message: DescMessage | undefined,
  raw: unknown,
): HostValue {
  switch (kind) {
    case 'scalar':
      return scalarToHost(scalar as ScalarType, raw as ScalarRuntime);
    case 'enum':
      return scalarValue({ kind: CelKind.Int, int: BigInt(raw as number) });
    case 'message':
      return messageValue(
        new ProtoMessageBacking(
          message as DescMessage,
          (raw as ReflectMessage).message,
        ),
      );
  }
}

// A proto scalar map key (from ReflectMap.keys()) → its CEL key.
function scalarKeyToCel(type: ScalarType, raw: ScalarRuntime): CelValue {
  const v = scalarToHost(type, raw);
  if (v.host !== 'scalar') {
    throw new HostBackingError('map key is not a scalar');
  }
  return v.value;
}

export class ProtoMessageBacking implements MessageBacking {
  public constructor(
    private readonly desc: DescMessage,
    private readonly msg: Message,
  ) {}

  /** The wrapped protobuf-es message — the pass-through `TypeRegistry`
   *  uses this to materialize a returned `CEL_MESSAGE` back to a typed
   *  message without rebuilding it field-by-field. */
  public get message(): Message {
    return this.msg;
  }

  /** The message descriptor (its `$typeName` etc.). */
  public get descriptor(): DescMessage {
    return this.desc;
  }

  private field(name: string): DescField | undefined {
    return this.desc.fields.find((f) => f.name === name);
  }

  public getField(name: string): HostValue | undefined {
    const field = this.field(name);
    if (field === undefined) {
      return undefined;
    }
    const value = reflect(this.desc, this.msg).get(field);
    switch (field.fieldKind) {
      case 'scalar':
        return scalarToHost(field.scalar, value as ScalarRuntime);
      case 'enum':
        return scalarValue({ kind: CelKind.Int, int: BigInt(value as number) });
      case 'message': {
        // CEL auto-unwraps a wrapper-typed field: set → its scalar, unset
        // → null (langdef "Wrapper types"). Non-wrapper messages pass
        // through as a nested backing.
        const unwrapped = this.unwrapWrapper(field, value as ReflectMessage);
        if (unwrapped !== undefined) {
          return unwrapped;
        }
        return messageValue(
          new ProtoMessageBacking(
            field.message,
            (value as ReflectMessage).message,
          ),
        );
      }
      case 'list':
        return listValue(new ProtoListBacking(field, value as ReflectList));
      case 'map':
        return mapValue(new ProtoMapBacking(field, value as ReflectMap));
    }
  }

  // If `field` is a WKT wrapper message (google.protobuf.*Value), return its
  // CEL value: `null` when unset, else the wrapped scalar. Returns
  // `undefined` for a non-wrapper field (caller treats it as a message).
  private unwrapWrapper(
    field: DescField & { fieldKind: 'message' },
    value: ReflectMessage,
  ): HostValue | undefined {
    if (!WRAPPER_FQNS.has(field.message.typeName)) {
      return undefined;
    }
    if (!reflect(this.desc, this.msg).isSet(field)) {
      return scalarValue({ kind: CelKind.Null });
    }
    const inner = field.message.fields.find((f) => f.name === 'value');
    if (inner?.fieldKind !== 'scalar') {
      return undefined; // not a scalar wrapper shape — treat as a message
    }
    return scalarToHost(
      inner.scalar,
      reflect(field.message, value.message).get(inner),
    );
  }

  public hasField(name: string): boolean {
    const field = this.field(name);
    if (field === undefined) {
      return false;
    }
    return reflect(this.desc, this.msg).isSet(field);
  }
}

/** The nine `google.protobuf.*Value` wrapper messages CEL auto-unwraps. */
const WRAPPER_FQNS = new Set<string>([
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

export class ProtoListBacking implements ListBacking {
  // `field` is a repeated DescField (fieldKind === 'list').
  public constructor(
    private readonly field: DescField & { fieldKind: 'list' },
    private readonly list: ReflectList,
  ) {}

  public get size(): number {
    return this.list.size;
  }

  public at(index: number): HostValue | undefined {
    if (index < 0 || index >= this.list.size) {
      return undefined;
    }
    return elementToHost(
      this.field.listKind,
      this.field.scalar,
      this.field.message,
      this.list.get(index),
    );
  }

  public forEach(visit: (element: HostValue) => void): void {
    for (let i = 0; i < this.list.size; i++) {
      visit(
        elementToHost(
          this.field.listKind,
          this.field.scalar,
          this.field.message,
          this.list.get(i),
        ),
      );
    }
  }
}

export class ProtoMapBacking implements MapBacking {
  // tag → [CEL key, value], materialized once.
  private readonly byTag = new Map<string, [CelValue, HostValue]>();

  public constructor(field: DescField & { fieldKind: 'map' }, map: ReflectMap) {
    for (const k of map.keys()) {
      const cel = scalarKeyToCel(field.mapKey, k as ScalarRuntime);
      const value = elementToHost(
        field.mapKind,
        field.scalar,
        field.message,
        map.get(k),
      );
      this.byTag.set(celKeyTag(cel), [cel, value]);
    }
  }

  public get size(): number {
    return this.byTag.size;
  }

  public get(key: CelValue): HostValue | undefined {
    return this.byTag.get(celKeyTag(key))?.[1];
  }

  public has(key: CelValue): boolean {
    return this.byTag.has(celKeyTag(key));
  }

  public forEach(visit: (key: CelValue, value: HostValue) => void): void {
    for (const [celKey, value] of this.byTag.values()) {
      visit(celKey, value);
    }
  }
}
