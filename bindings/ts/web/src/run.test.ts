// End-to-end run-path proof: compile a CEL expression through the SAME
// client-side path the SPA uses — `WasmCompileBackend` running the
// committed `compiler.wasm` — then evaluate the resulting Program through
// the exact client-side run path (`runProgram` → `@cel-wasm/eval`
// Engine/Instance) the browser uses.  No Monaco, no DOM, no server: this
// pins the static compile→run wiring the demo depends on.
//
// The compile half loads `public/compiler.wasm` (the committed asset the
// SPA lazy-fetches at runtime).  It is always present in the repo, so
// these cases run unconditionally.

import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

import {
  WasmCompileBackend,
  CelCompileError,
} from '@cel-wasm/compiler/wasm-backend';
import { decodeAbi, type Activation, type Program } from '@cel-wasm/eval';
import { beforeAll, describe, expect, it } from 'vitest';

import { runProgram } from './internal/run.js';
import { parseVariablesForm } from './internal/variables.js';

const COMPILER_WASM = fileURLToPath(
  new URL('../public/compiler.wasm', import.meta.url),
);

let backend: WasmCompileBackend;

beforeAll(async () => {
  const bytes = await readFile(COMPILER_WASM);
  backend = await WasmCompileBackend.create(bytes);
}, 60_000);

async function compileToProgram(
  source: string,
  vars: readonly { readonly name: string; readonly type: string }[] = [],
): Promise<Program> {
  const wasm = await backend.compile({ source, vars });
  return { wasm, abi: decodeAbi(wasm) };
}

describe('compile (compiler.wasm) → client-side run path', () => {
  it('evaluates a bound boolean access check to true', async () => {
    const program = await compileToProgram(
      'age >= 18 && country in ["US", "CA"]',
      [
        { name: 'age', type: 'int' },
        { name: 'country', type: 'string' },
      ],
    );
    const variables = parseVariablesForm('age:int=25\ncountry:string=US');
    const activation: Activation = {};
    for (const v of variables) {
      activation[v.decl.name] = v.value;
    }
    const result = await runProgram(program, activation);
    expect(result).toBe(true);
  });

  it('evaluates the same expression to false for an under-18 age', async () => {
    const program = await compileToProgram(
      'age >= 18 && country in ["US", "CA"]',
      [
        { name: 'age', type: 'int' },
        { name: 'country', type: 'string' },
      ],
    );
    const result = await runProgram(program, { age: 16n, country: 'US' });
    expect(result).toBe(false);
  });

  it('evaluates a list comprehension', async () => {
    const program = await compileToProgram('[1, 2, 3].map(x, x * 2)');
    const result = await runProgram(program, {});
    expect(result).toEqual([2n, 4n, 6n]);
  });

  it('evaluates a string builtin', async () => {
    const program = await compileToProgram('"hello".size()');
    expect(await runProgram(program, {})).toBe(5n);
  });

  it('surfaces divide-by-zero as a CelError value, not a throw', async () => {
    const program = await compileToProgram('1 / 0');
    const result = await runProgram(program, {});
    expect(result).toMatchObject({ kind: 'error' });
  });

  it('throws CelCompileError on an invalid expression and recovers', async () => {
    await expect(compileToProgram('1 +')).rejects.toBeInstanceOf(
      CelCompileError,
    );
    // The backend must remain usable after an exception-escape compile.
    expect(await runProgram(await compileToProgram('1 + 1'), {})).toBe(2n);
  });
});
