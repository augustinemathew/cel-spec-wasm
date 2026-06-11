// DYNAMIC-link Program eval — the dynamic twin of the static fixture
// suite.
//
// A DYNAMIC Program is a thin (~6 KB) expr module that imports the runtime
// helpers from the `cel` namespace (incl. the shared `cel.memory`) instead
// of bundling them, and is linked against a separately-instantiated
// `cel_runtime.wasm`.  This suite proves:
//
//   1. Routing: `engine.plan` detects the dynamic shape by import
//      introspection (a `cel.*` import) and links the runtime — while a
//      static Program (no `cel.*` import) still takes the bundled path.
//   2. Parity: a dynamic Program evaluates to the SAME CelValue as its
//      static twin for the same expression — scalar, variable,
//      comprehension, map, and string ops.
//   3. The shared-memory marshal/decode round-trips correctly against the
//      runtime's SharedArrayBuffer (the runtime is wasi-threads).
//
// The dynamic `.wasm` fixtures under `fixtures/dynamic/` were produced by
// driving `compiler.wasm`'s `cew_compile_opts` with the link_mode option
// set to DYNAMIC (see `scripts/gen-dynamic-fixtures.mjs`); each shares its
// name AND source with the static twin of the same name under `fixtures/`,
// so the parity assertion is exact (same source ⇒ same result).
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md;
//       doc/design/00-architecture.md §3 (link modes);
//       C++ reference `eval/engine.cc` (InstantiateRuntime → eval).

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { decodeAbi } from './abi.js';
import type { Activation } from './activation.js';
import { Engine } from './engine.js';
import { LinkMode } from './types.js';
import type { CelValue, Program } from './types.js';

const FIXTURES_DIR = fileURLToPath(new URL('../fixtures/', import.meta.url));
const DYNAMIC_DIR = fileURLToPath(
  new URL('../fixtures/dynamic/', import.meta.url),
);

function loadProgram(path: string): Program {
  const wasm = new Uint8Array(readFileSync(path));
  return { wasm, abi: decodeAbi(wasm) };
}

function loadDynamic(name: string): Program {
  return loadProgram(`${DYNAMIC_DIR}${name}.wasm`);
}

function loadStatic(name: string): Program {
  return loadProgram(`${FIXTURES_DIR}${name}.wasm`);
}

function importsCel(program: Program): boolean {
  const mod = new WebAssembly.Module(program.wasm);
  return WebAssembly.Module.imports(mod).some((i) => i.module === 'cel');
}

async function evalDynamic(
  name: string,
  activation?: Activation,
): Promise<CelValue> {
  const engine = await Engine.create();
  const instance = await engine.plan(loadDynamic(name));
  return instance.eval(activation);
}

// ── Routing detection ───────────────────────────────────────────────

describe('plan routing — import introspection', () => {
  it('a dynamic Program imports the `cel` namespace', () => {
    expect(importsCel(loadDynamic('int_add'))).toBe(true);
  });

  it('a static Program does NOT import the `cel` namespace', () => {
    expect(importsCel(loadStatic('int_add'))).toBe(false);
  });

  it('the dynamic fixture carries the DYNAMIC link_mode label', () => {
    expect(loadDynamic('int_add').abi.linkMode).toBe(LinkMode.DYNAMIC);
  });

  it('the static fixture carries the STATIC link_mode label', () => {
    expect(loadStatic('int_add').abi.linkMode).toBe(LinkMode.STATIC);
  });

  it('plans a dynamic Program into an Instance the runtime backs', async () => {
    const engine = await Engine.create();
    const instance = await engine.plan(loadDynamic('int_add'));
    // The dynamic Instance evaluates — proving the runtime linked and the
    // shared memory backs marshal/decode.
    expect(instance.eval()).toBe(3n);
  });
});

// ── Dynamic eval — one case per value shape ─────────────────────────

describe('Instance.eval — dynamic Programs', () => {
  it('scalar: 1 + 2 == 3n', async () => {
    expect(await evalDynamic('int_add')).toBe(3n);
  });

  it('variable: x + y == 42n (x=10, y=32)', async () => {
    expect(await evalDynamic('var_int_add', { x: 10n, y: 32n })).toBe(42n);
  });

  it('list comprehension: [1,2,3].map(x, x*2) == [2,4,6]', async () => {
    expect(await evalDynamic('list_map_double')).toEqual([2n, 4n, 6n]);
  });

  it('map index: {"a":1,"b":2}["a"] == 1n', async () => {
    expect(await evalDynamic('map_index')).toBe(1n);
  });

  it('string: "hello" + " world" == "hello world"', async () => {
    expect(await evalDynamic('string_concat')).toBe('hello world');
  });
});

// ── Static/dynamic parity — same source ⇒ same result ───────────────
//
// Each dynamic fixture shares its name + source with the static fixture
// of the same name, so the parity is exact: both are evaluated through
// the same Engine API with the same activation and must produce
// deep-equal CelValues.

interface ParityCase {
  readonly name: string;
  readonly activation?: Activation;
}

const PARITY: readonly ParityCase[] = [
  { name: 'int_add' },
  { name: 'var_int_add', activation: { x: 10n, y: 32n } },
  { name: 'list_map_double' },
  { name: 'map_index' },
  { name: 'string_concat' },
];

describe('dynamic ≡ static parity', () => {
  it.each(PARITY.map((c) => [c.name, c] as const))(
    'dynamic %s equals its static twin',
    async (_name, parity) => {
      const engine = await Engine.create();
      const dyn = await engine.plan(loadDynamic(parity.name));
      const sta = await engine.plan(loadStatic(parity.name));
      const dynResult = dyn.eval(parity.activation);
      const staResult = sta.eval(parity.activation);
      expect(dynResult).toEqual(staResult);
    },
  );
});

// ── A static Program still works unchanged through the same Engine ──

describe('static Program is unaffected by dynamic routing', () => {
  it('a static scalar Program still evaluates', async () => {
    const engine = await Engine.create();
    const instance = await engine.plan(loadStatic('int_add'));
    expect(instance.eval()).toBe(3n);
  });

  it('re-evaluates a static variable Program with two activations', async () => {
    const engine = await Engine.create();
    const instance = await engine.plan(loadStatic('var_int_identity'));
    expect(instance.eval({ x: 7n })).toBe(7n);
    expect(instance.eval({ x: 99n })).toBe(99n);
  });
});
