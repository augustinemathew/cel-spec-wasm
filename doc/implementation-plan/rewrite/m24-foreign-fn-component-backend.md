# M24 — Foreign custom functions via the Component Model

Status: research / design — drafted 2026-06-03, not yet started.
Absorbs the former separate "m25 DX" doc (backend + developer experience
are one design). Validated end-to-end (`wit/stub-demo/`, 17/17 e2e
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
of native-typed functions (`wit/stub-demo/user_fns.cc`).

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

The author edits **only the top box.** `wit/stub-demo/` is all three,
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
| `type` | `string` | `std::string` (type name) | |
| `optional<T>` | `option<wit T>` | `std::optional<C++ T>` | |
| `list<T>` | `list<wit T>` | `std::vector<C++ T>` | recurses |
| `map<K,V>` | `list<tuple<wit K, wit V>>` | `std::map<C++ K, C++ V>` | no WIT map; K ∈ {bool,int,uint,string} |
| `proto(fqn)` | `list<u8>` | the author's generated message class | serialized bytes (§8) |

Recursion is by **concrete expansion**, not a recursive type:
`list<map<string,list<int>>>` -> `list<tuple<string,list<s64>>>` nested
structs -> `std::vector<std::map<std::string,std::vector<int64_t>>>`.
The mapping is symmetric — it drives return types the same way.

## 7. The codec (generated, mechanical)

`wit/stub-demo/codec.h` is the pattern `celfnc` emits — a structural
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

`*` = covered by `wit/stub-demo/driver_main.cc` today (17 cases).

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
    asserts the `Value`. `wit/stub-demo/driver_main.cc` is the
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

## 13. Open questions / future work

  - **Component API in `eval/`**: wasmtime's component host API is a new
    eval dependency; verify the vendored build exposes it (the C API's
    component surface is thinner than Rust's — may force a shim).
  - **`celfnc` codec emission**: §7 is mechanical, but the generator
    must handle every §6 row + arbitrary nesting; `wit/stub-demo`'s
    codec is hand-written proof, not the generator.
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
