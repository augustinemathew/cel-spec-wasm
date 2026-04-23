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

### Closing out a planning doc

Planning docs (`doc/implementation-plan/**/*.md`) are living artifacts.
When the work they describe ships:

  1. **Update the header status line** so the doc itself signals state.
     `Status: plan — drafted YYYY-MM-DD, not yet started.` →
     `Status: shipped YYYY-MM-DD.` (with a one-paragraph "what landed"
     summary if the as-shipped shape differs from the as-written plan —
     architecture deltas, dropped/merged commits, scope changes).
  2. **Reflect deltas in-line.** If sections describe an approach that
     was revised during execution, leave the old text but mark the
     delta with a callout (`> Plan-vs-execution delta: …`) or replace
     the section with the as-shipped version, citing the doc that
     captures the reasoning for the change.
  3. **Append a "Future work" section at the bottom.** Anything
     surfaced during execution but not in scope of this doc — perf
     hotspots a bench measured, follow-up cleanups, design questions
     punted to the next milestone — gets a one-line bullet here.
     Future readers see what's done AND what's still open without
     having to read the whole doc.
  4. **Reconcile other docs that referenced the old plan.** Sibling
     plan docs, `design.md`, `cel-host-surface.md`, milestone docs —
     any doc that named the old shape gets updated in the same commit.
     Stale plan refs in master are worse than no plan refs.
  5. **Tick `testing-checklist.md` rows.** Per the checklist's own
     "How to update" rule — every merged feature flips at least one
     box; the milestone's "what coverage shipped" listed in §6.3 (or
     equivalent) becomes ticked rows in a "Rewrite M<n>" section.

The goal: a reader skimming the docs in `doc/implementation-plan/`
should see at a glance which milestones are done, which are in flight,
and what follow-ups each one surfaced — without having to grep the
git log.

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

**Unreachable switch defaults.**  When a `switch` enumerates a closed
set of cases (every `enum` value, every `cel::ExprKindCase`, every
`CelKind`, …), the `default:` arm is an invariant violation — not a
legitimate code path.  Fail loudly with `ABSL_CHECK(false)` (or
`ABSL_LOG(FATAL)`) naming the offending value.  **Do not use `DCHECK`**:
a compiler that silently miscodegens in release builds is worse than
one that crashes.  Only return a fallback value from `default:` when
the switch is genuinely open — e.g. parsing untrusted wire bytes, where
unknown bytes should pass through (see `FormatDirective` in
`compiler/host/cel_log.cc` for an example of the legitimate form).

**Unimplemented features.**  When a code path is a stub until a later
milestone — an arm of a switch that M1 doesn't handle, a
signature-final helper whose body lands in M5, a visitor override that
M2 will fill in, and so on — the body MUST be
`ABSL_CHECK(false) << "<symbol> is a stub until <milestone>"`.  No
silent fallbacks, no empty bodies, no bare `TODO` comment without the
check.  The rule is the same as for unreachable switch defaults: a
release build that silently miscompiles is worse than one that
crashes, and naming the symbol + milestone in the message turns an
earlier-than-expected caller into a directly actionable backtrace.
See `StaticMemoryBuilder::AllocateList` in
`compiler_v2/codegen/static_memory_builder.cc` for the canonical form.

## Lint & format (mandatory before every commit)

Formatter and linter are authoritative.  The configs live at the repo
root (`.clang-format`, `.clang-tidy`) and mirror google3 / cel-cpp
conventions.  `third_party/` is excluded from both.

Before every commit run, in order:

  1. `scripts/lint.sh` — `clang-format -i` + `clang-tidy` on the files
     you touched (diffed against `origin/master`).  Non-zero exit on any
     clang-tidy warning.  Run `scripts/refresh_compile_db.sh` first if
     `compile_commands.json` is stale or missing; analysis without it is
     partial.
  2. `bazel test //compiler/...`.
  3. Update `doc/implementation-plan/testing-checklist.md` and the
     active milestone doc (see "Authoritative docs" above).

**Function-size gate.**  `readability-function-size` is on with tight
thresholds (60 lines / 40 statements / 15 branches / 6 params / 5
nesting levels).  Functions do ONE thing and are short enough to
review without scrolling.  When the linter flags a function, split
it — do not `// NOLINT` around it.  Known exceedances are tracked in
`doc/implementation-plan/lint-backlog.md`; clear them before adding
new code in the same file.

Full workflow and install steps are in `doc/contributing.md`.

## Testing is mandatory

Compilers fail silently.  **Every type and every AST variant must have both
a positive and a negative test.**  Before a milestone is marked done:

  1. `compiler/<path>_test.cc` exists for every non-trivial source file.
  2. The relevant rows in `doc/implementation-plan/testing-checklist.md`
     are ticked for that milestone.
  3. `bazel test //compiler/...` is green.

**Testing principles.**  This codebase is a pipeline — frontend → IR →
codegen → runtime/host.  Each stage is a component, and any non-trivial
change touches several of them.  Apply these rules every time:

  - **No feature is done without tests.**  Ship the test in the same
    commit as the code; a "will add tests later" is shipping a
    regression surface.
  - **Test component-by-component, exhaustively.**  After a slice, audit
    every file you touched and enumerate which tests now cover which
    method / code path.  Gaps get filled before the slice lands.  A
    change that modifies, say, proto codegen, ABI emission, host
    parsing, and a trampoline needs tests at each level — not just one
    end-to-end test that happens to pass.
  - **The CEL language spec (`doc/langdef.md`) is the source of truth.**
    When testing 3VL / partial-eval / type coercion / comprehension
    semantics, the assertion matrix comes from the spec, not from
    whatever shape the implementation happens to produce.  Cite the
    spec section in the test comment when the behaviour is
    spec-mandated.

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

- **Unified symbol table (decide before M5 Slice A).** Name / type /
  scope info is currently split three ways: `CheckOptions::variable_specs`
  (frontend), `TypedAst::variables()` + `WasmAnnotations` (IR), and
  `LoweringContext.idents` in `compiler/codegen/expr_lower.cc`
  (codegen).  Works for today's flat, scope-free subset; breaks the
  moment comprehensions (M5), user functions (M6), or a leading-dot
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
