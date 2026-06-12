// e2e message-typed-variable behaviors — a Program compiled against an
// in-memory FileDescriptorSet, evaluated over a bound proto message:
// scalar / nested / repeated / map field reads, `has()` presence, and
// message equality.
//
// Ports the C++ field-read envelope: `e2e/m2_test.cc` (Ident / kSelect /
// has() over a message-typed variable, proto3 default semantics) and
// `e2e/arena_message_aggregate_eq_test.cc` (message equality through
// `cel_message_eq`).  The proto types come from the conformance corpus
// FileDescriptorSet (`cel.expr.conformance.proto3.TestAllTypes`), passed
// to `compile()` as `descriptorSetBytes` and to `Engine.create` as
// `descriptors` — the whole pipeline runs in-process, no CLI.
//
// Activation shapes covered: a protobufjs message instance (carries its
// own `$type`) AND a plain JS object (coerced against the descriptor via
// the Program's type table — including the JSON-natural map-field shape
// the coercion normalizes).  Spec citations: doc/langdef.md §"Field
// Selection" (select + has()), §"Protocol Buffer Data Conversion"
// (proto3 defaults).

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { compile } from '@cel-wasm/compiler';
import { describe, expect, it } from 'vitest';

import { errorCode } from './helpers.js';

import { CelErrorCode, CelMarshalError, Engine } from '@cel-wasm/eval';
import type { CelInput, CelValue, Instance } from '@cel-wasm/eval';
import { DescriptorSet, coerceObjectToMessage } from '@cel-wasm/eval/proto';

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
const CONTAINER = 'cel.expr.conformance.proto3';
const TAT = `${CONTAINER}.TestAllTypes`;

const DESCRIPTORS = DescriptorSet.fromFileDescriptorSet(FDS_BYTES);

/** Build a protobufjs TestAllTypes message from a plain field object. */
function tat(fields: Record<string, unknown>): CelInput {
  const type = DESCRIPTORS.messageType(TAT);
  return coerceObjectToMessage(type, fields) as unknown as CelInput;
}

/** Compile `source` over `x: TestAllTypes` (+ optional `y`) and plan it. */
async function planOverX(
  source: string,
  extraVars: readonly { name: string; type: string }[] = [],
): Promise<Instance> {
  const program = await compile(
    source,
    [{ name: 'x', type: TAT }, ...extraVars],
    { container: CONTAINER, descriptorSetBytes: FDS_BYTES },
  );
  const engine = await Engine.create({ descriptors: FDS_BYTES });
  return engine.plan(program);
}

async function evalOverX(
  source: string,
  x: CelInput,
  extra: Record<string, CelInput> = {},
  extraVars: readonly { name: string; type: string }[] = [],
): Promise<CelValue> {
  const instance = await planOverX(source, extraVars);
  return instance.eval({ x, ...extra });
}

describe('scalar field reads (m2_test.cc Select; every CEL-visible scalar)', () => {
  // One bound message, one read per CEL scalar family — 32-bit and
  // 64-bit integer fields both surface as CEL int/uint (bigint), float
  // and double as number, per langdef §"Protocol Buffer Data Conversion".
  const bound = tat({
    single_int32: -32,
    single_int64: 99,
    single_uint32: 32,
    single_uint64: 18446744073709551615n,
    single_float: 1.5,
    single_double: 2.25,
    single_bool: true,
    single_string: 'héllo',
    single_bytes: Uint8Array.of(0, 1, 255),
  });

  it.each<[string, CelValue]>([
    ['x.single_int32', -32n],
    ['x.single_int64', 99n],
    ['x.single_uint32', 32n],
    ['x.single_uint64', 18446744073709551615n],
    ['x.single_float', 1.5],
    ['x.single_double', 2.25],
    ['x.single_bool', true],
    ['x.single_string', 'héllo'],
    ['x.single_bytes', Uint8Array.of(0, 1, 255)],
  ])('%s', async (source, want) => {
    expect(await evalOverX(source, bound)).toEqual(want);
  });

  it.each<[string, CelValue]>([
    // proto3 implicit presence: unset scalars read as their defaults
    // (m2_test.cc proto3-default rows; langdef §"Default Values").
    ['x.single_int64', 0n],
    ['x.single_uint64', 0n],
    ['x.single_double', 0],
    ['x.single_bool', false],
    ['x.single_string', ''],
    ['x.single_bytes', Uint8Array.of()],
  ])('unset %s → proto3 default', async (source, want) => {
    expect(await evalOverX(source, tat({}))).toEqual(want);
  });
});

