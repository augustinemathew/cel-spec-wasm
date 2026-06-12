import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { compile, CelCompileError } from './index.js';

// The wasm preamble: `\0asm` magic + version 1 (little-endian u32).
const WASM_PREAMBLE = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 1, 0, 0, 0]);

const FIXTURES_DIR = fileURLToPath(
  new URL('../../eval/fixtures/', import.meta.url),
);
const GOLDEN_VAR_INT_ADD = new Uint8Array(
  readFileSync(`${FIXTURES_DIR}var_int_add.wasm`),
);

// `compile()` runs the in-process `compiler.wasm` backend — no native CLI is
// required — so these run unconditionally.
describe('compile (in-process compiler.wasm)', () => {
  it('compiles a constant expression to a Program with an empty variable table', async () => {
    const program = await compile('1 + 2');
    expect(program.wasm.subarray(0, WASM_PREAMBLE.length)).toEqual(
      WASM_PREAMBLE,
    );
    expect(program.abi.variables).toHaveLength(0);
  });

  it('compiles a variable expression, naming the declared variables in the ABI', async () => {
    const program = await compile('x + y', [
      { name: 'x', type: 'int' },
      { name: 'y', type: 'int' },
    ]);
    const names = program.abi.variables.map((v) => v.name);
    expect(names).toEqual(['x', 'y']);
  });

  it('produces bytes identical to the committed golden fixture (deterministic compile)', async () => {
    const program = await compile('x + y', [
      { name: 'x', type: 'int' },
      { name: 'y', type: 'int' },
    ]);
    expect(program.wasm).toEqual(GOLDEN_VAR_INT_ADD);
  });

  it('wraps a leading-dash expression so it is not parsed as a flag', async () => {
    const program = await compile('-x', [{ name: 'x', type: 'int' }]);
    expect(program.wasm.subarray(0, 4)).toEqual(WASM_PREAMBLE.subarray(0, 4));
  });

  it('rejects a syntactically invalid expression with a non-empty diagnostic', async () => {
    await expect(compile('1 +')).rejects.toThrow(CelCompileError);
    const err = await compile('1 +').catch((e: unknown) => e);
    expect(err).toBeInstanceOf(CelCompileError);
    const diags = (err as CelCompileError).diagnostics;
    expect(diags.length).toBeGreaterThan(0);
    expect(diags[0]?.message.length).toBeGreaterThan(0);
  });

  it('rejects a type-check failure with a located diagnostic', async () => {
    const err = await compile('1 + "a"').catch((e: unknown) => e);
    expect(err).toBeInstanceOf(CelCompileError);
    const diags = (err as CelCompileError).diagnostics;
    expect(diags.some((d) => d.line !== undefined)).toBe(true);
  });
});
