// End-to-end run-path proof: compile a CEL expression through the SAME
// client-side path the SPA uses — `WasmCompileBackend` running the
// committed `compiler.wasm` — then evaluate the resulting Program through
// the exact client-side run path (`runProgram` → `@cel-wasm/eval`
// Engine/Instance) the browser uses.  No Monaco, no DOM, no server: this
// pins the compile→run wiring the demo depends on, in both STATIC and
// DYNAMIC link modes (the demo compiles dynamically).
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
import {
  Engine,
  decodeAbi,
  type Activation,
  type Program,
} from '@cel-wasm/eval';
import { beforeAll, describe, expect, it } from 'vitest';

import { runProgram } from './internal/run.js';
import { parseVariablesForm } from './internal/variables.js';

const COMPILER_WASM = fileURLToPath(
  new URL('../public/compiler.wasm', import.meta.url),
);
const RUNTIME_WASM = fileURLToPath(
  new URL('../public/cel_runtime.wasm', import.meta.url),
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

/** Whether a compiled Program imports the `cel.*` runtime namespace. */
function importsCelRuntime(wasm: Uint8Array): boolean {
  const module = new WebAssembly.Module(wasm);
  return WebAssembly.Module.imports(module).some((i) => i.module === 'cel');
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

// The `linkMode` compile option (records-encoded into `cew_compile_opts`)
// chooses between a self-contained STATIC Program and a thin DYNAMIC expr
// module that imports the runtime from `cel.*`. The two artifacts differ
// by ~200x in size; both evaluate identically through the same `plan` →
// `eval` path (the Engine instantiates `cel_runtime.wasm` for the dynamic
// one, loadable from the shipped runtime in Node).
describe('static vs dynamic link mode (cew_compile_opts)', () => {
  it('static mode bakes the runtime in — no cel.* imports, large', async () => {
    const wasm = await backend.compile({
      source: '1 + 2',
      vars: [],
      linkMode: 'static',
    });
    expect(importsCelRuntime(wasm)).toBe(false);
    expect(wasm.length).toBeGreaterThan(100_000);
  });

  it('dynamic mode is a thin expr module importing cel.*, tiny', async () => {
    const wasm = await backend.compile({
      source: '1 + 2',
      vars: [],
      linkMode: 'dynamic',
    });
    expect(importsCelRuntime(wasm)).toBe(true);
    expect(wasm.length).toBeLessThan(50_000);
  });

  it('a dynamic Program evaluates to the same value as a static one', async () => {
    const vars = [
      { name: 'x', type: 'int' },
      { name: 'y', type: 'int' },
    ];
    const activation: Activation = { x: 10n, y: 32n };
    const staticWasm = await backend.compile({
      source: 'x + y',
      vars,
      linkMode: 'static',
    });
    const dynamicWasm = await backend.compile({
      source: 'x + y',
      vars,
      linkMode: 'dynamic',
    });
    const staticResult = await runProgram(
      { wasm: staticWasm, abi: decodeAbi(staticWasm) },
      activation,
    );
    const dynamicResult = await runProgram(
      { wasm: dynamicWasm, abi: decodeAbi(dynamicWasm) },
      activation,
    );
    expect(staticResult).toBe(42n);
    expect(dynamicResult).toBe(staticResult);
  });

  // The browser supplies the runtime explicitly via EngineOptions.runtime
  // (no node:fs); `web/public/cel_runtime.wasm` is the asset the SPA
  // fetches. This pins that exact override path the demo's run.ts uses.
  it('links a dynamic Program against an explicit runtime override (browser path)', async () => {
    const runtimeBytes = await readFile(RUNTIME_WASM);
    const engine = await Engine.create({ runtime: runtimeBytes });
    const wasm = await backend.compile({
      source: '[1, 2, 3].map(x, x * 2)',
      vars: [],
      linkMode: 'dynamic',
    });
    expect(importsCelRuntime(wasm)).toBe(true);
    const instance = await engine.plan({ wasm, abi: decodeAbi(wasm) });
    expect(instance.eval({})).toEqual([2n, 4n, 6n]);
  });
});

// A serialized FileDescriptorSet for `celwasm.test.Widget { string label = 1; }`
// (the bytes `protoc --descriptor_set_out` emits; regenerate via
// protobufjs/ext/descriptor or the C++ `BuildWidgetFds`). Widget exists ONLY
// in these bytes, never in any generated pool — so a compile that resolves it
// proves the descriptorSetBytes path is load-bearing.
const WIDGET_FDS_HEX =
  '0a3d0a0c7769646765742e70726f746f120c63656c7761736d2e74657374' +
  '22170a06576964676574120d0a056c6162656c180120012809620670726f746f33';

function hexToBytes(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

// descriptorSetBytes feeds an in-memory FileDescriptorSet to the wasm compiler
// (a 'd' record → cel_compile_opts_set_descriptor_set), so a proto-typed
// expression type-checks in the browser with no filesystem.
describe('descriptorSetBytes (proto compile via the wasm backend)', () => {
  it('resolves a supplied-pool message type from the FDS bytes', async () => {
    const wasm = await backend.compile({
      source: 'w.label',
      vars: [{ name: 'w', type: 'celwasm.test.Widget' }],
      descriptorSetBytes: hexToBytes(WIDGET_FDS_HEX),
      linkMode: 'dynamic',
    });
    // Type-checked + lowered to a Program (the message resolved via the FDS).
    expect(decodeAbi(wasm).version).toBeGreaterThanOrEqual(0);
  });

  it('fails without the FDS — the message type is undeclared', async () => {
    await expect(
      backend.compile({
        source: 'w.label',
        vars: [{ name: 'w', type: 'celwasm.test.Widget' }],
        linkMode: 'dynamic',
      }),
    ).rejects.toBeInstanceOf(CelCompileError);
  });
});
