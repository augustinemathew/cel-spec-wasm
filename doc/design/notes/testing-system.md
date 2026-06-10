# testing-system — design notes (undefined)

Scope: the testing system as a system — e2e suites, conformance harness,
cel-cpp oracle, known-bugs registry, checklist/gate docs, and the scripts
that bind them.  Verified against code on branch `m28-configurable-linking`
(2026-06-10).  third_party/ out of scope.

## 1. Verified architecture

### 1.1 The layers

The project tests at seven distinct layers; each pins a different failure
class:

1. **Unit / component tests** — one `*_test.cc` per source file across
   `compiler/`, `runtime/` (native-compiled C kernels), `abi/`, `shared/`,
   `eval/`.  Run in `bazel test //...`.  Per-layer required scenarios are
   enumerated in `doc/implementation-plan/per-component-test-coverage.md`
   §3.1–§3.13 (frontend → IR → resolve → layout → expr_lower → module →
   ABI → runtime → host imports → API → e2e → conformance → WAT).
2. **Manual-tagged wasmtime/cross-compile tests** — excluded from
   `bazel test //...` by `tags = ["manual"]`.  Verified current manual
   *test* targets: `//eval:instance_test` (eval/BUILD.bazel:615-617),
   `//eval:engine_test` (:677-679), `//runtime:cel_runtime_wasm_test`
   (runtime/BUILD.bazel:326 + manual tag), runtime stripped-bytes test
   (:779-781), `//tools/wat_runner:wat_runner_test`
   (tools/wat_runner/BUILD.bazel:32-35), `//tools/cel:cel_smoke_test`
   (tools/cel/BUILD.bazel:127), `//tools/cel:activation_matrix_test`
   (:138-140), `//conformance:run_conformance`
   (conformance/BUILD.bazel:165), all of `//bench/...`, and four e2e
   targets: `host_fn_test`, `foreign_component_dispatch_test`,
   `foreign_fn_type_matrix_test`, `host_fn_type_matrix_test`
   (e2e/BUILD.bazel:641,674,697,721).
3. **E2E suites** (`e2e/*.cc`) — full Compile → Plan → Eval/PartialEval
   pipeline, decoded-`Value` assertions.  One suite per milestone theme
   (`m2`…`m18`, `wkt_field_set`, `cctz_doubles`, `activation_boundary`,
   `optimize`, `program_roundtrip`, `mvp_concat`).  **Dual link-mode
   emission**: the `link_mode_e2e_cc_test` macro
   (e2e/link_mode_e2e_test.bzl:25-43) emits `<name>_dynamic` AND
   `<name>_static` cc_tests from one source; the `_static` build defines
   `CELWASM_E2E_USE_STATIC_LINK_MODE`, read by
   `e2e/link_mode_e2e_helpers.h:50-55` to set
   `CompilerOptions::LinkMode` for every `CompilePlan`/`DefaultOpts`
   call (helpers at :75-91).  A shared process-wide `GlobalEngine()`
   (:61-68) amortizes wasmtime instantiation of `cel_runtime.wasm`.
   Most e2e targets are NOT manual-tagged (e2e/BUILD.bazel — only the
   four above carry the tag), so they run in the default suite.
4. **Known-bugs registry** (`e2e/known_bugs_test.cc`) — each test is a
   CONFIRMED defect as a runnable regression: the body asserts the
   spec-correct behavior, a leading `GTEST_SKIP` keeps CI green; fixing
   = delete the SKIP line (file header :1-17).  Classes: lossy
   double map-key equality (:94-107), 64 KiB arena cliffs (:109-185,
   :617-692), conformance-mined divergences (:187-338), string-ext
   kernels (:340-384), comprehension semantics (:386-439), timestamp /
   double formatting (:441-522), parser codepoint cap (:579-598),
   long-arith unaligned-atomic trap (:783-799).  Fixed bugs stay as
   live (un-skipped) guards: `IntFromDoubleOutOfRange` (:260-268),
   `IntFromStringLeadingPlus`/`Uint…` (:303-318),
   `LongArith_1000Terms_Works` (:754-761).  One test
   (`TransformMapEntryComputedEntryCrash` :424-439) stays skipped
   because running it ABSL_CHECK-aborts the whole binary.
