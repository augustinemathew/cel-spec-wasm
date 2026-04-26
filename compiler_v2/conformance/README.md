# `compiler_v2/conformance/`

Harness for running upstream CEL conformance fixtures
(`tests/simple/testdata/*.textproto`) through the `compiler_v2`
pipeline (`cel::Compiler` → `cel::Engine::Plan` →
`cel::Instance::Eval`) and comparing the decoded `cel::Value`
against each test's `cel.expr.Value` matcher.

Single entry point — an exploration binary that walks the whole
corpus and prints a per-file tally:

```sh
bazel run //compiler_v2/conformance:run_conformance
```

No CI gate today.  Adding one is tracked under "Future work" below.

## Current state

`total=2454 · pass=664 (27.1%) · skip=1362 (55.5%) · fail=428 (17.4%)`
across 30 loadable fixtures.

The dominant remaining blockers, in approximate unlock order:

  - **M7** (proto literals + wrappers + message bindings).
    `proto2`, `proto3`, `wrappers`, the rest of `enums`,
    `fields.object_value` bindings.  Also unlocks the 67 SKIPs in
    `comparisons.textproto` that need `TestAllTypes{...}` literal
    construction (`kStruct` codegen) — message equality kernel is
    already shipped (M5.B step 2b), but no fixture row exercises
    it without first building a message literal.  Biggest absolute
    count if everything lights up.
  - **`has({...}.k)` bool-on-map dispatch** — 6 FAILs in `fields`.
    The dispatcher exists in M5.D step 2; the open issue is the
    bool-on-map operand path returning a kind the decoder doesn't
    recognise.
  - **`{kw}.kw` parse-time map-keyed-by-keyword** — 17 FAILs in
    `parse.textproto` `selectors/*` rows (`{ 'as': 1 }.as`); the
    map literal builds, the select returns `error` instead of the
    expected int.  Not yet root-caused — diagnostic candidate.
  - **`size('multibyte')` mismatch** — 2 FAILs in `string.textproto`
    (UTF-8 size returns codepoint count; matcher expects bytes).
    One-line fix once the spec is double-checked.
  - **`eval_error` cross-fixture matchers** — ~50+ SKIPs across
    `logic`, `integer_math`, parts of `comparisons` /
    `lists` / `fields` / `parse`.  The harness drops these; teaching
    `RunOne` to accept `eval_error` against a `Value::Error` result
    unblocks all of them.
  - **Comprehensions follow-on**.  `macros.textproto`,
    `macros2.textproto` three-arg forms.
  - **Extensions pass**.  `bindings_ext`, `block_ext`,
    `encoders_ext`, `math_ext`, `network_ext`, `optionals`,
    `string_ext`, `proto2_ext` — every test in these fixtures fails
    at "undeclared reference to `<extension symbol>`" today.
  - **Activation marshalling**: scalars + `kString` / `kBytes` /
    `kMessage` / `kList` ship.  Still SKIP at the encoder:
    `kMap`, `kDuration`, `kTimestamp`, `kEnum`, `kType`,
    `kUnknown`.  The `type_env: map_type` matcher (5 SKIPs in
    `fields.textproto`) is the most visible remaining gap.
  - **Static-subset rejection (`RejectDyn`)**: `dynamic.textproto`
    (226 tests) is permanently off the table by design.

`comparisons.textproto` is now FAIL-free (every cross-kind ordering
/ membership row that Slice 1.5 + 1.6 unlocked has graduated).

## Running

