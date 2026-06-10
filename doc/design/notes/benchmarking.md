# benchmarking — design notes (undefined)

## 1. Verified architecture

### 1.1 The two-directory split

- **`bench/`** — celwasm-vs-itself microbenches that "grew organically"
  (benchmark/README.md:34-37): used for **regression localisation**
  (which layer regressed: kernel vs trampoline vs codegen —
  bench/README.md:252-274).  All Google Benchmark `cc_binary`s,
  `manual`-tagged so `bazel test //...` skips them
  (bench/BUILD.bazel:5-13).
- **`benchmark/`** — the comparative, "adoption-grade" system: a YAML
  corpus drives two linkage-isolated binaries (celwasm vs vendored
  cel-cpp tree-walker) registering identical BM names, joined post-run
  (benchmark/README.md:1-49; benchmark/DESIGN.md §1.2, §5.1).
  DESIGN.md §2 states the non-goal explicitly: benchmark/ is NOT a
  replacement for bench/ (DESIGN.md:94-96).

### 1.2 `bench/` targets and what each measures

Registered in bench/BUILD.bazel (all `tags = ["manual"]`):

| target | boundary measured | evidence |
|---|---|---|
| `kernel_bench` | raw native kernel call — links the native `//runtime:cel_runtime` `cc_library`, **bypasses wasmtime entirely** (bench/README.md:70-74). Operands staged outside the loop; arena reset once per state; allocating kernels rewind the arena cursor per iteration by poking the cursor word at `cel_mem_base()+8` (kernel_bench.cc:66-69, 361-400, 451-464). | kernel_bench.cc:1-30; BUILD:19-29 |
| `pipeline_bench` | public `Compiler::Compile` / `Engine::Plan` / `Instance::Eval`, one bench family per cache boundary: `BM_Compile_*` (fresh compile per iter), `BM_Plan_*` (Program pre-built), `BM_Eval_*` (Compile+Plan outside loop, Eval-only inside). One shared process-static `GlobalEngine()` (pipeline_bench.cc:15-24, 66-73). Per-AST-kind matrix (literal/select/arith/compare/type/list/map/struct/conversion) pipeline_bench.cc:27-37. Plus paired `_Opt2` variants for the optimize-level trade-off table (pipeline_bench.cc:450-527). | BUILD:31-55 |
| `in_operator_bench` | large-list `in` at all three boundaries (Compile/Plan/Eval), LITERAL-source vs Activation-BOUND list flavours; headline 1M-int bound list at first/last/absent positions; 1K 50-byte IAM-permission strings; a long dependent-arith chain. | in_operator_bench.cc:1-39, 255-549 |
| `in_operator_cel_cpp_bench` | the SAME expressions/activations through cel-cpp's tree-walking evaluator. Eval-steady-state only; cel-cpp parse+check+CreateProgram outside the loop (in_operator_cel_cpp_bench.cc:7-11). Pre-loop assertion that cel-cpp returns the expected bool (RunEvalLoop, :226-245). **Standalone TU** — must not link `//eval/...`, `//compiler:...`, `//shared:...`, `//abi/...` (symbol clash with our `cel::Value` aliases; BUILD:83-92, in_operator_cel_cpp_bench.cc:13-22). | BUILD:89-112 |
| `foreign_component_bench` | foreign-component dispatch (`Engine::AddComponent` + canonical-ABI call) vs `AddTypedFunction` host-callback baseline on the same CEL decl; the delta IS the component-call overhead (foreign_component_bench.cc:20-28). Per-BM heap-owned Engine because "AddComponent is per-Plan and Engine state interferes if shared" (:176-183). 256 KiB foreign-side string shape is **disabled** (`cannot leave component instance` trap; :244-253) — only the host-fn baseline of shape B runs. | BUILD:115-153 |
| `program_size_main` | not timing — prints expr-module wasm byte sizes at optimize_level 0 vs 2 plus AST-proto wire size and `sizeof` of public C++ types; feeds the README "Program size" table. | program_size_main.cc:1-6; BUILD:158-179 |

