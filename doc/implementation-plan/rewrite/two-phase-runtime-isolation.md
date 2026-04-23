# Two-phase runtime isolation — restoring the wasm sandbox

Status: **shipped 2026-04-22 on branch `feat/two-phase-engine-shared`,
7 commits.  Plan-vs-execution deltas are documented inline below.**

Parent: `design.md` (this directory).
Predecessor: `predecessor-memory-ownership-flip.md`.
Sibling: `m1-scalar-pipeline.md` (the M1 doc this updates §2.9 of).

## 0. What this is

Restore the wasm sandbox boundary that commit 040c043
("compiler_v2: native-runtime trampolines + runtime memcpy fix")
gave up. The current host binary contains the runtime as native C
code reachable through `cel.cel_reset` / `cel.cel_alloc` host
trampolines holding raw pointers into wasm linear memory. Every
M3+ runtime helper (string ops, list ops, map ops, comprehensions)
will land the same way under the current architecture. The host
attack surface grows linearly with the runtime; that is the wrong
direction.

This plan restores the `cel_runtime.wasm` module on the eval path
and runs all non-host runtime helpers inside the wasm sandbox. Host
imports collapse to an enumerated set: `cel_env.cel_log` plus the
explicit user-declared functions and the small set of host-resident
ABI helpers (proto field reads — proto messages are host objects
and can't be otherwise).

This is **not** a re-architecture. The user-facing
`Compiler` / `Program` / `Instance` / `RuntimeBindings` surface
defined in `cel-host-surface.md` does not change. What changes is
the wasmtime state distribution behind that surface and the
codegen's handling of `cel.cel_*` imports.

## 1. Why now

Three forces converge:

  1. **Security.** Every runtime helper added to native trampolines
     is one more piece of code that runs unsandboxed with raw access
     to wasm memory and host objects. M3 already added trampolines
     for `cel_field_read_int`, `cel_unwrap_message`, etc. The
     trajectory is for the host to grow until "the runtime" is
     mostly host code.
  2. **Cost.** Settled by experiment (see §2). Caching the parsed
     runtime module on a shared engine makes per-Create cost
     under (A) ~12 µs versus current ~6 µs. The 2× factor is
     recoverable in the eval path (cross-module wasm calls beat
     host trampoline crossings); the >34× win comes from the
     module-cache. Module reparse without caching is 186 µs/Create
     — that's the path we have to avoid, and the design here makes
     it impossible to hit by construction.
  3. **Forward dead-end.** The current architecture commits us to
     adding host trampolines for every runtime helper through M8.
     Reversing course later costs more than reversing now.

## 2. What's settled by experiment

Two artifacts on branch `experiment/two-phase-shared-memory-smoke`
(forked from 800ef76 + cherry-pick of the runtime memcpy fix from
040c043):

  - `compiler_v2/host/two_phase_shared_memory_smoke_test.cc` —
    proves a host-allocated `wasmtime_memory_t` imported by both
    a runtime instance and an expr instance in the same store
    actually shares bytes correctly, that two such instances are
    fully isolated from each other, and that the wiring order
    (host trampolines → bind `cel.memory` → instantiate runtime →
    bind runtime exports as `cel.cel_*` → instantiate expr)
    side-steps the "circular import" 040c043 cited.
  - `compiler_v2/host/two_phase_topology_bench.cc` — settles the
    cost question. Numbers (darwin-arm64, opt build, 1s
    min/bench):

    | Bench | Wall (ns) |
    |---|---|
    | `BM_EngineNew` | 1,267 |
    | `BM_ModuleNew_RealRuntime` | 166,225 |
    | `BM_ModuleNew_TinyExpr` | 150,310 |
    | `BM_StoreNew` | 149 |
    | `BM_MemoryNew_2Pages` | 2,397 |
    | `BM_LinkerSetup` | 1,147 |
    | `BM_InstantiateRuntime_PerCreate` | 9,973 |
    | `BM_InstantiateExpr_PerCreate` | 5,550 |
    | `BM_FullCreate_TwoPhase_Hot` (min runtime) | 5,530 |
    | `BM_FullCreate_TwoPhase_RuntimeReparsed` | 186,071 |
    | `BM_FullCreate_TwoPhase_ExprCacheMiss` | 153,137 |
    | `BM_FullCreate_TwoPhase_Cold` | 351,121 |
    | `BM_FullCreate_CurrentHead_Hot` | 5,534 |
    | `BM_FullCreate_CurrentHead_Cold` | 175,075 |
    | `BM_CallEval` | 44 |

    Bias note: the (A) Hot benches use a hand-written minimal
    WAT runtime instead of the real `cel_runtime.wasm` (the real
    one is broken — see §3). Real-runtime instantiate is
    measurable (`BM_InstantiateRuntime_PerCreate` minus
    store/memory/linker = ~6 µs vs ~1 µs for the min runtime),
    so realistic (A) Hot ≈ 12 µs and ExprCacheMiss ≈ 160 µs.
    The bench in §6 of this doc fixes that bias by running
    against the real runtime (after the precondition fix lands).

Architectural conclusions:

  - Engine and runtime module **must** be cached. Ratios are 64×
    and 34×. This is the structural decision.
  - Two-phase per-Create cost (~12 µs realistic) is fast enough
    for any realistic policy workload (>80K Creates/sec/core).
  - First-Plan-of-a-new-Program is ~160 µs (dominated by
    `wasmtime_module_new(expr_bytes)`). Acceptable; happens once
    per unique program at `Compile()` / `FromWasm()` time, not
    per Plan.
  - `CallEval` dispatch overhead is 44 ns — negligible.

## 3. Precondition: fix the runtime null-pointer-elision bug

Discovered while writing the smoke test. `cel_runtime.c::cel_memory_base_()`
returns `(uint8_t*)0` on the wasm32 build path because address 0
in wasm linear memory is the start of the imported memory. clang
treats this as a C null pointer and elides every store through
`*(uint32_t*)(0+off) = v` as undefined behavior. Disassembly of
the current `cel_runtime.wasm`:

```wat
(func $cel_reset (param i32 i32) (call $cel_log ...) ...)   ;; no i32.store at 8 or 12
(func $cel_alloc (param i32) (result i32) (call $cel_log ...) (unreachable))
```

`cel_reset` compiles to a no-op that calls cel_log; `cel_alloc`
unconditionally traps. Latent because at HEAD these exports are
never called — host trampolines do all the work. The moment we
restore the runtime to the eval path, this bug surfaces.

### 3.1 Fix

Block the optimizer's reasoning about the base address being a C
null. Cleanest: route through `uintptr_t` plus an `__asm__`
opacity barrier so the value-of-zero is loaded into a register
the optimizer can't see through.

```c
#ifdef __wasm__
static uint8_t* cel_memory_base_(void) {
  uintptr_t p = 0;
  __asm__("" : "+r"(p));   // optimizer can't assume p == NULL
  return (uint8_t*)p;
}
#endif
```

Alternative considered: use a `volatile`-qualified extern symbol
declared at link-time-fixed offset. Rejected — relies on more
wasm-ld behaviour (stable symbol resolution to a specific address)
than the asm barrier does.

### 3.2 Test

`compiler_v2/runtime/cel_runtime_wasm_test.cc` — new. Builds
`cel_runtime.wasm` via the genrule, instantiates it under wasmtime
with a host-allocated 2-page memory bound as `cel.memory` and a
no-op `cel_env.cel_log` trampoline, calls `cel_reset(64, 65536)`
followed by `cel_alloc(24)`. Asserts:

  - `cel_alloc` returns 64 (not 0).
  - Memory bytes 8..16 contain `{64, 65536}` little-endian after
    `cel_reset`.
  - Memory bytes 8..12 contain 88 after the alloc (cursor advanced).
  - Calling `cel_alloc` a second time returns 88.

This is the single test that exercises the wasm runtime directly
until §5 hooks it up through `Program::Plan`.

### 3.3 Lint backlog

`doc/implementation-plan/lint-backlog.md` — no new entries. The
asm barrier is one line in a freestanding C file; no function-size
or readability impact.

## 4. Where wasmtime state lives — mapping to the existing surface

`cel-host-surface.md` has four classes:
`Compiler` / `Program` / `RuntimeBindings` / `Instance`. None of
them currently names where `wasm_engine_t` and parsed runtime
`wasmtime_module_t` live. The bench tells us the answer.

> **Plan-vs-execution delta (2026-04-22):** The original cut here
> pinned `wasm_engine_t` + parsed runtime to `Compiler`.  During
> execution that was rejected as a role conflation — Compiler
> should be pure compile-time (no wasmtime), and the runtime side
> belongs on a separate class.  The shipped architecture
> introduces a fifth user-facing class, `cel::Engine`, that owns
> the wasm execution machinery; `cel::Compiler` stays
> wasmtime-free.  See §4.1 (revised) for the corrected cut.

### 4.1 The cut (revised)

| Owns | Class | Lifetime | Justification |
|---|---|---|---|
| `wasm_engine_t` | **`cel::Engine`** (private shared state) | Engine lifetime; outlives Instances via `shared_ptr` | One per Engine.  Process-shared (or per-tenant in multi-tenant hosts).  64× cold-to-hot speedup from sharing. Engines are wasmtime-thread-safe. |
| Parsed runtime `wasmtime_module_t` | **`cel::Engine`** (same shared state) | Same | One per engine; the 34× speedup.  Parsed once in `Engine::Builder::Build()` from `kCelRuntimeWasmBytes`.  Modules are wasmtime-thread-safe. |
| Wasm bytes + (future) ABI | **`cel::Program`** (value type) | Program lifetime | Program is **pure data** — no wasmtime ref.  Serializable; safe to ship between processes.  Constructed by `Compiler::Compile(source) → Program` or `Program(bytes)`. |
| Parsed expr `wasmtime_module_t` | **`cel::Instance`** (per-Plan, internal) | Instance lifetime | Plan does `wasmtime_module_new(program.wasm_bytes())` per call (M1 default).  An Engine-side parsed-expr-module cache is the named follow-up surfaced by §6 bench data. |
| Per-Plan handles: `wasmtime_store_t`, host-owned `wasmtime_memory_t`, `wasmtime_linker_t`, runtime instance, expr instance, `eval_fn` | `cel::Instance` | Instance lifetime | The per-Plan ~12 µs cost (~162 µs in current M1 because expr-parse is in this layer; ~12 µs after the cache lands). |

### 4.2 New public class: `cel::Engine`

The original plan said "no new class".  Execution proved otherwise
— the role conflation between Compiler and Engine wasn't
sustainable.  Final shape:

  - `cel::Compiler` — compile-time only.  No wasmtime dep.
    Build can run in a build server that never executes anything.
  - `cel::Engine` — runtime only.  Owns wasm engine + parsed
    runtime module.  Required to evaluate; not required to
    compile.

Users with two Compilers cost ~free; users with two Engines pay
~167 µs each (engine_new + runtime parse).  Process-level Engine
sharing is the rule.

### 4.3 Cross-class lifetime

`Program` holds `std::shared_ptr<Compiler::WasmtimeState>`.
Dropping the originating `Compiler` while a Program is still
alive is supported — the shared_ptr keeps engine + runtime module
alive until the last Program is gone. `Instance` holds a
`std::shared_ptr<Compiler::WasmtimeState>` *and* a back-reference
to its Program (which holds the parsed expr module the Instance
uses). Drop-Compiler-then-keep-Instance is supported by the
shared_ptr chain.

### 4.4 Thread safety

  - `Compiler` after `Build()`: immutable, freely shareable.
  - `Compiler::WasmtimeState`: wasmtime engines + modules are
    documented thread-safe for sharing.
  - `Program`: immutable; freely shareable. `Plan(bindings)` is
    safe to call concurrently from multiple threads (each Plan
    creates a fresh store).
  - `Instance`: thread-owned. Single-threaded per the existing
    doc. Bind one per worker thread.
  - `RuntimeBindings`: value type; pass by const-ref.

## 5. Implementation, commit by commit

Each commit is testable in isolation; CI runs after each. Order
designed so the new code path can be exercised end-to-end before
the old `host_loader.{h,cc}` is deleted.

> **As-shipped commit log** (branch `feat/two-phase-engine-shared`):
>
> | Plan SHA | Subject |
> |---|---|
> | A | `compiler_v2/runtime: fix null-pointer-elision in cel_memory_base_` |
> | B (revised) | `compiler_v2/api: cel::Engine + cel::Program + cel::Compiler` |
> | C (revised) | `compiler_v2/api: cel::Engine::Plan + cel::Instance` (+ amended for memory_size_bytes accessor and 8-thread × 4-Plan concurrent test) |
> | F+G | `compiler_v2: codegen flips memory ownership + delete host_loader` (merged because the codegen flip breaks `host_loader_test`) |
> | E | `compiler_v2/api: cel::Instance::Eval — full Compile→Plan→Eval` |
> | §6 | `compiler_v2/api: cel_pipeline_bench against new API surface` |
> | (lifetime) | `compiler_v2/api: strengthen InstanceOutlivesEngine to call Eval` |
> | H | `doc: reconcile m1-scalar-pipeline / cel-host-surface / design / two-phase-runtime-isolation` (this commit) |
>
> Plan vs. as-shipped deltas:
> - **B and C revised**: split into `cel::Engine` + `cel::Compiler`
>   instead of pinning wasmtime to Compiler.  See §4 delta.
> - **F + G merged**: codegen flip and host_loader deletion landed
>   together because flip breaks the old loader's tests.
> - **D dropped**: the original "Commit D — Program::Plan" became
>   "Commit C — Engine::Plan" under the revised architecture.
> - **`Cel::Compiler::LoadProgram` dropped**: superseded by
>   `Program(bytes)` ctor — Loading a program is a Program-data
>   operation, not a Compiler responsibility (no wasmtime needed
>   to construct a Program).

### 5.1 Commit A — runtime null-pointer-elision fix

  - `compiler_v2/runtime/cel_runtime.c`: §3.1 patch.
  - `compiler_v2/runtime/cel_runtime_wasm_test.cc`: §3.2 test.
  - `compiler_v2/runtime/BUILD.bazel`: add the new cc_test
    target. Tagged `manual` (depends on the wasm genrule).
  - `doc/implementation-plan/lint-backlog.md`: nothing to add.

Acceptance: `bazel test //compiler_v2/runtime:cel_runtime_wasm_test`
green; lint clean; existing host_loader tests still green.

### 5.2 Commit B — `Compiler::WasmtimeState`

Add private nested struct + `shared_ptr` member on `Compiler`.
No public API change.

```cpp
// compiler_v2/api/compiler.h (additions only)
class Compiler {
 public:
  // ... existing API unchanged ...

 private:
  // Process-shared wasmtime state.  Engines + parsed modules are
  // thread-safe per wasmtime docs; many Programs and the Instances
  // they Plan safely share these.  shared_ptr lets a Program (and
  // its Instances) outlive the Compiler that built it.
  struct WasmtimeState {
    wasm_engine_t* engine = nullptr;
    wasmtime_module_t* runtime_module = nullptr;
    ~WasmtimeState();
    WasmtimeState(const WasmtimeState&) = delete;
    WasmtimeState& operator=(const WasmtimeState&) = delete;
  };
  std::shared_ptr<WasmtimeState> wasmtime_;
  // ... existing decl fields ...
};
```

`Compiler::Builder::Build()`:

  - `wasm_engine_new`. On null: `Internal("wasm_engine_new")`.
  - `wasmtime_module_new(engine, kCelRuntimeWasmBytes,
    kCelRuntimeWasmBytesSize, ...)`. On error:
    `Internal("module_new(runtime)")` + cleanup engine.
  - Move into `std::make_shared<WasmtimeState>(...)`.

Tests in `compiler_v2/api/compiler_test.cc` (extending the
existing file):

  - `BuildSucceedsAndProducesEngine` — `Build()` returns OK
    Compiler.
  - `BuilderRunsOnceCheap` — second `Build()` after the first
    returns moved-from-error (existing builder semantics).

`compiler_v2/api/BUILD.bazel`: add wasmtime + cel_runtime_wasm_bytes
deps to the `compiler` target.

Acceptance: `bazel test //compiler_v2/api:compiler_test` green; no
other targets touched.

### 5.3 Commit C — `Program` carries parsed expr module

`Program` gets:
  - `std::shared_ptr<Compiler::WasmtimeState> wasmtime_;`
  - `wasmtime_module_t* expr_module_;`
  - existing `wasm_bytes_` and `abi_`.

Constructors:
  - `Compiler::Compile(source, opts)` calls
    `wasmtime_module_new(state->engine, bytes, ...)` after producing
    bytes; on error, `Internal("module_new(expr)")`.
  - `Program::FromWasm(bytes, state)` — note signature change:
    requires the Compiler's WasmtimeState. The prior signature
    `FromWasm(absl::string_view)` doesn't have an engine to parse
    against. Two options:
      - (a) Add the state arg (clean, breaks the documented
        signature).
      - (b) Make `FromWasm` a method on `Compiler` (`Compiler::
        LoadProgram(wasm_bytes)`).
    **Decision: (b).** Programs are always associated with a
    Compiler in practice; cross-Compiler reuse of a serialized
    Program isn't a real workflow (different Compilers might have
    different host import declarations). Update
    `cel-host-surface.md §2.2` to move `FromWasm` to Compiler as
    `Compiler::LoadProgram`.

