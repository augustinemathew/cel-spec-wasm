# M24 — Foreign custom functions via the Component Model

Status: shipped 2026-06-04 (v1, native dispatch path).

> **2026-08-04:** the component/plugin backend this doc describes was
> removed from the tree (see `m39-component-removal.md`); the work is
> preserved on branch `component-functions-archive`.

What landed: the load-bearing slices A.1–A.5, B.1–B.7+B.9, C.1–C.4 plus
an e2e dispatch proof. A `kForeignComponent` decl now routes through
the shipped `kCelFn` import path (no new `ImportModule` variant),
`Engine::AddComponent(component_bytes, lib)` parses+instantiates a
component via the wasmtime C API, validates each declared export's
type, and binds it as a `HostCallback` whose body lifts CEL args into
component-model `wasmtime_component_val_t`, calls the export, and
lowers the result back to a `Value`. The marshaling bridge
(`eval/internal/cel_component.{h,cc}`) covers every CEL type the v1
scope admits — `bool` / `int` / `uint` / `double` / `null` / `string`
/ `bytes` / `duration` / `timestamp` / `list<T>` / `map<K,V>` /
`proto(fqn)` — with the §10 boundary matrix in
`cel_component_test.cc` (79 cases). End-to-end dispatch is pinned by
`e2e/foreign_component_dispatch_test.cc` (full pipeline:
`AddForeignComponent` → `Compile` → `AddComponent` → `Plan` → `Eval`,
incl. `INT64_MIN`).

Plan-vs-execution deltas — see §14 for the full list. Headline items:

  - **`optional<T>` and `type` permanently rejected as
    foreign-component declarable shapes.** §6 originally listed both
    as supported (`option<wit T>` and `string` carriers respectively);
    user direction (2026-06-04) closes both as foreign-component decl
    surfaces, with `Builder::Build()` rejecting either at decl time
    naming the offending decl. The marshaling layer keeps its kType
    Lift/Lower arm because other kCelFn / kHost paths can still use
    it (only the foreign-component decl surface is closed); the
    kOptional arm refuses outright. CEL `null` (kNull) is a distinct
    kind and stays supported — the wire-level `option<unit>`
    encoding for null is a hidden canonical-ABI detail with no
    user-facing optional<T> surface. See §14 for the permanent-scope
    block.
  - **`AddForeignComponent` IDL gates lifted to `Builder::Build()`.**
    The doc didn't pin where illegal-shape rejection runs; execution
    chose `Build()` (mirroring `MentionsProto`) so illegal types fail
    once, declaratively, before any compile attempt. The same `Build()`
    gate also catches illegal map-key kinds (langdef rule: keys are
    `bool|int|uint|string`), applied to **every** backend, not just
    `kForeignComponent`.
  - **Component-Model export-name kebab translation.** §3.5 was silent
    on snake-vs-kebab; execution surfaced that the Component Model
    spec restricts exports to kebab-case (`add_int_int` is rejected
    at parse with "not a valid extern name"). `Engine::AddComponent`
    now converts the CEL overload-id `foo_bar` → component export
    `foo-bar` at the lookup site, so the CEL author stays in snake
    and the WIT author stays in kebab.
  - **`wasmtime_wat2wasm` accepts component-model WAT.** Settled by
    `eval/probes/m24/component_wat_probe.cc` (now deleted at closeout).
    The e2e dispatch test assembles a tiny component inline, removing
    the wit-bindgen / wasi-sdk / wasm-tools tool-chain pre-req that
    §11 assumed for fixtures. That toolchain is still the right
    author-facing surface — it's only the *test fixture* that no
    longer needs it.
  - **`cel.abi` `CustomFunctionEntry.Backend = FOREIGN` round-trip
    not wired.** §3 listed this; the data path stays
    backend-agnostic at the wire level today (component-ness is
    invisible to the compiler — there is no per-decl backend bit on
    the wire), so the entry stayed unchanged. Recorded as future
    work (§14).
  - **TinyGo forcing fixture deferred.** §11 named a TinyGo-via-WIT
    component as the language-agnostic contract proof. v1 ships the
    contract-pinning C++ stub_demo (17 author-level cases) + the e2e
    dispatch test (every load-bearing CEL type round-trips through
    the wasmtime component API); the TinyGo variant is a future-work
    item (§14) and does not gate v1 sign-off because the contract is
    already pinned through wasmtime's component runtime, which is
    language-agnostic by construction.
  - **Large-payload boundary block added.** §10 listed empty + single
    + ragged-nested for aggregates, but the original test pass
    capped at KB scale.  v1 ships a separate large-payload block
    (F.1, F.2) covering 256 KiB strings/bytes, 10⁵ list elements,
    10⁴ map entries, 10⁴ proto-repeated entries, MiB-scale proto
    string/bytes fields, and an e2e dispatch test that crosses a
    256 KiB string + a 10⁵-element list through the wasmtime
    canonical-ABI boundary.  This catches the failure modes the KB
    cases miss: signed-int length counters, quadratic per-element
    walks, allocator pathologies under genuine memory pressure, and
    canonical-ABI realloc semantics with non-trivial payloads.

