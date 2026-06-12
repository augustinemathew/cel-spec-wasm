// e2e `@host` host-function behaviors — a compiled `@host`-calling
// Program driving the eval binding's `cel_fn.*` trampoline with JS
// implementations, end-to-end.
//
// Ports the reachable envelope of the C++ `e2e/host_fn_test.cc` +
// `e2e/host_fn_type_matrix_test.cc` suites (the m21 host-call adapter):
// every IDL-expressible argument/return type the `@host` boundary
// reaches — bool / int / uint / double / string / bytes / null /
// Duration / Timestamp / proto(<fqn>) / list<T> / map<K,V> — plus arity
// 0 / 2, overloads, error returns, and the trampoline failure modes.
// The same `.celfn` declaration string is passed to BOTH
// `CompileOptions.fns` (so the call type-checks and the Program imports
// `cel_fn.<overload_id>`) and `Engine.defineFunction` (so the JS impl
// registers under that overload id).
//
// Structural exclusions (mirroring host_fn_type_matrix_test.cc's header):
// `type` / `optional<T>` / WKT wrappers / WKT struct types have no
// `.celfn` IDL spelling (compiler/celfn/function_library.cc:256-322) —
// pinned below as compile-reject negatives, not silent omissions.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { compile } from '@cel-wasm/compiler';
import { describe, expect, it } from 'vitest';

import { errorCode } from './helpers.js';

import { Engine, CelErrorCode, CelEvalError } from '@cel-wasm/eval';
import type {
  CelInput,
  CelValue,
  HostFunction,
  Instance,
} from '@cel-wasm/eval';
import { DescriptorSet, coerceObjectToMessage } from '@cel-wasm/eval/proto';

// The conformance corpus FileDescriptorSet supplies the proto types the
// message-arg cases reference (cel.expr.conformance.proto3.TestAllTypes).
const FDS_BYTES = new Uint8Array(
  readFileSync(
    fileURLToPath(
      new URL(
        '../../conformance/fixtures/cel_conformance_protos.fds',
        import.meta.url,
      ),
    ),
  ),
);
const TAT = 'cel.expr.conformance.proto3.TestAllTypes';

interface HostDef {
  readonly decl: string;
  readonly impl: HostFunction;
}

interface PlanOpts {
  readonly vars?: readonly { name: string; type: string }[];
  readonly withDescriptors?: boolean;
}

/** Compile `source` with the defs' decls, register the impls, plan. */
async function planHost(
  source: string,
  defs: readonly HostDef[],
  opts: PlanOpts = {},
): Promise<Instance> {
  const program = await compile(source, opts.vars ?? [], {
    fns: defs.map((d) => d.decl),
    ...(opts.withDescriptors === true ? { descriptorSetBytes: FDS_BYTES } : {}),
  });
  const engine = await Engine.create(
    opts.withDescriptors === true ? { descriptors: FDS_BYTES } : {},
  );
  for (const def of defs) {
    engine.defineFunction(def.decl, def.impl);
  }
  return engine.plan(program);
}

/** One-shot compile + register + plan + eval. */
async function evalHost(
  source: string,
  defs: readonly HostDef[],
  activation: Record<string, CelInput> = {},
  opts: PlanOpts = {},
): Promise<CelValue> {
  const instance = await planHost(source, defs, opts);
  return instance.eval(activation);
}

const first = (...args: CelValue[]): CelValue => args[0] ?? null;

