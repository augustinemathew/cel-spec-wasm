// Tests for the decoded-CelValue → expected-matcher comparator.  The
// matrix pins each kind plus the CEL-specific equality edges (NaN-matches-
// NaN, error-matches-any-error, order-agnostic maps) from langdef.

import type { CelValue } from '@cel-wasm/eval';
import { describe, expect, it } from 'vitest';

import {
  celValuesEqual,
  compareEvalError,
  compareValue,
  isCelError,
  isCelType,
} from './compare.js';
import type { ExpectedValue } from './corpus.js';

function err(code: number): CelValue {
  return { kind: 'error', code, message: 'x' };
}

describe('compareValue — scalars', () => {
  it('matches null', () => {
    expect(compareValue(null, { kind: 'null' })).toBeUndefined();
    expect(compareValue(1n, { kind: 'null' })).toBeDefined();
  });

  it('matches bool', () => {
    expect(compareValue(true, { kind: 'bool', value: true })).toBeUndefined();
    expect(compareValue(false, { kind: 'bool', value: true })).toBeDefined();
  });

  it('matches int / uint as bigint', () => {
    expect(compareValue(5n, { kind: 'int', value: 5n })).toBeUndefined();
    expect(compareValue(5n, { kind: 'uint', value: 5n })).toBeUndefined();
    expect(compareValue(6n, { kind: 'int', value: 5n })).toBeDefined();
  });

  it('matches int64 boundary values', () => {
    expect(
      compareValue(-9223372036854775808n, {
        kind: 'int',
        value: -9223372036854775808n,
      }),
    ).toBeUndefined();
    expect(
      compareValue(18446744073709551615n, {
        kind: 'uint',
        value: 18446744073709551615n,
      }),
    ).toBeUndefined();
  });

  it('matches a double, with NaN-matches-NaN', () => {
    expect(compareValue(1.5, { kind: 'double', value: 1.5 })).toBeUndefined();
    expect(compareValue(NaN, { kind: 'double', value: NaN })).toBeUndefined();
    expect(compareValue(1.5, { kind: 'double', value: 2.5 })).toBeDefined();
  });

  it('does not match a bigint to a double matcher', () => {
    expect(compareValue(5n, { kind: 'double', value: 5 })).toBeDefined();
  });

  it('matches a string', () => {
    expect(compareValue('hi', { kind: 'string', value: 'hi' })).toBeUndefined();
    expect(compareValue('ho', { kind: 'string', value: 'hi' })).toBeDefined();
  });

  it('matches bytes including an embedded NUL', () => {
    expect(
      compareValue(new Uint8Array([0, 1, 2]), {
        kind: 'bytes',
        value: new Uint8Array([0, 1, 2]),
      }),
    ).toBeUndefined();
    expect(
      compareValue(new Uint8Array([0, 1]), {
        kind: 'bytes',
        value: new Uint8Array([0, 2]),
      }),
    ).toBeDefined();
  });
});

describe('compareValue — type matcher (type_value)', () => {
  it.each<string>(['int', 'uint', 'bool', 'list', 'map', 'null_type', 'type'])(
    'matches the scalar type name %s exactly',
    (name) => {
      expect(
        compareValue({ kind: 'type', name }, { kind: 'type', name }),
      ).toBeUndefined();
    },
  );

  it('matches a message-FQN type name', () => {
    const fqn = 'google.protobuf.Timestamp';
    expect(
      compareValue({ kind: 'type', name: fqn }, { kind: 'type', name: fqn }),
    ).toBeUndefined();
  });

  it('rejects a mismatched type name (exact equality, no loose match)', () => {
    expect(
      compareValue(
        { kind: 'type', name: 'uint' },
        { kind: 'type', name: 'int' },
      ),
    ).toBeDefined();
    // A prefix / suffix is not a match.
    expect(
      compareValue(
        { kind: 'type', name: 'google.protobuf.Timestamp' },
        { kind: 'type', name: 'Timestamp' },
      ),
    ).toBeDefined();
  });

  it('rejects a non-type result against a type matcher', () => {
    // The STRING "int" is not the TYPE int.
    expect(compareValue('int', { kind: 'type', name: 'int' })).toBeDefined();
    expect(
      compareValue(null, { kind: 'type', name: 'null_type' }),
    ).toBeDefined();
    expect(compareValue(err(13), { kind: 'type', name: 'int' })).toBeDefined();
  });
});

describe('isCelType', () => {
  it('narrows a tagged type record', () => {
    expect(isCelType({ kind: 'type', name: 'int' })).toBe(true);
  });

  it('rejects other shapes', () => {
    expect(isCelType('type')).toBe(false);
    expect(isCelType(null)).toBe(false);
    expect(isCelType(err(13))).toBe(false);
    expect(isCelType({ kind: 'timestamp', epochSeconds: 0n, nanos: 0 })).toBe(
      false,
    );
  });
});