Absorbs the former separate "m25 DX" doc (backend + developer experience
are one design). Validated end-to-end (`e2e/foreign_component_fixtures/stub_demo/`, 17/17 e2e
assertions on wasmtime 45). Builds on
[`m23-foreign-fn-component-abi.md`](m23-foreign-fn-component-abi.md) (the
ABI investigation, the complete CEL-value WIT model, the measured
boundary cost). Grounded in the **shipped** headers
(`compiler/codegen/overload_table.h`, `eval/engine.h`,
`compiler/celfn/function_library.h`), not the m13 doc prose — see §1.

This provides the **foreign-module backend that `m13-custom-fns.md`
scoped and then deferred**. It does not redesign m13's host or
CEL-defined backends, its checker hookup, or its codegen — those ship
and are reused.

## 0. TL;DR

A foreign custom function (Go/Rust/separately-compiled C++, isolated in
its own memory) integrates with **two key moves**:

  1. **Dispatch as a host callback.** It reuses the *exact* `kCelFn` /
     `Engine::AddFunction` path host-backed customs already use — the
     compiler, checker, overload table, codegen, and 3VL absorption are
     **unchanged**. New eval-side code: `Engine::AddComponent`.
  2. **Marshal via per-function typed WIT + a generated codec.** A custom
     fn has a concrete declared signature (`list<map<string,int>> ->
     int`); a concrete type is finite, so WIT expresses it directly
     (`list<tuple<string,s64>> -> s64`) and `wit-bindgen` generates the
     lift/lower. The author writes **only native C++**
     (`int64_t`/`std::string`/`std::vector`/`std::map`/nested) against
     generated stubs — never WIT, offsets, handles, or the canonical
     ABI.

The m23 `value`-resource (handles, ~500 ns/node) is reserved for the
rare **dynamic/variadic** custom fn (§4). The author surface is one file
of native-typed functions (`e2e/foreign_component_fixtures/stub_demo/user_fns.cc`).

## 1. Two foreign regimes — reconciled with what shipped

> **m13 staleness (read the code).** `m13 §5.3` recommends refactoring
> `ImportModule` into a `struct {Kind; module_name}`. The shipped
> `overload_table.h` keeps `enum class ImportModule {kCelRuntime,
> kCelHost, kCelFn, kUserModule}` and hangs `module_name` off
> `OverloadImpl`. This doc builds on the shipped enum and the real
> `RegisterCustom(overload_id, module, module_name, helper_name,
> num_args)`.

m13's foreign backend (`kUserModule`, `Engine::AddModule`) is the
**shared-memory** model: the user module imports `cel.memory` +
`cel.arena_alloc`, codegen emits a direct `(call $<alias>.<helper> …)`,
call cost is single-digit ns. m23 surfaced the catch: two independent
wasi runtimes can't share one memory (both put `__stack_pointer` at
`66560`), so `kUserModule` needs a **thin-guest** compile and m13 §4.5.1
already bars protos there. That is **Regime A**: fast, trusted,
special-compiled.

The Component Model is **Regime B**: a normal **isolated** component
(own memory, any toolchain, no thin-guest mode), protos cross as bytes,
at the m23 cost (~410 ns/call fixed; aggregates by typed copy, §4).

| | Regime A — `kUserModule` (m13) | Regime B — Component (this doc) |
| --- | --- | --- |
| memory | shared (`cel.memory`) | isolated (own) |
| call cost | single-digit ns (slot-out) | ~410 ns + typed-arg copy |
| foreign toolchain | thin guest | any (normal component) |
| protos | ❌ (m13 §4.5.1) | ✅ as serialized bytes |
| trust | trusted / co-compiled | untrusted / polyglot OK |
| dispatch | direct module import | host-callback (`cel_fn`) |

They coexist; the embedder picks per library/function. This doc adds B.

## 2. Dispatch: a component fn is a host fn

