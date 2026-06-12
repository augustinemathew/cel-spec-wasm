import type { CelValue } from '@cel-wasm/eval';
import { describe, expect, it } from 'vitest';

import { renderResult, renderValue, typeNameOf } from './render.js';

describe('renderValue', () => {
  it('renders null', () => {
    expect(renderValue(null)).toBe('null');
  });

  it('renders booleans', () => {
    expect(renderValue(true)).toBe('true');
    expect(renderValue(false)).toBe('false');
  });

  it('renders a bigint (int/uint) without a suffix', () => {
    expect(renderValue(42n)).toBe('42');
    expect(renderValue(-9223372036854775808n)).toBe('-9223372036854775808');
    expect(renderValue(18446744073709551615n)).toBe('18446744073709551615');
  });

  it('renders an integral double with a trailing .0', () => {
    expect(renderValue(2)).toBe('2.0');
    expect(renderValue(0)).toBe('0.0');
  });

  it('renders a fractional double as-is', () => {
    expect(renderValue(3.14)).toBe('3.14');
  });

  it('quotes strings', () => {
    expect(renderValue('hi')).toBe('"hi"');
    expect(renderValue('')).toBe('""');
    expect(renderValue('a"b')).toBe('"a\\"b"');
  });

  it('renders bytes as hex with a length suffix', () => {
    expect(renderValue(new Uint8Array([0, 255, 16]))).toBe(
      'b"00ff10" (3 bytes)',
    );
    expect(renderValue(new Uint8Array([]))).toBe('b"" (0 bytes)');
  });

  it('renders a list recursively', () => {
    expect(renderValue([2n, 4n, 6n])).toBe('[2, 4, 6]');
    expect(renderValue([])).toBe('[]');
    expect(renderValue([[1n], 'x'])).toBe('[[1], "x"]');
  });

  it('renders a map', () => {
    const map = new Map<CelValue, CelValue>([
      ['a', 1n],
      ['b', 2n],
    ]);
    expect(renderValue(map)).toBe('{"a": 1, "b": 2}');
  });

  it('renders a message object', () => {
    expect(renderValue({ name: 'Ada', age: 36n })).toBe(
      '{name: "Ada", age: 36}',
    );
  });

  it('renders a timestamp', () => {
    const ts: CelValue = { kind: 'timestamp', epochSeconds: 0n, nanos: 0 };
    expect(renderValue(ts)).toBe('timestamp(1970-01-01T00:00:00.000Z)');
  });

  it('renders a duration', () => {
    const dur: CelValue = { kind: 'duration', seconds: 90n, nanos: 0 };
    expect(renderValue(dur)).toBe('duration(90s)');
  });

  it('renders a type value as its name', () => {
    const ty: CelValue = { kind: 'type', name: 'int' };
    expect(renderValue(ty)).toBe('int');
  });

  it('renders an error value', () => {
    const err: CelValue = {
      kind: 'error',
      code: 11,
      message: 'divide by zero',
    };
    expect(renderValue(err)).toBe('error(11): divide by zero');
  });
});

describe('typeNameOf', () => {
  it.each<[CelValue, string]>([
    [null, 'null'],
    [true, 'bool'],
    [1n, 'int'],
    [1.5, 'double'],
    ['s', 'string'],
    [new Uint8Array([1]), 'bytes'],
    [[1n], 'list'],
    [new Map(), 'map'],
    [{ field: 1n }, 'message'],
    [{ kind: 'timestamp', epochSeconds: 0n, nanos: 0 }, 'timestamp'],
    [{ kind: 'duration', seconds: 0n, nanos: 0 }, 'duration'],
    [{ kind: 'type', name: 'int' }, 'type'],
    [{ kind: 'error', code: 11, message: 'x' }, 'error'],
  ])('names %o as %s', (value, expected) => {
    expect(typeNameOf(value)).toBe(expected);
  });
});

describe('renderResult', () => {
  it('classifies a value result', () => {
    const r = renderResult(true);
    expect(r.className).toBe('value');
    expect(r.typeName).toBe('bool');
    expect(r.text).toBe('true');
  });

  it('classifies an error result with a readable message', () => {
    const r = renderResult({ kind: 'error', code: 11, message: 'div by zero' });
    expect(r.className).toBe('error');
    expect(r.typeName).toBe('error');
    expect(r.text).toContain('div by zero');
    expect(r.text).toContain('11');
  });
});
