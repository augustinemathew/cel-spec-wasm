# `compiler_v2/conformance/`

Harness for running upstream CEL conformance fixtures
(`tests/simple/testdata/*.textproto`) through the `compiler_v2`
pipeline (`cel::Compiler` → `cel::Engine::Plan` →
`cel::Instance::Eval`) and comparing the decoded `cel::Value`
against each test's `cel.expr.Value` matcher.

## Headline

`total=2454 · pass=921 (37.5%) · skip=843 (34.4%) · fail=690 (28.1%)`
across 30 loadable fixtures.  The most recent landings: M7.A–E proto
literal construction (+131), §4.5 encoder polish + null-clear (+27),
M7 envelope + matcher widen (+18), and proto2 unblock + CEL_MESSAGE
root decoder (+45).  See `doc/implementation-plan/rewrite/m7-proto-literals.md`
§9 for the M7 plan-vs-execution delta.  M9 (type subsystem) is being
scoped — its prompt at `doc/implementation-plan/rewrite/m9-type-subsystem-prompt.md`
targets the 255-row `envelope: type_value` bucket plus the 47-row
`typed_result` bucket, the single biggest scope-not-yet-shipped
category in the corpus.

## Running

```sh
# Full corpus (all 30 fixtures):
bazel run //compiler_v2/conformance:run_conformance

# Single fixture:
bazel run //compiler_v2/conformance:run_conformance -- \
    --file=tests/simple/testdata/comparisons.textproto

# Show all FAIL details (default is 5 per file):
bazel run //compiler_v2/conformance:run_conformance -- \
    --max_fail_examples=2000

# Show SKIP reasons (default is 0):
bazel run //compiler_v2/conformance:run_conformance -- \
    --file=tests/simple/testdata/comparisons.textproto \
    --max_skip_examples=2000
```

The target carries `tags = ["manual"]` so it does not run as part
of `bazel test //...`.  Invoke it explicitly.

## Outcome taxonomy

Every `SimpleTest` that makes it past `LoadTestFile` lands in
exactly one of three buckets:

| Outcome | When | Regression? |
|---|---|---|
| `kPass` | Compiled, evaluated, decoded `cel::Value` matched the `cel.expr.Value` matcher. | — |
| `kUnsupported` | Outside the current envelope, or compile/eval returned `Unimplemented` from a deliberately-stubbed code path, or `InvalidArgument` whose status message contains `"static subset"` (the `RejectDyn` gate). | No. |
| `kFail` | Anything else: compile/plan/eval error that was not `Unimplemented` / not-in-static-subset, or a value mismatch. | Yes. |

What currently lands in `kUnsupported` (per `runner.cc` + `binding_marshal.cc`):

  - `disable_check: true` or `check_only: true` rows (early-out in `RunOne`).
  - Matcher kind not in `IsInM7Envelope`: `type_value`,
    `typed_result`, or no matcher set.  All other matcher kinds —
    scalars, `list_value`, `map_value`, `object_value`, `enum_value`,
    `eval_error`, `any_eval_errors`, `unknown`, `any_unknowns` — are
    in scope and route to a comparator.
  - `binding_marshal::ValueFromProto` returns Unimplemented for
    `bindings:` whose value is `map_value`, `list_value`,
    `type_value`, or an `ExprValue.error` / `ExprValue.unknown`
    binding.  `object_value` and `enum_value` bindings DO marshal
    today (M7).
  - `binding_marshal::TypeSpecFragment` returns Unimplemented for
    `type_env:` decls whose type is `wrapper`, `well_known`,
    `list_type`, `map_type`, `function`, `abstract_type`,
    `null_type`, or `dyn` / `type` / `type_param` / `error`.
    Primitive scalars and `message_type` decls are in scope.
  - Compile returned `Unimplemented` — most commonly comprehensions
    (`ResolvePass: comprehensions are M5 — reject until scope
    handling lands`), `int(x)` / `uint(x)` / `double(x)` /
    `string(x)` / `bytes(x)` / `bytes(x)` conversions (overload
    set not seeded), `matches` regex, `timestamp(...)` /
    `duration(...)` constructors.
  - Compile returned `InvalidArgument` containing `"static subset"`
    (the `RejectDyn` gate).
  - Eval / PartialEval returned `Unimplemented`, OR a wasm trap
    whose message starts with `"stub:"` (Layer-2 trampoline stubs
    pending real bodies).

