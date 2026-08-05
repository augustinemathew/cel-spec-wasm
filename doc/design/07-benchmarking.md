# Benchmarking design

Status: current — authored 2026-06-10 from the design-rebuild notes
(doc/design/notes/). Supersedes: benchmark/DESIGN.md (as design authority) and
doc/implementation-plan/rewrite/wasi/POST_MIGRATION_BENCH.md (historical
baselines). The operator manual for running individual benches is
`benchmark/README.md`.

> **2026-06-11 restructure:** the original two-tree split (`bench/` vs
> `benchmark/`) was dissolved; `bench/` no longer exists. Kernel localisation
> lives at `//benchmark/kernel`, Compile/Plan at `//benchmark/compiler`, and
> the comparative eval corpus at `benchmark/eval`. `bench/README.md` is
> archived at `doc/implementation-plan/rewrite/archive/bench-tree-readme.md`;
> `bench/*` references below are historical. (`//benchmark/plugin` was deleted
> with the plugin backend, 2026-08-04.)

This doc is the design authority for performance measurement: what we measure,
under what configuration a number counts as a baseline, how the comparative
harness works, what an honest published table must contain, and which
performance facts follow from the architecture itself.

## 1. Two jobs: localisation vs publication

Benchmarking has two deliberately separate jobs (benchmark/DESIGN.md §2):

- **Regression localisation (celwasm vs itself).** Google Benchmark
  `cc_binary` targets, all `manual`-tagged so `bazel test //...` skips them.
  Their job is attribution: a kernel-µbench delta with no matching Eval delta
  is a kernel regression; an Eval delta with no kernel delta points at the
  trampoline / instantiate / unwind path.
- **Publication (celwasm vs cel-cpp).** One YAML corpus
  (`benchmark/eval/corpus/`, 13 surface files, 232 cells) drives two
  linkage-isolated binaries — `celwasm_bench` and `celcpp_bench` — registering
  byte-identical Google Benchmark names per cell; `report.sh` joins the JSON
  outputs by name. Its output becomes published tables.

Historical notes, both resolved 2026-06-11: the orphaned
`bench/cel_pipeline_bench.cc` (V35) is superseded by
`//benchmark/compiler:stage_bench`; the out-of-bazel
`bench/foreign_component/` probe set was deleted — findings remain in
rw/m23-foreign-fn-component-abi.md. (Its successor
`//benchmark/plugin:plugin_bench` was deleted with the plugin backend,
2026-08-04.)

## 2. Measured boundaries

The bench taxonomy mirrors the host-side caches an embedder amortises:
`cel::Compiler` (pure data, reused per declared-variable set), `cel::Program`
(wasm bytes + ABI, reused across Plans), and `cel::Instance` (store + memory +
exports, reused across Evals). The invariant: **the timed region is always the
innermost boundary; everything outside it is pre-staged before the loop.**

| Boundary                     | Measured by                          |
|------------------------------|--------------------------------------|
| Raw native kernel call       | `kernel_bench`                       |
| `Compiler::Compile`          | `BM_Compile_*` (pipeline_bench)      |
| `Engine::Plan`               | `BM_Plan_*` (pipeline_bench)         |
| `Instance::Eval`             | `BM_Eval_*`, all of `benchmark/eval` |
| cel-cpp comparator           | `*_cel_cpp_bench`, `celcpp_bench`    |
| Program size (not timing)    | `program_size_main`                  |

Per-row staging rules:

- **Raw kernel call**: `kernel_bench` links the native `//runtime:cel_runtime`
  `cc_library`, bypassing wasmtime; operands staged outside the loop (see the
  arena-rewind caveat below).
- **Compile**: fresh compile per iteration; Compiler pre-built per
  declared-variable set.
- **Plan**: Program pre-built; measures incremental module-new + instantiate.
- **Eval**: Compile + Plan happen in the registration lambda; only
  `Instance::Eval(act)` is timed (celwasm_bench.cc `RegisterAll`).
- **cel-cpp comparator**: eval steady state only; parse + check +
  CreateProgram outside the loop.
- **Program size**: `program_size_main` prints expr-module wasm bytes at
  optimize_level 0 vs 2, AST wire size, and `sizeof` of public C++ types.

Engine sharing: one process-static `GlobalEngine()` everywhere.

