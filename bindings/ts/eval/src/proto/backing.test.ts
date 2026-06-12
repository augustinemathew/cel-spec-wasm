import * as protobuf from 'protobufjs';
import { describe, it, expect } from 'vitest';

import type { CelDuration, CelTimestamp, CelValue } from '../types.js';

import {
  ProtoFieldRangeError,
  ProtoMessageBacking,
  coerceObjectToMessage,
  messageToObject,
  peelDuration,
  peelTimestamp,
  peelWrapper,
  isWellKnownWrappable,
  isWellKnownConstructable,
  wrapWellKnownValue,
} from './backing.js';

// A namespace with scalars, a nested message, repeated/map fields, an
// enum, and WKT-shaped (Timestamp/Duration/wrapper) types.
const ROOT_JSON = {
  nested: {
    test: {
      nested: {
        Color: { values: { RED: 0, GREEN: 1 } },
        User: {
          fields: {
            id: { type: 'int64', id: 1 },
            name: { type: 'string', id: 2 },
          },
        },
        Msg: {
          fields: {
            i64: { type: 'int64', id: 1 },
            u64: { type: 'uint64', id: 2 },
            i32: { type: 'int32', id: 3 },
            b: { type: 'bool', id: 4 },
            s: { type: 'string', id: 5 },
            by: { type: 'bytes', id: 6 },
            d: { type: 'double', id: 7 },
            color: { type: 'Color', id: 8 },
            user: { type: 'User', id: 9 },
            tags: { rule: 'repeated', type: 'string', id: 10 },
            counts: { keyType: 'string', type: 'int32', id: 11 },
            ts: { type: 'google.protobuf.Timestamp', id: 12 },
            dur: { type: 'google.protobuf.Duration', id: 13 },
            cnt: { type: 'google.protobuf.Int32Value', id: 14 },
            nm: { type: 'google.protobuf.StringValue', id: 15 },
            i64w: { type: 'google.protobuf.Int64Value', id: 16 },
            u64w: { type: 'google.protobuf.UInt64Value', id: 17 },
            dw: { type: 'google.protobuf.DoubleValue', id: 18 },
            fw: { type: 'google.protobuf.FloatValue', id: 19 },
            bw: { type: 'google.protobuf.BoolValue', id: 20 },
            byw: { type: 'google.protobuf.BytesValue', id: 21 },
            val: { type: 'google.protobuf.Value', id: 22 },
            strct: { type: 'google.protobuf.Struct', id: 23 },
            lst: { type: 'google.protobuf.ListValue', id: 24 },
            fl: { type: 'float', id: 25 },
            u32w: { type: 'google.protobuf.UInt32Value', id: 26 },
            u32: { type: 'uint32', id: 27 },
            users: { rule: 'repeated', type: 'User', id: 28 },
            tss: {
              rule: 'repeated',
              type: 'google.protobuf.Timestamp',
              id: 29,
            },
            mts: { keyType: 'bool', type: 'google.protobuf.Timestamp', id: 30 },
          },
        },
      },
    },
    google: {
      nested: {
        protobuf: {
          nested: {
            Timestamp: {
              fields: {
                seconds: { type: 'int64', id: 1 },
                nanos: { type: 'int32', id: 2 },
              },
            },
            Duration: {
              fields: {
                seconds: { type: 'int64', id: 1 },
                nanos: { type: 'int32', id: 2 },
              },
            },
            Int32Value: { fields: { value: { type: 'int32', id: 1 } } },
            UInt32Value: { fields: { value: { type: 'uint32', id: 1 } } },
            Int64Value: { fields: { value: { type: 'int64', id: 1 } } },
            UInt64Value: { fields: { value: { type: 'uint64', id: 1 } } },
            DoubleValue: { fields: { value: { type: 'double', id: 1 } } },
            FloatValue: { fields: { value: { type: 'float', id: 1 } } },
            BoolValue: { fields: { value: { type: 'bool', id: 1 } } },
            StringValue: { fields: { value: { type: 'string', id: 1 } } },
            BytesValue: { fields: { value: { type: 'bytes', id: 1 } } },
            NullValue: { values: { NULL_VALUE: 0 } },
            Value: {
              oneofs: {
                kind: {
                  oneof: [
                    'null_value',
                    'number_value',
                    'string_value',
                    'bool_value',
                    'struct_value',
                    'list_value',
                  ],
                },
              },
              fields: {
                null_value: { type: 'google.protobuf.NullValue', id: 1 },
                number_value: { type: 'double', id: 2 },
                string_value: { type: 'string', id: 3 },
                bool_value: { type: 'bool', id: 4 },
                struct_value: { type: 'google.protobuf.Struct', id: 5 },
                list_value: { type: 'google.protobuf.ListValue', id: 6 },
              },
            },
            Struct: {
              fields: {
                fields: {
                  keyType: 'string',
                  type: 'google.protobuf.Value',
                  id: 1,
                },
              },
            },
            ListValue: {
              fields: {
                values: {
                  rule: 'repeated',
                  type: 'google.protobuf.Value',
                  id: 1,
                },
              },
            },
          },
        },
      },
    },
  },
};