5. **Conformance harness** (`conformance/`) — runs the vendored
   upstream corpus (`spec/tests/simple/testdata/*.textproto`, 30 files
   listed in `run_conformance.cc::DefaultCorpus` :63-97) through the
   real pipeline.  Detail in §1.2.
6. **cel-cpp oracle** (`testdata/cel_cpp_oracle.{h,cc}` +
   `cel_cpp_oracle_test.cc`) — differential reference.  Detail in §1.3.
7. **Examples smoke test** — `//examples:examples_smoke_test`, an
   always-on `sh_test` running all 9 example binaries and asserting key
   output lines; the rot gate for doc snippets (examples/BUILD.bazel:185-200).

### 1.2 Conformance harness contracts

- **Outcome taxonomy** — every row is exactly one of `kPass` /
  `kUnsupported` (SKIP) / `kFail` (runner.h:42-46).  SKIPs always carry
  a `SkipCategory` tag (9 values, runner.h:87-97); the textual names
  (`SkipCategoryName`, runner.cc:202-225) are a public contract — the
  README aggregates by them, and `runner_test.cc:161-174` pins all nine
  strings.
- **Classification by status payload, not message text** —
  `ClassifyCompileFailure` (runner.cc:698-729) reads
  `kStaticSubsetViolationUrl` / `kUndeclaredReferencesUrl` payloads set
  by `compiler/frontend` (`status_tags.h`), with a source-syntax
  fallback (`LooksLikeExtensionSyntax`, runner.cc:637-643) for
  parser-stage ext-lib failures where the payload never attaches.
  Ext-lib detection is two-tier: namespace roots
  (runner.cc:558-568) OR all-receiver-roots (:580-618).
- **A compile error satisfies an error matcher** — rows whose matcher
  is `eval_error` PASS when Compile fails for a non-SKIP reason
  (runner.cc:725-727), mirroring cel-cpp's upstream `conformance/run.cc`.
- **Error matching is kind-only** — `CompareEvalError` passes iff
  `got.IsError()`; message text is never compared (runner.cc:434-449).
  Same for unknowns: `CompareUnknown` checks `IsUnknown()` only — the
  matcher's AST-id list can't be round-tripped through `AttributeId`
  yet (runner.h:127-131).
- **Value comparison** — maps order-agnostic with
  `StructurallyEquals` keys (runner.cc:453-495, langdef map equality);
  lists order-aware (:498-526); `NaN` matches `NaN` (:168-174);
  `object_value` unpacks Any via the generated pool and uses
  `MessageDifferencer::Equals` with a descriptor pre-screen
  (:349-379); enums compare as int (:381-392); type values byte-equal
  (:396-407).  A row with NO result matcher is an implicit
  bool-true assertion (runner.cc:244-247, :762-772).
- **Per-row compiler reuse** — rows without `type_env` share one
  process-wide default Compiler (runner.cc:654-661); rows with declared
  variables build per-test (:663-667).  `binding_marshal.{h,cc}`
  marshals `bindings:`/`type_env:` entries, returning graceful
  `Unimplemented` (→ `kBindingUnsupported`/`kTypeEnvUnsupported`) for
  unsupported shapes (binding_marshal.h:6-26).  Supported value kinds:
  scalars, enum-as-int, Any `object_value`, `type_value`
  (binding_marshal.cc:81-140); map/list/error/unknown bindings still
  SKIP.
- **Dual link-mode runs** — `--link_mode={dynamic,static}` flag on the
  single binary (runner.cc:53-57, CompileForTest :669-684).
- **`run_conformance` is NOT a CI gate** — it exits 0 unconditionally
  (run_conformance.cc:7-9, main returns 0 at :249).  The gate is
  `scripts/check_conformance_monotonic.sh`: runs the binary once per
  link mode, extracts the PASS count, and enforces a monotonic
  baseline per mode (`conformance/.baseline` = 1899,
  `.baseline_static` = 1899; script :42-43, :148-155).  Deliberately
  fastbuild, not `-c opt` (script :81-89; pass counts verified
  identical).  The `.githooks/pre-push` hook runs the gate and then
  `regen_conformance_readme.sh --check --from-log` so the README's
  AUTOGEN tables (headline, per-fixture inventory, skip totals,
  addressable prose) cannot drift from the live run (hook lines
  ~46-58; markers in conformance/README.md:32-36, :125-158, :163-184).
