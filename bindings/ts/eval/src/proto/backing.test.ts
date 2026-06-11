import * as protobuf from 'protobufjs';
import { describe, it, expect } from 'vitest';

import type { CelDuration, CelTimestamp, CelValue } from '../types.js';

import {
  ProtoMessageBacking,
  coerceObjectToMessage,
  messageToObject,
  peelDuration,
  peelTimestamp,
  peelWrapper,
  isWellKnownWrappable,
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
            StringValue: { fields: { value: { type: 'string', id: 1 } } },
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