function root(): protobuf.Root {
  const r = protobuf.Root.fromJSON(ROOT_JSON);
  r.resolveAll();
  return r;
}

function msgType(): protobuf.Type {
  return root().lookupType('test.Msg');
}

function backingOf(obj: Record<string, unknown>): ProtoMessageBacking {
  const type = msgType();
  return new ProtoMessageBacking(type, type.fromObject(obj));
}

describe('ProtoMessageBacking.typeName', () => {
  it('returns the FQN without a leading dot', () => {
    expect(backingOf({}).typeName).toBe('test.Msg');
  });
});

describe('ProtoMessageBacking.readField — scalars', () => {
  it('reads a set int64 as a bigint (by name and by number)', () => {
    const b = backingOf({ i64: 42 });
    expect(b.readField('i64')).toBe(42n);
    expect(b.readField(1)).toBe(42n);
  });

  it('reads a set uint64 as a bigint', () => {
    expect(backingOf({ u64: '18446744073709551615' }).readField('u64')).toBe(
      18446744073709551615n,
    );
  });

  it('reads INT64_MIN / INT64_MAX boundaries', () => {
    expect(backingOf({ i64: '9223372036854775807' }).readField(1)).toBe(
      9223372036854775807n,
    );
    expect(backingOf({ i64: '-9223372036854775808' }).readField(1)).toBe(
      -9223372036854775808n,
    );
  });

  it('reads a bool, string, double, and bytes', () => {
    const b = backingOf({
      b: true,
      s: 'hello',
      d: 3.5,
      by: new Uint8Array([1, 2, 3]),
    });
    expect(b.readField('b')).toBe(true);
    expect(b.readField('s')).toBe('hello');
    expect(b.readField('d')).toBe(3.5);
    expect(b.readField('by')).toEqual(new Uint8Array([1, 2, 3]));
  });

  it('reads an enum as a CEL int (bigint)', () => {
    expect(backingOf({ color: 1 }).readField('color')).toBe(1n);
  });
});

describe('ProtoMessageBacking.readField — proto3 unset defaults', () => {
  it('reads an unset int64 as 0n', () => {
    expect(backingOf({}).readField('i64')).toBe(0n);
  });

  it('reads an unset string as the empty string', () => {
    expect(backingOf({}).readField('s')).toBe('');
  });

  it('reads an unset bool as false', () => {
    expect(backingOf({}).readField('b')).toBe(false);
  });

  it('reads an unset bytes as empty', () => {
    expect(backingOf({}).readField('by')).toEqual(new Uint8Array(0));
  });

  it('reads an unset message field as null', () => {
    expect(backingOf({}).readField('user')).toBeNull();
  });

  it('reads an unset repeated as the empty list', () => {
    expect(backingOf({}).readField('tags')).toEqual([]);
  });

  it('reads an unset map as the empty Map', () => {
    expect(backingOf({}).readField('counts')).toEqual(new Map());
  });
});

describe('ProtoMessageBacking.readField — aggregates + nested', () => {
  it('reads a set message field as a decoded object', () => {
    const b = backingOf({ user: { id: 7, name: 'amy' } });
    expect(b.readField('user')).toEqual({ id: 7n, name: 'amy' });
  });

  it('reads a repeated string as a list', () => {
    expect(backingOf({ tags: ['a', 'b'] }).readField('tags')).toEqual([
      'a',
      'b',
    ]);
  });

  it('reads a map<string,int32> as a Map of decoded values', () => {
    const m = backingOf({ counts: { x: 1, y: 2 } }).readField('counts');
    expect(m).toEqual(
      new Map<CelValue, CelValue>([
        ['x', 1n],
        ['y', 2n],
      ]),
    );
  });
});

