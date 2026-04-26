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

`total=2454 · pass=876 (35.7%) · skip=843 (34.4%) · fail=735 (29.9%)`
across 30 loadable fixtures.  M7.A–E shipped 2026-04-25
(`+131` PASS vs the M7-plan estimate of `+250`); §4.5 encoder
polish + null-clear shipped 2026-04-25 (`+27`, 831 → 858);
M7 envelope + matcher widen shipped 2026-04-25 (`+18`, 858 → 876
— but `~209` rows graduated SKIP→FAIL/PASS, surfacing previously-
hidden work).  See "Plan-vs-execution delta for M7" below.

The envelope-widen story: previously `IsInM7Envelope` SKIPped any
row whose value matcher was `object_value` / `enum_value` and any
row whose `type_env:` declared a `message_type` binding.  After
M7.A–E the codegen could handle those rows, but the harness never
ran them.  This commit:

  - admits `object_value` + `enum_value` matchers in
    `IsInM7Envelope`; `CompareValue` routes through new
    `CompareMessage` (Any-unpack + `MessageDifferencer::Equals`)
    + `CompareEnum` (int compare per langdef §"Enumerated Types") arms;
  - `binding_marshal::ValueFromProto` lights up `kObjectValue`
    (Any-unpack + `Value::OwnedMessage`) and `kEnumValue` (`Value::Int`);
  - `binding_marshal::TypeSpecFragment` + `CelTypeFromProtoType`
    light up `kMessageType` decls, emitting `name:<FQN>` for the
    spec parser;
  - `Value::OwnedMessage` impl moved out of value.cc's stub into
    cel_host.cc (wraps `unique_ptr<Message>` in `OwnedProtoBacking`);
  - shared `binding_marshal::UnpackAny` helper exposed via header.

The dominant remaining blockers, in approximate unlock order:

  - ~~**M7 read-side `Instance::Eval` encoder for `kHostList` /
    `kMap`**~~ — **shipped** as `DecodeHostListAt` /
    `DecodeHostMapAt` in `compiler_v2/api/instance.cc`.  Walks
    the per-Instance ExternrefTable backing via `ForEach`, wraps
    elements in a fresh vector-backed `Value::List` / `Value::Map`.
    Graduated 4 rows in `proto2.textproto`, 4 in `proto3.textproto`,
    2 in `enums.textproto` (the `empty_field/repeated_*` and
    `empty_field/map` cohort).  Same commit added a CEL_NULL arm
    to `SetScalarField`'s CPPTYPE_MESSAGE path so
    `Foo{m: null} == Foo{}` (clear-on-null-set per langdef +
    cel-cpp), graduating 4 `set_null/single_*` rows in each of
    proto2/proto3 (8 more).  Encoder polish + null-clear total:
    **+27 PASS**.
  - **M8 wrappers** (auto-wrap on construction + wrapper-vs-
    scalar `==` peel).  `wrappers.textproto` is now 18 SKIP / 18
    FAIL — the M7.A wrapper-message construction path admits
    `Int32Value{value: 5}` literal rows (so they reach FAIL
    instead of envelope-SKIP), but the `==` peel + auto-wrap
    paths required to make them PASS are M8.  Plus ~5 wrapper-
    typed `Foo{w: 5}` rows in `proto2`/`proto3` that scalar
    auto-wrap on construction unlocks.  See `m8-wrapper-types.md`.
  - **`Any` packing** (out of scope per `m7-proto-literals.md`
    §2.2).  `TestAllTypes{single_any: BoolValue{...}}`-shape rows
    — currently `kUnsupported` (descriptor-mismatch guard
    surfaces as Unimplemented).  ~5 rows across proto2/proto3.
  - **M3 read-side null propagation through chained selects**.
    `TestAllTypes{}.single_nested_message.bb` returns CEL_ERROR
    instead of null per langdef §"Field Selection".  M7.A fixed
    the immediate read (`{}.inner == null` → true), but chained
    selects through an unset message still error.  1 row in
    proto2 + 1 in proto3.
  - **`has({...}.k)` bool-on-map dispatch** — 6 FAILs in `fields`.
    The dispatcher exists in M5.D step 2; the open issue is the
    bool-on-map operand path returning a kind the decoder doesn't
    recognise.
  - **`{kw}.kw` parse-time map-keyed-by-keyword** — ~17 FAILs in
    `parse.textproto` `selectors/*` rows (`{ 'as': 1 }.as`); the
    map literal builds, the select returns `error` instead of the
    expected int.  Not yet root-caused — diagnostic candidate.
  - **`size('multibyte')` mismatch** — 2 FAILs in `string.textproto`
    (UTF-8 size returns codepoint count; matcher expects bytes).
    One-line fix once the spec is double-checked.
  - **Enum-set-on-message read paths in `enums.textproto`** — 12
    FAILs.  Pattern: `TestAllTypes{standalone_enum: ...}.standalone_enum`
    — construction works (M7.B/D); the read trips an envelope/decoder
    edge specific to enum field reads.  Diagnose post-§4.5.
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

