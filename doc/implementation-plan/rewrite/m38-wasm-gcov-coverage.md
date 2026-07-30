# m38 — wasm-side gcov coverage via the eval host

Status: shipped 2026-07-27.  Core implemented + smoke-verified, then
the full measurement run completed the same day: engine-level
`CollectWasmCoverage` API, per-workload collection tooling
(`scripts/coverage/collect_wasm_gcov.sh` + `wasm_gcov_report.py`,
§4.1), and headline numbers 87.3% line / 95.1% function for
`runtime/*` in the shipping wasm across 38 workloads (audit report
§2.3).  §6 records the as-shipped deltas.

## 1. Problem

All coverage numbers for `runtime/*.c` are measured on the **native** build
that the `runtime/*_test.cc` suites link (see
`reviews/2026-07-27-test-inventory-and-coverage.md` §2). The wasm copy —
the artifact that actually ships — is invisible to gcov: e2e and
conformance exercise it through wasmtime, but nothing records which lines
ran. The audit found whole families (`cel_time.c`'s `*_with_tz` accessors,
`cel_runtime.c`'s comprehension helpers) with zero native hits that are
*believed* covered via the wasm path; without wasm-side coverage that
belief is unverifiable.

## 2. Probe record (2026-07-27, all empirical)

Building `//runtime:cel_runtime_wasm` with
`--collect_code_coverage --instrumentation_filter='^//runtime[/:]'`:

  - Links successfully; the undefined profile-runtime symbols become
    **function imports on module `env`** (wasi-sdk 25 ships no
    `libclang_rt.profile` for wasm32, only `builtins` — checked
    `lib/clang/19/lib/wasi/`).
  - Exact import surface (parsed from the binary's import section):
    `llvm_gcda_start_file(i32 filename_ptr, i32 version, i32 checksum)`,
    `llvm_gcda_emit_function(i32 ident, i32 func_checksum, i32 cfg_checksum)`,
    `llvm_gcda_emit_arcs(i32 num_counters, i32 counters_ptr)`,
    `llvm_gcda_summary_info()`, `llvm_gcda_end_file()`,
    `llvm_gcov_init(i32 writeout_fn, i32 reset_fn)` — the last two args are
    **function-table indices**, registered per-TU by static ctors (which the
    engine already runs — the `cctz_doubles` forcing function).
  - `.gcno` notes files ARE emitted per TU, even with `-flto`, at
    `bazel-out/<wasm-config>/bin/runtime/_objs/cel_runtime_wasm.bin/<tu>.gcno`.
  - The function table is **not** exported (`tables=[]` in the export
    section), so host-invoked write-out needs `-Wl,--export-table`.
  - The runtime exports its memory as `memory` (shared); the host already
    holds a stable base pointer (`InstanceImpl::memory`).

Reference for the gcda wire protocol: compiler-rt `GCDAProfiling.c` at
`llvmorg-19.1.5` (matches wasi-sdk 25's LLVM). Header = magic `"gcda"` +
version word (verbatim from the call) + stamp; function record = tag
`0x01000000`, len 3 (gcov ≥ 4.7); arcs record = tag `0x01a10000`,
len `2*n`, u64 counters, **merged by summing against an existing file's
positionally-aligned record**; object summary tag `0xa1000000` (gcov ≥ 9).

## 3. Design

Host-side implementation of the six `env::llvm_*` imports — no compiler-rt
port, no WASI filesystem involvement:

  - **`//runtime:instrument_wasm`** (`bool_flag`, default False). When set,
    `cel_runtime_wasm.bin` adds `-fprofile-arcs -ftest-coverage` to copts
    and `-Wl,--export-table` + the profile-import allowance to linkopts.
    The normal artifact is byte-identical to today when the flag is off.
  - **`eval/internal/wasm_gcov.{h,cc}`** (`//:internal`):
    - `WasmGcovSink` — the gcda writer. Mirrors GCDAProfiling.c semantics
      (create-or-merge, positional arc summing, object summary) but
      in-memory: old file slurped on StartFile, new bytes buffered, one
      write on EndFile. Output path = `$CELWASM_WASM_GCOV_DIR/<basename>`
      (runtime TU basenames are unique).
    - `WasmGcovEnv` — per-Instance state: the sink, the registered
      write-out table indices, and the borrowed memory base/size.
    - `RegisterWasmGcovImports(linker, env)` — defines the six imports.
      Always registered (harmless when the module isn't instrumented);
      callbacks no-op when no collection destination is configured.
    - `DumpWasmGcov(context, helpers_instance, env)` — resolves the
      exported `__indirect_function_table`, `table.get`s each registered
      write-out index, and calls it; the guest then drives the
      `llvm_gcda_*` imports against the sink.
  - **Wiring**: `InitLinker` registers the imports; `InstanceImpl` gains a
    `std::unique_ptr<WasmGcovEnv>`; the `InstanceImpl` dtor calls
    `DumpWasmGcov` before tearing down the linker/store (counters live in
    guest memory, so the dump must precede store deletion).
  - **Engine-level API** (added at measurement time):
    `Engine::Builder::CollectWasmCoverage(output_dir)` fixes the
    collection destination at `Build()` — explicit dir wins, else the
    `CELWASM_WASM_GCOV_DIR` env var, else disabled.  Every Instance the
    engine Plans dumps into that dir on destruction.  Behavior pinned
    by `engine_test.cc` `EngineWasmCoverageTest` (config-aware: inert
    against the normal artifact, collecting against an instrumented
    one).

Scope: **dynamic link mode** is the measured configuration (the runtime is
a separate module there). Static mode merges the identical C code; the
imports are registered either way, so an instrumented static program would
also collect, but the measurement workflow below only exercises dynamic.

## 4. Measurement workflow (verified 2026-07-27)

One subtlety, learned empirically: a plain `--//runtime:instrument_wasm`
build emits `.gcno` only as an **undeclared side effect**, which the
sandbox discards. Combining the flag with bazel's coverage flags makes
the `.gcno` declared outputs that land in `bazel-out`; the duplicate
instrumentation flags are harmless.

```bash
# Build (the flag adds --export-table + import allowance; the coverage
# flags make bazel keep the .gcno):
bazel build --//runtime:instrument_wasm --collect_code_coverage \
    '--instrumentation_filter=^//runtime[/:]' \
    //e2e:<dynamic targets> //conformance:run_conformance

# Run each workload binary DIRECTLY (not `bazel test` — the sandbox
# would eat the output dir), sequentially (the sink has no file lock):
export CELWASM_WASM_GCOV_DIR=/tmp/wasm-gcov
./bazel-bin/e2e/<each dynamic e2e binary>
./bazel-bin/conformance/run_conformance   # dynamic link mode

# Pair the collected .gcda with the notes files.  The wasm objs dir is
# the ST-suffixed config, e.g.:
#   bazel-out/k8-fastbuild-ST-*/bin/runtime/_objs/cel_runtime_wasm.bin/
cp "$(dirname "$(find "$(bazel info output_path)" -path '*cel_runtime_wasm.bin*' -name 'cel_time.gcno' | head -1)")"/*.gcno /tmp/wasm-gcov/

cd /tmp/wasm-gcov && llvm-cov gcov -n *.gcda   # percentages; drop -n for .gcov annotations
```

The gcda version emitted by clang 19 here is `"408*"` (gcov 4.8 wire
format) — readable by llvm-cov 18+. Smoke-verified end to end: 2 cases
of `mvp_concat_test_dynamic` (since folded into `operators_test.cc`)
produced 15 .gcda files, decoded cleanly, with plausible per-file
percentages (`cel_string_ops.c` 17%, `cel_arena.c` 63%,
`cel_memory.c` 40%).

### 4.1 Productionized workflow (the full measurement run, 2026-07-27)

The manual recipe above is superseded by two committed tools:

  - **`scripts/coverage/collect_wasm_gcov.sh <out_root> <bin>...`** —
    runs each workload binary sequentially with its own
    `CELWASM_WASM_GCOV_DIR` (per-workload attribution), pairs the
    .gcda with the build's .gcno, and decodes twice (`llvm-cov gcov
    -n -b -f` human summary; `-i` machine-parsable intermediate
    records).
  - **`scripts/coverage/wasm_gcov_report.py --cov-root <out_root>`** —
    joins the per-workload records with `e2e/test_taxonomy.json` into
    `report.json` (function×workload hit matrix + gap/redundancy
    pivots) and `report.html` (clickable explorer: overview, function
    audit, workload contribution, annotated source).

Two .gcno-pairing traps, both learned the hard way and now encoded in
the collect script:

  1. **The runtime wasm links TUs from several targets.**  The
    extension TUs (`cel_base64_ext`, `cel_math_ext`, the
    `cel_string_ext_*` splits, `cel_time_parse`, `cel_optional`,
    `cel_matches`) compile in their own `_objs/<lib>/` dirs — copying
    only `_objs/cel_runtime_wasm.bin/` silently drops them from the
    report.  Gather .gcno from every `bin/runtime/_objs/` dir.
  2. **Basename collisions must resolve toward the linked objects,
    with `cp -f`.**  The base TUs compile twice (the `cel_runtime`
    cc_library AND the `.bin`); the .gcda stamps match the `.bin`
    objects, so its notes must be copied last — and because bazel
    outputs are mode `r-x`, a plain `cp` over an earlier copy fails
    `Permission denied` and silently leaves the WRONG notes in place
    (surfacing later as `file checksums do not match`).

Conformance is collected via
`CELWASM_WASM_GCOV_DIR=<dir> bazel run --//runtime:instrument_wasm
--collect_code_coverage '--instrumentation_filter=^//runtime[/:]'
//conformance:run_conformance -- --link_mode=dynamic`; the eval manual
suites (`engine_test_dynamic` needs its runfiles, so `bazel run` it the
same way) join the sweep as workloads.

Results of the full run (38 workloads: 34 dynamic e2e + engine /
instance / memory_grow_stability + conformance) are recorded in
`reviews/2026-07-27-test-inventory-and-coverage.md` §2.3: **87.3% line
/ 95.1% function** coverage of `runtime/*` in the shipping wasm, 27
zero-hit functions all classified (native-test-only, never-emitted
dispatch arms, defense-in-depth).

## 5. Testing

`eval/internal/wasm_gcov_test.cc` (target `//eval:wasm_gcov_test`,
manual-tagged like its eval/internal siblings) covers both halves:

  - Sink: header bytes, function/arcs/summary record layout (both the
    ≥4.7 and pre-4.7 function-record shapes), create-then-merge summing,
    run-count merge, shape-mismatch fallback, missing-dir creation,
    basename flattening, no-op paths.
  - Glue, hermetically: a synthetic WAT module (via `wasmtime_wat2wasm`)
    that imports the six `env::llvm_*` functions and exports a funcref
    table — asserting registration is inert for non-instrumented
    modules, `llvm_gcov_init` capture, table-driven dump producing a
    byte-checked .gcda, the disabled-sink no-op, and the
    missing-table error.

All 13 cases pass.

## 6. Handoff status (2026-07-27)

Done and verified in this branch:

  - `eval/internal/wasm_gcov.{h,cc}` + `wasm_gcov_test.cc` (13/13 pass).
  - `//runtime:instrument_wasm` flag; flag-built wasm verified to carry
    the six imports AND the exported `__indirect_function_table`.
  - Engine wiring: `InitLinker` registers the imports unconditionally;
    `CacheRuntimeMemory` hands the collector the stable memory view;
    `InstanceImpl` owns the env and its dtor dumps before store
    teardown.
  - End-to-end smoke: instrumented `mvp_concat_test_dynamic` run with
    `CELWASM_WASM_GCOV_DIR` set → 15 .gcda files, decoded by
    `llvm-cov gcov` against the build's .gcno (see §4 numbers).

Remaining-work status (updated 2026-07-27, measurement session):

  1. **Regression pass** — DONE for the touched surfaces
     (`wasm_gcov_test`, `engine_test_dynamic`, the six e2e suites that
     gained gap cases, `e2e_limits_dynamic`, `cel_cpp_oracle_test`, all
     green un-flagged); the full `$PROJ` sweep + conformance gate run
     at this session's commit gate.
  2. **`scripts/lint.sh --branch`** — at the commit gate.
  3. **The full measurement run** — DONE; see §4.1 and the audit
     report §2.3 (87.3% line / 95.1% function across 38 workloads,
     per-workload attribution, all 27 zero-hit functions classified).
     The zero-native-hit families the audit flagged are CONFIRMED
     wasm-covered.  An earlier parallel measurement pass on this
     branch (before the gap-test tranche) recorded 84.5% wasm-side /
     93.5% native∪wasm over 37 binaries + the 2,516-row conformance
     corpus; its operational notes still apply: three suites
     (`engine_test_dynamic`, both plugin tests) load fixtures via
     runfiles and need `TEST_SRCDIR=$PWD/<bin>.runfiles
     TEST_WORKSPACE=_main` when run outside `bazel test`; `llvm-cov
     gcov` must run with the repo's `runtime/` resolvable from the cwd
     (symlink suffices) or it emits header-only .gcov files;
     `scripts/coverage/gcov_to_lcov.py` folds the output for
     `lcov_merge.py`/`lcov_report.py`.
  4. Closeout: testing-checklist row ticked; static-link measurement
     deliberately NOT run (imports are registered either way; the
     static merge embeds the same C code, so dynamic-mode numbers are
     representative — revisit only if a static-only codepath appears).

Known limitations (by design, documented here rather than hedged):
collection is single-threaded, unlocked, dynamic-mode-scoped; the
`.gcno` pairing requires the combined-flags build in §4; the sink bumps
the run count once per file per dump (deviation from compiler-rt's
once-per-process quirk, noted in the .cc).

## 7. Session context for pickup

> **2026-07-27 (second session) — current state.  Read §7.0 FIRST; the
> subsections after it are the original session's context and remain
> valid background.**

### 7.0 Live state + the report-regeneration runbook

> **State refresh (2026-07-28 ~01:30):** branch pushed through
> `9ea7219`; full sweep #1 green (gates incl. conformance both
> modes at stage 1, full native + 39-workload wasm re-measure).
> **GOAL (user-stated, supersedes all earlier bars): e2e
> goal-metric = 100%** (waypoints 90/95) — every product-scope line
> either e2e-covered or verdict-classified with evidence; endgame is
> ledger closure enforced as a zero-unclassified-gap check.
> Trajectory: 74.07 → 75.57 (batch 1) → 78.52 (`c8e759d`) →
> 78.85 (`9ea7219`, full sweep #1) → 80.08 (`45d4ad8`, iter 5:
> AttributeQualifier string-only + probe batch) → 80.39 (`c37b2c4`,
> iter 6: narrowing range-check P0 fix + literal field-set matrix +
> CLI/examples measured as e2e) → **80.73** (`3dae78d`, iter 7 +
> full sweep #2); tests goal-metric 89.0.  Conformance 2035/2035
> both modes at sweep #2.
> Drive iterations from `scripts/coverage/plan_sim.py` (~21
> iterations remain, ~45 probes each, full sweep every 3rd); VERIFY
> CALLERS before writing probes — three "top gap" targets were dead
> code (LowerToCustomFn `c019fb1`, Engine::AddModule `c8e759d`;
> `WasmModule::SetMemory` still suspected).  Goal metrics print as
> the `GOAL METRICS` line of `native_cov_report.py`.
>
> **2026-07-29 — tests goal-metric definition corrected.**  The goal
> is "non-e2e + e2e test coverage (EXCLUDING conformance) at 100%
> excluding `ABSL_CHECK(false)`", and `line_pct_tests` has always
> honoured that (it prints as `tests-no-corpus`).  The *goal* metric
> did not: `compute_goal_metrics` counted a line as tests-covered on
> `bool(m)` — any workload at all, conformance included — so every
> line only the conformance corpus reached was silently credited.
> Fixed to require a non-corpus workload, which restates the tests
> goal-metric from 93.71 to **92.76** at `371b707`; nothing regressed,
> the earlier number was measuring the wrong thing.  The correction
> also realigns the two goals: a conformance-only line now counts
> against BOTH, so an e2e row covering one moves both metrics rather
> than only e2e.
>
> **Measurement fact worth not re-learning:** `runtime/*` rows come
> from the wasm gcov layer ALONE — `runtime/` is not in the native
> pass's `DEFAULT_SCOPES` — so native kernel unit tests
> (`cel_convert_test`, …) contribute nothing to those numbers.  A
> `poison(out, CEL_ERR_TYPE_MISMATCH)` guard in a runtime kernel is
> unreachable from any type-checked CEL expression, so it can only be
> retired by verdict classification, never by writing another native
> test.  Adding `runtime/` to the native scope was probed: it moves
> tests +0.55 but costs e2e 4.35 (runtime lines enter the e2e
> denominator while already counted via wasm), so it was NOT adopted.
>
> **2026-07-30 — lever returns, measured over batches 27-49.**  e2e
> 85.64 -> 87.73, tests 92.76 -> 93.81 (both on the corrected metric).
> Neither target met.  What each lever actually returns:
>
> | lever | e2e | tests |
> |---|---|---|
> | delete dead code | **+0.18** | **+0.18** |
> | verdict classification | +0.02 .. +0.15 | **0** |
> | unit tests on unreachable guards | 0 | +0.00 .. +0.02 |
> | e2e rows on conformance-only lines | +0.70 (once) | 0 |
>
> Deletion is the ONLY lever moving both, because it shrinks the
> denominator.  Classification cannot move tests at all: `test-only`
> exempts from e2e only, and `by-design` must be true.  The
> conformance-only pool that gave +0.70 in batch 29 is exhausted.
>
> **Do not bulk-tag the verdict ledger.**  ~215 functions read as
> e2e-dark-and-unclassified, but the density of legitimately taggable
> ones is far lower: a large share are template instantiations keyed to
> a test lambda (`BindTypedFunction`, `BindParsedFunction`) where a
> name-keyed verdict would exempt production instantiations too, and
> others have production callers a narrow grep misses (`BackendPrefix`
> is called from engine.cc:1397 and plugin_validate.cc:36).  Three
> near-misses in two batches.  Every verdict needs its callers read
> first — both ledger errors this session came from writing the
> evidence before finishing that check.
>
> **The tests gap needs infrastructure, not batches.**  ~1150 lines
> remain, concentrated in `eval/engine.cc` and `eval/instance.cc`
> wasmtime-trap and malloc-failure paths.  Reaching them needs a
> fault-injection harness; no amount of guard-arm unit testing gets
> there (that lever has decayed to +0.00).
>
> **Watch for tests that do not test what they are named.**  Four found
> this session: `IndexOfPosNegativeClamps` asserted the bug it should
> have caught; `cel_smoke_test`'s "not an FDS" row exercises the CLI's
> own loader (tools/cel/cel.cc:198), not `parse_and_check.cc:152`; and
> two of my own drafts (a `has(...)` row that would have claimed to read
> Struct-as-map, and a schema row whose runfiles-relative path silently
> took the NotFound arm).
>
> Verdict ledger
> = `scripts/coverage/function_verdicts.json`; per-iteration
> measurement = cached `bazel coverage` re-run + incremental
> `collect_wasm_gcov.sh` of changed binaries only (runtime/
> unchanged ⇒ wasm layer stays valid, incl. the conformance
> corpus's counters).  The prose below this callout describes the
> 2026-07-27 evening state and remains valid where not superseded.
>
> **Measurement invariant sharpened (2026-07-28, iteration 5):**
> editing a PRODUCT file invalidates the native `coverage.dat` of
> **every binary that links it**, not just the workloads with
> recorded hits on it — llvm-cov's export enumerates a binary's
> instrumented-line universe for a file even at zero hits, so stale
> exports resurrect deleted functions and old line tables into the
> merged report (observed: `SetMemory` and the deleted
> `AttributeQualifier` accessors reappearing as "newly uncovered",
> attribute.cc line-pct collapsing to 57%).  Since eval/ and
> compiler/ headers link into nearly every test, the practical rule
> is: **any product-code edit ⇒ re-run the full native `bazel
> coverage` pass** (cheap when the coverage config is warm — the
> expensive part is the config rebuild, not test execution); only
> pure test-file / script edits qualify for the narrow per-target
> re-measure.  Also: a plain `bazel test` of a target clobbers its
> `coverage.dat` — after any inner-loop testing, sweep for missing
> `.dat` files before regenerating (`tests(//...)` vs
> `bazel-testlogs/**/coverage.dat`).

**Branch:** `claude/m38-wasm-gcov-coverage`, 16 commits ahead of
origin, clean tree.  All gates were green at commit `761d3d7`
($PROJ 157/157, all 22 manual-tagged targets, conformance monotonic
both modes, full-PCH `scripts/lint.sh --branch` at zero findings);
commits after it (host-context tranche `bbba787`, eq/ne-arm deletion
`d2aff22`) have targeted test verification but still owe a final
full-gate pass before push.

**The goal (user-stated, superseded by the callout above):**
coverage-guided hill climbing to **e2e-only = 100% of non-error
branches** and **overall = 100% minus `ABSL_CHECK(false)` /
defense-in-depth arms**.  Error branches may be unit-covered; the
e2e-only column of the report is the number being climbed.

**THE REPORT (the deliverable the user keeps asking for):** the
combined Compiler / Eval / CelRuntime explorer with the e2e-only
vs all-tests split, function audit, branch-gap list, per-workload
redundancy, annotated source.  Published at
`https://claude.ai/code/artifact/f7eea479-1779-41a8-baed-4205c5b17230`
— from a NEW session, republish by passing that URL as the Artifact
tool's `url` parameter (same-file-path tricks only work within the
publishing conversation).

**One-command regeneration:**

```bash
scripts/coverage/run_full_coverage.sh /tmp/celwasm-coverage
# -> /tmp/celwasm-coverage/report.{html,json}
```

It drives: (1) instrumented-wasm build + per-workload gcda
collection over all dynamic e2e binaries + instance/memory_grow
manual suites, with engine_test + conformance via
`CELWASM_WASM_GCOV_DIR=<dir> bazel run` (they need runfiles);
(2) the native `bazel coverage` sweep in LLVM source-based mode
(the `llvm_gcov.sh` dual-role shim); (3)
`native_cov_report.py --wasm-cov-root` to join both layers.

**Regeneration invariants (violating these produced a silently-wrong
report once each):**

  - A plain `bazel test <target>` DELETES that target's
    `bazel-testlogs/<pkg>/<name>/coverage.dat`.  After ANY test
    running since the last sweep, re-run the `bazel coverage` sweep
    before regenerating — the generator prints `workloads: N`;
    anything under ~140 means clobbered data (a valid full run is
    143+ workloads).
  - Any change to `runtime/` invalidates every collected wasm
    `.gcda`/`.gcno` pair — wipe the wasm-cov raw tree and re-collect
    from a fresh instrumented build (checksum mismatches otherwise).
  - `.gcno` pairing rules are encoded in `collect_wasm_gcov.sh`
    (per-lib `_objs` dirs; `cp -f` because bazel outputs are r-x).

**Last VALID numbers (143 workloads, commit `bbba787`, before the
arm deletion):** total 82.74% line / 92.31% fn / 71.62% branch;
Compiler 90.04% line (e2e-only 70.87%), Eval 81.39% (62.98%),
CelRuntime 87.30% (87.14%), Other 57.61%.  195 zero-hit functions,
1,919 untaken branch sites.

**In flight at handoff:** a full uncached native sweep (all ~180
targets).  After it completes: instrumented wasm rebuild + FULL wasm
re-collection (runtime changed), regenerate, republish, then resume
the hill-climb loop: report → largest e2e-only non-error gaps →
e2e tests (or deletion / classification) → re-measure.

**User-approved deletions executed (`d2aff22`):** the 8 unreachable
per-kind eq/ne arms (int/uint/double eq+ne, bool_ne, numeric_ne) with
all consumers — exports, wat_runner bindings, WAT trace 17 retired,
traces 63/66/67 + focused compare tests + BM_IntEq repointed to the
production `cel_equals_at_vv` / `cel_not_equals_at_vv`; TypedAst
`mutable_ast` / `mutable_annotations` (zero callers).
**Recommended but NOT yet executed:** relocating `cel_make.c`'s
native-test-only constructors (all but `cel_make_string` +
`make_span_copy` + `alloc_cv`, which `cel_net_ext.c` uses) and
`arena_cursor`/`arena_capacity`/`cel_memory_size_`/`cel_mem_size`
into a test-support target so the shipped wasm stops carrying them.

**Classified unreachable — do NOT write tests for these (report
exemptions instead):** RejectDyn-blocked JSON `Struct`/`ListValue`
unpack (`cel_host.cc:270-364`) and double-key map hashing
(`cel_map_hash.h`) — unlock only via dyn passthrough;
`vend_poison_list_view`/`vend_poison_map_iter` + mismatch-message
builders + `WasmTrapToStatus` — codegen-drift tripwires;
`DecodeWireError` — 3VL absorbs error args before dispatch (pinned
e2e by `HostFnTest.ErrorValuedArgContract`); a non-strict-functions
API on `AddFunction` would be the unlock, `PROPOSALS.md`-worthy.

**Next e2e gap targets (from the last report):** `cel_host.cc` proto
read/write matrices (~780 uncovered lines — more field-shape e2e
cases, no product changes needed), `instance.cc` / `engine.cc` /
`cel_plugin.cc` / `cel_log.cc` remaining non-error runs, and the
branch-gap tab worked file-by-file.

**Session mechanics worth keeping:** commit hooks run a 90s review
agent that has timed out on every commit — `CEL_HOOK_SKIP=1 git
commit` is the sanctioned skip; `scripts/lint.sh` re-runs its ~10-min
cold-tree symlink populate after every bazel config switch (fastbuild
↔ coverage ↔ instrumented), so batch lint at gates; clangd
diagnostics in this tree are stale-index noise — bazel is the
authority.


Everything below is state from the session that produced this branch
(2026-07-27, base `ba28793`), recorded so a fresh agent/machine can
continue without re-deriving it.

### 7.1 Where this came from

This milestone is the direct follow-up to the audit at
`doc/implementation-plan/rewrite/reviews/2026-07-27-test-inventory-and-coverage.md`
(both commits are ancestors of this branch). The audit measured
coverage for the runtime on the **native** build only and flagged two
function families with zero native hits that are believed covered via
the wasm path (`cel_time.c` `*_with_tz` + conversions;
`cel_runtime.c` comprehension/aggregate helpers, minus the
`cel_host_cel_*` import shims which are wasm-only by construction).
The whole point of the m38 measurement run is to confirm or refute
that belief with real wasm-side line data and write the comparison
into that report's §2.

### 7.2 How the audit's NATIVE numbers were produced (reproduce before comparing)

Three non-obvious mechanics, all learned the hard way:

  1. **The toolchain's default gcov mismatches clang.** With the
     autodetected clang toolchain, bazel's collector runs GNU `gcov`,
     which cannot read clang's profile output — tests "pass" with
     empty coverage.dat. Fix: a wrapper script
     (`exec llvm-cov gcov "$@"`) and
     `coverage --repo_env=GCOV=<wrapper>` in `user.bazelrc`. Compile
     flags are unchanged, so this costs no rebuild.
  2. **A single `bazel coverage` pass cannot work.** Instrumenting
     `//runtime` leaks the profile flags into the wasm32 cross-compile
     (this is also what m38 exploits deliberately) — before this
     branch, every wasm-instantiating test then died at Plan on the
     unresolved `env::llvm_gcda_start_file` import. So native coverage
     ran as two passes, merged:
       - pass 1: `--instrumentation_filter='^//(compiler|eval|shared|abi|runtime|conformance|tools)[/:]'`
         over the 99 native-only suites;
       - pass 2: the same minus `runtime` over the 81
         wasm-instantiating suites;
       - `scripts/coverage/lcov_merge.py pass1.dat pass2.dat merged.dat`
         then `scripts/coverage/lcov_report.py merged.dat --misses`.
     NOTE: now that this branch's host hooks exist, the imports
     resolve, so a single-pass run may work again — but `--export-table`
     is keyed to `--//runtime:instrument_wasm`, not to the coverage
     config, so the dump step would error `no __indirect_function_table`
     (collection is env-var-gated anyway; it degrades to a warning).
     The two-pass recipe stays the documented native path.
  3. **The fuzz miner is excluded**: `//e2e/fuzz:cel_oracle_property_test`
     is a divergence discovery tool, not a regression suite.

Native headline at `ba28793`, all 180 targets green: compiler 90.2% /
runtime (native) 88.8% / eval 82.5% / abi 95.1% / shared 95.7% /
tools 75.9% lines. Full tables in the audit report.

### 7.3 Tooling committed with this branch

  - `scripts/coverage/lcov_merge.py` — merge lcov .dat reports.
  - `scripts/coverage/lcov_report.py` — per-file/per-dir tables +
    uncovered ranges.
  - `scripts/coverage/wasm_sections.py` — wasm import/export dumper
    (the §2 probe tool).

The wasm-side gcda→lcov step still needs a converter (llvm-cov gcov's
per-file output → lcov `DA:` records, or just report percentages
directly from `llvm-cov gcov -n`); nothing exotic — the report script
only needs `SF:`/`DA:` records.

### 7.4 Decisions already made (don't relitigate silently)

  - **Host-implemented gcda imports over porting compiler-rt to wasi**:
    wasi-sdk 25 ships no profile runtime, GCDAProfiling.c wants mmap,
    and host-side collection needs no preopened WASI dirs. Recorded in
    §2/§3; revisit only if wasi-sdk starts shipping
    `libclang_rt.profile-wasm32`.
  - **Imports registered unconditionally** on every Plan linker:
    harmless for normal modules, and it means an instrumented Program
    (static mode) also instantiates. Collection stays env-var-gated.
  - **Run-count deviation** from compiler-rt (per-file-per-dump instead
    of once-per-process) — deliberate, commented in the .cc, asserted
    by the tests.
  - **Basename flattening** of gcda paths is safe because runtime TU
    basenames are unique; revisit if a second instrumented module ever
    collects into the same directory.
  - The milestone number is **m38** (m37 is reserved/taken elsewhere).

### 7.5 Environment note

This branch was produced in a sandboxed cloud session whose egress
policy blocks bcr.bazel.build, GitHub `/archive/` tarballs,
mirror.bazel.build, and antlr.org; Bazel was fed via a registry mirror
+ repository-cache injection, all confined to the session's gitignored
`user.bazelrc`. None of that is needed on a normal network — a stock
checkout builds as usual. The only `user.bazelrc` line worth copying
locally is the coverage GCOV wrapper from §7.2 when the host compiler
is clang.
