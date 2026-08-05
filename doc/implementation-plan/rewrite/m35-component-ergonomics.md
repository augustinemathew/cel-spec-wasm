# m35 — Component ergonomics: self-describing plugins, `Use`, `Swap`

Status: plan — drafted 2026-07-22 from an API-design session with the
user; not yet started.  The C ABI contract is drafted at
`bindings/c/cel_component.h`; this doc records the C++ target shape,
the `cel.fns` section format decision, the swap semantics, and the
lifetime/concurrency design.  Depends on m34 (C API) only for the C
face; the C++ work is independent.

> **2026-08-04:** the component/plugin backend this doc describes was
> removed from the tree (see `m39-component-removal.md`); the work is
> preserved on branch `component-functions-archive`.

## 1. Motivation — the declaration triplication

Registering a component function today requires the same declaration
three times: once in the `.celfn` the component was built from, once
as a hand-written C++ `FunctionLibrary::Builder` mirror (~15 lines in
`examples/09`), and once threaded into both `Compiler::Builder::
AddLibrary` and `Engine::AddComponent(bytes, lib)`.  The C++ mirror is
the bug farm: it can drift from the `.celfn` silently, and the
resulting error surfaces at registration or eval, far from the drift.

## 2. Target shape (C++)

One noun, `Component`; the `.wasm` carries its own declarations.

```cpp
auto scorer = Component::Load(bytes).value();   // parses embedded cel.fns

auto compiler = Compiler::NewBuilder()
                    .DeclareVariable("amount", CelType::Int())
                    .Use(scorer)                // decls → type-checker
                    .Build().value();

auto engine = Engine::NewBuilder().Build().value();
CHECK_OK(engine.Use(scorer));                   // sandbox + export check
CHECK_OK(engine.Swap(scorer_v2));               // hot-swap, see §5
```

Registration points are deliberately asymmetric, matching each
phase's semantics: compile-side declarations are compile CONFIG
(builder, like `DeclareVariable`); eval-side components are runtime
BINDINGS (engine, like `BindFunction` — which is what makes `Swap`
expressible).  `FunctionLibrary` stays as the internal representation
and the explicit-decls escape hatch (`Component::LoadWithDecls`);
it leaves the embedder's field of vision.

Naming note (settled with the user 2026-07-22): do NOT split
registration by trust (`AddHostLibrary` / `AddUntrustedLibrary`).
Trust is already encoded in the declaration prefix (`@host.` /
`@component.`) and enforced at registration; a single library
legitimately mixes backends.  The axis worth naming is
declaration-vs-implementation: consider `DeclareFunctions(lib)`
(rhymes with `DeclareVariable`) as the rename of builder
`AddLibrary`, with eval-side `BindFunction` (trusted lambda) /
`Use(component)` (sandboxed bytes) carrying the trust distinction in
their argument types.

## 3. The `cel.fns` custom section

The `cel_wasm_component` build macro already has the `.celfn` text in
hand; it embeds it verbatim (UTF-8, uncompressed) in a wasm custom
section named `cel.fns` — the same pattern `Program` uses for
`cel.abi`.  `Component::Load`:

  - rejects bytes that are not a Component-Model component
    (InvalidArgument),
  - rejects a missing section (InvalidArgument, message points at
    `LoadWithDecls`),
  - parses the section with the existing `ParseCelfnSource`; any
    non-`@component` declaration in the section is InvalidArgument,
  - computes a 32-byte content hash over (bytes ‖ section text),
    exposed as `Component::hash()` — future `Plan`-time
    program↔plugin version verification hangs off this.

`LoadWithDecls(bytes, celfn_text)` covers pre-section artifacts; if
the component ALSO carries a section, the explicit text must match it
byte-for-byte (FailedPrecondition — two drifting sources of truth is
the bug class this milestone exists to end).

## 4. Multi-component reality (already true today)

The engine already supports N registered components — Plan walks
`component_libraries` and instantiates each into the per-Plan store,
each with its own linear memory (`eval/engine.cc:830`).  The flat
namespace is the constraint: overload-id collisions across
components/host fns → AlreadyExists.  Namespacing (register-under-
alias so call sites say `fraud.score()` vs `partner.score()`) is a
compile-side language decision — out of scope here, noted as future
work.

