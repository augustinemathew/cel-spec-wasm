# Authoritative WASI migration plan

Status: **plan — drafted 2026-05-18**.  This is the build
plan.  Other docs in this directory become reference
material; deviations from this plan need an explicit update
here.

## 0 Goal

**Simplify the codegen and host by replacing the bump-arena-
at-fixed-offsets architecture with wasi-libc's libc + a
hand-rolled bump arena backed by `malloc`.**

The migration is bounded by these non-negotiable
requirements:

  - Each Instance has its own memory (preserved).
  - Per-Eval cost stays within 5× of today's baseline
    (141 ns scalar, 157 ns string).
  - Conformance pass count is preserved (1,144).
  - The compiled wasm runs in **both wasmtime and Chrome**
    (the user's stated target).
  - Vendored C++ libraries (RE2, parts of abseil) can be
    linked into the runtime without an integration tax.

## 1 Resolved decisions (the five questions in HANDOFF.md §4)

| # | Question | Decision | Why |
|---|---|---|---|
| 1 | Tree strategy | **In-place migration in `compiler_v2/`** | User direction 2026-05-17.  No parallel `compiler_v3/`. |
| 2 | Allocator strategy | **Hand-rolled bump arena over a single `malloc()`** | `mspace_*` not available in stock wasi-libc (`exp_b_mspace.c` fails to link); `memory.fill` from host doesn't reset dlmalloc cleanly (`mparams` survives in BSS); re-instantiate per eval is 2000× slowdown.  Arena-over-malloc gives bump-arena perf AND library compat (`exp_d` proves it). |
| 3 | Memory layout | **`--global-base=8192` in runtime build.  Expr rodata lives in `[0, 8192)` via active data segments.** | `exp_a_rodata.c` proves the linker honors the flag (rodata moved to 8192 cleanly; stack + heap relocate accordingly).  8 KB is enough for typical expression rodata. |
| 4 | Threading target | **`wasm32-wasi` (vanilla, no threads).** | Our kernels don't use threads.  Pure-malloc wasm has **zero WASI imports** (`exp_c_malloc.c`); RE2 / absl vendor is deferred to a follow-up milestone, switch to `wasm32-wasi-threads` only if/when we do that. |
| 5 | Comprehensions branch ordering | **M5 follow-on ships first; this migration starts after.** | Both touch `expr_lower.cc` and `layout_pass.cc`.  Concurrent = guaranteed merge conflicts. |

## 2 Architecture

### 2.1 Memory layout under WASI

```
Linear memory of one Instance:

[0x00000, 0x02000)    8 KB FREE FOR EXPR MODULE
                       Expr rodata installed via active data
                       segments at codegen-time-known offsets.
                       Workspace slots also live here.
                       Codegen tracks layout the same way it
                       does today (LayoutPass.rodata_base = 16,
                       workspace_base, etc.).

[0x02000, ~0x12000)   wasi-libc static data + 64 KB stack
                       wasi-libc's startup machinery; we don't
                       touch it.  __data_end @ ~0x03020,
                       __heap_base @ ~0x12000.

[__heap_base, ∞)      dlmalloc heap
                       Grows up via memory.grow.
                       Per-Instance arena malloc'd here once
                       (default size 64 KB).
                       Activation binding buffer malloc'd here.
                       Plan-lifetime allocations (cel-cpp parsed
                       schemas, future RE2 regex objects) live here.
```

The 8 KB carve-out is at the **bottom** of memory, not the
top, because:

  - wasm-ld places its `__data_end` cursor after our carve-out
    (it just inserts our segment first and itself second).
  - Reserves clean offsets for codegen — same model as
    today's `[16, arena_base)` rodata at fixed addresses.
  - Active data segments install at instantiate time; no
    runtime relocation overhead.

### 2.2 Module + Instance relationships

```
Engine (process-global, shared):
  ├─ wasmtime_engine_t        (Cranelift state)
  └─ runtime_module           (parsed cel_runtime.wasm bytes — cached)

Instance ×N (one per Engine::Plan):
  ├─ wasmtime_store_t
  ├─ wasmtime_linker_t
  ├─ runtime_instance         (instantiated runtime_module)
  │   └─ exports memory       ← NEW: runtime owns memory
  │   └─ exports malloc/free  ← NEW: from wasi-libc
  │   └─ exports arena_*      ← NEW: bump-arena over malloc
  │   └─ exports cel_*        (kernels, unchanged signatures)
  ├─ expr_module              (parsed program.wasm bytes)
  ├─ expr_instance            (imports cel.memory + cel.arena_* + cel.cel_*)
  ├─ host_env                 (cel_host trampoline payload — points to runtime_instance.malloc)
  └─ activation_buf_offset    (NEW: a malloc'd region for activation bindings; grows; reused)
```

The host **does NOT allocate memory**.  The runtime owns it.

### 2.3 Runtime module surface

The `compiler_v2/runtime/` build switches to wasi-sdk and
exports (in addition to all the existing `cel_*` kernels):

```c
// NEW exports the migration adds:
void   arena_init(uint32_t cap_bytes);   // one-time per Instance
uint32_t arena_alloc(uint32_t n);        // returns offset (or 0 = OOM)
void   arena_reset(void);                // resets cursor to 0
uint32_t arena_capacity(void);           // for diagnostics
uint32_t arena_cursor(void);             // for diagnostics
void*  malloc(size_t);                   // standard wasi-libc
void   free(void*);                      // standard wasi-libc

// REMOVED exports:
// cel_alloc(uint32_t)        → callers route to arena_alloc instead
// cel_reset(uint32_t, uint32_t) → callers route to arena_reset

// All other cel_* kernels unchanged.
```

The runtime build flags change in
`compiler_v2/runtime/BUILD.bazel`:

```
- /opt/homebrew/opt/llvm/bin/clang
- --target=wasm32
- -ffreestanding -nostdlib
- -Xlinker --import-memory=cel,memory
- -Wl,--export=cel_alloc -Wl,--export=cel_reset
+ @wasi_sdk//:bin/clang
+ --target=wasm32-wasi
+ -nostartfiles
+ -Wl,--global-base=8192
+ -Wl,--export=arena_init -Wl,--export=arena_alloc
+ -Wl,--export=arena_reset -Wl,--export=malloc -Wl,--export=free
```

## 3 Per-Plan setup (cold path)

This is what runs once per `Engine::Plan(program)` call.
Equivalent to today's ~279 µs `BM_Plan_Hot`.

```
1. wasmtime_store_new                                  [~ 1 µs]
2. wasmtime_linker_new                                 [~ 0.5 µs]
3. Register cel_log + cel_host_* trampoline imports    [~ 1 µs]
4. wasmtime_linker_instantiate(runtime_module)         [~10 µs]
   → runtime_instance
   - Memory created here (owned by the instance).
   - wasi-libc's _initialize runs (initializes the
     stack pointer; dlmalloc lazy-init deferred to
     first malloc).
5. Pull from runtime_instance:                         [~ 2 µs]
   - memory       → bind on linker as cel.memory
   - malloc       → bind as cel.malloc
   - free         → bind as cel.free
   - arena_*      → bind as cel.arena_*
   - every cel_*  → bind as cel.<name>
6. wasmtime_func_call(arena_init, 64 * 1024)           [~ 5 µs]
   - First malloc: triggers dlmalloc lazy-init (~3 µs)
     + arena buffer allocation (~1 µs).
7. wasmtime_module_new(expr_bytes)                     [~30 µs]
8. wasmtime_linker_instantiate(expr_module)            [~10 µs]
   → expr_instance
   - Active data segments install expr rodata
     into [0, 8192) of the shared memory.
9. Pull expr_instance.eval → eval_fn handle            [~ 1 µs]
10. Optionally call host_alloc(activation_buf_size)    [~ 1 µs]
    to pre-allocate the binding buffer.

Total: ~60 µs.  Faster than today's 279 µs Plan_Hot,
because the host doesn't separately allocate the memory
+ the runtime instance is leaner without the import-memory
+ cursor-slot dance.
```

**Insight: per-Plan cost actually DROPS** because we remove
the host-side `wasmtime_memory_new` (the host no longer
allocates memory) and the implicit cost of stitching it
into the linker.  Today's 279 µs is dominated by wasmtime
internals; removing one round-trip might save 30-50 µs.
Measure to confirm.

## 4 Per-Eval (hot path) — the user's specific question

**Per Eval, do we re-instantiate the runtime memory?  NO.**

The Instance lives for the lifetime of the Plan.  Each Eval
just calls $eval through the same `eval_fn` handle.  Below
is the exact hot-path call sequence.

### 4.1 The Eval call

```
Instance::Eval(activation) — host side:
  ┌─────────────────────────────────────────────────────────┐
  │ 1. Marshal activation bindings:                         │
  │    for each (name, value) in activation:                │
  │      slot_off = layout.variables[name].slot_offset      │
  │      if value is kString or kBytes:                     │
  │        if activation_buf is too small:                  │
  │          host_realloc(activation_buf, new_size)         │
  │        memcpy bytes into activation_buf                 │
  │        write CelValue{kind=STRING, off=buf+cursor} to   │
  │          slot_off                                       │
  │      else:                                              │
  │        write CelValue directly to slot_off              │
  │ 2. wasmtime_func_call(eval_fn, []) → root_offset        │
  │ 3. Decode CelValue at root_offset                       │
  └─────────────────────────────────────────────────────────┘

Inside $eval — wasm side, codegen-emitted:
  ┌─────────────────────────────────────────────────────────┐
  │ 1. (call $arena_reset)              ← cursor = 0        │
  │ 2. <expression body>                                    │
  │    - kIdent loads from workspace slot (absolute i32.const)│
  │    - kConst loads from rodata (absolute i32.const)      │
  │    - kCall ... etc.                                     │
  │    - Allocations inside kernels call $arena_alloc       │
  │ 3. return <root_offset>                                 │
  └─────────────────────────────────────────────────────────┘
```

### 4.2 Per-Eval cost decomposition

| Step | Cost today | Cost post-migration | Delta |
|---|---:|---:|---:|
| Host marshal (no bindings) | ~0 ns | ~0 ns | 0 |
| Host marshal (1 string binding) | ~20 ns (host string arena cursor advance) | ~30 ns (malloc call OR cursor advance) | +10 ns |
| `wasmtime_func_call(eval_fn)` | ~100 ns | ~100 ns | 0 |
| `$eval` prologue (`cel_reset` vs `arena_reset`) | ~5 ns (2 i32.store) | ~5 ns (1 i32.store) | 0 |
| Expression body | depends | depends | 0 |
| CelValue decode | ~30 ns | ~30 ns | 0 |
| **Total (literal scalar)** | **~141 ns** | **~145 ns** | **+~4 ns** |

The per-Eval cost is essentially unchanged.  The arena
reset is the same number of instructions; the addressing
of workspace + rodata is the same (absolute i32.const).

**Within the 5× budget** from BENCHMARK_DESIGN.md §5
by a factor of 35.

### 4.3 Activation binding lifecycle

Per the user's pattern question ("how does it work"):

  - **First Eval of an Instance**: host calls
    `wasmtime_func_call(malloc, activation_buf_size)` via
    wasm reentry to allocate a buffer (~64 KB initial).
    Caches the offset in `InstanceImpl`.
  - **Subsequent Evals**: host overwrites the buffer.  No
    new allocation.  Each binding's bytes get written; the
    CelValue header gets stamped in the appropriate
    workspace slot.
  - **Buffer grow**: if a binding exceeds the buffer size,
    host calls `wasmtime_func_call(realloc, ...)` to grow.
    Rare.
  - **Instance destruction**: dlmalloc + memory go with the
    Instance.  No explicit cleanup.

This is the **same pattern as today's `host_string_arena`,
but with `malloc()` instead of `wasmtime_memory_grow`
directly**.  Net delta: the cursor / floor / capacity
tracking moves from `InstanceImpl` into dlmalloc's heap;
the host side simplifies to one cached `malloc'd_offset`.

### 4.4 Why we don't re-instantiate per Eval

The user asked specifically.  Three reasons:

  1. **Cost**: 60 µs Plan setup per eval = ~400× slowdown
     vs today's 141 ns.  Even with the migration's
     simplifications, instantiation is fundamentally a
     wasm + wasmtime operation that costs tens of
     microseconds.
  2. **Plan-lifetime state**: vendored libraries (future
     RE2, abseil-time formatters) hold malloc'd state
     across evals.  Re-instantiating wipes that state and
     forces re-initialisation per eval.
  3. **Activation bindings**: re-allocating the binding
     buffer per eval adds another ~1 µs of malloc cost.

The arena-over-malloc design gives us "wipe at eval
boundary" cheaply without throwing away the Instance.

## 5 What changes in compiler_v2 (concrete diff sketch)

### 5.1 `runtime/`

```diff
- cel_arena.c      (66 LoC, DELETED)
- cel_arena.h      (39 LoC, DELETED)
+ cel_arena.c      (47 LoC, REWRITTEN: bump arena over malloc)
+ cel_arena.h      (~30 LoC, NEW API: arena_init/alloc/reset)

  cel_memory.c     SIMPLIFIED (~30 LoC, drop inline-asm hack)
  cel_memory.h     unchanged signature

  cel_*.c kernels  s/cel_alloc/arena_alloc/g + s/cel_reset/arena_reset/g
                   (107 call sites, mechanical)

  *_test.cc        SetUp() calls arena_init + arena_reset
                   per test fixture
```

### 5.2 `codegen/`

```diff
  expr_lower.cc
- EmitCelResetCall(mod, layout.arena_base, opts.mem_size_bytes)
+ EmitArenaResetCall(mod)         ;; (call $arena_reset)
                                  ;; No args — arena lives in
                                  ;; the runtime's malloc'd buffer.

  layout_pass.h
- uint32_t arena_base = 0;        ← removed
  // workspace_base / workspace_bytes stay; rodata_base = 16 stays
  // (still using fixed offsets in [0, 8192))

  expr_lower.h
- inline constexpr ... kCelResetInternalName = "cel_reset";
+ inline constexpr ... kArenaResetInternalName = "arena_reset";
- LoweringOptions::mem_size_bytes  ← removed

  compile.cc
- AddFunctionImport("cel_reset", "cel", "cel_reset", reset_params, none);
- AddFunctionImport("cel_alloc", "cel", "cel_alloc", alloc_params, i32);
+ AddFunctionImport("arena_reset", "cel", "arena_reset", none_params, none);
+ AddFunctionImport("arena_alloc", "cel", "arena_alloc", alloc_params, i32);
```

### 5.3 `api/`

```diff
  engine.cc
- InitStoreAndMemory()        ← removed (no host memory alloc)
+ InitStore()                 ← just the store

  engine.cc::kRuntimeExports[]
- "cel_reset", "cel_alloc"
+ "arena_reset", "arena_alloc", "arena_init", "malloc", "free"

  instance.cc
- EnsureHostStringArenaCapacity ~110 LoC, DELETED
- HostStringArena helpers, DELETED
- host_string_arena_floor / capacity fields, DELETED
+ activation_buf_off field (i32), NEW
+ EncodeBoundString via host_malloc/realloc reentry, ~30 LoC

  cel_host_wasmtime.cc
  WasmtimeArenaAllocator renamed to WasmtimeMallocAllocator
  (same shape, just calls malloc instead of cel_alloc)
```

### 5.4 `BUILD.bazel`

```diff
  runtime/BUILD.bazel  flag swap (see §2.3 above)
  MODULE.bazel         + http_archive(wasi_sdk_*) ×4 platforms
                       + third_party/wasi_sdk/BUILD.external.bazel
```

## 6 Slice plan (the executable version)

12 slices, each independently committable.  Replaces
WORK_PLAN.md's 9 phases with finer granularity.

| Slice | Goal | Acceptance | Estimate |
|---|---|---|---:|
| S1 | wasi-sdk in MODULE.bazel (4 platforms) | `bazel build @wasi_sdk//:clang` works | 0.5 day |
| S2 | runtime/BUILD.bazel switch to wasi-sdk + --global-base | `cel_runtime.wasm` builds, has wasi-libc + dlmalloc | 0.5 day |
| S3 | Implement `arena_init` / `arena_alloc` / `arena_reset` in `cel_arena.c` (rewrite) | Unit tests for arena pass under native build | 0.5 day |
| S4 | Replace `cel_alloc` → `arena_alloc` in all 6 kernel `.c` files | Runtime tests pass (native) | 0.5 day |
| S5 | Replace `cel_reset` / `cel_alloc` in 21 test files | All `bazel test //compiler_v2/runtime/...` pass | 1 day |
| S6 | Codegen prologue: emit `(call $arena_reset)` instead of `(call $cel_reset c1 c2)` | `codegen/expr_lower_test.cc` updated, all green | 1 day |
| S7 | LayoutPass: drop `arena_base` field + `mem_size_bytes` from `LoweringOptions` | `layout_pass_test.cc` updated, all green | 0.5 day |
| S8 | Engine: stop host-allocating memory; pull from runtime_instance.memory | `BM_Plan_Hot` works; correct memory wiring | 0.5 day |
| S9 | Instance: delete `EnsureHostStringArenaCapacity`; replace with malloc-reentry encoder | activation marshalling tests pass | 1 day |
| S10 | Conformance: run, debug any divergence | 1,144 PASS, matching baseline | 1-2 days |
| S11 | Bench harness: re-run cel_pipeline_bench, write POST_MIGRATION_BENCH.md | Numbers captured + acceptance check (§7) | 0.5 day |
| S12 | Chrome smoke-test: a hand-coded driver loads cel_runtime + a sample expr in headless Chrome | Returns expected value for string concat sample | 1 day |
| **Total** | | | **8.5-10 days** |

Compared to ANALYSIS.md's ~16-session estimate: this is
**~10 days** of focused work because the experimental
findings reduce design uncertainty (the allocator
strategy, memory layout, and threading target are all
locked).

Risk-weighted: **2-3 weeks** with a realistic budget for
conformance debug + Chrome integration surprises.

## 7 Acceptance criteria

The migration ships when ALL of:

  - [ ] `bazel test //compiler_v2/...` green.
  - [ ] `bazel run //compiler_v2/conformance:run_conformance`
    → **1,144 PASS** (same as `BASELINE_BENCH.md`).
  - [ ] Per-Eval cost from `cel_pipeline_bench` ≤ **5× baseline**:
    - Scalar Eval: ≤ 705 ns (today 141 ns).
    - String Eval: ≤ 785 ns (today 157 ns).
  - [ ] Per-Plan cost ≤ **1.5× baseline**: ≤ 419 µs (today 279 µs).
    Plan_Hot is likely to DROP, not grow — see §3.
  - [ ] Memory baseline ≤ **1.5× baseline**: ≤ 192 KB
    initial (today 128 KB).  Expected ~150-180 KB.
  - [ ] Compile cost within ±10% (compiler untouched).
  - [ ] `cel_runtime.wasm` < **2× baseline**: ≤ 122 KB stripped.
    Expected ~90-110 KB.
  - [ ] **Sample expression runs in Chrome** (user-stated
    target): `"hello " + " world"` (string concat) compiles,
    loads via `WebAssembly.instantiate`, evaluates to
    `"hello  world"`.

### 7.1 Benchmark workload

S11 runs `cel_pipeline_bench` against this fixed set of CEL
expressions.  Each row produces a golden `cel::Value` that's
auto-asserted on every run.  Rows are listed by complexity
and tagged by the milestone that ships their codegen.

| # | Source | Bound vars | Exercises | Milestone |
|---|---|---|---|---|
| 1 | `42` | — | kConst, pure rodata | M1 |
| 2 | `true && false` | — | kCall(`_&&_`), short-circuit | M5 |
| 3 | `x` | `x: int` | kIdent, activation marshalling (scalar) | M2 |
| 4 | `x + y` | `x,y: int` | kCall(arith), 2 kIdent | M5 |
| 5 | `'foo' + 'bar'` | — | string concat (arena alloc in result) | M5C |
| 6 | `s.contains('hello')` | `s: string` | kCall receiver, string ops | M5C |
| 7 | `msg.field` | `msg: Customer` | kSelect, proto-field-read trampoline | M3 |
| 8 | `[1, 2, 3].size()` | — | list literal, list size | M4 |
| 9 | `{'a':1,'b':2}.size()` | — | map literal, map size | M3 |
| 10 | `int(msg.f) > 0` | `msg: Customer` | type conversion + arith | M10 |

Rows for comprehensions (`exists`, `cel.bind`) are
intentionally omitted — they depend on M5 follow-on which is
sequenced before this migration anyway.  Add them when that
ships.

Methodology:
  - Run on a quiet machine, no other load.
  - Pin to a single P-core if thermal variance is an issue
    (`taskset` on Linux; close other apps on macOS).
  - `--benchmark_min_time=2s --benchmark_repetitions=3`.
  - Capture mean + p99 + stddev.
  - Compare to `BASELINE_BENCH.md` deltas; pass/fail per §7.

## 8 Risks (load-bearing, from earlier docs)

  - **R1.  `--initial-memory` defaults to 2 pages.**
    wasi-libc's `_initialize` needs the heap to be growable;
    setting `--max-memory` too small could cause first-malloc
    OOM.  Mitigation: leave `--max-memory` unset (no limit);
    let memory.grow as needed.
  - **R2.  dlmalloc lazy-init timing.**  First malloc costs
    ~3-5 µs of one-time setup.  We trigger it deliberately
    in `arena_init` to amortise into Plan setup, not Eval.
  - **R3.  Activation buffer fragmentation.**  If activations
    have wildly different binding sizes, dlmalloc may
    fragment.  Mitigation: realloc strategy doubles the
    buffer; binding offsets within the buffer come from a
    single bump cursor (managed host-side).  Same model as
    today's `host_string_arena`.
  - **R4.  Codegen test rebaselining.**  Every fixture that
    asserts `EXPECT_STREQ(target, "cel_reset")` becomes
    `"arena_reset"`.  ~50 sites in `codegen/*_test.cc`.
    Mechanical but tedious — budget 1 day in S6.
  - **R5.  Chrome smoke-test surprises.**  Wasmtime ≠ Chrome
    in subtle ways (wasm validation strictness, WASI shim
    coverage if any imports exist).  Our zero-WASI-imports
    pure-malloc design should run cleanly, but verify in S12.
  - **R6.  M5 comprehensions follow-on parallel work.**  If
    that branch lands on master while we're mid-migration,
    rebase against the new expr_lower.cc shape.  Mitigation:
    don't start S6 until comprehensions has merged or been
    explicitly paused.

## 9 Files in `doc/implementation-plan/wasi/`

After this commit:

```
AUTHORITATIVE_PLAN.md          ← this doc; the build plan
BASELINE_BENCH.md              ← pre-migration numbers
MEMORY_OPTIONS.md              ← experimental findings
ANALYSIS.md                    ← reference: per-function inventory
HANDOFF.md                     ← reference: 5 decision questions
WORK_PLAN.md                   ← reference: superseded by §6 above
BENCHMARK_DESIGN.md            ← reference: bench methodology
AGENT_ASSESSMENT.md            ← reference: independent critique
CLAUDE_Do_NOT_DELETE...        ← sentinel
experiments/                   ← 4 .c probes + 1 .wat driver
```

Reading order for a new session:
  1. `AUTHORITATIVE_PLAN.md` (this file) — what to build.
  2. `BASELINE_BENCH.md` — the numbers to beat.
  3. `MEMORY_OPTIONS.md` — empirical justification for §1
     decisions.
  4. `experiments/` — running code that backs the
     decisions.
  5. Others — only if you need historical context.

## 10 What to do next

Per the user's most recent step plan:

  - **Next step**: start S1 + S2 (wasi-sdk in MODULE.bazel +
    runtime build switch).  Combined they're ~1 day.  The
    output is a `cel_runtime.wasm` built by wasi-sdk that
    still runs all existing tests (because we haven't
    touched any C code yet — just the linker config).
  - **Then S3-S5**: arena implementation + kernel migration
    + test fixture updates.  ~2 days.
  - **Then S6-S9**: codegen + host changes.  ~3 days.
  - **Then S10-S12**: conformance debug + bench + Chrome
    smoke-test.  ~2-3 days.

Total flow: **~8.5-10 working days** for the headline
migration; ~2-3 weeks calendar.