Two formerly hand-coded shapes are corpus cells now: the activation-marshal
isolation pair (`arith.abcAbcShape{Vars,Lit}`, arithmetic.yaml) and the
large-N eval shapes (1M-int bound list at first/last/absent positions; 1K
50-byte IAM-permission strings, bound — `in_list.bound*` / `in_list.iam*`,
lists.yaml). Compile/Plan-boundary timings for the LITERAL-source flavours
stay in `//benchmark/compiler:in_operator_compile_bench`.

**kernel_bench arena-rewind caveat.** The allocating kernel benches rewind the
arena cursor by poking the word at `cel_mem_base()+8` (kernel_bench.cc) — a
layout dead since the WASI migration moved arena state to BSS (cel_arena.c).
Allocating rows therefore never reclaim, hit the 64 KiB cap mid-run, and time
OOM/poison paths instead of the kernel (register row R59).

> **Open question (V34):** fix kernel_bench's dead cursor pokes (replace with
> `arena_reset()` per iteration + re-staging, or a real rewind API), re-run
> `bazel run -c opt //benchmark/kernel:kernel_bench`, and diff against the
> published numbers. Until then, allocating kernel_bench rows are not
> production-shape.

## 3. The production-config contract

A number only counts as a baseline if ALL FOUR axes are at production
strength:

1. **Bazel config: `-c opt`, always.** Debug builds are ~10× off, and `-c opt`
   activates `CEL_LOG_DISABLED` via runtime/BUILD.bazel's `opt_mode`
   config_setting. In fastbuild every public kernel opens with
   `CEL_LOG("enter")` — on the wasm runtime a wasm→host fprintf trampoline —
   worth 1.4–5.7× on aggregate-heavy Eval rows.
2. **Runtime build: `-O3 -flto` on both the native and wasm32 cross-compile.**
   LTO is load-bearing after the `cel_runtime.c` per-topic TU split: hot loops
   (e.g. the `in`-scan) only inline their helpers across TU boundaries through
   LTO (runtime/BUILD.bazel).
3. **Binaryen `optimize_level = 2` on every expr-module compile.** Default
   `Compile()` is level 0 — fine for tests, never for benches. The canonical
   seam is the `kBenchOptimizeLevel = 2` constant plus a `CompileOrDie` that
   always sets it (in_operator_bench.cc; celwasm_bench.cc).
4. **Link mode — the bench-default deviation.** `CompilerOptions::link_mode`
   defaults to `kStatic` (compiler/compiler.h:145), but `celwasm_bench`'s
   `BenchLinkMode()` defaults to `kDynamic` — deliberate, for comparability
   with historical dynamic-mode baselines. `benchmark/eval/run.sh` neutralises
   the mismatch by running the binary once per mode (`--link_mode=dynamic`
   then `static`, consumed from argv before Google Benchmark initialises) —
   static vs dynamic is a published axis, not a build fork. The trap: a bare
   `bazel run //benchmark/eval:celwasm_bench` silently measures the
   NON-default (dynamic) mode.

**The one sanctioned deviation from axis 3** is `pipeline_bench`: its
default-config benches pair with explicit `_Opt2` variants so a reviewer reads
the optimizer's Compile-up/Eval-down trade-off in one table. Caveat as
published: the twenty-term "pair" currently compiles DIFFERENT expressions —
`a < b && b < c && …` in the default benches vs `a + b + … == t` in `_Opt2`
(pipeline_bench.cc; register row R58) — so the headline "-52% Eval" compares
unlike workloads. Until the pair is actually paired, treat the trade-off
table's deltas as indicative, not citable.

## 4. The comparative harness

### 4.1 Corpus and loader

Cells live in `benchmark/eval/corpus/*.yaml` — one file per surface
(arithmetic, comparisons, comprehensions, conversions, index, lists, logic,
long_strings, maps, size, strings, ternary, time). Schema per cell: `id`,
`source`, optional `activation` (name → `{type, value}`), `expected`, optional
`tags` (corpus_loader.h).

