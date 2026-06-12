# Repo rules for Claude (celwasmc)

This repo is a CEL → WebAssembly AOT compiler.  Its source is organised by
**lifecycle role** at the top level (the layout cel-cpp itself uses):

  - `compiler/` — compile-time: CEL source → `Program` (wasm bytes + `cel.abi`).
    Children: `frontend/` (parse + type-check, wraps cel-cpp), `ir/` (typed
    AST + annotations), `codegen/` (Binaryen lowering), `celfn/` (function
    library), `internal/` (the private `compile.{h,cc}` pipeline facade);
    the public face is `compiler.{h,cc}` + `program.h`.
  - `eval/` — eval-time: `Program` + `Activation` → `Value` (the C++/wasmtime
    evaluator).  Public leaves `engine/instance/activation/value/error/
    attribute`; `host/` (cel_log trampolines) and `internal/` (wasmtime glue,
    `abi_decode`, `cel_host`) are private.
  - `shared/` — `CelType`, the type vocabulary both compile and eval speak.
    (Named `shared/`, NOT `common/`: a `common/` here collided with vendored
    cel-cpp's own `common/` top-level include dir.)
  - `abi/` — the `cel.abi` wire contract (the emit side; the parse
    side lives at `eval/internal/abi_decode.{h,cc}` because decoding
    round-trips `ir::Repr` — moving it here rides on relocating `Repr`
    to `shared/`).
  - `runtime/` — `cel_runtime.c` → `cel_runtime.wasm` (language-agnostic kernel).
  - `tools/` (cel CLI, wat_runner), `conformance/`, `e2e/`, `benchmark/`,
    `testdata/` — leaf binaries/tests.
  - `spec/` — cel-spec heritage (the `.textproto` conformance corpus under
    `spec/tests/`).  `proto/` STAYS at repo root for now (the move is deferred
    to a module-rename workstream).

The layering rule is **one-directional**: `compiler/` depends only on
`shared/` (and vendored cel-cpp / Binaryen) — never on `eval/` or
wasmtime — so `compiler.wasm` stays reachable as a future build target.
`eval/` may consume compiler-side *data* vocabulary (`//compiler:program`,
`//compiler/ir:annotations` for `Repr`, `//compiler/celfn:function_library`
for `Engine` registration) but never the compilation pipeline itself;
only eval *test* targets link `//compiler:compiler`.

Existing cel-spec artefacts (`doc/langdef.md`, the conformance corpus) are
upstream contract; we vendor `third_party/cel-cpp/` for parser + type-checker
reuse.

## Authoritative docs

Always read these before making non-trivial changes.  If a change invalidates
something in them, update them in the same commit as the code.

  - `doc/implementation-plan/rewrite/design.md` — architecture, host ABI,
    milestone slicing.  The active design surface lives under
    `doc/implementation-plan/rewrite/` (the original
    `doc/wasm-compiler-design.md` + per-milestone `mN-*.md` docs were
    superseded by the rewrite and removed).
  - `doc/implementation-plan/` — per-milestone plan + running test-coverage
    checklist; active milestone work happens under
    `doc/implementation-plan/rewrite/`.  **Keep this up to date.**  When the
    user gives new guidance or a new feature is added, add a tagged bullet;
    when a feature ships, tick the box.  Treat the checklist as the
    definition of "done" for each milestone.
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
    PR description.  **`bazel test $PROJ` being green does
    NOT mean a milestone is done — manual-tagged tests carry the
    load-bearing e2e assertions; they MUST be run explicitly.**

### Naming a new design doc

Every new design / planning doc under
`doc/implementation-plan/rewrite/` is named **`mNN-<name>.md`** — a
milestone number prefix, then a short kebab-case slug
(e.g. `m13-custom-fns.md`, `m18-network-ext.md`,
`m21-host-call-adapter.md`).  Do NOT create un-prefixed names like
`host-call-adapter.md` or `foo-design.md`.  Pick `NN` as the next free
milestone number (scan the existing `mNN-*.md` files for the highest;
gaps left by milestones living on other branches are not refilled —
take the next number after the highest).  Sub-slices of one milestone
share its number with a letter suffix (`m5b-…`, `m7a-…`).  The number
is the doc's stable handle; the slug can be terse.

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
  - **No C++ exceptions.**  Per the [Google C++ Style Guide]
    (https://google.github.io/styleguide/cppguide.html#Exceptions), we
    do not use exceptions: never `throw`, never `try` / `catch`, never
    add an exception-only header (`<exception>`, `<stdexcept>`).  Fallible
    APIs return `absl::Status` / `absl::StatusOr<T>`; programmer-error
    invariants use `ABSL_CHECK` (which aborts, it does not throw).  STL
    operations that can in principle throw `std::bad_alloc` are treated
    as process-fatal — we never catch them.  The single allowed
    accommodation is on a `cc_binary` `main`, where clang-tidy's
    `bugprone-exception-escape` is silenced with a trailing
    `// NOLINT(bugprone-exception-escape)` (see `tools/cel/cel.cc`); this
    is NOT a license to write `try` / `catch` in the body.
  - One logical unit per translation unit; BUILD targets are granular
    (`cc_library` per header).
  - Close namespaces with `}  // namespace celwasm`.

**No milestone / slice references in code comments.**  Code comments
must describe what the code does and why, in terms that stay true
after milestones close.  Do not write `// M14 Slice B: ...`,
`// Added in M5.B`, `// land in M14 Slice B`, or `// M14 expanded
the static subset to admit ...` — those references rot the moment
the work ships, and they leak project-tracker noise into the
codebase.  Cite a design-doc path only when the doc explains a
non-obvious invariant the reader needs (e.g. an ABI contract or a
rejected alternative); cite the path, not the milestone that
authored it.

**Single carved-out exception:** the `ABSL_CHECK(false) << "<symbol>
is a stub until <milestone>"` form below.  The milestone name there
is load-bearing — it tells the eventual debugger which slice owns
filling in the body — and the stub message is meant to be deleted
when the milestone lands, so it doesn't accumulate.

The rule applies to NEW comments.  Existing milestone references
in the codebase are grandfathered; clean them up opportunistically
when you're already editing the surrounding lines, not in
standalone churn commits.

**Unreachable switch defaults.**  When a `switch` enumerates a closed
set of cases (every `enum` value, every `cel::ExprKindCase`, every
`CelKind`, …), the `default:` arm is an invariant violation — not a
legitimate code path.  Fail loudly with `ABSL_CHECK(false)` (or
`ABSL_LOG(FATAL)`) naming the offending value.  **Do not use `DCHECK`**:
a compiler that silently miscodegens in release builds is worse than
one that crashes.  Only return a fallback value from `default:` when
the switch is genuinely open — e.g. parsing untrusted wire bytes, where
unknown bytes should pass through (see `FormatDirective` in
`eval/host/cel_log.cc` for an example of the legitimate form).

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
`compiler/codegen/static_memory_builder.cc` for the canonical form.

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

**The default is the cheap, working-set-scoped operation; the
exhaustive sweep is explicit.**  This principle holds across lint,
test, and build — the inner loop touches only what you changed, and
you opt in to the full pass once, at the gate.  clang-tidy parsing the
absl/cel-cpp headers costs ~4.6 s *per file* even with the PCH (the
floor), so this matters most for lint:

  - **Inner loop (default):** bare `scripts/lint.sh` lints only your
    **working-tree edits** (staged + unstaged) — the 1-2 files you're
    actively touching (~5-9 s).  `scripts/lint.sh <file>` for a single
    named file (~4.6 s).  `--dirty` is an explicit synonym for the
    default.
  - **Gate (explicit):** `scripts/lint.sh --branch` re-lints the
    **entire** branch diff vs `origin/master` plus the working tree
    (~20 files, ~73 s).  Run it once before committing / opening a PR,
    not in the loop.

Before every commit run, in order:

  1. `scripts/lint.sh --branch` — the full-branch gate.  `clang-format
     -i` + `clang-tidy`; non-zero exit on any clang-tidy warning.  Run
     `scripts/refresh_compile_db.sh` first if `compile_commands.json` is
     stale or missing; analysis without it is partial.  (First lint on a
     cold/fresh checkout also populates external symlinks via a one-time
     build — see "Dev-loop performance".)  In the loop, use bare
     `scripts/lint.sh` (working-set only) after each edit.
  2. `bazel test //<role>/<touched-package>` in the loop (e.g.
     `//compiler/codegen:...`, `//eval:...`, `//runtime:...`); the bare
     full sweep over `$PROJ` (see "Build & run") is a ~10-min from-cold
     run — do it (or rely on CI) at the gate, not per edit.  See
     "Dev-loop performance".
  3. Update `doc/implementation-plan/testing-checklist.md` and the
     active milestone doc (see "Authoritative docs" above).

**Running lint correctly — and the PCH gotcha.**  `scripts/lint.sh`
always tries to use a precompiled header (PCH) to amortise the absl +
protobuf header parse across every C++ TU it touches; without it,
clang-tidy is so much slower that the full repo lint pass becomes
unusable, **and the warning set itself changes**:

  - The PCH lives at `.lint-cache/lint_pch.h.pch` and is built from
    `scripts/lint_pch.h` (the union of absl + protobuf headers
    referenced by the first-party C++ tree).
  - `scripts/build_lint_pch.sh` rebuilds the PCH iff `lint_pch.h` or
    `compile_commands.json` is newer than the cached PCH.  Update
    `lint_pch.h` whenever a new absl / protobuf header lands in the
    first-party tree; run `scripts/refresh_compile_db.sh` after a bazel
    dep update so the PCH rebuilds against the right paths.
  - **PCH NOT loading is silent and changes warnings.**  When PCH
    fails to load (most commonly because the build/release
    mismatched the clang-tidy version, or because lint.sh's
    `-include-pch <path>` flag form regresses), clang-tidy still
    finishes — but it emits a different warning set.  Specifically:
    `cppcoreguidelines-pro-type-member-init` false-fires on every
    `std::string` / `std::vector` / `std::shared_ptr` field
    "without an initializer", because clang-tidy can't see the
    default ctors of those types.  If you start seeing a flood of
    `pro-type-member-init` warnings, **check the PCH is loading**
    before fixing them: a clean lint with PCH loaded will not flag
    those fields, and adding NSDMI defaults (`field_{}`) will
    instead trip `readability-redundant-member-init` once the PCH
    is restored.  The canary is the PCH error in the lint output:
    `'-pch=...' file not found` means PCH isn't loading; absence of
    that error means it is.

The two-argument form `--extra-arg-before=-include-pch` +
`--extra-arg-before=<path>` (as two separate flags) is the correct
way to pass clang's `-include-pch` through clang-tidy; the joined
form `--extra-arg-before=-include-pch=<path>` is parsed by clang's
driver as `-pch=<path>` (the `-include` prefix is stripped before
the equals splitter sees it) and the PCH never loads.

**Function-size gate.**  `readability-function-size` is on with tight
thresholds (60 lines / 40 statements / 15 branches / 6 params / 5
nesting levels).  Functions do ONE thing and are short enough to
review without scrolling.  When the linter flags a function, split
it — do not `// NOLINT` around it.  Known exceedances are tracked in
`doc/implementation-plan/lint-backlog.md`; clear them before adding
new code in the same file.

Full workflow and install steps are in `doc/contributing.md`.

## Probe vendored cel-cpp before bringing in a CEL language feature

When a milestone imports a CEL language feature — an extension
library (`strings`, `encoders`, `math_ext`, …), a new builtin
overload, a macro, or any spec-defined semantics — **cel-cpp is
the source of truth, and you read it directly before you write the
design doc.**  Do not plan from memory of the spec: overload-id
strings, error messages, alphabet/padding rules, and edge-case
behaviour are exactly the things memory gets subtly wrong, and a
wrong overload-id string fails late (codegen `not found in
OverloadTable`) or, worse, ships a conformance-diff.

cel-cpp is fetched on demand (only `third_party/cel-cpp.sha` is
committed).  Fetch it, then read the real source:

```bash
third_party/fetch_cel_cpp.sh          # clones at the pinned SHA
grep -n "MakeOverloadDecl\|InvalidArgumentError\|absl::Base64" \
    third_party/cel-cpp/extensions/<feature>.cc
```

For anything a grep can't settle — does the type-checker actually
stamp this overload id? does this absl primitive accept the
unpadded input the corpus feeds? — **write a throwaway probe**
under `compiler/probes/<milestone>/` (created ad hoc per milestone;
there is no committed `probes/` dir — past milestones' probes were
deleted at closeout, which is the discipline below).  A probe is a small
`cc_test` that links the real cel-cpp library (or our pipeline)
and asserts the assumption — e.g. compile `base64.encode(b'x')`
and print `annotations_[id].overload_id`, or feed a value through
`absl::Base64Unescape` and check the result.  Probes are
disposable: keep them while the milestone is in flight, delete
them at closeout (they are NOT permanent regression tests — those
live in the per-component test suites).

Record what each probe confirmed in the milestone design doc with
a dated callout, citing the cel-cpp file:line it came from
(see `rewrite/m17-encoders-ext.md` §2 for the pattern).  A design
doc that asserts a spec fact without either a cel-cpp citation or
a probe behind it is a guess, and gets sent back.

Why this is mandatory, not a nice-to-have:

  - **Conformance is byte-exact against cel-cpp.**  Error
    messages, canonical string forms, and rounding all have to
    match upstream verbatim; the only reliable way to get them
    right is to copy them from the source, not approximate them.
  - **Overload ids come from cel-cpp's `Reference`.**  Our
    `overload_table.cc` seeds must match the strings the checker
    library stamps — which are defined in cel-cpp's
    `MakeOverloadDecl` calls, readable in one grep.
  - **The cheap check now beats the expensive failure later.**  A
    5-minute fetch + grep + probe replaces a debugging session
    that starts at a codegen error or a conformance regression
    four layers downstream.

### The oracle is the empirical tiebreaker — reading cel-cpp source is not always enough

`testdata/cel_cpp_oracle_test.cc` (+ `cel_cpp_oracle.{h,cc}`) links the
**real cel-cpp parser + checker + runtime** and evaluates an expression
end-to-end (`EvalWithCelCpp(source, container)` → `OracleResult`).  It
is the **authoritative answer** to any "what is the correct result?"
question — value, canonical string form, error vs. value, rounding,
overflow, heterogeneous-equality edge — and it OUTRANKS a guess derived
from reading the source.

**Reading cel-cpp source tells you what the code does at one site;
the oracle tells you what the whole pipeline actually produces.**
The two diverge more often than you'd expect: a behavior is the
emergent product of parser desugaring + checker reference-resolution +
runtime dispatch + a registered extension's options, and eyeballing
one `.cc` misses the interaction.  Several times a "the source clearly
says X" conclusion has been wrong because a layer upstream rewrote the
input first.  So:

  - **For any behavioral uncertainty, ADD A CASE TO THE ORACLE TEST
    and run it** — that is the fastest way to settle the question, and
    it leaves a permanent regression pin (cite the oracle case in the
    test that depends on the fact).  Do this BEFORE writing the design
    doc or the assertion; an assertion whose expected value you
    *reasoned out* rather than *oracle-confirmed* is a guess.
  - **When the conflict is "codebase comment / memory / spec-reading
    says A, oracle says B," the oracle wins** — cel-cpp is the
    reference implementation and conformance is scored against it.
    (This is how `int(-2^63.0)` was settled: a code comment claimed
    cel-cpp admits `INT64_MIN`; the oracle returned a range error;
    the oracle won and the conversion path was fixed to match.)
  - **The oracle has gaps — extend it, don't fall back to guessing.**
    Today `EvalWithCelCpp` takes only `(source, container)`: it has no
    activation bindings and no unknown-attribute / partial-eval path,
    so partial-eval questions (e.g. "comprehension over an unknown
    range") can't be asked through it yet.  When a question needs a
    surface the oracle lacks, the correct move is to **extend the
    oracle** (add the binding / unknown-pattern parameter) or write a
    **throwaway cel-cpp probe** that drives the real runtime with that
    surface — NOT to settle it by reading alone.  Record the
    confirmed fact with the cel-cpp `file:line` (see backlog #14 for
    the pattern: comprehension-over-unknown-range confirmed against
    `eval/eval/comprehension_step.cc`).

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
(`tools/wat_runner`) with stub impls for any not-yet-
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
  3. `bazel test $PROJ` is green.

**Testing principles.**  This codebase is a pipeline — frontend → IR →
codegen → runtime/host.  Each stage is a component, and any non-trivial
change touches several of them.  Apply these rules every time:

  - **Strict author order: interface → tests → implementation.**  When
    coding anything, write the interface (`.h`) first; then write the
    tests *fully* — the complete positive + negative + boundary matrix —
    against that interface, each test body present but `GTEST_SKIP()`'d
    (or otherwise compiling green) *before any implementation exists*;
    then write the implementation, deleting the skip on each case as its
    path goes green.  Do NOT write the `.cc` before the tests — the
    tests are the spec the implementation satisfies, and writing them
    first is what stops the implementation from quietly defining its own
    (wrong) contract.  **Every individual source file gets its own
    `_test.cc`** — a new `.h`/`.cc` pair without a paired test is
    incomplete, even if an end-to-end test happens to cover it.
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
    rather than clarifying it.  See `runtime/cel_map_test.cc`
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

## Reporting & tracking bugs / gaps via tests

This is how a fixable bug or conformance gap enters and leaves the
codebase.  **Every bug and every behavior gap is accounted for by an
explicit test case — never by a silent omission, a TODO, or an
untracked FAIL.**  A reader (or `grep`) must be able to see, for any
behavior, either a passing test that pins it or a skipped test that
names why it isn't supported yet.

**1. One test case per bug / per conformance row.**  When you fix a
bug or unlock a conformance row, add a `TEST`/`TEST_F`/`TEST_P` that
exercises the *exact* failing input (for conformance, the literal
`expr:` from the `.textproto` row) and asserts the now-correct result
(value, or expected eval-error kind).  Name it after the bug /
conformance row (`<fixture>_<row_name>`) so the test↔row mapping is
greppable.  Put behavior that flows through the whole pipeline in an
`e2e/*_test.cc`; put kernel-level behavior in the
component's `*_test.cc`.

**2. Verified-dead / not-yet-supported → `GTEST_SKIP() << "<reason>"`,
never omission.**  If a row or behavior cannot pass today, still write
the test case, and `GTEST_SKIP()` it with the reason you
**verified** — naming the concrete blocker, not "not done":

```cpp
TEST_F(M18NetworkExt, OptionalChaining1) {
  GTEST_SKIP() << "blocked on the map.field selection gap "
                  "(cleanup-backlog #9); `{'c':{...}}.c` reaches as a "
                  "kSelectExpr the runtime can't index yet";
  // ... the expression + assertion, ready to un-skip when the blocker lands.
}
```

Legitimate skip reasons are things you have confirmed: out of the
static subset by design (`RejectDyn`), parse-only eval
(`disable_check`), a named upstream/sibling gap with a tracking
reference, an unimplemented later-milestone surface.  "This whole
feature isn't done" is **not** a reason — that is the silent-skip
anti-pattern that let M2 ship 29 dead `GTEST_SKIP`s (see
`per-component-test-coverage.md`).  Never `GTEST_SKIP` a fixture's
`SetUp`; skip the individual case so the rest of the fixture still
runs.

**3. The skip is a live TODO with the un-skip recipe baked in.**  A
skipped case carries the assertion it *will* make, so closing the
blocker is "delete the `GTEST_SKIP` line and confirm green."  When a
blocker is cleared, the commit that clears it removes the matching
`GTEST_SKIP`(s) — a skip that lingers after its blocker is gone is a
review finding.

**4. Conformance FAILs are bugs too.**  A row that FAILs (not SKIPs)
is an untracked bug until it has a focused test case pinning the
current-vs-expected gap — either fix it (case passes) or, if it's a
genuine harness/scope limitation, convert it to a reasoned
`GTEST_SKIP` AND record it in `cleanup-backlog.md`.  Do not leave a
bare FAIL with no test documenting it.

## Build & run

  - **`bazel build //...` works** — and is the preferred "whole project"
    pattern.  It used to die on a package-loading error (the vendored
    `third_party/cel-cpp/tools/testdata/BUILD` loads
    `@com_github_google_flatbuffers`, a repo not in our `MODULE.bazel`),
    because `third_party/cel-cpp` is a `local_path_override` module whose
    directory the main repo still globbed into.  Fixed by `.bazelignore`
    (it lists `third_party/cel-cpp`): the main repo no longer discovers
    that vendored module's packages, while `@cel-cpp//...` still resolves
    via the module override.  `//...` builds all first-party packages
    (the role dirs, `//third_party/{wasmtime,wasi_sdk,binaryen}`, the
    `doc/**` probes) — 170 targets, all green.
  - **`$PROJ` — the explicit project-package set** — still defined and
    valid; it predates the `//...` fix and remains handy when you want
    *only* the role packages (excluding the `doc/**` probe targets `//...`
    now also builds).  It enumerates:

    ```
    //compiler/... //eval/... //shared/... //abi/... //runtime/... \
    //tools/... //conformance/... //e2e/... //benchmark/... //testdata/... //spec/...
    ```

    `//...` is fine for `rdeps` / `visible` query universes now too.
  - Primary build: `bazel build //...` (or `bazel build $PROJ`).
  - Primary tests: `bazel test //...` (or `bazel test $PROJ`).
  - CLI: `bazel-bin/tools/cel/...` (see `tools/cel`).
  - The wasm32-wasi cross-compile is handled by a bazel-registered
    `@wasi_sdk_<host>` toolchain (the host system clang has no wasm32
    target).  The repo builds on both macOS (Apple Silicon) and Linux
    (arm64 / x86_64): bazel auto-detects the host C++ toolchain for the
    native build and resolves the matching wasi-sdk archive for the
    wasm cross-compile by `exec_compatible_with`.  No host-specific
    setup beyond bazel itself.
  - Shared proto fixtures live at `//testdata`.  The legacy V1 compiler
    tree was deleted (2026-05-24), and the `compiler_v2/` umbrella that
    succeeded it was dissolved into the top-level role dirs (2026-05-25).

## `bazel/` vs `third_party/`

  - **`third_party/`** is the external-dependency integration layer: the
    cel-cpp fetch, Binaryen + wasmtime BUILD glue, the wasi-sdk toolchain,
    and any upstream patches.  Do not edit files under
    `third_party/cel-cpp/` — it is vendored; a bug there gets an
    upstream-style patch under `third_party/patches/`.
  - **`bazel/`** is reserved for *first-party*, dependency-independent
    Starlark (a reusable macro or rule we author, e.g. a future
    `cel_wasm_embed` rule or a `wat_test` macro).  There is no `bazel/`
    dir today — we have almost no such macros (the wasi-sdk `.bzl` lives
    with its dep; `antlr_cc_library` is borrowed from `@cel-cpp//bazel`).
    When the first reusable first-party macro appears, it goes in a
    top-level `bazel/` mirroring cel-cpp — never scattered into package
    dirs, and never in `third_party/`.

## Visibility regime (standing rule)

The public/internal boundary is **`internal/` + Bazel `visibility`** (the
Abseil/cel-cpp convention), enforced at analysis time — a bad `deps` edge
fails the build.  The model is **two-tier**:

  - **Public API** — a curated, small set of targets carrying explicit
    `//visibility:public`.  Exactly: `//compiler:{compiler,program}`;
    `//eval:{engine,instance,activation,value,error,attribute,
    host_call_context,typed_function}`; `//shared:type`; `//abi:*`;
    `//runtime:*`.  This is what a future `bindings/` or any external
    consumer may depend on.
  - **First-party internal** — everything else, scoped to the root
    `//:internal` `package_group` (defined in the root `BUILD.bazel`,
    listing every first-party package).  The compiler pipeline components
    (`frontend`, `ir`, `codegen`, `celfn`, `compiler/internal`) and the
    eval internals (`cel_host*`, `abi_decode`, `instance_impl`, the
    wasmtime glue) default to `//:internal`: reachable by any first-party
    package (the real intra-project wiring — `abi` → `codegen`, `eval` →
    `ir:annotations`, …), NOT by `bindings/` or an external consumer.

Rules for contributors:

  - You may **not** widen a target to `//visibility:public` without
    review — the public surface is a contract, and growing it is a
    reviewable event.
  - Implementation targets stay on `//:internal` (or a narrower
    component scope).  Do not add a `//visibility:public` to make a
    one-off dep compile; wire it through `//:internal` instead.
  - `internal/` subdirs are the readability signal that pairs with the
    visibility scope — keep private code under `internal/`.

## Benchmark configuration

Benchmarks measure production-shape numbers, so every layer they touch
runs at the **highest available optimization level** — anything less is
a noisy / pessimised baseline that doesn't represent what an embedder
ships.  The three layers and their flags:

  - **Bazel config.**  Always `bazel run -c opt //benchmark/<pkg>:<target>`
    (or drive everything via `benchmark/eval/run.sh` /
    `benchmark/compiler/run.sh`); the fastbuild inner-loop config is for
    dev cycles, not measurement.
  - **wasm32-wasi cross-compile (`cel_runtime.wasm`).**  `-O3 -flto`
    via the wasi-sdk toolchain (`opt_feature`,
    `third_party/wasi_sdk/cc_toolchain_config.bzl:199`) and explicit
    `-flto` in `runtime/BUILD.bazel`'s wasm `cc_binary` (both copts
    and linkopts).  LTO is load-bearing — it lets the linker inline
    helpers like `cel_int_eq_at_vv` into `cel_list_in`'s scan loop
    across TU boundaries; without it, the hot loop pays a non-inlined
    call per element.  The native `cc_library` for `cel_runtime` has
    had `-O3 -flto` since 2026-05-15; **the wasm side gained `-flto`
    when this rule was added.**
  - **Expr-module wasm (Binaryen-generated from CEL source).**  Every
    bench passes `CompilerOptions::optimize_level = 2` (the highest
    Binaryen level we wire up today — the field accepts `[0, 3]`;
    when level 3 is validated, raise the constant).  Default-config
    `Compile()` is `optimize_level = 0`, which means an
    unoptimized expr module — fine for tests, **never for benches**.
    The canonical seam is the `kBenchOptimizeLevel` constant + a
    `CompileOrDie` that always sets `opts.optimize_level =
    kBenchOptimizeLevel`; see `benchmark/eval/celwasm_bench.cc` for the
    pattern.

When a bench reports a number, it's the production-config number.
Comparison benches (e.g. `BM_*_Opt2` paired with the unoptimized
variant in `benchmark/compiler/pipeline_bench.cc`) are an explicit
deviation — they exist so a reviewer can read the optimization delta
in one table.

Eval-comparison results are **auto-published**: `benchmark/eval/run.sh`
archives dated tables under `benchmark/eval/results/` and regenerates
the Results section of `benchmark/README.md` via
`benchmark/eval/report.py`.  Never hand-edit published numbers — re-run
the harness.

## Dev-loop performance (read before you wonder why it's slow)

Full analysis + numbers: `doc/implementation-plan/dev-loop-performance.md`.
The build dominates everything — of a 618 s `bazel test $PROJ`,
only ~45 s is test execution; the rest is compiling cel-cpp from source.
So:

  - **Stay in ONE configuration.**  Dev, `bazel test`, and the conformance
    gate all run in the **default (fastbuild)** config.  Do **NOT** run
    conformance or anything else under `-c opt` in the inner loop: `opt`
    is a *separate* build tree that shares nothing with fastbuild, so
    switching recompiles cel-cpp (~10 min).  `-c opt` is for
    `//benchmark/...` and CI only.  The conformance gate
    (`scripts/check_conformance_monotonic.sh`) deliberately runs
    fastbuild — pass count is identical to opt (verified 1774==1774).
  - **Don't `bazel test $PROJ` in the inner loop.**  Build/test
    the touched package (`bazel test //runtime:cel_foo_test`).
    The full sweep is a "from-cold" cost; Bazel's local cache makes
    targeted re-runs instant.
  - **Lint is fast on a warm tree, slow on a cold one** — because
    `build_lint_pch.sh` populates external symlinks via a `bazel build`
    that, when symlinks are missing, becomes a full cel-cpp compile
    (measured 609 s for 2 files).  It's guarded to skip on a warm tree.
    If lint hangs, your tree is cold — warm it with a normal build first.
  - **The pre-push hook runs the conformance gate.**  On a cold cache it
    runs long enough that GitHub's SSH connection idle-times-out and the
    push silently fails to land (the gate passes, the transfer drops).
    Warm the tree first, or — only when the gate has already been verified
    green separately and the change can't affect conformance (docs,
    `.bazelrc`, scripts) — `git push --no-verify`.
  - **Cross-checkout build cache is OPT-IN** (`user.bazelrc`,
    `--disk_cache=…`), never committed-on-by-default: the system
    Apple/brew toolchain is non-hermetic, so a cache outliving a
    compiler/SDK bump can serve a stale object.  If you enable it,
    `bazel clean` (or wipe the cache dir) whenever you upgrade Xcode or
    brew llvm.  Each checkout otherwise gets its own output tree (Bazel
    keys it by workspace path), so prefer **one** checkout over several.

## Periodic code review (every few commits, every milestone closeout)

A compiler accretes tech debt faster than you notice: a shim that
was "temporary for the MVP" turns load-bearing; a header gains
fields no one calls; a comment block describes the design from
two refactors ago.  The forcing function against this is a
**recurring code-review pass**, run holistically against a batch
of commits — not one commit at a time, where the drift is
invisible.

**Cadence.**  Trigger a review pass whenever ANY of these are true:

  - A milestone (M-numbered or a Phase A/B/C slice) just shipped.
  - 5+ commits have landed on a feature branch since the last
    review.
  - A long-lived branch (`wasi-malloc-migration`, anything
    expected to merge in >1 week) is about to merge to master.
  - A planning doc closed out (per "Closing out a planning doc"
    above) — the review verifies the closeout is honest.

**Scope of each pass.**  The reviewer agent reads:

  1. The commit range (`git log --oneline <prev-review>..HEAD`).
  2. Every design doc that the commits touched or referenced
     (`doc/implementation-plan/**`, especially `wasi/DESIGN.md`,
     `rewrite/design.md`, the relevant `m<N>-*.md`).
  3. The actual diff (`git diff <prev-review>..HEAD`).
  4. The headers + .cc/.c of every modified component, plus
     **one neighbouring component the reviewer picks** to catch
     "drift by adjacency" — fields/macros added in one TU that
     leak idiom changes into a sibling TU.
  5. The lint backlog (`doc/implementation-plan/lint-backlog.md`)
     and the per-component coverage doc — if either grew, the
     reviewer flags why.

**What the reviewer must produce.**  A dated markdown report
under `doc/implementation-plan/rewrite/wasi/reviews/YYYY-MM-DD-<slug>.md`
(or the equivalent reviews dir for the active workstream), with:

  - **Architectural drift** — places the as-built shape diverges
    from the as-designed shape; flag whether the doc or the code
    is the right correction.
  - **Tech-debt inventory** — compat shims still in place, dead
    code, headers with unused fields, stubs that grew bodies,
    comment blocks describing old designs.  Each entry has a
    severity (P0 ships-breaking, P1 must-fix-before-next-milestone,
    P2 cleanup-when-touched) and an estimated effort.
  - **Coverage gaps** — files/functions that landed without
    tests; spec citations missing from tests that assert
    spec-mandated behaviour.
  - **Doc drift** — sibling design docs that reference the old
    architecture and weren't updated in the same commit; stale
    plan docs that should be closed out.
  - **One-paragraph summary** at the top with the verdict
    (clean / dirty / mixed) and the top 3 items the user
    should look at first.

**Tracking what the review surfaces.**  Findings do NOT live
only in the report — they get woven into the work queue:

  - P0/P1 items become bullets in the active milestone's
    "Pre-close cleanup" section, ticked as they're addressed.
  - P2 items go into `doc/implementation-plan/cleanup-backlog.md`
    (create on first use), tagged with the originating review
    date so the trail back to context is preserved.
  - When a future commit addresses a backlog item, the commit
    message cites the review (`addresses cleanup-backlog #N
    surfaced in YYYY-MM-DD review`).

**Running a review.**  Use the Agent tool with `subagent_type:
general-purpose` (or `Explore` for a research-only pass).
Brief the agent with: the commit range, the doc roots to read,
the report path to write, and a reminder that the agent
**writes the report only — it does not change code**.  Code
changes happen in the follow-up commits the user authorises
after reading the report.

**Why this matters.**  Skipping a review pass for "just one
more milestone" is how `M2` shipped with 29 silent
GTEST_SKIPs.  The review is the cheapest way to catch
architectural drift before it calcifies into a rewrite.

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
  - **Don't create work for later milestones without being explicitly
    asked.**  Stay inside the slice / milestone the user asked for.
    Don't draft "follow-up" plan docs, file deferred-work bullets,
    spawn planning subagents, or write `// TODO(M7): ...` callouts
    unprompted.  If something genuinely belongs in a later milestone
    and you discover it mid-slice, surface it once in your end-of-turn
    summary so the user can decide; don't pre-commit to it in a doc
    or task list.  The user owns scope; jumping ahead burns their
    context and dilutes the current slice.