- **Current headline** — total=2454 pass=1899 (77.4%) skip=463 fail=92
  (README:34).  463 SKIPs: 371 out-of-scope by design
  (static_subset=227 + disable_check=144), 92 scope-not-shipped
  (ext_unimpl=55, check_only=25, type_env=12) (README:163-184).

### 1.3 The oracle

- `EvalWithCelCpp(source, container)` — parse + check + eval through
  the REAL cel-cpp compiler/runtime, configured to mirror cel-cpp's own
  modern conformance service (qualified type identifiers, heterogeneous
  equality, reference resolver, registered conformance enums;
  cel_cpp_oracle.cc:73-108).  Result is the neutral `cel.expr.Value`
  exchange proto plus `is_error` / `is_unknown` flags
  (cel_cpp_oracle.h:54-62).  A CEL eval error is a first-class outcome
  even when cel-cpp surfaces it as a non-OK `Evaluate()` status —
  folded into `is_error` (cel_cpp_oracle.cc:228-239); non-OK from
  earlier stages = harness failure and propagates.
- `PartialEvalWithCelCpp(source, container, vars, unknown_patterns)` —
  the partial-eval oracle (cel_cpp_oracle.h:97-100): declares every var
  as `dyn` (cc:134-140), binds those with values, installs dotted
  `AttributePattern`s, enables `UnknownProcessingOptions::kAttributeOnly`.
- `testdata/cel_cpp_oracle_test.cc` runs DIFFERENTIAL assertions:
  `ExpectAgree` evaluates through both engines and compares with the
  SAME comparator the conformance gate uses
  (`conformance::CompareValue`, oracle_test:100-118).  Pins the M20
  range-check contract (:145-217), Tier-1 conversion fixes (:226-236),
  and partial-eval semantics, including the cleanup-backlog #14
  comprehension-over-unknown-range verdict (:284-289) and
  loop-var-pattern-is-noop (:294-300).

### 1.4 SKIP discipline (as practiced)

- Rule source: per-component-test-coverage.md §4 (:377-399) — never
  fixture-level `SetUp` skips; per-test skips must name a verified
  blocker; CLAUDE.md adds "a skip that lingers after its blocker is
  gone is a review finding."
- Verified inventory: 97 `GTEST_SKIP` occurrences in e2e/ outside
  known_bugs_test, concentrated in `foreign_fn_type_matrix_test.cc`
  (45, all citing one of three named blockers B0/B1/B2 defined at
  :121-141), plus m5b (12), wkt_field_set (8), m8 (7),
  host_fn_type_matrix (7 — legitimate "no celfn IDL spelling for X"
  scope exclusions), m9 (5), m10 (5), and singletons.  Outside e2e:
  only 3 (eval/engine_test.cc:933, runtime/cel_list_test.cc:106,
  compiler/codegen/expr_lower_test.cc:600), each with a concrete
  reason.  No fixture-level `SetUp` skip found.
- known_bugs_test.cc is the structured home for *confirmed defects*
  (verify-first: candidates that didn't reproduce are recorded as
  deliberately absent, :524-528).  Non-eval findings (design-invariant
  breakages, doc inaccuracies, host/proto issues needing fixtures) live
  in `doc/implementation-plan/known-issues-findings.md` (header :1-8;
  session tally :231-244 — 27 encoded bugs, ~40 prose findings).

### 1.5 The gate and the manual-tag story

- `scripts/run_full_suite.sh` is the milestone-close gate: default
  suite + ALL manual-tagged tests + the dual-mode conformance gate.
  The manual list is **query-driven** (`bazel query 'attr(tags,
  "manual", tests(//...))'`, script :39-47) precisely because a
  hardcoded list rotted when the dual-link-mode macro renamed every
  e2e target to `<name>_{dynamic,static}` (comment :39-43).
- The "M2 incident" (per-component-test-coverage.md §0 :20-43) is the
  founding rationale: `bazel test //...` green while 29/44 e2e tests
  silently skipped behind a then-manual `//e2e:m2_test`.  Note the
  *current* mitigation differs from the doc's framing: most e2e suites
  are no longer manual at all (see §2 below).

## 2. Doc-vs-code discrepancies

