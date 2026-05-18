# Handoff — WASI / `malloc` migration

Status: handoff doc — drafted 2026-05-17.  The work below is
intended to happen on a **separate branch** because the
current branch is in active use by other agents.  This doc
captures everything the next session (on the new branch) needs.

## 0 Goal

**Simplify codegen and host.**  WASI + `malloc` is the
mechanism, but simplification is the outcome.  Specifically,
the migration eliminates:

  - The codegen `cel_reset` prologue + `arena_base` /
    `mem_size_bytes` threading through `LoweringOptions`.
  - The fixed cursor-slot memory layout at bytes 8/12 that
    every kernel reads via `load_u32` / `store_u32`.
  - The host's `EnsureHostStringArenaCapacity` hack (110
    LoC, ~50% of `instance.cc`'s complexity) that exists
    *because* the bump arena rewinds across evals.
  - The inline-asm opacity barrier in `cel_memory.c` (load-
    bearing today; literally a clang-defeat trick).
  - The 2-arg memory-type allocation in `engine.cc`
    (`max_present=false, max=0, page_size_log2=16`) plus
    the host's `wasmtime_memory_new` ownership semantics.
  - The 2-arg `cel_reset(base, limit)` ABI and the
    compile-time constant pair codegen has to track.
  - The `--import-memory=cel,memory` linker dance in
    `runtime/BUILD.bazel`.

What remains after the simplification: codegen emits a
prologue that calls `malloc` once for the workspace and
`free` once at the end.  Kernels call `malloc` / `free`
inline.  Host code instantiates the runtime, pulls its
exported memory, wires it onto the linker.  No cursor
slot.  No arena base.  No host string arena.

The benchmark exists to **validate the simplification
doesn't cost too much perf**.  Specifically: does eliminating
the bump arena (~10 ns/alloc) for `malloc` (~30-50 ns/alloc)
break the per-Eval cost budget?  The bench answers yes/no.

If yes (cost is too high), the simplification doesn't ship,
but the analysis still has value: it tells us what
optimisations the bump arena buys, which informs decisions
elsewhere.

If no (cost is acceptable), we get a measurably simpler
runtime + codegen + host architecture, with the side
benefit that future C/C++ library vendoring becomes free
(see `../exp1_re2/` — RE2 in 150 KB gzipped, end-to-end).

## 1 The honest scope reset

