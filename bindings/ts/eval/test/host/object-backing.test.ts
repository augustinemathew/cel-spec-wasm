import { describe, it, expect } from 'vitest';
import { CelKind } from '../../src/celvalue.js';
import { HostBackingError, type HostValue } from '../../src/host/backing.js';
import {
  ObjectListBacking,
  ObjectMapBacking,
  ObjectMessageBacking,
  jsToHost,
} from '../../src/host/object-backing.js';

function scalarKind(v: HostValue): CelKind {
  if (v.host !== 'scalar') throw new Error(`expected scalar, got ${v.host}`);
  return v.value.kind;
}

describe('jsToHost — JS value → HostValue', () => {
  it('maps each scalar JS type', () => {
    expect(jsToHost(null)).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Null },
    });
    expect(jsToHost(undefined)).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Null },
    });
    expect(jsToHost(true)).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Bool, bool: true },
    });
    expect(jsToHost(7n)).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 7n },
    });
    expect(jsToHost(2.5)).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Double, double: 2.5 },
    });
    expect(jsToHost('hi')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'hi' },
    });
    expect(jsToHost(new Uint8Array([1, 2]))).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Bytes, bytes: new Uint8Array([1, 2]) },
    });
  });

  it('maps containers to aggregate backings', () => {
    expect(jsToHost([1n, 2n]).host).toBe('list');
    expect(jsToHost(new Map([['k', 1n]])).host).toBe('map');
    expect(jsToHost({ a: 1n }).host).toBe('message');
  });

  it('throws on an unrepresentable JS value', () => {
    expect(() => jsToHost(() => 0)).toThrow(HostBackingError);
    expect(() => jsToHost(Symbol('x'))).toThrow(/cannot represent JS symbol/);
  });
});

describe('ObjectMessageBacking', () => {
  const m = new ObjectMessageBacking({ name: 'Ann', age: 20n, nick: null });

  it('reads present fields', () => {
    expect(scalarKind(m.getField('name')!)).toBe(CelKind.String);
    expect(scalarKind(m.getField('age')!)).toBe(CelKind.Int);
  });

  it('returns undefined for an absent field', () => {
    expect(m.getField('missing')).toBeUndefined();
  });

  it('hasField: present non-null true; null or absent false', () => {
    expect(m.hasField('name')).toBe(true);
    expect(m.hasField('nick')).toBe(false); // present but null
    expect(m.hasField('missing')).toBe(false);
  });
});

describe('ObjectListBacking', () => {
  const l = new ObjectListBacking([10n, 'x', true]);

  it('size + at within range', () => {
    expect(l.size).toBe(3);
    expect(scalarKind(l.at(0)!)).toBe(CelKind.Int);
    expect(scalarKind(l.at(1)!)).toBe(CelKind.String);
  });

  it('at out of range → undefined (negative and >= size)', () => {
    expect(l.at(-1)).toBeUndefined();
    expect(l.at(3)).toBeUndefined();
  });

  it('forEach visits every element in order', () => {
    const kinds: CelKind[] = [];
    l.forEach((e) => kinds.push(scalarKind(e)));
    expect(kinds).toEqual([CelKind.Int, CelKind.String, CelKind.Bool]);
  });
});

describe('ObjectMapBacking', () => {
  it('string keys: get / has / size / forEach', () => {
    const m = new ObjectMapBacking(
      new Map<unknown, unknown>([
        ['k', 7n],
        ['j', 9n],
      ]),
    );
    expect(m.size).toBe(2);
    expect(scalarKind(m.get({ kind: CelKind.String, value: 'k' })!)).toBe(
      CelKind.Int,
    );
    expect(m.has({ kind: CelKind.String, value: 'j' })).toBe(true);
    expect(m.get({ kind: CelKind.String, value: 'nope' })).toBeUndefined();
    expect(m.has({ kind: CelKind.String, value: 'nope' })).toBe(false);
    const keys: string[] = [];
    m.forEach((key) => {
      if (key.kind === CelKind.String) keys.push(key.value);
    });
    expect(keys.sort()).toEqual(['j', 'k']);
  });

  it('numeric keys match cross-type (number key ↔ int/uint query)', () => {
    const m = new ObjectMapBacking(new Map<unknown, unknown>([[5, 'five']]));
    // queried as int and as uint — both tag `i:5`.
    expect(m.get({ kind: CelKind.Int, int: 5n })?.host).toBe('scalar');
    expect(m.has({ kind: CelKind.Uint, uint: 5n })).toBe(true);
  });

  it('bigint and boolean keys (both true and false)', () => {
    const m = new ObjectMapBacking(
      new Map<unknown, unknown>([
        [9n, 'nine'],
        [true, 'yes'],
        [false, 'no'],
      ]),
    );
    expect(m.has({ kind: CelKind.Int, int: 9n })).toBe(true);
    expect(m.has({ kind: CelKind.Bool, bool: true })).toBe(true);
    expect(m.has({ kind: CelKind.Bool, bool: false })).toBe(true);
  });

  it('rejects invalid JS key types and non-integer number keys', () => {
    expect(() => new ObjectMapBacking(new Map([[{}, 1]]))).toThrow(
      /invalid JS map key type object/,
    );
    expect(() => new ObjectMapBacking(new Map([[1.5, 1]]))).toThrow(
      /non-integer map key/,
    );
  });

  it('rejects an invalid CEL key kind at lookup', () => {
    const m = new ObjectMapBacking(new Map<unknown, unknown>([['k', 1n]]));
    expect(() => m.get({ kind: CelKind.Double, double: 1 })).toThrow(
      /invalid map key kind/,
    );
  });
});
