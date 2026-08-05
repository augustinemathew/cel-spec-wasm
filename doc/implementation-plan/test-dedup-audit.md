# Cross-suite test-duplication audit

Date: 2026-08-05.  Audited against `origin/master` @ `f49cabe`; every
file:line below was re-verified in that tree (and spot-checked in the
working tree, which matches at every cited anchor).  Excluded per the
audit brief: `eval/internal/cel_host_test.cc` (mid-refactor on a
concurrent branch — not read as current truth).

Scope: 131 test files, ~75.5k lines.  Definition of duplication used
(per the repo's testing philosophy): **same assertion, same layer, same
input shape, surviving in two+ places.**  Explicitly NOT counted:
`_static`/`_dynamic` link-mode pairs, oracle `ExpectAgree` cases,
CELBUG/CELSKIP pins (skipped state), WAT regression runs, and
cross-layer re-assertion (runtime kernel + e2e of the same op).

Provenance note: the e2e language-surface reorg commit (`e69c7ae`,
2026-08-01) itself disclosed "~400 lines of redundancy — KNOWN, not yet
addressed", citing a review at `/tmp/e2e-dup-review.md` (still on disk,
378 lines).  Every one of that review's findings was re-verified live on
`origin/master` — **none was addressed** — and this audit extends it
with fresh cross-suite sweeps the branch-diff review did not cover.

## Summary table

| # | Cluster | Files (keep vs delete) | LoC recoverable | Risk | Recommendation |
|---|---------|------------------------|-----------------|------|----------------|
| C1 | BindFunction signature/arity-mismatch matrix re-implemented in e2e | keep `eval/engine_test.cc:699-800`; delete `e2e/host_fn_test.cc:1036-1086` | ~55 | Low | Delete e2e copy (it never reaches Compile/Plan/Eval; existing suite is strictly larger) |
| C2 | `ProgramFactsE2E` re-implements `abi/program_facts_test.cc` | keep `abi/program_facts_test.cc` (+4 var-kind rows folded in) and `e2e/program_roundtrip_test.cc:360` only; delete the other six tests at `e2e/program_roundtrip_test.cc:256-430` | ~135 | Low | Delete 6 of 7 e2e tests; fold `int`/`string`/`type`/`list<map<…>>` rows into `abi/program_facts_test.cc:139` first |
| C3 | Wrapper construct-and-read-back tested three times | keep `e2e/wrapper_test.cc:1134-1250`; delete `e2e/wkt_field_set_test.cc:682-708` (`WrapperFieldKindMatrix`) + `e2e/static_aggregate_test.cc:343-368` (3 tests) | ~57 | Low | Delete matrix + 3 rows; wrapper_test distinguishes proto2/proto3, the copies don't |
| C4 | Wrong-accessor wire-kind diagnostic asserted at three surfaces | keep `eval/host_call_context_test.cc:923` (9-kind unit matrix) + one e2e row (`e2e/host_fn_test.cc:587`); delete `e2e/host_fn_test.cc:835` matrix + its private helpers | ~90 | Med | Keep unit matrix + single e2e proof; coordinate with the in-flight cel_host split branch |
| C5 | Second timezone-accessor fixture beside the existing `TzGrid` | keep `e2e/time_test.cc:1366-1420` (`TzAccessorE2ETest`); delete `e2e/time_test.cc:866-931` (`CivilFieldTzE2ETest` + errors) after moving ~5 novel rows into `TzGrid` | ~50 net | Low | Merge rows, delete fixture |
| C6 | `"%d".format([<non-int>])` asserted twice in one file | keep `e2e/string_ext_test.cc:404`; delete `:508-524` (`FormatKindMismatchSurfacesError`) | ~17 | Low | Delete; the 404 matrix covers it plus four more malformed shapes |
| C7 | Proto map-field literal construction twice in one file | keep `e2e/wkt_field_set_test.cc:614-640` (MessageDifferencer group); drop `map_int32_int64` + `map_uint64_uint64` rows from `:764` | ~12 | Low | Drop the two overlapping rows |
| C8 | `StringConcatFooBar` is iteration 0 of its own sibling | keep `e2e/operators_test.cc:1937` (1024-eval arena-reset lock); delete `:1913-1928` | ~22 | Low | Delete; move the DESIGN §2 citation onto the sibling |
| C9 | Fixed known-bugs pins with byte-identical surface-suite twins | keep `e2e/conversion_test.cc:241-250` and `e2e/comprehension_test.cc:345`; delete `KnownBugs.IntFromDoubleOutOfRange` (`e2e/known_bugs_test.cc:288-296`) + `KnownBugs.ExistsAbsorbsErrorAccumulator` (`:514-…`) | ~30 | Low | Delete the known_bugs copies; surface suites carry oracle citations / full absorption tables |
| C10 | Null-pruned tests strictly subsume their plain siblings | `e2e/proto_from_host_test.cc:295` ⊃ `:309`; `:438` ⊃ `:409` | ~30 | Med | Optional — plain copies buy failure isolation only |
| C11 | Near-duplicate 3VL absorb rows | `e2e/partial_eval_test.cc:1363` vs `:1365` (`WrapperUnwrapErrorInt64`) | ~3 | Low | Drop the Int64 row (absorb fires before the kind cross-check; rows differ only cosmetically) |
| C12 | Wrapper rows repeated inside `proto_from_host_test.cc` | keep `:485` (`BoundWrapperFieldsReadAsNullableScalars`); drop the two wrapper rows from `:600` (`SingularFieldReadKindMatrix`) | ~10 | Low | Drop rows, point the matrix comment at `:485` |