Unregistered material under `bench/`:

- **`bench/cel_pipeline_bench.cc` has NO BUILD target** — the file
  exists (270 lines, per-stage `BM_Compiler_Build` / `BM_Engine_Build`
  / `BM_Compile` / `BM_Plan_Hot` / `BM_Eval` / cold-warm-hot composite
  pipeline benches over 5 scalar-literal inputs) but
  bench/BUILD.bazel declares only the six targets above.  See §2.
- **`bench/foreign_component/`** — throwaway component-model probes
  (arg_cost/, typed_fn/) deliberately **outside the bazel build**,
  driven by an external toolchain (cargo/wasm-tools/wac); kept as the
  empirical backing for m23 boundary-cost numbers, to be deleted when
  superseded (bench/foreign_component/README.md:1-24, REPRODUCE.md).

### 1.3 `benchmark/` — the comparative system as shipped

Data flow: `corpus/*.yaml` → `corpus_loader` → each of two bench mains
registers one Google Benchmark per cell → JSON outputs →
`report.sh` joins by BM name.

- **Corpus**: 13 surface YAMLs, **232 cells** (verified by `grep -c
  "  - id:"`; matches m28-bench-results.md:5 "232 cells").  Schema per
  cell: `id`, `source`, optional `activation` (map of name → `{type,
  value}`), `expected`, optional `tags` (corpus_loader.h:97-104).
- **Loader** (`corpus_loader.{h,cc}` + test): comparator-neutral
  `CelValueLiteral` so the cel-cpp binary can link it with zero
  first-party deps (corpus_loader.h:6-12; BUILD:7-18).  Hard-fails at
  startup on: YAML error, surface≠file-basename, bad id (empty /
  whitespace / `/`), duplicate `(surface,id)` within and across files,
  unknown type, unbound/unused activation variables (heuristic
  identifier scan, opt-out tag `skip-source-check`)
  (corpus_loader.cc:199-347, 373-401).  Cells returned sorted by
  `(surface,id)` for deterministic registration (:394-399).
  **Scalar-only activation values today**; list/map literals return
  `UnimplementedError` loudly (corpus_loader.cc:96-101; header doc
  :20-26).  `CanonicalForm` serialises doubles via `std::to_chars`
  shortest-round-trip, mirroring `runtime/cel_convert_double_format.cc`
  so all comparators print byte-identical doubles
  (corpus_loader.h:127-137, corpus_loader.cc:354-363).
  `AbbreviateForLabel` truncates >64-byte payloads identically in both
  binaries (corpus_loader.h:139-145).
- **`celwasm_bench`**: per cell builds Compiler (declaring each
  activation var's type), `Compile` with `optimize_level=2` +
  `BenchLinkMode()`, `Engine::Plan` — all inside the registration
  lambda but **before** the timed loop; only `Instance::Eval(act)` is
  timed (celwasm_bench.cc:87-96, 254-298).  One pre-loop Eval stamps a
  `result=… (type)` label for mechanical cross-binary diffing
  (:166-184, 254-259).  BM name = `BM_<prefix(surface)>_<id>` via the
  hand-maintained `BmPrefixForSurface` table (:189-212).  Cells tagged
  `celwasm-skip-*` are not registered (:271-277).  `--link_mode=
  dynamic|static` is consumed from argv before
  `benchmark::Initialize`; **default kDynamic**, explicitly chosen for
  comparability with historical baselines even though
  `CompilerOptions::link_mode` defaults to `kStatic`
  (celwasm_bench.cc:47-58; compiler/compiler.h:145).  Engine is a
  process-static singleton; `CELWASM_BENCH_PERFMAP=1` enables
  wasmtime's perf-map for profiler symbolication (:70-83).  Two
  hand-coded extra cells (`BM_arith_intAdd_AbcAbcShape_{VarsToday,
  LitToday}`) isolate activation-marshal cost on an identical
  call-graph shape; YAML can't express the pairing yet (:301-349,
  386-390).