`corpus_loader.{h,cc}` is comparator-neutral by construction — its own
`CelValueLiteral` type, zero first-party deps — so the cel-cpp binary can link
it. It hard-fails at startup on YAML errors, surface ≠ file basename,
malformed ids, duplicate `(surface, id)`, unknown types, and unbound/unused
activation variables (opt-out tag `skip-source-check`); cells return sorted by
`(surface, id)`. Activation values are **scalar-only** today; aggregate
literals return a loud `UnimplementedError` (corpus_loader.cc) — so aggregate
operands are constructed in the expression source and reduced to a scalar
(`size(…)`, `int(…)`, `.getSeconds()`), with the documented consequence that
`in_list`/`map_*`/`size_map*` cells time literal construction + operation
(OPERATORS.md).

`CanonicalForm` serialises doubles via shortest-round-trip `std::to_chars`
(mirroring `runtime/cel_convert_double_format.cc`) so all comparators print
byte-identical doubles; `AbbreviateForLabel` truncates >64-byte payloads
identically in both binaries.

### 4.2 Linkage isolation

cel-cpp's `cel::Value` / `cel::Activation` clash with our first-party `cel::`
aliases under archive-scan order, so every cel-cpp-side bench is a standalone
TU with zero first-party deps (`linkstatic = True`; the only shared code is
the neutral corpus loader). This also explains the proto-message corpus
exclusion (OPERATORS.md). The cel-cpp comparator is configured for parity:
qualified type identifiers + heterogeneous equality (runtime),
`max_recursion_depth = 16384` (parser — the 250/1000-term chains exceed the
default 32), cross-numeric comparisons (checker) (celcpp_bench.cc).

### 4.3 The same-BM-name join

Both binaries register one benchmark per cell with byte-identical names —
`BM_<prefix(surface)>_<id>` via a hand-maintained `BmPrefixForSurface` table —
and stamp a pre-loop `result=… (type)` label in a byte-identical format so
labels diff mechanically. `report.sh` joins the two JSON outputs by name.
The weak point: the prefix table and corpus-file list are hand-duplicated
across the two mains, and corpus paths are cwd-relative with no runfiles
lookup.

> **Open question (V38):** do corpus paths survive non-repo-root invocation,
> and do the two mains' prefix/file tables agree? Run
> `bazel-bin/benchmark/eval/celwasm_bench --benchmark_filter=BM_arith_intAdd2`
> from repo root AND another cwd; diff the two static tables (celwasm_bench.cc
> vs celcpp_bench.cc). A shared header or a pin test would close this.

### 4.4 Skip tags and the coverage ledger

The no-silent-gap rule: a cell a comparator cannot run stays in the YAML with
a reason-bearing tag — `celwasm-skip-<reason>` (cel-cpp still runs it),
`celcpp-skip-<reason>` (celwasm may run it), or both (documented grid
exclusion). `OPERATORS.md` (benchmark/eval/corpus/) is the authoritative
coverage ledger, including skipped cells. Because skipped cells are visible,
the corpus has doubled as a correctness probe — it surfaced the
ternary-ident-cond null bug, the dynamic-mode silent rodata miscompare, and
heterogeneous-equality checker gaps (OPERATORS.md "Findings").

### 4.5 Dynamic registration over codegen

Benchmarks are registered at startup from the parsed YAML
(`RegisterBenchmark` per cell); adding an operator is a YAML edit, not a C++
edit. The generate-C++-via-genrule design was explicitly rejected
(benchmark/DESIGN.md §5.3).

## 5. Honest-reporting rules — structural requirements

Requirements on any published table, not stylistic preferences. The founding
admission is on record: the original "AOT beats interpretation" thesis was
wrong for short expressions (benchmark/DESIGN.md §1.4); the rules exist so
that kind of finding is printed, not buried.

1. **Headline tables carry wins AND losses per family** — never an
   arithmetic-only or wins-only cut.
2. **A dedicated largest-losses table with named architectural causes.** The
   2026-06-09 publish names them: constant-aggregate rebuild per Eval behind
   the `map_*`/`in_list` losses; SIMD-less byte loops behind string losses;
   the per-Eval wasm-boundary floor behind every short-expression loss
   (doc/implementation-plan/rewrite/m28-bench-results.md §3).
3. **Crossover columns must admit defeat**: when we never win, the column
   reads "n/a (we never win)" — mandated by benchmark/DESIGN.md §12.4,
   implemented verbatim in report.sh.