**Totals: ~511 LoC recoverable overall; ~391 LoC at low risk
(C1+C2+C3+C5+C6+C7+C8+C9+C11+C12).  Biggest single cluster: C2
(`ProgramFactsE2E`, ~135 LoC).**

Context for scale: the recoverable total is ~0.7% of the 75.5k-line test
corpus.  The corpus is NOT broadly duplicative — duplication is
concentrated in the 2026-08-01 coverage-driven e2e expansion (10 of 12
clusters were introduced or doubled by commit `e69c7ae`), and the
dominant pattern is *a kind-matrix added for a surface that already had
a per-kind suite, with neither side deleted*.

---

## Per-cluster evidence

### C1 — BindFunction mismatch matrix (e2e re-implements the unit suite)

`e2e/host_fn_test.cc:1060` `BindFunctionSignatureMismatchMatrix` (11
rows) and `:1076` `BindFunctionArityMismatch`, plus their private helper
`ExpectBindMismatch` (`:1036-1055`), assert registration-time
`InvalidArgument` from `Engine::BindFunction` naming the declared slug
and C++ spelling.  They never call `Compile`, `Plan`, or `Eval` — there
is no e2e content.  `eval/engine_test.cc:699-800`
(`EngineBindFunctionTest`, 30 tests) is the same layer (a fresh
`Engine`, no wasm), strictly larger (adds `null`, asserts the parameter
index), and pre-existing.  The e2e block sits directly beneath a comment
stating the registration-level matrix "lives in eval/engine_test.cc"
(`e2e/host_fn_test.cc:1026-1028`).  Risk of deletion: none — every row
has a superset twin.

### C2 — `ProgramFactsE2E` vs `abi/program_facts_test.cc`

`abi/program_facts_test.cc` already compiles real programs through the
real `Compiler` (`CompileBytes`, line 37), so the unit-vs-e2e layer
distinction does not exist for this surface.  Pairwise (new → existing):

- `e2e/program_roundtrip_test.cc:413` `DescribeModuleWithoutAbiSection`
  → `abi/program_facts_test.cc:189` (identical 8-byte module; existing
  also checks rendered text — new one strictly weaker).
- `:424` `DescribeRejectsNonWasmBytes` → `:201` (identical, different
  garbage bytes).
- `:286` `DescribeReportsRequiredHostFn` → `:212` (same assertions;
  existing also checks `FormatProgramFacts`).
- `:399` `DescribeReportsLinkMode` → `:78` + `:235` (existing covers
  both link modes explicitly; new one only re-asserts the harness's
  mode).
- `:306` `DescribeRendersEveryDeclarableVarTypeSpec` → `:139` (two
  "every declarable kind" matrices for one renderer; neither a superset
  — fold the e2e-only kinds `int`, `string`, `type`, `list<map<…>>`
  into the abi test before deleting).
- `:256` `DescribeReportsDeclaredVars` → recombination of five existing
  assertions (`:56`, `:92`, `:104`, `:113`, `:182`); its only novel bit
  (index-order assertion) contradicts the existing test's deliberate
  refusal to pin declaration order.
