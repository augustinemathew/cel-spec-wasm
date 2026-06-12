// End-to-end eval against the golden compiled-Program fixtures.
//
// This is the keystone assertion of the eval binding: load every
// committed fixture under `eval/fixtures/` (driven off `manifest.json`),
// plan it through the `Engine`, eval it with the fixture's tagged
// `activation`, and assert the decoded result deep-equals the fixture's
// tagged `expected`.  A green run proves the whole pure-TS path —
// instantiate → marshal → `$eval` → decode — against real C++ compiler
// output, with no wasmtime and no C++ at eval time.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.5,
//       §A.7.  Fixture / tagged-value format: `eval/fixtures/README.md`.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import * as protobuf from 'protobufjs';
import { describe, expect, it } from 'vitest';

import { decodeAbi } from './abi.js';
import { Engine } from './engine.js';
import { ExternrefTable } from './externref.js';
import { buildCelFnImports } from './instance.js';
import { coerceObjectToMessage } from './proto/backing.js';
import { DescriptorSet } from './proto/descriptors.js';
import { encodeCelValue, resolveCelValue } from './resolving-codec.js';
import type { CodecEnv } from './resolving-codec.js';
import { CEL_VALUE_KIND_OFFSET, CEL_VALUE_SIZE, CelKind } from './types.js';
import type { CelInput, CelValue, HostFunction, Program } from './types.js';

const FIXTURES_DIR = fileURLToPath(new URL('../fixtures/', import.meta.url));

// ── Tagged value forms (eval/fixtures/README.md) ────────────────────

type TaggedValue =
  | { kind: 'null' }
  | { kind: 'bool'; value: boolean }
  | { kind: 'int'; value: string }
  | { kind: 'uint'; value: string }
  | { kind: 'double'; value: number }
  | { kind: 'string'; value: string }
  | { kind: 'bytes'; bytes: number[] }
  | { kind: 'list'; elements: TaggedValue[] }
  | { kind: 'map'; entries: [TaggedValue, TaggedValue][] }
  | { kind: 'error'; code: number };

interface Fixture {
  readonly name: string;
  readonly expr: string;
  readonly compileVars: string[];
  readonly activation: Record<string, TaggedValue>;
  readonly expected: TaggedValue;
}

interface Manifest {
  readonly fixtures: Fixture[];
}

function loadManifest(): Manifest {
  const text = readFileSync(`${FIXTURES_DIR}manifest.json`, 'utf-8');
  return JSON.parse(text) as Manifest;
}

function loadProgram(name: string): Program {
  const wasm = new Uint8Array(readFileSync(`${FIXTURES_DIR}${name}.wasm`));
  return { wasm, abi: decodeAbi(wasm) };
}

// ── Tagged → CelInput (the activation conversion) ───────────────────

function taggedToInput(tag: TaggedValue): CelInput {
  switch (tag.kind) {
    case 'null':
      return null;
    case 'bool':
      return tag.value;
    case 'int':
    case 'uint':
      return BigInt(tag.value);
    case 'double':
      return tag.value;
    case 'string':
      return tag.value;
    case 'bytes':
      return Uint8Array.from(tag.bytes);
    case 'list':
      return tag.elements.map(taggedToInput);
    case 'map': {
      const map = new Map<CelInput, CelInput>();
      for (const [k, v] of tag.entries) {
        map.set(taggedToInput(k), taggedToInput(v));
      }
      return map;
    }
    case 'error':
      throw new Error('an error tagged value is never an activation input');
  }
}

function activationOf(fixture: Fixture): Record<string, CelInput> {
  const out: Record<string, CelInput> = {};
  for (const [name, tag] of Object.entries(fixture.activation)) {
    out[name] = taggedToInput(tag);
  }
  return out;
}

// ── CelValue ↔ tagged comparator ────────────────────────────────────

/**
 * Assert a decoded {@link CelValue} matches a tagged `expected`.  The
 * int / uint distinction is not observable on a decoded `bigint` (both
 * decode to `bigint`), so the comparator checks the numeric value for
 * both; every other kind is matched structurally.
 */
function expectMatches(actual: CelValue, expected: TaggedValue): void {
  switch (expected.kind) {
    case 'null':
      expect(actual).toBeNull();
      return;
    case 'bool':
      expect(actual).toBe(expected.value);
      return;
    case 'int':
    case 'uint':
      expect(typeof actual).toBe('bigint');
      expect((actual as bigint).toString()).toBe(expected.value);
      return;
    case 'double':
      expect(actual).toBe(expected.value);
      return;
    case 'string':
      expect(actual).toBe(expected.value);
      return;
    case 'bytes':
      expect(actual).toBeInstanceOf(Uint8Array);
      expect([...(actual as Uint8Array)]).toEqual(expected.bytes);
      return;
    case 'list':
      expect(Array.isArray(actual)).toBe(true);
      expectListMatches(actual as CelValue[], expected.elements);
      return;
    case 'map':
      expect(actual).toBeInstanceOf(Map);
      expectMapMatches(actual as Map<CelValue, CelValue>, expected.entries);
      return;
    case 'error':
      expectErrorMatches(actual, expected.code);
      return;
  }
}

