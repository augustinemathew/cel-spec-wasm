# `e2e/fuzz/` — differential fuzzing of the compiler against cel-cpp

You are looking at the discovery tool that catches miscompiles before
users do: a **typed attribute grammar** generates CEL source that
type-checks by construction, every sample evaluates through BOTH our
Compile → Plan → Eval pipeline and the real cel-cpp evaluator
(`testdata/cel_cpp_oracle.cc`), and any value mismatch is a real bug —
never parser noise. Design doc:
[`m27-pbt-cel-generator.md`](../../doc/implementation-plan/rewrite/m27-pbt-cel-generator.md).

This README is the **living operational doc**: what the grammar covers
today, what it deliberately cannot reach yet (the gap list is the work
queue), how to run a mining session, and the dated notes log at the
bottom. Update it in the same commit as any grammar or harness change.

## The bug loop (the workflow this folder exists for)

1. **Mine**: run `mine_divergences` (or the fuzztest property) over a
   target type / depth range.
2. **Pin**: every divergence becomes a `TEST(KnownBugs, Pbt…)` row in
   [`e2e/known_bugs_test.cc`](../known_bugs_test.cc) — the exact
   shrunk source, the spec-correct assertion, the originating seed in
   the comment, `GTEST_SKIP` until fixed. The fuzz failure *emits a
   test*; the test is the deliverable even before the fix.
3. **Fix**: at the right layer (`layout_pass`, `expr_lower`, the C
   kernel…), delete the skip in the same commit, add the focused
   unit test beside the fix.
4. **Extend**: when a mining run comes back clean, the grammar is the
   suspect — add the next production family from the gap list below
   and mine again.

## Files

| File | Role |
| --- | --- |
| `grammar.{h,cc}` | `Production` / `Grammar` / `GrammarBuilder` + the L1 structural validator |
| `grammar_slice_b.{h,cc}` | scalar catalog: arithmetic, comparison, logical, ternary, concat, `size(string/bytes)`, safe conversions |
| `grammar_slice_c.{h,cc}` | aggregates: list/map literals (arity ≤10), `size`, `_in_`, comprehension macros over lists and maps |
| `grammar_test.cc` | L1 (structural) + L2 (per-production roundtrip through the real cel-cpp checker) + L3 (sampled composition at depths {1,3,6}) |
| `generator.{h,cc}` | `GenerateExpr(target, ctx)` — the seeded recursive walk |
| `oracle_harness.{h,cc}` | shared Compile→Plan→Eval + cel-cpp round-trip (`GenAndEvalSliceC`), the bound activation, `kMaxSourceBytes` = 4 KiB |
| `cel_oracle_property_test.cc` | `FUZZ_TEST` properties (fuzztest, shrinking), depth domain **0..6** |
| `mine_divergences.cc` | loop-driven miner: `bazel run //e2e/fuzz:mine_divergences -- <target> <max_seeds> <depth> [stop_after=5]` |
| `dump_samples.cc` | print generated sources for eyeballing grammar output |
| `fuzz_smoke_test.cc` | fuzztest framework wiring check |

L1+L2+L3 green ⇒ a value divergence is necessarily a runtime/codegen
bug, not a generator misconfiguration. Re-run `grammar_test` after ANY
catalog change, before mining.

## Running a session

```bash
bazel test //e2e/fuzz:grammar_test          # validate the grammar first
bazel run //e2e/fuzz:mine_divergences -- bool 2000 6      # 2000 seeds, depth 6
bazel run //e2e/fuzz:mine_divergences -- list_int 1000 8  # deep-depth list sweep
bazel test //e2e/fuzz:cel_oracle_property_test            # fuzztest mode (shrinking)
```

Miner output classes: `DIVERGE` (the prize — both sides evaluated,
values differ), `OUR-REJECT` / `ORACLE-REJECT` (one side refused a
source the other accepted — also a bug if systematic), `ORACLE-ERR-VAL`
(oracle produced a CEL error on a grammar-guarded-total expression —
suspicious, the grammar admits only total productions).