Tests in `compiler_v2/api/program_test.cc` (extending):
  - `CompileParsesExprModule` — verify Compile returns a Program
    whose `expr_module_` is non-null (via friend or behavior
    test).
  - `LoadProgramRoundTrip` — Compile → bytes → LoadProgram → Plan
    → Eval, expect equivalent Value.
  - `LoadProgramMalformedBytes` — `FailedPrecondition` with
    "module_new(expr)" in message.
  - `ProgramOutlivesCompiler` — drop the Compiler, the Program
    still works (shared_ptr held).

Acceptance: `bazel test //compiler_v2/api:program_test` green.

### 5.4 Commit D — `Program::Plan` end-to-end

Implements the wiring order from the smoke test:

```
1. wasmtime_store_new(state.engine)
2. wasmtime_memory_new(ctx, type{min=2, max=2}) → host_owned_memory
3. wasmtime_linker_new(state.engine)
4. RegisterCelLog(linker)
5. (M5+ stub) RegisterCustomImports(linker, bindings)
6. wasmtime_linker_define(linker, ctx, "cel", "memory", &mem_ext)
7. wasmtime_linker_instantiate(linker, ctx, state.runtime_module) → runtime_inst
8. BindRuntimeExport(linker, runtime_inst, "cel_reset")
9. BindRuntimeExport(linker, runtime_inst, "cel_alloc")
10. wasmtime_linker_instantiate(linker, ctx, expr_module_) → expr_inst
11. wasmtime_instance_export_get(expr_inst, "eval") → eval_fn
12. return Instance(...)
```