The shipped dispatch already has `kCelFn` — codegen emits `(call
$cel_fn.<helper> …)` and the embedder binds a **native host callback**
via `Engine::AddFunction(overload_id, num_args, HostCallback)`. A
Component-backed fn is **that**, with a callback body that invokes the
foreign component instead of running native code. Therefore:

  - **Overload table:** `RegisterCustom(overload_id, kCelFn, "",
    helper_name, num_args)` — identical to a host custom. No new
    `ImportModule` variant; component-ness is invisible to the compiler.
  - **Codegen / checker / 3VL absorption / arity / `cel.abi`
    `CustomFunctionEntry` (Backend=FOREIGN, dispatched via `cel_fn`):**
    unchanged. Per m13 §7.2 the call site is backend-agnostic.

The entire new surface is eval-side (§3.5). This keeps the "one dispatch
path, no silent absorption fork" property m13 §5.3 paid for.

## 3. Pipeline mapping (file by file)

  - **Frontend** `compiler/celfn/function_library.h` — add
    `Backend::kForeignComponent` + a builder entry `AddForeignComponent(
    fn_name, return_type, params)`. Same `(name, arg types, return
    type)` contract as a host fn.
  - **Checker** `compiler/frontend/` — **unchanged.** Declared to the
    cel-cpp `TypeCheckerBuilder` via `MakeOverloadDecl(overload_id, …)`;
    cel-cpp stamps `overload_id`. Component-ness is not a checker concept.
  - **Overload table** `compiler/codegen/overload_table.cc` —
    `RegisterCustom(overload_id, kCelFn, "", helper_name, num_args)`. No
    new variant.
  - **Codegen** `compiler/codegen/expr_lower.cc` — **unchanged.** Emits
    `(call $cel_fn.<helper> out_slot a0 a1 …)`.
  - **Eval** (the only new code): `Engine::AddComponent(component_bytes,
    const FunctionLibrary& lib)` — sibling of `AddModule`/`AddFunction`:
      1. Instantiate the component with the wasmtime **component API**
         (new eval dependency — none today; see §13).
      2. For each decl in `lib`, register a host callback (via the
         existing `AddFunction` path) whose body marshals args ->
         the component's typed export -> marshals the result.
      3. Validate each declared fn is exported with the matching
         `FuncType`; unbound-at-`Plan` / arity-mismatch are `Plan`-time
         errors (mirror `AddModule`).

`HostCallback`/`HostCallContext` already hand the callback the arg
`CelValue`s + a result slot — the component bridge is a callback like
any other.

## 4. Marshaling: typed WIT per fn + codec (default); resource for dynamic

