# M24 — Foreign custom-fn backend via the Component Model

Status: research / design — drafted 2026-06-03, not yet started. No
`compiler/` / `eval/` / `runtime/` code changes here. Grounded in the
**shipped** headers (`compiler/codegen/overload_table.h`,
`eval/engine.h`, `compiler/celfn/function_library.h`), not the m13 doc
prose — see the staleness note in §1.

This doc provides the **foreign-module backend that `m13-custom-fns.md`
scoped and then deferred**, now designed against the Component Model ABI
investigated in [`m23-foreign-fn-component-abi.md`](m23-foreign-fn-component-abi.md)
(the complete CEL-value WIT model + the measured boundary cost). It does
**not** redesign m13's host or CEL-defined backends, its checker hookup,
or its codegen — those ship and are reused verbatim.

## 0. TL;DR

A foreign custom function (implemented in Go/Rust/separately-compiled
C++, isolated in its own memory) is integrated **not as a new codegen
path, but as a host-callback** — i.e. it reuses the *exact* `kCelFn` /
`Engine::AddFunction` dispatch that host-backed customs already use. The
only new machinery is on the eval side:

  1. `Engine::AddComponent(bytes, library)` — instantiate the foreign
     **component** (wasmtime component API), host-implement the `value`
     resource over `CelValue`, and register each declared fn as a host
     callback whose body forwards to the component's `invoke`.
  2. A **marshaling adapter** (`CelValue` <-> the WIT `value`, per m23):
     scalars by value, nested aggregates via host-owned `value` handles,
     proto messages as serialized `bytes`.

Codegen, the checker hookup, the overload table, and 3VL/error
absorption are **unchanged** — a component-backed fn looks to the
compiler exactly like a `kCelFn` host custom.

## 1. Two foreign regimes — reconciled with what shipped

> **m13 staleness (read the code).** `m13 §5.3` recommends refactoring
> `ImportModule` into a `struct {Kind; module_name}`. The shipped
> `overload_table.h` instead keeps `enum class ImportModule {kCelRuntime,
> kCelHost, kCelFn, kUserModule}` and hangs `module_name` off
> `OverloadImpl` (populated only for `kUserModule`). This doc builds on
> the shipped enum, and on the real `RegisterCustom(overload_id, module,
> module_name, helper_name, num_args)` signature.

m13's foreign backend (`kUserModule`, `Engine::AddModule`) is the
**shared-memory** model: the user module imports `cel.memory` +
`cel.arena_alloc`, and codegen emits a direct
`(call $<alias>.<helper> out_slot arg_slots…)` — the slot-out
convention, all `i32` offsets into the one shared linear memory. Call
cost is single-digit ns (a plain cross-module wasm call).

m23 surfaced the catch m13's "any wasm toolchain works" framing glosses:
**two independent wasi runtimes can't share one memory** (both place
`__stack_pointer` at `66560`; empirically confirmed). So `kUserModule`
requires the foreign module to be compiled as a **thin guest** —
`--import-memory`, no libc heap, allocate via `cel.arena_alloc` — and
m13 §4.5.1 already restricts it to scalars + flat/nested aggregates of
scalars, **no protos** (the externref `msg_slot` is meaningless across
the boundary). That is **Regime A**: fast, trusted, special-compiled.

The Component Model is **Regime B**: the foreign module is a normal
**isolated component** (own memory, any toolchain, no thin-guest mode),
protos cross as bytes, and the host brokers every value — at the m23
cost (~410 ns/call fixed; ~500 ns per `value`-handle op; scalars free).

| | Regime A — `kUserModule` (m13) | Regime B — Component (this doc) |
| --- | --- | --- |
| memory | shared (`cel.memory`) | isolated (own) |
| call cost | single-digit ns (slot-out) | ~410 ns + per-arg marshal |
| foreign toolchain | thin guest (import memory, no libc heap) | any (normal component) |
| protos | ❌ (m13 §4.5.1) | ✅ as serialized bytes |
| trust | trusted / co-compiled | untrusted / polyglot OK |
| dispatch | direct module import | host-callback (`cel_fn`) |

They **coexist**: the embedder picks per library/function. This doc adds
B without touching A.

