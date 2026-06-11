# `e2e/fuzz/` — differential fuzzing of the compiler against cel-cpp

This folder catches miscompiles before users do. A typed attribute
grammar generates CEL source that **type-checks by construction**;
every sample evaluates through BOTH our Compile → Plan → Eval pipeline
and the real cel-cpp evaluator; any disagreement — value, error-ness,
or acceptance — is a finding, never parser noise. Plan of record:
[`m30-fuzz-full-dialect.md`](../../doc/implementation-plan/rewrite/m30-fuzz-full-dialect.md)
(architecture rationale in the closed
[`m27-pbt-cel-generator.md`](../../doc/implementation-plan/rewrite/m27-pbt-cel-generator.md)).

This README is the living operational doc: how the rig works, how to
run it, how to extend it, and the dated notes log at the bottom.
Update it in the same commit as any grammar or harness change.

## How it works

```
 ActivationSchema()            Grammar (the production catalog)
 grammar_scalars.h             grammar_scalars.cc   — constants (incl. boundary/
 name → CelType for every      grammar_aggregates.cc  unicode leaves), idents,
 bound variable; the ONE       │                      arithmetic, comparisons,
 list both sides consume       │  validated by        logic, ternary, lists/maps,
        │                      │  grammar_test.cc     size, _in_, comprehensions
        │                      ▼  (L1+L2+L3, below)
        │              GenerateExpr(target, seed, depth)     generator.cc
        │                      │
        │                      ▼
        │              one CEL source string, e.g.
        │              ([xs]).exists(v, (v + i_a) > 9007199254740992)
        │                      │
        ▼                      ▼
 oracle_harness.cc :: GenAndEvalFull(target, seed, depth)
        │                                   │
        ▼                                   ▼
 OURS: Compile → Plan → Eval     ORACLE: cel-cpp parse/check/eval
 (the production pipeline)       (testdata/cel_cpp_oracle.cc)
        │                                   │
        └───────────────┬───────────────────┘
                        ▼
        compare.cc :: Compare(ours, oracle, target)
        recursive, type-driven; NaN-agreement; map = key-set
                        │
                        ▼
   verdict: agreed · DIVERGE · ERROR-DIVERGE · both-errored ·
            our-reject · oracle-reject · too-large
```

Two drivers share that harness:

- **`mine_divergences`** — sequential-seed CLI; prints every anomaly
  with its seed + source, summary line at the end. The daily tool.
- **`cel_oracle_property_test`** — fuzztest properties (one per
  target type, depth domain 0..8) with shrinking; `manual`-tagged
  because finding a bug fails the test, which is its job.

**Why a typed grammar?** Random bytes die in the lexer; shape-driven
generation wastes half its budget on type-check rejects. Here every
emitted source reaches codegen and the runtime, so all the budget
lands where the bugs are.

**Why you can trust a failure.** `grammar_test.cc` runs three
validation layers before any mining: **L1** structural checks
(`Grammar::Validate()` — every type has a leaf, placeholders are
consistent), **L2** every single production round-tripped through the
real cel-cpp checker, **L3** sampled compositions at depths {1,3,6}.
All three green ⇒ a divergence is necessarily a compiler/runtime bug,
not a generator misconfiguration.

## Running a session

```bash
bazel test //e2e/fuzz:grammar_test            # validate the grammar first
bazel run //e2e/fuzz:mine_divergences -- bool 2000 6       # target, seeds, depth
bazel run //e2e/fuzz:mine_divergences -- list_int 1000 8 3 # …optional stop_after
bazel test //e2e/fuzz:cel_oracle_property_test             # fuzztest mode (shrinking)
bazel run //e2e/fuzz:dump_samples -- bool 4 10             # eyeball what the grammar emits
```

Targets: `bool int uint double string bytes list_int list_bool
list_double list_string map_string_int`.

Reading the summary line:

| Field | Meaning | Act on it? |
| --- | --- | --- |
| `agreed` | both sides evaluated, values match | no — the baseline |
| `diverged` | value mismatch, or we produced a value where cel-cpp errored (`ERROR-DIVERGE`) | **yes — pin it (see below)** |
| `our_rejected` | our Compile/Plan/Eval refused | `ResourceExhausted` = the known static-window capacity ceiling (rate climbs with depth); anything else = a bug |
| `oracle_rejected` | cel-cpp refused a source we accepted | yes — systematic cases are bugs |
| `both_errored` | both sides produced a CEL error | no — agreement (error *messages* aren't compared; backlog #31) |
| `too_large` | source exceeded `kMaxSourceBytes` (4 KiB) | no — generation cap |

## The bug loop

1. **Mine** until a `DIVERGE` / `ERROR-DIVERGE` appears.
2. **Pin**: add a `TEST(KnownBugs, Pbt…)` row to
   [`e2e/known_bugs_test.cc`](../known_bugs_test.cc) — the exact
   source, the spec-correct assertion, the seed in the comment,
   `GTEST_SKIP` until fixed. The fuzz failure *emits a test*; the
   test is the deliverable even before the fix.
3. **Fix** at the right layer (`layout_pass`, `expr_lower`, the C
   kernel…); delete the skip in the same commit; add the focused
   unit test beside the fix.
4. **Extend**: a clean run means the grammar is the suspect — add
   the next surface from the m30 queue and mine again.

## Adding a surface (the extension contract)

1. Productions/leaves go in `grammar_scalars.cc` (scalar families)
   or `grammar_aggregates.cc` (container families) via the
   `GrammarBuilder` shorthands (`Leaf`/`Unary`/`Binary`/`Ternary`/
   `Repeated`/`Comprehension`).
2. A new bound variable is TWO edits, enforced loudly:
   `ActivationSchema()` (grammar side) and `MakeEntry()` in
   `oracle_harness.cc` (its value, both representations) — a schema
   entry without a value CHECK-fails at first use, so the lists
   cannot drift silently.
3. `bazel test //e2e/fuzz:grammar_test` — L2 auto-covers every new
   production by construction; you write no new validation code.
4. Mine. Record the session in the notes log below.

Error-producing productions (division, fallible conversions) are
admissible: error-ness is a compared dimension, so "both error" is
agreement and "only cel-cpp errors" is a find.

## Trophy case (the method works)

- **#32/#33** — `exists_one` comprehension result stamped from the
  wrong slot (`layout_pass.cc`); found at depth 6, 3 pins
  (`PbtExistsOne*`), fixed.
- **`EmitConditional` local-vs-offset confusion** — ternary inside
  int subtract (seed 3696381601904611693, depth 4); pinned as
  `PbtTernaryInsideIntSubtract`, fixed.
- **#34** — depth-7/8 reliably hit the old fixed 64 KiB arena cliff;
  PBT proved the cliff reachable from ordinary nested expressions.
- **cel-cpp comprehension budget interaction** (2026-06-11) — a
  generated ~11k-iteration `exists_one` nest tripped cel-cpp's 10k
  DoS guard; oracle budget raised, and the production observation
  (cel-wasm has NO eval iteration cap) feeds m29.

## Gap list — what the grammar cannot find yet

> Scheduled as [`m30-fuzz-full-dialect.md`](../../doc/implementation-plan/rewrite/m30-fuzz-full-dialect.md):
> #2 → M30.B, #6/#7/#8 → M30.C, #5 → M30.D, #0b-d/#1 → M30.E.
> This list stays the evidence record; the m30 doc is the order.

Each entry names a bug class found *manually* in territory the
grammar couldn't reach — PBT would have found it first if the
productions existed.

0. **Large / wide expressions** — ~~bound aggregates~~ (`xs`/`ms`
   shipped, e9ab2fc). Still open: width (arity caps at 10), long
   chains, comprehensions over larger ranges, the 4 KiB source cap.
1. **Depth > 8** — depth 7–8 opened 2026-06-11 (0 divergences; the
   static window is the limiter — rejection ~1% at d7, ~8% at d8 for
   bool). The 9–12 band is unmined.
2. **Error-producing productions** — no `/`, `%`, unbounded index,
   so 3VL absorption divergences (`ExistsAbsorbsErrorAccumulator`)
   stay invisible. The harness side is ready (error-ness compared).
3. ~~**Boundary-value leaves**~~ — shipped (M30.A, 8236374): INT64
   boundaries, 2^53±1, UINT64_MAX, −0.0/epsilon/denormal/1e308.
4. ~~**Multi-byte UTF-8 leaves**~~ — shipped (M30.A, 8236374):
   2/3/4-byte, embedded NUL, combining mark; invalid-UTF-8 bytes.
5. **String-ext + conversion calls** — `contains`/`startsWith`/
   `endsWith`/`indexOf`/`replace`/`split`/`format`, `int(string)`
   shapes (the wave-4 manual bug cluster).
6. **Nested aggregates** — `list<list<int>>`, `map<string,list<T>>`;
   the comparator already recurses, the registration loops don't.
7. **Timestamp / duration** — absent entirely; the max-range
   construction bug class was found manually.
8. **Proto struct/select** — `celwasm.testdata.Customer` productions
   + a message-typed binding (needs OracleVar proto marshalling).

## Notes log (newest first; add an entry per session)

### 2026-06-11 — M30.A adversarial leaves + readability refactor

- 19 boundary/unicode leaf productions landed (8236374); every one
  round-trips the real cel-cpp checker (L2) and composes (L3).
  Mining sweep across 8 targets × depths 6/8 in flight; results
  appended here when it completes.
- Folder refactored for reviewability: `grammar_slice_b/c` →
  `grammar_scalars/aggregates`, all `SliceB`/`SliceC` identifiers
  and prose renamed to role-based names, and the activation became
  schema-derived (`ActivationSchema()` + `MakeEntry()` with a CHECK
  tripwire) instead of two hand-synced lists.

### 2026-06-11 — bound aggregates + error classification + harness dedup

- **Bound aggregates live**: `xs: list<int>` / `ms: map<string,int>`
  joined the activation (gap #0a) — PBT now reaches host-origin
  aggregate codegen.  Validation: bool d6 × 2000 seeds → 1997
  agreed / 0 diverged / 3 capacity-rejects.
- **Found + root-caused**: cel-cpp's `comprehension_max_iterations`
  (10k/eval DoS guard) fired on a generated 4-level `exists_one`
  nest needing ~11k iterations (bool seed=683 d6) — the oracle now
  runs with a 10M budget (`testdata/cel_cpp_oracle.cc`) and the
  seed agrees on value.  Production note for m29: cel-wasm has NO
  eval iteration cap at all.
- **Error-ness is now a compared dimension**: `GenAndEvalStatus`
  split `kOracleErrorValue` → `kBothErrored` (agreement) vs
  `kOracleErrorOnly` (the error-swallowing detector; miner prints
  `ERROR-DIVERGE`).
- **Dedup**: the property test carried a full pre-harness copy of
  the plumbing (activation drift was the failure mode).  Now routed
  through `oracle_harness`; property depth domain raised 0..8 with
  ResourceExhausted capacity-rejects skipped.
- **New `compare.{h,cc}` (+14-case test)**: recursive type-driven
  ours-vs-oracle comparator replacing per-kind if-chains.
- Dev-tooling unblocked in passing: `refresh_compile_db.sh` no
  longer dies on the wasm32-only manual target;
  `build_lint_pch.sh` tries candidate TU flag sets until the PCH
  compiles.

### 2026-06-11 — first deep-depth mining session (gap #1 opened)

Miner runs (fastbuild, Apple Silicon), all on the full grammar:

| target | depth | seeds evaluated | agreed | DIVERGE | OUR-REJECT |
| --- | --- | --- | --- | --- | --- |
| `bool` | 7 | 889 | 881 | 0 | 8 (~0.9%) |
| `bool` | 8 | 37 (stopped at 3 rejects) | 34 | 0 | 3 (~8%) |
| `list_int` | 8 | 113 | 110 | 0 | 3 (~2.7%) |
| `int` | 8 | 230 | 227 | 0 | 3 (~1.3%) |

- **Zero value divergences at depths 7–8** (1252 agreeing samples).
  The #34 arena cliff did not reappear post-growable-arena.
- **The new ceiling is compile capacity, not eval correctness** —
  every reject is the 8192-byte static window
  (`ValidateExprStaticRegion`); rate grows steeply with depth.
  Production impact: a large policy fails to compile with no
  recourse but hand-splitting.  Feeds the m29 readiness plan.

### 2026-06-11 — baseline analysis (loop session start)

- Audited the folder against m27 + the repo; miner, harness,
  property tests, and L1/L2/L3 all exist as documented.
- The #34 blocker on deep-depth mining is gone (growable arena,
  merge 3079b37) but the depth-6 cap and mining cadence never
  caught up — opened as the first target.
- Pinned-bug bookkeeping check: all prior PBT pins are un-skipped
  (fixed) — no orphaned skips.
