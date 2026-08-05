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
    The `manual` set is now scoped to targets that are genuinely
    expensive: every remaining one takes 33 s or more, and everything
    at or under ~14 s runs in the default sweep.  `manual` is a
    RUNTIME-COST exemption only — never a way to keep a fragile or
    inconvenient test out of CI.  Before adding it, measure with
    `bazel test --nocache_test_results <target>`: cached "PASSED in
    Ns" lines report the last recorded run under whatever load the
    machine was under, and were overstating several targets by 3-4x.

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
  - **A COLD TREE ALSO INVENTS WARNINGS, and they look real.**  Bazel
    only materialises `execroot/_main/external/` during an actual
    build, so on a tree where the relevant targets haven't been built
    the compile DB's include paths point at nothing.  clang-tidy then
    analyses a *recovery AST* and emits a flood of plausible-looking
    findings — `readability-non-const-parameter` on ~20 pointer
    params, `bugprone-branch-clone`, `misc-use-internal-linkage` on
    real exports — none of which reproduce once the tree is warm.
    The canary is a `'...' file not found` line (`gtest/gtest.h`,
    `binaryen-c.h`) anywhere in the output: if you see one, **stop and
    warm the tree** (`bazel build //...`, then
    `scripts/refresh_compile_db.sh`) before touching a single
    finding.  Do not "fix" warnings from a cold tree — the fixes are
    changes to correct code.

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

### Probe our own surfaces too — and delete the ones with no working setting

The two sections above are about *cel-cpp's* behaviour.  The same
discipline applies to **our own** options, flags, and public fields,
where the failure mode is quieter: nobody questions a knob that has
been in the header for a year.

**A hedge in a doc is an unrun experiment.**  When a design note says
"plausibly fails", "may be a no-op", "probe pending", or "the knob may
be deleted" — that is not a finding, it is a five-minute experiment
somebody deferred.  Run it before you write another word of prose
around it.  `mem_size_bytes` was diagnosed correctly in four separate
findings (R6, R8, R72, V8), each with the probe recipe spelled out,
and survived for months because the probe was never run; every pass
over the docs added more careful hedging instead.

**Probing our own code is usually cheaper than reading it.**  The
whole experiment is a `cc_test` that exercises the surface two ways
and prints the difference:

  - "Does this option affect the output?" — compile the same
    expression at two settings and compare `wasm_bytes()`.  Identical
    bytes is proof, not evidence.
  - "Does this option work at all?" — carry it through to
    `Engine::Plan` / `Instance::Eval`, not just `Compile`.  The
    `mem_size_bytes` failure was invisible at compile time and only
    appeared as `incompatible import type` at Plan.

Same rules as any probe: it lives under `compiler/probes/<milestone>/`,
it is deleted at closeout, and what it proved is recorded in the
milestone doc.

**Deletion is a legitimate — often the correct — outcome.**  When a
probe shows a surface has *no working configuration*, delete it; do
not document the caveat.  Documenting is the tempting move because it
is smaller, and it is how a dead knob acquires four accurate
paragraphs across four pages and stays dead.  The test to apply: *is
there any value a user could set here that does something useful?*  If
no, the option, its plumbing, its flag, and its prose all go in one
commit.  If yes, document the real envelope.

The same test retires unimplemented surfaces.  A public method whose
body is `ABSL_CHECK(false)` with no milestone that owns it is not a
promise, it is a crash with documentation — implement it or delete it
(`Activation::OverrideFunction` sat declared for months after the plan
doc had already sanctioned removing it).

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

Two halves, and both are mandatory: rules 1-5 say how a bug is
**tracked**, rule 6 says how it is **handled**.  Tracking without
handling is how a P0 lives on behind a tidy `GTEST_SKIP` —
see the exemplar at the end of this section.

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

**5. The regression test MUST be observed FAILING before you pin
it, and the skip message is a MACHINE-PARSEABLE `CELBUG` block.**
Write the case asserting the SPEC-CORRECT behaviour, run it against
current code, and confirm it fails for the reason you think it does
— then add the `GTEST_SKIP` carrying the block below.  Prose skip
messages are no longer acceptable: the pin file is the bug tracker,
so it has to be readable by a script (and by an agent sent to fix
it), not just by a human scrolling.