## 2. The integration insight: a component fn is a host fn

The shipped dispatch already has two custom flavors that both route
through the overload table:

  - **`kUserModule`** — codegen emits `(call $<alias>.<helper> …)`; the
    embedder wires the alias's module exports via `Engine::AddModule`.
  - **`kCelFn`** — codegen emits `(call $cel_fn.<helper> …)`; the
    embedder binds a **native host callback** via
    `Engine::AddFunction(overload_id, num_args, HostCallback)`.

A Component-backed fn is **the second one** with a different callback
body: instead of running native C++, the callback **invokes the foreign
component**. So:

  - **Overload table:** `RegisterCustom(overload_id, ImportModule::kCelFn,
    /*module_name=*/"", helper_name, num_args)` — identical to a host
    custom. (No new `ImportModule` variant; component-ness is invisible
    to the compiler.)
  - **Codegen:** unchanged — `(call $cel_fn.<helper> out_slot
    arg_slots…)`. Per m13 §7.2 the call site is backend-agnostic; this
    is why.
  - **3VL / error absorption, arity, the `cel.abi` `CustomFunctionEntry`
    (Backend = FOREIGN, dispatched through `cel_fn`):** unchanged.

The entire new surface is on the eval side (§3.5). This is the cheapest
correct integration and keeps the "one dispatch path, no silent
absorption fork" property m13 §5.3 paid for.

## 3. Pipeline mapping (file by file)

### 3.1 Frontend — `compiler/celfn/function_library.h`
Add a `Backend::kForeignComponent` (the shipped enum already has
`kHost` and, per m13, foreign/cel-defined) and a builder entry
`AddComponentFn(fn_name, return_type, params)` — or reuse `AddForeign`
with a component flag. A component fn declares the same `(name, arg
types, return type)` contract a host fn does; the checker sees a normal
declared overload.

### 3.2 Checker — `compiler/frontend/` (cel-cpp hookup)
**Unchanged.** The fn is declared to the cel-cpp `TypeCheckerBuilder`
via `MakeOverloadDecl(overload_id, …)` exactly like any custom; cel-cpp
stamps `overload_id` on the resolved call node. Component-ness is not a
checker concept.

### 3.3 IR / overload table — `compiler/codegen/overload_table.{h,cc}`
`RegisterCustom(overload_id, kCelFn, "", helper_name, num_args)`. No new
variant, no new field. `NodeAnnotation.overload_id` carries the interned
id, as for every other call (m13: "no custom-specific field").

### 3.4 Codegen — `compiler/codegen/expr_lower.cc`
**Unchanged.** Emits `(call $cel_fn.<helper> out_slot a0 a1 …)`. The
`cel_fn` import is host-provided; whether its body is native or a
component bridge is a link-time / eval-time concern.

### 3.5 Eval — the only new code (`eval/engine.h`, `eval/internal/`)
Two additions:

  - **`Engine::AddComponent(absl::string_view component_bytes, const
    FunctionLibrary& lib)`** (sibling of `AddModule` / `AddFunction`):
      1. Instantiate the component with the wasmtime **component API**
         (new dependency — `eval/` has no component usage today).
      2. Host-implement the `value` resource (`m23 wit/cel.wit`) with
         **rep = a reference to a `CelValue`** owned by the current
         eval (arena slot or an externref into the host table). This is
         the realistic host-owned-`types` deployment m23 §8 flagged.
      3. For each decl in `lib`, register a host callback via the
         existing `AddFunction` path whose body is "marshal args ->
         `invoke(name, args)` -> marshal result."
  - **The marshaling adapter** (`eval/internal/cel_component.{h,cc}`,
    new): `CelValue` <-> WIT `value`, §4.

`HostCallback` / `HostCallContext` (`eval/host_callback.h`,
`eval/host_call_context.h`) already give the callback the arg
`CelValue`s and a place to write the result — the component bridge is a
callback like any other.

## 4. The marshaling adapter (`CelValue` <-> WIT `value`)