The earlier estimate (`ANALYSIS.md §8` and §11: "8 slices,
~4.5 sessions") was **drastically too low**.  Below is the
honest restatement after a closer read.

### 1.1 What's actually being touched

  - **26,047 LoC** of production C/C++ in `compiler_v2/` —
    every layer (runtime kernels, codegen, host integration,
    bench) needs port work.
  - **10,981 LoC** of e2e tests across 12 files — every
    expected-emission assertion that mentions `cel_reset` or
    `cel_alloc` breaks.  Per-test fix-up isn't "mechanical"
    because lifetimes shift (today's "everything is freed
    by cel_reset" becomes "host or mspace tracks ownership").
  - **107 `cel_alloc` call sites in the runtime kernel** —
    not mechanical.  Each site needs a lifetime decision:
    who owns the allocation, who frees it, how does it
    survive the eval boundary if it's part of the result?
  - **20 codegen test files** that assert exact wasm
    emission shape.  Every test that expects
    `call $cel_reset` needs rewriting.
  - **A second compiler tree to live alongside the first**
    so we can benchmark side-by-side.  This is its own
    sub-project (see §3).

### 1.2 What "mechanical" actually means here

The phrase "mechanical search-and-replace" appeared 3 times
in `ANALYSIS.md`.  In practice:

  - **`cel_alloc(N)` → `malloc(N)`** is mechanical at the
    call site, but the **lifetime contract changes**.  Today
    every allocation lives until `cel_reset`; under malloc,
    each one needs an explicit owner.  CelValues containing
    span payloads (`{kind=CEL_STRING, payload.s={ptr,len}}`)
    are returned to the host — when does the host free the
    payload?  Today: never (cel_reset handles it).
    Tomorrow: somebody has to.
  - **`cel_reset(base, limit)` → mspace bootstrap** is
    mechanical in codegen, but the **mspace lifecycle
    choice** is a design decision (per-eval vs per-Instance
    vs explicit free) with downstream test, bench, and host
    integration implications.
  - **Codegen emission tests** that check for specific calls
    aren't mechanical — they assert design intent.  Every
    fixture needs re-grounding against the new design.

### 1.3 Realistic effort estimate

| Phase | Sessions | Notes |
|---|---:|---|
| 0 — Branch setup + parallel-tree decision | 1 | Including bazel rules to build both v2 and v3 toolchains. |
| 1 — wasi-sdk build infra (MODULE.bazel + 4 platforms) | 1 | Per-platform `http_archive` + `BUILD.external.bazel`; CI validation. |
| 2 — Standalone prototype runtime (mspace lifecycle proof) | 2 | New runtime in a sibling dir; mspace per-eval; pass kernel unit tests. |
| 3 — Codegen prologue rewrite (workspace malloc, relative addressing) | 3 | This is the architecturally heavy part — every kIdent / kConst emit changes; LayoutPass loses arena_base; LoweringOptions loses mem_size_bytes; ~50 codegen test fixtures update. |
| 4 — Host integration rewrite (engine + instance memory ownership flip) | 2 | wasmtime memory now comes from runtime_instance's export; host_string_arena deleted; activation marshalling via host malloc reentry. |
| 5 — All runtime kernels port (107 sites + 21 test files) | 2 | Per-site lifetime decisions; per-test fixture updates.  Mechanical at the call site, but each test file needs SetUp/TearDown reworked. |
| 6 — Conformance bring-up (rebaseline 1144 PASS) | 2 | Expect 1-2 days of debugging where lifetime corners diverge.  Same PASS count is the gate. |
| 7 — Bench harness: side-by-side v2 vs v3 | 2 | Parallel build, workload set, metrics, write-up. |
| 8 — Decision doc + cleanup or revert | 1 | Numbers in, decision out. |
| **Total** | **16 sessions** | **~3-4 weeks** for one person. |

That's the honest number.  The ANALYSIS.md estimate (4.5
sessions) ignored the codegen prologue rewrite (Phase 3 — by
itself bigger than my whole earlier estimate), the parallel
build setup (Phase 0+1), and the conformance debug pass
(Phase 6).

### 1.4 What could compress this

  - **Skip the parallel-tree benchmark.**  Migrate in place,
    measure baseline first, then migrate, then re-measure.
    Saves Phase 7's ~2 sessions and Phase 0's complexity.
    Cost: no A/B comparison after the fact; can't easily
    revert decisions.  **Not recommended** given the user's
    "benchmark vs baseline" requirement.
  - **Defer host_string_arena cleanup** to a follow-up.
    Phase 4 shrinks by ~1 session.  Cost: kept-alive hack
    that's harder to remove later.
  - **Skip the comprehensions follow-on first**.  Just rip.
    Cost: guaranteed merge conflicts on `expr_lower.cc` and
    `layout_pass.cc` when comprehensions branch comes back.

Net: probably realistic compression to **~12 sessions** if
we accept some technical debt and don't need the parallel
benchmark.  For the user's stated goal (bench v2 vs v3), the
16-session number stands.

## 2 The branch + directory layout the user should create

Recommendation:

```
git checkout -b wasi-malloc-migration
mkdir compiler_v3/                  # new tree, sibling of compiler_v2
```

Why a parallel tree and not in-place edit:

  - **A/B benchmarking requires both alive simultaneously.**
    The bench target needs to compile the same CEL
    expression both ways and measure each.
  - **Rollback is free.**  If WASI/malloc doesn't beat the
    bench, `rm -rf compiler_v3` and the branch is
    abandoned.  In-place editing would require a revert
    against thousands of changed lines.
  - **The current branch stays usable.**  Other agents
    working on comprehensions / M8 / extensions don't see
    half-migrated state.

Alternative considered: bazel `--config=wasi` flag that
selects between two runtime builds inside a single tree.
Pro: less duplication.  Con: every C++ file that touches
arena needs `#ifdef` branching, which is the kind of debt
that ossifies.  **Not recommended.**

### 2.1 What lives in `compiler_v3/`

Start by **copying compiler_v2 wholesale**.  Then incrementally
migrate.  This means:
  - `compiler_v3/runtime/` — full copy, then port.
  - `compiler_v3/codegen/` — full copy, then port (this is
    the big one — Phase 3).
  - `compiler_v3/api/` — full copy, then port.
  - `compiler_v3/conformance/` — copy + verify against
    same fixtures.
  - `compiler_v3/e2e/` — copy + verify.
  - `compiler_v3/bench/` — gets new bench targets that
    cross-compare against `compiler_v2/bench`.
  - `compiler_v3/host/`, `tools/`, `frontend/`, `ir/`,
    `abi/` — full copy + minor cleanup (mostly unchanged
    except for symbol references).

`MODULE.bazel` gains the wasi-sdk archives (§1.1 phase 1
deliverable) and a top-level `bench_v2_vs_v3` target.

### 2.2 Files to copy from this directory into the new branch

When the user spins up the new branch, drag these files
across:

  - `wasm_compilation_experiments/exp2_wasi_migration/ANALYSIS.md`
    — the comprehensive analysis (1,082 lines).  Source of
    truth for what's being touched.
  - `wasm_compilation_experiments/exp2_wasi_migration/HANDOFF.md`
    — this file.
  - `wasm_compilation_experiments/exp2_wasi_migration/WORK_PLAN.md`
    — detailed slice-by-slice task list (created below in
    §6 of this doc, or as a separate file).
  - `wasm_compilation_experiments/exp2_wasi_migration/BENCHMARK_DESIGN.md`
    — methodology for the v2-vs-v3 comparison (created in
    §5 of this doc, or as a separate file).
  - `wasm_compilation_experiments/exp1_re2/` — the proven
    wasi-sdk toolchain setup, RE2 build artifacts, working
    end-to-end demo.  Reference for "how to compile against
    wasi-sdk" — already validated.
  - `wasm_compilation_experiments/PLAN.md` and
    `WASI_AND_PORTABILITY.md` — the strategic context.
  - `wasm_compilation_experiments/exp2_wasi_migration/CLAUDE_Do_NOT_DELETE_OR_REVERT_FILES_IN_THIS_DIR`
    — keep the sentinel.

## 3 The benchmark goal — what "compare against baseline" means

This is the user's stated outcome.  Below is what
"benchmark our expressions e2e against baseline" should
mean operationally.

### 3.1 The workload set

A fixed set of CEL expressions, representative across the
features the compiler ships today (M1 through M10).  Same
set runs through both compilers.

Minimum viable workload (representative — refine on the new
branch):

```
1.  "42"                                                # literal int
2.  "true && false"                                      # bool short-circuit
3.  "x"                                                  # bound ident, int
4.  "x + y"                                              # bound idents, arith
5.  "msg.field1.subfield"                                # proto select chain
6.  "msg.field1 == 'expected'"                           # string compare
7.  "msg.scopes.exists(s, s == 'admin')"                 # comprehension (M5b)
8.  "{'a': 1, 'b': 2}.size()"                            # map operation
9.  "[1,2,3].all(v, v > 0)"                              # comprehension (M5b)
10. "timestamp('2026-05-17T00:00:00Z') > now"            # M7b time
11. "msg.any_field"                                      # Any unpack (M7a)
12. "int(msg.float_field)"                               # conversion (M10)
13. "cel.bind(n, msg.count, n > 0 && n < 100)"           # cel.bind (M5b)
```

Each expression must compile cleanly under both compilers;
each must produce identical output for the same input.
Inputs are pre-defined per expression as a fixed activation.

### 3.2 Metrics to capture (per expression × per compiler)

| Metric | How measured | Why |
|---|---|---|
| Compile time | `BM_Compile` from `cel_pipeline_bench.cc` | Per-source cost.  Compiler is shared; should be ~unchanged. |
| Plan time (cold) | `BM_Plan_Hot` first iteration | Per-Instance cost; expected to grow ~50-90% with WASI. |
| Plan time (warm) | `BM_Plan_Hot` steady-state | Cached path. |
| Eval time | `BM_Eval` (per expression, after warmup) | Per-call cost; this is the perf-critical metric. |
| Peak memory | Wasmtime memory size after first eval | Footprint.  Today: ~128 KB baseline; expected ~80-110 KB post-migration. |
| Allocation count per eval | New counter; instrument both runtimes | Diagnostic — explains the per-eval delta. |
| End-to-end latency | `BM_Pipeline_Cold` and `BM_Pipeline_HotEval` | What a user pays per request. |

### 3.3 The bench target shape

A new bench in `compiler_v3/bench/v2_vs_v3_bench.cc` (or
similar) that:

  - Takes the same `CompileOptions` + `Source` for each
    expression.
  - Runs it through `cel::v2::Compiler` and `cel::v3::Compiler`
    (where `v2`/`v3` are namespace aliases over the two
    parallel trees).
  - Captures the metrics above for each.
  - Outputs a CSV / table that a human can read.

### 3.4 Acceptance criteria for "WASI migration ships"

1. **All e2e tests in compiler_v3 pass** at the same rate
   as compiler_v2 (12 test files, 10,981 LoC).
2. **Conformance pass count is identical** to compiler_v2
   (1,144 PASS today; same in v3).
3. **Per-Eval cost ratio is ≤ 5×** (today: ~10-30 ns;
   acceptable post-migration: ≤ 150 ns).  Beyond 5× requires
   user sign-off.
4. **Per-Instance memory baseline is ≤ 1.5× today's** (today:
   128 KB; cap: 192 KB).  Expected: actually drops.
