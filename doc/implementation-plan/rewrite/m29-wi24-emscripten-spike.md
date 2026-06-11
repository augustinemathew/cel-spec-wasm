# M29 WI-2.4 — `compiler.wasm` feasibility spike

> ## ⚡ UPDATE 2026-06-11 — the port is DONE (via wasi-sdk, not emscripten)
>
> The "multi-day port" estimate below was **wrong by an order of
> magnitude** — but only because it assumed an *out-of-tree emscripten*
> toolchain. The repo's **existing wasi-sdk bazel toolchain** already
> cross-compiles the entire compiler. The actual work was **~1 hour**:
>
> 1. The whole frontend — cel-cpp parser + checker + **protobuf** +
>    ANTLR4 + RE2 + absl — cross-compiles to wasm32-wasi with just
>    `--cxxopt=-frtti --cxxopt=-fexceptions` (the toolchain disables both
>    by default for the tiny C runtime). `//compiler/frontend:parse_and_check`
>    → 823 actions, 0 errors.
> 2. **Binaryen** (the foreign_cc CMake dep) needed four macOS-host flags
>    scrubbed from `wasm_clang.sh` (`-arch`, `-Wl,-search_paths_first`,
>    `-Wl,-headerpad_max_install_names`, `-mmacosx-version-min=`) — CMake
>    leaks them from Apple-host detection; `wasm-ld` rejects them.
> 3. `//bindings/c:cel_capi` (the full compiler + C ABI) → **builds clean
>    under wasm32-wasi**, and `//bindings/c:compiler_wasm` produces a
>    **35.7 MB `compiler.wasm`** (a valid WebAssembly module).
>
> So WI-2.4 is **GO, in-tree, hermetic** — no emscripten, no out-of-tree
> toolchain. The new pieces: a `wasm_cpp_cc_binary` transition macro
> (platform + exceptions/RTTI, scoped so the runtime keeps its minimal
> flags), `compiler_main.cc` (a WASI CLI over the C ABI), and the
> `wasm_clang.sh` Apple-flag scrub.
>
> **Remaining to a working browser backend (wiring + optimization, no
> research risk):** (a) make the wasm *callable* — the toolchain links a
> reactor (no `_start`), so either export the C ABI functions + marshal
> from JS, or split the toolchain's `-nostartfiles -Wl,--no-entry` into a
> disable-able feature for a command-model `_start`; plus a `wasi:
> thread-spawn` stub (never called for single-threaded compile) and
> confirm the `env.__cxa_*` exception path. (b) the `compileWasm()` TS
> backend over `node:wasi` / a browser WASI shim, behind the existing
> `CompileBackend`. (c) **shrink it** — 35 MB is heavy for a browser
> download; `-O`/strip/LTO + the demo lazy-loading it.
>
> The emscripten investigation below is preserved as history; the
> wasi-sdk path supersedes it.

---

Status: spike complete — 2026-06-11. **Original verdict: CONDITIONAL GO,
but NOT for v1** (superseded by the update above). The codegen+optimize
half (Binaryen) is proven feasible under emscripten today; the
parse+check half (cel-cpp + protobuf + ANTLR4) is a multi-day toolchain
port that is NOT on the v1 critical path. v1 ships the demo on the
**N-API native backend (WI-2.2/2.3)** plus a thin local compile endpoint
(WI-4.1); the emscripten backend is a follow-up that upgrades the demo to
"no server" once the parse/check port lands.

This doc is the go/no-go required by WI-2.4's done-when. It records what
was proven, the exact remaining blocker, an effort estimate to clear it,
and the fallback confirmation.

---

## 1 What this spike was asked to determine

WI-2.4 (`m29-typescript-bindings.md` §B Phase 2, §A.2): cross-compile the
C ABI + C++ `Compiler` + cel-cpp + Binaryen to a `compiler.wasm` so a
browser can compile CEL→Program fully client-side. Flagged in the doc as
"the one genuinely hard, possibly-multi-day piece — time-boxed, with a
fallback so it never blocks the demo." This spike is the time-boxed
investigation, not the implementation.

The repo's layering rule makes this *architecturally* sound: `compiler/`
depends only on `shared/` + cel-cpp + Binaryen — never on `eval/` or
wasmtime — so `compiler.wasm` is a reachable target in principle (root
`CLAUDE.md`). The spike asks whether it is reachable *in practice* under
emscripten.

## 2 Environment findings

- **emscripten was NOT installed** (`emcc: command not found`; no `EMSDK`,
  no brew keg). It is **not** a repo build dependency and must not become
  one — the core build forbids platform-specific deps
  (`feedback_runtime_cross_platform`; root `CLAUDE.md`). emscripten is an
  **optional stretch backend**, kept out-of-tree.
