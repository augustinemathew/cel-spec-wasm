# Repo rules for Claude (celwasmc)

This repo is a CEL → WebAssembly AOT compiler.  Existing cel-spec artefacts
(Go protos, conformance tests, `doc/langdef.md`) are untouched; the new
compiler lives under `compiler/` and vendors `third_party/cel-cpp/` for
parser + type-checker reuse.

## Authoritative docs

Always read these before making non-trivial changes.  If a change invalidates
something in them, update them in the same commit as the code.

  - `doc/wasm-compiler-design.md` — architecture, host ABI, milestones M0-M8.
  - `doc/implementation-plan/` — per-milestone plan + running test-coverage
    checklist.  **Keep this up to date.**  When the user gives new guidance
    or a new feature is added, add a tagged bullet; when a feature ships,
    tick the box.  Treat the checklist as the definition of "done" for each
    milestone.
  - `doc/langdef.md` — CEL semantics we must honour.
  - `doc/implementation-plan/testing-checklist.md` — transverse coverage grid
    (CEL type × pipeline stage, AST variant × pipeline stage).  Every merged
    feature flips at least one box; every new variant adds at least one row.

## C++ style

Follow the Google C++ style (google3 flavour).  `third_party/cel-cpp/` is the
reference — when in doubt, copy its conventions verbatim.  In particular:

  - Header guards: `CELWASM_<PATH>_H_`.
  - Prefer `absl::` containers / status / strings over `std::` equivalents
    where cel-cpp does.  Use `absl::StatusOr<T>` for fallible returns.
  - Annotate pointers with `absl_nonnull` / `absl_nullable` where ownership
    is non-obvious.
  - `ABSL_MUST_USE_RESULT` on functions returning `absl::Status` /
    `absl::StatusOr`.
  - No raw `new` / `delete`.  Use `std::unique_ptr`, `std::make_unique`, or
    `google::protobuf::Arena` for checker-owned types.
  - One logical unit per translation unit; BUILD targets are granular
    (`cc_library` per header).
  - Close namespaces with `}  // namespace celwasm`.

## Testing is mandatory

Compilers fail silently.  **Every type and every AST variant must have both
a positive and a negative test.**  Before a milestone is marked done:

  1. `compiler/<path>_test.cc` exists for every non-trivial source file.
  2. The relevant rows in `doc/implementation-plan/testing-checklist.md`
     are ticked for that milestone.
  3. `bazel test //compiler/...` is green.

Prefer `@com_google_googletest//:gtest_main` + status matchers.  For
pipeline coverage:

  - **Parser / checker stages** — unit-test by inspecting the resulting
    `ParsedExpr` / `CheckedExpr` / `WasmAnnotations` / `absl::Status`.
  - **Codegen stage** — use Binaryen's module validator and inspect the
    emitted IR.  Do not rely on wasm execution alone to catch regressions.
  - **End-to-end** — instantiate the module under a real WASM runtime
    (wasmtime C API) and compare `CelValue` results to literals.

When a bug is fixed, add a regression test *in the same commit*.

## Build & run

  - Primary build: `bazel build //compiler/...`.
  - Primary tests: `bazel test //compiler/...`.
  - CLI: `bazel-bin/compiler/cli/celwasmc -e "<expr>" [--check ...]`.
  - Apple clang does **not** have a wasm32 target.  Cross-compilation uses
    brew's `llvm` + `binaryen` (already installed on this machine).

## Unresolved design debt to keep front-of-mind

Track these while working; raise them to the user when a change touches
the surrounding code, and update / close them when a decision ships.

- **Unified symbol table (decide before M4 Slice A).** Name / type /
  scope info is currently split three ways: `CheckOptions::variable_specs`
  (frontend), `TypedAst::variables()` + `WasmAnnotations` (IR), and
  `LoweringContext.idents` in `compiler/codegen/expr_lower.cc`
  (codegen).  Works for today's flat, scope-free subset; breaks the
  moment comprehensions (M4), user functions (M6), or a leading-dot
  rewrite pass need nested scopes.  Two options in the design doc's
  "Open questions" section: (A) promote to a `SymbolTable` on
  `TypedAst`, (B) side-table off cel-cpp's `reference_map`.  **If you
  are about to add comprehension lowering, a new IR pass, or a second
  binding frame, flag this to the user and ask which option to take
  before writing code.**  Close this bullet in both files when the
  decision ships.

## What not to do

  - Don't reimplement the CEL parser or type checker.  Reuse
    `@cel-cpp//parser`, `@cel-cpp//checker:*`, `@cel-cpp//common:*`.
  - Don't edit files under `third_party/cel-cpp/` — we treat that as a
    vendored dep.  If a bug lives there, file an upstream-style patch under
    `third_party/patches/`.
  - Don't ship a feature without updating both the milestone doc and the
    testing checklist.
  - Don't introduce dynamic typing; the compiler only accepts the static
    subset.  `RejectDyn` is the gate.