5. **Compile cost is unchanged** (within ±10%).  Compiler
   isn't touched.
6. **RE2 vendored as a smoke test**: a regex-using
   expression compiles, plans, evals, and matches under v3
   (proving the architectural payoff actually realises).

## 4 What must work before any of this starts

Decisions the user makes BEFORE the new branch is cut:

### 4.1 Tree strategy
  - **A) Parallel trees** (compiler_v2 + compiler_v3) — full
    A/B; biggest disruption.  Recommended.
  - **B) In-place migrate** — fastest but no comparison;
    requires a baseline bench snapshot taken before the
    work begins.
  - **C) Single tree with bazel `--config=wasi` flag** — not
    recommended (technical debt).

### 4.2 Allocator strategy
  - **A) `mspace` per eval** — bump-arena-like perf;
    requires wasi-libc's `mspace_*` APIs (verify in Phase
    2).
  - **B) Global malloc + explicit free** — simpler;
    requires every host-side decode path to call
    `cel_free` after reading.
  - **C) Per-Instance lifetime** — fastest per-eval; leaks
    if Instance is reused.

### 4.3 Memory layout (where rodata + workspace live)
  - **I) Fixed offset** — brittle.
  - **II) Malloc'd base, relative addressing** — clean;
    requires kIdent/kConst codegen rewrite.  Recommended.
  - **III) Static scratch in runtime_module** — ~64 KB bss
    per Instance.