```cpp
GTEST_SKIP() << R"CELBUG(CELBUG v1
id: CELW-0002
severity: P1
kind: over-permissive
summary: we accept duration(<int>); cel-cpp's runtime rejects it
repro: duration(1)
bindings: none
actual: a duration value of 1s
expected: an evaluation error ("No matching overloads found : duration(int64)")
layer: compiler/codegen/overload_table.cc (drop the int64_to_duration seed)
blocked-by: none
found-by: e2e/fuzz mine_divergences duration seed=4 depth=4
fix-hint: cel-cpp's CHECKER declares the overload
  (checker/standard_library.cc:369) but its runtime never registers an
  impl, so it type-checks then fails at eval.  Conformance scores against
  the runtime, so OUR acceptance is the non-conformant side.
issue: none
)CELBUG";
```

**Use the `R"CELBUG( … )CELBUG"` delimiter, not a bare `R"( … )"`.**
A plain raw string ends at the first `)"`, and pin text quotes
upstream error messages that routinely contain one (e.g.
`… duration(int64)")`), which silently truncates the block and then
fails to compile several lines later.

Fields.  Required: `id` `severity` `kind` `summary` `repro` `actual`
`expected` `layer` `blocked-by` `fix-hint`.  Optional: `bindings`
(the activation needed to run `repro`), `found-by` (fuzz seed /
conformance row / manual — the provenance that lets anyone
re-reproduce), `issue`, `status`.

  - `id` — `CELW-NNNN`, monotonically assigned, never reused.  Stable
    independently of any tracker; `scripts/bug_pins.py list` shows the
    highest in use.
  - `severity` — `P0` / `P1` / `P2` exactly as rule 6 defines them.
  - `kind` — `wrong-value` `missing-feature` `crash` `over-permissive`
    `diagnostics` `precision`.
  - `repro` — the SHORTEST expression that fails, reduced from
    whatever found it.  A 400-character fuzz source is not a repro.
  - `layer` — file (and symbol) where the fix goes, so the reader
    doesn't re-derive it.
  - `blocked-by` — `none`, or a comma-separated list of `CELW-` ids
    that must land first.  This is what makes the queue orderable;
    `bug_pins.py validate` fails on a dangling reference.
  - `fix-hint` — what a fixer needs that the diff won't tell them:
    the upstream citation, the wrong assumption, the trap to avoid.
    Continuation lines are indented and may contain colons.

Tooling — `scripts/bug_pins.py`:

```
scripts/bug_pins.py list         # the queue, severity-ordered
scripts/bug_pins.py validate     # CI gate: malformed / duplicate id / dangling blocked-by
scripts/bug_pins.py json         # for other tools
scripts/bug_pins.py issue CELW-0002   # render a tracker-ready issue body
scripts/bug_pins.py unmigrated   # skips still carrying prose messages
```

GitHub issues are currently DISABLED on `augustinemathew/cel-wasm`,
so `issue:` is `none` everywhere and the pin file IS the tracker.
If issues are enabled later, `bug_pins.py issue <ID>` renders the
body and the `issue:` field records the number — no format change
needed, and the pins stay the source of truth either way.

**Not every skip is a bug — use `CELSKIP` for the ones that
aren't.**  Roughly two thirds of this repo's skips are the system
working as designed: a static-subset (`RejectDyn`) rejection, a
scope boundary a milestone doc permanently decided, or a limitation
of the test harness rather than the product.  Those get the lighter
block, and they stay OUT of the bug queue so the queue stays
trustworthy:

```cpp
GTEST_SKIP() << R"CELSKIP(CELSKIP v1
reason: by-design
why-not-a-bug: an empty map literal types as map(dyn, dyn), and the static
  subset rejects dyn before codegen runs; the runtime path is covered by
  runtime/cel_map_test.cc
citation: doc/implementation-plan/rewrite/design.md (RejectDyn)
)CELSKIP";
```

`reason` is one of `by-design` / `harness-limit` /
`deferred-feature`; `why-not-a-bug` and `citation` are required —
a claim of "by design" without a pointer to the decision is just an
unexamined skip.  `bug_pins.py skips` lists them;
`bug_pins.py validate` checks them.