- **It is obtainable without a heavy/system-specific install.** `git clone
  https://github.com/emscripten-core/emscripten` (emsdk) + `./emsdk
  install latest && ./emsdk activate latest` pulls a self-contained
  toolchain (its own pinned clang 6.0.0 + node 22 + python 3.13) under the
  emsdk dir — no system clang/llvm, no brew. Measured: ~2-3 min, a few
  hundred MB, exit 0 on darwin-arm64. An embedder building the browser
  backend installs emsdk + cmake locally; nothing changes in the hermetic
  bazel build.

## 3 What was proven (the de-risked half)

All probes ran on darwin-arm64 with emscripten 6.0.0. Reproduce via
`bindings/c/emscripten/build_binaryen_probe.sh` (committed; runs end-to-end).

### 3.1 Trivial C++ → wasm → Node works
`em++ -O2 hello.cc` → `node hello.js` prints `compute=3`. Baseline sanity.

### 3.2 Binaryen — the heaviest dep — builds CLEANLY under emscripten ✅
Binaryen version_129 (the `MODULE.bazel` pin) configured via `emcmake
cmake` and built via `emmake make` to a **67 MB `libbinaryen.a`** (the
full optimizer + the stable C API `binaryen-c.h`), exit 0, zero source
patches. Binaryen has **first-class emscripten support in its own
CMakeLists** (it ships `binaryen.js`): it auto-disables threads
(`EMSCRIPTEN_ENABLE_PTHREADS=OFF` default), enables LTO, and handles EH —
exactly the single-threaded browser profile we want.

### 3.3 Binaryen's C API RUNS inside an emscripten wasm module ✅
A tiny driver linked against the emscripten-built `libbinaryen.a` builds
`(func $eval (result i32) (i32.add (i32.const 1) (i32.const 2)))`,
`BinaryenModuleValidate`s it, runs `BinaryenModuleOptimize` at -O2, and
`BinaryenModuleAllocateAndWrite`s it — **all inside the wasm module,
executed in Node** — emitting a valid 37-byte wasm artifact. This is the
exact codegen+optimize pattern `compiler/codegen` + the `optimize_level`
knob use. **The codegen+optimize half of the compiler is emscripten-feasible
today.**

### 3.4 The C++ feature set protobuf/cel-cpp need works under emscripten ✅
A probe exercising RTTI (`dynamic_cast`), C++ exceptions
(`throw`/`catch`), and `std::mutex` compiled and ran both with `-pthread`
and (the browser target) **without** `-pthread` — `std::mutex` degrades
to a no-op in the single-threaded build, which is precisely what
protobuf's descriptor pool and cel-cpp's checker need in a browser tab.
RTTI + exceptions are the two features most likely to block a large C++
port; both are green.

### 3.5 The static-link path needs no wasm-in-wasm at compile time ✅
`compiler/internal/compile.cc` embeds the stripped runtime as a **static
C byte array** (`runtime/cel_runtime_stripped_wasm_bytes.h`, generated by
a genrule). The static-link `Program` is assembled by Binaryen splicing
those bytes — no nested wasm engine is needed at compile time, so nothing
in the static-link path is hostile to running compile *inside* wasm.

## 4 The remaining blocker (the un-ported half)

The compile pipeline (`//compiler/internal:compile`) links, beyond
Binaryen + absl:

- `//compiler/frontend:parse_and_check` → cel-cpp's **parser** (ANTLR4
  C++ runtime) + **type-checker** + **`common/`** (depends on
  **protobuf** — full, not lite — for the descriptor pool, and **RE2**).
- `//abi:cel_abi_cc_proto` + `//abi:runtime_catalogue_cc_proto` →
  generated **protobuf** C++.

None of these were built under emscripten in this spike — that is the
genuinely hard, multi-day work, and the time-box was spent proving the
Binaryen half rather than half-finishing the cel-cpp half. The specific
obstacles, in descending risk order:

1. **It's a bazel tree, not a CMake tree.** Binaryen ported trivially
   because it owns a CMakeLists with emscripten support. cel-cpp +
   protobuf + ANTLR4 + the generated `cc_proto_library` targets are built
   by **bazel**. Porting them under emscripten means one of:
   - (a) **An emscripten bazel `cc_toolchain`** — a second cross-compile
     toolchain parallel to the existing `//third_party/wasi_sdk`
     wasm32-wasi one (Phase C built that for the *runtime*; this is a
     *different* toolchain because emscripten ≠ wasi-sdk: different libc,
     different sysroot, different default imports). ~50 features/flags to
     wire, per the Phase C Path-A experience, plus protobuf's
     `cc_proto_library` aspect must resolve `protoc` (an exec-config tool)
     while the C++ output cross-compiles. This is the cleanest long-term
     shape but is itself a multi-day milestone.
   - (b) **A non-bazel emcc/CMake build** that re-compiles cel-cpp +
     protobuf + ANTLR4 + absl + RE2 from source out-of-tree (the same
     out-of-tree posture as this Binaryen probe), linking the WI-2.1 C ABI
     object. Lower bazel-integration cost, but duplicates the dep graph
     bazel already expresses and re-solves protobuf's own build.