`bindings:` / `type_env:` / `container:` are NOT pre-filtered at
the envelope; per-stage marshallers SKIP gracefully so a single
fixture can mix in-envelope tests with not-yet-supported ones.

Comparator coverage in `runner.cc`: `CompareScalar` (null / bool /
int / uint / double / string / bytes), `CompareMap` (order-agnostic
per langdef §"Map equality"), `CompareList` (order-aware per langdef
§"List equality"), `CompareMessage` (Any-unpack +
`MessageDifferencer::Equals`), `CompareEnum` (int compare per
langdef §"Enumerated Types"), `CompareEvalError` (loose-message +
kind-only fallback per cel-cpp `conformance/run.cc`), `CompareUnknown`
(kind-only).

## SKIP-message taxonomy

Every SKIP row carries a stable `category: detail` prefix so the
output is greppable.  Categories (defined at the top of
`runner.cc`):

| Prefix | What it means | Source |
|---|---|---|
| `disable_check:` | Row carries `disable_check: true`.  **Out of conformance scope by design** — our pipeline runs cel-cpp's type-checker for every expression; supporting parse-only eval would require a separate codegen path with type-inference at lower time. | `RunOne` early-out |
| `check_only:` | Row carries `check_only: true` (`typed_result` matcher, no eval).  Harness follow-up — needs a `typed_result` comparator. | `RunOne` early-out |
| `envelope:` | The harness has no comparator for this row's `value:` matcher kind.  Almost entirely `type_value` matchers (`type(true)` whose expected result is the type-name string `"bool"`); also `typed_result:` rows in `type_deduction.textproto`.  Detail names the matcher kind verbatim. | `EnvelopeRejectReason` |
| `static_subset:` | Compile rejected by `RejectDyn` — `dyn(...)` aggregate or heterogeneous-typed expression.  Most of `dynamic.textproto`. | `ClassifyCompileFailure` |
| `compile unimplemented:` | Pipeline returned Unimplemented from a stage that's still stub.  Detail names the milestone (`expr_lower: kStructExpr` / `comprehensions are M5`). | `ClassifyCompileFailure` |
| `<stage> unimplemented:` | Eval / PartialEval returned Unimplemented from a runtime stage stub. | `ClassifyEvalFailure` |
| `<stage> trampoline stub:` | cel_host trampoline returned a `"stub:"` trap before the real body landed. | `ClassifyEvalFailure` |
| `type_env:` | binding-marshal rejected a `type_env` decl.  Detail forwards the marshal status. | `RunOne` per-stage |
| `bindings:` | binding-marshal rejected a bound value.  Detail forwards the marshal status. | `RunOne` per-stage |

Adding a new SKIP path: pick (or coin) a category prefix, document
it in `runner.cc`'s header block AND in this table, and ALWAYS use
the `category: detail` shape.

## Per-fixture SKIP breakdown by category

Counts come from grepping the live conformance output's
`category: detail` SKIP messages.  Use this table to answer "of
the N SKIPs in this fixture, how many are out-of-scope-by-design
vs how many are scope-not-yet-shipped?"

  - **`disable_check`** + **`static_subset`** + **`check_only`**
    are out-of-scope-by-design (parse-only eval / `dyn` rejection /
    no-eval check path).
  - **`envelope`** + **`compile unimpl`** + **`type_env`** +
    **`bindings`** are scope-not-yet-shipped (capabilities a
    future milestone graduates).

Fixtures with 0 SKIPs are omitted; see the inventory below for
those.  Sorted by total SKIP count descending.

