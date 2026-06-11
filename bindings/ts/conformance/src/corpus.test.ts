// Tests for the textproto → typed-corpus interpreter.  The matrix pins
// each `cel.expr.Value` matcher kind, the type_env decl shapes, and the
// binding-value lowering against synthetic textproto fragments, plus a
// round-trip against the real `basic.textproto` file.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import {
  expectedToInput,
  interpretValue,
  latin1ToBytes,
  loadSimpleTestFile,
  type ExpectedValue,
} from './corpus.js';
import { parseTextproto, type TextprotoMessage } from './textproto.js';

const CORPUS_DIR = fileURLToPath(
  new URL('../../../../spec/tests/simple/testdata/', import.meta.url),
);

function valueMsg(body: string): TextprotoMessage {
  const m = parseTextproto(`value: { ${body} }`);
  const v = m.fields.get('value')?.[0];
  if (v === undefined || v.kind !== 'message') {
    throw new Error('expected value message');
  }
  return v;
}

describe('interpretValue — scalar matchers', () => {
  it('reads null', () => {
    expect(interpretValue(valueMsg('null_value: NULL_VALUE'))).toEqual({
      kind: 'null',
    });
  });

  it('reads bool', () => {
    expect(interpretValue(valueMsg('bool_value: true'))).toEqual({
      kind: 'bool',
      value: true,
    });
  });

  it('reads int64 at the negative boundary', () => {
    expect(
      interpretValue(valueMsg('int64_value: -9223372036854775808')),
    ).toEqual({ kind: 'int', value: -9223372036854775808n });
  });

  it('reads uint64 at the max boundary', () => {
    expect(
      interpretValue(valueMsg('uint64_value: 18446744073709551615')),
    ).toEqual({ kind: 'uint', value: 18446744073709551615n });
  });

  it('reads a double', () => {
    expect(interpretValue(valueMsg('double_value: -23.5'))).toEqual({
      kind: 'double',
      value: -23.5,
    });
  });

  it('reads a string', () => {
    expect(interpretValue(valueMsg('string_value: "hi"'))).toEqual({
      kind: 'string',
      value: 'hi',
    });
  });

  it('decodes a string_value whose byte escapes are UTF-8', () => {
    // The corpus stores a `string_value` as escaped UTF-8 bytes; e.g.
    // ✌ (U+270C) is `\xe2\x9c\x8c`.  The interpreter must re-decode those
    // bytes as UTF-8, not keep them as 3 latin1 code units.
    expect(interpretValue(valueMsg('string_value: "\\xe2\\x9c\\x8c"'))).toEqual(
      { kind: 'string', value: '✌' },
    );
  });

  it('reads bytes with an embedded NUL', () => {
    const v = interpretValue(valueMsg('bytes_value: "\\x00\\x01"'));
    expect(v.kind).toBe('bytes');
    if (v.kind === 'bytes') {
      expect(Array.from(v.value)).toEqual([0, 1]);
    }
  });

  it('reads a type matcher', () => {
    expect(interpretValue(valueMsg('type_value: "list"'))).toEqual({
      kind: 'type',
      name: 'list',
    });
  });
});

describe('interpretValue — aggregate matchers', () => {
  it('reads an empty list', () => {
    expect(interpretValue(valueMsg('list_value: {}'))).toEqual({
      kind: 'list',
      elements: [],
    });
  });

  it('reads a list of ints', () => {
    const v = interpretValue(
      valueMsg(
        'list_value: { values { int64_value: 1 } values { int64_value: 2 } }',
      ),
    );
    expect(v).toEqual({
      kind: 'list',
      elements: [
        { kind: 'int', value: 1n },
        { kind: 'int', value: 2n },
      ],
    });
  });

  it('reads a map with a string key', () => {
    const v = interpretValue(
      valueMsg(
        'map_value: { entries { key { string_value: "a" } value { int64_value: 1 } } }',
      ),
    );
    expect(v).toEqual({
      kind: 'map',
      entries: [
        {
          key: { kind: 'string', value: 'a' },
          value: { kind: 'int', value: 1n },
        },
      ],
    });
  });

  it('reads an enum matcher as a bigint', () => {
    expect(interpretValue(valueMsg('enum_value: { value: 2 }'))).toEqual({
      kind: 'enum',
      value: 2n,
    });
  });

  it('models object_value as the out-of-scope kind', () => {
    expect(
      interpretValue(valueMsg('object_value: { [type.url/Foo] {} }')).kind,
    ).toBe('object');
  });
});

describe('expectedToInput', () => {
  it('lowers scalars', () => {
    expect(expectedToInput({ kind: 'int', value: 7n })).toBe(7n);
    expect(expectedToInput({ kind: 'string', value: 'x' })).toBe('x');
    expect(expectedToInput({ kind: 'null' })).toBeNull();
  });

  it('lowers a list', () => {
    const v: ExpectedValue = {
      kind: 'list',
      elements: [
        { kind: 'int', value: 1n },
        { kind: 'bool', value: true },
      ],
    };
    expect(expectedToInput(v)).toEqual([1n, true]);
  });

  it('lowers a map', () => {
    const v: ExpectedValue = {
      kind: 'map',
      entries: [
        {
          key: { kind: 'string', value: 'k' },
          value: { kind: 'int', value: 9n },
        },
      ],
    };
    const out = expectedToInput(v);
    expect(out).toBeInstanceOf(Map);
    expect((out as Map<unknown, unknown>).get('k')).toBe(9n);
  });

  it('refuses to bind a type value', () => {
    expect(() => expectedToInput({ kind: 'type', name: 'int' })).toThrow();
  });
});

describe('latin1ToBytes', () => {
  it('maps escape-decoded chars 1:1', () => {
    expect(Array.from(latin1ToBytes('\x00\xff'))).toEqual([0, 255]);
  });

  it('utf-8-encodes a literal multibyte char (code unit > 255)', () => {
    // '€' is U+20AC — a single code unit above latin1, UTF-8 = E2 82 AC.
    expect(Array.from(latin1ToBytes('€'))).toEqual([0xe2, 0x82, 0xac]);
  });
});

describe('loadSimpleTestFile — real basic.textproto', () => {
  const doc = parseTextproto(
    readFileSync(`${CORPUS_DIR}basic.textproto`, 'utf-8'),
  );
  const rows = loadSimpleTestFile('basic', doc);

  it('loads every row with file + section labels', () => {
    expect(rows.length).toBe(43);
    expect(rows[0]?.file).toBe('basic');
    expect(rows[0]?.section).toBe('self_eval_zeroish');
  });

  it('interprets the int-zero row', () => {
    const row = rows.find((r) => r.name === 'self_eval_int_zero');
    expect(row?.expr).toBe('0');
    expect(row?.matcher).toEqual({
      kind: 'value',
      value: { kind: 'int', value: 0n },
    });
  });

  it('interprets a bound-lookup row with type_env + bindings', () => {
    const row = rows.find((r) => r.name === 'self_eval_bound_lookup');
    expect(row?.expr).toBe('x');
    expect(row?.typeEnv).toEqual([
      { name: 'x', type: { kind: 'primitive', name: 'INT64' } },
    ]);
    expect(row?.bindings.get('x')).toBe(123n);
  });

  it('classifies an eval_error row', () => {
    const row = rows.find((r) => r.matcher.kind === 'evalError');
    expect(row).toBeDefined();
  });
});
