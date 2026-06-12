// Unit tests for the `.celfn` host-decl → overload-id synthesis.
//
// The expected ids mirror the compiler's `SynthesiseOverloadId` +
// `CelfnType::Argkind` (compiler/celfn/function_library.cc:180 / :30);
// each table row's decl string is the exact form the C++ e2e suites pass
// to `Compiler::Builder::AddFunction` (e2e/host_fn_type_matrix_test.cc),
// so the ids here are the ones a compiled Program actually imports under
// `cel_fn.*` (verified end-to-end in `eval/e2e/host-fns.test.ts`).

import { describe, expect, it } from 'vitest';

import { CelFnDeclError, hostFnDecl, parseHostFnDecl } from './celfn-decl.js';

describe('parseHostFnDecl — overload-id synthesis (the full argkind matrix)', () => {
  it.each([
    // Every scalar argkind (function_library.cc:30-49).
    ['bool @host.echo_bool(bool b);', 'echo_bool', 'echo_bool_bool'],
    ['int @host.echo_int(int x);', 'echo_int', 'echo_int_int'],
    ['uint @host.echo_uint(uint x);', 'echo_uint', 'echo_uint_uint'],
    [
      'double @host.echo_double(double x);',
      'echo_double',
      'echo_double_double',
    ],
    [
      'string @host.echo_string(string s);',
      'echo_string',
      'echo_string_string',
    ],
    ['bytes @host.echo_bytes(bytes b);', 'echo_bytes', 'echo_bytes_bytes'],
    ['bool @host.is_null(null x);', 'is_null', 'is_null_null'],
    // Duration / Timestamp lowercase in the id (function_library.cc:46-49).
    ['Duration @host.echo_dur(Duration d);', 'echo_dur', 'echo_dur_duration'],
    ['Timestamp @host.echo_ts(Timestamp t);', 'echo_ts', 'echo_ts_timestamp'],
    // Aggregates (function_library.cc:50-57).
    ['int @host.lsize(list<int> xs);', 'lsize', 'lsize_list_int'],
    [
      'int @host.lll_outer_size(list<list<int>> xs);',
      'lll_outer_size',
      'lll_outer_size_list_list_int',
    ],
    ['int @host.mlu(map<string, int> m);', 'mlu', 'mlu_map_string_int'],
    [
      'int @host.lmsz(list<map<string, int>> xs);',
      'lmsz',
      'lmsz_list_map_string_int',
    ],
    [
      'int @host.mm(map<string, map<string, int>> m);',
      'mm',
      'mm_map_string_map_string_int',
    ],
    // proto(<fqn>) → message_<fqn with dots → underscores>
    // (function_library.cc:58-60).
    [
      'int @host.tag_count(proto(celwasm.testdata.Customer) c);',
      'tag_count',
      'tag_count_message_celwasm_testdata_Customer',
    ],
    [
      'proto(celwasm.testdata.Customer) @host.build_customer(string n);',
      'build_customer',
      'build_customer_string',
    ],
    // Arity 0 and 2+.
    ['list<int> @host.three_ints();', 'three_ints', 'three_ints'],
    ['int @host.add2(int a, int b);', 'add2', 'add2_int_int'],
    [
      'string @host.rep(string s, int n, bool caps);',
      'rep',
      'rep_string_int_bool',
    ],
    // `this` receiver modifier does not change the id (the receiver is an
    // ordinary first param in SynthesiseOverloadId).
    ['string @host.shout(this string s);', 'shout', 'shout_string'],
    // Trailing semicolon is optional; whitespace is free-form.
    ['int @host.addOne( int x )', 'addOne', 'addOne_int'],
  ])('%s → %s / %s', (decl, name, overloadId) => {
    expect(parseHostFnDecl(decl)).toMatchObject({ name, overloadId });
  });

  it('flags a uint-declared return (the CEL_UINT re-stamp signal)', () => {
    expect(parseHostFnDecl('uint @host.echo_uint(uint x);').returnsUint).toBe(
      true,
    );
    expect(parseHostFnDecl('int @host.echo_int(int x);').returnsUint).toBe(
      false,
    );
  });

  it('captures the FQN of a proto(<fqn>)-declared return', () => {
    // The CEL_MESSAGE intern signal (HostCallContext::ReturnProto,
    // eval/host_call_context.cc:549).  The dotted FQN is captured during
    // the parse, not reversed from the argkind spelling — an underscored
    // package segment (`a.b_c.User` → `message_a_b_c_User`) makes the
    // reversal lossy.
    expect(
      parseHostFnDecl('proto(a.b_c.User) @host.get_user(int id);')
        .returnMessageFqn,
    ).toBe('a.b_c.User');
  });

  it.each([
    // Non-message returns carry no FQN — including aggregate returns
    // whose ELEMENT is a message (those are list / map returns, not
    // message returns) and a proto-typed PARAMETER on a scalar return.
    ['int @host.f();'],
    ['list<proto(a.B)> @host.f();'],
    ['map<string, proto(a.B)> @host.f();'],
    ['string @host.f(proto(a.B) m);'],
  ])('%s → returnMessageFqn undefined', (decl) => {
    expect(parseHostFnDecl(decl).returnMessageFqn).toBeUndefined();
  });
});

describe('parseHostFnDecl — rejects malformed / out-of-scope decls', () => {
  it.each([
    // A TS-style signature is not a .celfn decl.
    ['my_fn(int): int'],
    // No leading return type.
    ['@host.addOne(int x);'],
    // Missing the @ prefix (Celfn.g4 has a diagnostic-only bareHostDecl
    // production for exactly this shape).
    ['int host.addOne(int x);'],
    // Non-host backends are out of binding scope by design.
    ['int @component.addOne(int x);'],
    ['int @native.addOne(int x) = x + 1;'],
    // Unknown type keyword.
    ['float @host.f(float x);'],
    // Invalid map key kind (Celfn.g4 mapKeyType admits bool/int/uint/string).
    ['int @host.f(map<double, int> m);'],
    // Param name is required (Celfn.g4 param: "this"? type Identifier).
    ['int @host.f(int);'],
    // Unbalanced generics / trailing garbage.
    ['int @host.f(list<int xs);'],
    ['int @host.f(int x); extra'],
    // Empty / punctuation-only.
    [''],
    ['(int): int'],
  ])('rejects %s', (decl) => {
    expect(() => parseHostFnDecl(decl)).toThrow(CelFnDeclError);
  });
});

describe('hostFnDecl — bare-identifier escape hatch', () => {
  it('takes a bare identifier verbatim as the overload id', () => {
    expect(hostFnDecl('addOne_int')).toEqual({
      name: 'addOne_int',
      overloadId: 'addOne_int',
      returnsUint: false,
      returnMessageFqn: undefined,
    });
  });

  it('routes a full decl through the parser', () => {
    expect(hostFnDecl('int @host.addOne(int x);')).toEqual({
      name: 'addOne',
      overloadId: 'addOne_int',
      returnsUint: false,
      returnMessageFqn: undefined,
    });
  });

  it('still rejects a non-identifier non-decl string', () => {
    expect(() => hostFnDecl('(int): int')).toThrow(CelFnDeclError);
  });
});