> **This supersedes the value-resource-as-default sketch from earlier
> drafts.** For a **concretely-typed** custom fn — the overwhelming
> common case — the `value` resource is the wrong default: it costs
> ~500 ns *per node visited* (m23 §5). Typed-per-fn WIT crosses the
> whole aggregate **once** (canonical-ABI copy of a flat `list<...>`,
> per-byte cheap per m23's arg-cost sweep) and the codec lifts it
> **locally** — one crossing + O(n) memory, **not N crossings**.

Rule, by argument shape:

  - **Concrete-typed custom fn (common):** typed WIT per fn + codec
    (§5–§7). One crossing + local deserialize.
  - **Dynamic / variadic / `any`-typed custom fn (rare):** the m23
    `value` resource (handles), accepting ~500 ns/node. Scalars still go
    by value; nested via handles; proto as bytes — choose per value.

## 5. The author experience — three layers (validated)

```
 ┌─ author writes (user_fns.cc) ─ native C++ only ─────────────┐
 │ int64_t SumByKey(const std::map<std::string,                │
 │                  std::vector<int64_t>>& m, std::string_view) │
 └─────────────────────────────────────────────────────────────┘
 ┌─ generated by celfnc ──────────────────────────────────────┐
 │ codec.h          {ptr,len}/{f0,f1} structs  <->  std:: types │
 │ generated_stub.cc  export -> codec-in -> user fn -> codec-out│
 │ fns.wit          one typed WIT func per custom fn            │
 └─────────────────────────────────────────────────────────────┘
 ┌─ off-the-shelf ─ wit-bindgen / wasm-tools / wac ───────────┘
```

The author edits **only the top box.** `e2e/foreign_component_fixtures/stub_demo/` is all three,
running: `wasmtime run app.wasm` -> `17 passed, 0 failed` (incl.
boundary cases: INT64_MIN, empty list/map/string, embedded NUL, ragged
nesting, missing key).

## 6. CEL type <-> WIT type <-> C++ type — every type, in and out

| CEL type | WIT type | C++ (author sees) | Notes |
| --- | --- | --- | --- |
| `bool` | `bool` | `bool` | |
| `int` | `s64` | `int64_t` | |
| `uint` | `u64` | `uint64_t` | |
| `double` | `f64` | `double` | |
| `string` | `string` | `std::string_view` in / `std::string` out | UTF-8 |
| `bytes` | `list<u8>` | `std::vector<uint8_t>` (or `std::string`) | |
| `null` | `option<unit>` / dynamic | `std::monostate` / via `optional` | rarely a declared param |
| `duration` | `record {seconds:s64,nanos:s32}` | `absl::Duration` | |
| `timestamp` | `record {seconds:s64,nanos:s32}` | `absl::Time` | |
| `type` | `string` | `std::string` (type name) | **PERMANENTLY REJECTED as a foreign-component declarable shape** — `Builder::Build()` refuses; the kType Lift/Lower arm stays for kCelFn / kHost paths. See §14. |
| `optional<T>` | `option<wit T>` | `std::optional<C++ T>` | **PERMANENTLY REJECTED as a foreign-component declarable shape** — `Builder::Build()` refuses with the offending decl named. See §14. |
| `list<T>` | `list<wit T>` | `std::vector<C++ T>` | recurses |
| `map<K,V>` | `list<tuple<wit K, wit V>>` | `std::map<C++ K, C++ V>` | no WIT map; K ∈ {bool,int,uint,string} |
| `proto(fqn)` | `list<u8>` | the author's generated message class | serialized bytes (§8) |

Recursion is by **concrete expansion**, not a recursive type:
`list<map<string,list<int>>>` -> `list<tuple<string,list<s64>>>` nested
structs -> `std::vector<std::map<std::string,std::vector<int64_t>>>`.
The mapping is symmetric — it drives return types the same way.

## 7. The codec (generated, mechanical)

`e2e/foreign_component_fixtures/stub_demo/codec.h` is the pattern `celfnc` emits — a structural
walk of `{ptr,len}` (lists) and `{f0,f1}` (tuples):

```cpp
std::vector<int64_t> lift(const author_list_s64_t& l){ return {l.ptr, l.ptr+l.len}; }
std::map<std::string,int64_t> lift(const author_list_tuple2_string_s64_t& m){
  std::map<std::string,int64_t> r;
  for (size_t i=0;i<m.len;i++) r.emplace(std::string((const char*)m.ptr[i].f0.ptr,m.ptr[i].f0.len), m.ptr[i].f1);
  return r;
}
void lower(author_string_t* ret, std::string_view s){ author_string_dup_n(ret, s.data(), s.size()); }
```

Concrete type -> finite, total, trivially generatable. No reflection.

## 8. Proto handling

`proto(fqn)` maps to `list<u8>` (serialized wire bytes); the codec
deserializes into the author's generated message class (`celfnc` emits
the `Parse`/`SerializeToString`), so the author writes `bool Allow(const
acme::User& u)`. This is how m24 lifts m13 §4.5.1's "no protos across
the foreign boundary": a proto that can't cross as an externref **can**
cross as bytes — one copy, decoded by the author's proto runtime. Cost:
serialize + parse (the proto-crossing bench measured parse dominates) —
right when the fn needs the message, vs. pulling one field.

## 9. API surface (finalized)

  - **Author** — `user_fns.cc` only: native C++ signatures matching the
    declared CEL types via §6. The entire author surface.
  - **Embedder (C++ host):**
    ```cpp
    auto lib = FunctionLibrary::Builder()
        .AddForeignComponent("rules","allow", BoolType,
            {{true,StringType,"u"},{false,ListType(IntType),"rs"}}).Build();
    compilerBuilder.AddLibrary(lib);             // checker sees the overloads
    engine.AddComponent(component_bytes, lib);   // NEW (§3.5)
    ```
  - **Generator `celfnc`** (m13 §8.2): from the CEL decls, emits
    `fns.wit`, the codec, the stub, and a native-signature `user_fns`
    skeleton. The author fills the bodies and builds.

## 10. Boundary-condition matrix (the e2e suite MUST cover)

`*` = covered by `e2e/foreign_component_fixtures/stub_demo/driver_main.cc` today (17 cases).

