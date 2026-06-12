// Load the `compiler.wasm` bytes the default {@link WasmCompileBackend}
// runs — the CEL compiler (cel-cpp + Binaryen) cross-compiled to
// wasm32-wasi.
//
// This mirrors `@cel-wasm/eval`'s `runtime-loader.ts`: the package owns its
// own artifact (`compiler/wasm/compiler.wasm`, ~56 MB, git-ignored and built
// by `scripts/build-wasm-assets.sh`) rather than reaching into the web
// package's `public/` dir.  Loading is platform-portable:
//
//   - Node: read the shipped `compiler/wasm/compiler.wasm` via `node:fs`.
//   - Browser: there is no `node:fs`, so the caller constructs a
//     {@link WasmCompileBackend} directly from fetched bytes (the SPA's
//     `compile-client.ts` does exactly this); this loader's Node path is
//     never reached.
//
// No `node:*` module is imported statically: the only Node dependency
// (`node:fs/promises`) is reached through a dynamic `import()` so a browser
// bundler that never calls `getDefaultBackend()` need not resolve a Node
// builtin.

/**
 * The on-disk location of the shipped compiler wasm, resolved relative to
 * this module so it works from both `src/` (vitest) and `dist/` (published):
 * `dist/internal/` and `src/internal/` are both two levels under the package
 * root, where `wasm/compiler.wasm` lives.
 */
const COMPILER_WASM_URL = new URL('../../wasm/compiler.wasm', import.meta.url);

/**
 * Thrown when the compiler wasm cannot be located or read — a
 * host/environment failure (no shipped asset, or a browser reaching the
 * Node path), not a CEL compile error.
 */
export class CelCompilerLoadError extends Error {
  override readonly name = 'CelCompilerLoadError';
}

/**
 * Read the shipped `compiler.wasm` bytes via `node:fs`.  The import is
 * dynamic so a browser bundle that supplies its own bytes never has to
 * resolve `node:fs`; reaching this path in a browser surfaces a clear error.
 */
export async function readShippedCompilerWasm(): Promise<Uint8Array> {
  let readFile: typeof import('node:fs/promises').readFile;
  try {
    ({ readFile } = await import('node:fs/promises'));
  } catch (err) {
    throw new CelCompilerLoadError(
      'no shipped compiler.wasm loader is available in this environment ' +
        '(node:fs is absent — likely a browser); construct a ' +
        'WasmCompileBackend from fetched bytes instead',
      { cause: err },
    );
  }
  try {
    // node:fs accepts a file:// URL directly — no fileURLToPath (and thus
    // no static node:url import a browser bundler would choke on).
    return await readFile(COMPILER_WASM_URL);
  } catch (err) {
    throw new CelCompilerLoadError(
      `failed to read shipped compiler.wasm at ${COMPILER_WASM_URL.href} — ` +
        'build it with `bindings/ts/scripts/build-wasm-assets.sh`',
      { cause: err },
    );
  }
}
