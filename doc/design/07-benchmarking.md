# Benchmarking design

> **2026-06-11 restructure:** the two-tree split (bench/ vs
> benchmark/) described below was dissolved; bench/ no longer exists.
> Kernel localisation lives at //benchmark/kernel, component-boundary
> at //benchmark/component, Compile/Plan at //benchmark/compiler, and
> the comparative eval corpus at benchmark/eval. bench/README.md is
> archived at doc/implementation-plan/rewrite/archive/bench-tree-readme.md.
> Section references to bench/* below are historical.

Status: current — authored 2026-06-10 from the design-rebuild notes
(doc/design/notes/). Supersedes: benchmark/DESIGN.md (as design
authority; that file gets a status banner),
doc/implementation-plan/rewrite/wasi/POST_MIGRATION_BENCH.md
(historical baselines).

This doc is the design authority for performance measurement in this
repository: what we measure, under what configuration a number counts
as a baseline, how the comparative harness works, what an honest
published table must contain, and which performance facts follow from
the architecture itself. The operator manual for running individual
benches is now `benchmark/README.md`.

---

## 1. The two-tree split: localisation vs publication

The top-level design decision is that benchmarking lives in two
deliberately separate trees with different jobs. They are not merged,
and `benchmark/` is explicitly NOT a replacement for `bench/`
(benchmark/DESIGN.md §2).

- **`bench/` — regression localisation (celwasm vs itself).** Google
  Benchmark `cc_binary` targets, all `manual`-tagged so `bazel test
  //...` skips them (bench/BUILD.bazel). Their job is attribution:
  when a number moves, which of OUR layers moved it? A kernel-µbench
  delta with no matching Eval delta is a kernel regression; an Eval
  delta with no kernel delta points at the trampoline / instantiate /
  unwind path (bench/README.md "How to interpret a regression").
- **`benchmark/` — publication (celwasm vs cel-cpp).** One YAML corpus
  (`benchmark/eval/corpus/`, 13 surface files, 232 cells) drives two
  linkage-isolated binaries — `celwasm_bench` and `celcpp_bench` —
  that register byte-identical Google Benchmark names per cell;
  `report.sh` joins the JSON outputs post-run by name. This is the
  adoption-grade, apples-to-apples system whose output becomes
  published tables.

Six registered targets exist under `bench/`: `kernel_bench`,
`pipeline_bench`, `in_operator_bench`, `in_operator_cel_cpp_bench`,
`foreign_component_bench`, `program_size_main` (bench/BUILD.bazel,
verified). Two pieces of unregistered material are part of the story:

- `bench/cel_pipeline_bench.cc` is **orphaned source** — no BUILD
  target declares it, yet two docs cite `//bench:cel_pipeline_bench`
  as a runnable reproduction / gate target
  (per-component-test-coverage.md:94; POST_MIGRATION_BENCH.md).

> **Open question (V35):** register or delete
> `bench/cel_pipeline_bench.cc`. Verify with
> `bazel query 'kind(cc_binary, //bench:all)'` (expect 6, no
> `cel_pipeline_bench`); the same commit that decides must fix both
> docs citing the dead target.
> *Resolved 2026-06-11: the orphaned TU is superseded by
> `//benchmark/compiler:stage_bench`.*

- `bench/foreign_component/` is a deliberately out-of-bazel probe set
  (cargo / wasm-tools / wac driven) kept as the empirical backing for
  the component boundary-cost numbers
  (bench/foreign_component/README.md); it is deleted when superseded,
  not promoted into the build. *(Deleted 2026-06-11; findings remain
  recorded in rw/m23-foreign-fn-component-abi.md, and the production
  bench is `//benchmark/component:foreign_component_bench`.)*

`benchmark/compiler/` was a TODO only at authoring time; as of the
2026-06-11 restructure it hosts the ported Compile/Plan/cold-start
benches (`pipeline_bench`, `stage_bench`,
`in_operator_compile_bench`, `program_size_main`).

<!-- diagram-wanted: the two trees side by side, with the four host
     cache boundaries (Compiler / Program / Instance / Eval) as
     horizontal lanes and each bench family pinned to the lane it
     times; kernel_bench drawn below the wasmtime line, celcpp_bench
     outside the celwasm stack entirely. -->

## 2. Measured boundaries

The bench taxonomy mirrors the host-side caches an embedder amortises:
`cel::Compiler` (pure data, reused per declared-variable set),
`cel::Program` (wasm bytes + ABI, reused across Plans), and
`cel::Instance` (store + memory + exports, reused across Evals) —
bench/README.md "Caching boundaries we exercise". The invariant: **the
timed region is always the innermost boundary; everything outside it
is pre-staged before the loop.**

| Boundary                     | Measured by                          |
|------------------------------|--------------------------------------|
| Raw native kernel call       | `kernel_bench`                       |
| `Compiler::Compile`          | `BM_Compile_*` (pipeline_bench)      |
| `Engine::Plan`               | `BM_Plan_*` (pipeline_bench)         |
| `Instance::Eval`             | `BM_Eval_*`, all of `benchmark/eval` |
| Component-call overhead      | `foreign_component_bench`            |
| cel-cpp comparator           | `*_cel_cpp_bench`, `celcpp_bench`    |
| Program size (not timing)    | `program_size_main`                  |

Per-row staging rules:

- **Raw kernel call**: `kernel_bench` links the native
  `//runtime:cel_runtime` `cc_library` and bypasses wasmtime
  entirely; operands are staged outside the loop (but see the
  arena-rewind caveat below).
- **Compile**: fresh compile per iteration; the Compiler is
  pre-built per declared-variable set.
- **Plan**: the Program is pre-built; the bench measures the
  incremental module-new + instantiate cost.
- **Eval**: Compile + Plan happen in the registration lambda, before
  the timed loop; only `Instance::Eval(act)` is timed
  (celwasm_bench.cc `RegisterAll`; pipeline_bench, in_operator_bench
  follow the same rule).
- **Component-call overhead**: `Engine::AddComponent` dispatch vs an
  `AddTypedFunction` host-callback baseline on the same declaration;
  the delta IS the boundary cost (foreign_component_bench.cc).
- **cel-cpp comparator**: eval steady state only; parse + check +
  CreateProgram sit outside the loop.
- **Program size**: `program_size_main` prints expr-module wasm bytes
  at optimize_level 0 vs 2, AST wire size, and `sizeof` of public
  C++ types — a size probe, not a timer.

Engine sharing: one process-static `GlobalEngine()` everywhere
(construction is amortised in real use), EXCEPT
`foreign_component_bench`, where each benchmark owns a heap `Engine`
because `AddComponent` is per-Plan and shared Engine state interferes
across Plans (foreign_component_bench.cc).

Two shapes that used to need hand-coded benches are corpus cells now:

- The activation-marshal isolation pair lives in arithmetic.yaml as
  `arith.abcAbcShape{Vars,Lit}` (formerly the hand-coded
  `BM_arith_intAdd_AbcAbcShape_{VarsToday,LitToday}` registrations in
  celwasm_bench — the corpus can express var/lit adjacency as two
  cells, and cel-cpp gained the pair for free).
- The large-N eval shapes (1M-int bound list at first/last/absent
  positions; 1K 50-byte IAM-permission strings, bound) live in
  lists.yaml as `in_list.bound*` / `in_list.iam*`.  The
  Compile/Plan-boundary timings for the LITERAL-source flavours stay
  in `//benchmark/compiler:in_operator_compile_bench`.

**kernel_bench arena-rewind caveat.** The allocating kernel benches
rewind the arena cursor by poking the word at `cel_mem_base()+8`
(kernel_bench.cc) — a layout that has been dead since the WASI
migration moved arena state to BSS (cel_arena.c). Allocating rows
therefore never reclaim, hit the 64 KiB cap mid-run, and time
OOM/poison paths instead of the kernel (register row R59).

> **Open question (V34):** fix kernel_bench's dead cursor pokes
> (replace with `arena_reset()` per iteration + re-staging, or a real
> rewind API), re-run `bazel run -c opt //benchmark/kernel:kernel_bench`, and
> diff against the published numbers. Until then, allocating
> kernel_bench rows are not production-shape.

## 3. The production-config contract

Benchmarks measure production-shape numbers, so a number only counts
as a baseline if ALL FOUR axes are at production strength. Numbers
from any other configuration are not baselines.

1. **Bazel config: `-c opt`, always.** Debug builds are ~10× off, and
   `-c opt` is also what activates `CEL_LOG_DISABLED` via
   runtime/BUILD.bazel's `opt_mode` config_setting. In fastbuild every
   public kernel opens with `CEL_LOG("enter")`, which on the wasm
   runtime is a wasm→host fprintf trampoline — worth 1.4–5.7× on
   aggregate-heavy Eval rows (bench/README.md "Build configuration";
   historical measurement, POST_MIGRATION_BENCH.md).
2. **Runtime build: `-O3 -flto` on both the native and the wasm32
   cross-compile.** LTO is load-bearing after the `cel_runtime.c`
   per-topic TU split: hot loops (e.g. the `in`-scan) only inline
   their helpers across TU boundaries through LTO
   (runtime/BUILD.bazel; bench/README.md).
3. **Binaryen `optimize_level = 2` on every expr-module compile.**
   Default `Compile()` is level 0 (byte-identical no-op) — fine for
   tests, never for benches. The canonical seam is the
   `kBenchOptimizeLevel = 2` constant plus a `CompileOrDie` that
   always sets it (in_operator_bench.cc; celwasm_bench.cc).
4. **Link mode — and the bench-default-vs-production-default
   deviation.** `CompilerOptions::link_mode` defaults to `kStatic`
   (compiler/compiler.h:145), but `celwasm_bench`'s `BenchLinkMode()`
   defaults to `kDynamic` (celwasm_bench.cc) — a deliberate choice for
   comparability with historical dynamic-mode baselines.
   `benchmark/eval/run.sh` neutralises the mismatch for publish runs
   by running the binary once per mode (`--link_mode=dynamic` then
   `static`, the flag consumed from argv before Google Benchmark
   initialises) and joining each JSON against the same cel-cpp JSON —
   static vs dynamic is a published axis, not a build fork. The trap
   to know: a bare `bazel run //benchmark/eval:celwasm_bench`
   silently measures the NON-default (dynamic) mode.

**The one sanctioned deviation from axis 3** is `pipeline_bench`: its
default-config benches pair with explicit `_Opt2` variants so a
reviewer reads the optimizer's Compile-up/Eval-down trade-off in one
table (bench/README.md "Binaryen optimize_level trade-off", dated
2026-05-15). Caveat on that table as published: the twenty-term
"pair" currently compiles DIFFERENT expressions — `a < b && b < c &&
…` in the default benches vs `a + b + … == t` in `_Opt2`
(pipeline_bench.cc; register row R58) — so the headline "-52% Eval"
compares unlike workloads. The sanctioned-deviation story is honest
only once the pair is actually paired; until the fix lands, treat the
trade-off table's deltas as indicative, not citable.

## 4. The comparative harness

### 4.1 Corpus and loader

Cells live in `benchmark/eval/corpus/*.yaml` — one file per surface
(arithmetic, comparisons, comprehensions, conversions, index, lists,
logic, long_strings, maps, size, strings, ternary, time). Schema per
cell: `id`, `source`, optional `activation` (name → `{type, value}`),
`expected`, optional `tags` (corpus_loader.h).

`corpus_loader.{h,cc}` is comparator-neutral by construction — its
own `CelValueLiteral` type, zero first-party deps — so the cel-cpp
binary can link it (corpus_loader.h). It hard-fails at startup on:
YAML errors, surface ≠ file basename, malformed ids, duplicate
`(surface, id)` within and across files, unknown types, and
unbound/unused activation variables (heuristic identifier scan,
opt-out tag `skip-source-check`). Cells are returned sorted by
`(surface, id)` for deterministic registration. Activation values are
**scalar-only** today; aggregate literals return a loud
`UnimplementedError` (corpus_loader.cc) — which is why aggregate
operands are constructed in the expression source and reduced to a
scalar (`size(…)`, `int(…)`, `.getSeconds()`), with the documented
consequence that `in_list`/`map_*`/`size_map*` cells time literal
construction + operation (OPERATORS.md).

`CanonicalForm` serialises doubles via shortest-round-trip
`std::to_chars`, mirroring `runtime/cel_convert_double_format.cc`, so
all comparators print byte-identical doubles; `AbbreviateForLabel`
truncates >64-byte payloads identically in both binaries
(corpus_loader.h).

### 4.2 Linkage isolation

cel-cpp's `cel::Value` / `cel::Activation` clash with our first-party
`cel::` aliases under archive-scan order, so every cel-cpp-side bench
is a standalone TU with zero first-party deps (`linkstatic = True` in
benchmark/; the only shared code is the neutral corpus loader)
(in_operator_cel_cpp_bench.cc; benchmark/eval/BUILD.bazel). This
constraint also explains the proto-message corpus exclusion
(OPERATORS.md). The cel-cpp comparator is configured for parity:
qualified type identifiers + heterogeneous equality (runtime),
`max_recursion_depth = 16384` (parser — the 250/1000-term chains
exceed the default 32), cross-numeric comparisons (checker)
(celcpp_bench.cc).

### 4.3 The same-BM-name join

Both binaries register one benchmark per cell with byte-identical
names — `BM_<prefix(surface)>_<id>` via a hand-maintained
`BmPrefixForSurface` table — and stamp a pre-loop `result=… (type)`
label in a byte-identical format (shared truncation helper) so labels
diff mechanically across binaries. `report.sh` joins the two JSON
outputs by name. The contract's weak point: the prefix table and the
corpus-file list are hand-duplicated across the two mains, and the
corpus paths are cwd-relative with no runfiles lookup.

> **Open question (V38):** do corpus paths survive non-repo-root
> invocation, and do the two mains' prefix/file tables agree? Verify
> by running `bazel-bin/benchmark/eval/celwasm_bench
> --benchmark_filter=BM_arith_intAdd2` from repo root AND another
> cwd, and by diffing the two static tables (celwasm_bench.cc vs
> celcpp_bench.cc); a shared header or a pin test would close this.

### 4.4 Skip tags and the coverage ledger

The no-silent-gap rule: a cell a comparator cannot run stays in the
YAML with a reason-bearing tag — `celwasm-skip-<reason>` (cel-cpp
still runs it), `celcpp-skip-<reason>` (celwasm may run it), or both
(documented grid exclusion that runs nowhere). `OPERATORS.md`
(benchmark/eval/corpus/) is the authoritative coverage ledger: every
operator × surface cell is tracked there, including the skipped ones.
Because skipped cells are visible rather than omitted, the corpus has
doubled as a correctness probe — it surfaced the ternary-ident-cond
null bug, the dynamic-mode silent rodata miscompare, and
heterogeneous-equality checker gaps (OPERATORS.md "Findings").

### 4.5 Dynamic registration over codegen

Benchmarks are registered at startup from the parsed YAML
(`RegisterBenchmark` per cell); adding an operator is a YAML edit, not
a C++ edit. The earlier generate-C++-via-genrule design was explicitly
rejected (benchmark/DESIGN.md §5.3).

## 5. Honest-reporting rules — structural requirements

These are requirements on any published table, not stylistic
preferences. The founding admission is on record: the original "AOT
beats interpretation" thesis was wrong for short expressions
(benchmark/DESIGN.md §1.4), and the reporting rules exist so that
kind of finding is printed, not buried.

1. **Headline tables carry wins AND losses per family** — never an
   arithmetic-only or wins-only cut.
2. **A dedicated largest-losses table with named architectural
   causes.** The 2026-06-09 publish names them: constant-aggregate
   rebuild per Eval behind the `map_*`/`in_list` losses; SIMD-less
   byte loops behind string losses; the per-Eval wasm-boundary floor
   behind every short-expression loss
   (doc/implementation-plan/rewrite/m28-bench-results.md §3).
3. **Crossover columns must admit defeat**: when we never win, the
   column reads "n/a (we never win)" — mandated by
   benchmark/DESIGN.md §12.4 and implemented verbatim in report.sh,
   which also prints the ratio direction explicitly.
4. **Suspicious wins carry caveats** (e.g. the 48× regex win's
   cache-vs-no-cache footnote, m28-bench-results.md §2†).
5. **Claims that fail reproduction are downgraded in print** (the
   31× → 17–22× downgrade in m28-bench-results.md §4).
6. **The same discipline applies celwasm-vs-itself**: bench/README's
   arena-vs-proto crossover section names where the arena path is
   3–4× SLOWER.
7. **Skipped cells are visible** (§4.4) — a published grid never
   silently drops a cell a comparator couldn't run.

## 6. Baselines and reproduction

### 6.1 Canonical methodology numbers

All from the 2026-06-09 publish run
(doc/implementation-plan/rewrite/m28-bench-results.md §3, §5, §6);
reproduce the underlying measurements with `benchmark/eval/run.sh`
(dual link-mode + cel-cpp, `-c opt`, min_time 0.5 s; `smoke` arg
drops it to 0.1 s):

- **Per-Eval floor**: 62 ns static / 230 ns dynamic (one wasmtime
  boundary crossing + arena reset + result decode); cel-cpp's
  equivalent entry cost ≈ 45 ns.
- **Per-op slope** (dependent int-add chain): ~1.5–1.9 ns/op static
  vs ~32 ns/op for the cel-cpp tree-walker.
- **Crossover**: ≈ N=10 operations — below it the interpreter's
  cheaper entry wins; above it the slope difference dominates.
- **Noise rule**: treat sub-20% single-run differences as noise
  (observed ~15% run-to-run variance on the same cell, same day).

These are dated historical baselines on the publishing machine, not
machine-independent constants; there is no cross-machine parity story
yet (§8).

### 6.2 How a publish run works today — and its honest limits

`benchmark/eval/run.sh` builds both binaries under `-c opt`, runs
celwasm_bench once per link mode plus celcpp_bench once, and hands
each (mode, cel-cpp) JSON pair to `report.sh`. But `report.sh` is a
4-operator tool: it hardcodes `OPS=(intAdd intMul intSub doubleAdd)`
over the {2,10,50,250,1000} length sweep — 20 cells of 232 — and
stamps a hardcoded "Parity verified for all 20 cells (eyeballed…)"
line into every report regardless of input (report.sh, verified).

**The published full-corpus analysis is currently not reproducible.**
The 13-family geomean / win-loss tables in m28-bench-results.md cannot
come from report.sh, yet that doc cites run.sh as its reproduction
path; the analysis pipeline that produced §1–§5 of the publish is
unrecorded (register row R55). Raw JSONs from the publish run live in
/tmp, uncommitted. The never-shipped layer benchmark/DESIGN.md
promised (parity binary, `report.py` + test, comparator wrappers,
committed `results/`) does not exist (register row R54).

> **Open question (V36):** how were the full-corpus tables produced?
> Run `benchmark/eval/run.sh smoke` and diff report.sh's output shape
> against m28-bench-results.md §1–§5; recover or rewrite the analysis
> pipeline — and commit it — before the next publish. Until then, the
> full-corpus tables are a dated artifact, not a regenerable baseline.

### 6.3 Parity is currently eyeballs

Nothing machine-compares the `result=` labels across binaries or
against the cell's `expected` literal — celwasm_bench never validates
its Eval result against `expected`, so a wrong-value regression times
successfully. The dynamic-mode rodata miscompare was caught by a
human reading labels, not by the harness. Machine parity (label diff
+ expected-value check) is the top item in §8.

## 7. Perf-model facts from the architecture

The embedder-visible performance truths that follow from design
decisions documented elsewhere; benches must state them rather than
let readers infer wrong causes.

- **Per-Eval floor by link mode** (§6.1): every Eval pays one wasm
  boundary crossing + `arena_reset` + result decode; static linking
  cuts the floor ~3.7× vs dynamic (62 vs 230 ns, dated 2026-06-09).
- **`&&` / `||` always evaluate both operands.** They lower through
  the general slot-out call arm — eager evaluation, with 3VL
  absorption inside `cel_and`/`cel_or`. Spec-equivalent because CEL
  is side-effect-free; only `_?_:_` gets genuinely lazy arms
  (BinaryenIf) (expr_lower.cc kCall dispatch ladder;
  notes/codegen-lowering.md §5.2).
- **Constant aggregates rebuild per Eval.** Lists/maps are arena-built
  per evaluation, never rodata (static_memory_builder.cc) — the named
  architectural cause behind the `in_list`/`map_*` family losses, and
  the reason those corpus cells time construction + operation.
- **The arena cliff is transitional.** The notes-era kernel had a
  hard 64 KiB per-Eval arena (~2,700 list elements at 24 B, ~1,350
  map entries at 48 B); the in-flight merge replaces it with a
  chained grow-on-demand arena (4 KiB–1 MiB chunks,
  `runtime/cel_arena.c` `pick_grow_size`) and moves the binding
  constraint for large literals to the compile-time static-region
  gate (`ValidateExprStaticRegion` — see `04-runtime.md` §7; the
  historical 10K-list wasmtime panic was a workspace overrun, not an
  arena bug — the in-runtime allocators are exonerated, every
  `arena_alloc` consumer checks the 0 return). Arena-bound corpus
  cells and the cliff baselines must be re-measured once the merge
  settles. Comprehension accumulators remain pre-sized at the
  prologue with a runtime trap on overflow as the codegen-regression
  tripwire (expr_lower_comprehension.cc).
- **CEL_LOG in fastbuild is a 1.4–5.7× Eval tax** (§3 axis 1) —
  never benchmark a fastbuild binary.
- **The Engine caches the parsed runtime module**, bench-justified at
  ~34× per-Plan (~64× with process sharing); the per-Plan expr-module
  re-parse is the accepted default with a named future cache seam
  (eval/engine.h).
- **Workspace slot reuse is transitional.** The design-rebuild notes
  pinned `SlotAllocator::Release` as a no-op — workspace grew one
  24 B slot per kSelect/kCall/aggregate node, an unbounded-workspace
  concern for large expressions (notes/codegen-memory.md §2.1). A
  slot-reuse allocator (Sethi–Ullman-style release/acquire over the
  layout walks) is landing in an in-flight merge — the working-tree
  `compiler/codegen/slot_allocator.cc` already shows free-list reuse
  with a bump-only debug mode. Any workspace-growth or
  large-expression claim must be re-verified, and large-expression
  benches re-baselined, once that merge settles.

## 8. Future work and known gaps

Two cautionary tales motivate the top items. **R56**: the
duration/timestamp and Any kernel benches in kernel_bench have been
permanently dead behind `#ifdef CELWASM_M7B_SHIPPED` (defined
nowhere) since the features shipped 2026-05-16 — and have name-drifted
meanwhile (they call `cel_ts_year_utc`; the shipped kernel is
`cel_ts_year_utc_at_v`, runtime/cel_time.h), so flipping the guard
likely won't compile. **R59**: the kernel_bench arena-cursor poke
went stale across the WASI migration and silently turned allocating
rows into OOM-path timings (§2). Both rotted invisibly because
manual-tagged bench binaries are never built in any test sweep.

> **Open question (V37):** does flipping `CELWASM_M7B_SHIPPED` even
> compile? `bazel build -c opt //benchmark/kernel:kernel_bench
> --copt=-DCELWASM_M7B_SHIPPED` (expected: failure on the drifted
> kernel names) settles delete-vs-fix.

Priority order:

1. **Bench build-smoke in CI** — at minimum, build every manual
   bench binary in a CI sweep so R56/R59-style bit-rot is caught at
   the commit that causes it.
2. **Machine parity** — replace eyeballed `result=` labels with a
   label diff across binaries plus an expected-value check inside
   celwasm_bench (§6.3); delete report.sh's hardcoded parity line.
3. **Rebuild and commit the full-corpus analysis pipeline** (V36)
   before the next publish; commit results artifacts instead of /tmp.
4. **A perf analogue of the conformance monotonic gate.** Precedent
   exists next door: per-link-mode PASS-count baselines with a
   pre-push monotonic check (conformance/.baseline,
   scripts/check_conformance_monotonic.sh). The perf version would
   pin floor/slope baselines per machine class and fail on
   regression beyond the noise rule.
5. **Machine parity of baselines** — the §6.1 numbers are
   single-machine; record machine specs with every publish, and
   define how baselines transfer (or don't) across machines.
6. **Fix the `_Opt2` pairing** (R58) so the optimize-level trade-off
   table compares like workloads; fix the misnamed
   `BM_Eval_LongArith_10kTerms` (1,000 terms, three contradicting
   comments — R57, mirrored in the cel-cpp sibling).
7. **Recorded P2 debt**: cell-count drift (DESIGN.md/OPERATORS.md say
   229; the corpus is 232); OPERATORS.md claims a CI parser that
   doesn't exist; DESIGN.md §6.4.4's mandatory `purpose:` field is
   unparsed and unused; stale function-name cross-references in
   report.sh; stale future-work rows in bench/README.md; cwd-relative
   corpus paths + the duplicated prefix/file tables (V38);
   benchmark/compiler unbuilt; scalar-only activation values
   constrain corpus design; the m28-bench-results §2†-vs-§6
   internal contradiction.

---
