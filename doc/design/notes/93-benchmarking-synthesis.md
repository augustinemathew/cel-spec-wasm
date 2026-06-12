# 93-benchmarking-synthesis — cross-component lens

> **2026-06-11 restructure:** the two-tree split (bench/ vs
> benchmark/) described below was dissolved; bench/ no longer exists.
> Kernel localisation lives at //benchmark/kernel, component-boundary
> at //benchmark/component, Compile/Plan at //benchmark/compiler, and
> the comparative eval corpus at benchmark/eval. bench/README.md is
> archived at doc/implementation-plan/rewrite/archive/bench-tree-readme.md.
> Section references to bench/* below are historical.

Lens pass over all per-component notes in this directory, synthesizing
the raw material for the new benchmarking design doc. Primary source:
`benchmarking.md`; bench-relevant facts also drawn from
`runtime-kernel.md`, `codegen-lowering.md`, `codegen-memory.md`,
`eval-public.md`, `compiler-toplevel.md`, `testing-system.md`,
`design-heritage.md`, `doc-index.md`. Disagreements between notes were
settled against code (§6), not just reported. Verified 2026-06-10 on
branch `m28-configurable-linking`.

## 1. The two-tree architecture (bench/ vs benchmark/)

The split is **regression localisation vs publication**, and the new
doc should present it as the top-level design decision:

- **`bench/`** — celwasm-vs-itself Google Benchmark binaries, all
  `manual`-tagged (bench/BUILD.bazel:5-13). Answers "which of OUR
  layers regressed": a kernel-µbench delta vs an Eval delta attributes
  a regression to kernel / trampoline / codegen (bench/README.md:252-274;
  benchmarking.md §5.1). Six registered targets (verified: exactly six
  `cc_binary` rules in bench/BUILD.bazel — kernel_bench,
  pipeline_bench, in_operator_bench, in_operator_cel_cpp_bench,
  foreign_component_bench, program_size_main).
- **`benchmark/`** — the comparative, adoption-grade system: one YAML
  corpus (`benchmark/eval/corpus/`, 232 cells across 13 surfaces,
  verified by `grep -c "  - id:"`) drives two linkage-isolated
  binaries (`celwasm_bench`, `celcpp_bench`) that register
  byte-identical BM names per cell; `report.sh` joins post-run by
  name (benchmark/README.md:1-49; benchmark/DESIGN.md §1.2, §5.1).
  DESIGN.md §2 states the non-goal: benchmark/ does NOT replace
  bench/ (DESIGN.md:94-96).
- Unregistered material under bench/ is part of the story:
  `bench/cel_pipeline_bench.cc` is **orphaned source** (no BUILD
  target — confirmed §6.2), and `bench/foreign_component/` is a
  deliberately out-of-build cargo/wasm-tools probe set kept as
  empirical backing for the m23 boundary-cost numbers
  (bench/foreign_component/README.md:1-24).
- `benchmark/compiler/` is a TODO.md only — Compile/Plan comparative
  benches are specified but deferred (benchmarking.md §1.3 last
  bullet). The doc should carry this as explicit future work, not
  imply compile-side comparison exists.

## 2. Measured boundaries

The bench taxonomy mirrors the three real host caches — Compiler /
Program (Compile) / Instance (Plan) / Eval — and the timed region is
always the innermost boundary with everything else pre-staged
(bench/README.md:204-223; benchmarking.md §5.4):

| boundary | measured by | staging rule |
|---|---|---|
| raw native kernel call (no wasmtime) | `kernel_bench` (links native `//runtime:cel_runtime`) | operands staged outside loop; arena cursor rewound per iter by poking `cel_mem_base()+8` (kernel_bench.cc:66-69, 451-464) |
| `Compiler::Compile` | `BM_Compile_*` in pipeline_bench | fresh compile per iteration |
| `Engine::Plan` | `BM_Plan_*` | Program pre-built |
| `Instance::Eval` | `BM_Eval_*`, all of benchmark/eval | Compile+Plan in the registration lambda, before the timed loop; only `Eval(act)` timed (celwasm_bench.cc:87-96, 254-298) |
| component-call overhead | `foreign_component_bench`: `AddComponent` dispatch vs `AddTypedFunction` host-callback baseline on the same decl; the delta IS the boundary cost (foreign_component_bench.cc:20-28) |
| cel-cpp comparator | `in_operator_cel_cpp_bench` + `celcpp_bench`: eval-steady-state only, parse+check+CreateProgram outside the loop |
| program size (not timing) | `program_size_main`: wasm byte sizes at O0 vs O2, AST wire size, `sizeof` of public types |

Engine sharing: one process-static `GlobalEngine()` everywhere
(construction amortised in real use) EXCEPT foreign_component_bench,
where each BM owns a heap Engine because AddComponent state interferes
across Plans (foreign_component_bench.cc:176-183).

Perf facts from other components the boundary doc must state:

- **The Engine caches the parsed runtime module**; this is
  bench-justified at ~34× per-Plan (~64× with process sharing) and the
  per-Plan expr re-parse is the accepted default with a named future
  cache seam (eval-public.md §5; eval/engine.h:23-27, 75-78).
- **`&&`/`||` always evaluate both operands** (eager slot-out lowering
  with 3VL absorption in `cel_and`/`cel_or`) — spec-equivalent because
  CEL is side-effect-free, but a perf fact benches should state
  (codegen-lowering.md §2.3, §5.2; expr_lower.cc:1180-1193).
- **Constant aggregates rebuild per Eval** (lists/maps are arena-built
  per evaluation, never rodata; static_memory_builder.cc:139-153) —
  the named architectural cause behind the `in_list`/`map_*` losses
  in m28-bench-results §3 and the documented caveat that those cells
  time literal construction + operation (OPERATORS.md:34-41).
- **The 64 KiB bump arena is a hard per-Eval cliff**: ~2,700 list
  elements / ~1,350 map entries; pinned as skipped known-bug tests at
  4 000 (graceful OVERFLOW) and 10 000 (wasmtime panic — backlog #16)
  (runtime-kernel.md §1.9; e2e/known_bugs_test.cc:156-185, 617-692).
  runtime-kernel.md §1.7 exonerates the kernels (every in-runtime
  `arena_alloc` consumer checks the 0 return), so the 10K panic
  originates outside `runtime/` — bench docs must not repeat backlog
  #16's "runtime-side unchecked allocation" attribution.
- **Workspace slots never get reused**: `SlotAllocator::Release` is a
  no-op (verified, §6.1), so workspace grows 24 B per
  kSelect/kCall/aggregate node — relevant to any future
  large-expression bench and to the unbounded-workspace P0 in
  codegen-memory.md §2.1.

## 3. Production-config rules

Three orthogonal axes, all required for a number to count as a
baseline (bench/README.md:31-60; CLAUDE.md "Benchmark configuration"):

1. **Bazel config: `-c opt` always.** Debug is ~10× off, and `-c opt`
   also activates `CEL_LOG_DISABLED` via runtime/BUILD.bazel's
   `opt_mode` config_setting — in fastbuild every public kernel pays a
   wasm→host fprintf trampoline, worth 1.4–5.7× on Eval rows
   (bench/README.md:45-54; POST_MIGRATION_BENCH.md:78-98;
   runtime-kernel.md §1.5 confirms the `CEL_LOG("enter")` on every
   public helper and the `-DCEL_LOG_DISABLED` wiring at
   runtime/BUILD.bazel:83-92, 152-155, 625-628).
2. **Runtime flags: `-O3 -flto` on both native and wasm32 builds.**
   LTO is load-bearing post the cel_runtime.c TU split (cross-TU
   inlining of the `in`-scan hot loop) (bench/README.md:35-44;
   runtime-kernel.md §5 confirms at runtime/BUILD.bazel:143-151,
   619-624).
3. **Binaryen `optimize_level = 2`** on every expr-module compile via
   `kBenchOptimizeLevel = 2` + a `CompileOrDie` that always sets it
   (in_operator_bench.cc:142-153; celwasm_bench.cc:85-96). Default
   `Compile()` is level 0 — fine for tests, never for benches.
   `pipeline_bench` is the one sanctioned deviation: default-config
   benches paired with `_Opt2` variants so the optimizer trade-off
   reads in one table — but see §6.3: the twenty-term "pair" currently
   compiles different expressions.

A fourth axis arrived with m28 and needs first-class treatment:

4. **Link mode.** `celwasm_bench` defaults `kDynamic` (verified:
   celwasm_bench.cc:48-58) for comparability with historical
   baselines, while the production default is `kStatic`
   (compiler/compiler.h:145). `run.sh` runs both modes and joins each
   against the same cel-cpp JSON, making static-vs-dynamic a published
   axis, not a build fork (run.sh:24-56; benchmarking.md §5.6). The
   doc must state the default-mismatch explicitly: a bare
   `bazel run …:celwasm_bench` silently measures the non-default mode
   (benchmarking.md §2.10).

## 4. Baseline management — what exists and what doesn't

What exists today:

- **Methodology constants worth canonising** (m28-bench-results.md §3,
  §5, §6): 62 ns static / 230 ns dynamic per-Eval floor vs ~45 ns
  cel-cpp; ~1.5–1.9 ns/op static slope vs ~32 ns cel-cpp; crossover ≈
  N=10 ops; treat sub-20% single-run differences as noise.
- **The skip-tag regime** as the no-silent-gap rule:
  `celwasm-skip-<reason>` / `celcpp-skip-<reason>` / both-tagged =
  documented grid exclusion; OPERATORS.md is the authoritative
  coverage ledger and has doubled as a correctness probe
  (ternary-null bug, dynamic-mode silent rodata miscompare,
  het-eq checker gaps — OPERATORS.md:317-340).
- **Dual link-mode runs** joined against one cel-cpp baseline
  (run.sh).
- A conformance-style precedent for gating exists next door: the
  monotonic PASS-count baselines per link mode
  (`conformance/.baseline`, `check_conformance_monotonic.sh`;
  testing-system.md §1.2). Nothing equivalent exists for perf.

What does NOT exist (each verified):

- **No reproducible full-corpus analysis pipeline.** `report.sh`
  hardcodes `OPS=(intAdd intMul intSub doubleAdd)` (verified
  report.sh:26) — 4 operators / 20 cells out of 232 — and stamps a
  hardcoded "Parity verified for all 20 cells (eyeballed…)" line into
  every report (verified report.sh:108). The published 13-family
  geomean / win-loss tables in m28-bench-results.md are NOT producible
  by report.sh, yet that doc cites run.sh as the reproduction path
  (benchmarking.md §2.3; validation item 4 — the §1–§5 analysis
  pipeline is unrecorded).
- **No committed results.** Raw JSONs live in /tmp per
  m28-bench-results.md:134; the DESIGN.md `results/` dir, `report.py`
  + `report_test.py`, `parity_check_main.cc`, comparator wrapper
  layer, and `profile.sh` were never built (benchmarking.md §2.2).
- **No machine-checked parity.** `result=` labels are byte-identical
  by construction (shared 64-byte truncation helper) but nothing
  compares them across binaries or against the cell's `expected`
  literal — a wrong-value regression times successfully; the
  dynamic-mode rodata miscompare was caught by a human
  (benchmarking.md §4 gaps a/e).
- **No CI presence at all for bench binaries** (manual tag, never
  built in test sweeps) — which is how the kernel_bench M7B bit-rot
  (§6.4) stayed invisible.

## 5. The honest two-sided-narrative rule

Structural, not stylistic (benchmarking.md §1.5, §5.7; user memory
`feedback_honest_perf_narrative`). The elements to codify:

1. Headline tables carry **wins–losses per family**
   (m28-bench-results.md §1).
2. A dedicated **largest-losses table** with named architectural
   causes (constant-aggregate rebuild per Eval; SIMD-less byte loops;
   the 62 ns wasm-boundary floor) — m28-bench-results.md §3.
3. **Crossover columns must admit defeat**: DESIGN.md §12.4 mandates
   "n/a (we never win)" when true; report.sh implements exactly that
   string (report.sh:92-103) and prints ratio direction explicitly.
4. **Suspicious wins carry caveats** (the 48× regex win's
   cache-vs-no-cache footnote, m28-bench-results §2†).
5. **Claims that fail reproduction are downgraded in print** (the 31×
   → 17–22× downgrade, m28-bench-results §4).
6. The founding admission is on record: DESIGN.md §1.4 opens by
   recording that the original "AOT beats interpretation" thesis was
   wrong for short expressions.
7. The same discipline applies celwasm-vs-itself: bench/README's
   arena-vs-proto crossover names where the arena path is 3-4× SLOWER
   (bench/README.md:194-202).

## 6. Cross-note disagreements — settled

1. **SlotAllocator free-list: design-heritage.md vs codegen-memory.md.**
   design-heritage.md §1.1 (design.md §6 row) claims "`Release` is no
   longer a no-op — free-list reuse landed"; codegen-memory.md §1.5
   says Release is a no-op. **codegen-memory is right**:
   `SlotAllocator::Release` is `(void)offset;` with the comment "Naive
   path (M1–M9): no-op. At M10 this returns `offset` to a free-list"
   (verified compiler/codegen/slot_allocator.cc:24-28), and
   `peak_slots_` increments per Acquire (slot_allocator.cc:18-22) —
   total-acquires, not peak liveness. design-heritage's supersession
   row for design.md §6 must be corrected before the new doc inherits
   it (it read the header's *intended* discipline at
   slot_allocator.h:64-77 as shipped).
2. **`//bench:cel_pipeline_bench` orphan.** benchmarking.md §2.1 vs
   the manual-gate catalog. Confirmed: bench/BUILD.bazel declares six
   cc_binary targets, none named `cel_pipeline_bench`, while
   `bench/cel_pipeline_bench.cc` exists on disk and BOTH
   per-component-test-coverage.md:94 and
   POST_MIGRATION_BENCH.md:5-9,125 cite the target as a runnable gate
   (verified by grep). Decide register-or-delete; fix both docs in the
   same commit.
3. **The `_Opt2` "paired" twenty-term benches compile different
   expressions.** Confirmed: `BM_Compile_TwentyTermCompare` builds
   `a < b && b < c && …` (pipeline_bench.cc:162-170);
   `BM_Compile_TwentyTermCompare_Opt2` and `BM_Eval_TwentyTermCompare_Opt2`
   build `a + b + … + s == t` (pipeline_bench.cc:473-481, 507-515).
   The README trade-off table compares unlike workloads; the doc's
   "sanctioned deviation" story is only honest once the pair is
   actually paired.
4. **Dead M7B/M7A kernel benches won't compile if enabled.**
   Confirmed: `#ifdef CELWASM_M7B_SHIPPED` guards (kernel_bench.cc:558,
   576, 593-687) are defined nowhere in any BUILD/bzl (grep), and the
   guarded code calls `cel_ts_year_utc` (kernel_bench.cc:648, 659)
   while the shipped kernel is `cel_ts_year_utc_at_v`
   (runtime/cel_time.h:109). Both milestones shipped 2026-05-16; the
   benches are permanently dead AND name-drifted.
5. **Corpus cell-count drift.** Confirmed: 232 cells on disk (grep
   sum over corpus/*.yaml), matching m28-bench-results.md:5;
   benchmark/DESIGN.md:490 still says 229 (OPERATORS.md:17 likewise
   per benchmarking.md §2.7).
6. **Bench default link mode ≠ production default.** Confirmed:
   `BenchLinkMode()` static init is `kDynamic`
   (celwasm_bench.cc:55-58); `CompilerOptions::link_mode` default is
   `kStatic` (compiler/compiler.h:145). Deliberate (historical
   comparability; run.sh runs both) but must be documented as a
   deviation from the production default.

## 7. Gaps the readers flagged (consolidated)

P1 — fix before or alongside the new doc:

- report.sh covers 4/232 cells; the m28 full-corpus analysis pipeline
  is unrecorded (§4; benchmarking.md §2.3, validation 4).
- benchmark/README.md + DESIGN.md describe a never-shipped layout
  (parity binary, report.py, wrappers, results/) — re-status or
  rewrite as-shipped (benchmarking.md §2.2).
- Orphaned `cel_pipeline_bench.cc` + two docs pointing at the dead
  target (§6.2).
- Dead, name-drifted M7B/M7A kernel benches (§6.4).
- The `_Opt2` unlike-workload pair (§6.3).
- `BM_Eval_LongArith_10kTerms` is 1000 terms with three mutually
  contradicting comments (benchmarking.md §2.5).
- No machine parity check; celwasm_bench never validates Eval results
  against `expected` (benchmarking.md §4a).

P2 — record in the doc's future-work section:

- Cell-count drift 229 vs 232 (§6.5); OPERATORS.md's claimed CI
  parser doesn't exist (benchmarking.md §2.8); DESIGN.md §6.4.4's
  mandatory `purpose:` field is unimplemented (§2.9); m28-bench-results
  internal §2†-vs-§6 contradiction (§2.11); stale function-name
  cross-references in report.sh and stale future-work rows in
  bench/README (§2.12); corpus paths are cwd-relative with no
  runfiles lookup (validation 5); the two hand-duplicated
  BmPrefixForSurface/kCorpusFiles tables are unpinned (validation 7);
  benchmark/compiler is unbuilt; scalar-only activation values
  (loader UnimplementedError for aggregates) constrain corpus design
  (corpus_loader.cc:96-101).

Open validation items the doc should NOT paper over (benchmarking.md
§3): the wasmtime trap at N=10000 / arena OOM caps (cross-ref
runtime-kernel V1 — the kernels are exonerated, root cause unfound),
and whether corpus paths survive non-repo-root invocation.

## 8. Proposed design-doc outline

`doc/design/benchmarking.md` (reader path position: after testing
strategy, per notes/README.md principle 4):

1. **Why two trees** — localisation (bench/) vs publication
   (benchmark/); the explicit non-goal from DESIGN.md §2; the
   regression-attribution recipe (kernel µbench delta vs Eval delta,
   bench/README.md:252-274). One diagram: the four cache boundaries
   (Compiler/Program/Instance/Eval) with each bench family pinned to
   its boundary.
2. **Measured boundaries** — the §2 table above; the timed-region
   rule (Eval-only, everything pre-staged); the GlobalEngine sharing
   rule and its foreign-component exception; what each special bench
   isolates (activation-marshal pair, link-mode floor, component
   boundary cost, program_size).
3. **Production-config contract** — the four axes (§3): `-c opt` (+
   CEL_LOG kill), runtime `-O3 -flto`, `kBenchOptimizeLevel = 2`,
   link mode (both modes run; bench default kDynamic vs production
   kStatic stated as a deliberate deviation). The pipeline_bench
   default-vs-`_Opt2` pairing as the single sanctioned deviation —
   with the pairing fixed (§6.3). Rule: numbers from any other config
   are not baselines.
4. **The comparative harness** — corpus schema + loader validation
   gates; comparator neutrality / linkage isolation (the cel::Value
   symbol-clash constraint, standalone TUs, linkstatic); the
   same-BM-name join contract and byte-identical `result=` labels;
   dynamic registration over codegen (rejected gen.py alternative);
   skip-tag regime + OPERATORS.md as the coverage ledger; cel-cpp
   comparator configuration (recursion depth, het-eq,
   cross-numeric).
5. **Honest reporting rules** — the seven structural elements of §5,
   stated as requirements on any published table; the corpus's second
   job as a correctness probe.
6. **Baselines and reproduction** — canonical methodology numbers
   (62/230 ns floors, slopes, N=10 crossover, 20% noise rule); how a
   publish run works today (run.sh dual-mode → report.sh) and its
   honest limits (4-operator report; analysis pipeline for
   full-corpus tables to be rebuilt and committed); where raw results
   live; the aspiration: a perf analogue of the conformance
   monotonic-baseline gate, and machine parity (label diff +
   expected-value check) replacing eyeballs.
7. **Perf model facts from the architecture** — the embedder-visible
   truths: per-Eval floor by link mode; eager `&&`/`||`;
   constant-aggregate rebuild per Eval; 64 KiB arena cliff and the
   pre-size/trap pair; CEL_LOG cost in fastbuild; engine module-cache
   amortisation; no workspace slot reuse. Each with its component-doc
   cross-reference.
8. **Future work / known gaps** — §7's P2 list verbatim, plus
   benchmark/compiler, aggregate activation values, the unfound 10K
   trap root cause, bench-binary CI presence (at minimum a
   build-only smoke so manual benches can't bit-rot like M7B).

Doc-status housekeeping the rebuild owes (from doc-index.md's bench
row): bench/README.md stays [live] after the §6.3/§2.12 fixes;
benchmark/DESIGN.md gets re-statused as plan-with-deltas (or its
§§5.2/8/11/12/13 rewritten as-shipped); m28-bench-results.md stays the
published-results exemplar; POST_MIGRATION_BENCH.md is [hist] and its
reproduction command must be fixed or tombstoned alongside the
cel_pipeline_bench decision (§6.2).