| Fixture | Total | `static_subset` | `disable_check` | `envelope` | `compile unimpl` | `type_env` | `check_only` | other |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `string_ext.textproto`      | 122 |  0 | 44 | 78 |  0 | 0 |  0 | 0 |
| `conversions.textproto`     | 109 |  0 |  1 | 22 | 86 | 0 |  0 | 0 |
| `dynamic.textproto`         |  92 | 72 | 20 |  0 |  0 | 0 |  0 | 0 |
| `math_ext.textproto`        |  83 |  0 |  0 | 83 |  0 | 0 |  0 | 0 |
| `timestamps.textproto`      |  76 |  0 |  0 |  2 | 74 | 0 |  0 | 0 |
| `comparisons.textproto`     |  54 | 28 | 21 |  0 |  2 | 3 |  0 | 0 |
| `proto2.textproto`          |  49 | 19 |  6 | 18 |  6 | 0 |  0 | 0 |
| `type_deduction.textproto`  |  47 |  0 |  0 | 22 |  0 | 0 | 25 | 0 |
| `macros.textproto`          |  44 |  6 |  0 |  0 | 38 | 0 |  0 | 0 |
| `fields.textproto`          |  28 | 15 |  5 |  0 |  0 | 8 |  0 | 0 |
| `parse.textproto`           |  25 |  0 | 17 |  0 |  3 | 1 |  0 | 4 |
| `proto3.textproto`          |  19 |  7 |  6 |  0 |  6 | 0 |  0 | 0 |
| `proto2_ext.textproto`      |  18 |  0 |  0 | 18 |  0 | 0 |  0 | 0 |
| `wrappers.textproto`        |  18 | 18 |  0 |  0 |  0 | 0 |  0 | 0 |
| `enums.textproto`           |  17 |  0 |  2 | 12 |  3 | 0 |  0 | 0 |
| `namespace.textproto`       |  10 |  0 |  4 |  0 |  6 | 0 |  0 | 0 |
| `string.textproto`          |   9 |  0 |  0 |  0 |  9 | 0 |  0 | 0 |
| `logic.textproto`           |   9 |  0 |  9 |  0 |  0 | 0 |  0 | 0 |
| `basic.textproto`           |   6 |  2 |  4 |  0 |  0 | 0 |  0 | 0 |
| `integer_math.textproto`    |   3 |  0 |  3 |  0 |  0 | 0 |  0 | 0 |
| `lists.textproto`           |   3 |  3 |  0 |  0 |  0 | 0 |  0 | 0 |
| `fp_math.textproto`         |   1 |  0 |  1 |  0 |  0 | 0 |  0 | 0 |
| `plumbing.textproto`        |   1 |  0 |  1 |  0 |  0 | 0 |  0 | 0 |

Aggregated (corpus-wide):

| Category | Count | Disposition |
|---|---:|---|
| `envelope`        | 255 | Scope not yet shipped (matcher kind not yet handled — M9 target) |
| `compile unimpl`  | 233 | Scope not yet shipped (named milestone in detail) |
| `static_subset`   | 170 | Out-of-scope by design (`RejectDyn`) |
| `disable_check`   | 144 | Out-of-scope by design (parse-only eval) |
| `check_only`      |  25 | Scope not yet shipped (`typed_result` matcher — M9 target) |
| `type_env`        |  12 | Scope not yet shipped (binding-marshal aggregate types) |
| other             |   4 | Multi-line `expr:` rows the SKIP-output parser couldn't categorise (cosmetic) |
| **Total** | **843** | |

Of the 843 SKIPs, ~314 are out-of-scope-by-design (`disable_check`
+ `static_subset`) and ~525 are scope-not-yet-shipped (the rest).
The biggest scope-not-yet-shipped buckets are `envelope` (255) and
`compile unimpl` (233); the M9 type subsystem targets the bulk of
the envelope bucket plus the 25 `check_only` rows.

## Per-fixture inventory

Sorted by pass % descending, then pure-SKIP fixtures, then
ext-lib FAIL-dominated fixtures.

  - **Total / Pass / Skip / Fail** — from `run_conformance`.
  - **Pass %** — `pass / total`, rounded.
  - **Blocker** — the dominant reason most non-passing tests in
    this file aren't passing.
  - **Unlocks at** — the open milestone / follow-up whose landing
    should graduate the most tests in this fixture.

