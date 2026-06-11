// Tests for the corpus-subset textproto reader.  The grammar is the
// known risk of WI-3.1 (protobufjs cannot read textproto), so the matrix
// here is the load-bearing coverage: quote styles, escapes, adjacent
// concatenation, nested messages, repeated fields, enums, and the
// boundary numerics the corpus actually contains.

import { describe, expect, it } from 'vitest';

import {
  fieldValue,
  fieldValues,
  parseTextproto,
  TextprotoParseError,
  type TextprotoMessage,
  type TextprotoScalar,
} from './textproto.js';

function scalar(msg: TextprotoMessage, name: string): TextprotoScalar {
  const v = fieldValue(msg, name);
  if (v === undefined || v.kind === 'message') {
    throw new Error(`expected scalar field '${name}'`);
  }
  return v;
}

function message(msg: TextprotoMessage, name: string): TextprotoMessage {
  const v = fieldValue(msg, name);
  if (v === undefined || v.kind !== 'message') {
    throw new Error(`expected message field '${name}'`);
  }
  return v;
}

// A string scalar carries both `value` (UTF-8 text) and `bytes` (octets);
// most assertions only care about the decoded text.
function stringValue(msg: TextprotoMessage, name: string): string {
  const s = scalar(msg, name);
  if (s.kind !== 'string') {
    throw new Error(`expected string field '${name}', got ${s.kind}`);
  }
  return s.value;
}

describe('parseTextproto — scalars', () => {
  it('reads a double-quoted string', () => {
    const m = parseTextproto('expr: "abc"');
    expect(stringValue(m, 'expr')).toBe('abc');
  });

  it('reads a single-quoted string', () => {
    const m = parseTextproto("expr: 'abc'");
    expect(stringValue(m, 'expr')).toBe('abc');
  });

  it('reads an empty string', () => {
    const m = parseTextproto('value: ""');
    expect(stringValue(m, 'value')).toBe('');
  });

  it('reads a bool', () => {
    expect(scalar(parseTextproto('v: true'), 'v')).toEqual({
      kind: 'bool',
      value: true,
    });
    expect(scalar(parseTextproto('v: false'), 'v')).toEqual({
      kind: 'bool',
      value: false,
    });
  });

  it('reads an integer number', () => {
    const s = scalar(parseTextproto('v: 42'), 'v');
    expect(s).toEqual({ kind: 'number', value: 42, raw: '42' });
  });

  it('reads a negative number', () => {
    const s = scalar(parseTextproto('v: -23'), 'v');
    expect(s).toEqual({ kind: 'number', value: -23, raw: '-23' });
  });

  it('reads a double with an exponent', () => {
    const s = scalar(parseTextproto('v: -2.3e+1'), 'v');
    expect(s.kind).toBe('number');
    if (s.kind === 'number') {
      expect(s.value).toBeCloseTo(-23);
    }
  });

  it('preserves the raw text of an int64 boundary value', () => {
    // -9223372036854775808 exceeds Number precision; the `raw` text is
    // how the corpus layer recovers the exact bigint.
    const s = scalar(parseTextproto('v: -9223372036854775808'), 'v');
    expect(s.kind).toBe('number');
    if (s.kind === 'number') {
      expect(s.raw).toBe('-9223372036854775808');
      expect(BigInt(s.raw)).toBe(-9223372036854775808n);
    }
  });

  it('reads inf / nan', () => {
    const inf = scalar(parseTextproto('v: inf'), 'v');
    expect(inf.kind === 'number' && inf.value).toBe(Infinity);
    const nan = scalar(parseTextproto('v: nan'), 'v');
    expect(nan.kind === 'number' && Number.isNaN(nan.value)).toBe(true);
  });

  it('reads a bareword enum token', () => {
    expect(scalar(parseTextproto('v: NULL_VALUE'), 'v')).toEqual({
      kind: 'enum',
      value: 'NULL_VALUE',
    });
  });
});

