# PROPOSALS — feature work needing an owner decision

Status: live — created 2026-06-09; reconciled against HEAD 2026-07-25.

This is the running list of **changes that need a decision before they
can be made**: anything that alters a public signature, observable
behaviour, the ABI wire format, or repo infrastructure. An entry is
logged here instead of being made, so that finding "this API is wrong"
during unrelated work neither forces an unscoped detour nor gets
silently dropped.

Maintenance rules (also in `CLAUDE.md`, "Feature work is tracked in
`PROPOSALS.md`"): add an entry when you hit something needing an owner
call; **delete the entry in the commit that ships it**, citing this
file in the commit message; and re-check entries against HEAD before
citing them — items 6 and 7 below sat here claiming the repo had no CI
and no module version for weeks after both landed.

## API changes

1. **`Engine::AddPlugin(bytes, FunctionLibrary)` takes an
   internal-visibility type** (cleanup-backlog #32). A public method
   whose parameter type (`//compiler/celfn:function_library`,
   `//:internal`) external code cannot legally depend on. Options:
   promote `function_library` to public (smallest), or add a public
   declaration-string overload mirroring `BindFunction`. Affected:
   `eval/engine.h`, root `BUILD.bazel` examples carve-out.
2. **Host-fn `ErrorPayload.message` does not cross the wasm boundary**
   (cleanup-backlog #31). `Value::Error({code, "msg"})` from a callback
   decodes as `"runtime error code N"`. Behavior change: wire the
   message through the error slot (ABI addition) or document the
   code-only contract permanently. Affected: `cel_host_error.*`,
   runtime error encoding, `examples/08` (currently asserts the honest
   broken behavior).
3. **`HostMapView` has no key-enumeration API** (Size/Get/ContainsKey
   only). Add `Keys()` or an iterator; docs currently state the
   limitation honestly. Affected: `eval/host_call_context.h`.
4. **No `Value::AsProto<T>()`** — typed proto extraction requires
   `MessageBacking()` + an internal header. Add a public typed
   accessor. Affected: `eval/value.h`.
5. **Execution cost limit.** `Engine` configures wasmtime with no
   fuel, no epoch interruption, and no `max_wasm_stack`, and
   `Instance::Eval` takes no deadline or budget. CEL totality bounds
   non-termination but not *cost*: nested comprehensions over
   host-backed collections are polynomial with no ceiling and no way
   for the host to interrupt. Options: epoch interruption plus an
   `Eval(deadline)` overload (preferred — cheap, cancellable), or
   fuel metering (deterministic, higher overhead). Affected:
   `eval/engine.cc`, `eval/instance.{h,cc}`,
   `doc/user-guide/security-model.md`. **P0 for accepting
   semi-trusted expressions.**

## Infrastructure

6. **Root governance files**: no `CONTRIBUTING.md` (re-home or
   stub-link `doc/contributing.md`), no `SECURITY.md` (link
   `doc/user-guide/security-model.md` + a disclosure contact).
   GitHub surfaces neither today.
7. **Release contract**: `MODULE.bazel` carries `version = "0.1.0"`
   but there are no git tags, no `CHANGELOG.md`, and no
   `cel --version`. Adopters cannot pin a version, see what changed,
   or tell which compiler produced a given `.wasm` — and AOT
   artifacts are compiler-version sensitive. Propose `0.x` semver
   tags + a CHANGELOG seeded from the milestone history + a version
   stamp readable from both the CLI and the `cel.abi` section.
8. **Generated API reference**: no Doxygen config, and the headers
   (the best API prose in the repo) sit outside `docs_dir`, so a site
   reader never sees them. Wire Doxygen (or standardese-lite) into
   the pages workflow.
9. **CEL language-support matrix**: nothing enumerates supported vs
   rejected operators, macros, stdlib overloads, and extensions.
   `README.md` §Limitations is prose and `conformance/README.md` is
   per-fixture pass/skip; neither answers "does `getFullYear` work?".
   This is the most common adopter question.
10. **Documented capacity envelope**: limit values live only in
    `e2e/limits_test.cc` and `runtime/cel_layout.h`, and the policy
    lives in `CLAUDE.md`, which is not published. No user-facing page
    states a single numeric limit, so an embedder accepting untrusted
    expressions cannot size their risk.

## Shipped (kept briefly for traceability, then deleted)

- ~~**CI**~~ — landed: `.github/workflows/ci.yml` runs build +
  `bazel test //...` + `run_full_suite.sh` (manual targets +
  dual-mode conformance gate) on Ubuntu, macOS on demand.
- ~~**Graceful arena OOM**~~ (was #5) — the crash class closed with
  cleanup-backlog #16/#47; the expression static region is bounded
  and parser-depth overflow is a catchable trap, not a host SIGSEGV.
- ~~**`Activation::BindLazy` / `OverrideFunction`**~~ — resolved in
  m36: `BindLazy` implemented, `OverrideFunction` removed.

## Style-guide conflicts requiring signature changes

(None found yet — naming, explicit ctors, and pointer-vs-reference
conventions already conform. Entries will be added as the P2/P3 header
passes uncover them.)
