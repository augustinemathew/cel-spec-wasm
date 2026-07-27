# Test inventory, code coverage, and surface-area audit

Status: report — 2026-07-27, run against `ba28793` (master at the time of the
audit; PR #23 merge).

**Verdict: mixed.** The suite is large (181 test targets, 143 files, ~4.3k test
case macros, conformance at 2035/2516 with 0 FAILs) and the per-component
pairing discipline mostly holds. But (a) the e2e layer is organized by
milestone history rather than language surface, so finding "where is `X`
tested" requires a decoder ring; (b) several public API members have zero or
single-file coverage; (c) the two biggest first-party TUs
(`eval/internal/cel_host.cc`, `compiler/codegen/expr_lower_comprehension.cc`,
`runtime/cel_runtime.c`) have no same-named unit test; and (d) the
`testing-checklist.md` grids have drifted badly enough that they claim gaps
(list codegen, map e2e) that m4/m31 clearly closed.

Top three things to look at first:

  1. §5.1 — the P0-adjacent gaps: three open P0 bug pins, 17 unmigrated prose
     skips, and the `check_only`/`type_env`/`proto2_ext` conformance groups
     that are pure harness work (55 rows) rather than feature work.
  2. §2 — the coverage numbers, including the wasm-runtime caveat.
  3. §6 — the proposed language-surface reorganization of `e2e/`.

## Method

  - Inventory: `bazel query 'tests(//...)'` at `ba28793`, plus per-file
    `TEST*/GTEST_SKIP/CELBUG/CELSKIP` counts.
  - Coverage: `bazel coverage --combined_report=lcov` (gcc/gcov, fastbuild)
    over every test target except `//e2e/fuzz:cel_oracle_property_test` (the
    nightly divergence miner), **including all 22 `manual`-tagged targets**.
    Instrumentation was filtered to first-party packages.
  - **Wasm-side caveat.** gcov cannot instrument the wasm32-wasi
    cross-compile, so all `runtime/` numbers below are measured on the
    *native* build that the `runtime/*_test.cc` suites link. E2e tests that
    exercise the same C code through the compiled `cel_runtime.wasm` do not
    add to these percentages. Native line coverage is a faithful stand-in for
    statement reachability of the kernel logic, but wasm-only surfaces
    (`__wasm_call_ctors` ordering, import trampolines, `memory.grow`
    behaviour) are guarded only by the e2e/manual suites
    (`cel_runtime_wasm_test`, `cctz_doubles_test`, `memory_grow_stability_*`),
    not by these numbers.
  - The public-API symbol→test map was built by grepping every `*_test.cc`
    for each public class/method/field; the language-surface map from the
    conformance runner + corpus + `doc/langdef.md` + every `e2e/*_test.cc`.

## 1. Test inventory

### 1.1 Targets by package

181 test targets total; 66 of them in `//e2e` are 33 source files × 2 link
modes (`link_mode_e2e_cc_test` emits `_dynamic`/`_static` per source).

| Package | Targets | Notes |
|---|---:|---|
| `//e2e` | 66 | 33 files × 2 link modes (3 single-mode: `m28_static_link`, `proto_arena_lazy_copy`, `slot_aliasing`) |
| `//eval` | 33 | public-leaf tests + 20 `eval/internal` tests |
| `//runtime` | 30 | native-compiled kernel unit suites |
| `//compiler/codegen` | 7 | one per pass |
| `//abi` | 7 | + `//abi/internal:sha256_test` |
| `//tools/cel` | 5 | CLI |
| `//e2e/fuzz` | 5 | PBT harness self-tests + miner |
| `//compiler/celfn(+celfnc_emit)` | 6 | celfn parser/emitters |
| `//compiler` | 4 | `compiler_test` ×2 link modes, `program_test`, `memory_layout_test` |
| `//testdata` | 3 | the cel-cpp oracle |
| `//conformance` | 3 | runner unit tests + first-party-fixture differential |
| others | 12 | `frontend`, `ir`, `internal`, `shared`, `eval/host`, `abi/internal`, `wat_runner`, `benchmark`, `examples`, plugin demo ×2 |

### 1.2 Manual-tagged targets (22)

These do NOT run under a bare `bazel test //...` — they must be listed
explicitly (per `per-component-test-coverage.md` they carry load-bearing e2e
assertions):

`//compiler/celfn:{celfn_parser_probe_test,function_library_test}`,
`//e2e:{foreign_fn_type_matrix,host_fn,host_fn_type_matrix,plugin_dispatch}_test*`,
`//e2e/fuzz:cel_oracle_property_test`,
`//eval:{engine_test,instance_test,memory_grow_stability_test}_{dynamic,static}`,
`//eval:{host_trampoline_bounds,module_imports,required_fn_check}_test`,
`//runtime:cel_runtime_stripped_wasm_bytes_test`,
`//tools/cel:activation_matrix_test`, `//tools/wat_runner:wat_runner_test`.

Note that `engine_test` and `instance_test` — the tests of the two most
important public classes — are in this manual set. Anything that runs "the
default suite" and calls it green has not run them.

### 1.3 Case counts and skip discipline

143 test files, ~4,289 `TEST/TEST_F/TEST_P/TYPED_TEST` macros (parameterized
suites expand further at run time). Skip accounting:

  - 77 `GTEST_SKIP` sites total.
  - 18 CELBUG pins (the bug tracker): 3×P0, 14×P1, 1×P2 — see §5.1.
  - 27 CELSKIP blocks (14 by-design, 7 deferred-feature, 6 harness-limit).
  - **17 prose skips not yet migrated** (`scripts/bug_pins.py unmigrated`):
    10 in `e2e/foreign_fn_type_matrix_test.cc`, plus
    `eval/engine_test.cc:1190`, `eval/internal/cel_plugin_test.cc:6`,
    `e2e/plugin_dispatch_test.cc:699`, `e2e/slot_aliasing_test.cc:{53,57}`,
    `e2e/plugin_fixtures/.../demo_plugin_e2e_test.cc:{184,223}`.

### 1.4 Conformance corpus

31 fixture files (30 upstream `spec/tests/simple/testdata/*.textproto` + the
first-party `conformance/testdata/celwasm_edges.textproto`); nothing on disk
is excluded. Gate: `scripts/check_conformance_monotonic.sh`, baselines
`conformance/.baseline{,_static}` = **2035 pass**, `.max_fail{,_static}` =
**0**.

Headline (both link modes): `total=2516 pass=2035 (80.9%) skip=481 fail=0`.
Addressable pass rate (excluding 371 out-of-scope-by-design rows): **95%**.

Skip taxonomy: `static_subset` 227 (RejectDyn, by design), `disable_check`
144 (parse-only eval, out of scope by design), `ext_unimpl` 55 (37
`block_ext` + 18 `proto2_ext` — real gaps), `check_only` 25 (harness gap:
`deduced_type` path unwired), `spec_unimpl` 18 (strong-typed enums, mirrors
cel-cpp's own skip list), `type_env` 12 (marshal refuses aggregate decls —
harness gap).

### 1.5 The e2e decoder ring (milestone name → language surface)

| e2e file | What it actually tests |
|---|---|
| `mvp_concat_test` | string concat through the full WASI pipeline (smoke) |
| `m2_test` / `m2_partial_eval_test` | idents, proto field select, `has()`, Activation, unknowns / the full partial-eval propagation matrix |
| `m4_test` | list literals + indexing (+ rejection matrix) |
| `m5_test` | the built-in operator/overload set: arithmetic, comparison, string/bytes ops, `size`/`in`, polymorphic equality, 3VL |
| `m5b_test` | comprehensions (`all/exists/exists_one/map/filter`, two-var forms, `transformList/Map/MapEntry`) + `cel.bind` |
| `m7_test` / `m7a_test` / `m7b_test` | proto message literals / `google.protobuf.Any` / timestamp+duration |
| `m8_test` | wrapper WKTs (auto-wrap/unwrap) |
| `m9_test` | type subsystem (`type(x)`, type idents, type equality) |
| `m10_test` | scalar conversions (`int()`, `uint()`, `double()`, `string()`, `bytes()`, …) |
| `m12_test` / `m16_test` / `m17_test` / `m18_test` | string_ext / math_ext / encoders (base64) / network_ext (IP/CIDR) |
| `m14_test` | optionals (`.?`, `[?]`, `optional.*`, `or/orValue`, `optMap/optFlatMap`) |
| `m28_static_link_test` / `cctz_doubles_test` | static link mode plumbing / static-init (`__wasm_call_ctors`) forcing function |
| `m31_static_aggregate_test` | compile-time materialized const lists/maps |
| `m32_swisstable_index_test` | arena-map hash-index behaviour parity |
| `wkt_field_set_test` | WKT-valued proto fields (`Value`, `Struct`, Any, null-pruning) |
| `host_fn_test` / `host_fn_type_matrix_test` | `@host` custom functions (m21 adapter) + exhaustive type matrix |
| `foreign_fn_type_matrix_test` / `plugin_dispatch_test` | wasm-component foreign functions (m24) + plugin dispatch/verification (m35) |
| `known_bugs_test` | the divergence registry (CELBUG pins + fixed-bug regression rows) |
| `limits_test` | rodata window, parse nesting depth (loud-rejection pairs) |
| `activation_boundary_test` | activation marshal across buffer/arena capacity boundaries |
| `slot_aliasing_test` | slot-reuse stress (m1b discipline) |
| `program_roundtrip_test` | `Compile → bytes → Program → Plan → Eval` persistence |
| `arena_message_aggregate_eq_test` / `proto2_extension_list_eq_test` / `proto_arena_lazy_copy_test` | backlog-pin clusters (aggregate equality w/ messages, proto2 extensions, lazy-copy arena) |
| `optimize_test` | Binaryen `optimize_level` 0-vs-2 parity |
| `fuzz/*` | PBT generator + cel-cpp differential oracle |

## 2. Coverage numbers

> Pending: the `bazel coverage` run is in flight; this section is filled in
> by the follow-up commit on this branch.

## 3. Public API surface vs coverage

### 3.1 Visibility drift (CLAUDE.md vs BUILD reality)

The curated public list in CLAUDE.md says `//abi:*` and `//runtime:*` are
public, but the BUILD files narrow several: `abi/{wasm_binary, plugin_validate,
celfn_wire, program_facts}` and `runtime/{cel_layout, cel_map_hash}` are
`//:internal`. Also `//eval:host_callback` (the `HostCallback` typedef that
appears in public `Engine::AddFunction` signatures) carries no explicit
visibility and so defaults to internal while being required by a public
method. Either the CLAUDE.md list or the BUILD files should be corrected;
`host_callback` should be public.

### 3.2 Zero-coverage public surface

Public symbols with **zero** direct test references anywhere:

| Symbol | Header | Note |
|---|---|---|
| `Value::SharedMessageBacking()` | `eval/value.h` | used by production code (`instance.cc`, `cel_host.cc`) but never asserted directly |
| `Plugin::bytes()` | `abi/plugin.h` | only caller `eval/engine.cc:1734` |
| `AttributeQualifier::AsInt()` / `AsUint()` | `eval/attribute.h` | header says unconstructable today — candidates for deletion per the pre-1.0 rule rather than testing |
| `AttributeQualifier::operator<` / `Attribute::operator<` | `eval/attribute.h` | no ordering test at all |
| `RuntimeCatalogueTextproto()` | `abi/runtime_catalogue_textproto.h` | only consumed by `runtime_catalogue.cc` |
| `RequiredFn` (type name) / `VariableDeclaration` (type name) | `abi/program_facts.h`, `compiler/compiler.h` | fields are asserted, the types never named |

### 3.3 Single-file / thin coverage worth widening

  - `Engine::AddModule` and `Engine::Builder::EnableJitPerfMap` — only
    `eval/engine_test.cc`; no e2e exercises a user-supplied extra module.
  - `Instance::memory_size_bytes()` — only `eval/engine_test.cc`; the header
    itself flags it as a removal candidate. Decide (probe-or-delete rule).
  - `Value::StringView` / `Value::BytesView` — only `eval/value_test.cc`;
    nothing pins the aliasing/lifetime contract under Eval.
  - `Value::Type()` / `Value::AsType()` — no case in `eval/value_test.cc`
    itself (covered incidentally in m9/binding_marshal).
  - `Value::HostMap` / `SharedMapBacking` / `SharedListBacking` /
    `OwnedMessage` — single-file each.
  - `CelType::optional_element()` and `CelTypeKindName()` — only
    `shared/type_test.cc`.
  - `ErrorCode::{kTypeUnsupported,kDuplicateKey,kInvalidArgument}` — no case
    in `eval/error_test.cc` (covered only via instance/cel_host tests).
  - `CompilerOptions::container` — two positive references, no negative
    (bad-container) case.
  - `HostCallContext` — every method covered, but `ArgBytes`, `ArgDuration`,
    `ArgIsNull`, `ArgValue`, `ReturnBytes`, `ReturnNull`, `ReturnList`,
    `ReturnMap` sit at exactly 2 files.
  - `eval/attribute.h` — almost the entire surface is covered by
    `eval/attribute_test.cc` alone (acceptable for a leaf value type, but
    `Parse` round-trip is the only widely-exercised entry).

### 3.4 Internals without a paired test

Per the repo's own rule ("every individual source file gets its own
`_test.cc`"), the violations that matter, largest first:

| File | LOC | Current coverage story |
|---|---:|---|
| `compiler/codegen/expr_lower_comprehension.cc` | 1145 | no `_test.cc`; covered indirectly via `expr_lower_test`, e2e m5b/m14, oracle comprehension test |
| `runtime/cel_runtime.c` | 1812 | no same-named test; covered by `cel_data/cel_deep_eq/cel_aggregate_arena/cel_oom_poison` suites |
| `runtime/cel_string_format_render.cc` | 492 | folded into `cel_string_format_test` |
| `runtime/cel_map_index.c` | 209 | SwissTable index builder; covered via `cel_map_test`/`cel_map_hash_test`/e2e m32 |
| `runtime/strip_command_wrappers.cc` | 200 | Binaryen tool; output-verified only by `manual`-tagged `cel_runtime_stripped_wasm_bytes_test` |
| `eval/internal/{instance_impl,wasmtime_engine_state}.cc` | 29/26 | trivial; fine |

## 4. Language surface vs coverage

### 4.1 What is well covered

Every shipped surface has both an e2e file and (mostly) a conformance
fixture at 100% or near: string (100%), timestamps (100%), network_ext
(100%), encoders (100%), bindings_ext (100%), math_ext (97%), conversions
(98%), integer/fp math (95/96%), lists (92%), macros/macros2 (86/84%). The
runtime kernel suites give per-C-file positive+negative matrices for
arithmetic, comparison, conversion, 3VL, time, string ops/ext, optionals,
map/list, arena, OOM-poison.

### 4.2 The genuine language-surface gaps

Ordered by leverage (conformance rows unlocked or spec risk):

1. **`block_ext` (37 rows, 0% pass)** — `cel.block/index/iterVar` not
   registered. Whole extension absent.
2. **`proto2_ext` function forms (18 rows)** — operator-form extension reads
   work (`proto2_extension_list_eq_test`), but `proto.hasExt`/`proto.getExt`
   are unregistered (backlog #40 remainder).
3. **`check_only` rows (25)** — pure harness work: `deduced_type` needs
   `cel::Ast` type-map exposure through `Compiler::Compile`.
4. **Strong-typed enums (18 rows, `spec_unimpl`)** — mirrors cel-cpp's own
   skip list (backlog #39); the `enum` row of the per-type grid is the only
   fully-unticked type row.
5. **`type_env` aggregate decls (12 rows)** — `binding_marshal` refuses
   aggregate/function decls; ~8 more `fields` rows behind it.
6. **Front-end semantic checklist rows, all still unticked** (these are
   parser-level, cheap, and currently pinned nowhere):
   - precedence/associativity: `!` vs `&&`, `&&` vs `||`, `==` vs `&&`,
     unary-vs-binary minus, ternary right-associativity, relational vs logical;
   - literal forms: escapes (`\x41`, `☃`), raw strings, triple-quoted
     strings, `b"\xff"`, hex ints, scientific doubles, rejection of `-1u`;
   - member-access chains: `a.b.c` left-assoc, `a.b().c`, `has(a.b.c)`
     lowering only the outer select to test-only;
   - misc: line comments, reserved-word rejection, qualified type-name vs
     chained-select resolution.
7. **3VL absorption in non-absorbing ops (Slice F)** — rows 7-8, 14-17, 19,
   21 of the 22-row matrix still open.
8. **Partial-eval per-key granularity** — rejected at parse today (pinned as
   such); unknowns.textproto upstream is empty, so our own
   `m2_partial_eval_test` is the only spec surface. Keep it authoritative.

### 4.3 Checklist drift (doc bug, not code bug)

`testing-checklist.md`'s per-type and per-ExprKind grids claim `[ ]` for
list/map codegen+e2e, `kStructExpr` checker/codegen, `kListExpr`/`kMapExpr`
codegen/e2e — all of which m4/m7/m31 demonstrably cover. The grids were
never refreshed after the milestone sections were appended. Fixing the grids
should be part of the reorganization commit (§6), not left to drift further.

## 5. Missing test cases

### 5.1 Do these first (bugs and discipline, not new surface)

1. **Fix the three P0 pins** (per the repo rule, a P0 is fixed, never just
   pinned): CELW-0004 (map-key rounding >2^53 → spurious match), CELW-0010
   (`exists` short-circuits an ERROR/UNKNOWN accumulator), CELW-0012
   (`transformMapEntry` computed entry ABORTs the compiler).
2. **Migrate the 17 prose skips** to CELBUG/CELSKIP blocks so
   `bug_pins.py validate` sees them (10 are in
   `foreign_fn_type_matrix_test.cc`).
3. **Tighten the loose limit assertions** (backlog #48): assert message
   substrings/error codes in `DeepBracketInputFromSmallStackRejectsGracefully`
   and the two `limits_test.cc` map-error rows.

### 5.2 Public-API unit cases (cheap, mechanical)

  - `eval/value_test.cc`: add `Type()`/`AsType()` round-trip + wrong-kind
    negatives; `StringView/BytesView` lifetime-contract cases;
    `SharedMessageBacking/SharedListBacking/SharedMapBacking` shared-ownership
    cases (or delete the Shared* accessors if no embedder story needs them).
  - `eval/error_test.cc`: name-and-payload cases for `kTypeUnsupported`,
    `kDuplicateKey`, `kInvalidArgument`.
  - `eval/attribute_test.cc`: `operator<` ordering tables for
    `AttributeQualifier` and `Attribute` — or delete both operators plus
    `AsInt`/`AsUint` under the pre-1.0 breaking rule if nothing needs them.
  - `compiler_test.cc`: negative `CompilerOptions::container` case (bad
    container → which status?); a case that names `VariableDeclaration`.
  - `abi/plugin_test.cc`: `bytes()` round-trip (Load(bytes).bytes() == bytes).
  - `eval/engine_test.cc` → e2e: an `AddModule` end-to-end case (extra module
    imported by an expr program), currently unit-only.
  - Decide-or-delete: `Instance::memory_size_bytes()`,
    `RuntimeCatalogueTextproto()` direct test, `abi` internal-vs-public list.

### 5.3 Component tests for the unpaired TUs

  - `compiler/codegen/expr_lower_comprehension_test.cc` — lift the
    comprehension-lowering assertions (loop shape, accu slot discipline,
    absorption emit paths) out of e2e into IR-level tests; this is also where
    CELW-0010/0012 regression pins belong after the fix.
  - `runtime/cel_runtime_test.cc` (or split `cel_runtime.c` per backlog
    P9/#1791 first — the split is the better fix; a monolithic test would
    cement the monolith).
  - `runtime/cel_map_index_test.cc` — direct index-builder cases (threshold
    boundary, collision chains, tombstones if any) rather than only
    behaviour-parity via e2e.
  - `runtime/cel_string_format_render_test.cc` — direct render cases per
    directive × operand kind, especially the CELW-0008/0009 boundaries.

### 5.4 Language-surface additions (from §4.2)

  - A `frontend/parse_and_check_test.cc` block (or a new
    `e2e/syntax_test.cc`) for the precedence/literal-form/member-chain rows —
    these are one-liner cases; the whole checklist section is an afternoon.
  - Finish the Slice-F 3VL absorption matrix rows.
  - Harness: wire `deduced_type` (25 rows), aggregate `type_env`/`bindings`
    marshal (12+ rows) — these are conformance-harness tests, not product
    features, and they unlock more rows per hour than any feature work.
  - Feature: `proto2_ext` function forms, then `block_ext` (in that order —
    proto2_ext is 18 rows for two functions; block_ext is a lowering feature).

### 5.5 Wasm-only surfaces (coverage-invisible, keep explicitly listed)

Because native coverage can't see them, these must stay on the manual-run
closeout list: `cel_runtime_wasm_test` (wasm module exports/imports),
`cel_runtime_stripped_wasm_bytes_test`, `memory_grow_stability_*`,
`cctz_doubles_*` (static-init), `wat_runner_test` (WAT traces), the two
link modes of every e2e suite, and `optimize_test` (Binaryen parity).

## 6. Reorganization proposal

The problem: 20 of 33 e2e files are milestone-named. The mapping in §1.5 is
knowledge that currently lives nowhere in the tree; every new contributor (or
agent session) re-derives it. The suite should be organized by language
surface, mirroring the conformance corpus taxonomy, which is already the
lingua franca of CEL testing.

Proposed moves (pure renames — same TEST bodies, same
`link_mode_e2e_cc_test` targets; do it in one commit so blame stays usable,
and update `per-component-test-coverage.md` + the checklist in the same
commit):

| Current | New name |
|---|---|
| `m2_test.cc` | `ident_select_test.cc` |
| `m2_partial_eval_test.cc` | `partial_eval_test.cc` |
| `m4_test.cc` | `list_test.cc` |
| `m5_test.cc` | `operators_test.cc` |
| `m5b_test.cc` | `comprehension_test.cc` |
| `m7_test.cc` | `proto_literal_test.cc` |
| `m7a_test.cc` | `any_test.cc` |
| `m7b_test.cc` | `time_test.cc` |
| `m8_test.cc` | `wrapper_test.cc` |
| `m9_test.cc` | `type_value_test.cc` |
| `m10_test.cc` | `conversion_test.cc` |
| `m12_test.cc` | `string_ext_test.cc` |
| `m14_test.cc` | `optional_test.cc` |
| `m16_test.cc` | `math_ext_test.cc` |
| `m17_test.cc` | `encoders_ext_test.cc` |
| `m18_test.cc` | `network_ext_test.cc` |
| `m28_static_link_test.cc` | `static_link_test.cc` |
| `m31_static_aggregate_test.cc` | `static_aggregate_test.cc` |
| `m32_swisstable_index_test.cc` | `map_index_test.cc` |
| `mvp_concat_test.cc` | fold into `operators_test.cc` (it is 2 smoke cases) |

Notes:

  - There is no `map_test.cc` today: map-literal e2e lives split across
    m5/m31/known_bugs. Extract a `map_test.cc` in the rename commit so the
    list/map pair is symmetric.
  - Missing-by-name surfaces worth their own file as they grow:
    `enum_test.cc` (currently only oracle + conformance skips) and
    `syntax_test.cc` (the §4.2.6 precedence/literal rows).
  - Keep the non-milestone names as they are (`known_bugs`, `limits`,
    `activation_boundary`, `slot_aliasing`, `program_roundtrip`,
    `wkt_field_set`, `host_fn*`, `plugin_*`, `optimize`, `cctz_doubles` —
    maybe `static_init_test.cc` for the last).
  - Grep-ability rule going forward: e2e file names name the language
    surface; milestone docs cite file names, never the other way round. This
    also retires the §1.5 decoder ring.
  - In the same commit: refresh the stale grids in `testing-checklist.md`
    (§4.3) and re-point `per-component-test-coverage.md`'s manual-target
    catalog at the new names.

Not proposed: reorganizing `runtime/`, `eval/`, `compiler/` unit suites —
they are already organized by TU and that is correct for component tests.

## Future work

  - Wasm-side coverage: a `wasm-cov`-style pass (e.g. wasmtime's
    `--profile=guest` or instrumented builds) could close the "native
    coverage stands in for wasm" caveat; today the delta is guarded by the
    manual suites only.
  - The conformance runner counts rows, not branches; a per-fixture pinned
    `(pass, fail)` tuple test (README future-work item) would catch
    per-fixture regressions hidden by the aggregate baseline.
  - `unknowns.textproto` is empty upstream — consider contributing our
    partial-eval matrix upstream or vendoring a first-party unknowns fixture
    next to `celwasm_edges.textproto`.
