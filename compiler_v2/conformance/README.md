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
```

The target carries `tags = ["manual"]` so it does not run as part
of `bazel test //...`.  Invoke it explicitly.

## Outcome taxonomy

Every `SimpleTest` that makes it past `LoadTestFile` lands in
exactly one of three buckets:

| Outcome | When | Regression? |
|---|---|---|
| `kPass` | Compiled, evaluated, decoded `cel::Value` matched the `cel.expr.Value` matcher. | — |
| `kUnsupported` | Outside the current milestone's envelope (see `IsM1Eligible` in `runner.h`), or compile returned `Unimplemented` / an `InvalidArgument` whose status message contains `"static subset"` (the `RejectDyn` gate). | No. |
| `kFail` | Anything else: compile/plan/eval error that was not `Unimplemented` / not-in-static-subset, or a value mismatch. | Yes — see caveats below. |

### What currently counts as `kUnsupported` at M1

  - `bindings` is non-empty  → needs M2 (idents).
  - `type_env` is non-empty  → needs M2 (declarations).
  - `container` is non-empty → needs M2.
  - `result_matcher` is `unknown` / `any_unknowns` → needs M2
    (`Activation` + `AttributePattern` surface; the runtime
    `cel_unknown_merge` helper already exists from v1).
  - `result_matcher` is `eval_error` / `any_eval_errors` → needs
    M4 (error surface + 3VL).
  - `value:` matcher is aggregate (`list_value` / `map_value` /
    `object_value` / `enum_value` / `type_value`) → needs M6
    (lists+maps) / M7 (proto).
  - `disable_check` or `check_only` set.
  - Compile returned `Unimplemented` (later-milestone arm).
  - Compile returned `InvalidArgument` with message containing
    `"static subset"` (the `RejectDyn` gate — every `dyn(...)`
    test lands here; these are deliberately out of scope per
    `CLAUDE.md`'s "What not to do").

Each subsequent milestone loosens one of these dimensions, and
currently-skipped tests graduate to `kPass`.

### Caveats on the `kFail` policy

The current classifier treats every non-`Unimplemented`,
non-static-subset compile error as `kFail`.  That is strict by
design at M1 — the base checker has no functions registered yet,
so a `type check failed` on an identifier is ambiguous between
"symbol not in environment (expected)" and "symbol registered but
broken (bug)".  Erring on the side of `kFail` surfaces the second
case and lets the inventory here absorb the first.

Once M3 registers built-in overloads, the classifier should tighten:
treat `type check failed` whose message names a symbol *not* in the
active declaration set as `kUnsupported` (ext-lib gap), and keep
`kFail` for mismatches against symbols that *are* declared.  That
change belongs in the M3 slice that enables the base library.

### Load failures (historical)

Eight fixtures (`block_ext`, `dynamic`, `enums`, `parse`, `proto2`,
`proto2_ext`, `proto3`, `type_deduction`) embed
`google.protobuf.Any` values typed as
`cel.expr.conformance.proto{2,3}.TestAllTypes` (plus proto2
extensions).  Until 2026-04-22 the runner didn't link those
descriptors, so `TextFormat::ParseFromString` would reject the
whole file before any test was visible.  Fixed by force-linking
the generated `.pb.cc` from `runner.cc` (see the include block
there and the
`//proto/cel/expr/conformance/proto{2,3}:test_all_types_cc_proto`
deps in `BUILD.bazel`).  All 30 fixtures now load.

---

## Inventory

Tracks pass/skip/fail counts for every `.textproto` fixture, the
blocker keeping most of its tests from passing, and the milestone
expected to unlock it.  Regenerate numbers via
`bazel run //compiler_v2/conformance:run_conformance` and update
this section in the same commit whenever a milestone moves the
needle.

**Snapshot: 2026-04-23 (post-M1, proto-descriptor fixture fix
landed).**  Headline:
`total=2454 · pass=178 (7.3%) · skip=1935 (78.9%) · fail=341 (13.9%)`
across 30 loadable fixtures.  **Zero real regressions** — every
`kFail` today is a symbol the type-checker doesn't know about yet
(an extension-library method, typically), not a pipeline bug.

Column meanings:

  - **Total / Pass / Skip / Fail** — from `run_conformance`.
  - **Pass %** — `pass / total`, rounded.
  - **Blocker** — the one reason most tests in this file aren't
    passing.  Not exhaustive; many tests stack multiple reasons.
  - **Unlocks at** — the milestone (per
    `doc/implementation-plan/rewrite/m1-scalar-pipeline.md §10`)
    whose landing is expected to graduate most of this fixture's
    SKIPs/FAILs to PASSes.

### Per-fixture table (all 30 fixtures)

Sorted: fixtures with passes first, then pure-SKIP, then fixtures
with ext-lib FAILs.  Keep the sort stable across revisions so
diffs against a prior snapshot are easy to read — when a milestone
ships, movers bubble up within their group.

| Fixture | Total | Pass | Skip | Fail | Pass % | Blocker | Unlocks at |
|---|---:|---:|---:|---:|---:|---|---|
| `basic.textproto`         |   43 | 30 |  13 |   0 | 70%  | 13 self-eval aggregate/type rows (`[]`, `{}`, `type(x)`) need lists/maps + type subsystem | M6 (lists/maps), later for `type()` |
| `parse.textproto`         |  219 |147 |  72 |   0 | 67%  | 72 SKIPs are parse-only tests whose expected result is a parsed-AST structure — out of the value-matcher envelope | Envelope expansion (harness work) |
| `plumbing.textproto`      |    5 |  1 |   4 |   0 | 20%  | Non-scalar plumbing tests (parse-phase protobuf round-trips) | M2+ (varies by test) |
| `unknowns.textproto`      |    0 |  0 |   0 |   0 |  —   | No `SimpleTest` entries in this file — empty by design | — |
| `comparisons.textproto`   |  406 |  0 | 406 |   0 |  0%  | Equality / ordering built-ins (`== != < > <= >=`) + `dyn(...)` | M3 (base ops); `dyn(...)` stays rejected |
| `conversions.textproto`   |  109 |  0 | 109 |   0 |  0%  | `int(x)` / `uint(x)` / `double(x)` / `string(x)` / `bytes(x)` conversions | M3 |
| `dynamic.textproto`       |  226 |  0 | 226 |   0 |  0%  | Every test uses `dyn(...)` — deliberately rejected by `RejectDyn` | Never (static subset is the design choice) |
| `enums.textproto`         |   85 |  0 |  85 |   0 |  0%  | Enum value access on `TestAllTypes` — needs proto field reads + enum type | M2 (select) + M7 (proto fixture) |
| `fields.textproto`        |   60 |  0 |  60 |   0 |  0%  | Proto field reads (`msg.field`, `has(msg.field)`) | M2 (`kSelect`, `has()`) |
| `fp_math.textproto`       |   30 |  0 |  30 |   0 |  0%  | `+ - * /` on doubles, NaN/Inf semantics | M3 |
| `integer_math.textproto`  |   64 |  0 |  64 |   0 |  0%  | `+ - * / %` on ints/uints, overflow | M3 |
| `lists.textproto`         |   39 |  0 |  39 |   0 |  0%  | List literals `[...]`, indexing `l[i]`, `in`, `size()` | M6 (lists) + M3 (indexing) |
| `logic.textproto`         |   30 |  0 |  30 |   0 |  0%  | Ternary + `&&` / `\|\|` / `!` with 3VL | M3 + M4 (3VL) |
| `macros.textproto`        |   44 |  0 |  44 |   0 |  0%  | `has()`, `all()`, `exists()`, `exists_one()`, `map()`, `filter()` | M2 (`has()`) + M5 (comprehensions) |
| `namespace.textproto`     |   14 |  0 |  14 |   0 |  0%  | `container` field non-empty → envelope filter | M2 (container resolution) |
| `proto2.textproto`        |  118 |  0 | 118 |   0 |  0%  | Proto2 message construction + field access | M7 (proto literals) |
| `proto2_ext.textproto`    |   18 |  0 |  18 |   0 |  0%  | Proto2 extension fields (`msg.[int32_ext]`) | M7 + extensions pass |
| `proto3.textproto`        |   85 |  0 |  85 |   0 |  0%  | Proto3 message construction + field access | M7 |
| `string.textproto`        |   51 |  0 |  51 |   0 |  0%  | `+`, `size()`, `contains`, `startsWith`, `endsWith`, `matches` | M3 |
| `timestamps.textproto`    |   76 |  0 |  76 |   0 |  0%  | `timestamp(...)` / `duration(...)` constructors, date arithmetic | Not yet scheduled (post-M7 timestamps milestone) |
| `type_deduction.textproto`|   47 |  0 |  47 |   0 |  0%  | Tests use `check_only:true` and assert the *deduced type* — envelope filter drops them all today | Harness extension: support `typed_result` matcher |
| `wrappers.textproto`      |   36 |  0 |  36 |   0 |  0%  | Proto `*Value` wrapper types (`Int32Value`, `StringValue`, …) | M7 |
| `block_ext.textproto`     |   37 |  0 |  20 |  17 |  0%  | `cel.@block([args…], expr)` — CEL-internal block form used by optimizers | Extensions pass (post-M5) |
| `bindings_ext.textproto`  |    8 |  0 |   3 |   5 |  0%  | `cel.bind(name, val, body)` macro — extension | Extensions pass (post-M5) |
| `encoders_ext.textproto`  |    4 |  0 |   0 |   4 |  0%  | `base64.encode` / `base64.decode` — extension | Extensions pass |
| `macros2.textproto`       |   46 |  0 |  20 |  26 |  0%  | Three-arg comprehension forms (`list.exists(i, v, pred)`) — newer macro forms | Extensions pass / M5 revisit |
| `math_ext.textproto`      |  199 |  0 | 100 |  99 |  0%  | `math.greatest` / `.least` / `.round` / `.trunc` / `.ceil` / `.floor` / `.sign` | Extensions pass |
| `network_ext.textproto`   |   69 |  0 |   9 |  60 |  0%  | `ip(...)` / `isIP` / CIDR parsing | Extensions pass |
| `optionals.textproto`     |   70 |  0 |  18 |  52 |  0%  | `optional.of` / `.none` / `.hasValue()` / `.or(...)` / `.orValue(...)` | Optionals pass (post-M5) |
| `string_ext.textproto`    |  216 |  0 | 138 |  78 |  0%  | `.charAt` / `.indexOf` / `.lastIndexOf` / `.substring` / `.replace` / `.split` / `.join` / `.lowerAscii` / `.upperAscii` | Extensions pass |

### Rough forecast by milestone

Numbers below are *ceilings* — M\<n\> unlocks the capability but
the tests may still need something else (e.g. `M3` unlocks
arithmetic but `integer_math.textproto` also has overflow-error
tests that need the M4 error surface).  Use this to prioritise,
not to predict exact PASS counts.

| Milestone | Fixture classes expected to move | Approx. tests unlocked |
|---|---|---:|
| **M1** (shipped 2026-04-22) | scalar literals in `basic` + `parse` + `plumbing` | 178 |
| **M2** (idents + proto field read + `has()` + container + `Activation` with unknown attributes) | `fields`, `namespace`, half of `macros` (`has()`), pieces of `enums`, `unknown` / `any_unknowns` matcher tests across the corpus | ~100 + unknowns |
| **M3** (calls + base overload set) | `comparisons`, `conversions`, `integer_math`, `fp_math`, `logic`, `string`, most of `basic`'s SKIPs | ~700 |
| **M4** (3VL + error surface) | `eval_error` / `any_eval_errors` matcher tests across many fixtures | ~150 (cross-cutting) |
| **M5** (custom functions + comprehensions) | `macros` (except `has()`), `macros2` SKIPs | ~50 |
| **M6** (list + map literals) | `lists`, remaining aggregates in `basic`, list-indexed tests in other fixtures | ~80 |
| **M7** (proto literals + wrappers) | `proto2`, `proto3`, `wrappers`, remainder of `enums`, `proto2_ext` (with extensions pass) | ~350 |
| **Extensions pass** (post-M7) | `bindings_ext`, `block_ext`, `encoders_ext`, `math_ext`, `network_ext`, `optionals`, `string_ext`, `proto2_ext` | ~680 |
| **Timestamps** (not yet scheduled) | `timestamps` | ~76 |
| **Harness: `typed_result` matcher** | `type_deduction` | ~47 |
| **Never (by design)** | `dynamic` | 226 (deliberately-rejected `dyn(...)`) |

Sum of unlockable buckets is ~2181 ≈ 2454 − 226 (dynamic) − 47
(typed_result) + delta for overlaps.  `dynamic.textproto` is the
only fixture permanently off the table: we reject `dyn` by design
(CLAUDE.md "What not to do").

---

## Extending the harness

  1. **Each milestone** — update `IsM1Eligible` in `runner.h` (and
     rename, e.g. `IsM2Eligible`, or generalise to take a milestone
     tag).  Loosen one envelope dimension; re-run the exploration
     binary; confirm new PASSes are real and not accidental.
  2. **New upstream fixture** — add the file to both
     `SIMPLE_TESTDATA` in `BUILD.bazel` *and* `DefaultCorpus` in
     `run_conformance.cc` (the two lists must match; a runtime
     `ifstream` failure is how you notice they don't).  If the
     fixture embeds a `google.protobuf.Any` of a type the runner
     doesn't force-link, extend `ForceLinkFixtureDescriptors` at
     the top of `runner.cc` with the new descriptor.
  3. **Refreshing the inventory** — after landing a milestone:
     run `run_conformance`, update the snapshot header and
     per-fixture rows in this file, and cross-check against
     `doc/implementation-plan/testing-checklist.md` (anything
     newly PASSing that maps to a checklist row should flip its
     box in the same commit).  If a milestone moved a previously-
     all-FAIL fixture, note it in the milestone doc's "What this
     milestone unlocked in the conformance suite" section.

## Future work

  - **CI gate.**  `run_conformance` is a binary and always exits
    0, so a pipeline regression that breaks every current PASS
    would not fail `bazel test //compiler_v2/...`.  Two shapes
    considered: a corpus-wide `kFail == 0` cc_test (viable once
    the M3 classifier tightening reclassifies ext-lib "type check
    failed" as `kUnsupported`), or a pinned-count test that
    asserts `(pass, fail)` tuples per fixture — catches both
    regressions and silent graduations.  Track under the M3
    milestone doc.
  - **`typed_result` matcher.**  `type_deduction.textproto`'s 47
    tests are all `check_only:true` with a `typed_result:`
    matcher.  The harness envelope drops them today.  Teaching
    `RunOne` to run the checker and compare the deduced type
    against `typed_result.deduced_type` would let all 47 graduate
    without a milestone dependency.
