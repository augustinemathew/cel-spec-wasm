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
  - `doc/implementation-plan/rewrite/feature-pipeline-checklist.md` — for
    every feature type (new AST kind, new declarable type, new host import,
    new ABI field, …), the concrete list of files + tests that MUST be
    touched.  **Start every feature session by copying the matching
    section's checklist into the milestone doc's "In progress" section**;
    work through top-down.  This doc is the forcing function against
    forgetting one of the 8–12 stages a typical feature ripples through.
  - `doc/implementation-plan/per-component-test-coverage.md` — the keystone
    testing doc.  Per-component required test scenarios (positive +
    negative + boundary), the catalog of `manual`-tagged targets that
    MUST run before a milestone closes, SKIP discipline (never
    `GTEST_SKIP` an entire fixture's `SetUp` for "this whole feature
    isn't done yet" — that's how M2 silently shipped half-done with 29
    skipped tests), and the closeout gate to copy into every milestone
    PR description.  **`bazel test //compiler_v2/...` being green does
    NOT mean a milestone is done — manual-tagged tests carry the
    load-bearing e2e assertions; they MUST be run explicitly.**

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

**The rule applies to every control-flow shape, not just switches.**
Any branch that reaches a path a later milestone will light up gets
an `ABSL_CHECK(false) << "... is a stub until <milestone>"` at the
branch, not a silent skip.  Covers:

  - `switch` arms (closed enum case for a later-milestone kind)
  - `if` / `else if` branches gated on a runtime flag a later
    milestone will flip (e.g. `if (ann->map_origin == Origin::kArena)`)
  - early returns that trade a zero sentinel for "this isn't
    populated yet" (return the sentinel AND CHECK before use, or
    CHECK at the write site so the zero never travels)
  - factory / builder methods shipped as signature-final stubs
    (`Value::HostMap`, `Activation::BindLazy`, …)

The goal is invariant: a call into any code path that isn't done yet
crashes at the call site naming the symbol and milestone, rather than
miscompiling silently or producing a plausible-looking wrong answer
four passes later.  If a future-milestone path is unreachable under
the current pipeline (e.g. map codegen is gated out at the frontend),
the CHECK still lives at the reachable edge — the gate can regress,
the CHECK is the tripwire.

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

## WAT-first for ABI and codegen design

Before implementing any new codegen arm (kSelect, kCall, kComprehension,
list/map ops, …) or host-ABI surface (`cel_host.cel_get_field`,
future customs trampolines), write the **target wasm in WAT first**
under `doc/implementation-plan/rewrite/wat/NN_name.wat`, add the
walkthrough to `doc/implementation-plan/rewrite/wat-traces.md`, and
assemble it end-to-end:

```bash
wasm-as doc/implementation-plan/rewrite/wat/NN_name.wat -o /tmp/foo.wasm
```

Then — whenever the wasm plausibly runs (all imports exist, real or
stubbed) — execute it through the evaluator-runtime harness
(`compiler_v2/tools/wat_runner`) with stub impls for any not-yet-
implemented host functions.  The harness takes a `.wat` file and
optional pre-populated memory bytes and runs $eval through wasmtime
against `cel_runtime.wasm`, returning the decoded `CelValue`.

Why this is mandatory, not a nice-to-have:

  - **Memory layout and ABI are frozen at the WAT level.**  Every
    decision — slot sizes, offset alignment, trampoline arg order,
    locals vs constants, rodata vs workspace — is forced at WAT
    authoring time.  No "we'll figure it out as we go" mid-codegen.
  - **The WAT is executable.**  A WAT that fails to assemble means
    the shape is wrong before any codegen C++ is touched.  A WAT
    that assembles but wasmtime rejects means the ABI signatures
    are wrong.  A WAT that traps at runtime means the semantics
    are wrong.  Each failure mode surfaces before it contaminates
    codegen.
  - **ABI prototyping with stubs.**  A new host-imported function
    (e.g. `cel_host.cel_get_field` at M2.C) gets a stub impl in
    `wat_runner` that writes a caller-supplied CelValue to the
    `out_slot` — letting us run the dependent WAT end-to-end before
    the real trampoline is written.  When the real trampoline
    lands, we verify byte-identical output against the stubbed
    baseline.
  - **Regression tests come for free.**  `wat_runner_test.cc`
    re-assembles and re-runs every `.wat` under `doc/…/wat/` on
    every build.  A codegen arm that stops producing shape-
    matching wasm is caught by the WAT equivalence test, not by
    an obscure e2e breakage four passes downstream.

Workflow per codegen arm:

  1. Write the WAT file with inline comments explaining memory
     layout, offsets, and ABI rationale.
  2. Assemble + validate with `wasm-as`.
  3. If the WAT references host functions, add stub impls to
     `wat_runner` and run it end-to-end through wasmtime.
  4. Document in `wat-traces.md` (one section per WAT, with the
     source-expr header, expected memory map, and any invariants
     the shape locks).
  5. Implement the codegen arm in `expr_lower.cc`.  Add a test
     that compiles the source expression and asserts the emitted
     wasm matches the WAT's disassembly byte-for-byte (modulo
     Binaryen-assigned names).
  6. Remove the stub from `wat_runner` once the real host impl
     lands; keep the WAT running through the production
     trampoline.

This is the ONLY acceptable design process for new ABI surfaces
or codegen arms.  Freehand C++ codegen without a WAT trace is a
rewrite waiting to happen.

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
  - **Cover the edge-case matrix — this is a compiler.**  Compilers
    miscompile silently when an unhandled input shape slips through.
    For every new feature, enumerate the matrix explicitly: every
    allowed type (e.g. map keys → bool / int / uint / string; check
    each), every boundary value (`INT64_MIN`, `INT64_MAX`, `UINT64_MAX`,
    `0`, `-1`, empty string, embedded NUL, multi-byte UTF-8), and
    every disallowed shape (every CelKind that *isn't* a valid map
    key, every AST node a feature doesn't yet support).  Negative
    coverage is the load-bearing half — a poison/error path that
    isn't exercised will break in production silently.  When the
    spec restricts inputs to a closed set (e.g. valid map-key kinds,
    valid arithmetic operand kinds), the test matrix should cover
    every member of that set AND a representative set of complement
    members that must reject.
  - **Parameterize to remove duplication, but write the cases out
    longhand first.**  Drafting tests as separate `TEST_F`s is the
    clearest way to think through which cases exist and what each
    asserts.  Once the matrix is settled, consolidate the
    structurally-identical ones into a `TEST_P` + `INSTANTIATE_TEST_SUITE_P`
    table.  Keep tests with distinct stories (a specific
    bug-surface, a specific spec citation, a one-off invariant) as
    individual `TEST_F`s — parameterizing those obscures intent
    rather than clarifying it.  See `compiler_v2/runtime/cel_map_test.cc`
    for the canonical shape: parameterized round-trip / cross-type /
    disallowed-kind tables coexist with focused single-`TEST_F`
    cases for embedded-NUL strings and bool-vs-int distinctness.

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
