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

| Type            | parser | checker | annotations | RejectDyn | codegen | e2e eval |
| --------------- | :----: | :-----: | :---------: | :-------: | :-----: | :------: |
| `bool`          | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `int`           | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `uint`          | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `double`        | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `string`        | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `bytes`         | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `null_type`     | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `timestamp`     | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `duration`      | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `list<T>`       | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `map<K,V>`      | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| proto message   | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| enum            | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| wrapper (Int64Value …) | [ ] | [ ]  | [ ]         | [ ]       | [ ]     | [ ]      |
| `any`           | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ]      |
| `dyn` (rejected)| —      | —       | —           | [ ]       | —       | —        |
| `error`         | —      | —       | —           | [ ]       | —       | —        |

## Per `ExprKindCase`

| Variant             | parser | checker | annotations | RejectDyn | codegen | e2e |
| ------------------- | :----: | :-----: | :---------: | :-------: | :-----: | :-: |
| `kConstant`         | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ] |
| `kIdentExpr`        | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ] |
| `kSelectExpr` (field) | [ ]  | [ ]     | [ ]         | [ ]       | [ ]     | [ ] |
| `kSelectExpr` (`test_only`, from `has()`) | [ ] | [ ] | [ ]  | [ ] | [ ] | [ ] |
| `kCallExpr` (global) | [ ]   | [ ]     | [ ]         | [ ]       | [ ]     | [ ] |
| `kCallExpr` (member) | [ ]   | [ ]     | [ ]         | [ ]       | [ ]     | [ ] |
| `kCallExpr` (short-circuit `&&` / `||` / `?:`) | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| `kListExpr` (empty + non-empty) | [ ] | [ ] | [ ]  | [ ]       | [ ]     | [ ] |
| `kStructExpr` (proto ctor) | [ ] | [ ] | [ ]      | [ ]       | [ ]     | [ ] |
| `kMapExpr`          | [ ]    | [ ]     | [ ]         | [ ]       | [ ]     | [ ] |
| `kComprehensionExpr` (exists) | [ ] | [ ] | [ ]  | [ ]       | [ ]     | [ ] |
| `kComprehensionExpr` (all)    | [ ] | [ ] | [ ]  | [ ]       | [ ]     | [ ] |
| `kComprehensionExpr` (filter) | [ ] | [ ] | [ ]  | [ ]       | [ ]     | [ ] |
| `kComprehensionExpr` (map)    | [ ] | [ ] | [ ]  | [ ]       | [ ]     | [ ] |
| nested comprehensions with shadowing | [ ] | [ ] | [ ] | [ ]   | [ ]     | [ ] |

## Front-end helpers

### Variable-spec parser (`compiler/frontend/parse_and_check.cc`)

- [ ] Every primitive by name (`bool`, `int`, `uint`, `double`, `string`,
      `bytes`, `null_type`).
- [ ] Every well-known (`timestamp`, `duration`, `any`).
- [ ] `list<T>` with primitive, message, and nested-list `T`.
- [ ] `map<K,V>` with every permissible key type; reject `list<>` or message
      keys per spec.
- [ ] Proto message by FQN, incl. nested packages.
- [ ] Errors: missing `:`, empty name, unknown type, unbalanced `<>`,
      trailing garbage after the type.

### `RejectDyn`

- [ ] DYN at root (missing root type).
- [ ] DYN nested inside each `ExprKindCase`.
- [ ] `ErrorTypeSpec`, `FunctionTypeSpec`, `ParamTypeSpec`, `UnsetTypeSpec`
      each rejected with the correct label.
- [ ] Checked expression with no DYN returns OK.

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
