// Generate DYNAMIC-link Program fixtures for the eval binding's dynamic
// test suite.
//
// A dynamic Program is a thin (~6 KB) expr module that imports the runtime
// helpers from the `cel` namespace instead of bundling them.  The compiler
// emits dynamic Programs via `cew_compile(requestPtr, requestLen)` with a
// serialized `celwasm.compile.CompileRequest` whose `link_mode` is
// DYNAMIC (`bindings/c/compiler/compile_request.proto`).  This
// script drives `bazel-bin/bindings/c/compiler/compiler_wasm.wasm` from Node (the
// same WASI-shim pattern as `compiler/src/internal/wasm-backend.ts`) and
// compiles a curated subset of the static fixture manifest in dynamic link
// mode, writing each to `eval/fixtures/dynamic/<name>.wasm`.
//
// The request bytes come from the compiler binding's own encoder
// (`compiler/dist/internal/compile-request.js` — the built output of
// `compile-request.ts`), so this script can never drift from the schema
// the backend ships.
//
// The subset is chosen so each dynamic fixture is the dynamic twin of a
// static fixture with the SAME source + activation (so the dynamic test
// can assert dynamic ≡ static parity).  Re-run only when the wire format
// or the curated subset changes; the .wasm files are committed.
//
// Usage:  node bindings/ts/scripts/gen-dynamic-fixtures.mjs
// (run from the repo root, after `bazel build //bindings/c/compiler:compiler_wasm`
// and `npm run build` in bindings/ts).

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

import { encodeCompileRequest } from '../compiler/dist/internal/compile-request.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, '../../..');
const COMPILER_WASM = resolve(
  REPO_ROOT,
  'bazel-bin/bindings/c/compiler/compiler_wasm.wasm',
);
const FIXTURES_DIR = resolve(HERE, '../eval/fixtures');
const DYNAMIC_DIR = resolve(FIXTURES_DIR, 'dynamic');

// The static fixtures to produce dynamic twins of (by manifest `name`).
// One per value shape: scalar, variable, comprehension, map, string.
const SUBSET = [
  'int_add', // scalar:  1 + 2
  'var_int_add', // variable: x + y
  'list_map_double', // comprehension: [1,2,3].map(x, x*2)
  'map_index', // map: {"a":1,"b":2}["a"]
  'string_concat', // string: "hello" + " world"
];

const ERRNO_SUCCESS = 0;
const ERRNO_BADF = 8;

function makeWasiShim(getMemory) {
  const view = () => new DataView(getMemory().buffer);
  return {
    args_sizes_get: (argc, bufSize) => {
      const d = view();
      d.setUint32(argc, 0, true);
      d.setUint32(bufSize, 0, true);
      return ERRNO_SUCCESS;
    },
    args_get: () => ERRNO_SUCCESS,
    environ_sizes_get: (count, bufSize) => {
      const d = view();
      d.setUint32(count, 0, true);
      d.setUint32(bufSize, 0, true);
      return ERRNO_SUCCESS;
    },
    environ_get: () => ERRNO_SUCCESS,
    clock_time_get: (_id, _prec, out) => {
      view().setBigUint64(out, 0n, true);
      return ERRNO_SUCCESS;
    },
    clock_res_get: (_id, out) => {
      view().setBigUint64(out, 1000n, true);
      return ERRNO_SUCCESS;
    },
    fd_fdstat_get: () => ERRNO_SUCCESS,
    fd_fdstat_set_flags: () => ERRNO_SUCCESS,
    fd_prestat_get: () => ERRNO_BADF,
    fd_prestat_dir_name: () => ERRNO_BADF,
    fd_close: () => ERRNO_SUCCESS,
    fd_seek: () => ERRNO_SUCCESS,
    fd_read: (_fd, _iovs, _n, nread) => {
      view().setUint32(nread, 0, true);
      return ERRNO_SUCCESS;
    },
    fd_write: (_fd, iovs, n, nwritten) => {
      const d = view();
      let total = 0;
      for (let i = 0; i < n; i++) total += d.getUint32(iovs + i * 8 + 4, true);
      d.setUint32(nwritten, total, true);
      return ERRNO_SUCCESS;
    },
    path_open: () => ERRNO_BADF,
    poll_oneoff: () => ERRNO_SUCCESS,
    sched_yield: () => ERRNO_SUCCESS,
    random_get: (ptr, len) => {
      const u = new Uint8Array(getMemory().buffer, ptr, len);
      for (let i = 0; i < len; i++) u[i] = (i * 2654435761) & 0xff;
      return ERRNO_SUCCESS;
    },
    proc_exit: (code) => {
      throw new Error(`compiler.wasm proc_exit(${code})`);
    },
  };
}

