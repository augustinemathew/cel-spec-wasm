# `conformance/`

Harness that runs the upstream CEL conformance fixtures
(`spec/tests/simple/testdata/*.textproto`) through the celwasm
pipeline (`celwasm::Compiler::Compile` → `celwasm::Engine::Plan` →
`celwasm::Instance::Eval`) and compares the decoded `celwasm::Value`
against each test's `cel.expr.Value` matcher.

## Running it

```bash
# Full corpus, dynamic link mode (the default):
bazel run //conformance:run_conformance

# Static link mode (m28 merged-runtime Programs; ~25 min — every row
# pays the in-Compile Binaryen merge):
bazel run //conformance:run_conformance -- --link_mode=static

# Show every failure / skip in detail:
bazel run //conformance:run_conformance -- \
    --max_fail_examples=100000 --max_skip_examples=100000

# The pre-push gate — runs BOTH modes and enforces the monotonic
# pass-count baselines (conformance/.baseline, .baseline_static):
scripts/check_conformance_monotonic.sh
scripts/check_conformance_monotonic.sh --update   # bump baselines after a verified gain
```

Run from a warm fastbuild tree (see CLAUDE.md "Dev-loop performance");
pass counts are identical between fastbuild and -c opt.

<!-- BEGIN AUTOGEN headline -->
```
total=2516  pass=2035 (80.9%)  skip=481 (19.1%)  fail=0 (0.0%)
```
<!-- END AUTOGEN headline -->

`bazel test //conformance/...` does NOT exercise this — `run_conformance`
carries `tags = ["manual"]`.  Invoke it explicitly.

## Running

```sh
# Whole corpus, summary + per-fixture inventory + corpus-wide
# skip-by-category breakdown:
bazel run //conformance:run_conformance

# One fixture:
bazel run //conformance:run_conformance -- \
    --file=tests/simple/testdata/comparisons.textproto

# Dump every FAIL detail (default cap is 5 per file):
bazel run //conformance:run_conformance -- \
    --max_fail_examples=2000

# Dump every SKIP detail (default 0 — only the per-category
# counts are emitted unless you raise the cap):
bazel run //conformance:run_conformance -- \
    --max_skip_examples=2000
```

## Outcome taxonomy

Every row that loads classifies as exactly one of:

| Outcome | When | Regression? |
|---|---|---|
| `kPass` | Compiled, evaluated, and the decoded `cel::Value` matched the proto matcher. | — |
| `kUnsupported` (SKIP) | Outside the harness envelope — see categories below.  Tagged with a `SkipCategory` value so the run output can aggregate by tag. | No. |
| `kFail` | Compile / plan / eval failure not recognised as out-of-envelope, or a value mismatch. | Yes. |

## SKIP categories

Each `kUnsupported` row carries a `SkipCategory` enum (declared in
`runner.h`) — the names below are the textual surface emitted by
`SkipCategoryName(...)` and used in the per-fixture aggregator.
Adding a new category: extend the enum + the name table + the
display order in `run_conformance.cc::kCategories`, in the same
commit.

| Category | What it means | Source |
|---|---|---|
| `disable_check` | Row carries `disable_check: true`. **Out of conformance scope by design** — our pipeline is checker-passed only; supporting parse-only eval would require a separate codegen path. | `RunOne::ScopeReject` |
| `check_only` | Row carries `check_only: true` (`typed_result` matcher, no eval).  Harness follow-up — needs a `typed_result.deduced_type` comparator. | `RunOne::ScopeReject` |
| `envelope` | Matcher kind is not one the harness can compare today (e.g. `typed_result` with no embedded `result`, or `value:` with no kind set).  Detail names the offending matcher. | `EnvelopeRejectReason` |
| `static_subset` | Compile rejected by `RejectDyn` (`dyn(...)` aggregate or heterogeneous-typed expression).  Identified by the `kStaticSubsetViolationUrl` status payload set in `parse_and_check.cc`. | `ClassifyCompileFailure` |
| `compile_unimpl` | Compile returned `Unimplemented` — a stage stub for a later milestone (today: `matches` regex). | `ClassifyCompileFailure` |
| `eval_unimpl` | Eval / PartialEval returned `Unimplemented` from a runtime stage stub. | `ClassifyEvalFailure` |
| `ext_unimpl` | Compile failed because a cel-cpp extension library (`optionals`, `math_ext`, `network_ext`, `string_ext`, `encoders_ext`, `block_ext`) isn't registered.  Detected by either the `kUndeclaredReferencesUrl` payload's roots matching `ExtensionNamespaceRoots`/`ExtensionReceiverRoots`, OR the source expression containing an ext-lib-only syntax marker (`.?`, `[?`, `{?`, `cel.iterVar(`, `cel.index(`, `cel.block(`). | `ClassifyCompileFailure` |
| `type_env` | `binding_marshal` refused a `type_env` decl (aggregate / function / dyn type). | `RunOne` (type_env stage) |
| `bindings` | `binding_marshal` refused a `bindings:` entry (aggregate value, error / unknown ExprValue). | `RunOne` (bindings stage) |

## Classifier contract

