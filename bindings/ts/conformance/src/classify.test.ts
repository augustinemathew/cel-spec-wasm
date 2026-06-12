// Tests for the pre-compile scope classifier + type renderer + ext-lib
// detection.  Mirrors the C++ `ScopeReject` decisions; the matrix pins
// every skip category the pre-compile gate can produce.

import { describe, expect, it } from 'vitest';

import { classifyScope, looksLikeExtension, renderType } from './classify.js';
import type { DeclaredType, SimpleTest } from './corpus.js';

function row(overrides: Partial<SimpleTest>): SimpleTest {
  return {
    file: 't',
    section: 's',
    name: 'n',
    expr: '1',
    container: '',
    disableCheck: false,
    checkOnly: false,
    typeEnv: [],
    bindings: new Map(),
    unsupportedBindingReason: undefined,
    matcher: { kind: 'value', value: { kind: 'int', value: 1n } },
    ...overrides,
  };
}

describe('renderType', () => {
  it('renders primitives', () => {
    const cases: [string, string][] = [
      ['BOOL', 'bool'],
      ['INT64', 'int'],
      ['UINT64', 'uint'],
      ['DOUBLE', 'double'],
      ['STRING', 'string'],
      ['BYTES', 'bytes'],
    ];
    for (const [enumName, rendered] of cases) {
      expect(renderType({ kind: 'primitive', name: enumName })).toBe(rendered);
    }
  });

  it('renders the time well-known types', () => {
    expect(
      renderType({ kind: 'wellKnown', name: 'google.protobuf.Duration' }),
    ).toBe('duration');
    expect(
      renderType({ kind: 'wellKnown', name: 'google.protobuf.Timestamp' }),
    ).toBe('timestamp');
  });

  it('renders nested list / map', () => {
    const t: DeclaredType = {
      kind: 'map',
      key: { kind: 'primitive', name: 'STRING' },
      value: { kind: 'list', elem: { kind: 'primitive', name: 'INT64' } },
    };
    expect(renderType(t)).toBe('map<string, list<int>>');
  });

  it('renders a non-WKT message type to its fully-qualified name', () => {
    expect(renderType({ kind: 'message', fqn: 'cel.expr.X' })).toBe(
      'cel.expr.X',
    );
  });

  it('returns undefined for an unsupported decl', () => {
    expect(
      renderType({ kind: 'unsupported', reason: 'function decl' }),
    ).toBeUndefined();
  });

  it('renders a non-time well-known type as its message FQN', () => {
    // The pass-through the C++ harness uses
    // (conformance/binding_marshal.cc::CelTypeFromProtoType): wrappers /
    // Struct / Value / ListValue / Any declare as message FQNs and
    // resolve against the descriptor set.
    expect(
      renderType({ kind: 'wellKnown', name: 'google.protobuf.Value' }),
    ).toBe('google.protobuf.Value');
    expect(
      renderType({ kind: 'wellKnown', name: 'google.protobuf.Int32Value' }),
    ).toBe('google.protobuf.Int32Value');
  });

  it('returns undefined for google.protobuf.Any (out of scope, §A.3)', () => {
    expect(
      renderType({ kind: 'wellKnown', name: 'google.protobuf.Any' }),
    ).toBeUndefined();
  });
});

