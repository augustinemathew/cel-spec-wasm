# M1 — Type checker integration & static-subset validator

Status: **done** (2026-04).

## Scope

Wire cel-cpp's type checker into `celwasmc`, validate that the user expression
fits the statically-typed subset, and seed the per-node `WasmAnnotations`
side-map used by codegen.

## Deliverables

- [x] `compiler/ir/annotations.{h,cc}` — `Repr` enum (
      `kNull`, `kBool`, `kInt`, `kUint`, `kDouble`, `kString`, `kBytes`,
      `kList`, `kMap`, `kMessage`, `kEnum`, `kDuration`, `kTimestamp`,
      `kType`, `kUnknown`) and `WasmAnnotations` keyed by `cel::ExprId`.
- [x] `compiler/ir/typed_ast.{h,cc}` — owning wrapper over a checked
      `cel::Ast` + a `WasmAnnotations` side map.  Exposes `ReprOf(TypeSpec)`
      and `PopulateAnnotations()`.
- [x] `compiler/ir/static_subset.{h,cc}` — `RejectDyn()` walks the `Expr`
      tree and errors on nodes missing from `type_map` (DYN), or typed as
      `Error` / `Function` / `ParamType` / `Unset`.
- [x] `compiler/frontend/parse_and_check.{h,cc}` — drives
      `cel::CreateTypeCheckerBuilder` + `StandardCheckerLibrary`, loads a
      `FileDescriptorSet` into a `MergedDescriptorDatabase(schema,
      generated)`, parses `name:Type` specs (primitives, well-knowns,
      `list<T>`, `map<K,V>`, proto FQNs) with a small recursive-descent
      parser, and returns a `TypedAst`.
- [x] CLI flags added: `--check`, `--reject_dyn` (default true), `--schema`,
      `--var` (`;`-delimited so `map<K,V>` commas survive absl's splitter),
      `--container`.  `--check` prints the `CheckedExpr` textproto plus a
      `# WasmAnnotations (expr_id -> repr)` summary.

## Smoke tests performed (manual, pre-M1-gtest)

- [x] `1 + 2` → every node typed `int`.
- [x] `"hello " + name` with `name:string` → all `string`.
- [x] `[1,2,3].exists(x, x > 1)` → list + bool, iter_var typed `int`.
- [x] `parts.all(p, p.startsWith("x"))` with `parts:list<string>`.
- [x] `things.exists(k, things[k] > 0)` with `things:map<string,int>`.
- [x] `req.name == "foo" && req.size > 10` with schema + `req:sample.Request`.
- [x] `req.tags.exists(t, t == "admin") && req.headers["x-env"] == "prod"`.
- [x] `req.nested.enabled` → message→message→bool chain.
- [x] Negative: `1 + "abc"`, undeclared ident, missing proto field,
      `x + 1` with `x:dyn` → `--reject_dyn` catches it.

## gtest backfill (landed during M2)

Once M2 pulled googletest into the module, we wrote the full test suite
for M1 code:

- [x] `compiler/ir/annotations_test.cc` — `ReprName` row per enumerator
      plus `WasmAnnotations` map round-trip (positive/negative ids,
      default-creation semantics).
- [x] `compiler/ir/typed_ast_test.cc` — `ReprOf` exercised on every
      `TypeSpec` variant (primitives × 6, wrappers × 6, well-knowns × 3,
      null, list, map, message, type, dyn, error, function, param, unset,
      abstract).  Also covers `PopulateAnnotations` seeding and
      `TypedAst` move semantics.
- [x] `compiler/ir/static_subset_test.cc` — `RejectDyn` across every
      `ExprKindCase` with both all-typed and DYN-child configurations,
      plus each rejected `TypeSpec` variant (Dyn / Error / Function /
      Param / Unset) and a multi-violation message assertion.
- [x] `compiler/frontend/parse_and_check_test.cc` — every primitive +
      well-known + parameterized type spec, nested `list<list<int>>`,
      `google.protobuf.Empty` from the generated pool, whitespace
      tolerance, trailing-garbage / missing-colon / empty-name /
      unknown-type / unbalanced `<>` rejections, schema-not-found, parse
      errors, undeclared-variable and type-mismatch rejections.

## Still outstanding (tracked in testing-checklist.md)

- [ ] `kSelectExpr(test_only)` coverage via `has(msg.field)`.
- [ ] `kCallExpr(short-circuit)` coverage via `&&`/`||`/`?:`.
- [ ] Comprehension positive coverage in parser/checker/annotations (the
      RejectDyn walker is already proven).
- [ ] Nested comprehensions with shadowing.
- [ ] Enum and message-wrapper declaration paths in `ParseVariableSpec`.
- [ ] Map-key-type matrix (reject `list<>` and message keys per spec).