- **Keep** `:360` `DescribeRendersSignaturesAcrossTypeFamilies` — the
  one test proving the *compiler emits* those wire types (the rendering
  side is pinned by `abi/celfn_wire_test.cc:241-291`).

Risk: low.  Deleting after the fold loses nothing; a var-type matrix
was added to BOTH sides in the same commit (`e69c7ae` says so
explicitly).

### C3 — wrapper construct-and-read-back, three copies

`e2e/wkt_field_set_test.cc:684` drives
`TestAllTypes{single_int32_wrapper: 5}.single_int32_wrapper == 5` —
byte-identical to `e2e/wrapper_test.cc:1140` (and `:1151`, the proto2
twin).  Seven of the matrix's nine rows are covered one-per-test in
`e2e/wrapper_test.cc:1134-1250` (`WrapperRoundTripE2ETest`), which the
M8 doc treats as the spec-of-done and which distinguishes proto2 from
proto3 (the matrix does not).  The remaining two rows
(`single_uint32_wrapper`, `single_float_wrapper`) already appear as
construction-equality rows at `e2e/wrapper_test.cc:889`/`:901`.
The third copy: `e2e/static_aggregate_test.cc:343` `StructOfWrapperInt`,
`:352` `StructOfWrapperString`, `:361` `UnsetWrapperReadsNull`
(the last duplicating `e2e/wrapper_test.cc:504`
`UnsetInt32WrapperReadsAsNullProto3`) — nothing in them is about the
fixture's nesting cross-product; `StructOfStruct` (`:370`) is already
the "message-typed field" cell.  Risk: low; optionally add
uint32/float round-trip rows to `wrapper_test.cc` for exact parity.

### C4 — wrong-accessor wire-kind diagnostic, three surfaces

All three assert: `ArgInt` (etc.) on a non-matching slot fails
`InvalidArgument` naming the wire kind.

- `e2e/host_fn_test.cc:587` `ContextWrongAccessorDiagnosticNamesWireKind`
  (duration, single row).
- `e2e/host_fn_test.cc:835` `WrongAccessorDiagnosticKindMatrix` — 8 full
  compile+plan+eval cycles; its own comment concedes "the Duration arm
  is pinned above".
- `eval/host_call_context_test.cc:923` `MismatchDiagnosticNamesWireKind`
  — 9 rows including `null`/`type`/`unknown`/`error`, which the e2e
  matrix cannot reach at all.

The unit matrix is cheaper and strictly more complete.  Keep it plus ONE
e2e row (the `:587` duration test) as the "a real compiled program emits
this wire kind" proof; delete the 8-row matrix and its private helpers
(`WrongAccessorEvalStatus` / `ExpectWrongAccessorNames`, ~`:780-856`).
Risk: **medium only operationally** — `eval/host_call_context_test.cc`
sits adjacent to the in-flight `cel_host*` split; sequence this deletion
after that branch lands.

### C5 — second timezone fixture beside `TzGrid`

`e2e/time_test.cc:866-931` (`CivilFieldTzCase`/`CivilFieldTzE2ETest` +
`ZoneGrid`, 10 rows, + `CivilFieldTzE2ETestErrors`) is structurally the
pre-existing `e2e/time_test.cc:1366-1420` `TzAccessorE2ETest`/`TzGrid`
harness with the instant hard-coded instead of parameterised.  Row-level:
`OffsetNeg_Hours` (`-03:00`) pins the same `ResolveTimeZone` `sign = -1`
arm as `TzGrid`'s `FixedMinus8_Hours`
(`eval/internal/cel_host.cc:5295`); `MinutesOutOfRangeOffsetErrors`
(`+05:60`) and `TzGrid`'s `InvalidOffset` (`+25:00`) both reject on the
single check at `cel_host.cc:5305`.  Move the genuinely novel rows (the
NY accessor kinds, unsigned `05:30`, bare `Z`, half-hour minutes,
`+05:60` as an `expect_error` row) into `TzGrid`; delete the fixture.
Risk: low (rows preserved by the move).

### C6 / C7 / C8 / C11 / C12 — intra-file pairs (all introduced 2026-08-01)

- C6: `e2e/string_ext_test.cc:404` includes row
  `"%d".format(["not an int"])` → IsError; `:511` asserts
  `"%d".format(["not a number"])` → IsError.  Same verb, kind,
  assertion; strings differ in wording only.