Two parts of the contract are worth calling out so future changes
don't accidentally re-introduce substring-matching fragility:

  - **Status payloads, not message text.**  `parse_and_check.cc`
    attaches `absl::Status::SetPayload(URL, ...)` tags
    (`kStaticSubsetViolationUrl`, `kUndeclaredReferencesUrl` —
    declared in `compiler/frontend/status_tags.h`) so the
    harness classifies by tag, not by `absl::StrContains(msg,
    "static subset")` etc.  Adding a new structural distinction
    (e.g. an extension parse-syntax tag, or an enum-out-of-range
    tag): introduce a URL constant in `status_tags.h`, set the
    payload at the producer, read it at the consumer.
  - **Ext-lib detection is two-tier.**  Type-check-stage failures
    surface their undeclared-symbol roots via the
    `kUndeclaredReferencesUrl` payload — those classify directly
    by namespace / receiver match against `ExtensionNamespaceRoots`
    / `ExtensionReceiverRoots` in `runner.cc`.  Parser-stage
    failures land before the payload can be attached, so a
    source-expression fallback recognises the ext-lib-only syntax
    markers listed under `ext_unimpl` above.  Both routes
    converge on the same `SkipCategory::kExtensionUnimpl` tag.

## Per-fixture inventory

`pass / skip / fail` per fixture, sorted by pass rate.  The table
below and the corpus-wide SKIP table further down are auto-
regenerated by `scripts/regen_conformance_readme.sh` and gated by
the `.githooks/pre-push` hook — do not hand-edit between the
`AUTOGEN` markers.

<!-- BEGIN AUTOGEN per-fixture -->
| Fixture | Total | Pass | Skip | Fail | Pass% | Skip categories |
|---|---:|---:|---:|---:|---:|---|
| `bindings_ext.textproto`     |   8 |   8 |   0 |   0 | 100% | — |
| `celwasm_edges.textproto`    |  62 |  62 |   0 |   0 | 100% | — |
| `encoders_ext.textproto`     |   4 |   4 |   0 |   0 | 100% | — |
| `network_ext.textproto`      |  69 |  69 |   0 |   0 | 100% | — |
| `string.textproto`           |  51 |  51 |   0 |   0 | 100% | — |
| `timestamps.textproto`       |  76 |  76 |   0 |   0 | 100% | — |
| `conversions.textproto`      | 109 | 107 |   2 |   0 | 98% | disable_check=1 static_subset=1 |
| `math_ext.textproto`         | 199 | 194 |   5 |   0 | 97% | static_subset=5 |
| `fp_math.textproto`          |  30 |  29 |   1 |   0 | 96% | disable_check=1 |
| `integer_math.textproto`     |  64 |  61 |   3 |   0 | 95% | disable_check=3 |
| `lists.textproto`            |  39 |  36 |   3 |   0 | 92% | static_subset=3 |
| `parse.textproto`            | 219 | 201 |  18 |   0 | 91% | disable_check=17 type_env=1 |
| `comparisons.textproto`      | 406 | 354 |  52 |   0 | 87% | disable_check=21 static_subset=28 type_env=3 |
| `basic.textproto`            |  43 |  37 |   6 |   0 | 86% | disable_check=4 static_subset=2 |
| `macros.textproto`           |  44 |  38 |   6 |   0 | 86% | static_subset=6 |
| `macros2.textproto`          |  46 |  39 |   7 |   0 | 84% | static_subset=7 |
| `proto3.textproto`           |  85 |  72 |  13 |   0 | 84% | disable_check=6 static_subset=7 |
| `plumbing.textproto`         |   5 |   4 |   1 |   0 | 80% | disable_check=1 |
| `string_ext.textproto`       | 216 | 172 |  44 |   0 | 79% | disable_check=44 |
| `proto2.textproto`           | 118 |  93 |  25 |   0 | 78% | disable_check=6 static_subset=19 |
| `enums.textproto`            |  85 |  65 |  20 |   0 | 76% | disable_check=2 spec_unimpl=18 |
| `namespace.textproto`        |  14 |  10 |   4 |   0 | 71% | disable_check=4 |
| `logic.textproto`            |  30 |  21 |   9 |   0 | 70% | disable_check=9 |
| `dynamic.textproto`          | 226 | 134 |  92 |   0 | 59% | disable_check=20 static_subset=72 |
| `fields.textproto`           |  60 |  32 |  28 |   0 | 53% | disable_check=5 static_subset=15 type_env=8 |
| `wrappers.textproto`         |  36 |  18 |  18 |   0 | 50% | static_subset=18 |
| `type_deduction.textproto`   |  47 |  22 |  25 |   0 | 46% | check_only=25 |
| `optionals.textproto`        |  70 |  26 |  44 |   0 | 37% | static_subset=44 |
| `block_ext.textproto`        |  37 |   0 |  37 |   0 |  0% | ext_unimpl=37 |
| `proto2_ext.textproto`       |  18 |   0 |  18 |   0 |  0% | ext_unimpl=18 |
| `unknowns.textproto`         |   0 |   0 |   0 |   0 |  —  | (empty fixture) |
<!-- END AUTOGEN per-fixture -->