describe('nested message reads (m2_test.cc nested-hop rows)', () => {
  it('x.single_nested_message.bb', async () => {
    expect(
      await evalOverX(
        'x.single_nested_message.bb',
        tat({ single_nested_message: { bb: 7 } }),
      ),
    ).toBe(7n);
  });

  it('an unset message field reads as the default instance (chained select → 0)', async () => {
    expect(await evalOverX('x.single_nested_message.bb', tat({}))).toBe(0n);
  });
});

describe('repeated field reads', () => {
  const bound = tat({ repeated_int32: [10, 20, 30], repeated_string: ['a'] });

  it('indexing: x.repeated_int32[1]', async () => {
    expect(await evalOverX('x.repeated_int32[1]', bound)).toBe(20n);
  });

  it('size(x.repeated_int32)', async () => {
    expect(await evalOverX('size(x.repeated_int32)', bound)).toBe(3n);
  });

  it('membership: 20 in x.repeated_int32', async () => {
    expect(await evalOverX('20 in x.repeated_int32', bound)).toBe(true);
    expect(await evalOverX('99 in x.repeated_int32', bound)).toBe(false);
  });

  it('out-of-bounds indexing yields the spec error value', async () => {
    expect(errorCode(await evalOverX('x.repeated_string[5]', bound))).toBe(
      CelErrorCode.INDEX_OUT_OF_BOUNDS,
    );
  });
});

describe('map field reads', () => {
  const bound = tat({
    map_string_string: { k: 'v', k2: 'v2' },
    map_int32_int32: { '5': 50 },
  });

  it("indexing: x.map_string_string['k']", async () => {
    expect(await evalOverX("x.map_string_string['k']", bound)).toBe('v');
  });

  it('an int-keyed map field indexes by int', async () => {
    expect(await evalOverX('x.map_int32_int32[5]', bound)).toBe(50n);
  });

  it('size + membership', async () => {
    expect(await evalOverX('size(x.map_string_string)', bound)).toBe(2n);
    expect(await evalOverX("'k2' in x.map_string_string", bound)).toBe(true);
    expect(await evalOverX("'nope' in x.map_string_string", bound)).toBe(false);
  });

  it('a missing key yields the spec no-such-key error value', async () => {
    expect(
      errorCode(await evalOverX("x.map_string_string['nope']", bound)),
    ).toBe(CelErrorCode.NO_SUCH_KEY);
  });
});

describe('has() presence (m2_test.cc Has; langdef §"Field Selection")', () => {
  it.each<[string, CelInput, boolean]>([
    // proto3 implicit-presence scalar: present iff != default.
    ['has(x.single_int64)', tat({ single_int64: 1 }), true],
    ['has(x.single_int64)', tat({}), false],
    ['has(x.single_string)', tat({ single_string: 'x' }), true],
    ['has(x.single_string)', tat({}), false],
    // message field: present iff set.
    [
      'has(x.single_nested_message)',
      tat({ single_nested_message: { bb: 0 } }),
      true,
    ],
    ['has(x.single_nested_message)', tat({}), false],
    // repeated / map: present iff non-empty.
    ['has(x.repeated_int32)', tat({ repeated_int32: [1] }), true],
    ['has(x.repeated_int32)', tat({}), false],
    ['has(x.map_string_string)', tat({ map_string_string: { a: 'b' } }), true],
    ['has(x.map_string_string)', tat({}), false],
  ])('%s over %o → %s', async (source, bound, want) => {
    expect(await evalOverX(source, bound)).toBe(want);
  });
});