- C7: `e2e/wkt_field_set_test.cc:614` constructs
  `TestAllTypes{map_int32_int64: {1: 2}}` with whole-message
  `MessageDifferencer` equality; `:764`'s first row is the same
  construction asserted by read-back, and its `map_uint64_uint64` row
  duplicates `:627`/`:634`.  Keep the stronger MessageDifferencer
  group.
- C8: `e2e/operators_test.cc:1913` `StringConcatFooBar` is iteration 0
  of `:1937` `StringConcatRepeatedAcrossManyEvals` (same source, same
  assertions, wrapped in a 1024-eval loop that adds the arena-reset
  guarantee).  Both were folded from the retired `mvp_concat_test.cc`
  in the reorg — this is the one true "pre-rename leftover twin" the
  reorg left behind.  A third copy at `:301` (`Concat`, `"a"+"b"`), and
  the static-link copy at `e2e/static_link_test.cc:103` is mode
  coverage (not a finding).
- C11: `e2e/partial_eval_test.cc:1363` `WrapperUnwrapError` vs `:1365`
  `WrapperUnwrapErrorInt64` — both rows are absorbed by the
  field-type-agnostic guard (`eval/internal/cel_host.cc:5116`, then
  `:5386`) before the int32/int64 distinction is ever reached.
- C12: `e2e/proto_from_host_test.cc:600` `SingularFieldReadKindMatrix`
  repeats the `single_int32_wrapper`/`single_string_wrapper` read path
  that `:485` covers for all seven wrapper kinds.

### C9 — fixed known-bugs pins whose surface twin also exists

`e2e/known_bugs_test.cc` holds 57 tests (23 still CELBUG/CELSKIP-gated,
excluded from this audit; 34 un-skipped, i.e. live fixed-bug regression
guards).  Two of the live ones have byte-identical same-layer twins:

- `KnownBugs.IntFromDoubleOutOfRange` (`:288-296`,
  `int(-9223372036854775808.0)` → error) vs
  `e2e/conversion_test.cc:241-250` `DoubleAtIntMinIsError` — same
  literal expression, same layer, same assertion; the conversion_test
  copy carries the cel-cpp oracle citation and is the keep.
- `KnownBugs.ExistsAbsorbsErrorAccumulator` (`:514`,
  `[0, 2].exists(x, 2/x == 1)`) vs
  `e2e/comprehension_test.cc:345` `AccuAbsorptionCase
  {"ExistsErrorThenMatch", "[0, 2].exists(x, 2/x == 1)", …}` — the
  surface suite's parameterised absorption table (10 rows,
  `:345-367`) strictly subsumes the pin.

The other 32 live pins were cross-checked and have **no** same-layer
same-shape twin (e.g. the map-dot-field family `{'a': 1}.a` etc. exists
ONLY in known_bugs; `size('πέντε')` as a *literal* differs in input
shape from `operators_test.cc:1062` `BindMultibyteUtf8`, which binds the
string — rodata vs activation-marshal paths; the host-origin `Pbt*`
family is unique).  Keep them.

### C10 — null-pruned supersets (judgement call)

`e2e/proto_from_host_test.cc:309` `HostMapValueMatrix.MessageValue` vs
`:295` `MessageValueNullPruned`, and `:409`
`HostRepeatedMatrix.MessageElements` vs `:438`
`MessageElementNullPruned`: the pruned tests' bindings are supersets and
assert the same expected message, so the plain tests add nothing except
failure isolation (if pruning regressed, only the pruned test reddens).
Delete only if that isolation is judged not worth 30 lines.

---

## Categories checked and found CLEAN (no action)

- **`e2e/host_fn_test.cc` vs `e2e/host_fn_type_matrix_test.cc`** — the
  matrix file's header (`host_fn_type_matrix_test.cc:1-15`) declares the
  tier split: host_fn's boundary suites use `AddTypedFunction`
  (`host_fn_test.cc:2254-2360`), the matrix's use the Context tier
  (`host_fn_type_matrix_test.cc:1327`, `:1349`, `:376`) — different
  registration surface, different marshalling code path.  Exactly one
  deliberate overlap cell, disclosed in-line
  (`host_fn_type_matrix_test.cc:274`).
- **Cross-file expression sweep over all 25 e2e language-surface
  files** — extracting every string compiled via
  `Compile`/`CompilePlan`/`Eval*` helpers found only `"1 + 1"` and
  `"x + 1"` (harness smoke exprs) appearing in two files.  The e2e
  duplication is fixture-level (the clusters above), not scattered.
