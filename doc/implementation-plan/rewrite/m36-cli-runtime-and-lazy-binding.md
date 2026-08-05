# m36 — CLI run-time half (`run`, `inspect`), exit-code contract, and lazy binding

Status: in progress — drafted 2026-07-25.

> **2026-08-04:** the plugin/component portions of this doc — the
> `inspect` "plugin fns" reporting, the `run` diagnostics pointing at
> `Engine::AddComponent`, and the deferred `--module` plugin-loading
> flag — describe a backend that was removed from the tree (see
> `m39-component-removal.md`); the work is preserved on branch
> `component-functions-archive`.  The rest of the doc (exit codes,
> `run`/`inspect`, lazy binding) is unaffected.

Closes the two halves of the production-readiness gap that the 2026-07-25
readiness audit rated launch-blocking:

  1. The CLI could produce a `.wasm` and throw it away — it could neither
     execute nor describe one (`cel run`, `cel inspect`).
  2. A CEL evaluation error exited 0, so the CLI was unsafe to drive from
     a script.
  3. `Activation` shipped two public methods whose bodies abort the
     process. `OverrideFunction` is removed; `BindLazy` is implemented.

Supersedes the run/inspect portion of `cel-cli-design.md` (that doc stays
as the rationale record; the as-shipped shape is here).

---

## 1. Exit-code contract

The contract was documented as 0/1/2 in `tools/cel/README.md` but the
implementation violated it in three places: a CEL error value exited 0, an
unrecognized flag exited 1 (via `absl::ParseCommandLine`, colliding with
"expression failed"), and engine/plan failures returned 2 (the usage code).

As shipped, for every subcommand:

| Code | Meaning |
|---|---|
| `0` | Success. The expression compiled / evaluated and produced a non-error value. |
| `1` | **The expression or program failed.** Parse/check diagnostics, compile failure, `Eval` returning a non-OK status, an engine/plan failure, a result that is a CEL **error** value, or a result that is **unknown**. |
| `2` | **Usage.** Unknown subcommand, wrong positional count, unrecognized or invalid flag, malformed `--var` / `--format`, unreadable input file, a `--var` that contradicts the program's `cel.abi`. |

Rationale for the two judgement calls:

  - **A CEL error value is exit 1, not 0.** `Instance::Eval` returns an OK
    status carrying `Value::Kind::kError` for divide-by-zero, no-such-key,
    and overflow — that is the correct *library* contract (the error is a
    CEL value and is catchable by `||`/`?:`), but at the process boundary
    the expression did not produce a result. Errors go to **stderr**, so
    `$(cel eval …)` never captures an error string as if it were a value.
  - **An unknown result is exit 1.** `cel eval` binds no unknown patterns,
    so an unknown escaping to the top level means the expression could not
    be resolved from the supplied activation. Also stderr.

Flag errors reach code 2 by replacing `absl::ParseCommandLine` with
`absl::ParseAbseilFlagsOnly` + an explicit unrecognized-flag report;
`ParseCommandLine` calls `exit(1)` internally and cannot be reclassified.

## 2. `cel inspect <prog.wasm>`

Answers "what does this artifact declare?" — the operator-facing half.
Decodes the `cel.abi` custom section via `DecodeCelAbiFromWasm` (no
wasmtime, no `Engine`), so it works on a program this build could not
actually run.

Output as shipped:

```
vars:       a:int, b:int
plugin fns: none
host fns:   none
link:       static (cel.abi v1, runtime abi v4)
```

**Variable types are kinds, not full types.** `cel.abi`'s
`VariableEntry` carries only `repr` (a numeric mirror of `ir::Repr`) —
field 5 is reserved for a full `CelType` and unused. So an aggregate
shows as `xs:list`, never `xs:list<int>`. Stated rather than faked.