**How to write a pin — the procedure.**

  1. **Reduce the reproducer first.**  Whatever found it (a 400-char
     fuzz source, a conformance row) is not the repro.  Bisect down
     to the shortest expression that still fails, and probe the
     neighbours — the difference between "concat is broken" and
     "concat is broken whenever an operand is host-origin" is the
     difference between an unactionable pin and a fixable one.
  2. **Run it and watch it fail**, for the reason you think.  Copy
     the real observed output into `actual` verbatim; never
     paraphrase it from memory.
  3. **Decide bug or not-a-bug.**  If the current behaviour is
     correct-by-design, write `CELSKIP` with a citation and stop.
  4. **Assign severity** by rule 6 — and remember that *silently
     wrong outranks loudly wrong*.
  5. **Pick the next free `id`** (`bug_pins.py list` shows the
     highest in use) and set `blocked-by` if another pin must land
     first.
  6. **Write `fix-hint` for someone who has not read this
     conversation** — the upstream citation, the wrong assumption
     that caused it, the trap to avoid.  Assume the reader is an
     agent that will be handed this text and nothing else.
  7. **Run `scripts/bug_pins.py validate`** before you commit.
  8. **If it is a P0, rule 6 applies** — dispatch the fix; the pin
     is not the deliverable.

A pin whose assertion was reasoned out rather than executed is a
guess, and a pin that passes by accident is worse than no pin — it
reads as coverage while testing nothing.  If the failure is not
obvious, write a throwaway characterisation probe first (a
temporary `TEST` that prints the actual result for several nearby
inputs), read it, then delete the probe and write the real pin from
what you saw.  That is how the host-origin family below was
characterised: probing `arena+arena`, `arena+host`, `host+arena`,
`host+host` turned "concat is broken" into "every host-origin
operand fails," which is what made the fix tractable.

**6. Triage every bug by severity — a P0 gets FIXED, never just
pinned.**  Pinning is how a bug is *tracked*, not how it is
*handled*.  Classify on the spot:

  - **P0** — missing functionality reachable from ordinary input,
    a silently WRONG value, or a crash / trap.  **Do not
    pin-and-move-on.**  Fix it in the same session, or **dispatch an
    agent to fix it** (Agent tool, `subagent_type: general-purpose`,
    same briefing discipline as "Periodic code review" below: name
    the definition of done, the layers to touch, and the verification
    command).  A P0 may be pinned *in addition*, so the regression is
    guarded once fixed — but the pin is never the deliverable.
  - **P1** — wrong only on unusual input, or an over-permissiveness
    where we accept what cel-cpp rejects (e.g.
    `PbtIntOfDurationOverPermissive`).  Pin + a `cleanup-backlog.md`
    entry.
  - **P2** — cosmetic, diagnostics, or a message-text mismatch.  Pin
    or backlog.

**Silently wrong beats loudly wrong for severity**: a path that
returns a plausible value with no error is P0 even when the input
is unusual, because nothing downstream can detect it.

**Withholding a fuzz production is a TEMPORARY companion to a fix,
never a substitute.**  When the differential fuzzer finds a P0 and
you withhold its grammar production to keep the nightly green, the
same change must dispatch or perform the fix — otherwise you have
removed the only thing that was going to find it again.

*The exemplar (2026-07-25).*  `CelListConcatImpl` was a stub that
poisoned `TYPE_MISMATCH`, so `[1,2] + xs` errored for any bound
`xs`.  It survived two months (introduced 2026-05-25, found
2026-07-25) because: (a) the stub failed silently, so
it looked like a legitimate type error; (b) `add_list` had no
first-class grammar production, so the fuzzer only ever reached it
through macro expansion, where both operands are arena-built; and
(c) nothing in this section obliged anyone to FIX rather than pin.
A follow-up sweep found four more of the same family — including
`xss == xss` returning `false`, a reflexivity violation.  All were
P0 by the rule above.

## Breaking public APIs is allowed — this is pre-1.0

**There are no pinned external consumers.**  No release tags, no
published package, no downstream repo builds against these headers.
The entire cost of a breaking change is fixing the in-tree call sites,
and the build finds every one of them for you.  So:

  - **Change the signature; fix the callers in the same commit.**  Do
    not add an overload beside the old one, a `*_v2`, a deprecation
    shim, a compat alias, or a "legacy path kept for existing
    callers".  There are no existing callers but us.
  - **Two APIs that do the same thing is a defect, not a migration
    step.**  When verification shows a surface is wrong, *replace* it.
    Adding a second, correct API next to the wrong one doubles the
    surface, doubles the docs, and guarantees somebody picks the wrong
    one.
  - **Delete rather than deprecate.**  A method nobody should call is
    removed, not annotated.  If it turns out to be needed, git has it.