async function instantiate(mod) {
  const holder = {};
  const getMemory = () => holder.instance.exports.memory;
  holder.instance = await WebAssembly.instantiate(mod, {
    wasi_snapshot_preview1: makeWasiShim(getMemory),
    env: {
      __cxa_allocate_exception: () => {
        throw new Error('cel-cpp threw (invalid input)');
      },
      __cxa_throw: () => {
        throw new Error('cel-cpp threw (invalid input)');
      },
      __cxa_init_primary_exception: () => {
        throw new Error('cel-cpp threw (invalid input)');
      },
    },
    wasi: { 'thread-spawn': () => -1 },
  });
  holder.instance.exports.__wasm_call_ctors();
  return holder.instance;
}

function compileDynamic(inst, source, compileVars) {
  const ex = inst.exports;
  const mem = () => ex.memory.buffer;
  // The manifest carries `name:type` decl strings; the request wants
  // {name, type} pairs (split on the FIRST ':' — types contain none).
  const vars = (compileVars ?? []).map((decl) => {
    const colon = decl.indexOf(':');
    return { name: decl.slice(0, colon), type: decl.slice(colon + 1) };
  });
  const request = encodeCompileRequest({ source, vars, linkMode: 'dynamic' });
  const requestPtr = ex.cew_alloc(request.length);
  new Uint8Array(mem(), requestPtr, request.length).set(request);
  const len = ex.cew_compile(requestPtr, request.length);
  if (len < 0) {
    const u8 = new Uint8Array(mem());
    let end = ex.cew_error();
    while (u8[end] !== 0) end++;
    const msg = new TextDecoder().decode(u8.slice(ex.cew_error(), end));
    throw new Error(`compile failed: ${msg}`);
  }
  const program = new Uint8Array(mem(), ex.cew_program(), len).slice();
  ex.cew_free(requestPtr);
  ex.cew_reset();
  return program;
}

async function main() {
  const manifest = JSON.parse(
    readFileSync(resolve(FIXTURES_DIR, 'manifest.json'), 'utf-8'),
  );
  const byName = new Map(manifest.fixtures.map((f) => [f.name, f]));

  const mod = await WebAssembly.compile(readFileSync(COMPILER_WASM));
  const inst = await instantiate(mod);

  mkdirSync(DYNAMIC_DIR, { recursive: true });
  for (const name of SUBSET) {
    const fixture = byName.get(name);
    if (fixture === undefined) {
      throw new Error(`manifest has no fixture named '${name}'`);
    }
    const program = compileDynamic(inst, fixture.expr, fixture.compileVars);
    const m = new WebAssembly.Module(program);
    const imp = WebAssembly.Module.imports(m);
    const isDyn = imp.some((i) => i.module === 'cel');
    if (!isDyn) {
      throw new Error(`compiled '${name}' is not dynamic (no cel.* import)`);
    }
    writeFileSync(resolve(DYNAMIC_DIR, `${name}.wasm`), program);
    console.log(
      `wrote dynamic/${name}.wasm  (${program.length} bytes, ` +
        `cel.imports=${imp.filter((i) => i.module === 'cel').length})`,
    );
  }
}

await main();
