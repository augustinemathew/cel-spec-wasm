// The browser compile backend: runs `compiler.wasm` (the CEL compiler
// cross-compiled to wasm32-wasi) entirely client-side, so a static page —
// e.g. on GitHub Pages — can compile CEL to a portable Program with no
// server. It implements the same {@link CompileBackend} contract as the
// native-CLI backend, so `compile()` is backend-agnostic.
//
// `compiler.wasm` is a wasi-sdk *reactor* exporting the `cew_*` functions
// (see `bindings/c/compiler_wasm_exports.cc`). This backend provides a
// minimal hand-written WASI shim (no `node:wasi`, which is Node-only),
// runs the C++ static constructors via `__wasm_call_ctors`, marshals the
// source + variable declarations through linear memory, and reads the
// Program bytes back.
//
// KNOWN LIMITATION — error diagnostics. cel-cpp's parser (ANTLR) uses C++
// exceptions, and stock wasi-sdk ships libc++abi without an exception
// runtime, so an INVALID expression unwinds into the `__cxa_*` stub
// instead of returning cel-cpp's diagnostic. This backend catches that,
// discards the (now-corrupt) instance, and throws a generic
// CelCompileError. Full diagnostics need the emscripten build (which has
// the exception runtime); until then use the native/subprocess backend
// when precise errors matter.

import { CelCompileError, type Diagnostic } from '../errors.js';

import type { CompileBackend, CompileRequest } from './cli-backend.js';

/** The `cew_*` reactor surface exported by `compiler.wasm`. */
interface CompilerExports {
  readonly memory: WebAssembly.Memory;
  readonly __wasm_call_ctors: () => void;
  readonly cew_alloc: (n: number) => number;
  readonly cew_free: (p: number) => void;
  readonly cew_compile: (sourcePtr: number, varDeclsPtr: number) => number;
  readonly cew_program: () => number;
  readonly cew_error: () => number;
  readonly cew_reset: () => void;
}

/** A no-op WASI preview1 errno. */
const ERRNO_SUCCESS = 0;
/** WASI `EBADF` — used to end libc's preopen-directory scan (no preopens). */
const ERRNO_BADF = 8;

/**
 * The WASI preview1 imports `compiler.wasm` needs to run a pure
 * computation (no real filesystem / env). `getMemory` is read lazily so
 * the shim sees the instance's memory after instantiation.
 */
function makeWasiShim(
  getMemory: () => WebAssembly.Memory,
): WebAssembly.ModuleImports {
  const view = (): DataView => new DataView(getMemory().buffer);
  return {
    args_sizes_get: (argc: number, bufSize: number): number => {
      const d = view();
      d.setUint32(argc, 0, true);
      d.setUint32(bufSize, 0, true);
      return ERRNO_SUCCESS;
    },
    args_get: (): number => ERRNO_SUCCESS,
    environ_sizes_get: (count: number, bufSize: number): number => {
      const d = view();
      d.setUint32(count, 0, true);
      d.setUint32(bufSize, 0, true);
      return ERRNO_SUCCESS;
    },
    environ_get: (): number => ERRNO_SUCCESS,
    clock_time_get: (_id: number, _prec: bigint, out: number): number => {
      view().setBigUint64(out, 0n, true);
      return ERRNO_SUCCESS;
    },
    clock_res_get: (_id: number, out: number): number => {
      view().setBigUint64(out, 1000n, true);
      return ERRNO_SUCCESS;
    },
    fd_fdstat_get: (): number => ERRNO_SUCCESS,
    fd_fdstat_set_flags: (): number => ERRNO_SUCCESS,
    fd_prestat_get: (): number => ERRNO_BADF,
    fd_prestat_dir_name: (): number => ERRNO_BADF,
    fd_close: (): number => ERRNO_SUCCESS,
    fd_seek: (): number => ERRNO_SUCCESS,
    fd_read: (
      _fd: number,
      _iovs: number,
      _n: number,
      nread: number,
    ): number => {
      view().setUint32(nread, 0, true);
      return ERRNO_SUCCESS;
    },
    fd_write: (
      _fd: number,
      iovs: number,
      n: number,
      nwritten: number,
    ): number => {
      // Pretend everything was written so libc's stdio doesn't loop.
      const d = view();
      let total = 0;
      for (let i = 0; i < n; i++) {
        total += d.getUint32(iovs + i * 8 + 4, true);
      }
      d.setUint32(nwritten, total, true);
      return ERRNO_SUCCESS;
    },
    path_open: (): number => ERRNO_BADF,
    poll_oneoff: (): number => ERRNO_SUCCESS,
    sched_yield: (): number => ERRNO_SUCCESS,
    random_get: (ptr: number, len: number): number => {
      const u = new Uint8Array(getMemory().buffer, ptr, len);
      for (let i = 0; i < len; i++) {
        u[i] = (i * 2654435761) & 0xff;
      }
      return ERRNO_SUCCESS;
    },
    proc_exit: (code: number): never => {
      throw new Error(`compiler.wasm called proc_exit(${String(code)})`);
    },
  };
}