## Trophy case (proves the method works)

- **#32/#33** — `exists_one` comprehension result stamped from the
  wrong slot (`layout_pass.cc`); found at depth 6, 3 pins in
  `known_bugs_test.cc` (`PbtExistsOne*`), fixed.
- **`EmitConditional` local-vs-offset confusion** — ternary inside int
  subtract (seed 3696381601904611693, depth 4); pinned as
  `PbtTernaryInsideIntSubtract`, fixed.
- **#34** — depth-7/8 reliably hit the fixed 64 KiB arena cliff; PBT
  was the instrument proving the cliff reachable from ordinary nested
  expressions, not just pathological literals.

## Gap list — what the grammar CANNOT find today (the work queue)

Ordered by expected bug yield. Evidence: every entry names a bug class
that was found **manually** (conformance mining / code reading) in
territory the grammar doesn't reach — meaning PBT would have found it
first if the productions existed.

0. **Large / wide expressions and bound aggregates** (steer,
   2026-06-11): the grammar grows expressions by *depth* only; width
   is capped at arity-10 list literals, and the activation binds
   **scalars only** (`oracle_harness.cc::SliceBBoundActivation`) — so
   PBT can never emit an activation-bound `list<T>` / `map<K,V>`,
   leaving the whole host-origin aggregate path (`cel_list_in` over
   bound data, host map lookup — the #17 family) invisible. Needed:
   (a) bound `xs: list<int>` / `ms: map<string,int>` activation
   entries + ident leaves (the m27 vocabulary table already specifies
   them — never wired); (b) wide productions — arity-20/50 literals,
   long `+`/`&&` chains via a width knob; (c) comprehensions over
   bound ranges, not just literals; (d) revisit
   `kMaxSourceBytes = 4 KiB` which silently caps exactly these shapes.
1. **Depth > 6 (deep nesting)** — *opened 2026-06-11, first results
   below.* The property test caps at depth 6
   (`cel_oracle_property_test.cc:338`), and historical deep mining was
   blocked by the #34 arena cliff. The growable arena landed with the
   ssp-fix merge (3079b37). First re-mining session: **0 value
   divergences at depths 7–8**, but a systematic compile-capacity
   ceiling (see notes log) — the static window, not the arena, is now
   the deep/large-expression limiter.
2. **Error-producing productions (3VL / absorption semantics)** — the
   grammar admits only total operations (no `/`, `%`, no unbounded
   indexing), so divergences like `ExistsAbsorbsErrorAccumulator`
   (`[0,2].exists(x, 2/x == 1)` — found manually) are structurally
   invisible. A guarded slice — `(%0 / %1)` where `%1`'s sub-grammar
   can emit `0` — turns the whole error-absorption matrix minable.
   The oracle harness already distinguishes error values
   (`kOracleErrorValue`), so the property becomes "same value OR same
   error-ness".
3. **Boundary-value leaf domains** — leaves are tiny fixed sets
   (`0`, `1`, `7`, `"hello"`…). `MapKeyLossyDoubleEquality` lives at
   2^53; int overflow lives at INT64_MIN/MAX; none are emittable.
   `grammar.h` already anticipates domain-backed leaves; implement
   them (weighted boundary set: MIN, MAX, ±2^53, -1, 0).
4. **Multi-byte UTF-8 string leaves** — `size('πέντε')`,
   `indexOf` codepoint-vs-byte bugs were all found manually; current
   string leaves are ASCII-only. One leaf per UTF-8 width class.
5. **String-ext + conversion calls** — `contains`/`startsWith`/
   `endsWith`/`indexOf`/`replace`/`split`, `int(string)` with
   leading-`+`/whitespace shapes (the wave-4 manual bug cluster).
   Total subset first (predicates), error-producing forms after #2.
6. **Nested aggregates** — `list<list<int>>`, `map<string, list<T>>`:
   the registration loops iterate scalars only (m27 §"Future work" has
   the 5-step recipe; miner comparison must recurse).
7. **Timestamp / duration** — entirely absent from the grammar; the
   max-range construction bug was manual. Total arithmetic subset
   (literal ± bounded duration) is safe to admit.
8. **kStructExpr / kSelectExpr (Slice C2)** — proto messages
   (`celwasm.testdata.Customer`) deferred; field-read codegen is
   unexercised by PBT.

## Notes log (newest first; add an entry per session)

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
  `ERROR-DIVERGE`).  This unblocks gap #2 (error-producing
  productions) and boundary-value leaves (#3): overflow shapes
  classify instead of polluting runs.
- **Dedup**: `cel_oracle_property_test.cc` carried a full
  pre-harness copy of the plumbing (activation drift was the
  failure mode — it broke the moment `xs` landed).  Now routed
  through `oracle_harness`; property depth domain raised 0..8 with
  ResourceExhausted capacity-rejects skipped.
- **New `compare.{h,cc}` (+14-case test)**: recursive type-driven
  ours-vs-oracle comparator (nested aggregates ready) replacing the
  miner's per-kind if-chains.
- Dev-tooling unblocked in passing: `refresh_compile_db.sh` no
  longer dies on the wasm32-only manual target;
  `build_lint_pch.sh` tries candidate TU flag sets until the PCH
  compiles (entry order broke it after a compile-db regen).

### 2026-06-11 — first deep-depth mining session (gap #1 opened)

Miner runs (fastbuild, Apple Silicon), all on the Slice C grammar:

| target | depth | seeds evaluated | agreed | DIVERGE | OUR-REJECT |
| --- | --- | --- | --- | --- | --- |
| `bool` | 7 | 889 | 881 | 0 | 8 (~0.9%) |
| `bool` | 8 | 37 (stopped at 3 rejects) | 34 | 0 | 3 (~8%) |
| `list_int` | 8 | 113 | 110 | 0 | 3 (~2.7%) |
| `int` | 8 | 230 | 227 | 0 | 3 (~1.3%) |

Findings:

- **Zero value divergences at depths 7–8** (1252 agreeing samples).
  The slot-reuse + growable-arena codegen holds up at depths never
  mined before. The #34 arena cliff did not reappear.
- **The new ceiling is compile capacity, not eval correctness.**
  Every reject is `RESOURCE_EXHAUSTED: expression requires too many
  workspace slots` — rodata + workspace exceeding the 8192-byte
  static window (`ValidateExprStaticRegion`). Rejection rate grows
  steeply with depth (~1% at 7 → ~8% at 8 for `bool`). This is the
  same window pinned by `LiteralIntListInScan*` in
  `known_bugs_test.cc`, but the PBT evidence is new: it bites
  *ordinary nested expressions* at depth 8, not just pathological
  literals. Production impact: a large policy expression fails to
  compile with no recourse but hand-splitting. Feeds the m29
  readiness plan (the static-region grow/relocate aspiration).
- Steer received this session: prioritize **large** expressions —
  width, many variables, bound lists/maps, comprehension-heavy
  shapes. Filed as gap #0 above; it's the next surface to implement.

### 2026-06-11 — baseline analysis (loop session start)

- Audited the folder against m27 + the repo. m27's status header is
  accurate (Slices A+B+C1 shipped; C2/D open). The miner, harness,
  property tests, and L1/L2/L3 all exist as documented.
- Key observation: the #34 blocker on deep-depth mining is *gone*
  (growable arena, merge 3079b37) but the depth-6 cap and the mining
  cadence never caught up. Gap #1 opened as the first target.
- Pinned-bug bookkeeping check: all `PbtExistsOne*` and
  `PbtTernaryInsideIntSubtract` rows in `known_bugs_test.cc` are
  un-skipped (fixed) — no orphaned skips from prior PBT sessions.
