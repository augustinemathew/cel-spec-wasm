// M19 Probe P-4 (keystone) — end-to-end scalar eval of a compiled
// Program in Node, with ZERO npm deps (pure `WebAssembly` + node:fs).
//
// Proves the §4.4 two-module, runtime-owns-memory instantiation can be
// reproduced in TS: instantiate cel_runtime.wasm (it owns + exports the
// shared memory), bind its `cel_*` exports + memory into the expr
// module's `cel.*` / `cel.memory` imports, stub `cel_host.*` / WASI,
// call `$eval`, and decode the 24-byte CelValue at the returned offset.
//
// Mirrors compiler_v2/api/engine.cc (InstantiateRuntime / SeedRuntimeArena
// / InstantiateExpr) + instance.cc (DecodeCelValueAt) — cited inline.
//
// Run:
//   bazel build //compiler_v2/runtime:cel_runtime_wasm
//   node compiler_v2/api/eval/ts/probes/m19/p4_scalar_eval.mjs \
//        bazel-bin/compiler_v2/runtime/cel_runtime_wasm.wasm
//
// Throwaway probe — delete at M19 closeout. NOT the permanent suite.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const runtimePath =
  process.argv[2] ??
  "bazel-bin/compiler_v2/runtime/cel_runtime_wasm.wasm";

// CelKind (runtime/cel_data.h:31). Only the scalars P-4 needs.
const CEL_INT = 2;

// Arena capacity — cel_layout.h:41 (64 KiB).
const CELWASM_ARENA_CAPACITY_BYTES = 64 * 1024;

// A stub that throws if a not-yet-implemented host trampoline is ever
// *called* (it must still be *defined* to instantiate). For "1 + 2"
// none of cel_host.* is reached, so a throw here that never fires is
// the correctness signal. Mirrors the trap-stub policy in §0.5 Slice B.
function trapStub(modName, fnName) {
  return (...args) => {
    throw new Error(`UNEXPECTED call into stub ${modName}.${fnName}(${args})`);
  };
}

// A Proxy import namespace: any field access yields a trap-stub named
// after the requested field. Covers every cel_host.* / wasi.* name the
// module declares without enumerating them by hand.
function trapNamespace(modName, overrides = {}) {
  return new Proxy(overrides, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (typeof prop !== "string") return undefined;
      return trapStub(modName, prop);
    },
    has() {
      return true;
    },
  });
}

// Decode a 24-byte CelValue at `offset` — the scalar arms of
// instance.cc:260 DecodeCelValueAt. Layout (cel_data.h:109):
//   +0  u32 kind
//   +4  u32 _pad
//   +8  union payload (16 bytes) — i is i64 at +8.
function decodeScalar(view, offset) {
  const kind = view.getUint32(offset, /*littleEndian=*/ true);
  switch (kind) {
    case CEL_INT:
      return { kind, i: view.getBigInt64(offset + 8, true) };
    default:
      return { kind, raw: "kind not decoded by this probe" };
  }
}

async function main() {
  const exprPath = join(HERE, "add.wasm"); // compiled `1 + 2`
  const runtimeBytes = readFileSync(runtimePath);
  const exprBytes = readFileSync(exprPath);

  // ── Step 1-3 (engine.cc InstantiateRuntime): instantiate the runtime.
  // It DEFINES + EXPORTS its own memory (no imported memory — P-3).
  //
  // FINDING (P-4): the runtime does NOT run __wasm_call_ctors at
  // instantiation. wasi-libc's `.command_export` wrappers run ctors
  // lazily on the FIRST exported call (here, arena_init); ctors call
  // __wasilibc_init_ssp → `random_get` to seed the stack canary. So
  // `random_get` must be a REAL impl, not a trap. Memory doesn't exist
  // until after instantiation, but random_get only fires during
  // arena_init (post-instantiate), so a late-bound `mem` is safe.
  let mem = null; // set to runtime's exported WebAssembly.Memory below
  const runtimeImports = {
    cel_env: { cel_log: () => {} }, // 9-arg diagnostic; no-op (cel_log.h)
    cel_host: trapNamespace("cel_host"),
    wasi_snapshot_preview1: trapNamespace("wasi_snapshot_preview1", {
      proc_exit: () => {},
      // errno=0 (success) for the benign env/clock probes if reached.
      random_get: (ptr, len) => {
        crypto.getRandomValues(new Uint8Array(mem.buffer, ptr, len));
        return 0;
      },
    }),
  };
  const { instance: runtime } = await WebAssembly.instantiate(
    runtimeBytes,
    runtimeImports,
  );
  const rx = runtime.exports;
  mem = rx.memory; // shared WebAssembly.Memory, owned by runtime
  const memory = mem;

  // ── Step 5 (SeedRuntimeArena): arena_init exactly once (also triggers
  // lazy ctor init → random_get).
  rx.arena_init(CELWASM_ARENA_CAPACITY_BYTES);

  // ── Step 7 (InstantiateExpr): bind the runtime's exports as the
  // expr module's `cel.*` imports + `cel.memory`. The expr module
  // imports EVERY runtime helper (no lazy imports — repo rule), so we
  // hand it all of rx plus the shared memory. cel_host.* stays stubbed.
  const celNs = { memory };
  for (const [name, val] of Object.entries(rx)) {
    if (typeof val === "function") celNs[name] = val;
  }
  const exprImports = {
    cel: celNs,
    cel_host: trapNamespace("cel_host"),
  };
  const { instance: expr } = await WebAssembly.instantiate(
    exprBytes,
    exprImports,
  );

  // ── eval (): zero args, returns the i32 byte offset of the result
  // CelValue (instance.cc:1006).
  const offset = expr.exports.eval();
  const view = new DataView(memory.buffer);
  const result = decodeScalar(view, offset);

  // ── assert
  const ok = result.kind === CEL_INT && result.i === 3n;
  console.log("node", process.version);
  console.log("runtime bytes:", runtimeBytes.length, "expr bytes:", exprBytes.length);
  console.log("eval() -> offset", offset, "decoded:", JSON.stringify(result, (_k, v) => typeof v === "bigint" ? v.toString() : v));
  if (!ok) {
    console.log("P-4 FAILED ❌  expected {kind:2 (CEL_INT), i:3}");
    process.exit(1);
  }
  console.log("P-4 PASS ✅  `1 + 2` == 3 end-to-end through the TS-reproduced wiring");
}

main().catch((e) => {
  console.error("P-4 ERROR ❌:", e);
  process.exit(1);
});
