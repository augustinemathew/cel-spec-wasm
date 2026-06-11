// Unit coverage for the dynamic-link runtime loader.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { CelRuntimeLoadError, loadRuntimeModule } from './runtime-loader.js';

const RUNTIME_WASM = fileURLToPath(
  new URL('../runtime/cel_runtime.wasm', import.meta.url),
);

describe('loadRuntimeModule', () => {
  it('loads the shipped runtime when no override is given (Node path)', async () => {
    const module = await loadRuntimeModule(undefined);
    expect(module).toBeInstanceOf(WebAssembly.Module);
    // The shipped runtime exports the shared memory + arena the dynamic
    // link path binds as `cel.*`.
    const exports = WebAssembly.Module.exports(module).map((e) => e.name);
    expect(exports).toContain('memory');
    expect(exports).toContain('arena_alloc');
    expect(exports).toContain('arena_init');
  });

  it('returns a pre-compiled module as-is', async () => {
    const pre = await WebAssembly.compile(readFileSync(RUNTIME_WASM));
    const module = await loadRuntimeModule(pre);
    expect(module).toBe(pre);
  });

  it('compiles override bytes', async () => {
    const bytes = readFileSync(RUNTIME_WASM);
    const module = await loadRuntimeModule(bytes);
    expect(module).toBeInstanceOf(WebAssembly.Module);
  });

  it('throws CelRuntimeLoadError on malformed override bytes', async () => {
    const garbage = Uint8Array.of(1, 2, 3, 4);
    await expect(loadRuntimeModule(garbage)).rejects.toBeInstanceOf(
      CelRuntimeLoadError,
    );
  });
});