describe('parseTextproto — escapes', () => {
  it('decodes C-style escapes', () => {
    const m = parseTextproto('v: "a\\nb\\tc\\\\d\\"e"');
    expect(stringValue(m, 'v')).toBe('a\nb\tc\\d"e');
  });

  it('decodes a hex byte escape to a single byte', () => {
    const m = parseTextproto('v: "\\x00"');
    const s = scalar(m, 'v');
    expect(s.kind).toBe('string');
    if (s.kind === 'string') {
      expect(Array.from(s.bytes)).toEqual([0]);
      // \x00 is a valid (NUL) UTF-8 byte → the decoded string is U+0000.
      expect(s.value).toBe('\x00');
    }
  });

  it('decodes an octal byte escape to its raw byte', () => {
    // \377 == 0xFF — a lone invalid-UTF-8 byte; it stays a byte in
    // `bytes`, and `value` (the UTF-8 decode) is the replacement char.
    const m = parseTextproto('v: "\\377"');
    const s = scalar(m, 'v');
    expect(s.kind).toBe('string');
    if (s.kind === 'string') {
      expect(Array.from(s.bytes)).toEqual([0xff]);
    }
  });

  it('decodes \\xHH bytes of a multibyte char as UTF-8', () => {
    // \xe2\x9c\x8c is the UTF-8 of ✌ (U+270C).
    const m = parseTextproto('v: "\\xe2\\x9c\\x8c"');
    const s = scalar(m, 'v');
    expect(s.kind).toBe('string');
    if (s.kind === 'string') {
      expect(Array.from(s.bytes)).toEqual([0xe2, 0x9c, 0x8c]);
      expect(s.value).toBe('✌');
    }
  });

  it('decodes a \\u unicode escape', () => {
    const m = parseTextproto('v: "\\u00ff"');
    const s = scalar(m, 'v');
    expect(s.kind === 'string' && s.value).toBe('ÿ');
  });

  it('concatenates adjacent string literals', () => {
    const m = parseTextproto('expr:\n  "foo"\n  "bar"');
    expect(stringValue(m, 'expr')).toBe('foobar');
  });
});

describe('parseTextproto — messages', () => {
  it('reads a nested message with the brace form', () => {
    const m = parseTextproto('value { int64_value: 7 }');
    const inner = message(m, 'value');
    expect(scalar(inner, 'int64_value')).toEqual({
      kind: 'number',
      value: 7,
      raw: '7',
    });
  });

  it('reads a nested message with the colon-brace form', () => {
    const m = parseTextproto('value: { bool_value: true }');
    const inner = message(m, 'value');
    expect(scalar(inner, 'bool_value')).toEqual({ kind: 'bool', value: true });
  });

  it('reads the angle-bracket message form', () => {
    const m = parseTextproto('value <int64_value: 1>');
    expect(scalar(message(m, 'value'), 'int64_value')).toEqual({
      kind: 'number',
      value: 1,
      raw: '1',
    });
  });

  it('reads an empty message', () => {
    const m = parseTextproto('value: { list_value: {} }');
    const inner = message(m, 'value');
    const lv = message(inner, 'list_value');
    expect(lv.fields.size).toBe(0);
  });

  it('keeps every occurrence of a repeated field', () => {
    const m = parseTextproto(
      'list_value { values { int64_value: 1 } values { int64_value: 2 } }',
    );
    const lv = message(m, 'list_value');
    const values = fieldValues(lv, 'values');
    expect(values.length).toBe(2);
  });

  it('keeps repeated top-level test entries', () => {
    const m = parseTextproto(
      'test { name: "a" }\ntest { name: "b" }\ntest { name: "c" }',
    );
    expect(fieldValues(m, 'test').length).toBe(3);
  });
});

describe('parseTextproto — trivia', () => {
  it('skips line comments', () => {
    const m = parseTextproto('# a comment\nexpr: "x"  # trailing\n');
    expect(stringValue(m, 'expr')).toBe('x');
  });

  it('handles the proto-file / proto-message header comments', () => {
    const m = parseTextproto(
      '# proto-file: x.proto\n# proto-message: Foo\nname: "basic"\n',
    );
    expect(stringValue(m, 'name')).toBe('basic');
  });
});

describe('parseTextproto — errors', () => {
  it('throws on an unterminated string', () => {
    expect(() => parseTextproto('v: "abc')).toThrow(TextprotoParseError);
  });

  it('throws on an unterminated message body', () => {
    expect(() => parseTextproto('v { a: 1')).toThrow(TextprotoParseError);
  });

  it('throws on a stray value with no field name', () => {
    expect(() => parseTextproto(': 1')).toThrow(TextprotoParseError);
  });
});