function expectListMatches(actual: CelValue[], expected: TaggedValue[]): void {
  expect(actual.length).toBe(expected.length);
  for (let i = 0; i < expected.length; i++) {
    const element = actual[i];
    const tag = expected[i];
    expect(element).toBeDefined();
    expect(tag).toBeDefined();
    if (element !== undefined && tag !== undefined) {
      expectMatches(element, tag);
    }
  }
}

function expectMapMatches(
  actual: Map<CelValue, CelValue>,
  expected: [TaggedValue, TaggedValue][],
): void {
  expect(actual.size).toBe(expected.length);
  for (const [keyTag, valTag] of expected) {
    const key = taggedToScalar(keyTag);
    expect(actual.has(key)).toBe(true);
    const value = actual.get(key);
    expect(value).toBeDefined();
    if (value !== undefined) {
      expectMatches(value, valTag);
    }
  }
}

/** A scalar tagged value as the JS-natural key a decoded Map uses. */
function taggedToScalar(tag: TaggedValue): CelValue {
  switch (tag.kind) {
    case 'bool':
      return tag.value;
    case 'int':
    case 'uint':
      return BigInt(tag.value);
    case 'double':
      return tag.value;
    case 'string':
      return tag.value;
    default:
      throw new Error(`tagged value '${tag.kind}' is not a scalar map key`);
  }
}

function expectErrorMatches(actual: CelValue, code: number): void {
  expect(typeof actual).toBe('object');
  expect(actual).not.toBeNull();
  const err = actual as { kind?: unknown; code?: unknown };
  expect(err.kind).toBe('error');
  expect(err.code).toBe(code);
}

// ── The fixture suite ───────────────────────────────────────────────

describe('Instance.eval — golden Program fixtures', () => {
  const manifest = loadManifest();

  it('exercises all 27 committed fixtures', () => {
    expect(manifest.fixtures.length).toBe(27);
  });

  it.each(manifest.fixtures.map((f) => [f.name, f] as const))(
    'evaluates %s',
    async (_name, fixture) => {
      const engine = await Engine.create();
      const instance = await engine.plan(loadProgram(fixture.name));
      const result = instance.eval(activationOf(fixture));
      expectMatches(result, fixture.expected);
    },
  );
});

describe('Instance.eval — repeated eval reuses one Instance', () => {
  it('re-evaluates a variable Program with two activations', async () => {
    const engine = await Engine.create();
    const instance = await engine.plan(loadProgram('var_int_identity'));
    const a = instance.eval({ x: 7n });
    const b = instance.eval({ x: 99n });
    expect(a).toBe(7n);
    expect(b).toBe(99n);
  });
});

// ── Host-function round-trip (the cel_fn trampoline) ────────────────
//
// The §A.5 done-when calls for a registered host fn to round-trip.  No
// compiled `@host` Program is reachable in the TS test toolchain yet (the
// fixtures are scalar / aggregate; building one needs the compiler binding
// + the `.celfn` IDL), so this proves the trampoline the engine wires for
// each `cel_fn.*` import: decode the argument slots through the resolving
// codec, call the JS impl, encode the result back into the out slot.
//
// e2e GAP: an end-to-end test that drives a real `cel_fn.*` import from
// inside a compiled Program is a focused follow-up gated on a host-fn
// fixture; the trampoline contract is unit-pinned here meanwhile.

function makeCodecHarness(): { env: CodecEnv; refs: ExternrefTable } {
  const buffer = new ArrayBuffer(4096);
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  const refs = new ExternrefTable();
  let cursor = 1024;
  const env: CodecEnv = {
    view: () => view,
    bytes: () => bytes,
    refs,
    arenaAlloc: (n: number) => {
      const ptr = cursor;
      cursor += (n + 7) & ~7;
      return ptr;
    },
  };
  return { env, refs };
}

