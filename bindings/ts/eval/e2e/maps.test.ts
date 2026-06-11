// e2e map behaviors — literals, indexing, membership, size, and the
// comprehension macros over a map source (which iterate keys).
//
// Ported from the map coverage in the C++ `e2e/m5_test.cc` /
// `e2e/m5b_test.cc` (the comprehension follow-on, map sources).
// Grounded in `doc/langdef.md`:
//   - §"Maps" — `{...}`, `m[k]`, `k in m`, `size(m)`; missing key is a
//     NO_SUCH_KEY runtime error VALUE.
//   - §"Macros" — over a map, the iteration variable binds each KEY.
//
// A decoded CEL map is a JS `Map`; string keys decode to string, int/uint
// keys to `bigint`, bool keys to boolean.

import { describe, expect, it } from 'vitest';

import { errorCode, evalCel } from './helpers.js';

import { CelErrorCode } from '@cel-wasm/eval';
import type { CelValue } from '@cel-wasm/eval';

function asMap(value: CelValue): Map<CelValue, CelValue> {
  expect(value).toBeInstanceOf(Map);
  return value as Map<CelValue, CelValue>;
}

describe('map literals & key kinds', () => {
  it('string-keyed map decodes to a Map', async () => {
    const m = asMap(await evalCel("{'a': 1, 'b': 2}"));
    expect(m.size).toBe(2);
    expect(m.get('a')).toBe(1n);
    expect(m.get('b')).toBe(2n);
  });
  it('int-keyed map', async () => {
    const m = asMap(await evalCel("{1: 'a', 2: 'b'}"));
    expect(m.get(1n)).toBe('a');
    expect(m.get(2n)).toBe('b');
  });
  it('bool-keyed map', async () => {
    const m = asMap(await evalCel('{true: 1, false: 0}'));
    expect(m.get(true)).toBe(1n);
    expect(m.get(false)).toBe(0n);
  });
  it.skip('empty map — bare {} types to map(dyn, dyn), rejected by the static-subset gate', async () => {
    // `{}` has key/value type `dyn`; the compiler's `RejectDyn` gate (the
    // static subset) refuses an unconstrained empty map.
    expect(asMap(await evalCel('{}')).size).toBe(0);
  });
});

describe('indexing', () => {
  it.each([
    ["{'a': 1, 'b': 2}['a']", 1n],
    ["{'a': 1, 'b': 2}['b']", 2n],
    ["{1: 'x'}[1]", 'x'],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
  it('missing key → NO_SUCH_KEY value', async () => {
    expect(errorCode(await evalCel("{'a': 1}['z']"))).toBe(
      CelErrorCode.NO_SUCH_KEY,
    );
  });
  it('missing int key → NO_SUCH_KEY value', async () => {
    expect(errorCode(await evalCel("{1: 'a'}[9]"))).toBe(
      CelErrorCode.NO_SUCH_KEY,
    );
  });
});

describe('membership & size (key membership)', () => {
  it.each([
    ["'a' in {'a': 1}", true],
    ["'z' in {'a': 1}", false],
    ['1 in {1: 0}', true],
    ['9 in {1: 0}', false],
    ["size({'a': 1, 'b': 2})", 2n],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
  it.skip('size({}) — bare {} types to map(dyn, dyn), rejected by the static-subset gate', async () => {
    expect(await evalCel('size({})')).toBe(0n);
  });
});

describe('macros over a map source — iterate keys (langdef §Macros)', () => {
  it('map(k, ...) projects each key', async () => {
    const out = await evalCel("{'a': 1, 'b': 2}.map(k, k)");
    expect(out).toEqual(['a', 'b']);
  });
  it('map(k, m[k]) projects each value via key lookup', async () => {
    expect(await evalCel("{'x': 10}.map(k, {'x': 10}[k])")).toEqual([10n]);
  });
  it('filter(k, ...) over keys', async () => {
    expect(await evalCel("{'a': 1, 'b': 2}.filter(k, k == 'a')")).toEqual([
      'a',
    ]);
  });
  it('exists over keys', async () => {
    expect(await evalCel("{'a': 1}.exists(k, k == 'a')")).toBe(true);
    expect(await evalCel("{'a': 1}.exists(k, k == 'z')")).toBe(false);
  });
  it('all over keys', async () => {
    expect(await evalCel("{'a': 1, 'b': 2}.all(k, k == 'a' || k == 'b')")).toBe(
      true,
    );
  });
});
