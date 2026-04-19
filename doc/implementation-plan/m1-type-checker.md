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

## Outstanding obligations rolled into M2 / the testing checklist

- [ ] Write `cc_test` equivalents for every smoke test above (M2 requires
      gtest infra; the first test targets land with M2).
- [ ] Cover every primitive type declaration path in `ParseVariableSpec`
      (see `testing-checklist.md §Variable-spec parser`).
- [ ] Cover every `ExprKindCase` in `RejectDyn` including nested
      comprehensions with shadowing (expr ids may repeat across scopes —
      verify the walker visits each instance).
