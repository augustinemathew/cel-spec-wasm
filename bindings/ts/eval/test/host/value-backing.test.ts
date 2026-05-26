/**
 * `value-backing.ts` — the `Value` ⇆ `HostValue` bridge + the `Value[]`-
 * backed list/map. Exercised indirectly by value.test.ts (via
 * `Value.list`/`map`/`asList`/`asMap`); this hits the converters and
 * backing methods directly so every arm/branch is covered, including the
 * 3VL-adjacent UNKNOWN reject and cross-type map keys.
 */
import { describe, it, expect } from 'vitest';
import { CelKind } from '../../src/celvalue.js';
import { HostBackingError } from '../../src/host/backing.js';
import { ObjectMessageBacking } from '../../src/host/object-backing.js';
import {
  ValueListBacking,
  ValueMapBacking,
  hostToValue,
  valueToHost,
} from '../../src/host/value-backing.js';
import { Value } from '../../src/value.js';

describe('valueToHost', () => {
  it('wraps each scalar arm as a scalar HostValue', () => {
    expect(valueToHost(Value.int(7n))).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 7n },
    });
    expect(valueToHost(Value.string('x'))).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'x' },
    });
    expect(valueToHost({ kind: CelKind.Error, errorCode: 13 })).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Error, errorCode: 13 },
    });
  });

  it('maps the host-backed arms to their host kinds', () => {
    const msg = Value.object({ name: 'Ann' });
    expect(valueToHost(msg)).toMatchObject({ host: 'message' });
    expect(valueToHost(Value.list([Value.int(1n)]))).toMatchObject({
      host: 'list',
    });
    expect(
      valueToHost(Value.map([[Value.int(1n), Value.int(2n)]])),
    ).toMatchObject({ host: 'map' });
  });

  it('rejects UNKNOWN (not a host-readable value)', () => {
    expect(() => valueToHost(Value.unknown())).toThrow(HostBackingError);
  });
});

describe('hostToValue', () => {
  it('round-trips every host arm', () => {
    expect(
      hostToValue({ host: 'scalar', value: { kind: CelKind.Int, int: 5n } }),
    ).toEqual({ kind: CelKind.Int, int: 5n });
    const backing = new ObjectMessageBacking({ a: 1n });
    expect(hostToValue({ host: 'message', backing })).toEqual({
      kind: CelKind.Message,
      backing,
    });
    expect(hostToValue(valueToHost(Value.list([Value.int(1n)]))).kind).toBe(
      CelKind.ListHost,
    );
    expect(
      hostToValue(valueToHost(Value.map([[Value.int(1n), Value.int(2n)]])))
        .kind,
    ).toBe(CelKind.MapHost);
  });
});

describe('ValueListBacking', () => {
  it('reports size and reads in-range elements', () => {
    const b = new ValueListBacking([Value.int(1n), Value.string('x')]);
    expect(b.size).toBe(2);
    expect(b.at(0)).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 1n },
    });
  });

  it('returns undefined for out-of-range / negative index', () => {
    const b = new ValueListBacking([Value.int(1n)]);
    expect(b.at(1)).toBeUndefined();
    expect(b.at(-1)).toBeUndefined();
  });

  it('forEach visits every element in order', () => {
    const seen: number[] = [];
    new ValueListBacking([Value.int(1n), Value.int(2n)]).forEach((e) => {
      if (e.host === 'scalar' && e.value.kind === CelKind.Int) {
        seen.push(Number(e.value.int));
      }
    });
    expect(seen).toEqual([1, 2]);
  });
});

describe('ValueMapBacking', () => {
  it('looks up by cross-type numeric key and reports has/size', () => {
    const b = new ValueMapBacking([
      [Value.int(1n), Value.string('a')],
      [Value.string('k'), Value.bool(true)],
    ]);
    expect(b.size).toBe(2);
    // int key 1 matches a uint 1 lookup (cross-type).
    expect(b.get(Value.uint(1n) as never)).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'a' },
    });
    expect(b.has({ kind: CelKind.String, value: 'k' })).toBe(true);
    expect(b.has({ kind: CelKind.String, value: 'absent' })).toBe(false);
    expect(b.get({ kind: CelKind.String, value: 'absent' })).toBeUndefined();
  });

  it('forEach visits key/value pairs', () => {
    const keys: string[] = [];
    new ValueMapBacking([[Value.string('k'), Value.int(9n)]]).forEach((k) => {
      if (k.kind === CelKind.String) {
        keys.push(k.value);
      }
    });
    expect(keys).toEqual(['k']);
  });

  it('rejects a non-scalar key', () => {
    expect(() => new ValueMapBacking([[Value.null(), Value.null()]])).toThrow(
      HostBackingError,
    );
  });
});