describe('@host scalar round-trips (host_fn_type_matrix_test.cc scalar tiers)', () => {
  // Each row echoes a literal through a typed JS impl: the arg slot
  // decodes to the JS-natural CelValue, and the return encodes back.
  it.each<[string, string, CelValue]>([
    ['bool @host.echo_bool(bool b);', 'echo_bool(false)', false],
    ['int @host.echo_int(int x);', 'echo_int(-7)', -7n],
    ['uint @host.echo_uint(uint x);', 'echo_uint(0u)', 0n],
    [
      'uint @host.echo_uint(uint x);',
      'echo_uint(18446744073709551615u)',
      18446744073709551615n,
    ],
    ['double @host.echo_double(double x);', 'echo_double(1.5)', 1.5],
    ['string @host.echo_string(string s);', "echo_string('')", ''],
    [
      'string @host.echo_string(string s);',
      "echo_string('héllo 日本語')",
      'héllo 日本語',
    ],
    [
      'bytes @host.echo_bytes(bytes b);',
      "echo_bytes(b'\\x00\\x01')",
      Uint8Array.of(0, 1),
    ],
    ['bytes @host.echo_bytes(bytes b);', "echo_bytes(b'')", Uint8Array.of()],
  ])('%s / %s → %s', async (decl, source, want) => {
    expect(await evalHost(source, [{ decl, impl: first }])).toEqual(want);
  });

  it('int64 boundaries round-trip through a bound variable', async () => {
    // INT64_MIN is not spellable as a literal (the parser negates a
    // too-large positive); bind it instead, mirroring the C++ matrix's
    // bound-variable boundary rows.
    const decl = 'int @host.echo_int(int x);';
    const instance = await planHost('echo_int(x)', [{ decl, impl: first }], {
      vars: [{ name: 'x', type: 'int' }],
    });
    expect(instance.eval({ x: -9223372036854775808n })).toBe(
      -9223372036854775808n,
    );
    expect(instance.eval({ x: 9223372036854775807n })).toBe(
      9223372036854775807n,
    );
  });

  it('a uint-declared return composes with uint operators (CEL_UINT re-stamp)', async () => {
    // Regression: a JS bigint return encodes as CEL_INT; without the
    // declared-uint re-stamp (buildCelFnImports), `echo_uint(7u) + 1u`
    // failed with no-matching-overload (code 13).  C++ mirror:
    // HostCallContext::ReturnUint.
    const decl = 'uint @host.echo_uint(uint x);';
    expect(await evalHost('echo_uint(7u) + 1u', [{ decl, impl: first }])).toBe(
      8n,
    );
  });

  it('null arg is detected and null return surfaces (ContextNullArgDetected)', async () => {
    const isNull: HostDef = {
      decl: 'bool @host.is_null(null x);',
      impl: (...args: CelValue[]): CelValue => args[0] === null,
    };
    expect(await evalHost('is_null(null)', [isNull])).toBe(true);

    const makeNull: HostDef = {
      decl: 'null @host.make_null();',
      impl: (): CelValue => null,
    };
    expect(await evalHost('make_null()', [makeNull])).toBeNull();
  });
});

describe('@host temporal round-trips (DurationBoundary / TimestampBoundary)', () => {
  it('Duration arg decodes to a tagged record and returns intact', async () => {
    const seen: CelValue[] = [];
    const decl = 'Duration @host.echo_dur(Duration d);';
    const impl: HostFunction = (...args: CelValue[]): CelValue => {
      seen.push(args[0] ?? null);
      return args[0] ?? null;
    };
    const out = await evalHost("echo_dur(duration('90s'))", [{ decl, impl }]);
    expect(seen[0]).toEqual({ kind: 'duration', seconds: 90n, nanos: 0 });
    expect(out).toEqual({ kind: 'duration', seconds: 90n, nanos: 0 });
  });

  it('Timestamp round-trips (epoch + a civil date)', async () => {
    const decl = 'Timestamp @host.echo_ts(Timestamp t);';
    const out = await evalHost("echo_ts(timestamp('2001-01-01T00:00:00Z'))", [
      { decl, impl: first },
    ]);
    expect(out).toEqual({
      kind: 'timestamp',
      epochSeconds: 978307200n,
      nanos: 0,
    });
  });
});