> **Plan-vs-execution delta — the import walk was built, reverted,
> and then made unnecessary.** The first cut added
> `DecodeImportsFromWasm` to `abi_decode` and classified import
> namespaces so `inspect` could report required functions. It was
> reverted on finding m35 specified both halves better, and m35 then
> landed (merged 2026-07-26): `//abi:wasm_binary` consolidates all
> wasm binary-format knowledge, and `cel.abi` field 8
> (`required_functions`) carries the required-function table with full
> signatures and a HOST/PLUGIN backend tag, emitted from the
> post-optimize import surface. `inspect` and `run` now read that
> field via `abi::RenderSignature`, so the CLI and `Engine::Plan`
> agree by construction. A byte-level import walk would have been a
> second, weaker source of truth — and would have desynced at
> optimize levels ≥1, where Binaryen drops unused imports.
>
> One fact from the reverted probe is still worth recording: a
> **static**-linked program also imports `wasi_snapshot_preview1` (the
> adopted runtime's own libc imports), so any future import-based
> classifier must treat that namespace as engine-supplied.

## 3. `cel run <prog.wasm>`

Evaluates a precompiled program with **no recompile**: read file →
`Program(std::move(bytes))` → `Engine::Plan` → `Instance::Eval`.
`Program` needs nothing but the bytes; `Engine::Plan` decodes `cel.abi`
itself and auto-detects the link mode.

```
cel compile "a * b + 1" --var a:int --var b:int --output p.wasm
cel run p.wasm --var a=6 --var b=7        # → 43
```

**`--var` on `run` takes values, not declarations.** The type comes
from the program's `cel.abi`, so the CLI reconstructs the full
`name:Type=value` spec and hands it to the same literal parser `eval`
uses — one grammar, two entry points.

  - `--var name=value` — the normal form; the declared repr selects the
    parse.
  - `--var name:Type=value` — still accepted, and the **only** way to
    bind an aggregate, whose full type the wire does not carry. The
    refusal names the escape hatch instead of guessing an element type.

Two usage errors are caught before evaluation rather than during it:
binding a name the program does not declare (the message lists the ones
it does), and leaving a declared variable unbound (all missing names
reported at once, rather than the first one surfacing as a
`FailedPrecondition` mid-marshal).

A program importing `@host` / `@component` functions cannot run under
the stock CLI — those are C++ in the embedder's process. `run`
translates the `cel_fn` link failure into an actionable message
pointing at `Engine::AddFunction` / `AddComponent`. When `cel.abi`
field 8 lands, `Engine::Plan` will produce that diagnosis itself and
this translation can be deleted.

**Deferred:** `--module <alias>=<path.wasm>`. m35 reshapes how
components are supplied (`Component::Load`, `Engine::Use`, selective
instantiation), so wiring `Engine::AddModule` from the CLI now would be
rework.

## 4. `Activation` — `OverrideFunction` removed, `BindLazy` implemented

### 4.1 `OverrideFunction` is deleted

Per-call function override was planned for the customs milestone and never
built; `m2-ident-select-unknowns.md` §"deferred" already sanctioned leaving
it off the API rather than shipping a stub. Deleting it also retires the
`FunctionImpl` alias, which had no other user in the repo. Function
overriding remains available at `Engine` build time
(`AddFunction` / `BindFunction`); nothing regresses.

### 4.2 `BindLazy`'s achievable contract

The header promised the binder runs "the first time the expression
references `name`". **That is not implementable at the Activation layer**,
and the promise is corrected rather than faked.

Variable marshalling is **eager**: `MarshalActivation` writes a 24-byte
`CelValue` into each declared variable's slot *before* `$eval` is invoked,
and there is no variable-read trampoline in the `cel_host` import table —
the wasm side reads a slot offset from a local, never calls back to ask for
a variable. Making the binder fire on first *reference* would require a new
host import plus a codegen change at every `kIdent`, which is a different
milestone.

What ships:

> The binder is invoked **at most once per `Eval`**, and **only if the
> compiled program declares `name`** (i.e. `name` appears in the program's
> `cel.abi`) **and the variable is not blanked by an unknown pattern**. Its
> result is memoized for the remainder of that `Eval`; a subsequent `Eval`
> re-invokes the binder. A binder that returns a non-OK status aborts the
> evaluation and that status is propagated verbatim.

This still delivers the real use case — *don't pay to materialize a
variable the program never declared* — and it is honest about the rest.

### 4.3 API change: `Find` → `Resolve`

A lazy binder can fail, and `const Value* Find(...)` has no status channel.
`Find` is replaced (not supplemented) by:

```cpp
absl::StatusOr<const Value* absl_nullable> Resolve(absl::string_view name) const;
```

Two lookup methods where one silently ignores lazy bindings would be a
footgun, and `Find` had exactly two call sites in the repo (both in
`eval/instance.cc`). Returns `nullptr` when `name` has no binding of
either kind — preserving the existing "declared but not bound"
`FailedPrecondition` at the call site.

Memoization uses a `mutable` cache cleared by `ClearLazyCache()`, which
`MarshalActivation` calls at entry so each `Eval` re-invokes binders. The
cache makes `Activation` **move-only** (`AnyInvocable` is), and it is
**not thread-safe** — one Activation per evaluation, matching `Instance`'s
documented thread-owned model.

### 4.4 Ordering constraint (the subtle part)

`MarshalActivation` reads the activation **twice**: a sizing pre-pass
(`TotalHostStringBytes`, string/bytes/type reprs only) and the per-variable
marshal. Two invariants:

  - The binder must run **after** the unknown-pattern check, or a variable
    the caller declared fully-unknown would still have its binder invoked
    (and could fail) — violating "an unknown variable need not be bound".
    The sizing pre-pass therefore skips fully-unknown variables too, which
    it previously did not need to.
  - Both read sites must share the memo, or a lazy `string` variable's
    binder runs twice per `Eval`.

---

## In progress — feature-pipeline checklist

From `feature-pipeline-checklist.md`. This change adds no AST kind, no
declarable type, and no ABI field, so §2.1/2.2/2.6 do not apply. The
applicable stages:

- [x] **Public API** — `eval/activation.h`: dropped `OverrideFunction` +
      `FunctionImpl`; `Find` → `Resolve`; honest `BindLazy` contract with
      a thread-safety statement.
- [x] **Eval plumbing** — `eval/instance.cc`: `ClearLazyCache` at marshal
      entry; unknown-check-before-binder ordering; shared memo across the
      sizing pre-pass and the marshal loop (the pre-pass now skips
      fully-unknown variables, which it previously did not need to).
- [x] **Tests (interface → tests → impl)** — `eval/activation_test.cc`
      rewritten: both death tests deleted, 18 cases covering
      invoked-once, re-invocation after `ClearLazyCache`, verbatim error
      propagation, failure-not-cached, cross-kind overwrite, move
      semantics.
- [x] **CLI exit codes** — `tools/cel/cel.cc`: a CEL error or unknown
      result now reports on stderr and exits 1; flag errors reclassified
      to 2 via `ParseAbseilFlagsOnly`; `--help` honoured in any position;
      the duplicated per-subcommand prologue factored into `PrepareCli`.
- [x] **CLI `run` + `inspect`** — `tools/cel/abi_describe.{h,cc}` is the
      shared decode/describe helper (per `cel-cli-design.md` §6: one
      helper, not two implementations), with `abi_describe_test.cc`.
      `run` binds `--var name=value` by recovering the type from the
      program's declared repr, pre-flights the whole declared set, and
      translates a `cel_fn` link failure into the actionable message.
- [x] **Import walk** — **dropped, deliberately.**  The first cut added
      `DecodeImportsFromWasm` to `abi_decode` so `inspect` could report
      required foreign modules and host functions.  It was reverted on
      finding that `m35-component-ergonomics.md` already specifies both
      halves better: §4 consolidates *all* wasm binary-format knowledge
      into `//abi:wasm_binary` (its 2026-07-25 sweep counted five
      existing copies of the framing logic and declares a sixth a review
      finding), and §5.1 puts the required-function table on the wire as
      `cel.abi` field 8, emitted from the post-optimize import surface
      and verified at `Engine::Plan`.  A byte-level import walk would
      have been a second, weaker source of truth for the same facts,
      deleted within weeks.  `inspect` therefore reports what the
      program *declares* today; the `requires:` / `host fns:` lines land
      when field 8 does.
- [x] **e2e (exit codes)** — 18 `expect_exit` cases in
      `cel_smoke_test.sh` pinning each code, plus stdout/stderr routing.
- [x] **e2e (run)** — compile→inspect→run round trip in
      `cel_smoke_test.sh`, plus usage-error cases (undeclared var,
      unbound declared var, bad value, missing file, non-wasm input) and
      a CEL-error-exits-1 case for a precompiled program.
- [x] **Docs** — `tools/cel/README.md` exit-code table;
      `doc/user-guide/index.md` §4.5 (`BindLazy`) and §9 (exit codes,
      message-typed `--var` forms); `testing-checklist.md` rows;
      `eval-public.md`, `current-capabilities.md`, `cleanup-backlog.md`
      #44 reconciled.

### Corrections made along the way

Verifying claims before documenting them turned up three that were
false in the shipped tree:

  - **`mem_size_bytes` does nothing in the default configuration.**
    It reaches the module only via `InstallExprModuleImports`, which
    runs on the `kDynamic` arm alone, and `LoweringOptions::mem_size_bytes`
    is vestigial (`arena_reset` takes no arguments). `compiler.h`,
    `compile.h`, `getting-started.md` and the CLI flag help all told
    users to "raise it for a larger arena"; all four corrected.
  - **The expression-writable window is 256 KiB, not 8 KiB.**
    `doc/design/01-compiler.md` still described `--global-base=8192`;
    the linker uses `--global-base=262144`.
  - **Two `testing-checklist.md` rows were ticked but false** —
    `Find` on unbound never returned `NotFoundError`, and no `BindLazy`
    memoization test existed. Both now describe what the tests assert.

## Future work

- `--activation <file>` bulk binding (`cel-cli-design.md` §3.2) — deferred;
  needs the descriptor-source question (§7 (b)) answered first.
- Rich `CelType` on `VariableEntry` (reserved field 5) would let
  `run --var` accept aggregates without the `name:Type=value` escape
  hatch, and let `inspect` print parameterized types.
- True reference-triggered laziness needs a variable-read host import and
  a codegen change at `kIdent`.