| Fixture | Total | Pass | Skip | Fail | Pass % | Blocker | Unlocks at |
|---|---:|---:|---:|---:|---:|---|---|
| `fp_math.textproto`         |  30 |  29 |   1 |   0 | 97% | 1 SKIP is `mod_not_support` — `disable_check:true`, out of conformance scope by design | Out-of-scope by design |
| `integer_math.textproto`    |  64 |  61 |   3 |   0 | 95% | 3 SKIPs are `disable_check:true` rows (`unary_minus_not_*`) — out of conformance scope by design | Out-of-scope by design |
| `lists.textproto`           |  39 |  34 |   3 |   2 | 87% | 3 SKIPs are `dyn(aggregate)` rejections; 2 FAILs are bound-list operands | M5.D step 2 (bound-list ops) |
| `basic.textproto`           |  43 |  37 |   6 |   0 | 86% | `[]` self-eval / `type(x)` (type subsystem) and message-typed shapes | M9 |
| `comparisons.textproto`     | 406 | 325 |  54 |  27 | 80% | 27 FAILs in the wrapper-equality (`eq_wrapper/*`) cohort gated on M8 | M8 (wrapper `==` peel) |
| `plumbing.textproto`        |   5 |   4 |   1 |   0 | 80% | 1 SKIP is parse-phase protobuf round-trip | M2+ (varies) |
| `parse.textproto`           | 219 | 174 |  25 |  20 | 80% | 17 SKIPs are `disable_check`-rejected receiver-function-name rows; 20 FAILs include keyword-keyed map self-eval | harness AST-matcher + classifier |
| `string.textproto`          |  51 |  40 |   9 |   2 | 78% | 9 SKIPs are all `matches` regex (deferred — no regex engine); 2 FAILs are `size('multibyte')` UTF-8 vs bytes mismatches | regex `matches` ext-lib |
| `logic.textproto`           |  30 |  21 |   9 |   0 | 70% | 9 SKIPs are all `disable_check:true` rows (parse-only conditional / AND / OR coercion tests) | Out-of-scope by design |
| `proto3.textproto`          |  85 |  52 |  19 |  14 | 61% | 14 FAILs split across wrapper-typed (M8), Any-pack (M7-future), enum-on-message-read | M8 + Any |
| `enums.textproto`           |  85 |  46 |  17 |  22 | 54% | 22 FAILs on dyn / wrapper / repeated-enum-as-object-value paths; 12 SKIPs are `type_value` envelope | M9 + classifier tightening + M8 |
| `proto2.textproto`          | 118 |  55 |  49 |  14 | 47% | 14 FAILs: ~9 wrapper-typed (M8), ~3 Any/Struct/Value pack (M7-future), ~2 misc; 18 SKIPs are `type_value` envelope | M9 + M8 + Any |
| `fields.textproto`          |  60 |  26 |  28 |   6 | 43% | 13 SKIPs `dyn(aggregate)`; 8 `type_env: map_type`; 5 `disable_check`; 6 FAILs are `has({...}.k)` bool-on-map dispatch | map-type marshalling + M5.D step 2 |
| `namespace.textproto`       |  14 |   4 |  10 |   0 | 29% | 6 SKIPs are comprehension-shaped (`[0].exists(y, ...)`); 4 are `disable_check` self-eval | Comprehensions follow-on |
| `wrappers.textproto`        |  36 |   9 |  18 |   9 | 25% | 9 PASSes via wrapper construction; 9 FAILs and 18 `static_subset`-classified SKIPs gate on M8 (wrapper `==` peel + scalar auto-wrap) | M8 |
| `dynamic.textproto`         | 226 |   4 |  92 | 130 |  2% | Every test uses `dyn(...)` aggregate — most rejected by `RejectDyn`; 130 FAILs are dyn-shaped construction reaching past the gate | Never (static subset) + classifier tightening |
| `unknowns.textproto`        |   0 |   0 |   0 |   0 |  —  | No `SimpleTest` entries (empty by design) | — |
| `conversions.textproto`     | 109 |   0 | 109 |   0 |  0% | `int(x)` / `uint(x)` / `double(x)` / `string(x)` / `bytes(x)` — overload set not seeded | M5.D step 2 (host conversions) |
| `macros.textproto`          |  44 |   0 |  44 |   0 |  0% | 38 SKIPs are comprehension-shaped (`exists`/`all`/`exists_one`/`map`/`filter`); 6 `dyn(aggregate)` rejections | Comprehensions follow-on |
| `timestamps.textproto`      |  76 |   0 |  76 |   0 |  0% | `timestamp(...)` / `duration(...)` constructors, date arithmetic | Timestamps slice (post-M7) |
| `type_deduction.textproto`  |  47 |   0 |  47 |   0 |  0% | All tests `check_only:true` with `typed_result:` matcher — envelope drops them | M9 (`typed_result` matcher) |
| `proto2_ext.textproto`      |  18 |   0 |  18 |   0 |  0% | Proto2 extension fields (`msg.[int32_ext]`) — `type_value` envelope SKIP | M9 + extensions pass |
| `bindings_ext.textproto`    |   8 |   0 |   0 |   8 |  0% | `cel.bind(name, val, body)` macro | Extensions pass |
| `encoders_ext.textproto`    |   4 |   0 |   0 |   4 |  0% | `base64.encode` / `base64.decode` | Extensions pass |
| `block_ext.textproto`       |  37 |   0 |   0 |  37 |  0% | `cel.@block([args…], expr)` — CEL-internal block form | Extensions pass |
| `macros2.textproto`         |  46 |   0 |   0 |  46 |  0% | Three-arg comprehension forms (`list.exists(i, v, pred)`); compile fails on undeclared three-arg form | Comprehensions follow-on |
| `network_ext.textproto`     |  69 |   0 |   0 |  69 |  0% | `ip(...)` / `isIP` / CIDR parsing | Extensions pass |
| `math_ext.textproto`        | 199 |   0 |  83 | 116 |  0% | `math.greatest` / `.least` / `.round` / `.trunc` / `.ceil` / `.floor` / `.sign` | Extensions pass |
| `optionals.textproto`       |  70 |   0 |   0 |  70 |  0% | `optional.of` / `.none` / `.hasValue()` / `.or(...)` / `.orValue(...)` | Optionals pass (post-M5) |
| `string_ext.textproto`      | 216 |   0 | 122 |  94 |  0% | `.charAt` / `.indexOf` / `.lastIndexOf` / `.substring` / `.replace` / `.split` / `.join` / `.lowerAscii` / `.upperAscii` | Extensions pass |

