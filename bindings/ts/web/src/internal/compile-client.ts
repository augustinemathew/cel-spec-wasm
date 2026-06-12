// The browser-side compile client: runs `compiler.wasm` (the CEL compiler
// cross-compiled to wasm32-wasi) fully client-side via
// {@link WasmCompileBackend}, so the demo is a static page with no server.
//
// The compiler wasm is ~54 MB raw (~6 MB gzipped); it is lazy-fetched on
// the first compile and the instantiated backend is cached for the rest of
// the session.  Eval never round-trips — once this returns a Program, the
// run path is pure client-side TS (`run.ts`).
//
// KNOWN LIMITATION — diagnostics.  The stock wasi-sdk `compiler.wasm` has
// no C++ exception runtime, so an invalid expression cannot recover
// cel-cpp's line/column diagnostic; {@link WasmCompileBackend} throws a
// single generic {@link CelCompileError}.  Precise diagnostics need the
// native / emscripten backend.

import {
  WasmCompileBackend,
  type Diagnostic,
} from '@cel-wasm/compiler/wasm-backend';
import { decodeAbi, type Program } from '@cel-wasm/eval';

/** A declared free variable for the compile request. */
export interface CompileVar {
  readonly name: string;
  readonly type: string;
}

/** A diagnostic surfaced from the compiler (line/column are 1-based). */
export type CompileDiagnostic = Diagnostic;

/** Re-exported so callers import the typed failure from one module. */
export { CelCompileError } from '@cel-wasm/compiler/wasm-backend';
export type { Diagnostic } from '@cel-wasm/compiler/wasm-backend';

/**
 * Observe the one-time compiler-wasm load.  `onLoadStart` fires before the
 * (potentially multi-second) fetch + instantiate of the first compile;
 * `onLoadEnd` fires once the backend is ready (or fetch failed).
 */
export interface CompileClientHooks {
  readonly onLoadStart?: () => void;
  readonly onLoadEnd?: () => void;
}

/**
 * The lazy client-side CEL compiler.  Fetches and instantiates
 * `compiler.wasm` on first use, caching the backend for the session.
 */
export class CompileClient {
  #backend: WasmCompileBackend | undefined;
  #loading: Promise<WasmCompileBackend> | undefined;
  readonly #hooks: CompileClientHooks;

  constructor(hooks: CompileClientHooks = {}) {
    this.#hooks = hooks;
  }

  /** Whether the compiler wasm has already been fetched + instantiated. */
  get ready(): boolean {
    return this.#backend !== undefined;
  }

  /**
   * Compile `source` (with optional declared `vars`) to a portable
   * {@link Program}.  The first call fetches `compiler.wasm` (showing the
   * load hooks); subsequent calls reuse the cached backend.
   *
   * @throws {CelCompileError} on a parse / type-check failure.  The wasm
   *   backend reports a single location-less diagnostic (see the
   *   module-level KNOWN LIMITATION note).
   */
  async compile(source: string, vars: readonly CompileVar[]): Promise<Program> {
    const backend = await this.#getBackend();
    const wasm = await backend.compile({
      source,
      vars: vars.map((v) => ({ name: v.name, type: v.type })),
      // DYNAMIC link mode: emit a ~6 KB expr module that imports the
      // runtime from `cel.*` rather than a self-contained ~1.3 MB static
      // Program.  The run path (`run.ts`) links it against the shared
      // `cel_runtime.wasm` pulled into the page.
      linkMode: 'dynamic',
    });
    // The backend returns only the portable wasm bytes; the `cel.abi`
    // descriptor is decoded from them here, client-side — the same decode
    // the eval binding does at plan time.
    return { wasm, abi: decodeAbi(wasm) };
  }

  async #getBackend(): Promise<WasmCompileBackend> {
    if (this.#backend !== undefined) {
      return this.#backend;
    }
    this.#loading ??= this.#load();
    return this.#loading;
  }

  async #load(): Promise<WasmCompileBackend> {
    this.#hooks.onLoadStart?.();
    try {
      const url = `${import.meta.env.BASE_URL}compiler.wasm`;
      const response = await fetch(url);
      if (!response.ok) {
        throw new Error(
          `failed to fetch the compiler wasm (HTTP ${String(response.status)})`,
        );
      }
      const bytes = await response.arrayBuffer();
      const backend = await WasmCompileBackend.create(bytes);
      this.#backend = backend;
      return backend;
    } finally {
      // Allow a retry if the fetch failed; clear the in-flight promise.
      if (this.#backend === undefined) {
        this.#loading = undefined;
      }
      this.#hooks.onLoadEnd?.();
    }
  }
}
