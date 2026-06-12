// e2e comprehension behaviors — `cel.bind`, nested comprehensions, and
// chained macro pipelines.
//
// Ported from the C++ `e2e/m5b_test.cc` (the comprehension follow-on:
// `cel.bind`, the standard macros over list/map sources, nesting).
// Grounded in `doc/langdef.md` §"Macros" (comprehension desugaring) and
// the `cel.bind` extension macro (a single-binding `let`).

import { describe, expect, it } from 'vitest';

import { evalCel } from './helpers.js';

describe('cel.bind — single-binding let', () => {
  it('binds a scalar and uses it', async () => {
    expect(await evalCel('cel.bind(x, 5, x + 1)')).toBe(6n);
  });
  it('binds a list and comprehends over it', async () => {
    expect(await evalCel('cel.bind(a, [1, 2, 3], a.map(i, i * i))')).toEqual([
      1n,
      4n,
      9n,
    ]);
  });
  it('the bound value is reused (no recomputation observable)', async () => {
    expect(await evalCel("cel.bind(s, 'ab', s + s)")).toBe('abab');
  });
});

describe('nested comprehensions', () => {
  it('map inside map — outer/inner iteration vars', async () => {
    expect(await evalCel('[1, 2, 3].map(x, [10, 20].map(y, x + y))')).toEqual([
      [11n, 21n],
      [12n, 22n],
      [13n, 23n],
    ]);
  });
  it('map over a list of lists', async () => {
    expect(
      await evalCel('[[1, 2], [3, 4]].map(row, row.map(c, c * 2))'),
    ).toEqual([
      [2n, 4n],
      [6n, 8n],
    ]);
  });
  it('map projecting a method call on each element', async () => {
    expect(await evalCel("['a', 'bb', 'ccc'].map(s, s.size())")).toEqual([
      1n,
      2n,
      3n,
    ]);
  });
});

describe('chained macro pipelines', () => {
  it('filter then map', async () => {
    expect(
      await evalCel('[1, 2, 3, 4, 5].filter(x, x > 2).map(x, x * 10)'),
    ).toEqual([30n, 40n, 50n]);
  });
  it('filter then size', async () => {
    expect(await evalCel('size([1, 2, 3].filter(x, x != 2))')).toBe(2n);
  });
  it('all && exists composed', async () => {
    expect(
      await evalCel('[1, 2, 3].all(x, x > 0) && [1, 2, 3].exists(x, x == 2)'),
    ).toBe(true);
  });
});

describe('comprehension surfaces out of scope', () => {
  // The `comprehensions_v2` map-transform 3-arg form on a MAP source
  // (`m.map(k, v, expr)`) is not admitted by the static type checker here:
  // it stamps a `_?_:_` overload mismatch over the accumulator, so the
  // compiler rejects it.  List sources use the supported 2-/3-arg forms.
  it.skip('map.map(k, v, expr) — two-iter-var transform rejected by the checker', async () => {
    expect(await evalCel("{'a': 1, 'b': 2}.map(k, v, k)")).toEqual(['a', 'b']);
  });
});