`comparisons.textproto` graduated 47 rows during M7.A (the 67-SKIP
`kStruct` cohort the message-eq kernel had been waiting on); 18
FAILs remain, all in the wrapper-equality (`eq_wrapper/*`) cohort
gated on M8.

## Plan-vs-execution delta for M7

The `m7-proto-literals.md` plan estimated `+250` PASS for M7.A–E.
Actual landed: `+131`.  The `~119` gap maps cleanly to deferred
work, not to defects in M7 codegen / trampolines:

| Bucket | Rows | Status |
|---|---:|---|
| **§4.5 encoder polish** (`kHostList` / `kMap` in `Instance::Eval`) | ~32–48 | Punted from M7 landing; small follow-up |
| **M8 wrapper auto-wrap + `==` peel** | ~36 | Whole next milestone |
| **`Any` packing** | ~5 | Out of scope per `m7-proto-literals.md` §2.2 |
| **Chained-null read** + enum-on-message-read edges | ~14 | Diagnostic-shaped, post-§4.5 |
| Plan estimate ceiling overshoot in `enums` | ~30 | Plan §1 estimated `+50–70` for `enums`; M7.D landed +20 — the rest gates on the same encoder/null edges above |

What M7.A–E actually delivered, by fixture:

| Fixture | Pre-M7 | Post-M7 | Δ |
|---|---:|---:|---:|
| `proto2.textproto` | 0 | 29 | +29 |
| `proto3.textproto` | 0 | 26 | +26 |
| `comparisons.textproto` | 287 | 334 | +47 |
| `enums.textproto` | 0 | 20 | +20 |
| `dynamic.textproto` | 0 | 9 | +9 (`InlineConstantReferences` rewrite admits a few const-fold rows) |
| `wrappers.textproto` | 0 PASS / 0 SKIP / 36 FAIL→envelope | 0 PASS / 18 SKIP / 18 FAIL | rows graduated from envelope-skip to compile-FAIL — visible progress, M8-blocked at compile |

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
  - `value:` matcher is `object_value` / `enum_value` /
    `type_value`.  `list_value` / `map_value` matchers DO compare
    today.  `eval_error` / `any_eval_errors` matchers also compare
    today (kind-only — see "Outcome taxonomy" → `CompareEvalError`
    in `runner.cc`; mirrors cel-cpp's `conformance/run.cc` rule).
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
| `fp_math.textproto`         |   30 |  29 |    1 |   0 | 97% | 1 SKIP is `mod_not_support` (`47.5 % 5.5`) — `disable_check:true` with `eval_error` matcher (env gate ahead of envelope) | M5.D step 2 (host conversions) |
| `basic.textproto`           |   43 |  37 |    6 |   0 | 86% | `[]` self-eval / `type(x)` (type subsystem) and message-typed shapes | M7 |
| `string.textproto`          |   51 |  40 |    9 |   2 | 78% | 9 SKIPs are all `matches` regex (deferred — no regex engine wired); 2 FAILs are `size('multibyte')` mismatches (size returns int, matcher expects bytes) | regex `matches` ext-lib |
| `parse.textproto`           |  219 | 157 |   45 |  17 | 72% | 34 SKIPs are envelope-rejected (`disable_check` receiver-function-name rows, parse-only AST matchers); 7 are `kStruct` (proto literals); 1 type_env list_type; 1 missing `uint64_to_int64` overload; 17 FAILs are string-keyed map self-eval | harness AST-matcher + M7 |
| `comparisons.textproto`     |  406 | 334 |   54 |  18 | 82% | 47 of the 67 `kStruct` SKIPs graduated PASS via M7.A; remaining 18 FAILs are wrapper-equality (`eq_wrapper/*`) — the M8 peel surfaces these as compile-success / eval-FAIL today | M8 (wrapper `==` peel) |
| `integer_math.textproto`    |   64 |  61 |    3 |   0 | 95% | 16 of the 19 `eval_error` rows now PASS via M4 `CompareEvalError`; 3 SKIPs remain on `disable_check:true` rows (`unary_minus_not_*`) | M5.D step 2 (host overload set) |
| `lists.textproto`           |   39 |  34 |    3 |   2 | 87% | All 7 `eval_error` rows now PASS; 3 SKIPs are `dyn(aggregate)` rejections; 2 FAILs are bound-list operands | M5.D step 2 (bound-list ops) |
| `plumbing.textproto`        |    5 |   4 |    1 |   0 | 80% | 1 SKIP is parse-phase protobuf round-trip; the `error_result` `eval_error` row now PASSes | M2+ (varies) |
| `logic.textproto`           |   30 |  21 |    9 |   0 | 70% | All 5 `eval_error` rows now PASS (conditional / AND / OR `error_*`); 9 SKIPs remain on mixed-type ternary / type-mismatch operands | M5 follow-on (mixed-type 3VL) |
| `fields.textproto`          |   60 |  26 |   28 |   6 | 43% | 7 `eval_error` rows now PASS (no_such_key / duplicate_key / bad_key_type); 13 SKIPs remain `dyn(aggregate)`; 5 are `type_env: map_type`; 6 FAILs are `has({...}.k)` bool-on-map dispatch | map-type marshalling + M5.D step 2 |
| `namespace.textproto`       |   14 |   4 |   10 |   0 | 29% | Most SKIPs are comprehension-shaped (`[0].exists(y, ...)`) — comprehension lowering is the M5 follow-on; remainder is `disable_check` self-eval | Comprehensions follow-on |
| `unknowns.textproto`        |    0 |   0 |    0 |   0 |  —  | No `SimpleTest` entries (empty by design) | — |
| `conversions.textproto`     |  109 |   0 |  109 |   0 |  0% | `int(x)` / `uint(x)` / `double(x)` / `string(x)` / `bytes(x)` — overload set not seeded | M5.D step 2 (host conversions) |
| `dynamic.textproto`         |  226 |   9 |  175 |  42 |  4% | Every test uses `dyn(...)` aggregate — most rejected by `RejectDyn`.  The 9 PASSes graduated via M7.D's `InlineConstantReferences` rewrite (some `dyn(constant)` rows fold to a constant before the gate) | Never (static subset) |
| `enums.textproto`           |   85 |  36 |   17 |  32 | 42% | M7.D `InlineConstantReferences` + §4.5 encoder polish + envelope widen carried 36 rows.  32 FAILs surfaced from the envelope widen (previously SKIPped) — most are dyn / wrapper / object-value-matcher rows now reachable but failing on M8 / dyn-typed deps | Diagnose ext-lib gaps |
| `macros.textproto`          |   44 |   0 |   44 |   0 |  0% | 33 SKIPs are comprehension-shaped (`exists`/`all`/`exists_one`/`map`/`filter`); 6 envelope (`eval_error`/disable_check); 5 `dyn(aggregate)` rejections | Comprehensions follow-on |
| `proto2.textproto`          |  118 |  39 |   49 |  30 | 33% | M7.A–E + polish + envelope widen.  Of the 30 FAILs (envelope previously hid these as SKIP): wrapper-typed rows (M8), dyn-typed rows (static-subset rejection), Any packing (M7-future), and the chained-null read edge | M8 + classifier tightening |
| `proto3.textproto`          |   85 |  36 |   19 |  30 | 42% | Same shape as proto2 — 30 FAILs surfaced from envelope widen | M8 + classifier tightening |
| `timestamps.textproto`      |   76 |   0 |   76 |   0 |  0% | `timestamp(...)` / `duration(...)` constructors, date arithmetic | Timestamps slice (post-M7) |
| `type_deduction.textproto`  |   47 |   0 |   47 |   0 |  0% | All tests `check_only:true` with `typed_result:` matcher — envelope drops them | Harness: `typed_result` matcher |
| `wrappers.textproto`        |   36 |   0 |   18 |  18 |  0% | M7.A admits wrapper-type construction at the parse stage (rows graduated from envelope-skip to compile-FAIL); 18 FAILs all gate on M8 (wrapper `==` peel + scalar auto-wrap) | M8 |
| `proto2_ext.textproto`      |   18 |   0 |   18 |   0 |  0% | Proto2 extension fields (`msg.[int32_ext]`) | M7 + extensions pass |
| `bindings_ext.textproto`    |    8 |   0 |    0 |   8 |  0% | `cel.bind(name, val, body)` macro | Extensions pass |
| `encoders_ext.textproto`    |    4 |   0 |    0 |   4 |  0% | `base64.encode` / `base64.decode` | Extensions pass |
| `block_ext.textproto`       |   37 |   0 |   11 |  26 |  0% | `cel.@block([args…], expr)` — CEL-internal block form | Extensions pass |
| `macros2.textproto`         |   46 |   0 |    0 |  46 |  0% | Three-arg comprehension forms (`list.exists(i, v, pred)`); the 8 previously-SKIP `eval_error` rows now FAIL at compile (same root cause: undeclared three-arg form) | Comprehensions follow-on |
| `network_ext.textproto`     |   69 |   0 |    0 |  69 |  0% | `ip(...)` / `isIP` / CIDR parsing; 9 previously-SKIP `eval_error` rows now FAIL on the same undeclared-symbol error | Extensions pass |
| `math_ext.textproto`        |  199 |   0 |   83 | 116 |  0% | `math.greatest` / `.least` / `.round` / `.trunc` / `.ceil` / `.floor` / `.sign`; 17 previously-SKIP `eval_error` rows now FAIL on undeclared-symbol | Extensions pass |
| `optionals.textproto`       |   70 |   0 |    0 |  70 |  0% | `optional.of` / `.none` / `.hasValue()` / `.or(...)` / `.orValue(...)`; 3 previously-SKIP `eval_error` rows now FAIL on the same root cause | Optionals pass (post-M5) |
| `string_ext.textproto`      |  216 |   0 |  122 |  94 |  0% | `.charAt` / `.indexOf` / `.lastIndexOf` / `.substring` / `.replace` / `.split` / `.join` / `.lowerAscii` / `.upperAscii`; 9 previously-SKIP `eval_error` rows now FAIL on the same root cause | Extensions pass |

Sums (cross-check): pass = 876, skip = 843, fail = 735, total = 2454.

## Forecast by remaining (open) milestone

Numbers below are *ceilings* — a milestone unlocks the capability
but the tests may still need something else.  Use this to
prioritise, not to predict exact PASS counts.

| Milestone | Fixture classes expected to move | Approx. tests unlocked |
|---|---|---:|
| **M8 wrappers** (auto-wrap on construction + wrapper-vs-scalar `==` peel) | `wrappers.textproto` (36 rows) + the 18 `comparisons.eq_wrapper/*` FAILs + ~5 wrapper-typed field rows in `proto2`/`proto3` | ~+50–60 |
| **Chained-null read fix** (cel-cpp's null-propagation-with-default-instance through unset-message chains) | `empty_field/nested_message_subfield` rows in `proto2`/`proto3` | ~+2 |
| **`Any` packing** (M7-future) | downstream Any-comparison rows | ~+3–5 |
| **Enum-set-on-message diagnosis** | 10 FAILs in `enums.textproto` `repeated_field_assign/*` + `single_field_assign/*` | ~+5–10 |
| **Comprehensions follow-on** | `macros` (33), `macros2` three-arg forms, `namespace_shadowing/*` rows | ~+50–80 |
| **Extensions pass** | `bindings_ext`, `block_ext`, `encoders_ext`, `math_ext`, `network_ext`, `optionals`, `string_ext`, `proto2_ext` | ~+680 |
| **Timestamps** (not yet scheduled) | `timestamps` | ~76 |
| **Harness: `typed_result` matcher** | `type_deduction` | ~47 |
| **Map-type / aggregate `type_env` marshalling** | 5 SKIPs in `fields`, 1 in `parse`, 3 in `comparisons` | ~+10 |
| **`matches` regex helper** | 9 SKIPs in `string.textproto`'s `matches/*` section | ~+9 |
| **Classifier tightening** | Reclassifies most ext-lib FAILs (math/network/optionals/string-ext) as `kUnsupported` so `kFail==0` becomes a viable CI gate | 0 PASS, but unblocks CI |
| **Never (by design)** | `dynamic.textproto` | ~217 (deliberately-rejected `dyn(...)` aggregate forms; 9 fold via M7.D const-rewrite) |

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
