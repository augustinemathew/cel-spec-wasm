# `compiler_v2/conformance/`

Harness for running upstream CEL conformance fixtures
(`tests/simple/testdata/*.textproto`) through the `compiler_v2`
pipeline (`cel::Compiler` → `cel::Engine::Plan` →
`cel::Instance::Eval`) and comparing the decoded `cel::Value`
against each test's `cel.expr.Value` matcher.

## Headline

`total=2454 · pass=1142 (46.5%) · skip=602 (24.5%) · fail=710 (28.9%)`
(post-M7B + polish, 2026-05-16; +84 PASS vs the M10-closeout 1058
baseline.  Polish round added WKT-coerce-on-bind, fixed-offset TZ
without sign prefix, sub-second-ms semantics for `getMilliseconds`,
and `Z`-suffix UTC formatting.)
across 30 loadable fixtures.  The most recent landings:

  - **M7-A.C cel_message_eq Any-peel** (2026-05-16): 0 conformance-row
    delta but unlocks direct Any-literal equality patterns
    (Any-vs-typed, Any-vs-Any cross-descriptor) that customers depend
    on per the user's "must work like cel-cpp" requirement.  The
    conformance rows that exercise this (comparisons.textproto
    eq_proto*_any_unpack_*) already passed via the outer
    MessageDifferencer path after M7-A.B; M7-A.C closes the
    direct-literal gap pinned by e2e tests in
    `AnyEqualityE2ETest::Direct*ViaPeel`.
  - **M7-A.B Any read-side unwrap** (2026-05-16): +3 PASS.
    `ProtoBacking::ReadField` now detects `google.protobuf.Any`-typed
    singular-message fields and unwraps via `UnpackAnyToValue`
    (type_url parse → pool lookup → ParseFromString) using the Any
    field's own descriptor pool.  Frontend §3.5.A select-through-Any
    carve-out admits the dyn-typed chained selects (`msg.single_any.x`)
    that cel-cpp accepts.  `wrappers.textproto :: */to_any` rows
    remain FAIL because they require M8's wrapper auto-unwrap (the
    inner step that turns `Int32Value{value:1}` into `1`).
  - **M7-A.A Any pack arm** (2026-05-16): +4 PASS.  `WriteMessageOrPack`
    helper threaded through 4 cpp_type-MESSAGE call sites (singular,
    repeated arena, repeated host, map host); `single_any /
    repeated_any / map_str_to_any` fixture fields added to HostMsg3.
  - **M10 type conversions** (slices A–E, 2026-05-14): +83 PASS.
    `bool` / `int` / `uint` / `double` / `string` / `bytes`
    inter-conversions + identity arms.  `conversions.textproto`
    moved from 0% → 91% pass.  See
    `doc/implementation-plan/rewrite/m10-conversions.md`.
  - **M9 type subsystem** (slices A–F, 2026-05-14): +54 PASS.
    `type(x)` standard function + type-identifier idents (`int`,
    `<message-FQN>`, ...) + `CEL_TYPE` equality + runner
    `kTypeValue` matcher graduation.  The 255-row
    `envelope: type_value` bucket dropped to 197 (rest pending
    Slice 3 classifier-tightening).  See
    `doc/implementation-plan/rewrite/m9-type-subsystem.md`.
  - **M7 proto literal construction** (slices A–E + §4.5 polish,
    2026-04-25): +221 PASS.  See `m7-proto-literals.md`.