- **`celcpp_bench`**: same corpus, same BM names, same result-label
  format; `linkstatic = True`, zero first-party deps except the
  neutral loader (BUILD:92-136).  cel-cpp configured with
  `enable_qualified_type_identifiers`, `enable_heterogeneous_equality`
  (runtime), `max_recursion_depth=16384` (parser — 250/1000-term
  chains exceed the default 32), and
  `enable_cross_numeric_comparisons` (checker)
  (celcpp_bench.cc:64-101).  Skips `celcpp-skip-*` tagged cells
  (:259-265).  No `--link_mode` flag.
- **`run.sh`**: builds both under `-c opt`, runs celwasm_bench twice
  (dynamic then static), celcpp once, hands the two pairs to
  `report.sh` (run.sh:24-56).  `smoke` arg drops min_time 0.5s→0.1s.
- **`report.sh`**: bash+jq+awk.  Per-operator headline = linear
  regression T(N)=setup+N·per_op over the {2,10,50,250,1000}
  length-sweep, slope/intercept/crossover columns (report.sh:73-158)
  — but **only for 4 hardcoded operators** (`intAdd intMul intSub
  doubleAdd`, :26-27) plus a per-cell detail table for the same 20
  arith cells.  The full-corpus family-geomean tables in
  m28-bench-results.md are NOT producible by report.sh (see §2/§3).
- **Skip-tag regime** (the no-silent-gap rule): `celwasm-skip-<reason>`
  = celcpp still runs it; `celcpp-skip-<reason>` = celwasm may run it;
  both = documented grid exclusion that runs nowhere
  (OPERATORS.md:26-32, 300-315).  OPERATORS.md is the authoritative
  coverage ledger (DESIGN.md §6.4.6) and records three correctness
  findings the corpus itself surfaced (ternary-ident-cond null bug,
  dynamic-mode silent rodata miscompare, het-eq checker gaps;
  OPERATORS.md:317-340).
- **`benchmark/compiler/`** = TODO.md only; Compile/Plan comparative
  benches deferred with the full would-be matrix specified
  (compiler/TODO.md).

### 1.4 The production-config rule

Three orthogonal axes (bench/README.md:31-60):

1. **Bazel config**: always `bazel run -c opt`; debug ~10× off
   (bench/BUILD.bazel:11-13).  `-c opt` also activates
   `CEL_LOG_DISABLED` via runtime/BUILD.bazel's `opt_mode`
   config_setting — in fastbuild every public kernel pays a
   wasm→host fprintf trampoline, worth 1.4–5.7× on Eval rows
   (bench/README.md:45-54; POST_MIGRATION_BENCH.md:78-98).
2. **Runtime flags**: `-O3 -flto` on both native and wasm32 runtime
   builds; LTO is load-bearing post the cel_runtime.c per-topic split
   (cross-TU inlining) (bench/README.md:35-44).
3. **Binaryen `optimize_level=2`** on every expr-module compile, via
   the `kBenchOptimizeLevel = 2` constant + a `CompileOrDie` that
   always sets it (in_operator_bench.cc:142-153,
   celwasm_bench.cc:85-96, foreign_component_bench.cc:68,159-174).
   `pipeline_bench` is the sanctioned deviation: its default-config
   benches pair with explicit `_Opt2` variants so a reviewer reads
   the optimization delta in one table (pipeline_bench.cc:450-457;
   CLAUDE.md "Benchmark configuration").

### 1.5 The honest two-sided narrative rule

