# Testing checklist

Compilers fail silently.  The grid below is the minimum coverage the project
must keep green; tick items off as the corresponding `cc_test` lands.
Negative tests are as important as positive ones — every row needs both a
"this works" and a "this fails with a good message".

Conventions:
  - `compiler/<path>_test.cc` is Google Test (`@com_google_googletest//:gtest_main`).
  - End-to-end (wasm-executing) tests live under `compiler/e2e/`.
  - Each box is `[ ]` when pending, `[x]` when a committed test covers it.

## Per CEL type

For every type T, we need a positive test and at least one negative test in
each stage of the pipeline where T can appear.

`checker` = `parse_and_check_test.cc` accepts an expression that produces
the type. `annotations` = `typed_ast_test::ReprOfTest` maps the TypeSpec
variant to the right `Repr`. `RejectDyn` tests live in
`static_subset_test.cc`.

| Type            | parser | checker | annotations | RejectDyn | codegen | e2e eval |
| --------------- | :----: | :-----: | :---------: | :-------: | :-----: | :------: |
| `bool`          | [x]    | [x]     | [x]         | [x]       | [x]     | [ ]      |
| `int`           | [x]    | [x]     | [x]         | [x]       | [x]     | [ ]      |
| `uint`          | [x]    | [x]     | [x]         | [ ]       | [x]     | [ ]      |
| `double`        | [x]    | [x]     | [x]         | [ ]       | [x]     | [ ]      |
| `string`        | [x]    | [x]     | [x]         | [ ]       | [ ]     | [ ]      |
| `bytes`         | [x]    | [x]     | [x]         | [ ]       | [ ]     | [ ]      |
| `null_type`     | [x]    | [x]     | [x]         | [ ]       | [ ]     | [ ]      |
| `timestamp`     | [x]    | [x]     | [x]         | [ ]       | [ ]     | [ ]      |
| `duration`      | [x]    | [x]     | [x]         | [ ]       | [ ]     | [ ]      |
| `list<T>`       | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| `map<K,V>`      | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| proto message   | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ]      |
| enum            | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| wrapper (Int64Value …) | [ ] | [ ]  | [x]         | [ ]       | [ ]     | [ ]      |
| `any`           | [x]    | [x]     | [x]         | [ ]       | [ ]     | [ ]      |
| `dyn` (rejected)| —      | —       | —           | [x]       | —       | —        |
| `error`         | —      | —       | —           | [x]       | —       | —        |

## Per `ExprKindCase`

| Variant             | parser | checker | annotations | RejectDyn | codegen | e2e |
| ------------------- | :----: | :-----: | :---------: | :-------: | :-----: | :-: |
| `kConstant`         | [x]    | [x]     | [x]         | [x]       | [x]     | [ ] |
| `kIdentExpr`        | [x]    | [x]     | [x]         | [x]       | [ ]     | [ ] |
| `kSelectExpr` (field) | [x]  | [x]     | [x]         | [x]       | [ ]     | [ ] |
| `kSelectExpr` (`test_only`, from `has()`) | [ ] | [ ] | [ ]  | [ ] | [ ] | [ ] |
| `kCallExpr` (global) | [x]   | [x]     | [x]         | [x]       | [x]     | [ ] |
| `kCallExpr` (member) | [x]   | [x]     | [x]         | [x]       | [ ]     | [ ] |
| `kCallExpr` (short-circuit `&&` / `||` / `?:`) | [ ] | [ ] | [ ] | [ ] | [x] | [ ] |
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