4. **Suspicious wins carry caveats** (e.g. the 48× regex win's
   cache-vs-no-cache footnote, m28-bench-results.md §2†).
5. **Claims that fail reproduction are downgraded in print** (the 31× →
   17–22× downgrade in m28-bench-results.md §4).
6. **The same discipline applies celwasm-vs-itself**: bench/README's
   arena-vs-proto crossover section names where the arena path is 3–4×
   SLOWER.
7. **Skipped cells are visible** (§4.4) — a published grid never silently
   drops a cell.

## 6. Baselines and reproduction

### 6.1 Canonical methodology numbers

All from the 2026-06-09 publish run (m28-bench-results.md §3, §5, §6);
reproduce with `benchmark/eval/run.sh` (dual link-mode + cel-cpp, `-c opt`,
min_time 0.5 s; `smoke` drops it to 0.1 s):

- **Per-Eval floor**: 62 ns static / 230 ns dynamic (one wasmtime boundary
  crossing + arena reset + result decode); cel-cpp's equivalent entry cost
  ≈ 45 ns.
- **Per-op slope** (dependent int-add chain): ~1.5–1.9 ns/op static vs
  ~32 ns/op for the cel-cpp tree-walker.
- **Crossover**: ≈ N=10 operations — below it the interpreter's cheaper entry
  wins; above it the slope difference dominates.
- **Noise rule**: treat sub-20% single-run differences as noise (~15%
  run-to-run variance observed on the same cell, same day).

These are dated baselines on the publishing machine, not machine-independent
constants; there is no cross-machine parity story yet (§8).

### 6.2 How a publish run works today — and its honest limits

`benchmark/eval/run.sh` builds both binaries under `-c opt`, runs
celwasm_bench once per link mode plus celcpp_bench once, and hands each
(mode, cel-cpp) JSON pair to `report.sh`. But `report.sh` is a 4-operator
tool: it hardcodes `OPS=(intAdd intMul intSub doubleAdd)` over the
{2,10,50,250,1000} length sweep — 20 cells of 232 — and stamps a hardcoded
"Parity verified for all 20 cells (eyeballed…)" line into every report
regardless of input (report.sh, verified).

**The published full-corpus analysis is currently not reproducible.** The
13-family geomean / win-loss tables in m28-bench-results.md cannot come from
report.sh, yet that doc cites run.sh as its reproduction path; the analysis
pipeline behind §1–§5 of the publish is unrecorded (register row R55). Raw
JSONs live in /tmp, uncommitted. The never-shipped layer benchmark/DESIGN.md
promised (parity binary, `report.py` + test, comparator wrappers, committed
`results/`) does not exist (register row R54).

> **Open question (V36):** how were the full-corpus tables produced? Run
> `benchmark/eval/run.sh smoke`, diff report.sh's output shape against
> m28-bench-results.md §1–§5; recover or rewrite the analysis pipeline — and
> commit it — before the next publish. Until then the full-corpus tables are a
> dated artifact, not a regenerable baseline.

### 6.3 Parity is currently eyeballs

Nothing machine-compares the `result=` labels across binaries or against the
cell's `expected` literal — celwasm_bench never validates its Eval result
against `expected`, so a wrong-value regression times successfully. The
dynamic-mode rodata miscompare was caught by a human reading labels. Machine
parity (label diff + expected-value check) is the top item in §8.

## 7. Perf-model facts from the architecture

Embedder-visible performance truths that follow from design decisions; benches
must state them rather than let readers infer wrong causes.

- **Per-Eval floor by link mode** (§6.1): every Eval pays one wasm boundary
  crossing + `arena_reset` + result decode; static linking cuts the floor
  ~3.7× vs dynamic (62 vs 230 ns, dated 2026-06-09).
- **`&&` / `||` always evaluate both operands.** They lower through the
  general slot-out call arm — eager, with 3VL absorption inside
  `cel_and`/`cel_or`. Spec-equivalent because CEL is side-effect-free; only
  `_?_:_` gets genuinely lazy arms (BinaryenIf) (expr_lower.cc kCall dispatch
  ladder; notes/codegen-lowering.md §5.2).
- **Constant aggregates rebuild per Eval.** Lists/maps are arena-built per
  evaluation, never rodata (static_memory_builder.cc) — the named
  architectural cause behind the `in_list`/`map_*` family losses, and why
  those corpus cells time construction + operation.