m28-bench-results.md is the exemplar: headline table carries
**wins–losses per family** (§1), a dedicated "largest losses" table
(§3, "the honest column") with named architectural causes
(constant-aggregate rebuild per Eval; SIMD-less byte loops; the 62 ns
wasm-boundary floor), the cache-vs-no-cache caveat on the 48×
regex win (§2†), and a reproduction-verdict section that **downgrades
a previously claimed 31× to 17–22×** (§4).  benchmark/DESIGN.md §1.4
opens by recording that the original "AOT beats interpretation"
thesis was *wrong for short expressions*; §12.4 mandates the
crossover column read "n/a (we never win)" when true; report.sh
implements exactly that string (report.sh:92-103) and prints the
ratio direction explicitly (:187).  bench/README.md applies the same
discipline to celwasm-vs-itself (arena-vs-proto crossover where the
arena path is 3-4× SLOWER, :194-202).

## 2. Doc-vs-code discrepancies

1. **P1 — `//bench:cel_pipeline_bench` doesn't exist as a target.**
   POST_MIGRATION_BENCH.md:5-11,124-127 gives `bazel run -c opt
   //bench:cel_pipeline_bench` as the reproduction command and
   per-component-test-coverage.md:94 lists it as a manual gate target;
   bench/BUILD.bazel declares no such target (only kernel_bench,
   pipeline_bench, in_operator_*, foreign_component_bench,
   program_size_main).  `bench/cel_pipeline_bench.cc` is orphaned
   source.
2. **P1 — benchmark/README.md + DESIGN.md describe a system that
   never shipped in that shape.**  Layout (README:53-68) and DESIGN
   §§5.2, 8, 11, 12, 13 name `parity_check_main.cc`, `report.py` +
   `report_test.py`, `profile.sh`, `comparators/{celwasm,celcpp}_
   wrapper.{h,cc}`, `eval/README.md`, `eval/HARNESS.md`,
   `results/` (committed CSVs).  None exist (verified `ls
   benchmark/eval/`, repo-wide grep).  As-shipped: bench logic inline
   in the two `_main`-style `.cc`s, `report.sh` (bash/jq/awk) instead
   of report.py, no parity binary (parity = eyeballing `result=`
   labels), no committed results dir (raw JSONs in /tmp per
   m28-bench-results.md:134), no per-comparator wrapper layer.
3. **P1 — report.sh covers 4 operators / 20 cells; the corpus is 232
   cells.**  report.sh:26-27 hardcodes `OPS=(intAdd intMul intSub
   doubleAdd)`; both detail tables iterate only those.  The published
   full-corpus tables in m28-bench-results.md (13 families, geomeans,
   win/loss counts) cannot come from report.sh, yet that doc says
   "Reproduce with benchmark/eval/run.sh" (:14) and run.sh ends in
   report.sh.  Also report.sh:108-109 hardcodes "Parity verified for
   all 20 cells (eyeballed…)" into every report regardless of input.
4. **P1 — M7B/M7A kernel benches are permanently dead although both
   milestones shipped 2026-05-16.**  kernel_bench.cc gates the
   duration/timestamp benches behind `#ifdef CELWASM_M7B_SHIPPED`
   (:558,593-687) and the Any benches behind `kM7aShipped = false`
   (:712); neither symbol is defined/flipped anywhere
   (repo grep), but m7b-duration-timestamp.md:3 and m7a-any.md:3 are
   both "Status: shipped 2026-05-16", and bench/README.md:285-287,
   331-333 promises they "turn on row-by-row as M7B.B/M7B.C land" /
   "turn on when M7-A.A/B/C ship".  Worse, the guarded code calls
   `cel_ts_year_utc` / `cel_ts_day_of_week_utc` (:648,670) while the
   shipped runtime exports `cel_ts_year_utc_at_v`
   (runtime/cel_time.h:109) — flipping the guard likely won't compile.
5. **P1 — `BM_Eval_LongArith_10kTerms` is 1000 terms, with three
   mutually contradicting comments.**  Function-level comment says
   "50-term quadratic-form polynomial … 99 operations"
   (in_operator_bench.cc:104-110); the body comment says "10 000
   multiplications + 9 999 additions ≈ 20 000 binary ops" (:112-118);
   the code is `constexpr int kTerms = 1000` (:119).  Bench name says
   10k.  The cel-cpp sibling repeats the "50-term" header and the
   `_10kTerms` name with the same kTerms=1000
   (in_operator_cel_cpp_bench.cc:422-446).