| Type | Boundary inputs | Output boundaries |
| --- | --- | --- |
| int | `0,-1,INT64_MIN*,INT64_MAX*` | `INT64_MIN/MAX` |
| uint | `0,1,UINT64_MAX` | `UINT64_MAX` |
| double | `0.0,-0.0,NaN,±Inf,DBL_MIN,DBL_MAX` | `NaN`/`Inf` |
| bool | `true,false` | both |
| string | `""*, ascii, embedded NUL*, multi-byte UTF-8, long` | empty*, NUL, UTF-8 |
| bytes | `[]*, [0x00], 0xFF run, long` | empty, NUL |
| list | `[]*, [one], [INT64_MIN]*, large` | empty, large |
| map | `{}*, {one}, all key kinds (bool/int/uint/string)` | empty, large |
| list<list> | `[], [[]]*, ragged*` | nested empty |
| map<.,list> | `{}, {k:[]}, missing-key*` | |
| duration/timestamp | min/max seconds, negative, nanos boundary | |
| optional | `none`, `some(boundary)` | `none`, `some` |
| null | as `optional none`, as map value | |
| proto | empty, all-fields-set, unknown fields, nested | round-trip |

## 11. E2e test plan

  - **Harness:** `e2e/foreign_fn_*_test.cc` builds CEL-shaped args
    host-side, evaluates an expression that calls the custom fn through
    the full pipeline (compile -> plan -> `AddComponent` -> eval),
    asserts the `Value`. `e2e/foreign_component_fixtures/stub_demo/driver_main.cc` is the
    component-level seed (17 cases); the suite drives via `Instance::Eval`.
  - **One fn per type, in and out** (§6) + the §10 boundary matrix.
  - **Negative coverage:** wrong-arity registration, missing export at
    `AddComponent`, a fn returning `eval-error` (-> `CEL_ERROR`), a
    component trap (-> host `absl::Status`), 3VL absorption (error/
    unknown arg short-circuits before marshaling).
  - **Forcing function:** a Go-via-TinyGo component implementing the same
    `fns.wit`, proving the contract is language-agnostic (m13 §8.3).
  - **Regression pins + SKIP discipline** per CLAUDE.md.

## 12. Feature-pipeline checklist (files + tests touched)

  - `compiler/celfn/function_library.{h,cc}` (+`_test`) —
    `kForeignComponent` backend + builder entry.
  - `compiler/codegen/overload_table.cc` — none (reuses `kCelFn`); test
    that a component decl registers as `kCelFn`.
  - `abi/cel_abi.proto` + `abi_decode` — `CustomFunctionEntry.Backend`
    already has `FOREIGN`; round-trip test.
  - `eval/engine.{h,cc}` (+`_test`) — `AddComponent`; binding-validation
    errors.
  - `eval/internal/cel_component.{h,cc}` (+`_test`) — the typed
    marshaling + codec bridge; **exhaustive per-CEL-type matrix** (§6,
    §10) + 3VL/error/unknown propagation.
  - `e2e/foreign_fn_*_test.cc` — the §11 suite + the TinyGo forcing fn.
  - `bench/` — `AddComponent` invoke cost vs `kUserModule` slot-out vs
    native `AddFunction`.
  - `testing-checklist.md` — tick the foreign-component rows.

## 13. Open questions / future work (historical — as-of drafting)

The doc-time list, kept for context. The as-shipped future-work
backlog lives in §14.

  - **Component API in `eval/`** — *resolved 2026-06-03 via
    `eval/probes/m24/wasmtime_component_api_probe.{cc,BUILD.bazel}`,
    deleted at closeout.* The vendored darwin_arm64 wasmtime archive
    ships the C API component-model symbols (`wasmtime_component_new`,
    `wasmtime_component_linker_{new,instantiate,instance_add_func}`,
    `wasmtime_component_instance_get_func`,
    `wasmtime_component_func_call`).  Every typedef and decl in
    `wasmtime/component.h` (and its subtree `component/{func,instance,
    linker,types,val}.h`) is gated `#ifdef
    WASMTIME_FEATURE_COMPONENT_MODEL`, which the vendored
    `wasmtime/conf.h` does NOT define — so any consumer TU must
    force-define it at compile time
    (`copts = ["-DWASMTIME_FEATURE_COMPONENT_MODEL"]`).  With the
    define set, headers compile, symbols link, and
    `wasmtime_component_new(garbage)` returns a real error — the
    library bodies are present, not stubs.  **No Rust shim required;
    `Engine::AddComponent` (§3.5) is written natively against the
    C API.**
  - **`celfnc` codec emission**: §7 is mechanical, but the generator
    must handle every §6 row + arbitrary nesting; `e2e/foreign_component_fixtures/stub_demo`'s
    codec is hand-written proof, not the generator. (Future work, §14.)
  - **`-fno-exceptions` requirement**: wasm C++ leaf code must avoid the
    exception runtime (`__cxa_throw` unresolved); the generated build
    rules set it.
  - **`std::string_view` in / `std::string` out** as the generator
    default (prototype convention).
  - **Return ownership**: lowered strings/lists use `wit-bindgen`'s
    `*_dup_n` / list ctors; `post_return` frees them — not bare malloc.
  - **Handle lifetime (dynamic path)**: a `value` handle is scoped to one
    call; deny cross-call stashing in v1.
  - **`-c opt` re-measure** of the one-crossing typed path vs the handle
    path, to make §4 quantitative in-tree.
  - **Regime A's thin-guest compile story**: orthogonal, still m13's.