### 4.4 Threading target
  - **`wasm32-wasi`** — vanilla, no threads.  Works for our
    kernels (none use threads).  RE2 needs absl which uses
    `std::mutex`; if we vendor RE2 later, switch then.
  - **`wasm32-wasi-threads`** — has `std::mutex`; needs
    `--shared-memory`.  Required for vendoring RE2.

### 4.5 Comprehensions branch ordering
  - **Ship M5 comprehensions follow-on first**, then this
    migration.  Both touch `expr_lower.cc` and
    `layout_pass.cc`.  Recommended.
  - Concurrent: guaranteed merge conflicts.  Not recommended.

## 5 Benchmark design (workload, harness, metrics)

This is the new branch's primary deliverable.  See
`BENCHMARK_DESIGN.md` (companion file in this directory) for
the full methodology — workload list, metric definitions,
harness scaffolding, output format.

The high-level approach:

  - Two parallel trees, `compiler_v2/` (baseline) and
    `compiler_v3/` (WASI migration).
  - A new `bench/v2_vs_v3_bench.cc` target compiled
    against both.
  - 13-row workload table (§3.1) running through both.
  - Output: CSV + a `RESULTS.md` summarising deltas.

Hard requirements:
  - **Eval semantics match** (each expression returns the
    same `cel::Value` from both compilers).  Auto-check on
    every bench run.
  - **Conformance pass count matches** (run before bench).
  - **At least 1000 iterations per metric** for stable
    timing on per-Eval (the ns-scale measurements need
    that to be statistically meaningful).

## 6 Work plan — slice-by-slice for the new branch

See `WORK_PLAN.md` (companion file) for the full breakdown.
Summary:

  - **Phase 0 (1 session)**: branch setup, copy compiler_v2
    → compiler_v3.
  - **Phase 1 (1 session)**: wasi-sdk in MODULE.bazel,
    BUILD.external.bazel files, runtime build target swap.
  - **Phase 2 (2 sessions)**: standalone prototype runtime —
    mspace lifecycle proof, kernel unit tests pass.
  - **Phase 3 (3 sessions)**: codegen prologue rewrite,
    LayoutPass cleanup, ~50 codegen test fixtures updated.
  - **Phase 4 (2 sessions)**: host integration — engine
    memory ownership flip, instance.cc host_string_arena
    deletion.
  - **Phase 5 (2 sessions)**: 107 kernel call sites + 21
    test files ported.
  - **Phase 6 (2 sessions)**: conformance debug + rebaseline.
  - **Phase 7 (2 sessions)**: bench harness, v2-vs-v3
    workload runs.
  - **Phase 8 (1 session)**: decision doc + closeout.