describe('ProtoMessageBacking.readField — WKT peel', () => {
  it('peels a Timestamp field to a tagged record', () => {
    const b = backingOf({ ts: { seconds: 5, nanos: 250 } });
    expect(b.readField('ts')).toEqual({
      kind: 'timestamp',
      epochSeconds: 5n,
      nanos: 250,
    } satisfies CelTimestamp);
  });

  it('peels a Duration field to a tagged record', () => {
    const b = backingOf({ dur: { seconds: 3, nanos: 1 } });
    expect(b.readField('dur')).toEqual({
      kind: 'duration',
      seconds: 3n,
      nanos: 1,
    } satisfies CelDuration);
  });

  it('peels an Int32Value wrapper to a bigint', () => {
    expect(backingOf({ cnt: { value: 99 } }).readField('cnt')).toBe(99n);
  });

  it('peels a StringValue wrapper to a string', () => {
    expect(backingOf({ nm: { value: 'wrapped' } }).readField('nm')).toBe(
      'wrapped',
    );
  });
});

describe('ProtoMessageBacking.hasField', () => {
  it('reports a set scalar as present and an unset one as absent', () => {
    const b = backingOf({ s: 'x' });
    expect(b.hasField('s')).toBe(true);
    expect(b.hasField('i64')).toBe(false);
  });

  it('reports a set message field present, unset absent', () => {
    expect(backingOf({ user: { id: 1 } }).hasField('user')).toBe(true);
    expect(backingOf({}).hasField('user')).toBe(false);
  });

  it('reports presence by field number too', () => {
    expect(backingOf({ s: 'x' }).hasField(5)).toBe(true);
  });
});

describe('ProtoMessageBacking.setField', () => {
  it('sets a scalar and reads it back', () => {
    const b = backingOf({});
    b.setField('s', 'set-me');
    expect(b.readField('s')).toBe('set-me');
    expect(b.hasField('s')).toBe(true);
  });

  it('sets an int64 from a bigint and reads it back', () => {
    const b = backingOf({});
    b.setField('i64', 123n);
    expect(b.readField('i64')).toBe(123n);
  });

  it('sets a nested message from a plain object and reads it back', () => {
    const b = backingOf({});
    b.setField('user', { id: 9n, name: 'z' });
    expect(b.readField('user')).toEqual({ id: 9n, name: 'z' });
  });

  it('sets a field by wire number', () => {
    const b = backingOf({});
    b.setField(5, 'by-number');
    expect(b.readField('s')).toBe('by-number');
  });
});

// WKT-typed field construction from a scalar / map / list.  cel-cpp wraps the
// CEL value into the target WKT message (doc/langdef.md §"JSON Data
// Conversion"); the binding's `setField` must do the same so the constructed
// message round-trips.  Expected values are pinned to the cel-cpp conformance
// corpus (spec/tests/simple/testdata/{proto3,dynamic}.textproto) cited per case.
describe('ProtoMessageBacking.setField — WKT wrapper construction', () => {
  // proto3.textproto "int64_wrapper": `{single_int64_wrapper: -321}` →
  // `Int64Value{value: -321}`; reads back peeled to the bigint.
  it('wraps a bigint into Int64Value (reads back as bigint)', () => {
    const b = backingOf({});
    b.setField('i64w', -321n);
    expect(b.readField('i64w')).toBe(-321n);
    expect(b.hasField('i64w')).toBe(true);
  });

  // dynamic.textproto "uint64_wrapper": `{single_uint64_wrapper: 432u}`.
  it('wraps a bigint into UInt64Value', () => {
    const b = backingOf({});
    b.setField('u64w', 432n);
    expect(b.readField('u64w')).toBe(432n);
  });

  // proto3.textproto "int32_wrapper": `{single_int32_wrapper: -456}`.
  it('wraps a bigint into Int32Value', () => {
    const b = backingOf({});
    b.setField('cnt', -456n);
    expect(b.readField('cnt')).toBe(-456n);
  });

  // proto3.textproto "double_wrapper": `{single_double_wrapper: 2.71828}`.
  it('wraps a number into DoubleValue', () => {
    const b = backingOf({});
    b.setField('dw', 2.71828);
    expect(b.readField('dw')).toBe(2.71828);
  });

  // dynamic.textproto "float_wrapper": `{single_float_wrapper: 86.75}` (86.75 is
  // float-exact, so it round-trips without narrowing loss).
  it('wraps a number into FloatValue', () => {
    const b = backingOf({});
    b.setField('fw', 86.75);
    expect(b.readField('fw')).toBe(86.75);
  });

  // dynamic.textproto "bool_wrapper": `{single_bool_wrapper: true}`.
  it('wraps a bool into BoolValue', () => {
    const b = backingOf({});
    b.setField('bw', true);
    expect(b.readField('bw')).toBe(true);
  });

  // dynamic.textproto "string_wrapper": `{single_string_wrapper: 'baz'}`.
  it('wraps a string into StringValue', () => {
    const b = backingOf({});
    b.setField('nm', 'baz');
    expect(b.readField('nm')).toBe('baz');
  });

  // dynamic.textproto "bytes_wrapper": `{single_bytes_wrapper: b'baz'}`.
  it('wraps bytes into BytesValue', () => {
    const b = backingOf({});
    b.setField('byw', new Uint8Array([0x62, 0x61, 0x7a]));
    expect(b.readField('byw')).toEqual(new Uint8Array([0x62, 0x61, 0x7a]));
  });
});

