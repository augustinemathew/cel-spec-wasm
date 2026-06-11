// e2e operator-matrix behaviors — arithmetic, comparison, logic, the
// conditional, and overflow/divide error VALUES.
//
// Ported from the C++ `e2e/m5_test.cc` (the general `kCall` arm: scalar
// arithmetic, comparison, equality, logic, ternary) and the
// arithmetic-error rows it pins.  Each assertion is grounded in
// `doc/langdef.md`:
//   - §"Numbers"/"Overflow" — int64/uint64 arithmetic traps to an OVERFLOW
//     error value; division/modulus by zero to DIVIDE_BY_ZERO/MODULUS_BY_ZERO.
//   - §"Logical Operators" — `&&` / `||` short-circuit; `!` negates.
//   - §"Conditional" — `c ? a : b`.
//
// CEL's static type checker forbids HETEROGENEOUS arithmetic/equality
// (`1 + 1u`, `1 == 1.0`): there is no cross-numeric-type overload in the
// static subset.  Those are `it.skip`ed with the verified blocker.

import { describe, expect, it } from 'vitest';

import { errorCode, evalCel } from './helpers.js';

import { CelErrorCode } from '@cel-wasm/eval';

describe('arithmetic — int64 (langdef §Numbers)', () => {
  it.each([
    ['1 + 2', 3n],
    ['7 - 3', 4n],
    ['6 * 7', 42n],
    ['9 / 2', 4n], // integer division truncates toward zero
    ['9 % 2', 1n],
    ['-9 / 2', -4n],
    ['-(5)', -5n],
    ['2 + 3 * 4', 14n], // precedence
    ['(2 + 3) * 4', 20n],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('arithmetic — uint64 / double', () => {
  it('1u + 2u == 3u', async () => {
    expect(await evalCel('1u + 2u')).toBe(3n);
  });
  it('uint division truncates: 9u / 2u == 4u', async () => {
    expect(await evalCel('9u / 2u')).toBe(4n);
  });
  it('1.5 + 2.25 == 3.75', async () => {
    expect(await evalCel('1.5 + 2.25')).toBe(3.75);
  });
  it('7.5 / 2.0 == 3.75', async () => {
    expect(await evalCel('7.5 / 2.0')).toBe(3.75);
  });
});

describe('arithmetic error values (langdef §Overflow)', () => {
  it('int overflow → OVERFLOW value, not a throw', async () => {
    expect(errorCode(await evalCel('9223372036854775807 + 1'))).toBe(
      CelErrorCode.OVERFLOW,
    );
  });
  it('int underflow → OVERFLOW value', async () => {
    expect(errorCode(await evalCel('-9223372036854775807 - 2'))).toBe(
      CelErrorCode.OVERFLOW,
    );
  });
  it('int multiply overflow → OVERFLOW value', async () => {
    expect(errorCode(await evalCel('9223372036854775807 * 2'))).toBe(
      CelErrorCode.OVERFLOW,
    );
  });
  it('uint underflow (1u - 2u) → OVERFLOW value', async () => {
    expect(errorCode(await evalCel('1u - 2u'))).toBe(CelErrorCode.OVERFLOW);
  });
  it('uint overflow (max + 1) → OVERFLOW value', async () => {
    expect(errorCode(await evalCel('18446744073709551615u + 1u'))).toBe(
      CelErrorCode.OVERFLOW,
    );
  });
  it('int divide by zero → DIVIDE_BY_ZERO value', async () => {
    expect(errorCode(await evalCel('1 / 0'))).toBe(CelErrorCode.DIVIDE_BY_ZERO);
  });
  it('uint divide by zero → DIVIDE_BY_ZERO value', async () => {
    expect(errorCode(await evalCel('1u / 0u'))).toBe(
      CelErrorCode.DIVIDE_BY_ZERO,
    );
  });
  it('int modulus by zero → MODULUS_BY_ZERO value', async () => {
    expect(errorCode(await evalCel('1 % 0'))).toBe(
      CelErrorCode.MODULUS_BY_ZERO,
    );
  });
});

describe('comparison — same-type (langdef §Comparisons)', () => {
  it.each([
    ['1 < 2', true],
    ['2 < 1', false],
    ['2 <= 2', true],
    ['3 > 2', true],
    ['2 >= 3', false],
    ['1u < 2u', true],
    ['1.5 < 2.5', true],
    ['2.5 <= 2.5', true],
    ["'a' < 'b'", true],
    ["'b' < 'a'", false],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('equality — same-type (langdef §Equality)', () => {
  it.each([
    ['1 == 1', true],
    ['1 != 2', true],
    ['1 == 2', false],
    ['true == true', true],
    ['true != false', true],
    ['null == null', true],
    ["'foo' == 'foo'", true],
    ["'foo' == 'bar'", false],
    ["b'abc' == b'abc'", true],
    ["b'abc' == b'xyz'", false],
    ['[1, 2, 3] == [1, 2, 3]', true],
    ['[1, 2, 3] == [1, 2]', false],
    ['1u == 1u', true],
    ['1.5 == 1.5', true],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('heterogeneous numeric ops — out of the static subset', () => {
  // langdef §"Numeric values" defines numericEquals across int/uint/double,
  // but cel-cpp's static type checker stamps NO cross-type overload for the
  // raw operators, so the compiler rejects these at type-check time.  They
  // are admissible only through `dyn(...)`, which is out of scope (§A.3).
  it.skip('1 == 1.0 (cross-type eq rejected by the static type checker)', async () => {
    expect(await evalCel('1 == 1.0')).toBe(true);
  });
  it.skip('1 == 1u (cross-type eq rejected by the static type checker)', async () => {
    expect(await evalCel('1 == 1u')).toBe(true);
  });
  it.skip('1 + 1u (cross-type arithmetic rejected by the static type checker)', async () => {
    expect(await evalCel('1 + 1u')).toBe(2n);
  });
});

describe('logic — short-circuit & negation (langdef §Logical Operators)', () => {
  it.each([
    ['true && true', true],
    ['true && false', false],
    ['false && true', false],
    ['true || false', true],
    ['false || false', false],
    ['!true', false],
    ['!false', true],
    ['!(1 < 2)', false],
    ['true && (1 < 2)', true],
    ['(2 > 1) || (1 > 2)', true],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('conditional (langdef §Conditional)', () => {
  it('true ? 1 : 2 == 1', async () => {
    expect(await evalCel('true ? 1 : 2')).toBe(1n);
  });
  it('false ? 1 : 2 == 2', async () => {
    expect(await evalCel('false ? 1 : 2')).toBe(2n);
  });
  it('selects the taken branch by value', async () => {
    expect(await evalCel("2 > 1 ? 'y' : 'n'")).toBe('y');
    expect(await evalCel("2 < 1 ? 'y' : 'n'")).toBe('n');
  });
  it('short-circuits the untaken branch (no divide-by-zero on the false arm)', async () => {
    // The `1 / 0` arm is not taken, so no error value surfaces.
    expect(await evalCel('true ? 7 : (1 / 0)')).toBe(7n);
  });
});