Pre-M9+M10 headline (post-M7, 2026-04-25): `921 / 843 / 690`.
Pre-M7 headline (post-M5.D step 2, 2026-04-25): `664 / 1362 / 428`.

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
    handling lands`; 44 SKIPs across `macros` + `namespace`),
    `matches` regex (9 SKIPs in `string`), `timestamp(...)` /
    `duration(...)` constructors + arithmetic + accessors
    (~50 SKIPs across `timestamps` + scattered).  M10
    graduated the scalar conversion overload set
    (`int(x)` / `uint(x)` / `double(x)` / `string(x)` /
    `bytes(x)` etc.); the remainder of the SKIP set in
    `conversions.textproto` is the timestamp / duration
    conversion arms carved out per
    `m10-conversions.md` §2.2.
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

| Fixture | Total | `static_subset` | `disable_check` | `envelope` | `compile unimpl` | `type_env` | `check_only` |
|---|---:|---:|---:|---:|---:|---:|---:|
| `string_ext.textproto`      | 122 |  0 | 44 | 78 |  0 | 0 |  0 |
| `dynamic.textproto`         |  92 | 72 | 20 |  0 |  0 | 0 |  0 |
| `math_ext.textproto`        |  83 |  0 |  0 | 83 |  0 | 0 |  0 |
| `timestamps.textproto`      |   0 |  0 |  0 |  0 |  0 | 0 |  0 |
| `comparisons.textproto`     |  52 | 28 | 21 |  0 |  0 | 3 |  0 |
| `proto2.textproto`          |  49 | 19 |  6 | 18 |  6 | 0 |  0 |
| `type_deduction.textproto`  |  47 |  0 |  0 | 22 |  0 | 0 | 25 |
| `macros.textproto`          |  44 |  6 |  0 |  0 | 38 | 0 |  0 |
| `fields.textproto`          |  28 | 15 |  5 |  0 |  0 | 8 |  0 |
| `proto3.textproto`          |  19 |  7 |  6 |  0 |  6 | 0 |  0 |
| `proto2_ext.textproto`      |  18 |  0 |  0 | 18 |  0 | 0 |  0 |
| `wrappers.textproto`        |  18 | 18 |  0 |  0 |  0 | 0 |  0 |
| `parse.textproto`           |  18 |  0 | 17 |  0 |  0 | 1 |  0 |
| `namespace.textproto`       |  10 |  0 |  4 |  0 |  6 | 0 |  0 |
| `string.textproto`          |   9 |  0 |  0 |  0 |  9 | 0 |  0 |
| `logic.textproto`           |   9 |  0 |  9 |  0 |  0 | 0 |  0 |
| `basic.textproto`           |   6 |  2 |  4 |  0 |  0 | 0 |  0 |
| `conversions.textproto`     |   5 |  0 |  1 |  0 |  4 | 0 |  0 |
| `integer_math.textproto`    |   3 |  0 |  3 |  0 |  0 | 0 |  0 |
| `lists.textproto`           |   3 |  3 |  0 |  0 |  0 | 0 |  0 |
| `enums.textproto`           |   2 |  0 |  2 |  0 |  0 | 0 |  0 |
| `fp_math.textproto`         |   1 |  0 |  1 |  0 |  0 | 0 |  0 |
| `plumbing.textproto`        |   1 |  0 |  1 |  0 |  0 | 0 |  0 |

Aggregated (corpus-wide):

| Category | Count | Disposition |
|---|---:|---|
| `envelope`        | 197 | Scope not yet shipped (matcher kind not yet handled — remaining bucket targets the `enum` / `proto2_ext` / `string_ext` envelope tails and `type_deduction` typed-result rows that the M9.F harness graduation doesn't reach via the `check_only:true` early-out path) |
| `static_subset`   | 171 | Out-of-scope by design (`RejectDyn`) |
| `compile unimpl`  | 144 | Scope not yet shipped — top sub-buckets: 44 comprehensions (`ResolvePass: comprehensions are M5`), 9 `matches` regex, ~50 timestamp/duration construction + arithmetic + accessors, scattered remainder |
| `disable_check`   | 144 | Out-of-scope by design (parse-only eval) |
| `check_only`      |  25 | Scope not yet shipped (`typed_result` matcher with `check_only:true` — the M9.F runner change graduated `kTypedResult` for eval-style rows but the 25 `type_deduction` rows that combine it with `check_only:true` still early-out before reaching the comparator) |
| `type_env`        |  12 | Scope not yet shipped (binding-marshal aggregate types) |
| **Total** | **693** | |

Of the 693 SKIPs, ~315 are out-of-scope-by-design (`disable_check`
+ `static_subset`) and ~378 are scope-not-yet-shipped (the rest).
The biggest scope-not-yet-shipped buckets are `envelope` (197 —
dominated by extensions / proto2_ext, ext-libs likely move via the
extensions pass) and `compile unimpl` (144 — split across
comprehensions / regex / timestamps).

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
| `conversions.textproto`     | 109 |  99 |   5 |   5 | 91% | 4 of 5 SKIPs are timestamp / duration conversion arms (`int(timestamp)`, `string(duration)`, ...) carved out of M10 per §2.2; 5 FAILs are bool→int/uint/double rows that cel-cpp's checker doesn't declare (only its runtime registers them) | Timestamps slice + v2 checker extension |
| `lists.textproto`           |  39 |  34 |   3 |   2 | 87% | 3 SKIPs are `dyn(aggregate)` rejections; 2 FAILs are bound-list operands | M5.D step 2 (bound-list ops) |
| `basic.textproto`           |  43 |  37 |   6 |   0 | 86% | 6 SKIPs are `[]` / `{}` self-eval `disable_check`+ `static_subset` (out-of-scope by design) | — |
| `parse.textproto`           | 219 | 181 |  18 |  20 | 83% | 17 SKIPs are `disable_check`-rejected receiver-function-name rows; 20 FAILs include keyword-keyed map self-eval | harness AST-matcher + classifier |
| `comparisons.textproto`     | 406 | 327 |  52 |  27 | 81% | 27 FAILs in the wrapper-equality (`eq_wrapper/*`) cohort gated on M8 | M8 (wrapper `==` peel) |
| `plumbing.textproto`        |   5 |   4 |   1 |   0 | 80% | 1 SKIP is parse-phase protobuf round-trip | M2+ (varies) |
| `string.textproto`          |  51 |  40 |   9 |   2 | 78% | 9 SKIPs are all `matches` regex (deferred — no regex engine); 2 FAILs are `size('multibyte')` UTF-8 vs bytes mismatches | regex `matches` ext-lib |
| `logic.textproto`           |  30 |  21 |   9 |   0 | 70% | 9 SKIPs are all `disable_check:true` rows (parse-only conditional / AND / OR coercion tests) | Out-of-scope by design |
| `enums.textproto`           |  85 |  55 |   2 |  28 | 65% | M9 graduated 15 `type_value` envelope SKIPs (12 → 0); 28 FAILs persist on dyn / wrapper / repeated-enum-as-object-value paths | classifier tightening + M8 |
| `proto3.textproto`          |  85 |  53 |  19 |  13 | 62% | 13 FAILs split across wrapper-typed (M8) and enum-on-message-read; Any pack/unpack landed at M7-A | M8 |
| `proto2.textproto`          | 118 |  56 |  49 |  13 | 47% | 13 FAILs: ~9 wrapper-typed (M8), ~2 Struct/Value pack (M7-future), ~2 misc; 18 SKIPs still in `type_value` envelope; Any pack/unpack landed at M7-A | M8 |
| `fields.textproto`          |  60 |  26 |  28 |   6 | 43% | 13 SKIPs `dyn(aggregate)`; 8 `type_env: map_type`; 5 `disable_check`; 6 FAILs are `has({...}.k)` bool-on-map dispatch | map-type marshalling + M5.D step 2 |
| `namespace.textproto`       |  14 |   4 |  10 |   0 | 29% | 6 SKIPs are comprehension-shaped (`[0].exists(y, ...)`); 4 are `disable_check` self-eval | Comprehensions follow-on |
| `wrappers.textproto`        |  36 |   9 |  18 |   9 | 25% | 9 PASSes via wrapper construction; 9 FAILs and 18 `static_subset`-classified SKIPs gate on M8 (wrapper `==` peel + scalar auto-wrap) | M8 |
| `dynamic.textproto`         | 226 |   4 |  92 | 130 |  2% | Every test uses `dyn(...)` aggregate — most rejected by `RejectDyn`; 130 FAILs are dyn-shaped construction reaching past the gate | Never (static subset) + classifier tightening |
| `unknowns.textproto`        |   0 |   0 |   0 |   0 |  —  | No `SimpleTest` entries (empty by design) | — |
| `macros.textproto`          |  44 |   0 |  44 |   0 |  0% | 38 SKIPs are comprehension-shaped (`exists`/`all`/`exists_one`/`map`/`filter`); 6 `dyn(aggregate)` rejections | Comprehensions follow-on |
| `timestamps.textproto`      |  76 |  74 |   0 |   2 | 97% | 2 FAILs in the `*_range/sub_time_duration_*` cohort — `ts(year-1) - ts(year-9999)` = ±315.537B seconds, within proto-Duration's documented ±315.576B range but considered overflow by cel-cpp (uses a tighter timestamp-pair bound).  Other 74 rows pass post-M7B | M7B shipped 2026-05-16 + polish round |
| `type_deduction.textproto`  |  47 |  20 |  25 |   2 | 43% | M9.F's `kTypedResult` matcher graduates eval-style rows; the 25 remaining SKIPs are `check_only:true` typed-result rows that early-out before reaching the comparator | M9 follow-up (check_only typed-result path) |
| `proto2_ext.textproto`      |  18 |   0 |  18 |   0 |  0% | Proto2 extension fields (`msg.[int32_ext]`) — `type_value` envelope SKIP | extensions pass |
| `bindings_ext.textproto`    |   8 |   0 |   0 |   8 |  0% | `cel.bind(name, val, body)` macro | Extensions pass |
| `encoders_ext.textproto`    |   4 |   0 |   0 |   4 |  0% | `base64.encode` / `base64.decode` | Extensions pass |
| `block_ext.textproto`       |  37 |   0 |   0 |  37 |  0% | `cel.@block([args…], expr)` — CEL-internal block form | Extensions pass |
| `macros2.textproto`         |  46 |   0 |   0 |  46 |  0% | Three-arg comprehension forms (`list.exists(i, v, pred)`); compile fails on undeclared three-arg form | Comprehensions follow-on |
| `network_ext.textproto`     |  69 |   0 |   0 |  69 |  0% | `ip(...)` / `isIP` / CIDR parsing | Extensions pass |
| `math_ext.textproto`        | 199 |   0 |  83 | 116 |  0% | `math.greatest` / `.least` / `.round` / `.trunc` / `.ceil` / `.floor` / `.sign` | Extensions pass |
| `optionals.textproto`       |  70 |   0 |   0 |  70 |  0% | `optional.of` / `.none` / `.hasValue()` / `.or(...)` / `.orValue(...)` | Optionals pass (post-M5) |
| `string_ext.textproto`      | 216 |   0 | 122 |  94 |  0% | `.charAt` / `.indexOf` / `.lastIndexOf` / `.substring` / `.replace` / `.split` / `.join` / `.lowerAscii` / `.upperAscii` | Extensions pass |

Sums (cross-check): pass = 1058, skip = 693, fail = 703, total = 2454.

## Top remaining unlock buckets

Approximate PASS-impact ordering — see "Forecast by open milestone"
for ceilings.

  1. **Extensions pass** (~+680) — math/network/optionals/string-ext
     fixtures all fail at "undeclared reference to `<extension symbol>`".
     Whole next-tier milestone.
  2. **Comprehensions follow-on** (~+50–80) — `macros` (38
     comprehension SKIPs), `macros2` three-arg forms (46 FAILs),
     `namespace` exists/all rows.
  3. **M8 wrappers** (~+50–60) — `wrappers.textproto` (27 non-
     passing rows) + the 27 `comparisons.eq_wrapper/*` FAILs +
     wrapper-typed field rows in `proto2`/`proto3`.
  4. **Timestamps slice** (~+76) — `timestamp(...)` / `duration(...)`
     constructors, date arithmetic.  Also unblocks 4 of 5 remaining
     `conversions` SKIPs (timestamp / duration conversion arms
     carved out per `m10-conversions.md` §2.2).
  5. **Classifier tightening** (Slice 3 of
     `conformance-unlock-plan.md`) — reclassifies most ext-lib
     FAILs (math/network/optionals/string-ext) as `kUnsupported`
     so `kFail==0` becomes a viable CI gate; 0 PASS impact but
     unblocks a corpus-wide regression test.
  6. **M9 follow-up** (~+25) — the 25 `check_only:true`
     `typed_result` rows in `type_deduction`.  M9.F's
     `kTypedResult` matcher graduated eval-style rows
     (20 PASS); the `check_only` cohort needs the no-eval
     deduced-type comparison path.
  7. **Map-type / aggregate `type_env` marshalling** (~+12) —
     8 SKIPs in `fields`, 1 in `parse`, 3 in `comparisons`.
  8. **`matches` regex helper** (~+9) — 9 SKIPs in
     `string.textproto`'s `matches/*` section.
  9. **v2 checker extension for bool→{int,uint,double}
     conversions** (~+5) — cel-cpp's runtime declares the
     overloads but its checker doesn't; M10 dropped the rows
     rather than ship a v2-side extension.

## Forecast by open milestone

Numbers are *ceilings* — a milestone unlocks the capability but
the tests may still need something else.  Use this to prioritise,
not to predict exact PASS counts.

| Milestone | Fixture classes expected to move | Approx. tests unlocked |
|---|---|---:|
| **M8 wrappers** (auto-wrap on construction + wrapper-vs-scalar `==` peel) | `wrappers.textproto` (27 rows) + the 27 `comparisons.eq_wrapper/*` FAILs + wrapper-typed field rows in `proto2`/`proto3` | ~+50–60 |
| **Comprehensions follow-on** | `macros` (38), `macros2` three-arg forms (46), `namespace_shadowing/*` rows | ~+50–80 |
| **Timestamps** (shipped M7B, 2026-05-16) | `timestamps.textproto`: 0/76 → 74/76 (2 FAILs on proto-Duration boundary corners — see fixture row); scattered timestamp/duration `compile unimpl` SKIPs across `proto2` / `proto3` / `conversions` graduated.  Net corpus delta: pass 1058 → 1142 (**+84**). | shipped +84 |
| **Chained-null read fix** (cel-cpp's null-propagation through unset-message chains) | `empty_field/nested_message_subfield` rows in `proto2`/`proto3` | ~+2 |
| **Enum-set-on-message diagnosis** | FAILs in `enums.textproto` `repeated_field_assign/*` + `single_field_assign/*` | ~+5–10 |
| **Extensions pass** | `bindings_ext`, `block_ext`, `encoders_ext`, `math_ext`, `network_ext`, `optionals`, `string_ext`, `proto2_ext` | ~+680 |
| **M9 follow-up** (`check_only:true` + `typed_result:` matcher) | 25 SKIPs in `type_deduction.textproto` (the M9.F runner change graduated the eval-style typed-result cohort; the check-only cohort still early-outs) | ~+25 |
| **v2 checker extension** (bool → {int, uint, double} conversion overloads) | The 5 `conversions.textproto` FAIL rows that cel-cpp's runtime registers but its checker doesn't | ~+5 |
| **Map-type / aggregate `type_env` marshalling** | 8 SKIPs in `fields`, 1 in `parse`, 3 in `comparisons` | ~+12 |
| **`matches` regex helper** | 9 SKIPs in `string.textproto`'s `matches/*` section | ~+9 |
| **Classifier tightening** (Slice 3 of `conformance-unlock-plan.md`) | Reclassifies most ext-lib FAILs (math/network/optionals/string-ext) as `kUnsupported` so `kFail==0` becomes a viable CI gate | 0 PASS, but unblocks CI |
| **Never (by design)** | `dynamic.textproto` | ~217 (deliberately-rejected `dyn(...)` aggregate forms; 4 fold via M7.D const-rewrite) |

**Closed milestones** (no longer in this forecast):

  - **M7-A google.protobuf.Any** (shipped 2026-05-16 across slices
    A/B/C): +7 PASS.  Pack (WriteMessageOrPack), read-side unwrap
    (UnpackAnyToValue + frontend §3.5.A select-through-Any
    carve-out), equality peel (PeelAnyForEq).  Most `wrappers.textproto :: */to_any`
    rows still FAIL — they require M8's wrapper auto-unwrap.
  - **M7 proto literals** (shipped 2026-04-25 across slices A–E +
    §4.5 polish + envelope/matcher widen + proto2 unblock):
    +221 PASS.
  - **M9 type subsystem** (shipped 2026-05-14 across slices A–F):
    +54 PASS.  Graduated `type(x)` codegen + 58 of 255 `envelope:
    type_value` SKIPs + 20 of 47 `type_deduction` rows.
    Remainder of the envelope bucket is in ext-libs (M9 covers
    the spec types; ext-lib type names land with the extensions
    pass).
  - **M10 type conversions** (shipped 2026-05-14 across slices
    A–E): +83 PASS.  Graduated `conversions.textproto` from
    0% → 91%; remaining 5 SKIPs are timestamp/duration arms
    (`m10-conversions.md` §2.2 carve-out) and 5 FAILs are
    bool→{int,uint,double} (v2 checker extension above).

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
  - **`typed_result` matcher (`check_only:true` path).**  M9.F
    graduated the eval-style `kTypedResult` matcher cohort
    (20 of 47 rows in `type_deduction.textproto`).  The remaining
    25 SKIPs are `check_only:true` rows that early-out in
    `RunOne` before reaching the comparator; teaching the
    check-only branch to recover the deduced type from
    `cel::Ast::TypeMap` and compare against the matcher would
    unlock them.
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