**Total: 16 sessions ≈ 3-4 weeks of focused work.**

## 7 Known unknowns the new branch must resolve

  1. **Does wasi-libc expose `mspace_*`?**  Verify in Phase
     2 by compiling a probe.  If not, fall back to plan B
     (global malloc + explicit free).  This decision
     influences ~20% of the codegen Phase 3 work.
  2. **How much does dlmalloc lazy-init cost per Instance?**
     Estimated ~5-10 µs in `ANALYSIS.md §4.2` — verify by
     directly measuring in Phase 2's prototype.
  3. **Does the wasm32-wasi target's libc++ have the
     features we'd need for future C++ vendoring?**  Test
     by building a tiny C++ kernel in Phase 2.
  4. **Will relative addressing in codegen break the active
     data segment install?**  Phase 3 is where this gets
     answered.  Mitigation: keep both paths in code during
     Phase 3, decide based on what compiles.
  5. **What's the actual Per-Eval delta?**  The 80-150 ns
     estimate in ANALYSIS.md is a model, not a measurement.
     Phase 7's bench is the ground truth.
  6. **Will RE2 vendor cleanly into compiler_v3?**  The
     exp1_re2 prototype proved RE2 compiles to wasm; whether
     it integrates with our cel-host trampoline ABI requires
     real work.  Likely a follow-up milestone, not in scope
     of the migration itself.

## 8 Definition of done

When the new branch can demonstrate ALL of:

  - [ ] `bazel test //compiler_v3/...` all green.
  - [ ] `bazel run //compiler_v3/conformance:run_conformance`
    reports 1144 PASS (matching v2 baseline).
  - [ ] `bench/v2_vs_v3_bench` produces a side-by-side CSV
    across the 13-expression workload set.
  - [ ] RESULTS.md in this directory captures the deltas
    with a go/no-go recommendation backed by numbers.
  - [ ] A regex-using expression evaluates end-to-end under
    v3 (smoke-test of the architectural payoff — vendor RE2
    or just std::regex with exception support, doesn't have
    to be production-grade).
  - [ ] All affected docs in `doc/implementation-plan/`
    updated (§2.9 list in ANALYSIS.md).

Then the user can decide:
  - Adopt v3 (rip out v2 in a follow-up).
  - Keep v2 (revert the branch, ship the lessons in the
    docs).
  - Hybrid (cherry-pick specific improvements).

## 9 Files in this directory after this commit

```
wasm_compilation_experiments/exp2_wasi_migration/
├── CLAUDE_Do_NOT_DELETE_OR_REVERT_FILES_IN_THIS_DIR
├── ANALYSIS.md                  # 1,082-line comprehensive analysis
├── HANDOFF.md                   # this doc
├── WORK_PLAN.md                 # slice-by-slice task list
└── BENCHMARK_DESIGN.md          # v2-vs-v3 methodology
```

Plus, dragged across to the new branch's directory once the
user creates it:
  - `wasm_compilation_experiments/exp1_re2/` (toolchain
    setup reference + RE2-in-wasm proof).
  - `wasm_compilation_experiments/PLAN.md` (strategic
    context).
  - `wasm_compilation_experiments/WASI_AND_PORTABILITY.md`
    (browser/edge target story).

## 10 Final note on scope honesty

The pattern in the previous estimates was: enumerating call
sites, deciding each one was "mechanical", multiplying by
average per-site cost.  That undercounts:

  - **Design decisions** (allocator strategy, layout
    strategy, lifecycle model) that take a session apiece
    to settle.
  - **Test fixture rebaselining** which is per-test-file
    real engineering — not a sed.
  - **Conformance debug** when lifetime corners diverge —
    expect 1-2 days of "this row passes under v2, fails
    under v3, why?".
  - **Parallel-tree maintenance** — every v2 change during
    the migration window has to be evaluated for v3
    porting.

The 16-session estimate accounts for all of those.  It is
**honest** — not padded.  A faster engineer + careful
sequencing could land closer to 12 sessions; a slower one
or unexpected issues with mspace / wasi-libc could push it
to 20.

The user is right that 4.5 was "drastically under" —
it was understating by roughly 4×.