Function-size: 5.5 lines plus 4 helper calls under the lint
ceiling. The existing 800ef76 split (`InitWasmtimeHandles` /
`InstantiateExprModule` / `InstantiateRuntimeModule`) is already
proven to fit; add `BindMemory` / `BindRuntimeExports` as further
helpers.

Tests in `compiler_v2/api/program_test.cc`:
  - `PlanReturnsInstance` — happy path.
  - `PlanCalledManyTimesYieldsIndependentInstances` — port of the
    smoke test: two Instances from the same Program have isolated
    memories.
  - `PlanFailsWhenBindingsMissingRequiredImport` — M5+; for M1
    the test shape exists but is a no-op until customs land.
  - `PlanIsThreadSafe` — call Plan concurrently from N threads
    (relying on `wasmtime_store_new` being safe to call from
    different threads with the same engine, which it is).

Acceptance: `bazel test //compiler_v2/api:program_test` green.

### 5.5 Commit E — `Instance::Eval` and `Reset`

```cpp
absl::StatusOr<Value> Instance::Eval(const Activation& act) {
  Reset();   // see below
  // M1: no Activation bindings yet (no variables in scope).
  // M2+: marshal Activation values into arena; push as $eval args.
  wasmtime_val_t result;
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_func_call(
      ctx_, &eval_fn_, /*args=*/nullptr, /*nargs=*/0,
      &result, /*nresults=*/1, &trap);
  if (err != nullptr) return WasmtimeErrorToStatus("Eval", err);
  if (trap != nullptr) return WasmTrapToStatus("Eval trapped", trap);
  // Decode CelValue at result.of.i32 → Value.
  return DecodeCelValueAtOffset(/*ctx=*/ctx_, /*memory=*/memory_,
                                /*offset=*/result.of.i32);
}

void Instance::Reset() {
  // $eval's first instruction is a baked-in cel_reset call per
  // m1-scalar-pipeline.md §2.9, so this is a no-op for M1.  Kept
  // public so callers can express intent + so M2+ paths that pre-
  // populate the arena can clear it.
}
```

