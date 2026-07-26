# PBT / fuzz infrastructure architecture review — 2026-06-12

Trigger: user assessment that the code "looks like it is built
piecemeal."  Two read-only review agents (grammar/catalog layer;
harness/driver layer), briefed to diagnose the accretion patterns and
propose ONE coherent target architecture each, with stepwise
migrations.  Builds on `2026-06-11-pbt-subsystem.md` (whose
code-structure queue items are subsumed into the plans below).

## Verdict

The piecemeal feel is **real but localized to the seams, not the
content**.  The lower layers (grammar data model, generator walker,
comparator recursion, oracle contract) are sound.  The accretion is:

1. **Catalogs organized by *when added*, not *what it is*.**  Function
   boundaries encode mining-session eras (`RegisterArithmetic` vs
   `RegisterFallibleArithmetic` split at the moment error-comparison
   shipped; conversions split across two functions for the same
   reason; temporal comparisons live apart from all other comparisons;
   `split`/`join` sit in the aggregates file purely because their type
   signature crossed the old Slice-B/C seam).
2. **Load-bearing decisions exist only as prose.**  Every
   withhold-decision (production deliberately not generated because
   the oracle disagrees) is a comment citing a `Pbt*` pin by name in
   free text; nothing machine-checks the comment ↔ pin-test ↔
   COVERAGE.md triangle.
3. **The verdict pipeline was never reified.**  Classify → compare →
   render → pass/fail exists in two-and-a-half hand-rolled copies:
   the miner's switch, the property test's switch, PLUS the property
   test re-implements six per-kind comparators duplicating
   `compare.cc` (including the NaN discipline).  Consequence: the
   drivers **disagree on what a failure is** — a non-capacity
   our-side rejection fails the property test but never affects the
   miner's exit code, so that regression class passes nightly CI.
4. **Registration-by-boilerplate.**  Each property-test target is
   ~10 hand-written lines + a FUZZ_TEST line — which is *why* only 6
   of 13 targets are CI-gated; `fuzz.sh` still hand-syncs a third
   target list.

## Target architecture (one recommendation per layer)

**Grammar layer — a Surface registry over per-family catalog TUs,
with withholds first-class in the Grammar:**

- `Surface {name, coverage_anchor, Register fn}` +
  `AllSurfaces()` in `surfaces.{h,cc}` — the ONLY chronological file;
  `BuildFullGrammar()` loops it.  Each mining session = one family TU
  + one registry line (the session-at-a-time loop gets a sanctioned
  place to append instead of mid-file insertions).
- Family TUs split semantically: `grammar_leaves`, `grammar_arith`,
  `grammar_comparisons` (one `RegisterComparisons(b, type, ordered)`
  replaces the 3× hand-expanded sextet), `grammar_strings` (split/join
  reunified), `grammar_math_ext` (table), `grammar_temporal`,
  `grammar_conversions` (all conversions in one place),
  `grammar_aggregates` (shrinks ~40%); `grammar_scalars.{h,cc}` and
  `BuildScalarGrammar` are deleted (the scalar-only grammar existed
  only as the Slice-B era artifact and doubles L2/L3 test cost).
- `GrammarBuilder::Withhold(target, {name, format, reason, pin_test})`
  + `Grammar::Withheld()` — withholds become grammar data;
  `Validate()` rejects re-registering a withheld name (the
  "someone re-added it without clearing the pin" tripwire).
- `surfaces_test.cc` cross-checks: every `coverage_anchor` is a
  COVERAGE.md heading; every `pin_test` exists in
  `known_bugs_test.cc`.  Same trick `targets_test` already applies.
- Migration: 8 steps, ~1.5–2 days, every step grammar_test-green; a
  **manifest golden test** (sorted production (name, format, type)
  triples) lands first and proves steps 2–4 and 7 are pure moves.

**Harness layer — one verdict path consumed by both drivers:**

- `verdict.{h,cc}`: `Verdict RunOne(target, seed, depth)` owning
  classify+compare+render once.  `VerdictKind` splits today's 6-way
  status into 9 (notably `kOurCapacityReject` vs
  `kOurUnexpectedReject`, and `kBothErroredAgree` vs
  `kErrorKindDiverged` — the queued error-kind comparison lands in
  exactly one place).  `Verdict::IsFailure()` becomes THE failure
  definition for miner exit code, property test, and CI alike.
- `targets.h` becomes an X-macro table; property-test registrations
  are macro-generated from it → **all 13 targets CI-gated
  automatically**, new target = one row.  `fuzz.sh` derives
  `ALL_TARGETS` from `mine_divergences --list-targets` (deletes the
  third hand-synced copy).
- Miner shrinks ~70 lines; property test ~170 (its six duplicate
  comparators die; it inherits `compare.cc`'s aggregate support and
  renders).
- Oracle boundary stays fuzz-agnostic (shared with 10+ non-fuzz
  consumers).  Two wrinkles noted: (a) oracle declares vars as `dyn`
  while our compiler declares precise types — latent overload-
  resolution asymmetry; fix = optional `OracleVar::type`; (b)
  checker-side vs runtime-side extension registration are two
  hand-synced lists 80 lines apart — collapse to one paired table.
- Migration: 6 steps, ~2–3 days incl. the error-kind mining session;
  steps 1–2 verified by byte-identical `RESULT` lines on a fixed seed
  set; step 6 (exit-code includes unexpected rejects) makes the
  nightly gate strictly stricter — flagged as a deliberate behavior
  change.

## Explicitly NOT changing

`grammar.{h,cc}` core data model · the longhand leaf lists with
per-leaf rationale comments (relocate, don't compress) · the
generator walker · `targets.{h,cc}` (the template this copies) ·
`compare.cc` recursion + never-agree default (extend, don't fork) ·
the `testdata/cel_cpp_oracle` contract · operational contracts
(`RESULT` line format, 125 exit clamp, `manual` tags, fuzz.sh
subcommands) · the activation schema/values split (dependency-forced,
CHECK-enforced) · `kMaxSourceBytes` capacity-skip semantics.

## Execution order (recommended)

Harness step 1–2 (verdict extraction) and grammar steps 1–4 (manifest
golden + pure moves) are independent and can interleave; the
high-value behavior changes (13-target CI, error-kind comparison,
stricter exit code) ride on the harness track.  Full plans live in
the two agent reports' migration sections, reproduced above in
condensed form; treat this doc as the plan of record and tick steps
here as they land.

- [ ] H1 verdict.{h,cc} + miner port (fixed-seed RESULT-identical)
- [ ] H2 property test port (deletes duplicate comparators)
- [ ] G1 manifest golden test
- [ ] G2 activation.{h,cc} extract (fixes generator→catalog inversion)
- [ ] G3 family-TU split (manifest-identical)
- [ ] G4 dissolve Slice-B/C seam (manifest-identical)
- [ ] H3 X-macro targets + 13 CI-gated properties
- [ ] H4 fuzz.sh --list-targets
- [ ] G5 Surface registry; delete scalar grammar; collapse test dup
- [ ] G6 Withhold() + surfaces_test cross-checks
- [ ] G7 intra-family tables (comparison helper, MathOp[], leaf struct)
- [ ] H5 error-kind comparison (+ mining session, pin finds)
- [ ] H6 stricter exit code + contract test
- [ ] G8 BUILD cleanup + --list-surfaces