describe('classifyScope', () => {
  it('skips a disable_check row', () => {
    const d = classifyScope(row({ disableCheck: true }));
    expect(d.kind === 'skip' && d.category).toBe('disable_check');
  });

  it('pre-skips a spec-unimplemented strong-enum row as spec_unimpl', () => {
    // Mirrors the C++ harness's per-row list
    // (conformance/runner.cc::IsSpecUnimplSection, cel-cpp issues/119).
    const d = classifyScope(
      row({ file: 'enums', section: 'strong_proto3', name: 'convert_string' }),
    );
    expect(d.kind === 'skip' && d.category).toBe('spec_unimpl');
  });

  it('does NOT pre-skip strong-enum rows off the spec-unimpl list', () => {
    // Rows in strong_proto2/3 not on the list (e.g. the error-matcher
    // conversions) stay live — they pass via compile/eval.
    const d = classifyScope(
      row({
        file: 'enums',
        section: 'strong_proto2',
        name: 'convert_string_bad',
        matcher: { kind: 'evalError' },
      }),
    );
    expect(d.kind).toBe('proceed');
  });

  it('does NOT pre-skip listed names outside the enums strong sections', () => {
    const d = classifyScope(
      row({ file: 'basic', section: 'self_eval', name: 'convert_string' }),
    );
    expect(d.kind).toBe('proceed');
  });

  it('skips a check_only row', () => {
    const d = classifyScope(row({ checkOnly: true }));
    expect(d.kind === 'skip' && d.category).toBe('check_only');
  });

  it('skips an unknown matcher as envelope', () => {
    const d = classifyScope(
      row({ matcher: { kind: 'unsupported', reason: 'unknown matcher' } }),
    );
    expect(d.kind === 'skip' && d.category).toBe('envelope');
  });

  it('proceeds an object_value matcher (compared against descriptors)', () => {
    const d = classifyScope(
      row({
        matcher: {
          kind: 'value',
          value: {
            kind: 'object',
            fqn: 'cel.expr.conformance.proto3.TestAllTypes',
            message: { kind: 'message', fields: new Map() },
          },
        },
      }),
    );
    expect(d.kind).toBe('proceed');
  });

  it('proceeds a type_value matcher (compared via the type comparator)', () => {
    const d = classifyScope(
      row({
        matcher: { kind: 'value', value: { kind: 'type', name: 'int' } },
      }),
    );
    expect(d.kind).toBe('proceed');
  });

  it('renders a non-WKT message type_env decl to its FQN', () => {
    const d = classifyScope(
      row({
        typeEnv: [{ name: 'm', type: { kind: 'message', fqn: 'cel.expr.X' } }],
      }),
    );
    expect(d.kind === 'proceed' && d.compileVars[0]?.type).toBe('cel.expr.X');
  });

  it('skips a row with an unsupported binding value', () => {
    const d = classifyScope(
      row({
        unsupportedBindingReason: "binding 'm': cannot bind a object value",
      }),
    );
    expect(d.kind === 'skip' && d.category).toBe('bindings');
  });

  it('proceeds with rendered vars for an in-scope row', () => {
    const d = classifyScope(
      row({
        typeEnv: [{ name: 'x', type: { kind: 'primitive', name: 'INT64' } }],
      }),
    );
    expect(d.kind).toBe('proceed');
    if (d.kind === 'proceed') {
      expect(d.compileVars).toEqual([{ name: 'x', type: 'int' }]);
    }
  });

  it('proceeds for an evalError matcher', () => {
    const d = classifyScope(row({ matcher: { kind: 'evalError' } }));
    expect(d.kind).toBe('proceed');
  });

  it('proceeds for a boolTrue matcher', () => {
    const d = classifyScope(row({ matcher: { kind: 'boolTrue' } }));
    expect(d.kind).toBe('proceed');
  });
});

describe('looksLikeExtension', () => {
  it('detects a namespace call', () => {
    expect(looksLikeExtension('math.greatest(1, 2)')).toBe(true);
    expect(looksLikeExtension('cel.bind(x, 1, x)')).toBe(true);
  });

  it('detects a receiver-method call', () => {
    expect(looksLikeExtension("'tacocat'.charAt(3)")).toBe(true);
    expect(looksLikeExtension("'a'.upperAscii()")).toBe(true);
  });

  it('detects optionals syntax', () => {
    expect(looksLikeExtension('{}.?c')).toBe(true);
    expect(looksLikeExtension('m[?0]')).toBe(true);
  });

  it('does not flag plain expressions', () => {
    expect(looksLikeExtension('1 + 2')).toBe(false);
    expect(looksLikeExtension("size('abc')")).toBe(false);
    expect(looksLikeExtension('[1, 2, 3].exists(e, e > 1)')).toBe(false);
  });
});
