// Generate DYNAMIC-link Program fixtures for the eval binding's dynamic
// test suite.
//
// A dynamic Program is a thin (~6 KB) expr module that imports the runtime
// helpers from the `cel` namespace instead of bundling them.  The compiler
// emits dynamic Programs via `cew_compile_opts(srcPtr, optionsPtr,
// optionsLen)` with a `link_mode` option record set to DYNAMIC.  This
// script drives `bazel-bin/bindings/c/compiler_wasm.wasm` from Node (the
// same WASI-shim pattern as `compiler/src/internal/wasm-backend.ts`) and
// compiles a curated subset of the static fixture manifest in dynamic link
// mode, writing each to `eval/fixtures/dynamic/<name>.wasm`.
//
// The compile-options blob is a sequence of records — `kind` (1 byte) +
// `len` (u32 LE) + `len` payload bytes — per `cew_compile_opts`'s
// `ApplyOptions` (`bindings/c/compiler_wasm_exports.cc`).  We emit a `v`
// record per `name:type` var-decl and an `l` (link-mode) record whose
// single payload byte is 0 = DYNAMIC.
//
// The subset is chosen so each dynamic fixture is the dynamic twin of a
// static fixture with the SAME source + activation (so the dynamic test
// can assert dynamic ≡ static parity).  Re-run only when the wire format
// or the curated subset changes; the .wasm files are committed.
//
// Usage:  node bindings/ts/scripts/gen-dynamic-fixtures.mjs
// (run from the repo root, after `bazel build //bindings/c:compiler_wasm`).

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, '../../..');
const COMPILER_WASM = resolve(
  REPO_ROOT,
  'bazel-bin/bindings/c/compiler_wasm.wasm',
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

// Build the compile-options blob: a `v` record per var-decl + an `l`
// link-mode record (payload byte 0 = DYNAMIC).
function buildOptionsBlob(compileVars) {
  const enc = new TextEncoder();
  const records = [];
  const pushRecord = (kind, payload) => {
    const header = new Uint8Array(5);
    header[0] = kind.charCodeAt(0);
    const n = payload.length;
    header[1] = n & 0xff;
    header[2] = (n >> 8) & 0xff;
    header[3] = (n >> 16) & 0xff;
    header[4] = (n >> 24) & 0xff;
    records.push(header, payload);
  };
  for (const decl of compileVars ?? []) {
    pushRecord('v', enc.encode(decl));
  }
  pushRecord('l', Uint8Array.of(0)); // 0 = DYNAMIC link mode
  const total = records.reduce((acc, r) => acc + r.length, 0);
  const blob = new Uint8Array(total);
  let off = 0;
  for (const r of records) {
    blob.set(r, off);
    off += r.length;
  }
  return blob;
}

function compileDynamic(inst, source, compileVars) {
  const ex = inst.exports;
  const enc = new TextEncoder();
  const mem = () => ex.memory.buffer;
  const writeBytes = (b) => {
    const p = ex.cew_alloc(b.length + 1);
    const dst = new Uint8Array(mem(), p, b.length + 1);
    dst.set(b);
    dst[b.length] = 0;
    return p;
  };
  const srcPtr = writeBytes(enc.encode(source));
  const blob = buildOptionsBlob(compileVars);
  const optsPtr = writeBytes(blob);
  const len = ex.cew_compile_opts(srcPtr, optsPtr, blob.length);
  if (len < 0) {
    const u8 = new Uint8Array(mem());
    let end = ex.cew_error();
    while (u8[end] !== 0) end++;
    const msg = new TextDecoder().decode(u8.slice(ex.cew_error(), end));
    throw new Error(`compile failed: ${msg}`);
  }
  const program = new Uint8Array(mem(), ex.cew_program(), len).slice();
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