/** Thrown by the `__cxa_*` stubs when cel-cpp tries to throw (invalid input). */
class WasmExceptionEscape extends Error {}

/**
 * The CEL compiler running in WebAssembly, client-side.
 *
 * Construct with {@link WasmCompileBackend.create} (compiles the module
 * once); each {@link compile} call marshals through a fresh-enough
 * instance. On the exception-escape path the instance is replaced.
 */
export class WasmCompileBackend implements CompileBackend {
  readonly #module: WebAssembly.Module;
  #instance: WebAssembly.Instance | null = null;

  private constructor(module: WebAssembly.Module) {
    this.#module = module;
  }

  /** Compile the `compiler.wasm` bytes (or accept a pre-compiled module). */
  static async create(
    wasm: BufferSource | WebAssembly.Module,
  ): Promise<WasmCompileBackend> {
    const module =
      wasm instanceof WebAssembly.Module
        ? wasm
        : await WebAssembly.compile(wasm);
    return new WasmCompileBackend(module);
  }

  async #instantiate(): Promise<WebAssembly.Instance> {
    // Holder breaks the import↔instance cycle: the WASI shim needs the
    // instance's memory at call time, but the instance can't exist until
    // its imports (the shim) do.
    const holder: { instance?: WebAssembly.Instance } = {};
    const getMemory = (): WebAssembly.Memory => {
      const inst = holder.instance;
      if (inst === undefined) {
        throw new Error('compiler.wasm memory accessed before instantiation');
      }
      return (inst.exports as unknown as CompilerExports).memory;
    };
    const imports: WebAssembly.Imports = {
      wasi_snapshot_preview1: makeWasiShim(getMemory),
      // cel-cpp/ANTLR's C++ exception ABI — unresolved in stock wasi-sdk.
      // These only fire on invalid input; throwing unwinds out to compile().
      env: {
        __cxa_allocate_exception: (): never => {
          throw new WasmExceptionEscape();
        },
        __cxa_throw: (): never => {
          throw new WasmExceptionEscape();
        },
        __cxa_init_primary_exception: (): never => {
          throw new WasmExceptionEscape();
        },
      },
      // wasi-threads: never used for a single-threaded compile.
      wasi: { 'thread-spawn': (): number => -1 },
    };
    holder.instance = await WebAssembly.instantiate(this.#module, imports);
    (holder.instance.exports as unknown as CompilerExports).__wasm_call_ctors();
    return holder.instance;
  }

  async compile(request: CompileRequest): Promise<Uint8Array> {
    if (this.#instance === null) {
      this.#instance = await this.#instantiate();
    }
    const ex = this.#instance.exports as unknown as CompilerExports;
    const mem = (): ArrayBuffer => ex.memory.buffer;
    const enc = new TextEncoder();

    const writeCString = (s: string): number => {
      const bytes = enc.encode(s);
      const ptr = ex.cew_alloc(bytes.length + 1);
      const dst = new Uint8Array(mem(), ptr, bytes.length + 1);
      dst.set(bytes);
      dst[bytes.length] = 0;
      return ptr;
    };
    const readCString = (ptr: number): string => {
      const u8 = new Uint8Array(mem());
      let end = ptr;
      while (u8[end] !== 0) end++;
      return new TextDecoder().decode(u8.subarray(ptr, end));
    };

    const varDecls = request.vars.map((v) => `${v.name}:${v.type}`).join('\n');

    let srcPtr = 0;
    let varsPtr = 0;
    try {
      srcPtr = writeCString(request.source);
      varsPtr = writeCString(varDecls);
      const len = ex.cew_compile(srcPtr, varsPtr);
      if (len < 0) {
        // The C ABI returned a real (caught) diagnostic — rare in wasm,
        // since most errors unwind to the WasmExceptionEscape path below.
        const message = readCString(ex.cew_error());
        const diag: Diagnostic = { message };
        throw new CelCompileError([diag], message);
      }
      // Copy the Program out before any further allocation can move memory.
      const program = new Uint8Array(mem(), ex.cew_program(), len).slice();
      ex.cew_reset();
      return program;
    } catch (err) {
      if (err instanceof WasmExceptionEscape) {
        // cel-cpp threw (invalid expression) and stock wasi-sdk has no
        // exception runtime to recover its diagnostic. The instance's
        // C++ heap is now inconsistent — replace it.
        this.#instance = null;
        const diag: Diagnostic = {
          message:
            'Invalid CEL expression (the wasm compiler cannot recover the ' +
            'detailed diagnostic; the native/emscripten backend reports ' +
            'line and column).',
        };
        throw new CelCompileError([diag], diag.message);
      }
      throw err;
    } finally {
      if (this.#instance !== null) {
        if (srcPtr !== 0) ex.cew_free(srcPtr);
        if (varsPtr !== 0) ex.cew_free(varsPtr);
      }
    }
  }
}
