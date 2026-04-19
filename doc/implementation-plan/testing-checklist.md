# Testing checklist

Compilers fail silently.  The grid below is the minimum coverage the project
must keep green; tick items off as the corresponding `cc_test` lands.
Negative tests are as important as positive ones — every row needs both a
"this works" and a "this fails with a good message".

Conventions:
  - `compiler/<path>_test.cc` is Google Test (`@com_google_googletest//:gtest_main`).
  - End-to-end (wasm-executing) tests live under `compiler/e2e/`.
  - Each box is `[ ]` when pending, `[x]` when a committed test covers it.

## Gap summary (updated 2026-04-19)

The detailed grids below have the authoritative per-cell state; this
block is the triage view a reviewer can read in ten seconds.

**Closed by M2:**
  - `kConstant` + `kCallExpr` (global) across parser / checker /
    annotations / RejectDyn / codegen / e2e for `bool` / `int` /
    `uint` / `double`.
  - Short-circuit `&&` / `||` / `?:` — codegen + e2e.
  - Runtime allocator + every scalar `cel_make_*` constructor.
  - The CLI (`--emit_wasm` positive + negative).
  - Per-type `RejectDyn` acceptance for every primitive / wrapper /
    `null_type` / `timestamp` / `duration` / `any`
    (`static_subset_test::{AcceptsEveryPrimitiveAtRoot,
    AcceptsEveryPrimitiveWrapperAtRoot,
    AcceptsNullTimestampDurationAny}`).
  - `kSelectExpr (test_only, from has())` — parser / checker /
    annotations row closed via
    `parse_and_check_test::HasMacroLowersToTestOnlySelectExpr`; the
    `RejectDyn` row closed via
    `static_subset_test::{TestOnlySelectExprIsAcceptedWhenOperandTyped,
    DynOperandInTestOnlySelectIsRejected}`.  Codegen + e2e remain
    deferred until M3 wires up proto fields and strings.
  - Codegen negative tests now assert `HasSubstr` on the diagnostic
    message so a regression that collapses distinct Unimplemented
    strings into one generic blurb fails
    (`expr_lower_test::{ListExprIsUnimplementedWithListRepr,
    MapExprIsUnimplementedWithMapRepr,
    StringConstantIsUnimplementedWithStringRepr,
    IdentifierIsUnimplementedWithKindAndId}`).
  - Per-op e2e comparison coverage for `uint` and `double`, plus
    `double` negate (`eval_test::{UintComparisons, DoubleComparisons,
    DoubleNegate}`).

**Still open inside M2** (the gating items for calling M2 done):
  - `cel_ref_intern` / `cel_ref_get` / `cel_refs_reset` — closed via
    `compiler/codegen/cel_refs.{h,cc}`, emitted from Binaryen C API.
    Covered by `cel_refs_test.cc` (structure + export shape) and
    `compiler/e2e/cel_refs_e2e_test.cc` (wasmtime round-trip:
    intern → get returns host payload, second intern advances slot,
    reset rewinds, slot 0 is null sentinel).
  - wasm32 cross-compile of `cel_runtime.c` — closed via two genrules
    in `compiler/runtime/BUILD.bazel`: one invokes brew's clang
    (`--target=wasm32 -ffreestanding -nostdlib`) to produce
    `cel_runtime.wasm`; the second embeds the bytes as
    `kCelRuntimeWasmBytes` / `kCelRuntimeWasmBytesSize`.  Coverage:
    `compiler/codegen/runtime_link_test.cc`
    (`BinaryenModuleReadWithFeatures` + validator + full
    `cel_make_*` + `memory` export-set check + Allocate-and-Write
    round-trip).  Both genrules and the test are tagged `manual`
    because `/opt/homebrew/opt/llvm/bin/clang` is non-hermetic.
  - `cel_unwrap_message` / `cel_wrap_message` — still open.  The
    cross-compile target exists now, but these two helpers live on
    top of both the externref table *and* `cel_alloc`, so they need
    the runtime+eval **merge** step (deferred to M3) before they
    can be emitted.
  - List / map growth + iteration runtime tests — still open.  The
    wasm32 cross-compile is in place but no codegen caller
    constructs a list or map yet (first caller lands in M4).  When
    it does, add a test that instantiates the merged module under
    wasmtime, grows a list through the allocator, and walks it from
    the host.
  - `cel.abi` custom section — closed via
    `compiler/codegen/abi.{h,cc}` (writer using libprotobuf +
    cel-cpp's `AstToCheckedExpr`) and `cel_abi.proto` (schema
    matching design Appendix A).  `abi_test.cc` covers the proto
    round-trip through a serialized `.wasm`; the CLI attaches the
    section on every `--emit_wasm` invocation.  M2 populates
    `version` / `cel_source` / `checked` / `layout` / empty
    `function_set`; `types` / `attributes` / `patterns` /
    `error_msgs` fill in as M3–M5 introduce features that
    reference them.