Sums (cross-check): pass = 921, skip = 843, fail = 690, total = 2454.

## Top remaining unlock buckets

Approximate PASS-impact ordering — see "Forecast by open milestone"
for ceilings.

  1. **Extensions pass** (~+680) — math/network/optionals/string-ext
     fixtures all fail at "undeclared reference to `<extension symbol>`".
     Whole next-tier milestone.
  2. **M9 type subsystem** (in-flight scoping; see
     `m9-type-subsystem-prompt.md`) — targets the 255-row `envelope:
     type_value` bucket plus the 25 `check_only` /
     `typed_result` rows in `type_deduction`.
  3. **Comprehensions follow-on** (~+50–80) — `macros` (38
     comprehension SKIPs), `macros2` three-arg forms (46 FAILs),
     `namespace` exists/all rows.
  4. **M8 wrappers** (~+50–60) — `wrappers.textproto` (27 non-
     passing rows) + the 27 `comparisons.eq_wrapper/*` FAILs +
     wrapper-typed field rows in `proto2`/`proto3`.
  5. **Timestamps slice** (~+76) — `timestamp(...)` / `duration(...)`
     constructors, date arithmetic.
  6. **Classifier tightening** — reclassifies most ext-lib FAILs
     (math/network/optionals/string-ext) as `kUnsupported` so
     `kFail==0` becomes a viable CI gate; 0 PASS impact but
     unblocks a corpus-wide regression test.
  7. **Map-type / aggregate `type_env` marshalling** (~+10) —
     8 SKIPs in `fields`, 1 in `parse`, 3 in `comparisons`.
  8. **`matches` regex helper** (~+9) — 9 SKIPs in
     `string.textproto`'s `matches/*` section.

## Forecast by open milestone

Numbers are *ceilings* — a milestone unlocks the capability but
the tests may still need something else.  Use this to prioritise,
not to predict exact PASS counts.

