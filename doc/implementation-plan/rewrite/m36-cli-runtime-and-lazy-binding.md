# m36 — CLI run-time half (`run`, `inspect`), exit-code contract, and lazy binding

Status: in progress — drafted 2026-07-25.

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

Answers "what does this artifact need in order to run?" — the operator-facing
half. Decodes the `cel.abi` custom section via
`DecodeCelAbiFromWasm` (no wasmtime, no Engine) and walks the wasm **import
section** to classify what the module demands of its host.

Output — fixed labels, `none` as the empty sentinel:

```
vars:     a:int, b:int
requires: none
host fns: none
link:     static (cel.abi v1, runtime abi v4)
```

`--format json` emits the same facts as a machine-readable object.

**Import classification.** Import module names partition into fixed
namespaces (`compiler/codegen/overload_table.h`, `ImportModuleSource`) and
everything else:

| Module | Meaning for `inspect` |
|---|---|
| `cel` | `cel_runtime.wasm` exports — supplied by the engine (dynamic link mode). Not reported. |
| `cel_host` | Host trampolines — always supplied by the engine. Not reported. |
| `cel_env` | Host environment helpers (`cel_log`). Not reported. |
| `cel_fn` | `@host` / `@component` custom functions. Reported under `host fns:`. **The CLI cannot supply these.** |
| anything else | A foreign-module alias. Reported under `requires:` with the `--module` remediation hint. |

This is why `inspect` walks imports rather than reading `cel.abi`: the ABI
section carries neither a host-import list nor a foreign-alias list
(`cel-cli-design.md` §7 open question (a)). Walking the import section
answers it with no wire-format change, so **no `cel.abi` extension is in
scope for m36**.

**Variable types.** `cel.abi`'s `VariableEntry` carries only `repr` (a
numeric mirror of `ir::Repr`), not a full `CelType` — field 5 is reserved
for that. So `inspect` prints the repr name (`int`, `string`, `map`,
`message`), not a parameterized type: an aggregate shows as `m:map`, never
`m:map<string,int>`. Stated in the output legend rather than faked.

## 3. `cel run <prog.wasm>`

Evaluates a precompiled program with **no recompile**: read file →
`Program(std::move(bytes))` → `Engine::Plan` → `Instance::Eval`. `Program`
needs nothing but the bytes; `Engine::Plan` decodes `cel.abi` itself.

```
cel compile "a * b" --var a:int --var b:int -o p.wasm
cel run p.wasm --var a=6 --var b=7
```

**`--var` on `run` takes values, not declarations.** The type comes from
the program's `cel.abi`. Because only `repr` is on the wire, this resolves
cleanly for scalar reprs (bool/int/uint/double/string/bytes/duration/
timestamp) and cannot for aggregates. So:

  - `--var name=value` — the normal form. The repr from `cel.abi` selects
    the parse. A name not declared by the program is a **usage error**
    (exit 2) naming the declared set.
  - `--var name:Type=value` — the escape hatch for aggregate-repr
    variables, where the caller supplies the full type because the wire
    cannot. If the given type's repr disagrees with the declared repr,
    that is a usage error.

Unsupplied host functions get the explicit error the design doc mandates
verbatim, rather than a wasmtime link failure:

> program imports host fn 'rate_string'; the CLI cannot supply host impls
> — use the C++ API, or redefine it as @native / foreign.

`--module <alias>=<path.wasm>` (repeatable) wires `Engine::AddModule`.
Required aliases are validated against the import section **before**
evaluation, so a missing alias is a usage error naming the alias, not a
link trap.

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
- [ ] **CLI `run` + `inspect`** — with `tools/cel/abi_describe.{h,cc}` as
      the shared decode/describe helper (per `cel-cli-design.md` §6: one
      helper, not two implementations), plus `abi_describe_test.cc`.
- [ ] **Import walk** — extend the `abi_decode` byte walker to parse
      section id 2, exposed for `inspect`; its own test.
- [x] **e2e (exit codes)** — 18 `expect_exit` cases in
      `cel_smoke_test.sh` pinning each code, plus stdout/stderr routing.
- [ ] **e2e (run)** — compile→run round trip; pending `cel run`.
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