describe('compareValue — aggregates', () => {
  it('matches an ordered list', () => {
    const got: CelValue = [1n, 2n, 3n];
    const want: ExpectedValue = {
      kind: 'list',
      elements: [
        { kind: 'int', value: 1n },
        { kind: 'int', value: 2n },
        { kind: 'int', value: 3n },
      ],
    };
    expect(compareValue(got, want)).toBeUndefined();
  });

  it('rejects a list with a different order', () => {
    const got: CelValue = [1n, 3n, 2n];
    const want: ExpectedValue = {
      kind: 'list',
      elements: [
        { kind: 'int', value: 1n },
        { kind: 'int', value: 2n },
        { kind: 'int', value: 3n },
      ],
    };
    expect(compareValue(got, want)).toBeDefined();
  });

  it('matches a map regardless of insertion order', () => {
    const got = new Map<CelValue, CelValue>([
      ['b', 2n],
      ['a', 1n],
    ]);
    const want: ExpectedValue = {
      kind: 'map',
      entries: [
        {
          key: { kind: 'string', value: 'a' },
          value: { kind: 'int', value: 1n },
        },
        {
          key: { kind: 'string', value: 'b' },
          value: { kind: 'int', value: 2n },
        },
      ],
    };
    expect(compareValue(got, want)).toBeUndefined();
  });

  it('rejects a map with a missing key', () => {
    const got = new Map<CelValue, CelValue>([['a', 1n]]);
    const want: ExpectedValue = {
      kind: 'map',
      entries: [
        {
          key: { kind: 'string', value: 'z' },
          value: { kind: 'int', value: 1n },
        },
      ],
    };
    expect(compareValue(got, want)).toBeDefined();
  });

  it('matches an enum result as an int', () => {
    expect(compareValue(2n, { kind: 'enum', value: 2n })).toBeUndefined();
  });
});

describe('compareEvalError', () => {
  it('matches any error value', () => {
    expect(compareEvalError(err(2))).toBeUndefined();
    expect(compareEvalError(err(13))).toBeUndefined();
  });

  it('rejects a non-error value', () => {
    expect(compareEvalError(5n)).toBeDefined();
    expect(compareEvalError(null)).toBeDefined();
  });
});

describe('isCelError', () => {
  it('recognizes error values only', () => {
    expect(isCelError(err(1))).toBe(true);
    expect(isCelError(5n)).toBe(false);
    expect(isCelError({ kind: 'timestamp', epochSeconds: 0n, nanos: 0 })).toBe(
      false,
    );
  });
});

describe('celValuesEqual — deep CelValue equality (object_value compare)', () => {
  it('matches equal scalars across kinds', () => {
    expect(celValuesEqual(1n, 1n)).toBeUndefined();
    expect(celValuesEqual(1n, 2n)).toBeDefined();
    expect(celValuesEqual(true, true)).toBeUndefined();
    expect(celValuesEqual('a', 'a')).toBeUndefined();
    expect(celValuesEqual('a', 'b')).toBeDefined();
    expect(celValuesEqual(null, null)).toBeUndefined();
    expect(celValuesEqual(null, 1n)).toBeDefined();
  });

  it('matches doubles with NaN-equals-NaN', () => {
    expect(celValuesEqual(1.5, 1.5)).toBeUndefined();
    expect(celValuesEqual(NaN, NaN)).toBeUndefined();
    expect(celValuesEqual(1.5, 2.5)).toBeDefined();
  });

  it('matches bytes element-wise', () => {
    expect(
      celValuesEqual(Uint8Array.of(1, 2), Uint8Array.of(1, 2)),
    ).toBeUndefined();
    expect(celValuesEqual(Uint8Array.of(1), Uint8Array.of(1, 2))).toBeDefined();
  });

  it('matches lists order-aware', () => {
    expect(celValuesEqual([1n, 2n], [1n, 2n])).toBeUndefined();
    expect(celValuesEqual([1n, 2n], [2n, 1n])).toBeDefined();
  });

  it('matches maps order-agnostic', () => {
    const a = new Map<CelValue, CelValue>([
      ['x', 1n],
      ['y', 2n],
    ]);
    const b = new Map<CelValue, CelValue>([
      ['y', 2n],
      ['x', 1n],
    ]);
    expect(celValuesEqual(a, b)).toBeUndefined();
    expect(celValuesEqual(a, new Map([['x', 1n]]))).toBeDefined();
  });

  it('matches message objects field-by-field', () => {
    const got: CelValue = { a: 1n, b: 'hi', c: [true] };
    expect(celValuesEqual(got, { a: 1n, b: 'hi', c: [true] })).toBeUndefined();
    expect(celValuesEqual(got, { a: 2n, b: 'hi', c: [true] })).toBeDefined();
    expect(celValuesEqual(got, { a: 1n, b: 'hi' })).toBeDefined();
  });

  it('matches nested message + tagged records', () => {
    const ts = { kind: 'timestamp', epochSeconds: 5n, nanos: 0 };
    expect(celValuesEqual({ when: ts }, { when: { ...ts } })).toBeUndefined();
  });
});
