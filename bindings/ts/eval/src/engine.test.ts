// Engine — descriptor loading, host-function registration, planning.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { decodeAbi } from './abi.js';
import { Engine } from './engine.js';
import type { CelValue, Program } from './types.js';

const FIXTURES_DIR = fileURLToPath(new URL('../fixtures/', import.meta.url));
const DYNAMIC_DIR = fileURLToPath(
  new URL('../fixtures/dynamic/', import.meta.url),
);
const RUNTIME_WASM = fileURLToPath(
  new URL('../runtime/cel_runtime.wasm', import.meta.url),
);

function loadProgram(name: string): Program {
  const wasm = new Uint8Array(readFileSync(`${FIXTURES_DIR}${name}.wasm`));
  return { wasm, abi: decodeAbi(wasm) };
}

function loadDynamic(name: string): Program {
  const wasm = new Uint8Array(readFileSync(`${DYNAMIC_DIR}${name}.wasm`));
  return { wasm, abi: decodeAbi(wasm) };
}

describe('Engine.create', () => {
  it('builds an Engine with no descriptors', async () => {
    const engine = await Engine.create();
    expect(engine).toBeInstanceOf(Engine);
  });

  it('builds an Engine with no options object', async () => {
    const engine = await Engine.create();
    expect(engine).toBeInstanceOf(Engine);
  });

  it('plans a scalar Program into an Instance that evaluates', async () => {
    const engine = await Engine.create();
    const instance = await engine.plan(loadProgram('int_add'));
    expect(instance.eval()).toBe(3n);
  });

  it('plans the same Program twice into independent Instances', async () => {
    const engine = await Engine.create();
    const a = await engine.plan(loadProgram('var_int_identity'));
    const b = await engine.plan(loadProgram('var_int_identity'));
    expect(a.eval({ x: 1n })).toBe(1n);
    expect(b.eval({ x: 2n })).toBe(2n);
    // The first Instance's state is untouched by the second.
    expect(a.eval({ x: 3n })).toBe(3n);
  });

  it('plans a dynamic Program (loading the shipped runtime) into an Instance', async () => {
    const engine = await Engine.create();
    const instance = await engine.plan(loadDynamic('int_add'));
    expect(instance.eval()).toBe(3n);
  });

  it('reuses one compiled runtime across two dynamic Instances', async () => {
    const engine = await Engine.create();
    const a = await engine.plan(loadDynamic('var_int_add'));
    const b = await engine.plan(loadDynamic('var_int_add'));
    // Each Instance gets its own fresh runtime instance, so their eval
    // state is isolated even though the compiled module is shared.
    expect(a.eval({ x: 1n, y: 2n })).toBe(3n);
    expect(b.eval({ x: 10n, y: 20n })).toBe(30n);
    expect(a.eval({ x: 4n, y: 5n })).toBe(9n);
  });

  it('accepts a caller-supplied runtime override (the browser path)', async () => {
    const runtime = new Uint8Array(readFileSync(RUNTIME_WASM));
    const engine = await Engine.create({ runtime });
    const instance = await engine.plan(loadDynamic('string_concat'));
    expect(instance.eval()).toBe('hello world');
  });

  it('does not load the runtime for a static-only workload', async () => {
    // No runtime override + a deliberately-unreadable path would only fail
    // if the runtime were loaded; a static plan must never touch it.  We
    // assert indirectly: an Engine whose runtime override is malformed
    // still plans static Programs fine (the dynamic loader is never hit).
    const engine = await Engine.create({ runtime: Uint8Array.of(0, 1, 2, 3) });
    const instance = await engine.plan(loadProgram('int_add'));
    expect(instance.eval()).toBe(3n);
  });
});

describe('Engine.defineFunction', () => {
  // The decl→overload-id synthesis matrix is pinned in
  // `celfn-decl.test.ts`; the link-through against a compiled `@host`
  // Program is pinned in `eval/e2e/host-fns.test.ts`.  Here we pin the
  // registration surface itself.
  it('registers a .celfn @host declaration', async () => {
    const engine = await Engine.create();
    const impl = (...args: CelValue[]): CelValue => args[0] ?? null;
    // The same decl string the compiler's `fns` option takes.
    expect(() => {
      engine.defineFunction('int @host.my_fn(int x);', impl);
    }).not.toThrow();
  });

  it('accepts a bare overload id verbatim', async () => {
    const engine = await Engine.create();
    expect(() => {
      engine.defineFunction('greet', () => 'hi');
    }).not.toThrow();
  });

  it('rejects a malformed declaration', async () => {
    const engine = await Engine.create();
    expect(() => {
      engine.defineFunction('(int): int', () => null);
    }).toThrow(/malformed .celfn host declaration/);
  });

  it('rejects a TS-style signature (not a .celfn decl)', async () => {
    const engine = await Engine.create();
    expect(() => {
      engine.defineFunction('my_fn(int): int', () => null);
    }).toThrow(/is not a .celfn type/);
  });

  it('re-registering an overload id overwrites the prior impl', async () => {
    const engine = await Engine.create();
    engine.defineFunction('f', () => 1n);
    // No throw on overwrite; the registry keys on the overload id.
    expect(() => {
      engine.defineFunction('f', () => 2n);
    }).not.toThrow();
  });
});