Known perf note: every registered component instantiates into every
Plan whether or not the Program calls it; lazy instantiation is the
obvious follow-up if plugin fleets grow.

## 5. `Swap` contract

`Engine::Swap(replacement)` — hot-swap without restart or recompile:

  - **Matched by declaration set.**  The replacement's parsed decls
    must equal some registered component's decls function-for-
    function (names + signatures).  NotFound if nothing matches;
    FailedPrecondition if a same-named set differs in any signature —
    a signature change invalidates compiled call sites, so it is a
    compile-side event (recompile policies), never a silent runtime
    substitution.
  - **Validated at swap, never at eval** — exports + FuncTypes
    checked before the registry pointer moves; a botched upload
    leaves v1 serving.
  - **Takes effect at the next `Plan`.**  Existing Instances keep
    the old version; rollout is per-worker re-plan; rollback is
    `Swap` with the old bytes.

## 6. Lifetime & concurrency (the shared_ptr question)

Question raised by the user: "do we use shared_ptr — what if
something is using it?"  Answer: extend the pattern the codebase
already uses (Instance pins per-Plan component state via
`std::shared_ptr<void> InstanceImpl::component_fn_envs`; Instance
outlives its Engine handle the same way):

  - Engine registry: `std::shared_ptr<const RegisteredComponent>`
    per component — an immutable snapshot (bytes, decls, hash).
  - `Plan` copies the shared_ptr into the InstanceImpl.  The
    Instance owns its version outright from then on.
  - `Swap` builds a new snapshot and replaces the registry pointer.
    In-flight Evals and existing Instances are untouched; the old
    snapshot frees when the last pinning Instance re-plans or dies.
    Worst-case memory: versions × still-pinning instances, shrinking
    as workers re-plan.
  - NEW concurrency requirement vs today's registration family:
    `Swap` runs concurrent with `Plan` (unlike startup-only
    `AddFunction`), so the registry read in Plan takes a mutex or
    `std::atomic<std::shared_ptr>` — one pointer read per Plan.
    `Swap` remains NOT thread-safe vs other registration calls.

## 7. C ABI

Drafted at `bindings/c/cel_component.h` (2026-07-22): opaque
`cel_component`, `cel_component_load` / `_load_with_decls` /
introspection (`fn_count`/`fn_decl`/`fn_doc`/`hash`) /
`cel_compiler_builder_use_component` / `cel_engine_use_component` /
`cel_engine_swap_component` / setter-style limits
(`cel_engine_component_max_memory`).  Key property, and the reason
this surface matters for wasm-hosted embedders: the component path is
PURE DATA — no C function pointers — so it is the one custom-function
mechanism that crosses every FFI and a future wasm-compiled embedder,
where `cel_host_fn` callbacks cannot.

## 8. Slices

  A. `cel.fns` emission in `cel_wasm_component` + section reader +
     `Component` noun (`Load`/`LoadWithDecls`/introspection/hash) +
     unit tests.
  B. `Compiler::Builder::Use` + `Engine::Use` (delegating to the
     existing `AddComponent` internals) + examples/09 and
     writing-component-functions.md rewritten onto the new surface;
     old two-arg `AddComponent` stays as the internal/escape form.
  C. `Engine::Swap` + registry shared_ptr snapshotting + the
     Plan-concurrency guard + swap e2e test (v1 serves → swap →
     re-plan adopts v2; signature-change rejection pinned).
  D. C ABI impl over A–C, per the frozen header.

Per-slice test matrix follows `feature-pipeline-checklist.md` (new
ABI-adjacent surface: section format gets a boundary test — empty
section, non-UTF8, oversized; swap gets the three-outcome matrix).

## 9. Out of scope / future work

  - Component namespacing / aliasing (call-site `fraud.score()`).
  - Lazy per-Program component instantiation at Plan.
  - CPU-time limits for component calls (tracked in README
    Limitations; the `cel_engine_component_max_memory` setter is the
    ABI slot such knobs land in).
  - `Plan`-time program↔plugin hash verification.