This is the posture that let `Find` become `Resolve` (a lazy binder
needs a status channel and the old signature had none — two call
sites, both fixed in the same commit), `OverrideFunction` be deleted
outright, and `mem_size_bytes` be removed from a public struct rather
than documented around.  Each of those had been sitting behind an
implicit "but it's public API" hesitation that was never real.

**What this does NOT license:**

  - **Silent behaviour changes.**  A break must be *loud* — a compile
    error, not a runtime surprise.  Changing what a function returns
    while keeping its signature is the one thing worse than changing
    the signature.
  - **Skipping the docs.**  A break lands with its doc update in the
    same commit (next section), and with the reasoning recorded if the
    old shape was ever documented as correct.
  - **Skipping the tests.**  Callers you fix include test callers; a
    test that no longer compiles gets rewritten, never deleted to make
    the build pass.
  - **Unilateral design changes.**  `PROPOSALS.md` still exists for
    decisions that are genuinely contentious (what *should* the API
    be).  "This would be a breaking change" is not by itself a reason
    to defer something to that file.

Revisit this section the moment a semver tag or an external consumer
exists — at that point the cost model inverts.

## Docs ship with the change (public API + CLI)

**A public-API or CLI change is not done until the user-facing docs
say the new thing, in the same commit.**  Triggering changes: any
public header (`compiler/{compiler,program}.h`, the `eval/` public
leaves, `shared/type.h`, `abi/*`, `runtime/*.h`) — signature,
contract, status codes, defaults, thread-safety; and anything in
`tools/cel/` a user can observe — subcommand, flag, output shape,
exit code, error message.

**There are two separate tellings, and both must be updated.**
`mkdocs.yml` sets `docs_dir: doc`, so **only `doc/**.md` reaches the
site** — `doc/user-guide/` for embedder how-to, `doc/design/` for
architecture claims.  `tools/cel/README.md` and the root `README.md`
live outside it and are GitHub-only.  Updating one is not updating
the other.

**Verify before you write.**  Check every claim against the code that
implements it, never against a sibling doc or memory.  Two docs
disagreeing is worse than a gap — the reader can't tell which to
trust — so find the code path and fix both.  And correct a promise
you can't keep rather than restating it: `BindLazy` documented a
trigger the eager marshal cannot deliver, so the contract was
replaced with the achievable one and the reason recorded
(`m36-cli-runtime-and-lazy-binding.md` §4.2).

**Keep them readable** — these pages are read by someone deciding
whether to adopt the project, not by someone auditing it.  Lead with
a runnable example.  One telling per topic; a fact needed twice gets
a link, never a second copy of a table that can drift.  Complete
sentences, not fragments or arrow chains.  State limitations plainly
and in place — an honest "not implemented" beats a hedge.  No
milestone numbers: a reader doesn't know what m21 is.

## Feature work is tracked in `PROPOSALS.md`

`PROPOSALS.md` is the running list of **changes that need a decision
before they can be made** — anything altering a public signature,
observable behaviour, the ABI wire format, or repo infrastructure.
It exists so that discovering "this API is wrong" during unrelated
work doesn't force an unscoped detour, and doesn't get silently
dropped either.

Keep it live:

  - **Add an entry** when you hit something that needs an owner
    decision instead of a patch — a public method that can't be used
    as designed, a missing surface an adopter will need, an ABI field
    that would unblock a feature.  One entry: what's wrong, the
    options with trade-offs, and the affected files.
  - **Remove an entry when it ships**, in the commit that ships it,
    and say so in the commit message.  A proposal that lingers after
    it's built is exactly as misleading as a stale plan doc.
  - **Re-check entries against HEAD before citing them.**  This file
    has been wrong before: it claimed the repo had no CI and no module
    version long after `.github/workflows/ci.yml` and
    `MODULE.bazel`'s `version` landed.  If an entry no longer matches
    the tree, fix the entry as part of whatever you were doing.

## Compilation limits

Every fixed boundary on what the compiler accepts (rodata window,
parse nesting depth, workspace slots, …) is pinned in
`e2e/limits_test.cc` (`//e2e:e2e_limits`, both link modes), which
documents each limit inline.  Rules: every limit carries a passing
case (just inside) and a failing case (past it) that asserts a **loud**
rejection (status code + message substring), never a silent
miscompile; a new limit adds a case before it ships; when a limit
moves, its case moves in the same commit.

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
    helpers like `cel_numeric_eq_at_vv` into `cel_list_in`'s scan loop
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

## CI (`.github/workflows/`)

Three workflows: `ci` (push to master + every PR), `fuzz` (nightly
07:00 UTC), `pages` (docs).