## 14. Future work (as-shipped backlog)

Surfaced during execution; not in v1 scope. Each entry is a follow-up
the v1 closeout deliberately defers.

  - **TinyGo forcing-function fixture (D.2).** A TinyGo-via-WIT
    component implementing the same `fns.wit` as a Go peer of the C++
    `stub_demo` fixture, driven through `Engine::AddComponent` from an
    e2e test. Pins the contract as language-agnostic at the test
    layer (today it's already language-agnostic at the wasmtime
    component-runtime layer, which is what passes the v1 gate).
    Blocker: TinyGo toolchain is not wired into bazel; lands with
    that wiring.
  - **Large-payload e2e — broader matrix.** v1 ships dispatch-level
    coverage at 256 KiB (string) and 10⁵ (list<int>); the marshaling
    layer covers the full type matrix at scale.  An e2e expansion
    that crosses 10⁴ map entries / proto-with-MiB-bytes through the
    full pipeline (rather than only through the cel_component
    bridge) is a natural extension once the codec generator lands —
    today it would need bespoke WAT components per shape.
  - **`bench/foreign_component` (D.3).** Production-config
    (`-c opt`, `optimize_level = 2`) head-to-head of `AddComponent`
    invoke cost vs `kUserModule` slot-out (Regime A) vs native
    `AddFunction`. Quantifies §4's "~410 ns + typed copy" claim in
    the tree and gives the embedder a measured cost to pick between
    Regimes A and B per fn. Blocker: none — the dispatch path is
    shipped; this is purely a benchmark add.
  - **`celfnc` codec generator.** The §5–§7 author
    surface assumes a generator emits `fns.wit` + `codec.h` +
    `generated_stub.cc` from the CEL decls. v1 ships the marshaling
    runtime; `e2e/foreign_component_fixtures/stub_demo` is hand-written proof of the
    target shape. The generator itself is the standalone tool that
    closes the author surface.
  - **`cel.abi` `CustomFunctionEntry.Backend = FOREIGN` wire-format
    round-trip.** The proto field exists; backend-ness stays
    invisible to the compiler today because dispatch is uniform
    through `kCelFn`. If a future bindings layer needs to discover
    per-decl backend from the `.wasm` artifact, populate the field
    at `abi.cc` emit time and add the round-trip test.
  - **`-c opt` re-measure** of §4's typed-cross vs handle-pull
    numbers, in-tree (paired with D.3).

> **Permanently out of scope, not deferred.** The foreign-fn
> author surface will **not** accept `optional<T>` or `type` as
> declarable param / return shapes — user direction, 2026-06-04.
> `Builder::Build()` rejects either at decl time (`MentionsOptional`
> / `MentionsType` walk both nest through `list`, `map`, and
> `optional` carriers, so `list<map<string, type>>` and
> `optional<type>` also fall here), naming the offending decl /
> param. This is the contract for both v1 and the foreseeable
> lifespan of the typed path; the kType Lift/Lower arm in
> `eval/internal/cel_component.cc` stays because other kCelFn /
> kHost paths can still use it, but no foreign-component decl can
> reach it.
>
> **CEL `null` (kNull) stays supported** as a declarable
> foreign-component param / return shape — it is a distinct CEL
> kind from kOptional, and `MentionsOptional` does not flag it.
> The wire-level `option<unit>` encoding for null is a hidden
> canonical-ABI detail; the author IDL and `CelfnType::Kind` both
> see it as plain `null`. Coverage: `function_library_test`
> `ForeignComponentNullParamIsAccepted` and
> `ForeignComponentNullReturnIsAccepted`.