// The dynamic JSON-value WKTs (`Value` / `Struct` / `ListValue`).  cel-cpp maps
// a CEL scalar/map/list through the JSON conversion of doc/langdef.md.  The
// constructed sub-message's *wire bytes* are the structural identity the corpus
// `object_value` matcher reduces to (conformance compares decoded trees built
// the same way on both sides); `expectFieldEncodes` pins them against the
// expected protobuf-JSON shape built from `fromObject`.
describe('ProtoMessageBacking.setField — dynamic Value/Struct/ListValue', () => {
  // The sub-message stored at `field` after a `setField`, encoded to wire bytes.
  function encodedSub(b: ProtoMessageBacking, field: string): Uint8Array {
    const f = msgType().fields[field];
    if (f === undefined) {
      throw new Error(`no field '${field}'`);
    }
    f.resolve();
    const sub = (b.raw as unknown as Record<string, unknown>)[field];
    const subType = f.resolvedType;
    if (
      !(subType instanceof protobuf.Type) ||
      sub === null ||
      sub === undefined
    ) {
      throw new Error(`field '${field}' is not a set sub-message`);
    }
    return subType.encode(sub as protobuf.Message).finish();
  }

  // The wire bytes of `subType` built from the expected protobuf-JSON `obj`.
  function expectedBytes(
    subFqn: string,
    obj: Record<string, unknown>,
  ): Uint8Array {
    const t = root().lookupType(subFqn);
    return t.encode(t.fromObject(obj)).finish();
  }

  function expectFieldEncodes(
    b: ProtoMessageBacking,
    field: string,
    subFqn: string,
    obj: Record<string, unknown>,
  ): void {
    expect(Array.from(encodedSub(b, field))).toEqual(
      Array.from(expectedBytes(subFqn, obj)),
    );
  }

  // proto3.textproto "value": `{single_value: 'foo'}` → `Value{string_value:'foo'}`.
  it('wraps a string into Value.string_value', () => {
    const b = backingOf({});
    b.setField('val', 'foo');
    expectFieldEncodes(b, 'val', 'google.protobuf.Value', {
      string_value: 'foo',
    });
  });

  // langdef §"JSON Data Conversion": int → JSON Number (double).  proto3
  // "struct" embeds `1` as `number_value: 1.0`, so a bigint into Value is a
  // double `number_value`.
  it('wraps a bigint into Value.number_value (as a double)', () => {
    const b = backingOf({});
    b.setField('val', 5n);
    expectFieldEncodes(b, 'val', 'google.protobuf.Value', { number_value: 5 });
  });

  it('wraps a number into Value.number_value', () => {
    const b = backingOf({});
    b.setField('val', 2.5);
    expectFieldEncodes(b, 'val', 'google.protobuf.Value', {
      number_value: 2.5,
    });
  });

  // dynamic.textproto: a bool into single_value → `bool_value`.
  it('wraps a bool into Value.bool_value', () => {
    const b = backingOf({});
    b.setField('val', true);
    expectFieldEncodes(b, 'val', 'google.protobuf.Value', { bool_value: true });
  });

  // dynamic.textproto "single_value: null" → `null_value: NULL_VALUE`.
  it('wraps null into Value.null_value', () => {
    const b = backingOf({});
    b.setField('val', null);
    expectFieldEncodes(b, 'val', 'google.protobuf.Value', { null_value: 0 });
  });

  // proto3.textproto "struct": `{single_struct: {'one': 1, 'two': 2}}` →
  // `Struct{fields{'one': number_value:1}{'two': number_value:2}}`.
  it('wraps a Map into a Struct via Value (a single_value map)', () => {
    const b = backingOf({});
    b.setField(
      'val',
      new Map<CelValue, CelValue>([
        ['one', 1n],
        ['two', 2n],
      ]),
    );
    expectFieldEncodes(b, 'val', 'google.protobuf.Value', {
      struct_value: {
        fields: { one: { number_value: 1 }, two: { number_value: 2 } },
      },
    });
  });

  // A list into single_value → `list_value`.
  it('wraps an Array into a ListValue via Value', () => {
    const b = backingOf({});
    b.setField('val', [3.0, 'foo', null]);
    expectFieldEncodes(b, 'val', 'google.protobuf.Value', {
      list_value: {
        values: [
          { number_value: 3 },
          { string_value: 'foo' },
          { null_value: 0 },
        ],
      },
    });
  });

  // dynamic.textproto "single_struct": `{single_struct: {'un': 1.0, 'deux': 2.0}}`.
  it('wraps a Map directly into a Struct field', () => {
    const b = backingOf({});
    b.setField(
      'strct',
      new Map<CelValue, CelValue>([
        ['un', 1.0],
        ['deux', 2.0],
      ]),
    );
    expectFieldEncodes(b, 'strct', 'google.protobuf.Struct', {
      fields: { un: { number_value: 1 }, deux: { number_value: 2 } },
    });
  });

  // dynamic.textproto: `{single_struct: {}}` → an empty Struct.
  it('wraps an empty Map into an empty Struct', () => {
    const b = backingOf({});
    b.setField('strct', new Map<CelValue, CelValue>());
    expectFieldEncodes(b, 'strct', 'google.protobuf.Struct', { fields: {} });
  });

  // dynamic.textproto: `google.protobuf.ListValue{values: [3.0, 'foo', null]}`
  // and `{list_value: [1.0, 'one']}`.
  it('wraps an Array directly into a ListValue field', () => {
    const b = backingOf({});
    b.setField('lst', [1.0, 'one']);
    expectFieldEncodes(b, 'lst', 'google.protobuf.ListValue', {
      values: [{ number_value: 1 }, { string_value: 'one' }],
    });
  });

  // dynamic.textproto: `{list_value: []}` → an empty ListValue.
  it('wraps an empty Array into an empty ListValue', () => {
    const b = backingOf({});
    b.setField('lst', []);
    expectFieldEncodes(b, 'lst', 'google.protobuf.ListValue', { values: [] });
  });
});