**CI is split by LINK MODE, not by package.**  That is the whole
design, and it follows from where the time actually goes — measured on
the Linux leg:

    29 e2e suites, static     36.3 min
    the same 29, dynamic       5.3 min
    97 other targets          10.0 min
    build (cel-cpp from source)  13-15 min, paid by EVERY lane

Static mode re-verifies LINKING, not semantics: the same assertions run
with the runtime compiled in rather than imported.  Those breaks come
from BUILD / toolchain changes, not from editing a kernel, so they do
not need to gate every review round-trip.

    pull_request   fast + conformance-dynamic
    push (master)  the above PLUS static + conformance-static

All lanes run in PARALLEL, so wall-clock is the slowest lane, not the
sum: ~44 min on a PR (measured), ~45-50 min on master, against ~116 min
serial — which used to exceed the 120-minute timeout outright.

Nothing is skipped on master, so a link-mode regression still cannot
reach a release; it just stops blocking the feedback a PR author is
waiting on.

**Lanes select targets by NAME and pass EXPLICIT labels:**

    static:  bazel query 'filter("_static$", tests(//...))'
    fast:    bazel query 'tests(//...) except filter("_static$", tests(//...))'

The explicit labels are load-bearing.  `bazel test //...` silently skips
`manual`-tagged targets; passing labels runs them.  That is what folds
the old `run_full_suite.sh` manual pass into the lanes instead of
appending it.  If you change the lane queries, re-check the partition —
it must be exact, and no `manual` target may fall outside both lanes.

`scripts/check_conformance_monotonic.sh` takes `--mode dynamic|static`
(default `both`, so the pre-push hook and local runs are unchanged).

**Disk.**  A cold build needs ~16 GB and a runner starts with ~21 GB.
`scripts/ci_free_disk.sh` reclaims ~20 GB of preinstalled SDKs.  Bazel
7.3.2 has NO disk-cache GC, so `--disk_cache` grows without bound and
`restore-keys` ratchets it forward run over run —
`scripts/ci_prune_disk_cache.py` caps it before the save.  Skipping
either is how CI ends up failing with `write (No space left on device)`
during an unrelated-looking dependency fetch.

**Warm image (`docker/Dockerfile.ci`, `.github/workflows/ci-image.yml`)
— built, not yet adopted by the lanes.**  It bakes a populated
`--disk_cache` so lanes skip the 13-15 min cel-cpp compile they
currently each pay.  The premise was verified before it was written: a
cache populated at one workspace path replayed at a DIFFERENT path with
1135/1135 hits, 146.0s -> 15.9s, because bazel's disk cache is
content-addressed.

When a pinned input moves (`third_party/cel-cpp.sha`, `MODULE.bazel`,
`.bazelversion`, `docker/Dockerfile`) the baked entries stop matching.
Nothing becomes WRONG — content addressing means bazel rebuilds rather
than serving a stale artifact — but CI silently returns to cold-build
speed.  `scripts/ci_check_image_stamp.sh` turns that silence into a
hard failure; the image is tagged with `scripts/ci_image_stamp.sh`, so
a moved pin asks for a tag that does not exist yet.

**Do not chain `lint` and a coverage sweep in one command.**  They build
in different configurations (`--collect_code_coverage`,
`--//runtime:instrument_wasm`), and bazel keys its output tree by
configuration, so alternating between them recompiles cel-cpp each way
(~10 min).  Order a batch as: edit -> test -> sweep -> lint -> commit,
entering each configuration once.

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

**`git fetch` first — always.**  A review or audit run against a
stale checkout produces confident findings that were fixed weeks ago,
and the stale ones are indistinguishable from the live ones in the
report.  Sync (or at minimum `git fetch` and read via
`git show origin/master:<path>`) before reading a single file, and
state the SHA the review was run against at the top of the report.  A
2026-07-25 readiness audit ran 45 commits behind and had to have every
finding re-verified; roughly a third had already been fixed upstream.

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

  - Don't commit build products.  `.wasm` binaries are outputs
    (bazel-built fixtures, probe compiles), never sources —
    `.gitignore` enforces this repo-wide.  Committed probe binaries
    once weighed ~19MB; they were deleted 2026-07-25 with the probe
    trees that produced them.  A test that needs wasm bytes builds
    them (genrule / cc_binary with the wasm toolchain) or frames
    them in-test via `//abi:wasm_binary`.

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