| Milestone | Fixture classes expected to move | Approx. tests unlocked |
|---|---|---:|
| **M8 wrappers** (auto-wrap on construction + wrapper-vs-scalar `==` peel) | `wrappers.textproto` (27 rows) + the 27 `comparisons.eq_wrapper/*` FAILs + wrapper-typed field rows in `proto2`/`proto3` | ~+50–60 |
| **M9 type subsystem** (in-flight scoping — `m9-type-subsystem-prompt.md`) | `envelope: type_value` SKIPs across `enums` (12), `proto2` (18), `proto2_ext` (18), `string_ext` (78), `math_ext` (83), `conversions` (22), `timestamps` (2), `type_deduction` (22) + the 25 `check_only` / `typed_result` rows | TBD (pending plan) |
| **Chained-null read fix** (cel-cpp's null-propagation through unset-message chains) | `empty_field/nested_message_subfield` rows in `proto2`/`proto3` | ~+2 |
| **`Any` packing** (M7-future) | `wrappers.textproto` to_any rows + downstream Any-comparison rows | ~+5–9 |
| **Enum-set-on-message diagnosis** | FAILs in `enums.textproto` `repeated_field_assign/*` + `single_field_assign/*` | ~+5–10 |
| **Comprehensions follow-on** | `macros` (38), `macros2` three-arg forms (46), `namespace_shadowing/*` rows | ~+50–80 |
| **Extensions pass** | `bindings_ext`, `block_ext`, `encoders_ext`, `math_ext`, `network_ext`, `optionals`, `string_ext`, `proto2_ext` | ~+680 |
| **Timestamps** (not yet scheduled) | `timestamps` | ~+76 |
| **Map-type / aggregate `type_env` marshalling** | 8 SKIPs in `fields`, 1 in `parse`, 3 in `comparisons` | ~+10 |
| **`matches` regex helper** | 9 SKIPs in `string.textproto`'s `matches/*` section | ~+9 |
| **Classifier tightening** | Reclassifies most ext-lib FAILs (math/network/optionals/string-ext) as `kUnsupported` so `kFail==0` becomes a viable CI gate | 0 PASS, but unblocks CI |
| **Never (by design)** | `dynamic.textproto` | ~217 (deliberately-rejected `dyn(...)` aggregate forms; 4 fold via M7.D const-rewrite) |

## Extending the harness

  1. **Each milestone** — update the envelope check in `runner.cc`
     (currently named `IsInM7Envelope` — rename forward or
     parameterise as the envelope keeps loosening).  Loosen one
     dimension; re-run; confirm new PASSes are real.
  2. **New upstream fixture** — add the file to both
     `SIMPLE_TESTDATA` in `BUILD.bazel` *and* `DefaultCorpus` in
     `run_conformance.cc` (the two lists must match).  If the
     fixture embeds a `google.protobuf.Any` of a type the runner
     doesn't force-link, extend `ForceLinkFixtureDescriptors` at
     the top of `runner.cc`.
  3. **New SKIP category** — add the prefix to the header block in
     `runner.cc`, document it in the SKIP-message-taxonomy table
     above, and use the `category: detail` shape at the call site.
  4. **Refreshing the inventory** — after landing a milestone:
     run `run_conformance` and `run_conformance --max_skip_examples=2000`,
     update the headline + per-fixture rows + SKIP-by-category
     table in this file, and cross-check against
     `doc/implementation-plan/testing-checklist.md`.

## Future work

  - **CI gate.**  `run_conformance` is a binary that always exits
    0; a regression that breaks every current PASS would not fail
    `bazel test //compiler_v2/...`.  Two viable shapes: a
    corpus-wide `kFail == 0` cc_test (waits on classifier
    tightening), or a pinned-count test that asserts `(pass, fail)`
    tuples per fixture (catches both regressions and silent
    graduations).
  - **`typed_result` matcher.**  `type_deduction.textproto`'s 47
    tests are all `check_only:true` with a `typed_result:` matcher;
    teaching `RunOne` to compare the deduced type would unlock
    them.  Tracked under M9.
  - **`unknown:` ExprValue bindings.**  The marshaller refuses
    `bindings:` whose value is an `unknown:` UnknownSet because
    there is no per-test expr-id → `AttributeId` map plumbed
    through `RunOne`.  Synthesising `AttributePattern`s from the
    bound name + per-test attribute IDs would unlock the
    `partial_*` sections embedded across several fixtures.
  - **Classifier tightening.**  Once the base library is fully
    seeded, the classifier should treat `type check failed` whose
    message names a symbol *not* in the active declaration set as
    `kUnsupported` (ext-lib gap), keeping `kFail` for mismatches
    against symbols that *are* declared.  Today the
    math/network/optionals/string-ext fixtures' FAILs are all
    ext-lib "undeclared reference" misses that should reclassify.