describe('@host arity / composition (host_fn_test.cc call shapes)', () => {
  it('a 0-arg function', async () => {
    const decl = 'int @host.answer();';
    expect(
      await evalHost('answer()', [{ decl, impl: (): CelValue => 42n }]),
    ).toBe(42n);
  });

  it('a 2-arg function with mixed types', async () => {
    const decl = 'string @host.rep(string s, int n);';
    const impl: HostFunction = (...args: CelValue[]): CelValue =>
      (args[0] as string).repeat(Number(args[1] as bigint));
    expect(await evalHost("rep('ab', 3)", [{ decl, impl }])).toBe('ababab');
  });

  it('a host-fn result feeds downstream operators', async () => {
    const decl = 'int @host.addOne(int x);';
    const impl: HostFunction = (...args: CelValue[]): CelValue =>
      (args[0] as bigint) + 1n;
    const instance = await planHost('addOne(x) * 2', [{ decl, impl }], {
      vars: [{ name: 'x', type: 'int' }],
    });
    expect(instance.eval({ x: 20n })).toBe(42n);
  });

  it('the receiver (`this`) form dispatches as a method call', async () => {
    const decl = 'string @host.shout(this string s);';
    const impl: HostFunction = (...args: CelValue[]): CelValue =>
      (args[0] as string).toUpperCase();
    expect(await evalHost("'abc'.shout()", [{ decl, impl }])).toBe('ABC');
  });

  it('two overloads of one name register under distinct overload ids', async () => {
    // The overload-id synthesis is what disambiguates: `over_int` and
    // `over_string` are two `cel_fn.*` imports; a name-keyed registry
    // could not link this Program.
    const overInt: HostDef = {
      decl: 'int @host.over(int x);',
      impl: (): CelValue => 1n,
    };
    const overString: HostDef = {
      decl: 'int @host.over(string s);',
      impl: (): CelValue => 2n,
    };
    expect(await evalHost("over(0) + over('x')", [overInt, overString])).toBe(
      3n,
    );
  });
});

describe('@host aggregate args / returns (TypedListSize / TypedMapKey rows)', () => {
  it('a list<int> arg decodes to a JS array', async () => {
    const decl = 'int @host.lsum(list<int> xs);';
    const impl: HostFunction = (...args: CelValue[]): CelValue =>
      (args[0] as bigint[]).reduce((a, b) => a + b, 0n);
    expect(await evalHost('lsum([1, 2, 3])', [{ decl, impl }])).toBe(6n);
  });

  it('an empty list arg arrives empty', async () => {
    const decl = 'int @host.lsize(list<string> xs);';
    const impl: HostFunction = (...args: CelValue[]): CelValue =>
      BigInt((args[0] as CelValue[]).length);
    expect(await evalHost('lsize([])', [{ decl, impl }])).toBe(0n);
  });

  it('a list<int> return is indexable and sizeable in CEL', async () => {
    const decl = 'list<int> @host.three_ints();';
    const impl: HostFunction = (): CelValue => [10n, 20n, 30n];
    expect(await evalHost('three_ints()[1]', [{ decl, impl }])).toBe(20n);
    expect(await evalHost('size(three_ints())', [{ decl, impl }])).toBe(3n);
  });

  it('a map<string, int> arg decodes to a JS Map', async () => {
    const decl = 'int @host.mget(map<string, int> m);';
    const impl: HostFunction = (...args: CelValue[]): CelValue => {
      const m = args[0] as Map<CelValue, CelValue>;
      return (m.get('a') as bigint) + BigInt(m.size);
    };
    expect(await evalHost("mget({'a': 1, 'b': 2})", [{ decl, impl }])).toBe(3n);
  });

  it('a map<string, int> return is indexable in CEL', async () => {
    const decl = 'map<string, int> @host.ages();';
    const impl: HostFunction = (): CelValue =>
      new Map<CelValue, CelValue>([
        ['ada', 36n],
        ['grace', 85n],
      ]);
    expect(await evalHost("ages()['ada']", [{ decl, impl }])).toBe(36n);
  });
});