describe('WKT construct helpers (standalone)', () => {
  it('isWellKnownConstructable covers wrappers + Value/Struct/ListValue', () => {
    expect(isWellKnownConstructable('google.protobuf.Int64Value')).toBe(true);
    expect(isWellKnownConstructable('.google.protobuf.BytesValue')).toBe(true);
    expect(isWellKnownConstructable('google.protobuf.Value')).toBe(true);
    expect(isWellKnownConstructable('google.protobuf.Struct')).toBe(true);
    expect(isWellKnownConstructable('google.protobuf.ListValue')).toBe(true);
  });

  it('isWellKnownConstructable excludes Timestamp/Duration + non-WKT', () => {
    // Timestamp/Duration are constructed from their own seconds/nanos object,
    // not wrapped from a scalar — not constructable in the wrap sense.
    expect(isWellKnownConstructable('google.protobuf.Timestamp')).toBe(false);
    expect(isWellKnownConstructable('google.protobuf.Duration')).toBe(false);
    expect(isWellKnownConstructable('test.Msg')).toBe(false);
  });

  function wktType(fqn: string): protobuf.Type {
    return root().lookupType(fqn);
  }

  // The bytes of a sub-message built from the expected protobuf-JSON shape.
  function wktBytes(fqn: string, obj: Record<string, unknown>): number[] {
    const t = root().lookupType(fqn);
    return Array.from(t.encode(t.fromObject(obj)).finish());
  }

  it('wrapWellKnownValue: wrapper → {value: <scalar>} (bigint stringified)', () => {
    const i64 = wktType('google.protobuf.Int64Value');
    expect(
      Array.from(i64.encode(wrapWellKnownValue(i64, -321n)).finish()),
    ).toEqual(wktBytes('google.protobuf.Int64Value', { value: '-321' }));
    const str = wktType('google.protobuf.StringValue');
    expect(
      Array.from(str.encode(wrapWellKnownValue(str, 'x')).finish()),
    ).toEqual(wktBytes('google.protobuf.StringValue', { value: 'x' }));
  });

  it('wrapWellKnownValue: Value oneof selection by CEL kind', () => {
    const v = wktType('google.protobuf.Value');
    const enc = (cv: CelValue): number[] =>
      Array.from(v.encode(wrapWellKnownValue(v, cv)).finish());
    expect(enc(null)).toEqual(
      wktBytes('google.protobuf.Value', { null_value: 0 }),
    );
    expect(enc(true)).toEqual(
      wktBytes('google.protobuf.Value', { bool_value: true }),
    );
    // int/uint → number_value (a double), per langdef.
    expect(enc(7n)).toEqual(
      wktBytes('google.protobuf.Value', { number_value: 7 }),
    );
    expect(enc('hi')).toEqual(
      wktBytes('google.protobuf.Value', { string_value: 'hi' }),
    );
  });
});