Driven by the m23 cost rule — choose representation per value:

  - **Scalars** (null/bool/int/uint/double/string/bytes/duration/
    timestamp) -> the WIT `primitive` variant, **by value**. ~free per
    arg over the ~410 ns call. The 90% path; prefer an
    `invoke-prim(list<primitive>)` overload for all-scalar calls (one
    crossing, m23: ~525 ns vs ~2500 ns for the handle path).
  - **Nested aggregates** (list/map of values) -> a host-owned `value`
    **handle**; the component pulls via `list-get` / `map-get` /
    `as-*`. ~500 ns **per node the component touches** — fine for
    sparse reads, a pull-storm for full traversals.
  - **Proto messages** -> serialized `bytes` (`record message
    {type-name, wire}`). This is exactly how m24 lifts m13 §4.5.1's
    "no protos" restriction: a proto that can't cross as an externref
    *can* cross as bytes, deserialized by the foreign side. One copy,
    no handle walk.
  - **Big aggregates read in full** -> same as proto: serialize to
    bytes, decode locally, rather than a ~500 ns x N handle walk.

3VL/error: a `CEL_ERROR` / `CEL_UNKNOWN` arg short-circuits per CEL
semantics **before** any marshaling (absorption stays in the shared
`cel_fn` path, not duplicated). The component returns `result<value,
eval-error>`; an `eval-error` maps back to a `CEL_ERROR` CelValue; a
wasmtime trap (component OOM / panic) maps to a host `absl::Status`,
distinct from a CEL error.

## 5. When to use which backend (the embedder's choice)

  - **Hot custom fn, trusted, you control the toolchain** -> Regime A
    (`kUserModule`, shared memory). Single-digit-ns; the Component
    Model's ~410 ns/call would dominate a comprehension loop.
  - **Untrusted, polyglot, or you can't thin-guest-compile it** ->
    Regime B (this doc). Pay ~410 ns + marshal for real isolation +
    any-language + protos.
  - **Needs protos and you won't build the copy-to-arena / externref
    bridge m13 §4.5.1 deferred** -> Regime B (protos as bytes is the
    cheapest unlock).

## 6. Feature-pipeline checklist (files + tests this ripples through)

Per `feature-pipeline-checklist.md` — a new host-dispatched backend:

  - `compiler/celfn/function_library.{h,cc}` (+`_test`) — the
    `kForeignComponent` backend + builder entry.
  - `compiler/codegen/overload_table.cc` — none (reuses `kCelFn`); add a
    test asserting a component decl registers as `kCelFn`.
  - `abi/cel_abi.proto` + `abi_decode` — `CustomFunctionEntry.Backend`
    already has `FOREIGN`; confirm emission/round-trip test.
  - `eval/engine.{h,cc}` (+`_test`) — `AddComponent`; binding-validation
    errors (missing export, arity mismatch, unbound at `Plan`).
  - `eval/internal/cel_component.{h,cc}` (+`_test`) — the marshaling
    adapter; **exhaustive per-CEL-type matrix** (every kind, by-value
    vs handle vs bytes path) + 3VL/error/unknown propagation + the
    `result<_, eval-error>` mapping.
  - `e2e/` — a Go-or-Rust component end-to-end (the m13 §8.3 forcing
    function, now as a component), one fn per representation path
    (scalar / nested / proto-as-bytes).
  - `bench/` — `AddComponent` invoke cost vs `kUserModule` slot-out vs
    native `AddFunction`, to keep the §5 guidance numeric.
  - `testing-checklist.md` — tick the foreign-component rows.

## 7. Open questions / future work

  - **Component API in `eval/`**: wasmtime's component host API (resource
    host-impl, `Linker`) is a new eval dependency; verify the vendored
    wasmtime build exposes it (the C API's component surface is thinner
    than Rust's — may force a Rust shim or a wider C-API binding).
  - **`invoke-prim` fast path**: worth a second WIT entry so all-scalar
    calls skip the handle machinery (m23 measured ~5x).
  - **Lifetime of host-owned `value` handles**: scoped to one
    `invoke` (drop on return). A component that stashes a handle across
    calls needs generational handles or an explicit deny — pick the
    deny for v1.
  - **Regime A's thin-guest compile story**: orthogonal, still m13's;
    this doc only adds Regime B.
  - **`-c opt`-grade re-measure** of the ~410 ns baseline before it's
    treated as fixed (m23 §8).