6. **P1 — the `_Opt2` "paired" twenty-term benches compile a different
   expression than their non-Opt2 counterparts.**  The pairing comment
   (pipeline_bench.cc:451-453) and the README trade-off table
   (bench/README.md:234-245, rows `BM_Compile_TwentyTermCompare`
   default vs Opt2) imply same-expression columns; non-Opt2 builds
   `a < b && b < c && …` (pipeline_bench.cc:163-169,288-294) while
   `_Opt2` builds `a + b + … + s == t` (:475-481,508-515).  The
   "-52% Eval" headline compares unlike workloads.
7. **P2 — cell-count drift**: DESIGN.md §6.4.6 (:487-489) and
   OPERATORS.md:17 say 229 cells; actual corpus = 232
   (m28-bench-results.md:5 agrees with 232).
8. **P2 — OPERATORS.md:386-390 claims a CI check** ("the OPERATORS.md
   parser is part of the benchmark harness — a CI check fails if an
   operator is shipped but not yet tracked").  No such parser or
   check exists (grep scripts/, .github/).
9. **P2 — DESIGN.md §6.4.4 mandates a `purpose:` field on every cell**
   ("If a cell can't articulate its purpose … drop it"); zero corpus
   cells carry one and the loader doesn't parse it (grep `purpose:`
   over corpus/*.yaml = 0 across all 13 files; corpus_loader.cc
   ParseCellNode reads only id/source/activation/expected/tags).
10. **P2 — bench default link mode ≠ production default.**
    `BenchLinkMode()` defaults kDynamic (celwasm_bench.cc:47-58) while
    `CompilerOptions::link_mode` defaults kStatic
    (compiler/compiler.h:145).  Documented in the comment (historical
    comparability; run.sh runs both), but a bare `bazel run
    …:celwasm_bench` silently measures the non-default mode.
11. **P2 — internal contradiction in m28-bench-results.md**: §2†
    (:57-67) says the 48× regex cause is "confirmed (2026-06-09)";
    caveat §6 (:149) still says "pending cause verification (§2 †)".
12. **P2 — stale cross-references**: report.sh:36 cites
    `celwasm_bench.cc::MakeBmName` and :49-50 `MakeChainSource` —
    neither function exists (it's `BmPrefixForSurface` + StrCat;
    chains live in the YAML now).  bench/README.md:359-370 still lists
    "Comprehension benches … once M11 ships" as future work although
    comprehension benches now exist (in benchmark/eval corpus,
    compr.*), and pipeline_bench has no comprehension row.
    benchmark/compiler/TODO.md cross-links `../eval/HARNESS.md`
    (nonexistent).
13. **P2 — pipeline_bench.cc:423-427 comment** says "An empty
    Activation is legal — declared variables only need to be bound if
    the body reads them", then the code binds a dummy `c` anyway.

## 3. Validation items

1. **Q: Is `//bench:cel_pipeline_bench` really absent from the build
   graph (vs defined elsewhere)?**  How: `bazel query 'kind(cc_binary,
   //bench:all)'` — expect 6 targets, no cel_pipeline_bench.  Settles
   whether to register it or delete the file + fix the two docs.
2. **Q: Do the documented runtime caps still hold?**  (a) wasmtime
   trap at N=10000 literal int-list Eval (in_operator_bench.cc:311-314)
   — raise `BM_Eval_In_IntList_Literal_WorstCase` to `->Arg(10000)`
   and run `bazel run -c opt //bench:in_operator_bench --
   --benchmark_filter=BM_Eval_In_IntList_Literal`; (b) arena OOM at
   10k bound strings (:516-519) — same with
   `BM_Eval_In_IamPermissions_Bound_Last` at 10000.
3. **Q: Does flipping `CELWASM_M7B_SHIPPED` compile against the
   shipped kernel names?**  How: `bazel build -c opt
   //bench:kernel_bench --copt=-DCELWASM_M7B_SHIPPED` — expect
   failure on `cel_ts_year_utc` vs `cel_ts_year_utc_at_v`
   (runtime/cel_time.h:109).
4. **Q: How were the m28 full-corpus geomean/win-loss tables actually
   produced?**  How: run `benchmark/eval/run.sh smoke` and diff its
   report.sh output against the m28-bench-results.md table shapes; if
   (as the code says) report.sh only emits 4-operator arith tables,
   the analysis pipeline for §1-§5 of that doc is unrecorded and
   needs to be recovered or rewritten before the next publish.
5. **Q: Do the relative corpus paths resolve under both `bazel run`
   and direct `bazel-bin/...` invocation from repo root?**  How: run
   `bazel-bin/benchmark/eval/celwasm_bench --benchmark_filter=
   BM_arith_intAdd2` from repo root AND from another cwd; the
   `kCorpusFiles` paths (celwasm_bench.cc:355-369) are cwd-relative
   with no runfiles lookup, so the second should fail with the
   LoadCorpus CHECK naming `$PWD`.
6. **Q: Is result-label parity ever machine-checked?**  How: grep
   run.sh/report.sh for any label comparison (none found); if
   confirmed manual-only, the DESIGN §11 parity check remains
   unimplemented and OPERATORS.md's covered-criterion 2 ("paired
   `result=` labels are byte-identical", :43-48) is enforced by eyeball.
7. **Q: Do both bench mains' `BmPrefixForSurface` tables and
   `kCorpusFiles` lists actually agree?**  How: a trivial diff of the
   two static tables (celwasm_bench.cc:189-212,355-369 vs
   celcpp_bench.cc:222-245,305-319) — today they match by inspection,
   but nothing pins them; a shared header or a test would.

## 4. Test coverage observations

- **Pinned well**: `corpus_loader_test.cc` covers every DESIGN §6.3
  validation rule one-for-one (duplicate ids in/across files, surface/
  basename mismatch, id charset, unbound/extra activation vars,
  `skip-source-check` opt-out, uint-literal-suffix non-variable,
  unknown type, missing path), plus `CanonicalForm` per kind
  (shortest-round-trip doubles included) and `AbbreviateForLabel`
  boundaries, plus a `LoadsAllCommittedSurfaces` smoke over all 13
  YAMLs with a ≥150-cell lower bound (corpus_loader_test.cc:56-485).
  This is the only conventional test in either tree; it runs in
  `bazel test` (not manual-tagged).
- **Self-checking inside benches**: every Eval is `ABSL_CHECK_OK`;
  in_operator_cel_cpp_bench asserts the expected boolean pre-loop
  (RunEvalLoop, :226-245); corpus benches stamp `result=` labels.
- **Gaps**: (a) nothing machine-compares the result labels across
  binaries or against `expected` — celwasm_bench never checks its
  Eval result against the cell's `expected` literal, so a wrong-value
  regression times successfully (the dynamic-mode rodata miscompare
  in OPERATORS.md Findings #2 was caught by a human, not the
  harness); (b) the BM-name contract (two hand-duplicated prefix
  tables + file lists) is untested; (c) skip tags are trusted — a
  stale `celwasm-skip-*` after its blocker is fixed is silent;
  (d) the bench binaries themselves never build in CI/test runs
  (manual tag), so bit-rot like the M7B kernel-name drift (§2.4) is
  invisible until someone runs them; (e) report.sh has no test
  (DESIGN promised report_test.py).

## 5. Design decisions worth preserving

1. **bench/ vs benchmark/ is a localisation-vs-publication split.**
   bench/ answers "which of OUR layers regressed" (kernel µbench delta
   vs Eval delta attributes a regression to kernel vs trampoline —
   bench/README.md:252-274); benchmark/ answers "what should an
   embedder expect vs cel-cpp" with apples-to-apples methodology.
   Don't merge them (DESIGN.md §2 first bullet).
2. **Linkage isolation of the cel-cpp comparator.**  cel-cpp's
   `cel::Value`/`cel::Activation` clash with first-party `cel::`
   aliases under archive-scan order; every cel-cpp-side bench is a
   standalone TU with zero first-party deps (+`linkstatic` in
   benchmark/) and rebuilds its corpora inline; the only shared code
   is the deliberately backend-neutral corpus_loader
   (in_operator_cel_cpp_bench.cc:13-22; benchmark/eval/BUILD:92-97;
   corpus_loader.h:6-12).  This constraint also explains the
   proto-message corpus exclusion (OPERATORS.md:278-296).
3. **Same-BM-name join contract.**  The two binaries register
   byte-identical BM names per cell; the report joins on name; the
   result-label format is kept byte-identical (including the shared
   64-byte truncation helper) so labels diff mechanically
   (celwasm_bench.cc:8-19,166-184; celcpp_bench.cc:8-11,128-153).
4. **Timed region = Eval only; everything else pre-staged.**
   Compile + Plan in the registration lambda, before the loop;
   one shared GlobalEngine (construction amortised in real use) —
   except foreign_component_bench, where each BM owns an Engine
   because AddComponent state interferes (foreign_component_bench.cc:
   176-183).  The bench taxonomy mirrors the three real host caches:
   Compiler / Program / Instance (bench/README.md:204-223).
5. **Production-config rule with one explicit deviation.**
   `-c opt` (which also kills CEL_LOG), runtime `-O3 -flto`, expr
   modules at `kBenchOptimizeLevel = 2`; pipeline_bench's
   default-vs-`_Opt2` pairs are the sanctioned exception that makes
   the optimizer's Compile-up/Eval-down trade-off readable
   (bench/README.md:31-60,225-250).  Bench numbers from any other
   config are not baselines.
6. **Dual link-mode runs.**  One binary, `--link_mode` consumed before
   Google Benchmark sees argv, run.sh invokes once per mode and joins
   each against the same cel-cpp JSON — static vs dynamic is a
   published axis, not a build fork (celwasm_bench.cc:404-427;
   run.sh:29-56).
7. **Honest two-sided reporting is structural, not stylistic.**
   Losses get their own table with architectural causes; crossover
   columns admit "n/a (we never win)"; suspicious wins carry caveats
   (regex cache-vs-no-cache); claims that fail reproduction are
   downgraded in print (m28-bench-results.md §§1-4; DESIGN.md §1.4,
   §4 adversarial framing, §12.4).
8. **Skip tags over omission.**  A cell a comparator can't run stays
   in the YAML with a reason-bearing tag and an OPERATORS.md row; the
   corpus has doubled as a correctness probe precisely because skipped
   cells are visible (the ternary-null and silent-rodata-miscompare
   bugs were corpus findings — OPERATORS.md:317-340).
9. **Scalar-only activations, in-source aggregates, scalar reduction.**
   Aggregate/time operands are constructed in the expression and
   reduced (`size(…)`, `int(…)`, `.getSeconds()`) so both comparators
   time identical work and the expected-value schema stays scalar —
   with the explicit consequence (documented as a caveat) that
   `in_list`/`map_*`/`size_map*` cells time literal construction +
   operation (OPERATORS.md:34-41; m28-bench-results.md §6).
10. **Dynamic registration over codegen.**  RegisterBenchmark-per-cell
    from YAML parsed at startup; adding an operator is a YAML edit, not
    a C++ edit; the earlier gen.py/genrule design was explicitly
    rejected (DESIGN.md §5.3; OPERATORS.md:9-15).
11. **Methodology numbers worth carrying forward**: 62 ns static /
    230 ns dynamic per-Eval floor vs ~45 ns cel-cpp; ~1.5-1.9 ns/op
    static slope vs ~32 ns cel-cpp; crossover ≈ N=10 ops; treat
    sub-20% single-run differences as noise (m28-bench-results.md §3,
    §5, §6).
