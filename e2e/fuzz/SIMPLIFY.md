# Simplification plan — `e2e/fuzz/`

Status: in progress — drafted 2026-06-12; step 1 (verdict
unification) shipped same day, proven pure by a byte-identical
fixed-seed RESULT diff across all 13 targets.
Supersedes the code-structure bullets in `m30-fuzz-full-dialect.md` §5
and condenses the two architecture reviews
(`doc/implementation-plan/rewrite/reviews/2026-06-12-pbt-architecture.md`)
into the minimal shape.  **Close-out:** when every box below is
ticked, fold a summary into `SESSIONS.md`, update `README.md`'s file
map, and delete this doc.

## Goal

~3,860 → ~1,880 lines with the same coverage and a stronger
extension story.  The simplicity rule: **everything is either a
catalog data row or goes through `RunOne()`.**  Two extension points,
one new concept (`Verdict`), zero registries/macros.  The shrink is
subtraction — deleting parallel implementations:

| Deleted | Lines | Replaced by |
| --- | ---: | --- |
| `compare.{h,cc}` + `compare_test.cc` | 459 | `conformance::CompareValue` (the gate's own comparator — already NaN/duration/timestamp-aware, already used by `cel_cpp_oracle_test.cc`) |
| miner's verdict switch + property test's verdict switch + its 6 hand-rolled comparators | ~330 | `verdict.{h,cc}` `RunOne()` (~90) |
| `BuildScalarGrammar` + scalar/aggregate L2/L3 test duplication + the L3 copy of the generator walker | ~300 | one grammar, one suite, the real walker |
| boilerplate in drivers | ~150 | thin loops/asserts around `RunOne` |

## Target layout

```
e2e/fuzz/
  targets.{h,cc}        WHAT types we mine (13 rows)            [exists]
  grammar.{h,cc}        engine: Production/Builder/Generate     [absorbs generator.{h,cc}]
  catalog_leaves.cc     ┐ WHAT expressions look like — data rows
  catalog_ops.cc        │ grouped by family, ~100-200 lines each,
  catalog_strings.cc    │ one Register fn per file, composed by
  catalog_temporal.cc   │ BuildGrammar() in catalog.h/.cc
  catalog_aggregates.cc ┘
  verdict.{h,cc}        HOW we judge: RunOne(target, seed, depth)
  oracle_harness.{h,cc} bindings + dual-eval only (shrinks)
  mine_divergences.cc   loop → count → RESULT → exit code (~80)
  cel_oracle_property_test.cc  FUZZ_TEST per target (~60)
  dump_samples.cc       print samples                            [exists, thin]
  grammar_test.cc       L1 units + L2 round-trip + L3 (one grammar, ~180)
  verdict_test.cc       Verdict classification matrix
```

Conventions (not machinery):
- A withheld production = delete the row, leave `// WITHHELD: <PbtTest>`.
- New catalog row → L2 auto-validates it against the real checker.
- The `RESULT` line format, exit-code clamp (125), and `manual` tags
  are frozen operational contracts.

## Verification protocol (used by every step)

Fixed-seed baseline: for each of the 13 targets,
`mine_divergences <t> 300 5 99999` → keep only the `RESULT` line.
Steps marked **[pure]** must reproduce the baseline byte-identically.
Step 2 (comparator swap) must reproduce all *counts* (renders may
differ); any count delta means a semantic difference between the two
comparators — STOP and investigate before proceeding.  Capture before
step 1 into `/tmp/fuzz_baseline.txt`.

---

## Step 1 — `verdict.{h,cc}`: one judge, thin drivers  **[pure]**

- [x] Capture the fixed-seed baseline (all 13 targets) to
      `/tmp/fuzz_baseline.txt`.
- [x] `verdict.h`: `VerdictKind` (kAgreed, kBothErrored, kValueDiverged,
      kOracleErrorOnly, kOurCapacityReject, kOurUnexpectedReject,
      kOracleRejected, kSourceTooLarge), `struct Verdict {kind, seed,
      depth, source, ours, oracle, detail}`, `IsDivergence()`,
      `IsFailure()`, `Report()`, `RunOne(target, seed, depth)`.
- [x] `verdict_test.cc` first (interface → tests → impl): one case per
      VerdictKind via expressions that force each outcome, + Report()
      smoke.
- [x] `verdict.cc`: move (not rewrite) the miner's classify/compare/
      render logic; the capacity-vs-unexpected reject split uses the
      `ResourceExhausted` check from the property test.
- [x] Port `mine_divergences.cc`: argv → loop → `RunOne` →
      `counters[kind]++` → `Report()` on divergence → same `RESULT`
      line → same exit code (divergences only, for now).
- [x] Port `cel_oracle_property_test.cc`: each registered target body
      becomes `Verdict v = RunOne(...); EXPECT_FALSE(v.IsFailure()) <<
      v.Report();`.  Delete its 6 hand-rolled comparators.
- [x] BUILD: `:verdict` library (testonly); drivers depend on it.
- [x] `bazel test //e2e/fuzz:...` green (incl. property test, manual).
- [x] Baseline diff: byte-identical `RESULT` lines.
- [x] Lint touched files; commit.

## Step 2 — delete `compare.{h,cc}`: one comparator in the repo

- [x] Verify semantic parity where it matters before swapping.
      Verified: doubles use the IDENTICAL algorithm on both sides
      (NaN-matches-NaN then C++ `==`, so ±0.0 equal in both —
      `runner.cc:173` vs old `compare.cc` ScalarsEqual); kind
      mismatch fails in both; list/map recursion exists
      (`CompareList`/`CompareMap`, `runner.cc:477-480` — the same
      comparator that scores all 1973 conformance rows).
- [x] Swap `verdict.cc`'s comparison to `conformance::CompareValue`;
      divergence render = the Status message (`mismatch =` line in
      the DIVERGE report).  Any non-OK status — including
      InvalidArgument for a kind it has no comparator for — is a
      divergence, preserving the never-agree-on-a-gap discipline.
      Dead `Verdict::{ours,oracle}` fields and Judge's unused
      `target` param removed.
- [x] Delete `e2e/fuzz/compare.{h,cc}`, `compare_test.cc`; BUILD edit
      (`:verdict` deps on `//conformance:runner`).
- [x] Mine the 13 targets at the fixed seeds: all counts identical to
      baseline.  Property test (~6000 iterations) green.
- [x] Lint; commit.

## Step 3 — one grammar, one test suite  **[pure]**

- [x] Merge `generator.{h,cc}` into `grammar.{h,cc}` (engine = data
      model + walk); `NewGenCtx` moved beside `ActivationSchema` in
      the scalar catalog (fixes the engine→catalog dep inversion);
      `generator_test.cc`'s determinism/depth/reachability cases
      folded into `grammar_test.cc`.
- [x] Delete `BuildScalarGrammar`; one grammar (rename
      `BuildFullGrammar` → `BuildGrammar`).
- [x] `grammar_test.cc`: collapsed scalar/aggregate L1+L2+L3
      duplication into one suite over the one grammar
      (635 → ~540 lines); L3 calls the real `GenerateExpr`
      (the `namespace l3` walker copy deleted).
- [x] Baseline diff: byte-identical (verified twice — after the
      move and again after the lint cleanup below).
- [x] Lint; commit.  Bonus: cleared ALL 10 pre-existing clang-tidy
      warnings in `grammar.{h,cc}` (const-ref `target` params;
      dropped never-passed `weight` defaults from `Ternary` /
      `Comprehension`, fixing their 7-param gate exceedance;
      `Validate` split via `ValidateProduction` under the 60-line
      gate).  `grammar.{h,cc}` is now fully lint-clean.

## Step 4 — catalogs regrouped by family  **[pure]**

- [x] Split/regroup `grammar_scalars.cc` + `grammar_aggregates.cc`
      into `catalog_{leaves,ops,strings,temporal,aggregates}.cc`
      along semantic lines (function bodies moved VERBATIM via
      scripted extraction — zero retyping).
      > Delta: list/map leaf literals stayed in
      > `catalog_aggregates.cc` (they share the `ScalarVocab` /
      > `MapVocab` internals with the constructors; a shared-internal
      > header for one move wasn't worth it).  Temporal comparisons
      > stayed inside `RegisterTemporal` in `catalog_temporal.cc` —
      > arguably more semantic than relocating rows out of their
      > function.
- [x] `catalog.h/.cc`: activation schema + `NewGenCtx` +
      `BuildGrammar()` composition.
      > Delta: the historical registration order INTERLEAVES families
      > (strings register at two points), and order is
      > generation-affecting (rules-vector order feeds
      > `PickProduction`).  So `BuildGrammar` calls the fine-grained
      > Register fns in the exact historical sequence — family files
      > group the *definitions*; the composition documents
      > "append at the end, never re-sort."
- [x] Rows moved verbatim — production names/format strings unchanged;
      leaf rationale comments relocated verbatim.
- [ ] Comparison-sextet helper + math table where they collapse
      copy-paste *within* a family (names preserved) — DEFERRED to a
      follow-up; the move itself stayed pure.
- [x] Delete `grammar_scalars.{h,cc}`, `grammar_aggregates.{h,cc}`;
      BUILD: one `:catalog` target (6 srcs, 1 hdr).
- [x] Baseline diff: byte-identical.  `grammar_test` /
      `verdict_test` / `targets_test` green.
- [x] Lint (all 7 new files clean); commit.

## Step 5 — close out

- [ ] Register the 7 missing aggregate targets in the property test
      (now one line each).
- [ ] `mine_divergences --list-targets`; `fuzz.sh` derives
      `ALL_TARGETS` from it (deletes the hand-synced copy).
- [ ] Update `README.md` file map + "adding a surface" contract;
      `COVERAGE.md` anchors (function names → catalog files);
      `SESSIONS.md` entry; reconcile `m30` §5.
- [ ] Full sweep green (`scripts/fuzz.sh validate && sweep`).
- [ ] Delete this doc (fold summary into SESSIONS.md).

## NOT changing

Grammar data model + `GrammarBuilder` vocabulary · leaf rationale
comments (relocate verbatim) · `targets.{h,cc}` + `targets_test` pin ·
`testdata/cel_cpp_oracle` contract · `RESULT`/exit-code/`manual`
contracts · `fuzz.sh` subcommand surface · `kMaxSourceBytes` capacity
semantics · activation schema/values split (dependency-forced,
CHECK-enforced).

Deferred (unchanged from m30 §5): error-kind comparison inside
`RunOne` (lands in one place when it comes) · stricter exit code ·
proto slice · net_ext/optionals type-vocab work.