describe('message equality (arena_message_aggregate_eq_test.cc)', () => {
  it('two equal bound messages compare equal (x == y)', async () => {
    const instance = await planOverX('x == y', [{ name: 'y', type: TAT }]);
    const a = tat({ single_int64: 5, single_string: 'hi' });
    const b = tat({ single_int64: 5, single_string: 'hi' });
    const c = tat({ single_int64: 6, single_string: 'hi' });
    expect(instance.eval({ x: a, y: b })).toBe(true);
    expect(instance.eval({ x: a, y: c })).toBe(false);
  });

  it('a bound message compares against a message literal', async () => {
    const instance = await planOverX('x == TestAllTypes{single_int64: 5}');
    expect(instance.eval({ x: tat({ single_int64: 5 }) })).toBe(true);
    expect(instance.eval({ x: tat({ single_int64: 6 }) })).toBe(false);
    // Extra populated fields break equality.
    expect(
      instance.eval({ x: tat({ single_int64: 5, single_bool: true }) }),
    ).toBe(false);
  });

  it('nested-message equality through a literal', async () => {
    const instance = await planOverX(
      'x.single_nested_message == TestAllTypes.NestedMessage{bb: 7}',
    );
    expect(
      instance.eval({ x: tat({ single_nested_message: { bb: 7 } }) }),
    ).toBe(true);
    expect(
      instance.eval({ x: tat({ single_nested_message: { bb: 8 } }) }),
    ).toBe(false);
  });
});

describe('activation shapes — plain object vs protobufjs message', () => {
  it('a plain JS object binds when the Program interns the type', async () => {
    // The literal `TestAllTypes{…}` interns the FQN into the Program's
    // `cel.abi` types table, which is what the plain-object marshal
    // resolves the descriptor through (eval/src/marshal.ts
    // messageTypeNameFor).  Field keys are the descriptor's snake_case
    // names; map fields take the JSON-natural record shape (normalized
    // by coerceObjectToMessage).
    const instance = await planOverX('x == TestAllTypes{single_int64: 5}');
    expect(instance.eval({ x: { single_int64: 5n } })).toBe(true);
    expect(instance.eval({ x: { single_int64: 6n } })).toBe(false);
  });

  it('a plain object with nested / repeated / map fields binds and reads', async () => {
    const instance = await planOverX(
      "x == TestAllTypes{} ? 'empty' : x.single_nested_message.bb == 7 " +
        "&& size(x.repeated_int32) == 2 && x.map_string_string['a'] == 'b' " +
        "? 'match' : 'no-match'",
    );
    expect(
      instance.eval({
        x: {
          single_nested_message: { bb: 7n },
          repeated_int32: [1n, 2n],
          map_string_string: { a: 'b' },
        },
      }),
    ).toBe('match');
  });

  it('a plain object binding WITHOUT a type-table entry throws CelMarshalError (the documented wire limitation)', async () => {
    // The `cel.abi` VariableEntry deliberately carries no message FQN
    // ("Rich type info is NOT on the wire", abi/cel_abi.proto:52), so a
    // Program that never names the type (no message literal) cannot
    // coerce a plain object — the marshal throws, and the supported
    // shape is a protobufjs message (which carries its own $type).
    const instance = await planOverX('x.single_int64');
    expect(() => instance.eval({ x: { single_int64: 5n } })).toThrow(
      CelMarshalError,
    );
    // The same Program + a protobufjs message binding works.
    expect(instance.eval({ x: tat({ single_int64: 5 }) })).toBe(5n);
  });
});