1. **[P1] per-component-test-coverage.md §2 catalog names dead /
   re-tagged targets.**  The catalog (:89-101) lists `//e2e:m<N>_test`
   as a manual-tagged target and frames all e2e as excluded from
   `bazel test //...` "because they require external toolchains."
   Code: `link_mode_e2e_cc_test` emits only `m<N>_test_dynamic` /
   `m<N>_test_static` (e2e/link_mode_e2e_test.bzl:29-43) and does NOT
   tag them manual (e2e/BUILD.bazel — manual only on the four host-fn /
   foreign-component targets :641,674,697,721), so milestone e2e
   suites now run in the default suite.  §5's closeout block
   (`bazel test //e2e:m<N>_test passes`, :414) is uncopyable as
   written.  The `//e2e:eval_test` row is already self-flagged stale
   in-doc (:96).  run_full_suite.sh's query-driven list compensates,
   but the keystone gate doc misdescribes the load-bearing mechanism.
2. **[P1] Stale blocker text + stale skips: `Engine::AddComponent`.**
   `e2e/foreign_fn_type_matrix_test.cc:121-126` (`kBlockerB0`, cited
   by ~40 of its 45 skips) says "Engine::AddComponent currently
   returns Unimplemented".  Code: `eval/engine.cc:1518+` is a real
   implementation (validates bytes, conflict-checks overload-ids,
   parses the component), and `e2e/foreign_component_dispatch_test.cc`
   exercises it end-to-end.  The section comment at engine.cc:1500
   ("forward-declared, not yet wired") is likewise stale.  Per the
   skip discipline these are review findings: blocker gone (or
   morphed into "no per-decl component fixtures"), skips linger with
   the wrong reason.
3. **[P1] Stale M2-era skips contradicted by later suites.**
   `e2e/m2_test.cc:206,210` skip `IdentE2ETest::{String,Bytes}` as
   "needs host arena plumbing; deferred to M2.C", and
   `e2e/m4_test.cc:448-450` skips list<string> binding citing the same
   gap.  Code: string/bytes variables are bound and evaluated
   throughout `e2e/m5_test.cc:842-928` and `list<string>` bindings are
   exercised across `e2e/activation_boundary_test.cc:369-553` and
   `known_bugs_test.cc:651-692`.  The named blocker no longer exists.
4. **[P2] binding_marshal.h:91-96 says `ValueFromProto` is
   Unimplemented for "aggregate / enum / type_value"** — but enum and
   type_value ARE implemented (binding_marshal.cc:107-112, :127-133;
   the header's own supported-list at :13-17 says so).  Only the
   PopulateActivation comment is stale.
5. **[P2] conformance/README.md hand-prose vs autogen counts.**
   Headline says `fail=92` (:34, autogen) and the per-fixture table
   sums to 92; the hand-maintained FAIL-buckets prose says "93 FAILs
   across 14 fixtures" (:188-189).  Off by one.
6. **[P2] conformance/README.md duplicate/`stale` run instructions.**
   Two near-identical sections ("Running it" :9-30 and "Running"
   :41-60); the second uses `--file=tests/simple/testdata/...` —
   missing the `spec/` prefix used by `DefaultCorpus()`
   (run_conformance.cc:64-95) and the first section.
7. **[P2] run_full_suite.sh:18-21 claims `//...` "is unusable (dies on
   package loading)"** — contradicted by CLAUDE.md ("`bazel build
   //...` works", fixed via `.bazelignore`) and by the script's own
   `bazel query ... tests(//...)` at :45.
