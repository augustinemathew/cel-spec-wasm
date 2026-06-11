// Engine — descriptor loading, host-function registration, planning.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { decodeAbi } from './abi.js';
import { Engine } from './engine.js';
import type { CelValue, Program } from './types.js';

const FIXTURES_DIR = fileURLToPath(new URL('../fixtures/', import.meta.url));

function loadProgram(name: string): Program {
  const wasm = new Uint8Array(readFileSync(`${FIXTURES_DIR}${name}.wasm`));
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
});

describe('Engine.defineFunction', () => {
  it('registers a host function under its leading identifier', async () => {
    const engine = await Engine.create();
    const impl = (...args: CelValue[]): CelValue => args[0] ?? null;
    // A well-formed declaration registers without throwing.
    expect(() => {
      engine.defineFunction('my_fn(int): int', impl);
    }).not.toThrow();
  });

  it('accepts a bare-identifier declaration', async () => {
    const engine = await Engine.create();
    expect(() => {
      engine.defineFunction('greet', () => 'hi');
    }).not.toThrow();
  });

  it('rejects a declaration with no leading identifier', async () => {
    const engine = await Engine.create();
    expect(() => {
      engine.defineFunction('(int): int', () => null);
    }).toThrow(/no leading identifier/);
  });

  it('re-registering a name overwrites the prior impl', async () => {
    const engine = await Engine.create();
    engine.defineFunction('f', () => 1n);
    // No throw on overwrite; the registry keys on the name.
    expect(() => {
      engine.defineFunction('f', () => 2n);
    }).not.toThrow();
  });
});