describe('@host message args (ContextProtoArgReadsRepeatedField et al.)', () => {
  it('a proto(<fqn>) arg decodes to the message field object', async () => {
    const decl = `string @host.name_of(proto(${TAT}) m);`;
    const impl: HostFunction = (...args: CelValue[]): CelValue => {
      const m = args[0] as Record<string, CelValue>;
      return m.single_string as string;
    };
    const instance = await planHost('name_of(x)', [{ decl, impl }], {
      vars: [{ name: 'x', type: TAT }],
      withDescriptors: true,
    });
    const type =
      DescriptorSet.fromFileDescriptorSet(FDS_BYTES).messageType(TAT);
    const msg = coerceObjectToMessage(type, {
      single_string: 'hi there',
      repeated_int32: [1, 2, 3],
      map_string_string: { k: 'v' },
    });
    expect(instance.eval({ x: msg as unknown as CelInput })).toBe('hi there');
  });

  it('repeated / map fields of a message arg decode to array / Map', async () => {
    const decl = `int @host.agg_sizes(proto(${TAT}) m);`;
    const impl: HostFunction = (...args: CelValue[]): CelValue => {
      const m = args[0] as Record<string, CelValue>;
      const repeated = m.repeated_int32 as CelValue[];
      const map = m.map_string_string as Map<CelValue, CelValue>;
      return BigInt(repeated.length) + BigInt(map.size);
    };
    const instance = await planHost('agg_sizes(x)', [{ decl, impl }], {
      vars: [{ name: 'x', type: TAT }],
      withDescriptors: true,
    });
    const type =
      DescriptorSet.fromFileDescriptorSet(FDS_BYTES).messageType(TAT);
    const msg = coerceObjectToMessage(type, {
      repeated_int32: [1, 2, 3],
      map_string_string: { a: 'x', b: 'y' },
    });
    expect(instance.eval({ x: msg as unknown as CelInput })).toBe(5n);
  });
});

describe('@host message returns (ContextProtoReturnPopulatesMultipleFields)', () => {
  // A `proto(<fqn>)`-declared RETURN: the impl hands back a protobufjs
  // Message (it carries its own `$type`), the `cel_fn` trampoline interns
  // it into the externref message table and stamps a CEL_MESSAGE slot —
  // the TS mirror of `HostCallContext::ReturnProto`
  // (eval/host_call_context.cc:549).
  const tatType =
    DescriptorSet.fromFileDescriptorSet(FDS_BYTES).messageType(TAT);
  const buildTat: HostDef = {
    decl: `proto(${TAT}) @host.build_tat(string s);`,
    impl: (...args: CelValue[]) =>
      coerceObjectToMessage(tatType, {
        single_string: args[0] as string,
        single_int64: 7,
      }),
  };
  const msgOpts: PlanOpts = { withDescriptors: true };

  it('a proto(<fqn>) RETURN surfaces as a CEL_MESSAGE (build_tat(s).single_string)', async () => {
    expect(
      await evalHost("build_tat('Ada').single_string", [buildTat], {}, msgOpts),
    ).toBe('Ada');
    // A second field off the same returned message (the multi-field shape
    // of the C++ ContextProtoReturnPopulatesMultipleFields).
    expect(
      await evalHost("build_tat('Ada').single_int64", [buildTat], {}, msgOpts),
    ).toBe(7n);
  });

  it('a message return feeds has() (proto3 presence)', async () => {
    expect(
      await evalHost(
        "has(build_tat('x').single_string)",
        [buildTat],
        {},
        msgOpts,
      ),
    ).toBe(true);
    // proto3 scalar presence: an empty string is "unset" (langdef.md
    // §field selection — has() on a proto3 scalar tests the default).
    expect(
      await evalHost(
        "has(build_tat('').single_string)",
        [buildTat],
        {},
        msgOpts,
      ),
    ).toBe(false);
  });

  it('a message return compares == to a message literal', async () => {
    expect(
      await evalHost(
        `build_tat('x') == ${TAT}{single_string: 'x', single_int64: 7}`,
        [buildTat],
        {},
        msgOpts,
      ),
    ).toBe(true);
    expect(
      await evalHost(
        `build_tat('x') == ${TAT}{single_string: 'y', single_int64: 7}`,
        [buildTat],
        {},
        msgOpts,
      ),
    ).toBe(false);
  });

  it('a plain-object return coerces against the declared FQN', async () => {
    // The plain-object arm mirrors the message-typed activation binding
    // (`messageBackingFrom`, eval/src/marshal.ts): the decl's FQN + the
    // Engine's descriptors drive coerceObjectToMessage, so a JS-natural
    // record works without constructing a protobufjs Message by hand.
    const objReturn: HostDef = {
      decl: `proto(${TAT}) @host.obj_tat();`,
      impl: () => ({ single_string: 'hi there', single_int64: 3n }),
    };
    expect(
      await evalHost('obj_tat().single_string', [objReturn], {}, msgOpts),
    ).toBe('hi there');
  });

  it('a Message return on an int-declared fn is host misuse → TRAP', async () => {
    // The pinned negative contract: returning a protobufjs message where
    // the decl says `int` throws in the trampoline (never a silent
    // re-intern as a host map), unwinding through the wasm frame as a
    // CelEvalError TRAP naming the offending fn.
    const liar: HostDef = {
      decl: 'int @host.fake_int();',
      impl: () => coerceObjectToMessage(tatType, { single_int64: 1 }),
    };
    const instance = await planHost('fake_int()', [liar]);
    let thrown: unknown;
    try {
      instance.eval({});
    } catch (err) {
      thrown = err;
    }
    expect(thrown).toBeInstanceOf(CelEvalError);
    expect((thrown as CelEvalError).code).toBe('TRAP');
    expect((thrown as CelEvalError).message).toContain(
      "host fn 'fake_int': the impl returned a protobufjs message",
    );
  });
});