describe('ProtoMessageBacking — unknown field rejects', () => {
  it('throws reading an unknown field number', () => {
    expect(() => backingOf({}).readField(999)).toThrow(/unknown field number/);
  });

  it('throws reading an unknown field name', () => {
    expect(() => backingOf({}).readField('nope')).toThrow(/unknown field/);
  });

  it('throws setting an unknown field', () => {
    expect(() => {
      backingOf({}).setField('nope', 1n);
    }).toThrow(/unknown field/);
  });
});

describe('coerceObjectToMessage / messageToObject round-trip', () => {
  it('coerces a nested plain object to a message and back', () => {
    const type = msgType();
    const msg = coerceObjectToMessage(type, { user: { id: 7 }, s: 'US' });
    const obj = messageToObject(msg);
    expect(obj.s).toBe('US');
    expect(obj.user).toEqual({ id: 7n, name: '' });
  });

  it('messageToObject decodes every field of a Msg', () => {
    const obj = messageToObject(msgType().fromObject({ i64: 1, s: 'hi' }));
    expect(obj.i64).toBe(1n);
    expect(obj.s).toBe('hi');
    expect(obj.user).toBeNull();
  });
});

describe('WKT peel helpers (standalone)', () => {
  it('isWellKnownWrappable recognizes Timestamp/Duration/wrappers', () => {
    expect(isWellKnownWrappable('google.protobuf.Timestamp')).toBe(true);
    expect(isWellKnownWrappable('.google.protobuf.Duration')).toBe(true);
    expect(isWellKnownWrappable('google.protobuf.Int32Value')).toBe(true);
    expect(isWellKnownWrappable('test.Msg')).toBe(false);
  });

  it('peelTimestamp throws on a non-Timestamp message', () => {
    const m = msgType().fromObject({});
    expect(() => peelTimestamp(m)).toThrow(/not a Timestamp/);
  });

  it('peelDuration throws on a non-Duration message', () => {
    const m = msgType().fromObject({});
    expect(() => peelDuration(m)).toThrow(/not a Duration/);
  });

  it('peelWrapper throws on a non-wrapper message', () => {
    const m = msgType().fromObject({});
    expect(() => peelWrapper(m)).toThrow(/not a wrapper/);
  });
});

// ───────────────────────────────────────────────────────────────────
// Field presence — langdef §"Field Selection" `has()` semantics, mirroring
// cel-cpp `ProtoBacking::HasField` (`eval/internal/cel_host.cc`): repeated /
// map present iff non-empty; proto3 implicit-presence scalars present iff !=
// default; proto2 (explicit presence) scalars present iff assigned.  Pinned
// to the conformance corpus's proto2/proto3 `has` sections.
// ───────────────────────────────────────────────────────────────────
describe('ProtoMessageBacking.hasField — presence semantics', () => {
  // proto3.textproto has/repeated_none_implicit + repeated_none_explicit.
  it('reports a repeated field present iff non-empty', () => {
    const b = backingOf({});
    expect(b.hasField('tags')).toBe(false);
    b.setField('tags', []);
    expect(b.hasField('tags')).toBe(false);
    b.setField('tags', ['a']);
    expect(b.hasField('tags')).toBe(true);
  });

  // proto3.textproto has/map_none_implicit + map_none_explicit.
  it('reports a map field present iff non-empty', () => {
    const b = backingOf({});
    expect(b.hasField('counts')).toBe(false);
    b.setField('counts', new Map<CelValue, CelValue>());
    expect(b.hasField('counts')).toBe(false);
    b.setField('counts', new Map<CelValue, CelValue>([['x', 1n]]));
    expect(b.hasField('counts')).toBe(true);
  });

  // proto3.textproto has/single_set_to_default: a proto3 implicit-presence
  // scalar explicitly set to its default reads as absent.
  it('reports a proto3 scalar set to its default as absent', () => {
    const b = backingOf({});
    b.setField('i64', 0n);
    expect(b.hasField('i64')).toBe(false);
    b.setField('s', '');
    expect(b.hasField('s')).toBe(false);
    b.setField('b', false);
    expect(b.hasField('b')).toBe(false);
    b.setField('by', new Uint8Array(0));
    expect(b.hasField('by')).toBe(false);
  });

  // proto3.textproto has/single_enum_set_zero: enum set to its 0 value.
  it('reports a proto3 enum set to zero as absent', () => {
    const b = backingOf({});
    b.setField('color', 0n);
    expect(b.hasField('color')).toBe(false);
    b.setField('color', 1n);
    expect(b.hasField('color')).toBe(true);
  });

  // proto2.textproto has/required + optional sections: explicit presence —
  // a proto2 scalar assigned its default value is still PRESENT.
  it('reports a proto2 scalar set to its default as present', () => {
    const { root: r2 } = protobuf.parse(`
      syntax = "proto2";
      package t2;
      message M {
        optional int32 a = 1;
        repeated int32 r = 2;
      }
    `);
    r2.resolveAll();
    const t2 = r2.lookupType('t2.M');
    const b = new ProtoMessageBacking(t2, t2.create());
    expect(b.hasField('a')).toBe(false);
    b.setField('a', 0n);
    expect(b.hasField('a')).toBe(true);
    // proto2 repeated: still non-empty semantics (cel-cpp FieldSize > 0).
    expect(b.hasField('r')).toBe(false);
  });
});