```sh
# Full corpus (all 30 fixtures):
bazel run //compiler_v2/conformance:run_conformance

# Single fixture:
bazel run //compiler_v2/conformance:run_conformance -- \
    --file=tests/simple/testdata/comparisons.textproto

# Show all failure details (default is 5 per file):
bazel run //compiler_v2/conformance:run_conformance -- \
    --max_fail_examples=1000

# Show SKIP reasons too — use this to diagnose why a fixture
# isn't budging.  Default is 0 (no skip details printed):
bazel run //compiler_v2/conformance:run_conformance -- \
    --file=tests/simple/testdata/comparisons.textproto \
    --max_skip_examples=200
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

What currently lands in `kUnsupported`:

  - `bindings:` value is `unknown:` / `error:` / aggregate
    (`list_value` / `map_value` / `object_value` / `enum_value` /
    `type_value`) — `binding_marshal::ValueFromProto` returns
    Unimplemented.
  - `type_env:` declares a non-scalar / non-string / non-bytes type.
  - `result_matcher` is `eval_error` / `any_eval_errors` (cross-
    cutting; needs harness work + cleaner 3VL surfacing).
  - `value:` matcher is `object_value` / `enum_value` /
    `type_value`.  `list_value` / `map_value` matchers DO compare
    today.
  - `disable_check` or `check_only` set.
  - Compile / eval returned `Unimplemented` — most commonly
    `expr_lower: expression kind 'struct' is not supported yet`
    (proto literal construction — M7); also `matches` regex,
    `int(x)` / `uint(x)` / `double(x)` / `string(x)` / `bytes(x)`
    conversions (overload set not seeded), comprehensions
    (`ResolvePass: comprehensions are M5 — reject until scope
    handling lands`).
  - Compile returned `InvalidArgument` containing `"static subset"`
    (the `RejectDyn` gate).

`bindings:` / `type_env:` / `container:` are NOT pre-filtered at
the envelope; the harness lets per-stage marshallers SKIP
gracefully, so a single fixture can mix in-envelope tests with
not-yet-supported ones.

## Per-fixture inventory

Sorted: highest pass % first, then pure-SKIP fixtures, then
ext-lib FAIL-dominated fixtures.

  - **Total / Pass / Skip / Fail** — from `run_conformance`.
  - **Pass %** — `pass / total`, rounded.
  - **Blocker** — the dominant reason most non-passing tests in
    this file aren't passing.
  - **Unlocks at** — the open milestone whose landing should
    graduate the most tests in this fixture.

| Fixture | Total | Pass | Skip | Fail | Pass % | Blocker | Unlocks at |
|---|---:|---:|---:|---:|---:|---|---|
| `fp_math.textproto`         |   30 |  29 |    1 |   0 | 97% | 1 SKIP is `mod_not_support` (`47.5 % 5.5`) — `disable_check:true` with `eval_error` matcher | (cross-cutting `eval_error`) |
| `basic.textproto`           |   43 |  37 |    6 |   0 | 86% | `[]` self-eval / `type(x)` (type subsystem) and message-typed shapes | M7 |
| `string.textproto`          |   51 |  40 |    9 |   2 | 78% | 9 SKIPs are all `matches` regex (deferred — no regex engine wired); 2 FAILs are `size('multibyte')` mismatches (size returns int, matcher expects bytes) | regex `matches` ext-lib |
| `parse.textproto`           |  219 | 157 |   45 |  17 | 72% | 34 SKIPs are envelope-rejected (`eval_error` matchers, `disable_check` receiver-function-name rows, parse-only AST matchers); 7 are `kStruct` (proto literals); 1 type_env list_type; 1 missing `uint64_to_int64` overload; 17 FAILs are string-keyed map self-eval | harness AST-matcher + `eval_error` matcher + M7 |
| `comparisons.textproto`     |  406 | 287 |  119 |   0 | 71% | 67 SKIPs need proto literals (`TestAllTypes{...}`) — codegen rejects `kStruct`; 28 are `dyn(aggregate)` deliberate rejections (Slice 1.5); 21 envelope-rejected (cross-numeric without dyn, mixed-type list literals); 3 are aggregate `type_env` declarations | M7 (proto literals) + harness type_env aggregate marshalling |
| `integer_math.textproto`    |   64 |  45 |   19 |   0 | 70% | All 19 SKIPs are `eval_error` matchers (int overflow / div-by-zero / mod-zero / unary-minus-on-uint) | harness `eval_error` matcher |
| `lists.textproto`           |   39 |  27 |   10 |   2 | 69% | 7 SKIPs are `eval_error` matchers (out-of-bounds index, bad index type); 3 are `dyn(aggregate)` rejections; 2 FAILs are bound-list operands | harness `eval_error` matcher + M5.D step 2 (bound-list ops) |
| `plumbing.textproto`        |    5 |   3 |    2 |   0 | 60% | Non-scalar plumbing (parse-phase protobuf round-trips) | M2+ (varies) |
| `logic.textproto`           |   30 |  16 |   14 |   0 | 53% | All 14 SKIPs are envelope-rejected: `eval_error` cross-fixture matchers + mixed-type ternary / type-mismatch in `&&`/`||` operands | harness `eval_error` matcher |
| `fields.textproto`          |   60 |  19 |   35 |   6 | 32% | 17 SKIPs are envelope-rejected (`eval_error` matchers, `disable_check`, disallowed-key-kind rows); 13 are `dyn(aggregate)`; 5 are `type_env: map_type`; 6 FAILs are `has({...}.k)` bool-on-map dispatch | harness `eval_error` matcher + map-type marshalling + M5.D step 2 |
| `namespace.textproto`       |   14 |   4 |   10 |   0 | 29% | Most SKIPs are comprehension-shaped (`[0].exists(y, ...)`) — comprehension lowering is the M5 follow-on; remainder is `disable_check` self-eval | Comprehensions follow-on |
| `unknowns.textproto`        |    0 |   0 |    0 |   0 |  —  | No `SimpleTest` entries (empty by design) | — |
| `conversions.textproto`     |  109 |   0 |  109 |   0 |  0% | `int(x)` / `uint(x)` / `double(x)` / `string(x)` / `bytes(x)` — overload set not seeded | M5.D step 2 (host conversions) |
| `dynamic.textproto`         |  226 |   0 |  226 |   0 |  0% | Every test uses `dyn(...)` aggregate — deliberately rejected by `RejectDyn` | Never (static subset) |
| `enums.textproto`           |   85 |   0 |   77 |   8 |  0% | Enum value access on `TestAllTypes` — proto field reads + enum subsystem.  8 FAILs hit "variable not bound" | M7 |
| `macros.textproto`          |   44 |   0 |   44 |   0 |  0% | 33 SKIPs are comprehension-shaped (`exists`/`all`/`exists_one`/`map`/`filter`); 6 envelope (`eval_error`/disable_check); 5 `dyn(aggregate)` rejections | Comprehensions follow-on |
| `proto2.textproto`          |  118 |   0 |  118 |   0 |  0% | Proto2 message construction + field access | M7 |
| `proto3.textproto`          |   85 |   0 |   85 |   0 |  0% | Proto3 message construction + field access | M7 |
| `timestamps.textproto`      |   76 |   0 |   76 |   0 |  0% | `timestamp(...)` / `duration(...)` constructors, date arithmetic | Timestamps slice (post-M7) |
| `type_deduction.textproto`  |   47 |   0 |   47 |   0 |  0% | All tests `check_only:true` with `typed_result:` matcher — envelope drops them | Harness: `typed_result` matcher |
| `wrappers.textproto`        |   36 |   0 |   36 |   0 |  0% | Proto `*Value` wrapper types | M7 |
| `proto2_ext.textproto`      |   18 |   0 |   18 |   0 |  0% | Proto2 extension fields (`msg.[int32_ext]`) | M7 + extensions pass |
| `bindings_ext.textproto`    |    8 |   0 |    0 |   8 |  0% | `cel.bind(name, val, body)` macro | Extensions pass |
| `encoders_ext.textproto`    |    4 |   0 |    0 |   4 |  0% | `base64.encode` / `base64.decode` | Extensions pass |
| `block_ext.textproto`       |   37 |   0 |   11 |  26 |  0% | `cel.@block([args…], expr)` — CEL-internal block form | Extensions pass |
| `macros2.textproto`         |   46 |   0 |    8 |  38 |  0% | Three-arg comprehension forms (`list.exists(i, v, pred)`) | Comprehensions follow-on |
| `network_ext.textproto`     |   69 |   0 |    9 |  60 |  0% | `ip(...)` / `isIP` / CIDR parsing | Extensions pass |
| `math_ext.textproto`        |  199 |   0 |  100 |  99 |  0% | `math.greatest` / `.least` / `.round` / `.trunc` / `.ceil` / `.floor` / `.sign` | Extensions pass |
| `optionals.textproto`       |   70 |   0 |    3 |  67 |  0% | `optional.of` / `.none` / `.hasValue()` / `.or(...)` / `.orValue(...)` | Optionals pass (post-M5) |
| `string_ext.textproto`      |  216 |   0 |  131 |  85 |  0% | `.charAt` / `.indexOf` / `.lastIndexOf` / `.substring` / `.replace` / `.split` / `.join` / `.lowerAscii` / `.upperAscii` | Extensions pass |

Sums (cross-check): pass = 664, skip = 1362, fail = 428, total = 2454.

## Forecast by remaining (open) milestone

Numbers below are *ceilings* — a milestone unlocks the capability
but the tests may still need something else.  Use this to
prioritise, not to predict exact PASS counts.

| Milestone | Fixture classes expected to move | Approx. tests unlocked |
|---|---|---:|
| **Harness: `eval_error` matcher** | All-SKIP `logic` / `integer_math` rows + cross-cutting in `fields` / `lists` / `parse` / `comparisons` | ~+50–80 |
| **Comprehensions follow-on** | `macros` (33), `macros2` three-arg forms, `namespace_shadowing/*` rows | ~+50–80 |
| **M7** (proto literals + wrappers + message bindings) | `proto2`, `proto3`, `wrappers`, remaining `enums`, the 67 message-eq rows in `comparisons`, aggregates in `basic`, `fields.object_value` bindings | ~+350 |
| **`kStruct` codegen alone** (subset of M7) | The 67 `Foo{...}` literal rows in `comparisons.textproto` — message equality kernel ships, just needs literal construction | ~+67 |
| **Extensions pass** | `bindings_ext`, `block_ext`, `encoders_ext`, `math_ext`, `network_ext`, `optionals`, `string_ext`, `proto2_ext` | ~+680 |
| **Timestamps** (not yet scheduled) | `timestamps` | ~76 |
| **Harness: `typed_result` matcher** | `type_deduction` | ~47 |
| **Map-type / aggregate `type_env` marshalling** | 5 SKIPs in `fields`, 1 in `parse`, 3 in `comparisons` | ~+10 |
| **`matches` regex helper** | 9 SKIPs in `string.textproto`'s `matches/*` section | ~+9 |
| **Classifier tightening** | Reclassifies most ext-lib FAILs (math/network/optionals/string-ext) as `kUnsupported` so `kFail==0` becomes a viable CI gate | 0 PASS, but unblocks CI |
| **Never (by design)** | `dynamic.textproto` | 226 (deliberately-rejected `dyn(...)` aggregate forms) |

## Extending the harness

  1. **Each milestone** — update the envelope check in `runner.h`
     (currently named `IsInM3Envelope` — rename forward or
     parameterise as the envelope keeps loosening).  Loosen one
     dimension; re-run; confirm new PASSes are real.
  2. **New upstream fixture** — add the file to both
     `SIMPLE_TESTDATA` in `BUILD.bazel` *and* `DefaultCorpus` in
     `run_conformance.cc` (the two lists must match).  If the
     fixture embeds a `google.protobuf.Any` of a type the runner
     doesn't force-link, extend `ForceLinkFixtureDescriptors` at
     the top of `runner.cc`.
  3. **Refreshing the inventory** — after landing a milestone:
     run `run_conformance`, update the headline + per-fixture rows
     in this file, and cross-check against
     `doc/implementation-plan/testing-checklist.md` (newly-PASSing
     rows should flip in the same commit).

## Future work

  - **CI gate.**  `run_conformance` is a binary that always exits
    0; a regression that breaks every current PASS would not fail
    `bazel test //compiler_v2/...`.  Two viable shapes: a
    corpus-wide `kFail == 0` cc_test (waits on classifier
    tightening), or a pinned-count test that asserts `(pass, fail)`
    tuples per fixture (catches both regressions and silent
    graduations).  Track under M5.H closeout.
  - **`typed_result` matcher.**  `type_deduction.textproto`'s 47
    tests are all `check_only:true` with a `typed_result:` matcher;
    teaching `RunOne` to compare the deduced type would unlock
    them without a milestone dependency.
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
