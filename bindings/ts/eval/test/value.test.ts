import { describe, it, expect } from 'vitest';
import { CelKind } from '../src/celvalue.js';
import {
  Value,
  ValueError,
  asBool,
  asBytes,
  asDouble,
  asInt,
  asList,
  asMap,
  asMessage,
  asString,
  asUint,
  isError,
  isNull,
  isUnknown,
} from '../src/value.js';
import { HostBackingError } from '../src/host/backing.js';
import {
  ObjectListBacking,
  ObjectMapBacking,
  ObjectMessageBacking,
} from '../src/host/object-backing.js';

describe('Value factories', () => {
  it('build the expected discriminated-union shape', () => {
    expect(Value.null()).toEqual({ kind: CelKind.Null });
    expect(Value.bool(true)).toEqual({ kind: CelKind.Bool, bool: true });
    expect(Value.int(7n)).toEqual({ kind: CelKind.Int, int: 7n });
    expect(Value.uint(7n)).toEqual({ kind: CelKind.Uint, uint: 7n });
    expect(Value.double(1.5)).toEqual({ kind: CelKind.Double, double: 1.5 });
    expect(Value.string('hi')).toEqual({ kind: CelKind.String, value: 'hi' });
    const b = new Uint8Array([1, 2]);
    expect(Value.bytes(b)).toEqual({ kind: CelKind.Bytes, bytes: b });
  });

  it('int/uint carry full 64-bit range (bigint)', () => {
    const max = 2n ** 64n - 1n;
    expect(asUint(Value.uint(max))).toBe(max);
    expect(asInt(Value.int(-(2n ** 63n)))).toBe(-(2n ** 63n));
  });
});

describe('accessors return the payload on a kind match', () => {
  it('each asX returns its value', () => {
    expect(asBool(Value.bool(true))).toBe(true);
    expect(asInt(Value.int(42n))).toBe(42n);
    expect(asUint(Value.uint(42n))).toBe(42n);
    expect(asDouble(Value.double(2.5))).toBe(2.5);
    expect(asString(Value.string('x'))).toBe('x');
    expect([...asBytes(Value.bytes(new Uint8Array([9])))]).toEqual([9]);
  });
});

describe('accessors throw ValueError on a kind mismatch', () => {
  it('every asX rejects a wrong-kind value', () => {
    expect(() => asBool(Value.int(1n))).toThrow(ValueError);
    expect(() => asInt(Value.bool(true))).toThrow(ValueError);
    expect(() => asUint(Value.int(1n))).toThrow(ValueError);
    expect(() => asDouble(Value.int(1n))).toThrow(ValueError);
    expect(() => asString(Value.int(1n))).toThrow(ValueError);
    expect(() => asBytes(Value.string('x'))).toThrow(ValueError);
  });

  it('the message names the actual kind', () => {
    expect(() => asInt(Value.bool(true))).toThrow(
      `expected int, got kind ${CelKind.Bool}`,
    );
  });
});

describe('predicates', () => {
  it('isNull is true only for null', () => {
    expect(isNull(Value.null())).toBe(true);
    expect(isNull(Value.int(0n))).toBe(false);
  });

  it('isError is true only for error', () => {
    expect(isError({ kind: CelKind.Error, errorCode: 13 })).toBe(true);
    expect(isError(Value.null())).toBe(false);
  });

  it('isUnknown is true only for unknown', () => {
    expect(isUnknown(Value.unknown())).toBe(true);
    expect(isUnknown(Value.null())).toBe(false);
  });
});

describe('aggregate factories build host-backed arms', () => {
  it('Value.message wraps a MessageBacking', () => {
    const backing = new ObjectMessageBacking({ name: 'Ann' });
    expect(Value.message(backing)).toEqual({
      kind: CelKind.Message,
      backing,
    });
  });

  it('Value.object wraps a plain object as a message', () => {
    const v = Value.object({ name: 'Ann' });
    expect(v.kind).toBe(CelKind.Message);
    expect(asMessage(v).getField('name')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'Ann' },
    });
  });

  it('Value.list / Value.map carry a backing of the right size', () => {
    const list = Value.list([Value.int(1n), Value.int(2n)]);
    expect(list.kind).toBe(CelKind.ListHost);
    const map = Value.map([[Value.string('k'), Value.int(7n)]]);
    expect(map.kind).toBe(CelKind.MapHost);
  });

  it('Value.listOf / Value.mapOf wrap an existing backing', () => {
    expect(Value.listOf(new ObjectListBacking([1n, 2n])).kind).toBe(
      CelKind.ListHost,
    );
    expect(Value.mapOf(new ObjectMapBacking(new Map([['k', 7n]]))).kind).toBe(
      CelKind.MapHost,
    );
  });
});

describe('aggregate accessors materialize', () => {
  it('asList walks the backing into a Value[]', () => {
    const list = Value.list([Value.int(1n), Value.string('x')]);
    expect(asList(list)).toEqual([
      { kind: CelKind.Int, int: 1n },
      { kind: CelKind.String, value: 'x' },
    ]);
  });

  it('asMap walks the backing into [key, value] pairs', () => {
    const map = Value.map([
      [Value.string('k'), Value.int(7n)],
      [Value.int(2n), Value.bool(true)],
    ]);
    const pairs = asMap(map);
    expect(pairs).toContainEqual([
      { kind: CelKind.String, value: 'k' },
      { kind: CelKind.Int, int: 7n },
    ]);
    expect(pairs).toContainEqual([
      { kind: CelKind.Int, int: 2n },
      { kind: CelKind.Bool, bool: true },
    ]);
  });

  it('asMessage returns the read interface', () => {
    const v = Value.object({ active: true });
    expect(asMessage(v).hasField('active')).toBe(true);
  });

  it('aggregate accessors reject a scalar', () => {
    expect(() => asMessage(Value.int(1n))).toThrow(ValueError);
    expect(() => asList(Value.int(1n))).toThrow(ValueError);
    expect(() => asMap(Value.int(1n))).toThrow(ValueError);
  });

  it('nested aggregates recurse (list of messages, map to lists)', () => {
    const list = Value.list([Value.object({ n: 1n }), Value.object({ n: 2n })]);
    const items = asList(list);
    expect(items).toHaveLength(2);
    expect(asMessage(items[0]!).getField('n')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 1n },
    });
  });
});

describe('Value.map key validation', () => {
  it('accepts scalar keys (string / int / uint / bool)', () => {
    expect(() =>
      Value.map([
        [Value.string('s'), Value.null()],
        [Value.int(1n), Value.null()],
        [Value.uint(2n), Value.null()],
        [Value.bool(true), Value.null()],
      ]),
    ).not.toThrow();
  });

  it('int and uint keys collide (cross-type numeric equality)', () => {
    const map = Value.map([
      [Value.int(1n), Value.string('a')],
      [Value.uint(1n), Value.string('b')],
    ]);
    // Both keys reduce to the same `i:1` tag → last write wins, size 1.
    expect(asMap(map)).toHaveLength(1);
  });

  it('rejects a non-scalar key (message / list / null / double)', () => {
    expect(() => Value.map([[Value.object({}), Value.null()]])).toThrow(
      HostBackingError,
    );
    expect(() => Value.map([[Value.null(), Value.null()]])).toThrow(
      HostBackingError,
    );
    expect(() => Value.map([[Value.double(1.5), Value.null()]])).toThrow(
      HostBackingError,
    );
  });
});