describe('buildCelFnImports — host-fn round-trip', () => {
  it('decodes args, calls the impl, and encodes the result', () => {
    const { env } = makeCodecHarness();
    const calls: CelValue[][] = [];
    const impl: HostFunction = (...args: CelValue[]): CelValue => {
      calls.push(args);
      const [a, b] = args;
      return (a as bigint) + (b as bigint);
    };
    const imports = buildCelFnImports(
      env,
      new Map([
        ['add', { impl, returnsUint: false, returnMessageFqn: undefined }],
      ]),
    );

    // Slots: out @0, arg0 @24, arg1 @48.
    const outSlot = 0;
    const arg0 = CEL_VALUE_SIZE;
    const arg1 = CEL_VALUE_SIZE * 2;
    encodeCelValue(env, arg0, 10n);
    encodeCelValue(env, arg1, 32n);

    const add = imports.add;
    expect(add).toBeDefined();
    add?.(outSlot, arg0, arg1);

    // The impl saw two decoded INT args, and the out slot decodes to 42.
    expect(calls).toEqual([[10n, 32n]]);
    expect(env.view().getUint32(outSlot + CEL_VALUE_KIND_OFFSET, true)).toBe(
      CelKind.INT,
    );
    expect(resolveCelValue(env, outSlot)).toBe(42n);
  });

  it('round-trips a string-returning host fn through the arena', () => {
    const { env } = makeCodecHarness();
    const impl: HostFunction = (...args: CelValue[]): CelValue => {
      const who = args[0];
      return `hello ${typeof who === 'string' ? who : ''}`;
    };
    const imports = buildCelFnImports(
      env,
      new Map([
        ['greet', { impl, returnsUint: false, returnMessageFqn: undefined }],
      ]),
    );
    encodeCelValue(env, CEL_VALUE_SIZE, 'world');
    imports.greet?.(0, CEL_VALUE_SIZE);
    expect(env.view().getUint32(CEL_VALUE_KIND_OFFSET, true)).toBe(
      CelKind.STRING,
    );
    expect(resolveCelValue(env, 0)).toBe('hello world');
  });

  it('builds no imports for an empty registry', () => {
    const { env } = makeCodecHarness();
    expect(Object.keys(buildCelFnImports(env, new Map()))).toEqual([]);
  });

  it('re-stamps a uint-declared bigint return as CEL_UINT', () => {
    // A JS bigint carries no int/uint distinction, so encodeCelValue
    // stamps CEL_INT; a `uint`-declared return must surface as CEL_UINT
    // (the C++ mirror is HostCallContext::ReturnUint) or downstream uint
    // overloads reject it with no-matching-overload.
    const { env } = makeCodecHarness();
    const impl: HostFunction = (): CelValue => 7n;
    const imports = buildCelFnImports(
      env,
      new Map([
        ['seven_u', { impl, returnsUint: true, returnMessageFqn: undefined }],
      ]),
    );
    imports.seven_u?.(0);
    expect(env.view().getUint32(CEL_VALUE_KIND_OFFSET, true)).toBe(
      CelKind.UINT,
    );
    expect(resolveCelValue(env, 0)).toBe(7n);
  });

  it('does not re-stamp a non-bigint return of a uint-declared fn', () => {
    // The patch is gated on the impl actually returning a bigint — an
    // error value (or any mismatched shape) keeps its own kind.
    const { env } = makeCodecHarness();
    const impl: HostFunction = (): CelValue => ({
      kind: 'error',
      code: 41,
      message: 'host adapter error',
    });
    const imports = buildCelFnImports(
      env,
      new Map([
        ['bad_u', { impl, returnsUint: true, returnMessageFqn: undefined }],
      ]),
    );
    imports.bad_u?.(0);
    expect(env.view().getUint32(CEL_VALUE_KIND_OFFSET, true)).toBe(
      CelKind.ERROR,
    );
  });

  it('passes a CelError value the impl returns straight through', () => {
    const { env } = makeCodecHarness();
    const impl: HostFunction = (): CelValue => ({
      kind: 'error',
      code: 41,
      message: 'host adapter error',
    });
    const imports = buildCelFnImports(
      env,
      new Map([
        ['boom', { impl, returnsUint: false, returnMessageFqn: undefined }],
      ]),
    );
    imports.boom?.(0);
    const out = resolveCelValue(env, 0) as { kind: string; code: number };
    expect(out.kind).toBe('error');
    expect(out.code).toBe(41);
  });
});

// ── Message-declared returns (HostCallContext::ReturnProto mirror) ───

const USER_FQN = 'unit.User';

/** An inline protobufjs root with one message type for the return tests. */
function makeUserRoot(): protobuf.Root {
  return protobuf.Root.fromJSON({
    nested: {
      unit: {
        nested: {
          User: {
            fields: {
              name: { type: 'string', id: 1 },
              id: { type: 'int64', id: 2 },
            },
          },
        },
      },
    },
  });
}

