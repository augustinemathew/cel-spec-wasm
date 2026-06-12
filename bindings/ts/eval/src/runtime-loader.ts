// Load + compile the standalone `cel_runtime.wasm` a DYNAMIC Program
// links against.
//
// A dynamic Program is a thin (~6 KB) expr module that imports the
// runtime helpers from the `cel` namespace rather than bundling them
// (~1.3 MB).  The Engine instantiates one shared `cel_runtime.wasm`
// (2.8 MB, exporting `memory` + `arena_alloc` + the 200-odd helpers) and
// exposes its exports as the expr module's `cel.*` imports.  This module
// owns the platform-portable *loading* of those bytes:
//
//   - Node: read the runtime shipped at `eval/runtime/cel_runtime.wasm`
//     via `node:fs`, then `WebAssembly.compile`.
//   - Browser: there is no `node:fs`, so the caller passes the bytes (or
//     a pre-compiled module) via `EngineOptions.runtime`; the static
//     import of `node:fs` is deferred behind a runtime check so a browser
//     bundler never has to resolve it for a caller-supplied runtime.
//
// The runtime is built for `wasm32-wasi-threads` (cctz needs `<mutex>`),
// so its exported `memory` is a SharedArrayBuffer.  Compilation happens
// once per Engine; instantiation is per-Instance (see `instance.ts`).
//
// No `node:*` module is imported statically: the only Node dependency
// (`node:fs/promises`) is reached through a dynamic `import()` inside
// `readShippedRuntime`, so a browser bundler that supplies its own
// runtime via `EngineOptions.runtime` never has to resolve a Node
// builtin.

/**
 * The on-disk location of the shipped runtime, resolved relative to this
 * module so it works from both `src/` (vitest) and `dist/` (published).
 */
const RUNTIME_WASM_URL = new URL(
  '../runtime/cel_runtime.wasm',
  import.meta.url,
);

/**
 * Thrown when the dynamic-link runtime cannot be located or compiled — a
 * host/environment failure (no shipped runtime, no override in a browser,
 * malformed bytes), not a CEL spec error.
 */
export class CelRuntimeLoadError extends Error {
  override readonly name = 'CelRuntimeLoadError';
}

/**
 * Compile the `cel_runtime.wasm` module a dynamic Program links against.
 *
 * When `override` is a {@link WebAssembly.Module}, it is returned as-is.
 * When it is `BufferSource` bytes, they are compiled.  When omitted, the
 * shipped runtime is read from disk (Node only) and compiled; in a
 * browser, where `node:fs` is unavailable, omitting `override` throws —
 * the caller must supply the bytes via `EngineOptions.runtime`.
 */
export async function loadRuntimeModule(
  override: BufferSource | WebAssembly.Module | undefined,
): Promise<WebAssembly.Module> {
  if (override instanceof WebAssembly.Module) {
    return override;
  }
  if (override !== undefined) {
    return compileBytes(override, 'EngineOptions.runtime');
  }
  const bytes = await readShippedRuntime();
  return compileBytes(bytes, RUNTIME_WASM_URL.href);
}

async function compileBytes(
  bytes: BufferSource,
  source: string,
): Promise<WebAssembly.Module> {
  try {
    return await WebAssembly.compile(bytes);
  } catch (err) {
    throw new CelRuntimeLoadError(
      `failed to compile cel_runtime.wasm from ${source}`,
      { cause: err },
    );
  }
}

/**
 * Read the shipped runtime bytes via `node:fs`.  The import is dynamic so
 * a browser bundle that supplies its own runtime never has to resolve
 * `node:fs`; reaching this path in a browser surfaces a clear error
 * pointing the caller at `EngineOptions.runtime`.
 */
async function readShippedRuntime(): Promise<Uint8Array> {
  let readFile: typeof import('node:fs/promises').readFile;
  try {
    ({ readFile } = await import('node:fs/promises'));
  } catch (err) {
    throw new CelRuntimeLoadError(
      'no shipped cel_runtime.wasm loader is available in this environment ' +
        '(node:fs is absent — likely a browser); pass the runtime bytes via ' +
        'EngineOptions.runtime',
      { cause: err },
    );
  }
  try {
    // node:fs accepts a file:// URL directly — no fileURLToPath (and thus
    // no static node:url import that a browser bundler would choke on).
    return await readFile(RUNTIME_WASM_URL);
  } catch (err) {
    throw new CelRuntimeLoadError(
      `failed to read shipped cel_runtime.wasm at ${RUNTIME_WASM_URL.href}`,
      { cause: err },
    );
  }
}
