// End-to-end run-path proof: compile a CEL expression through the same
// dev-server handler the browser calls, then evaluate the resulting
// Program through the exact client-side run path (`runProgram` →
// `@cel-wasm/eval` Engine/Instance) the browser uses.  No Monaco, no DOM —
// this pins the compile→run wiring the demo depends on.
//
// The compile half needs the native `cel` CLI; when it is not built the
// cases return early (mirroring the compiler binding's CLI-absent
// discipline) rather than failing.

import { decodeAbi, type Activation, type Program } from '@cel-wasm/eval';
import { describe, expect, it } from 'vitest';

import { runCompile } from '../dev-server/compile-handler.js';

import { base64ToBytes } from './internal/compile-client.js';
import { runProgram } from './internal/run.js';
import { parseVariablesForm } from './internal/variables.js';

async function compileToProgram(
  source: string,
  vars: readonly { readonly name: string; readonly type: string }[] = [],
): Promise<Program | undefined> {
  let response;
  try {
    response = await runCompile({ source, vars });
  } catch (err) {
    if (err instanceof Error && err.message.includes('cel CLI not found')) {
      return undefined;
    }
    throw err;
  }
  if (!response.ok) {
    throw new Error(`unexpected compile failure: ${response.error}`);
  }
  const wasm = base64ToBytes(response.wasmBase64);
  return { wasm, abi: decodeAbi(wasm) };
}

describe('compile → client-side run path', () => {
  it('evaluates a bound boolean access check to true', async () => {
    const program = await compileToProgram(
      'age >= 18 && country in ["US", "CA"]',
      [
        { name: 'age', type: 'int' },
        { name: 'country', type: 'string' },
      ],
    );
    if (program === undefined) {
      return;
    }
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
    if (program === undefined) {
      return;
    }
    const result = await runProgram(program, { age: 16n, country: 'US' });
    expect(result).toBe(false);
  });

  it('evaluates a list comprehension', async () => {
    const program = await compileToProgram('[1, 2, 3].map(x, x * 2)');
    if (program === undefined) {
      return;
    }
    const result = await runProgram(program, {});
    expect(result).toEqual([2n, 4n, 6n]);
  });

  it('evaluates a string builtin', async () => {
    const program = await compileToProgram('"hello".size()');
    if (program === undefined) {
      return;
    }
    expect(await runProgram(program, {})).toBe(5n);
  });

  it('surfaces divide-by-zero as a CelError value, not a throw', async () => {
    const program = await compileToProgram('1 / 0');
    if (program === undefined) {
      return;
    }
    const result = await runProgram(program, {});
    expect(result).toMatchObject({ kind: 'error' });
  });
});