// ───────────────────────────────────────────────────────────────────
// Out-of-range narrowing — cel-cpp raises a CEL "range error" when an
// int64/uint64 CEL value outside the 32-bit domain is assigned to an
// int32/uint32/enum field or a 32-bit wrapper (`eval/internal/cel_host.cc`
// CheckInt32Range / CheckUint32Range).  Pinned to enums.textproto
// legacy_*/assign_standalone_int_too_{big,neg} and dynamic.textproto
// int32/uint32 field_assign_*_range.
// ───────────────────────────────────────────────────────────────────
describe('ProtoMessageBacking.setField — range checks', () => {
  it('throws ProtoFieldRangeError on an out-of-int32-range enum assignment', () => {
    const b = backingOf({});
    expect(() => {
      b.setField('color', 5000000000n);
    }).toThrow(ProtoFieldRangeError);
    expect(() => {
      b.setField('color', -7000000000n);
    }).toThrow(/range error/);
  });

  it('throws on an out-of-range Int32Value wrapper assignment', () => {
    const b = backingOf({});
    expect(() => {
      b.setField('cnt', 12345678900n);
    }).toThrow(ProtoFieldRangeError);
    expect(() => {
      b.setField('cnt', -998877665544332211n);
    }).toThrow(ProtoFieldRangeError);
  });

  it('throws on an out-of-range UInt32Value wrapper assignment', () => {
    const b = backingOf({});
    expect(() => {
      b.setField('u32w', 6111222333n);
    }).toThrow(ProtoFieldRangeError);
  });

  it('throws on out-of-range plain int32 / uint32 scalar fields', () => {
    const b = backingOf({});
    expect(() => {
      b.setField('i32', 12345678900n);
    }).toThrow(ProtoFieldRangeError);
    expect(() => {
      b.setField('u32', 6111222333n);
    }).toThrow(ProtoFieldRangeError);
  });

  it('admits the exact 32-bit boundary values', () => {
    const b = backingOf({});
    b.setField('i32', 2147483647n);
    expect(b.readField('i32')).toBe(2147483647n);
    b.setField('i32', -2147483648n);
    expect(b.readField('i32')).toBe(-2147483648n);
    b.setField('u32', 4294967295n);
    expect(b.readField('u32')).toBe(4294967295n);
    b.setField('cnt', -456n);
    expect(b.readField('cnt')).toBe(-456n);
  });
});

// ───────────────────────────────────────────────────────────────────
// Float narrowing — a `float` field carries float32 precision; the decode
// narrows so construction round-trips match protobuf reflection (cel-cpp
// `static_cast<float>`).  Pinned to dynamic.textproto float/literal_not_double
// + field_assign_*_round_to_zero + field_assign_*_range.
// ───────────────────────────────────────────────────────────────────
describe('float field narrowing', () => {
  it('narrows a FloatValue wrapper read to float32 precision', () => {
    const b = backingOf({});
    b.setField('fw', 1.333);
    expect(b.readField('fw')).toBe(Math.fround(1.333));
    expect(b.readField('fw')).not.toBe(1.333);
  });

  it('rounds a subnormal-underflow double to zero (1e-50 → 0)', () => {
    const b = backingOf({});
    b.setField('fw', 1e-50);
    expect(b.readField('fw')).toBe(0);
  });

  it('rounds an over-range double to infinity (1.4e55 → +inf)', () => {
    const b = backingOf({});
    b.setField('fw', 1.4e55);
    expect(b.readField('fw')).toBe(Infinity);
    b.setField('fw', -9.9e100);
    expect(b.readField('fw')).toBe(-Infinity);
  });

  it('narrows a plain float scalar field on read', () => {
    const b = backingOf({});
    b.setField('fl', 3.1416);
    expect(b.readField('fl')).toBe(Math.fround(3.1416));
  });
});