describe('@host error / failure contracts', () => {
  it('an impl returning a CelError value propagates it as a value', async () => {
    // §A.4.5: CEL spec errors are values on the wire, never thrown.
    const decl = 'int @host.err_now();';
    const impl: HostFunction = (): CelValue => ({
      kind: 'error',
      code: CelErrorCode.HOST_ADAPTER_ERROR,
      message: 'host adapter error',
    });
    const out = await evalHost('err_now()', [{ decl, impl }]);
    expect(out).toMatchObject({
      kind: 'error',
      code: CelErrorCode.HOST_ADAPTER_ERROR,
    });
    // The error value is absorbed (not swallowed) by downstream operators.
    expect(errorCode(await evalHost('err_now() + 1', [{ decl, impl }]))).toBe(
      CelErrorCode.HOST_ADAPTER_ERROR,
    );
  });

  it('an impl that THROWS surfaces as CelEvalError TRAP (the host-failure contract)', async () => {
    // A thrown JS exception is a host failure, not a CEL spec error: it
    // unwinds through the wasm frame and `Instance.eval` wraps the trap.
    const decl = 'int @host.boom();';
    const impl: HostFunction = (): CelValue => {
      throw new Error('kaboom from JS');
    };
    const instance = await planHost('boom()', [{ decl, impl }]);
    let thrown: unknown;
    try {
      instance.eval({});
    } catch (err) {
      thrown = err;
    }
    expect(thrown).toBeInstanceOf(CelEvalError);
    expect((thrown as CelEvalError).code).toBe('TRAP');
    expect((thrown as CelEvalError).message).toContain('kaboom from JS');
  });

  it('planning a Program whose host fn is NOT registered rejects', async () => {
    // The Program imports `cel_fn.ghost`; with no registration the wasm
    // instantiate fails (a LinkError naming the missing import) — the
    // TS mirror of the C++ engine's linker define failure.
    const program = await compile('ghost()', [], {
      fns: ['int @host.ghost();'],
    });
    const engine = await Engine.create();
    await expect(engine.plan(program)).rejects.toThrow(/cel_fn.*ghost/);
  });

  it('calling an undeclared function fails type-check at compile', async () => {
    // Without the `fns` decl the reference is undeclared — the compile
    // gate that motivated wiring `CompileOptions.fns` through this suite.
    await expect(compile('myFn(1, 2)')).rejects.toThrow();
  });

  it('a `type`-typed arg has no .celfn spelling and rejects at compile', async () => {
    // Structural exclusion (host_fn_type_matrix_test.cc header): the
    // celfn IDL admits only the 12 IDL types
    // (compiler/celfn/function_library.cc:256-322); `type`, `optional<T>`
    // and the WKT struct types are not spellable as @host args.
    await expect(
      compile('f(1)', [], { fns: ['int @host.f(type t);'] }),
    ).rejects.toThrow();
  });
});