describe('buildCelFnImports — proto(<fqn>)-declared returns', () => {
  it('interns a protobufjs Message return as CEL_MESSAGE', () => {
    // The TS mirror of HostCallContext::ReturnProto
    // (eval/host_call_context.cc:549): the message backs an externref
    // entry and the out slot stamps kind 10 + the ref payload.
    const { env, refs } = makeCodecHarness();
    const type = makeUserRoot().lookupType(USER_FQN);
    const impl: HostFunction = () =>
      coerceObjectToMessage(type, { name: 'Ada', id: 7n });
    const imports = buildCelFnImports(
      env,
      new Map([
        ['get_user', { impl, returnsUint: false, returnMessageFqn: USER_FQN }],
      ]),
    );
    imports.get_user?.(0);
    expect(env.view().getUint32(CEL_VALUE_KIND_OFFSET, true)).toBe(
      CelKind.MESSAGE,
    );
    // The externref table holds the backing (slot 1 = first intern).
    expect(refs.message.lookup(1)).toBeDefined();
    expect(resolveCelValue(env, 0)).toEqual({ name: 'Ada', id: 7n });
  });

  it('coerces a plain-object return via the descriptors', () => {
    // The plain-object arm mirrors the message-typed activation binding
    // (messageBackingFrom, marshal.ts): the declared FQN + the Engine's
    // descriptors drive coerceObjectToMessage.
    const { env } = makeCodecHarness();
    const descriptors = DescriptorSet.fromRoot(makeUserRoot());
    const impl: HostFunction = () => ({ name: 'Grace' });
    const imports = buildCelFnImports(
      env,
      new Map([
        ['get_user', { impl, returnsUint: false, returnMessageFqn: USER_FQN }],
      ]),
      descriptors,
    );
    imports.get_user?.(0);
    expect(env.view().getUint32(CEL_VALUE_KIND_OFFSET, true)).toBe(
      CelKind.MESSAGE,
    );
    expect(resolveCelValue(env, 0)).toEqual({ name: 'Grace', id: 0n });
  });

  it('throws on a plain-object return without descriptors', () => {
    const { env } = makeCodecHarness();
    const impl: HostFunction = () => ({ name: 'Grace' });
    const imports = buildCelFnImports(
      env,
      new Map([
        ['get_user', { impl, returnsUint: false, returnMessageFqn: USER_FQN }],
      ]),
    );
    expect(() => imports.get_user?.(0)).toThrow(/needs descriptors/);
  });

  it('passes null and CelError returns through as values', () => {
    // §A.4.5: errors are values on the wire; null is a legal message-typed
    // result (C++: ReturnNull / ReturnError next to ReturnProto).
    const { env } = makeCodecHarness();
    const implNull: HostFunction = () => null;
    const implErr: HostFunction = () => ({
      kind: 'error',
      code: 41,
      message: 'host adapter error',
    });
    const imports = buildCelFnImports(
      env,
      new Map([
        [
          'u_null',
          { impl: implNull, returnsUint: false, returnMessageFqn: USER_FQN },
        ],
        [
          'u_err',
          { impl: implErr, returnsUint: false, returnMessageFqn: USER_FQN },
        ],
      ]),
    );
    imports.u_null?.(0);
    expect(resolveCelValue(env, 0)).toBeNull();
    imports.u_err?.(0);
    expect(resolveCelValue(env, 0)).toMatchObject({ kind: 'error', code: 41 });
  });

  it('throws on a non-message, non-record return for a message decl', () => {
    const { env } = makeCodecHarness();
    const impl: HostFunction = () => 42n;
    const imports = buildCelFnImports(
      env,
      new Map([
        ['get_user', { impl, returnsUint: false, returnMessageFqn: USER_FQN }],
      ]),
    );
    expect(() => imports.get_user?.(0)).toThrow(
      /declared to return proto\(unit\.User\).*bigint/,
    );
  });

  it('throws on a Message return for a NON-message-declared fn', () => {
    // The negative contract pinned e2e in host-fns.test.ts: a protobufjs
    // message returned where the decl says e.g. `int` is host misuse —
    // a clear throw (surfacing as a CelEvalError TRAP), never a silent
    // re-intern as a host map.
    const { env } = makeCodecHarness();
    const type = makeUserRoot().lookupType(USER_FQN);
    const impl: HostFunction = () => coerceObjectToMessage(type, {});
    const imports = buildCelFnImports(
      env,
      new Map([
        ['get_int', { impl, returnsUint: false, returnMessageFqn: undefined }],
      ]),
    );
    expect(() => imports.get_int?.(0)).toThrow(
      /returned a protobufjs message.*not proto\(<fqn>\)/,
    );
  });
});
