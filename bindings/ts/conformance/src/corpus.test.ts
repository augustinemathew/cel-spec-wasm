// Tests for the textproto → typed-corpus interpreter.  The matrix pins
// each `cel.expr.Value` matcher kind, the type_env decl shapes, and the
// binding-value lowering against synthetic textproto fragments, plus a
// round-trip against the real `basic.textproto` file.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import {
  CorpusError,
  expectedToInput,
  interpretValue,
  loadSimpleTestFile,
  type ExpectedValue,
  type MessageBindingBuilder,
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

describe('expectedToInput — object (proto-message) bindings', () => {
  // An `object` ExpectedValue as interpretValue produces it from the
  // textproto `object_value` Any expansion.
  function objectValue(): ExpectedValue {
    return interpretValue(
      valueMsg('object_value: { [type.googleapis.com/foo.Bar] { f: 1 } }'),
    );
  }

  it('lowers an object value through the supplied builder', () => {
    const built = { f: 1n };
    let gotFqn: string | undefined;
    const builder: MessageBindingBuilder = (fqn, message) => {
      gotFqn = fqn;
      expect(message.fields.has('f')).toBe(true);
      return built;
    };
    expect(expectedToInput(objectValue(), builder)).toBe(built);
    expect(gotFqn).toBe('foo.Bar');
  });

  it('lowers an object value nested in a list through the builder', () => {
    const obj = objectValue();
    if (obj.kind !== 'object') {
      throw new Error('expected an object value');
    }
    const v: ExpectedValue = { kind: 'list', elements: [obj] };
    const builder: MessageBindingBuilder = (fqn) => fqn;
    expect(expectedToInput(v, builder)).toEqual(['foo.Bar']);
  });

  it('throws a CorpusError without a builder (no descriptor set)', () => {
    expect(() => expectedToInput(objectValue())).toThrow(CorpusError);
    expect(() => expectedToInput(objectValue())).toThrow(
      /cannot bind a object value/,
    );
  });

  it('wraps a builder failure (unknown FQN) in a CorpusError naming the type', () => {
    const builder: MessageBindingBuilder = (fqn) => {
      throw new Error(`no such type: ${fqn}`);
    };
    expect(() => expectedToInput(objectValue(), builder)).toThrow(CorpusError);
    expect(() => expectedToInput(objectValue(), builder)).toThrow(
      /cannot build message 'foo\.Bar': no such type: foo\.Bar/,
    );
  });
});

describe('loadSimpleTestFile — object_value bindings', () => {
  const ROW = `
    section: {
      name: "s"
      test: {
        name: "bind_msg"
        expr: "x.f"
        type_env: {
          name: "x"
          ident: { type: { message_type: "foo.Bar" } }
        }
        bindings: {
          key: "x"
          value: {
            value: {
              object_value: {
                [type.googleapis.com/foo.Bar] { f: 7 }
              }
            }
          }
        }
        value: { int64_value: 7 }
      }
    }`;

  it('binds the built message when a builder is supplied', () => {
    const built = { f: 7n };
    const rows = loadSimpleTestFile('synthetic', parseTextproto(ROW), () => {
      return built;
    });
    expect(rows.length).toBe(1);
    expect(rows[0]?.unsupportedBindingReason).toBeUndefined();
    expect(rows[0]?.bindings.get('x')).toBe(built);
  });

  it('marks the row unsupported without a builder', () => {
    const rows = loadSimpleTestFile('synthetic', parseTextproto(ROW));
    expect(rows.length).toBe(1);
    expect(rows[0]?.bindings.get('x')).toBeUndefined();
    expect(rows[0]?.unsupportedBindingReason).toMatch(
      /binding 'x': cannot bind a object value/,
    );
  });

  it('marks the row unsupported (not a crash) when the builder throws', () => {
    const rows = loadSimpleTestFile('synthetic', parseTextproto(ROW), () => {
      throw new Error('foo.Bar is not in the descriptor set');
    });
    expect(rows.length).toBe(1);
    expect(rows[0]?.unsupportedBindingReason).toMatch(
      /binding 'x': cannot build message 'foo\.Bar'/,
    );
  });
});

describe('interpretValue — string vs bytes byte handling', () => {
  it('reads a bytes_value as raw escape bytes (UTF-8 of ÿ)', () => {
    // `\303\277` is the UTF-8 of ÿ; in a bytes field it stays two octets.
    const v = interpretValue(valueMsg('bytes_value: "\\303\\277"'));
    expect(v.kind).toBe('bytes');
    if (v.kind === 'bytes') {
      expect(Array.from(v.value)).toEqual([0xc3, 0xbf]);
    }
  });

  it('reads a string_value whose escape bytes are UTF-8 (ÿ)', () => {
    // The same bytes in a string field decode as UTF-8 → the char ÿ.
    expect(interpretValue(valueMsg('string_value: "\\303\\277"'))).toEqual({
      kind: 'string',
      value: 'ÿ',
    });
  });

  it('reads a string_value with a literal multibyte char', () => {
    expect(interpretValue(valueMsg('string_value: "rôle"'))).toEqual({
      kind: 'string',
      value: 'rôle',
    });
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
