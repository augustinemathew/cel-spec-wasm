// e2e list behaviors — literals, indexing, membership, size, concat,
// and the comprehension macros over a list source.
//
// Ported from the C++ `e2e/m4_test.cc` (list literals + indexing, OOB /
// negative-index error values) and the list-macro coverage in
// `e2e/m5_test.cc` / `e2e/m5b_test.cc`.  Grounded in `doc/langdef.md`:
//   - §"Lists" — `[...]`, `l[i]`, `e in l`, `size(l)`, `l1 + l2`.
//   - §"Macros" — map / filter / exists / all / exists_one.
// Out-of-bounds and negative indices produce an INDEX_OUT_OF_BOUNDS error
// VALUE (langdef §"Errors": indexing past the end is a runtime error).

import { describe, expect, it } from 'vitest';

import { errorCode, evalCel } from './helpers.js';

import { CelErrorCode } from '@cel-wasm/eval';

describe('list literals & element kinds', () => {
  it.skip('empty list — bare [] types to list(dyn), rejected by the static-subset gate', async () => {
    // An unadorned `[]` has element type `dyn` (nothing constrains it), and
    // the compiler's `RejectDyn` gate (the static subset) refuses it.  An
    // empty list IS reachable when context supplies the element type — see
    // `[] + [1]` in the concatenation suite.
    expect(await evalCel('[]')).toEqual([]);
  });
  it('int list round-trips to bigint[]', async () => {
    expect(await evalCel('[1, 2, 3]')).toEqual([1n, 2n, 3n]);
  });
  it('string list', async () => {
    expect(await evalCel("['a', 'b', 'c']")).toEqual(['a', 'b', 'c']);
  });
  it('bool list', async () => {
    expect(await evalCel('[true, false, true]')).toEqual([true, false, true]);
  });
  it('double list', async () => {
    expect(await evalCel('[1.5, 2.5]')).toEqual([1.5, 2.5]);
  });
  it('nested homogeneous list', async () => {
    expect(await evalCel('[[1], [2, 3]]')).toEqual([[1n], [2n, 3n]]);
  });
});

describe('indexing', () => {
  it.each([
    ['[10, 20, 30][0]', 10n],
    ['[10, 20, 30][2]', 30n],
    ["['a', 'b', 'c'][1]", 'b'],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
  it('out-of-bounds index → INDEX_OUT_OF_BOUNDS value', async () => {
    expect(errorCode(await evalCel('[1, 2][5]'))).toBe(
      CelErrorCode.INDEX_OUT_OF_BOUNDS,
    );
  });
  it('negative index → INDEX_OUT_OF_BOUNDS value', async () => {
    expect(errorCode(await evalCel('[1, 2][-1]'))).toBe(
      CelErrorCode.INDEX_OUT_OF_BOUNDS,
    );
  });
});

describe('membership & size', () => {
  it.each([
    ['1 in [1, 2, 3]', true],
    ['9 in [1, 2, 3]', false],
    ["'a' in ['a', 'b']", true],
    ["'z' in ['a', 'b']", false],
    ['size([1, 2, 3])', 3n],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
  it.skip('size([]) — bare [] types to list(dyn), rejected by the static-subset gate', async () => {
    expect(await evalCel('size([])')).toBe(0n);
  });
});

describe('concatenation (langdef §Lists)', () => {
  it('[1, 2] + [3] == [1, 2, 3]', async () => {
    expect(await evalCel('[1, 2] + [3]')).toEqual([1n, 2n, 3n]);
  });
  it('[] + [1] == [1]', async () => {
    expect(await evalCel('[] + [1]')).toEqual([1n]);
  });
});

describe('macros over a list source (langdef §Macros)', () => {
  it('map transforms each element', async () => {
    expect(await evalCel('[1, 2, 3].map(x, x * 2)')).toEqual([2n, 4n, 6n]);
  });
  it('map with a filter predicate (3-arg form)', async () => {
    expect(await evalCel('[1, 2, 3, 4].map(x, x > 2, x)')).toEqual([3n, 4n]);
  });
  it('filter keeps matching elements', async () => {
    expect(await evalCel('[1, 2, 3, 4].filter(x, x % 2 == 0)')).toEqual([
      2n,
      4n,
    ]);
  });
  it('exists — at least one match', async () => {
    expect(await evalCel('[1, 2, 3].exists(x, x > 2)')).toBe(true);
    expect(await evalCel('[1, 2, 3].exists(x, x > 9)')).toBe(false);
  });
  it('all — every element matches', async () => {
    expect(await evalCel('[1, 2, 3].all(x, x > 0)')).toBe(true);
    expect(await evalCel('[1, 2, 3].all(x, x > 1)')).toBe(false);
  });
  it('exists_one — exactly one match', async () => {
    expect(await evalCel('[1, 2, 3].exists_one(x, x == 2)')).toBe(true);
    expect(await evalCel('[1, 2, 3].exists_one(x, x > 1)')).toBe(false);
  });
});