2. **protobuf (full) is the highest-risk single dep.** The descriptor pool
   pulls in the bulk of libprotobuf. protobuf *does* build under
   emscripten in the wild, but it is large and historically needs
   `-fexceptions -frtti` (both proven green here) plus care around
   `std::thread`/`std::filesystem`. Phase C cross-compiled absl + RE2 to
   wasm32-wasi but **did not** cross-compile protobuf — the runtime
   doesn't need it. So protobuf-to-wasm is unproven in this repo on *any*
   toolchain.

3. **ANTLR4 C++ runtime** uses threads + a `ParserATNSimulator` cache;
   single-threaded builds exist but it's another from-source port with its
   own patch surface (the repo already patches ANTLR for the native build:
   `third_party/patches/antlr-cpp-runtime.patch`).

**No single error text "stops the build"** because the build was never
attempted end-to-end — that attempt *is* the multi-day work, and the
time-box correctly stopped before sinking days into a toolchain the
fallback makes optional. The probes above instead establish that the
*hard-to-predict* risks (Binaryen optimizer under wasm, RTTI/EH/mutex)
are all green, leaving the remaining work as *known, bounded toolchain
plumbing* rather than an unknown-feasibility gamble.

## 5 Effort estimate to clear it

- **Path (b), out-of-tree emcc/CMake** (recommended if/when pursued):
  ~3-5 focused days. Breakdown: protobuf-emscripten build + link (the
  long pole, ~1-2 d, mostly its own build system + dependency closure);
  ANTLR4 runtime build (~0.5 d, patch already exists to port); cel-cpp
  parser+checker+common compile against those (~1 d, expect a handful of
  `<filesystem>`/thread guards); wire the WI-2.1 C ABI + Binaryen (already
  proven) into one `compiler.wasm` and a Node/browser harness (~0.5-1 d).
- **Path (a), emscripten bazel `cc_toolchain`**: ~5-8 days, but yields a
  hermetic, reusable toolchain and rides protobuf's existing
  `cc_proto_library` wiring. Higher ceiling, higher floor.

Either way it is a **standalone follow-up milestone**, not a v1 line item.

## 6 Fallback confirmation (the v1 path)

**Confirmed: the demo's v1 fallback is the right path and is NOT
blocked by this spike.** Per `m29-typescript-bindings.md` §A.2 / WI-4.1,
the browser demo compiles via:

- **N-API native addon (WI-2.2) + TS compiler API (WI-2.3)** over the
  WI-2.1 C ABI, fronted by a **thin local compile endpoint** in the Vite
  dev server (WI-4.1 owns it). Eval is already pure-TS and runs fully
  client-side (Phase 1), so only *compile* needs the round-trip, and only
  until the emscripten backend lands behind the same TS interface.

The two-backend architecture (§A.2) was designed precisely so WI-2.4 can
stay open without blocking: `bindings/ts/compiler` exposes one interface;
the N-API backend ships now, the `compiler.wasm` backend slots in later.
**Nothing in the demo's critical path depends on this spike converging.**

## 7 Artifacts produced by this spike

- `bindings/c/emscripten/build_binaryen_probe.sh` — reproducible script
  that installs-nothing (takes `EMSDK_DIR` + `BINARYEN_SRC`), builds
  `libbinaryen.a` under emscripten, links the `binaryen-c.h` driver, and
  runs it in Node. Verified end-to-end (`emitted wasm bytes = 37`).
- This doc.

Deliberately NOT produced: a full `compiler.wasm` (the parse/check port is
the multi-day work above) and any `compileWasm()` TS backend (WI-2.4's TS
half — gated on the port). The `bindings/c/cel_capi.{h,cc}` C ABI (WI-2.1)
and the main `bindings/c/BUILD.bazel` are owned by WI-2.1 and untouched
here; the emscripten build will consume that C ABI once both land.

## 8 Future work

- Port the parse/check half (protobuf + ANTLR4 + cel-cpp) under emscripten
  — Path (b) recommended; ~3-5 d; standalone milestone.
- Once `compiler.wasm` exists, add the `compileWasm()` backend in
  `bindings/ts/compiler` behind the existing two-backend interface and
  flip the demo from the local endpoint to client-side compile.
- Decide N-API-vs-wasm default per environment (native where available,
  wasm in pure-browser).