The `DecodeCelValueAtOffset` helper is mostly a port of the
existing `host_loader_test.cc::ReadCelValue` plus the type-specific
dispatch from `compiler_v2/api/value.cc`.

Tests in `compiler_v2/api/instance_test.cc` (new file, port from
`host_loader_test.cc`):
  - `EvalsIntLiteral` through `EvalsBytesLiteral` — port the 7
    scalar tests.
  - `EvalManyTimesIsDeterministic` — call Eval N times on the
    same Instance, verify each returns the same Value.
  - `EvalAfterPlanFromAnotherInstanceIndependent` — two Instances
    from the same Program; mutate one's bytes, the other is
    unaffected.
  - `MissingEvalExportFails` — port from host_loader_test.
  - `MalformedWasmBytesFail` — port.
  - `ReadBytesBeyondEndReturnsOutOfRange` — port (now testing
    DecodeCelValueAtOffset's bounds check).

`compiler_v2/api/BUILD.bazel`: add the `instance` cc_library + test.

Acceptance: `bazel test //compiler_v2/api/...` green.

### 5.6 Commit F — codegen: stop importing `cel.cel_*`, route through runtime

Codegen currently emits `(import "cel" "cel_reset" ...)` etc.
Under (A) this becomes a runtime export. The codegen change:

  - `compiler_v2/codegen/module.{h,cc}`: replace
    `AddCelResetImport` / `AddCelAllocImport` with calls into
    the runtime instance via the same import shape — but the
    *binding* is now wasm-to-wasm (via the linker's
    `linker_define(cel.cel_*, runtime_inst.exports.*)`), not
    wasm-to-host. The wasm bytecode the codegen emits is
    unchanged.

This is actually a **no-op for codegen output**. The codegen still
emits `(import "cel" "cel_reset" (func ...))`. What changes is
the *resolution* of those imports at instantiate-time, which is
loader-side work (Commit D). Codegen stays the same.

The only codegen change required: drop the line that emits
`(memory (export "memory") 1)` and replace with
`(import "cel" "memory" (memory 1))` — expr no longer defines
memory; it imports it. Per the predecessor `predecessor-memory-ownership-flip.md`
this is straightforward in `module.cc::AddMemoryImport` (which
already exists with optional DataSegments per 040c043's commit).

Tests in `compiler_v2/codegen/module_test.cc`:
  - `EmittedModuleImportsCelMemory` — assert the emitted module
    has `(import "cel" "memory" ...)` and no `(memory ...)`
    definition.
  - `EmittedModuleHasDataSegments` — verify rodata still lands
    in data segments (which now apply to the imported memory at
    instantiate-time — same wasm semantics).

Acceptance: `bazel test //compiler_v2/codegen/...` green.

### 5.7 Commit G — delete `host_loader.{h,cc,_test.cc}`

After Commits A-F land and `Cel::Compiler::Compile` →
`Program::Plan` → `Instance::Eval` work end-to-end:

  - Delete `compiler_v2/host/host_loader.h`,
    `compiler_v2/host/host_loader.cc`,
    `compiler_v2/host/host_loader_test.cc`.
  - Remove the two `host_loader` targets from
    `compiler_v2/host/BUILD.bazel`.
  - Update `compiler_v2/cli/BUILD.bazel`:
    `//compiler_v2/host:host_loader` → `//compiler_v2/api:program`
    + `//compiler_v2/api:instance`.
  - Update `compiler_v2/e2e/BUILD.bazel` similarly.
  - Update `compiler_v2/runtime/cel_runtime.c`: drop
    `cel_reset_native` / `cel_alloc_native` (they're dead — only
    the host_loader trampolines called them). Drop the matching
    declarations in `cel_arena.h`.

Acceptance: `bazel test //compiler_v2/...` all green.

### 5.8 Commit H — doc reconciliation

  - `doc/implementation-plan/rewrite/m1-scalar-pipeline.md §2.9`:
    update the host loader section. Current text (already stale
    relative to 040c043) describes "expr defines memory; runtime
    imports cel.memory". Replace with the actual M1 shape: host
    allocates memory; both modules import `cel.memory`; wiring
    order is host trampolines → bind cel.memory → instantiate
    runtime → bind runtime exports as cel.cel_* → instantiate
    expr.
  - `doc/implementation-plan/rewrite/cel-host-surface.md §2.2`:
    move `Program::FromWasm` to `Compiler::LoadProgram` per §5.3.
  - `doc/implementation-plan/rewrite/design.md`: add a one-paragraph
    callout in the host topology section noting the M1+ shape and
    the security argument.
  - `doc/implementation-plan/testing-checklist.md`: tick rows for
    every test added in commits A-E.

Acceptance: Diffs are doc-only. No bazel target affected.

## 6. Benchmark tests against the new API

`compiler_v2/api/cel_pipeline_bench.cc` — new. Replaces the
experimental `compiler_v2/host/two_phase_topology_bench.cc`
(which measures raw wasmtime APIs); this one measures the
production user-facing surface.

### 6.1 What it measures

Per-stage cost across the user-facing pipeline:

| Bench | What it measures | Expected (from §2 + bias correction) |
|---|---|---|
| `BM_Compiler_Build` | One-time engine + runtime parse | ~167 µs |
| `BM_Compiler_Compile_TinyExpr` | Compiler hot, expr parse cold | ~170 µs (parse + check + lower + assemble + module_new) |
| `BM_Compiler_LoadProgram_TinyExpr` | Skip front-end, just module_new(bytes) | ~150 µs |
| `BM_Program_Plan_Hot` | Per-eval Plan with all caches hot, real runtime | ~12 µs |
| `BM_Instance_Eval` | Steady-state evaluation | ~50 ns + decode cost |
| `BM_Pipeline_FirstEval_Cold` | Build + Compile + Plan + Eval, fresh Compiler per iter | ~340 µs |
| `BM_Pipeline_FirstEval_WarmCompiler` | (Compiler reused) Compile + Plan + Eval per iter | ~170 µs |
| `BM_Pipeline_FirstEval_WarmProgram` | (Program reused) Plan + Eval per iter | ~12 µs |
| `BM_Pipeline_HotEval` | (Instance reused) Eval per iter | ~50 ns |

The 5 input exprs from earlier discussion (L/A/S/B/M):

  - `L = "42"` — pure literal
  - `A = "1 + 2 * 3"` — int arithmetic
  - `S = "\"foo\" + \"bar\""` — string concat
  - `B = "(a && b) || c"` — 3VL with bool vars
  - `M = "msg.x + msg.y"` — proto field reads (mixed host/wasm)

Each Compile/Plan/Eval bench runs against all 5 inputs; output
columns split by input ID so the cost shape per CEL feature is
visible. For M5+ when runtime arms grow, additional inputs added
to this matrix without restructuring the bench.

### 6.2 Source structure

```cpp
// compiler_v2/api/cel_pipeline_bench.cc

namespace cel {
namespace {

// One Compiler per process (fixture).
const Compiler& SharedCompiler() {
  static const Compiler* c = []() {
    auto b = Compiler::NewBuilder();
    // Declare any vars / fns the bench inputs need.  M1 has none.
    return new Compiler(std::move(*std::move(b).Build()));
  }();
  return *c;
}

// Pre-compiled Programs for each input (fixture).
const std::vector<Program>& SharedPrograms() {
  static const auto* progs = []() {
    auto* v = new std::vector<Program>;
    for (auto src : {kInputL, kInputA, kInputS, kInputB, kInputM}) {
      auto p = SharedCompiler().Compile(src);
      ABSL_CHECK_OK(p);
      v->push_back(*std::move(p));
    }
    return v;
  }();
  return *progs;
}

void BM_Pipeline_HotEval(benchmark::State& state) {
  size_t i = state.range(0);  // 0..4 picks input
  const Program& prog = SharedPrograms()[i];
  RuntimeBindings bindings;  // empty for M1
  auto inst = prog.Plan(bindings);
  ABSL_CHECK_OK(inst);
  Activation act;  // empty for M1
  for (auto _ : state) {
    auto v = inst->Eval(act);
    ABSL_CHECK_OK(v);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_Pipeline_HotEval)->DenseRange(0, 4);

// ... similar shape for the other benches above.

}  // namespace
}  // namespace cel

BENCHMARK_MAIN();
```

### 6.3 Build target

`compiler_v2/api/BUILD.bazel`:

```python
cc_binary(
    name = "cel_pipeline_bench",
    srcs = ["cel_pipeline_bench.cc"],
    tags = ["manual"],
    deps = [
        ":activation",
        ":compiler",
        ":instance",
        ":program",
        ":runtime_bindings",
        ":value",
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/strings",
        "@com_google_benchmark//:benchmark_main",
    ],
)
```

Tagged `manual` matching the rest of the wasmtime-dependent
targets (darwin-arm64-only).

### 6.4 How to run

```
bazel run -c opt //compiler_v2/api:cel_pipeline_bench -- \
    --benchmark_min_time=1s
```

### 6.5 What we're checking

Three architectural invariants the bench should confirm:

  1. **`BM_Pipeline_HotEval` ≈ `BM_CallEval` from the experiment**
     (~50 ns range). If wildly different, there's overhead in
     the `Instance::Eval` decode path that needs investigation.
  2. **`BM_Pipeline_FirstEval_WarmProgram` ≈ 12 µs across all
     inputs.** If one input is wildly higher, the Plan path has
     input-dependent cost that should be in Compile (Plan should
     be input-shape-agnostic).
  3. **`BM_Compiler_Compile_TinyExpr` >> `BM_Program_Plan_Hot`**
     by ≥10×. Confirms the expensive cost is paid at Compile
     time, not Plan. Inverts → architectural regression.

No CI pass-fail gate on absolute numbers (bench machine variance
is too high). The bench is a planning + regression-investigation
tool, run on demand.

### 6.6 Why a bench cc_binary, not a cc_test

google_benchmark is a measurement tool, not an assertion tool.
Wrapping it in a cc_test would either flake on CI machines (slow,
contended) or measure nothing useful (very short min_time). Run
manually when investigating perf regressions or evaluating
follow-up optimizations (memory pools, expr-module cache).

## 7. Tests added (full list)

| File | Test | Purpose |
|---|---|---|
| `compiler_v2/runtime/cel_runtime_wasm_test.cc` | `CelResetWritesArenaCursor` | §3 fix verification |
| `` | `CelAllocReturnsValidOffset` | §3 fix verification |
| `compiler_v2/api/compiler_test.cc` | `BuildSucceedsAndProducesEngine` | Commit B |
| `compiler_v2/api/program_test.cc` | `CompileParsesExprModule` | Commit C |
| `` | `LoadProgramRoundTrip` | Commit C |
| `` | `LoadProgramMalformedBytes` | Commit C |
| `` | `ProgramOutlivesCompiler` | Commit C, lifetime |
| `` | `PlanReturnsInstance` | Commit D |
| `` | `PlanCalledManyTimesYieldsIndependentInstances` | Commit D, smoke-test port |
| `` | `PlanIsThreadSafe` | Commit D |
| `compiler_v2/api/instance_test.cc` | `EvalsIntLiteral` … `EvalsBytesLiteral` | Commit E, port from host_loader_test |
| `` | `EvalManyTimesIsDeterministic` | Commit E |
| `` | `EvalAfterPlanFromAnotherInstanceIndependent` | Commit E |
| `` | `MissingEvalExportFails` | Commit E, port |
| `` | `MalformedWasmBytesFail` | Commit E, port |
| `` | `DecodesCelValuePayloadCorrectly` | Commit E |
| `compiler_v2/codegen/module_test.cc` | `EmittedModuleImportsCelMemory` | Commit F |
| `` | `EmittedModuleHasDataSegments` | Commit F |

**Coverage delta on `testing-checklist.md`:** every M1 row that was
ticked under the old host_loader path stays ticked under the new
api path. No new rows; the architectural change preserves every
language capability M1 already had.

## 8. Documentation updates

  - `m1-scalar-pipeline.md §2.9` — see Commit H.
  - `cel-host-surface.md §2.2` — see Commit H.
  - `design.md` host topology section — see Commit H.
  - `testing-checklist.md` — re-tick rows under api/ instead of
    host/ paths.
  - `lint-backlog.md` — no entries needed.

## 9. What is NOT in this slice

Explicit non-goals so review can focus:

  - **No new `Cel::Engine` class.** Engine + runtime module live
    on Compiler.
  - **No expr-module cache keyed by source text.** If a user
    wants compile-once-eval-many they hold the Program. A
    Compiler-level "compile cache by source hash" is a future
    option if profiles demand it.
  - **No `wasmtime_memory_t` pool.** `memory_new` is 2.4 µs per
    Plan — the largest line item in the per-Plan cost. Pooling
    is an obvious future lever; it's not done now because no
    workload has demanded it and a bench-driven decision is
    cheaper later than premature plumbing now.
  - **No async / multi-threaded `Eval`.** Instances stay
    thread-owned per existing design.
  - **No M3+ host import surface changes.** `cel_field_read_*` /
    `cel_unwrap_message` stay as host trampolines (they have to —
    proto messages are host objects). Future work registers them
    via the same `RegisterCustomImports(linker, bindings)` hook
    Plan already calls.
  - **No CI pass-fail bench gate.** §6.6 explains.

## 10. Risk register

| Risk | Mitigation |
|---|---|
| Runtime null-pointer-elision fix doesn't survive a future clang upgrade | Test in §3.2 catches it; freeze the brew/llvm version pinned in `compiler_v2/runtime/BUILD.bazel` if needed. |
| `wasmtime_memory_new` per Plan becomes a hotspot at scale | §6 bench will show it; pool memories on the engine if so. |
| `Plan` thread-safety claim is wrong (concurrent Plans on same engine race) | `PlanIsThreadSafe` test in Commit D. wasmtime docs say engines + modules are thread-safe; if not, fall back to engine-per-thread. |
| First-Plan latency (~160 µs) too high for some workload | Doc the WarmCompiler vs FirstEval expectations; add Compiler-level expr cache as follow-up if needed. |
| Cross-module wasm calls expr → runtime cost more than measured | Bench `BM_Pipeline_HotEval` against multiple input shapes catches this; wasm cross-module calls are documented as essentially free in wasmtime, so unlikely. |

## 11. Estimated effort

  - Commit A: ~1h (small fix + new test).
  - Commit B: ~2h (struct + Build wiring + lifetime test).
  - Commit C: ~3h (FromWasm move + lifecycle tests).
  - Commit D: ~4h (Plan implementation + thread-safety test).
  - Commit E: ~3h (Eval + Reset + decode helper + 7 ports).
  - Commit F: ~2h (codegen change + tests).
  - Commit G: ~1h (deletes + dep updates).
  - Commit H: ~2h (doc reconciliation).
  - Bench (§6): ~3h (cc_binary + 9 benches × 5 inputs).

Total: ~21h focused work. Realistic calendar: 3–4 days.

## 12. Acceptance criteria

This plan is "done" when:

  1. `bazel test //compiler_v2/...` green.
  2. `bazel run -c opt //compiler_v2/api:cel_pipeline_bench` runs
     to completion; numbers in §6.5 invariants hold.
  3. `host_loader.{h,cc,_test.cc}` no longer exists.
  4. The user-facing surface defined in `cel-host-surface.md` is
     unchanged except for the documented `FromWasm` →
     `LoadProgram` move.
  5. The runtime wasm is on the eval path; the only host-side
     runtime helpers are `cel_env.cel_log` plus the explicit
     proto-reflection imports declared in
     `compiler_v2/host_abi/cel_env.h` (existing).
  6. `m1-scalar-pipeline.md §2.9` matches the implementation.
  7. `testing-checklist.md` has no M1 row that was ticked before
     this slice and is unticked after.
