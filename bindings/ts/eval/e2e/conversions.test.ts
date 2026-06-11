// e2e type-conversion behaviors — int / uint / double / string / bytes /
// bool inter-conversions and their error VALUES.
//
// Ported from the C++ `e2e/m10_test.cc` (the conversion overloads
// `bool/int/uint/double/string/bytes` and their inter-conversions).
// Grounded in `doc/langdef.md` §"Type conversions" (lines ~2055-2210):
//   - `int(double)` / `uint(double)` round toward zero, error if out of
//     range (langdef: "rounds toward zero, errors if out of range").
//   - `string(double)` yields the canonical decimal form; ±Inf render as
//     `+Inf` / `-Inf`; NaN as `NaN`.
//   - `bytes(string)` is the UTF-8 encoding of the string.

import { describe, expect, it } from 'vitest';

import { isCelError } from './helpers.js';
import { evalCel } from './helpers.js';

describe('int(...)', () => {
  it.each([
    ["int('42')", 42n],
    ["int('-7')", -7n],
    ['int(3.9)', 3n], // rounds toward zero (truncates)
    ['int(-3.9)', -3n],
    ['int(2u)', 2n],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
  it('int(out-of-range double) → error value (langdef: errors if out of range)', async () => {
    expect(isCelError(await evalCel('int(9223372036854775807.0)'))).toBe(true);
  });
  it('int(non-numeric string) → error value', async () => {
    expect(isCelError(await evalCel("int('xyz')"))).toBe(true);
  });
});

describe('uint(...)', () => {
  it.each([
    ["uint('5')", 5n],
    ['uint(5)', 5n],
    ['uint(3.9)', 3n], // truncates
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
  it('uint(negative string) → error value', async () => {
    expect(isCelError(await evalCel("uint('-1')"))).toBe(true);
  });
});

describe('double(...)', () => {
  it.each([
    ['double(3)', 3],
    ['double(2u)', 2],
    ["double('3.14')", 3.14],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
  it('double("nan") decodes to JS NaN', async () => {
    const v = await evalCel('double("nan")');
    expect(typeof v).toBe('number');
    expect(Number.isNaN(v as number)).toBe(true);
  });
  it('double("inf") decodes to Infinity', async () => {
    expect(await evalCel('double("inf")')).toBe(Infinity);
  });
  it('NaN != NaN (IEEE / langdef §Equality)', async () => {
    expect(await evalCel('double("nan") == double("nan")')).toBe(false);
  });
});

describe('string(...)', () => {
  it.each([
    ['string(42)', '42'],
    ['string(2u)', '2'],
    ['string(3.0)', '3'],
    ['string(1.5)', '1.5'],
    ['string(true)', 'true'],
    ["string(b'abc')", 'abc'],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
  it('string(+Inf) → "+Inf" (CEL canonical form)', async () => {
    expect(await evalCel('string(1.0 / 0.0)')).toBe('+Inf');
  });
});

describe('bytes(...) — UTF-8 encode', () => {
  it("bytes('abc') == [97,98,99]", async () => {
    const v = await evalCel("bytes('abc')");
    expect(v).toBeInstanceOf(Uint8Array);
    expect([...(v as Uint8Array)]).toEqual([97, 98, 99]);
  });
  it("bytes('héllo') is the UTF-8 encoding", async () => {
    const v = await evalCel("bytes('héllo')");
    expect([...(v as Uint8Array)]).toEqual([104, 195, 169, 108, 108, 111]);
  });
});

describe('bool(...)', () => {
  it.each([
    ["bool('true')", true],
    ["bool('false')", false],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('conversions out of the static subset', () => {
  // `type(x)` returns a CEL `type` value (kind 11 on the wire), which the
  // TS codec treats as out of scope (§A.3): the type subsystem / `type`
  // value kind is not a supported decode.  The conversion family below it
  // (int/uint/double/string/bytes/bool) is fully supported.
  it.skip('type(1) — the type value kind is out of scope for the codec', async () => {
    expect(await evalCel('type(1)')).toBeDefined();
  });
});