- **The arena cliff is transitional.** The notes-era kernel had a hard 64 KiB
  per-Eval arena (~2,700 list elements at 24 B, ~1,350 map entries at 48 B);
  the in-flight merge replaces it with a chained grow-on-demand arena
  (4 KiB–1 MiB chunks, `runtime/cel_arena.c` `pick_grow_size`) and moves the
  binding constraint for large literals to the compile-time static-region gate
  (`ValidateExprStaticRegion`; `04-runtime.md` §7 — the historical 10K-list
  wasmtime panic was a workspace overrun, not an arena bug). Arena-bound
  corpus cells and cliff baselines must be re-measured once the merge
  settles. Comprehension accumulators remain pre-sized at the prologue with a
  runtime trap on overflow (expr_lower_comprehension.cc).
- **CEL_LOG in fastbuild is a 1.4–5.7× Eval tax** (§3 axis 1) — never
  benchmark a fastbuild binary.
- **The Engine caches the parsed runtime module**, bench-justified at ~34×
  per-Plan (~64× with process sharing); the per-Plan expr-module re-parse is
  the accepted default with a named future cache seam (eval/engine.h).
- **Workspace slot reuse is transitional.** The notes pinned
  `SlotAllocator::Release` as a no-op — one 24 B slot per
  kSelect/kCall/aggregate node (notes/codegen-memory.md §2.1); the
  working-tree `compiler/codegen/slot_allocator.cc` already shows free-list
  reuse. Workspace-growth claims and large-expression benches must be
  re-baselined once that merge settles.

## 8. Future work and known gaps

Two cautionary tales motivate the top items. **R56**: the duration/timestamp
and Any kernel benches in kernel_bench have been permanently dead behind
`#ifdef CELWASM_M7B_SHIPPED` (defined nowhere) since the features shipped
2026-05-16 — and have name-drifted meanwhile (they call `cel_ts_year_utc`; the
shipped kernel is `cel_ts_year_utc_at_v`, runtime/cel_time.h), so flipping the
guard likely won't compile. **R59**: the kernel_bench arena-cursor poke went
stale across the WASI migration and silently turned allocating rows into
OOM-path timings (§2). Both rotted invisibly because manual-tagged bench
binaries are never built in any test sweep.

> **Open question (V37):** does flipping `CELWASM_M7B_SHIPPED` even compile?
> `bazel build -c opt //benchmark/kernel:kernel_bench
> --copt=-DCELWASM_M7B_SHIPPED` (expected: failure on the drifted kernel
> names) settles delete-vs-fix.

Priority order:

1. **Bench build-smoke in CI** — build every manual bench binary in a CI
   sweep so R56/R59-style bit-rot is caught at the causing commit.
2. **Machine parity** — replace eyeballed `result=` labels with a label diff
   across binaries plus an expected-value check inside celwasm_bench (§6.3);
   delete report.sh's hardcoded parity line.
3. **Rebuild and commit the full-corpus analysis pipeline** (V36) before the
   next publish; commit results artifacts instead of /tmp.
4. **A perf analogue of the conformance monotonic gate** — per-machine-class
   floor/slope baselines with a pre-push check failing on regression beyond
   the noise rule (precedent: conformance/.baseline,
   scripts/check_conformance_monotonic.sh).
5. **Machine parity of baselines** — record machine specs with every publish;
   define how baselines transfer (or don't) across machines.
6. **Fix the `_Opt2` pairing** (R58) so the trade-off table compares like
   workloads; fix the misnamed `BM_Eval_LongArith_10kTerms` (1,000 terms,
   three contradicting comments — R57, mirrored in the cel-cpp sibling).
7. **Recorded P2 debt**: cell-count drift (DESIGN.md/OPERATORS.md say 229;
   the corpus is 232); OPERATORS.md claims a CI parser that doesn't exist;
   DESIGN.md §6.4.4's mandatory `purpose:` field is unparsed; stale
   function-name cross-references in report.sh; stale future-work rows in
   bench/README.md; cwd-relative corpus paths + duplicated prefix/file tables
   (V38); scalar-only activation values constrain corpus design; the
   m28-bench-results §2†-vs-§6 internal contradiction.