**M3 slice A+B (2026-04-19): string literals execute end-to-end.**
String constants now lower to a block that calls `cel_alloc`, stores
bytes into the runtime's shared memory, and calls
`cel_make_string_view`.  `WasmTypeFor(kString)` is `i32` (the offset of
a `CelValue` in the runtime's arena).  The eval module declares the
full runtime import set unconditionally; a new two-module host
(`//compiler/host:host_loader`) instantiates runtime + eval under a
shared `wasmtime_linker_t` and exposes `CallNullaryEval()`.  Coverage:
`expr_lower_test::{StringConstantReturnsI32, EmptyStringLowers,
EvalModuleImportsSharedMemoryFromRuntime,
EvalModuleDeclaresRuntimeFunctionImports}`,
`host_loader_test::{LoadsAndCallsNullaryEval, ResolvesRuntimeImports,
CanBeCalledRepeatedly, ReturnsScalarReprsUnchanged,
RejectsNonWasmBytes, RejectsEvalModuleWithUnsatisfiedImports}`, and
`eval_test::StringLiteralReturnsCelValueOffset`.  The `string` row in
the per-type grid is now ticked for `codegen` and `e2e eval`.

**M3 entry — front-end gaps closed (2026-04-19).**
The per-`ExprKindCase` grid had claimed parser / checker /
annotations `[x]` for `kIdentExpr`, `kSelectExpr` (field), and
`kCallExpr` (member), but the ticks were implicit — no dedicated
test pinned the AST shape for those variants.  Closed via three
tests in `parse_and_check_test.cc`:
  - [x] `kIdentExpr` — `IdentExprParsesToIdentNode` asserts
        `root.kind_case() == kIdentExpr` and the ident name round-trips.
  - [x] `kSelectExpr (field)` — `SelectExprParsesToFieldAccessOnProtoMessage`
        parses `d.name` against `google.protobuf.DescriptorProto` and
        asserts `!test_only` + field name + annotated as `kString`.
  - [x] `kCallExpr (member)` — `MemberCallExprParsesWithTarget` parses
        `"hi".startsWith("h")` and asserts `has_target()` + function
        name + constant-literal target.

**M3 entry — unpinned front-end semantics.** The grids do not cover
these (see §"Front-end semantic coverage" below).  None are blocking
for M3 codegen start, but the member-access chain row and the
string-escape row become load-bearing once M3 lowers
`request.user.name` and string constants, so prioritize those.

**Waiting on later milestones** (don't try to close in M2):
  - `kListExpr`, `kMapExpr`, `kStructExpr` — codegen + e2e rows
    `[ ]`.  Unblocked by M4 (collections).
  - `kComprehensionExpr` × 4 variants + nested shadowing — M3/M4.
  - Arithmetic-overflow / divide-by-zero / NaN-unordered / string
    coercion / unknown-propagation e2e — M5 (three-valued logic).
  - Partial-eval commutativity for `unknown && false → false` — M5.
  - Enum and `Any`-unwrap rejection — M7 stdlib or earlier if a
    user expression forces the question.
  - Conformance suite from `tests/simple/testdata/` — M8.

A bullet in "Still open inside M2" should NEVER outlive M2.  A bullet
in "Waiting on later milestones" SHOULD outlive M2 and flip during
the milestone that introduces the feature.

## Per CEL type

For every type T, we need a positive test and at least one negative test in
each stage of the pipeline where T can appear.

`checker` = `parse_and_check_test.cc` accepts an expression that produces
the type. `annotations` = `typed_ast_test::ReprOfTest` maps the TypeSpec
variant to the right `Repr`. `RejectDyn` tests live in
`static_subset_test.cc`.

| Type            | parser | checker | annotations | RejectDyn | codegen | e2e eval |
| --------------- | :----: | :-----: | :---------: | :-------: | :-----: | :------: |
| `bool`          | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `int`           | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `uint`          | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `double`        | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `string`        | [x]    | [x]     | [x]         | [x]       | [x]     | [x]      |
| `bytes`         | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `null_type`     | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `timestamp`     | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `duration`      | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `list<T>`       | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `map<K,V>`      | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| proto message   | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| enum            | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| wrapper (Int64Value …) | [ ] | [ ]  | [x]         | [x]       | [ ]     | [ ]      |
| `any`           | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `dyn` (rejected)| —      | —       | —           | [x]       | —       | —        |
| `error`         | —      | —       | —           | [x]       | —       | —        |

## Per `ExprKindCase`

| Variant             | parser | checker | annotations | RejectDyn | codegen | e2e |
| ------------------- | :----: | :-----: | :---------: | :-------: | :-----: | :-: |
| `kConstant`         | [x]    | [x]     | [x]         | [x]       | [x]     | [x] |
| `kIdentExpr`        | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ] |
| `kSelectExpr` (field) | [x]  | [x]     | [x]         | [x]       | [ ]     | [ ] |
| `kSelectExpr` (`test_only`, from `has()`) | [x] | [x] | [x]  | [x] | [ ] | [ ] |
| `kCallExpr` (global) | [x]   | [x]     | [x]         | [x]       | [x]     | [x] |
| `kCallExpr` (member) | [x]   | [x]     | [x]         | [x]       | [ ]     | [ ] |
| `kCallExpr` (short-circuit `&&` / `||` / `?:`) | [ ] | [ ] | [ ] | [ ] | [x] | [x] |
| `kListExpr` (empty + non-empty) | [x] | [x] | [x]  | [x]       | [ ]     | [ ] |
| `kStructExpr` (proto ctor) | [x] | [ ] | [x]      | [x]       | [ ]     | [ ] |
| `kMapExpr`          | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ] |
| `kComprehensionExpr` (exists) | [ ] | [ ] | [ ]  | [x]       | [ ]     | [ ] |
| `kComprehensionExpr` (all)    | [ ] | [ ] | [ ]  | [x]       | [ ]     | [ ] |
| `kComprehensionExpr` (filter) | [ ] | [ ] | [ ]  | [x]       | [ ]     | [ ] |
| `kComprehensionExpr` (map)    | [ ] | [ ] | [ ]  | [x]       | [ ]     | [ ] |
| nested comprehensions with shadowing | [ ] | [ ] | [ ] | [ ]   | [ ]     | [ ] |