Corpus-wide SKIP totals (counts auto-regenerated; Disposition prose
is hand-maintained):

<!-- BEGIN AUTOGEN skip-totals -->
| Category | Count | Disposition |
|---|---:|---|
| `static_subset`  | 227 | Out-of-scope by design (`RejectDyn`). |
| `disable_check`  | 144 | Out-of-scope by design (parse-only eval). |
| `ext_unimpl`     |  55 | Scope-not-shipped — extensions pass (the largest single bucket; covers all of `math_ext` / `string_ext` / `optionals` / `network_ext` / `block_ext` / `encoders_ext` plus a handful of ext-shaped rows scattered through proto2/proto3/wrappers). |
| `check_only`     |  25 | Scope-not-shipped — `typed_result` `check_only:true` rows in `type_deduction.textproto`. |
| `spec_unimpl`    |  18 | Spec surface the reference implementation itself doesn't honour — strong-typed enums (`enums.textproto`); cel-cpp decays enums to plain `int`, so these rows fail against cel-cpp too.  Reclassified FAIL→SKIP with the parity citation in cleanup-backlog #39. |
| `type_env`       |  12 | Scope-not-shipped — `binding_marshal` doesn't yet decode aggregate `type_env` decls. |
| **Total**          | **481** | |
<!-- END AUTOGEN skip-totals -->

> Note: only the `Count` column of the SKIP-totals table is
> auto-regenerated.  The `Disposition` prose for each category is
> hand-maintained and preserved across regenerations.

<!-- BEGIN AUTOGEN addressable-prose -->
Of the 481 SKIPs: ~371 are out-of-scope by design
(`disable_check` + `static_subset`); the rest (110) are
scope-not-yet-shipped capabilities a future milestone will
graduate.  Effective pass rate against the addressable corpus
(2516 - 371 = 2145) is **95%**.
<!-- END AUTOGEN addressable-prose -->

## Top remaining FAIL buckets

**0 FAIL in the whole corpus** (both link modes).  The last FAIL —
`optionals/optional_ofNonZeroValue_struct_optional_ofNonZeroValue_map_optindex_field`,
the `optional.ofNonZeroValue(<message>)` zero-predicate trap — was
fixed 2026-06-10 by the `cel_host.cel_message_is_zero` probe
(cleanup-backlog #10, now closed; 1972 → 1973 PASS per mode).
(The same day's earlier burndown flipped 6 rows to PASS: the 4
mixed-origin map-equality rows — `proto{2,3}
set_null/map_{timestamp,duration}_null_pruned`, cleanup-backlog #12 —
and the 2 repeated-extension list-equality rows — `proto2
extensions_get/*_repeated_test_all_types`, the #40 remainder.)

## Future work

  - **CI gate.**  `run_conformance` itself exits 0 unconditionally,
    but `scripts/check_conformance_monotonic.sh` (the pre-push +
    CI gate) now enforces BOTH directions per link mode: pass-count
    floor (`conformance/.baseline{,_static}`) and FAIL-count ceiling
    (`conformance/.max_fail{,_static}`, currently 0) — a SKIP→FAIL
    conversion at flat pass count no longer slips through.  Possible
    later refinement: a pinned-`(pass, fail)`-per-fixture tuple test
    (catches silent graduations too).
  - **Single corpus list.**  `SIMPLE_TESTDATA` (BUILD), `DefaultCorpus()`
    (run_conformance.cc), and `ForceLinkFixtureDescriptors`
    (runner.cc) must currently be edited together when a fixture
    lands.  A genrule emitting a shared `.inc` would collapse
    them; until then the BUILD comment block notes the pairing.
  - **`typed_result.deduced_type` path.**  The eval-style
    typed-result cohort is wired; the 25 `check_only:true`
    rows in `type_deduction.textproto` still SKIP because the
    no-eval check path isn't wired.  Needs `cel::Ast::TypeMap`
    surfaced through the public `Compiler::Compile` API.
  - **Aggregate `bindings:` / `type_env:`.**  `list_value` /
    `map_value` bindings SKIP at marshal-time;
    `binding_marshal::ValueFromProto` hasn't been extended to
    the runtime aggregate types yet.  Unlocks ~8 SKIPs across `fields`
    and a few scattered rows.
    > Update 2026-05-22: the runtime-side encoder
    > (`instance.cc::EncodeMap`) shipped, so the bottleneck is
    > now purely `binding_marshal::ValueFromProto`'s aggregate
    > decoder.  Indexing / size / `in` operator on bound maps
    > works end-to-end (see `tools/cel/
    > activation_matrix_test.cc::BoundMap*`).  Comprehension
    > iteration over a bound list/map and over proto
    > repeated/map fields also ships in the same commit
    > (Slices 1+2 — `m5b-comprehensions-simplification.md`
    > §CCF-8 + `m5-comprehensions-followon.md` §10).
  - **`unknown:` ExprValue bindings.**  Refused because there's
    no per-test expr-id → `AttributeId` map.  Unlocks the
    `partial_*` sections embedded across multiple fixtures.