- **Runtime suite cross-file** — the only kernels invoked from 3+ files
  are harness helpers (`cel_make_*`, `cel_value_at`, `cel_mem_base`);
  `cel_equals_at_vv` appears in `cel_compare_test` (scalars),
  `cel_deep_eq_test` (aggregates), `cel_optional_test` (optionals) —
  disjoint value shapes.
- **`runtime/cel_convert_test.cc` (98 singles, 2 tables)** — sampled;
  each single is a distinct boundary story (`UintToIntOverflow`,
  `DoubleToIntNaNRejected`, …) with a distinct input+expectation.
  Longhand style, zero repeated assertions — matches the CLAUDE.md
  "distinct stories stay TEST_F" rule.  Not duplication.
- **`compiler/compiler_test.cc` vs `compiler/internal/compile_test.cc`**
  — facade lifecycle/decl-validation vs pipeline/module-shape; the one
  overlap (bad source → InvalidArgument at both entry points) is two
  public entry points, ~10 lines, not worth touching.
- **`compiler/internal/compile_test.cc:336-435` budget tests vs
  `e2e/limits_test.cc`** — near/over-budget via the internal pipeline in
  both link modes vs exact numeric boundaries (10909/10910 frames, 2048
  depth, 100k codepoints) via the public API.  Overlapping intent,
  different precision and API level; the CLAUDE.md "Compilation limits"
  section mandates the e2e pair.  Left as-is (~40 borderline lines).
- **`eval/engine_test.cc` vs `eval/instance_test.cc`** — suite-level
  division (Plan-time vs Eval-time) holds; no shared assertions found.
- **Conformance corpus vs e2e hand-assertions (audit category d)** —
  `e2e/encoders_ext_test.cc` (129 lines) and `e2e/wkt_field_set_test.cc`
  re-assert corpus rows verbatim and say so
  (`encoders_ext_test.cc:4-6`, `wkt_field_set_test.cc:1-12`).  This is
  by-design under the repo's own bug-tracking rule 1 ("one test case per
  conformance row") and buys default-sweep signal the pre-push-only
  conformance gate does not.  Not recommended for deletion; the only
  actionable overage is where a corpus row is asserted in TWO e2e
  suites, which is C3/C7.
- **Pre-rename leftovers (audit category c)** — the reorg (`e69c7ae`)
  deleted `m12_test.cc` and `mvp_concat_test.cc` outright and renamed 21
  files with small deltas; no old-named twin survived as a file.  The
  only content-level leftover is C8 (the mvp_concat fold-in).
- **Suite-name collision sweep** — the only fixture names appearing in
  two e2e files (`ErrorE2ETest`, `RejectE2ETest`, `RoundTripE2ETest`)
  are generic names over disjoint domains (math vs network, conversion
  vs time, encoders vs time).

## Related non-duplication findings (from the surviving 2026-08-01 review, still unfixed)

Not LoC-recoverable, listed so they aren't lost when `/tmp` is cleared:

- `e2e/operators_test.cc:284` `DoubleNegateAndSubtract` does not test
  double negation (`-1.5` is parser-folded; its own sibling comment at
  `:426-429` says so).  Rename to `DoubleSubtract`, drop the `-1.5`
  assertion; `:432` is the real negate coverage.
- `e2e/activation_boundary_test.cc:209` asserts only `!v.ok()` and so
  cannot distinguish the `malloc returned NULL` arm it documents from
  the `arena_alloc` failures its two sibling tests hit; assert the
  message substring.
- Misplaced comment blocks that overstate coverage:
  `e2e/string_ext_test.cc:191-197`,
  `e2e/proto_from_host_test.cc:658-670`,
  `e2e/partial_eval_test.cc:1300-1310`.

## Bottom line

- Realistic low-risk recovery: **~391 LoC** across 10 clusters, almost
  all of it deleting re-implementations whose keep-side is strictly
  larger or carries the better citation.
- Full recovery including the two medium-risk clusters: **~511 LoC**
  (~0.7% of the test corpus).
- The corpus is not systemically duplicative; the concentration is the
  2026-08-01 coverage expansion, whose own commit message flagged the
  redundancy as "KNOWN, not yet addressed".  Executing C1-C3 + C5-C9 +
  C11-C12 clears that debt; C4 should wait for the cel_host split
  branch to land.