## Front-end helpers

### Variable-spec parser (`compiler/frontend/parse_and_check.cc`)

- [x] Every primitive by name (`bool`, `int`, `uint`, `double`, `string`,
      `bytes`, `null_type`).  (`parse_and_check_test::PrimitiveVariableSpecs`)
- [x] Every well-known (`timestamp`, `duration`, `any`).
- [x] `list<T>` with primitive and nested-list `T`.
      *(message-element list pending until schema fixture lands.)*
- [x] `map<K,V>` with string key, int value.  *(Full key-type matrix still
      pending — `map<list<int>,int>` rejection not yet asserted.)*
- [x] Proto message by FQN (`google.protobuf.Empty` from the generated
      pool).  *(Custom-schema FQNs pending until e2e fixtures land.)*
- [x] Errors: missing `:`, empty name, unknown type, unbalanced `<>`,
      trailing garbage after the type.

### `RejectDyn`

- [x] DYN at root (missing root type).
- [x] DYN nested inside call, select, list, struct, map, comprehension
      subtrees.
- [x] `ErrorTypeSpec`, `FunctionTypeSpec`, `ParamTypeSpec`, `UnsetTypeSpec`
      each rejected with the correct label.
- [x] Checked expression with no DYN returns OK.

## Front-end semantic coverage (beyond the grids)

The per-type and per-`ExprKindCase` grids above cover *which shapes
are accepted and annotated*, but say nothing about *how the source
string decomposes into those shapes*.  Most of the unpinned items
below delegate to the vendored cel-cpp parser and would rarely
regress from our side — but a wrong expectation (e.g. we assume
left-assoc when the parser is right-assoc, or a codegen pass that
collapses two SelectExprs into one) would silently miscompile.
Each bullet is one cheap regression test.

### Operator precedence & associativity

**Pinned** by a test that inspects an inner result:
  - `*` vs `+`: `1 + 2 * 3 == 7` (`eval_test::IntArithmeticPrecedence`).
  - Explicit parens override: `(1 + 2) * 3 == 9`
    (`eval_test::MixedArithAndConditional`).