8. **[P2] feature-pipeline-checklist.md path rot.**  §1's pipeline map
   and §2 checklists cite pre-restructure paths: `api/value`,
   `api/compiler`, `api/internal/cel_host`, `common/type`,
   `compiler/e2e/` (e.g. :45-47, :124, :139, :169-171, :180) — now
   `eval/value`, `compiler/compiler`, `eval/internal/cel_host`,
   `shared/type`, top-level `e2e/`.  Same rot in
   testing-checklist.md:9-10 ("End-to-end tests live under
   `compiler/e2e/`").
9. **[P2] per-component-test-coverage.md §3.12 names the envelope
   filter `IsInM<N>Envelope`** (:361) — code is the un-versioned
   `IsInEnvelope` (runner.h:118).
10. **[P2] CLAUDE.md "The oracle has gaps — it has no activation
    bindings and no unknown-attribute path"** — code: both gaps were
    filled by `PartialEvalWithCelCpp` + `OracleVar`
    (cel_cpp_oracle.h:82-100), exercised in
    cel_cpp_oracle_test.cc:269-300.

## 3. Validation items

1. **Are the m2/m4 string-ident skips removable?**  Delete the
   `GTEST_SKIP` lines at e2e/m2_test.cc:206,210 and m4_test.cc:448,
   then `bazel test //e2e:m2_test_dynamic //e2e:m2_test_static
   //e2e:m4_test_dynamic` — green means three stale skips and the
   discrepancy-3 diagnosis is confirmed.
2. **How many foreign_fn_type_matrix B0 skips are now live?**  Remove
   the `GTEST_SKIP() << kBlockerB0;` from one scalar cell (e.g. the
   bool echo at :234) and run `bazel test
   //e2e:foreign_fn_type_matrix_test` — does it fail on empty
   `component_bytes` (InvalidArgument per engine.cc:1520-1523, i.e.
   blocked on fixtures, reason-text wrong) or pass (skip fully stale)?
3. **Do the dual-mode e2e targets actually run in the default suite?**
   `bazel query 'tests(//e2e/...) except attr(tags, "manual",
   tests(//e2e/...))'` and confirm `m2_test_dynamic` etc. appear; then
   `bazel test //e2e/...` to confirm they execute (not skipped via
   some .bazelrc filter).
4. **Is the conformance README in sync right now?**
   `scripts/regen_conformance_readme.sh --check` (after one
   `bazel run //conformance:run_conformance` to produce the log) —
   settles whether fail=92 or 93 and whether the prose drifted.
5. **Is the manual-target query complete?**  `bazel query 'attr(tags,
   "manual", tests(//...))'` — verify it returns the §1.1 list and
   nothing the per-component doc's catalog names that no longer
   exists.
6. **Does `//conformance:runner_test` run by default?**  `bazel query
   'attr(tags, "manual", //conformance:all)'` — only `run_conformance`
   appeared tagged manual (BUILD:165); confirm `runner_test` and
   `binding_marshal_test` are default-suite.
7. **Which of the 92 conformance FAILs have pinning tests?**  Cross-
   reference `bazel run //conformance:run_conformance --
   --max_fail_examples=100000` output against
   `grep -rn <expr> e2e/ conformance/` — the proto2-extension (20),
   parse (19), and enums (18) buckets appear to have no per-row
   pinning tests (README:192-207 cites only backlog numbers), which
   CLAUDE.md's "Conformance FAILs are bugs too" rule requires.

## 4. Test coverage observations

**Pinned well:**
- The conformance classifier itself is unit-tested (runner_test.cc:
  kind-only error matching :37-74, envelope membership :78-157,
  exhaustive SkipCategory names :161-174) — the harness's contract is
  regression-guarded independently of the corpus.
- Dual link-mode coverage is structural, not opt-in: every e2e source
  builds twice (bzl macro), the conformance gate runs twice, and
  `cctz_doubles_test` exists specifically as the forcing function for
  the static-mode `__wasm_call_ctors` hazard (e2e/BUILD.bazel:466-489).
- Boundary discipline is real: `activation_boundary_test` sweeps every
  variable-length Value kind across activation-buffer / arena /
  memory-capacity boundaries; known_bugs encodes the arena cliffs with
  exact byte arithmetic; the type-matrix suites
  (host_fn_type_matrix, foreign_fn_type_matrix) enumerate the full CEL
  type list per direction with explicit scope-exclusion skips.
- Bug lifecycle is executable: found → verified reproducing →
  GTEST_SKIP'd assertion in known_bugs_test → fix = delete one line.
  Fixed bugs remain as live guards in the same file.
- Partial-eval semantics are pinned against the RUNNING reference
  implementation (PartialEvalOracle tests), not source-reading.

**Gaps:**
- **No fuzzing anywhere** — no fuzz targets exist in any first-party
  BUILD file (grep over compiler/eval/runtime/e2e/conformance/abi/
  tools).  For a compiler accepting untrusted CEL and a wasm decoder
  (`abi_decode` parsing wire bytes), parser/codegen/decoder fuzzers
  are the standard missing layer.
- **92 conformance FAILs largely lack pinning tests** (validation
  item 7) — known_bugs encodes ~27 expressions; the proto2-ext /
  parse / enums buckets (57 rows) are tracked only as README prose.
- **`run_conformance` cannot fail CI** — the only enforcement is the
  monotonic PASS-count baseline; a change that converts a SKIP to a
  FAIL while another row graduates nets zero and passes the gate.
  README "Future work" (:211-215) names the fix (kFail==0 test or
  pinned per-fixture tuples) but it isn't built.
- **Oracle is sample-based, not corpus-wide** — `ExpectAgree` runs on
  ~25 hand-picked expressions; there is no bulk differential run of
  the conformance corpus through both engines (which would catch
  divergences the matcher-literal corpus misses).
- **Unknown matchers compare kind only** (runner.h:127-131) — a
  partial-eval result carrying the WRONG attribute set still passes
  conformance.
- **`m28_static_link_test` is a plain `cc_test`** (e2e/BUILD.bazel:
  221-233), outside the dual-mode macro — its static-vs-dynamic
  equivalence slice is small relative to the macro'd suites.
- **Three-way export-list drift** (cleanup-backlog #2): runtime
  exports exist in `engine.cc::kRuntimeExports`, runtime linkopts, and
  `wasm_imports.txt` with no consistency test.

## 5. Design decisions worth preserving

1. **SKIP is a code path with a verified reason, never an omission.**
   Every unsupported behavior is visible to grep as either a passing
   test or a `GTEST_SKIP` naming the concrete blocker with the
   un-skip recipe baked in (per-component doc §4; known_bugs_test
   header).  The M2 incident (29 silent fixture skips behind a manual
   tag) is the canonical counter-example and must stay in the docs.
2. **Manual-tag awareness is query-driven, not list-driven.**  The
   hardcoded manual list in run_full_suite.sh rotted on the first
   rename; `bazel query 'attr(tags, "manual", tests(//...))'` is the
   durable form (script :39-47).  Any new gate doc should cite the
   query, not target names.
3. **Classifier reads status payloads, not message substrings**
   (conformance/README "Classifier contract"; runner.cc:698-729).  New
   structural distinctions get a URL constant in
   `compiler/frontend/status_tags.h`, set at the producer, read at the
   consumer.
4. **Error matching is kind-only, by upstream precedent** (runner.cc:
   434-449).  Resist any future "match error text" proposal — corpus
   messages diverge from runtime payloads on non-semantic axes, and
   cel-cpp's own harness checks `has_error()` only.
5. **SkipCategory names are a public contract** — README tables and
   operator greps key on them; enum + name table + display order
   (run_conformance.cc:102-112) must move in one commit, and
   runner_test pins the strings exhaustively.
6. **The gate is monotonic-PASS, run in fastbuild, per link mode.**
   Fastbuild ≡ opt for pass counts (verified) and avoids a 10-minute
   config-switch rebuild on every push (check_conformance_monotonic.sh
   :81-89).  Dual baselines let each mode move independently.
7. **README conformance tables are generated, drift-gated artifacts**
   — AUTOGEN markers + `regen_conformance_readme.sh --check` in
   pre-push; the Disposition prose column is deliberately
   hand-maintained and preserved across regenerations (README:174-176).
8. **The oracle outranks source-reading**, and the differential test
   reuses the conformance comparator so "agrees with cel-cpp" and
   "passes conformance" are the same equality (cel_cpp_oracle_test.cc:
   100-118).  Eval errors are values, not harness failures, on both
   sides (oracle.cc:228-239; runner.cc:434-441).
9. **One bug = one runnable regression, verify-first.**  known_bugs
   only admits reproductions; claims that didn't reproduce are
   recorded as deliberately absent (:524-528).  Non-eval findings go
   to known-issues-findings.md so nothing is tracked only in
   chat/commit history.
10. **Dual-emission beats parameterization for build-level matrix
    axes.**  Link mode is a per-binary compile-time define
    (two targets), not a runtime test parameter — so a static-mode
    failure is attributable from the target name alone and the
    default suite covers both modes with zero per-test code.
11. **Shared corpus lists must converge.**  `SIMPLE_TESTDATA` (BUILD),
    `DefaultCorpus()` and `ForceLinkFixtureDescriptors` must be edited
    together today (run_conformance.cc:58-62; README "Future work");
    the planned single-`.inc` genrule is the right end state.