// ───────────────────────────────────────────────────────────────────
// null assignment — assigning `null` to a singular message-typed field
// CLEARS it (equivalent to unset), except `google.protobuf.Value` which
// packs an explicit `null_value` (cel-cpp SetScalarField CPPTYPE_MESSAGE
// CEL_NULL arm).  Pinned to wrappers.textproto */to_null.
// ───────────────────────────────────────────────────────────────────
describe('ProtoMessageBacking.setField — null clears message fields', () => {
  it('clears a wrapper field assigned null (reads back unset)', () => {
    const b = backingOf({});
    b.setField('cnt', null);
    expect(b.hasField('cnt')).toBe(false);
    expect(b.readField('cnt')).toBeNull();
  });

  it('leaves the wire bytes identical to an untouched message', () => {
    const type = msgType();
    const b = new ProtoMessageBacking(type, type.create());
    b.setField('nm', null);
    b.setField('bw', null);
    expect(Array.from(type.encode(b.raw).finish())).toEqual(
      Array.from(type.encode(type.create()).finish()),
    );
  });

  it('clears a plain nested message field assigned null', () => {
    const b = backingOf({ user: { id: 1 } });
    b.setField('user', null);
    expect(b.hasField('user')).toBe(false);
  });
});

// ───────────────────────────────────────────────────────────────────
// Timestamp / Duration packing — a tagged CelTimestamp / CelDuration
// assigned to a Timestamp / Duration field (singular, repeated element, map
// value) packs into the WKT message (cel-cpp MaybeSetWktMessageField).
// Pinned to proto3.textproto literal_wellknown/timestamp + set_null rows.
// ───────────────────────────────────────────────────────────────────
describe('ProtoMessageBacking.setField — timestamp/duration packing', () => {
  const ts = (s: bigint): CelTimestamp => ({
    kind: 'timestamp',
    epochSeconds: s,
    nanos: 0,
  });

  it('packs a CelTimestamp into a singular Timestamp field', () => {
    const b = backingOf({});
    b.setField('ts', {
      kind: 'timestamp',
      epochSeconds: 1234567890n,
      nanos: 0,
    });
    expect(b.readField('ts')).toEqual({
      kind: 'timestamp',
      epochSeconds: 1234567890n,
      nanos: 0,
    });
  });

  it('packs a CelDuration into a singular Duration field', () => {
    const b = backingOf({});
    b.setField('dur', { kind: 'duration', seconds: 7n, nanos: 250 });
    expect(b.readField('dur')).toEqual({
      kind: 'duration',
      seconds: 7n,
      nanos: 250,
    });
  });

  it('packs timestamps through a repeated element', () => {
    const b = backingOf({});
    b.setField('tss', [ts(1n), ts(2n)]);
    expect(b.readField('tss')).toEqual([ts(1n), ts(2n)]);
  });

  it('packs timestamps through a map value', () => {
    const b = backingOf({});
    b.setField('mts', new Map<CelValue, CelValue>([[false, ts(1n)]]));
    expect(b.readField('mts')).toEqual(
      new Map<CelValue, CelValue>([[false, ts(1n)]]),
    );
  });

  // proto3.textproto set_null/repeated_field_timestamp_null_pruned: a null
  // element of a message-typed repeated field is pruned, not appended.
  it('prunes a null element of a repeated message-typed field', () => {
    const b = backingOf({});
    b.setField('tss', [ts(1n), null]);
    expect(b.readField('tss')).toEqual([ts(1n)]);
    b.setField('users', [null, { id: 5n, name: 'x' }]);
    expect(b.readField('users')).toEqual([{ id: 5n, name: 'x' }]);
  });

  // proto3.textproto set_null/map_timestamp_null_pruned: a null value of a
  // message-typed map field prunes the whole entry.
  it('prunes a null-valued entry of a message-typed map field', () => {
    const b = backingOf({});
    b.setField(
      'mts',
      new Map<CelValue, CelValue>([
        [true, null],
        [false, ts(1n)],
      ]),
    );
    expect(b.readField('mts')).toEqual(
      new Map<CelValue, CelValue>([[false, ts(1n)]]),
    );
  });
});