**Unpinned** — no test asserts either the parse tree or the value:
  - [ ] `!` vs `&&` — `!true && false` must parse as `(!true) && false`
        and evaluate to `false`.
  - [ ] `&&` vs `||` — `true && false || true` must parse as
        `(true && false) || true` and evaluate to `true`.
  - [ ] `==` vs `&&` — `1 == 1 && 2 == 2` evaluates to `true` (i.e.
        `&&` does not bind tighter than `==`).
  - [ ] Unary vs binary minus — `1 - -2 == 3` (the `-2` is a unary
        negate of a literal, not a malformed token).
  - [ ] Ternary right-associativity — `true ? 1 : false ? 2 : 3 == 1`
        and `false ? 1 : true ? 2 : 3 == 2`.
  - [ ] Relational vs logical — `1 < 2 && 3 < 4` evaluates to `true`
        (relationals bind tighter than `&&`).

Any of the above can be a ~three-line `eval_test` addition — a cheap
insurance policy for parser upgrades.

### Literal forms

**String / bytes** — pinned: `"abc"`, `'hello'`, `b"ok"`.
  - [ ] Escape sequences: `"\n"`, `"\t"`, `"\\"`, `"\""`,
        `"\x41"` (hex byte), `"\u2603"` (BMP codepoint).  Each must
        round-trip through `size()` / equality unchanged.
  - [ ] Raw strings: `r"\n"` → four codepoints, not one newline.
  - [ ] Triple-quoted: `"""hello"""` parses; internal quotes allowed.
  - [ ] Bytes with high bytes: `b"\xff\x00"` has size 2.

**Integer / double** — pinned: `42`, `1u`, `1.5`,
`18446744073709551615u`.
  - [ ] Hex integer: `0xff == 255` and `0xFFu == 255u`.
  - [ ] Scientific double: `1e2 == 100.0`, `1.5e-1 == 0.15`.
  - [ ] Rejection: `-1u` must fail the checker (uint can't be
        negative; the parser accepts the token, the checker must
        flag).

### Member-access & call chains

M3's `request.user.name` and `"hi".startsWith("h")` exercise these
shapes for the first time; today nothing pins them.
  - [ ] `a.b.c` parses as nested `SelectExpr(SelectExpr(Ident "a",
        "b"), "c")`, left-associative.
  - [ ] `a.b().c` parses as `SelectExpr(CallExpr(target=Ident "a",
        fn="b"), "c")` — call-then-member.
  - [ ] `"a".b().c` — literal receiver works the same way.
  - [ ] Chain-level `has()`: `has(a.b.c)` lowers to `test_only`
        SelectExpr at the outermost level only; the inner
        `SelectExpr(a, "b")` stays a normal (non-test_only) select.

### Miscellaneous front-end behaviour

  - [ ] Line comments: `1 + 2 // trailing` parses and evaluates to 3.
  - [ ] Reserved-word rejection: bare `has`, `in`, `as`, `type`
        as an identifier is flagged by the checker, not silently
        treated as a user variable.
  - [ ] Qualified enum-ish name resolution: `com.google.protobuf.Empty`
        parses as a **type reference**, not as three chained selects
        on an ident `com`.

### Scope of this section

These rows do **not** belong in the per-type or per-`ExprKindCase`
grids — those axes are about "can the type system produce this
shape?".  The rows above are about "does the parser produce the
shape we think it produces?".  The distinction matters because a
grid tick can be true (shape is accepted and annotated) while the
source→shape mapping still has an unpinned surprise.

## Runtime (native-compiled for unit tests)

- [x] Bump allocator: alignment, reset semantics, out-of-memory.
      (`compiler/runtime/cel_runtime_test.cc`)
- [x] `cel_make_*` constructors populate the right tag + payload.
      (covers null/bool singletons, int/uint/double, string/bytes copy +
       view, message, type, duration, timestamp, optional some/none,
       unknown, error)
- [x] `cel_string_eq` / `cel_bytes_eq` on empty, equal, unequal-length,
      different-content inputs.
- [ ] List / map growth, iteration.
- [ ] `cel_ref_intern` dedup + `cel_unwrap_message` round-trip.

## End-to-end

Each e2e test instantiates the generated module against a host stub, calls
`eval`, and asserts the returned `CelValue`.  Track one row per smoke
expression from `m1-type-checker.md`, plus:

- [ ] Arithmetic overflow error (int + int overflows to `ERROR`).
- [ ] Division by zero (`int / 0` and `double / 0`).
- [ ] String coercion errors where the spec forbids them.
- [ ] `unknown` propagation through `&&` / `||` (M5).
- [ ] Partial-eval: `unknown && false → false` commutatively (M5).

## How to update

When you add a test, flip the box to `[x]` and include the test's path in
the adjacent cell *if* the mapping isn't obvious.  When a new AST variant or
type lands, add a new row; never silently drop a row.
