# M23 — Foreign custom functions via the Component Model

Status: research / design — drafted 2026-06-03, not yet started.
Single source of truth for foreign custom functions (absorbed the former
separate m24 "backend" and m25 "DX" docs — one feature, one doc).
Validated end-to-end: `wit/bench/` (cost) and `wit/stub-demo/` (17/17 DX
assertions on wasmtime 45). No `compiler/` / `eval/` / `runtime/` code
changes. Part II is grounded in the **shipped** headers
(`compiler/codegen/overload_table.h`, `eval/engine.h`,
`compiler/celfn/function_library.h`), not the m13 doc prose (§6 note).

Provides the foreign-module backend that `m13-custom-fns.md` scoped and
then deferred. Reuses m13's host/CEL-defined backends, checker hookup,
and codegen unchanged.

## 0. TL;DR

A foreign custom function (Go/Rust/separately-compiled C++, isolated in
its own memory) is linked to the CEL evaluator via the **Component
Model**, with two key moves:

  1. **Dispatch as a host callback** — reuses the shipped `kCelFn` /
     `Engine::AddFunction` path; compiler, checker, overload table,
     codegen, 3VL absorption all **unchanged**. New eval code:
     `Engine::AddComponent`.
  2. **Marshal via per-function typed WIT + a generated codec** — the
     author writes **only native C++** (`int64_t`/`std::string`/
     `std::vector`/`std::map`/nested) against generated stubs.

Measured cost (Part I): a cross-component call is **~410 ns fixed**,
scalars free, every *dynamic* `value`-handle op **~500 ns**. So
concretely-typed fns use the typed-WIT path (one crossing + local
deserialize); the `value` resource is reserved for the rare dynamic fn.

---

# Part I — Investigation & findings

## 1. The constraint

A custom function compiled separately, in its own wasm module, **owns
its own linear memory** and can't share the evaluator's without
colliding (two independent wasi runtimes both put `__stack_pointer` at
`66560` — empirically confirmed). Between two isolated instances only
**function calls carrying `i32/i64/f32/f64`** cross, plus a **broker**
(the native host) that can read/write both memories. A pointer never
crosses. So every aggregate (string, list, map, proto) is **copied by
the host** or referenced by an **opaque handle** the receiver calls back
to read.

## 2. This is the Component Model

Working the constraint forward yields exactly the WebAssembly Component
Model / WASI Preview 2 canonical ABI, re-derived:

| Hand-rolled contract | Component Model name |
| --- | --- |
| private memory per module | components are isolated by definition |
| host brokers the byte copy | the Canonical ABI (lift / lower) |
| callee-exported `alloc` + reset | `cabi_realloc` + `post_return` |
| position-independent inline blob | the canonical record / list encoding |
| opaque handle + pull accessors | `resource` types |
| ABI version check | WIT package version |
| scalars by value | flattened core params |

So we define the boundary in **WIT** and let `wit-bindgen` / `wasm-tools`
/ `wac` generate the lift/lower glue — none of it hand-written.

## 3. Two regimes

| | Regime A — `kUserModule` (m13) | Regime B — Component (this doc) |
| --- | --- | --- |
| memory | shared (`cel.memory`) | isolated (own) |
| call cost | single-digit ns (slot-out) | ~410 ns + typed-arg copy |
| foreign toolchain | thin guest (import memory, no libc heap) | any (normal component) |
| protos | ❌ (m13 §4.5.1) | ✅ as serialized bytes |
| trust | trusted / co-compiled | untrusted / polyglot OK |
| dispatch | direct module import | host-callback (`cel_fn`) |

m13's shipped foreign plan is **Regime A** (shared memory; fast; needs a
thin-guest compile; no protos). This doc adds **Regime B** (isolated;
any toolchain; protos as bytes; at the ~410 ns cost). They coexist; the
embedder picks per library/function.

## 4. Measured cost (`wit/bench/`)

Built for real — C++ components, separate memories, composed with `wac`,
run on wasmtime 45. Absolute ns are noisy on the shared host; the ratios
and per-operation unit are durable.

**Per argument type** (`wit/bench/arg-cost`):
  - the cross-component **call is the cost: ~410 ns fixed**, ~100x a
    same-module wasm call;
  - **scalars are free** (~1-2 ns each);
  - **a string (20-50 B) adds ~80 ns**, ~0.6 ns/byte — length in that
    band barely matters; it's per-arg fixed cost.

**Typed value model** (`wit/bench/typed-fn`):
  - `invoke-prim(3 ints, by value)` ~525 ns (1 crossing);
  - `as-primitive` (pull one value) ~543 ns (1 crossing);
  - `of-primitive` + `drop` ~1000 ns (2 crossings);
  - `invoke(3 handles)` ~2500 ns (~4 crossings, **~5x** the by-value path).

The load-bearing fact: **every `value`-resource op (construct / pull /
drop / call) is one ~500 ns crossing; handles don't amortize.** This is
why §8 prefers typed WIT over handles for concrete signatures.

## 5. The complete CEL value WIT model (`wit/cel.wit`)

Validated with `wasm-tools component wit`; `wit-bindgen c` generates all
`of-*` / `as-*`.

**The recursion constraint.** WIT forbids recursive value types
(`variant value { list(list<value>) }` -> "type depends on itself",
confirmed empirically). A CEL value is recursive (`map<string,
list<map<...>>>`), so the *dynamic* value is a **`resource`** (nominal,
may be cyclic) whose aggregate accessors return **more handles** —
arbitrary nesting without a recursive type. Coverage: every CEL type is
constructible + readable; map keys restricted to `variant map-key {bool,
int, uint, string}`; **proto messages cross as `record message {
type-name, wire: list<u8> }`** (serialized, deserialized by the far
side); `optional`/`error`/`unknown` handled (see the file).

This dynamic model is the fallback (§8); for concrete signatures the
typed path (Part II) is used instead and the resource is not needed.

---

# Part II — Design (compiler integration + developer experience)

## 6. Dispatch: a component fn is a host fn

> **m13 staleness (read the code).** `m13 §5.3` proposes an
> `ImportModule` struct; the shipped `overload_table.h` is `enum class
> ImportModule {kCelRuntime, kCelHost, kCelFn, kUserModule}` with
> `module_name` on `OverloadImpl`. This builds on the shipped enum and
> the real `RegisterCustom(overload_id, module, module_name,
> helper_name, num_args)`.

The shipped `kCelFn` flavor: codegen emits `(call $cel_fn.<helper> …)`
and the embedder binds a **native host callback** via
`Engine::AddFunction(overload_id, num_args, HostCallback)`. A
Component-backed fn is **that**, with a callback body that invokes the
foreign component. Therefore:

  - **Overload table:** `RegisterCustom(overload_id, kCelFn, "",
    helper_name, num_args)` — identical to a host custom; component-ness
    is invisible to the compiler.
  - **Codegen / checker / 3VL absorption / arity / `cel.abi`
    (Backend=FOREIGN, via `cel_fn`):** unchanged (m13 §7.2: the call
    site is backend-agnostic).

The entire new surface is eval-side (§7) — keeping m13 §5.3's "one
dispatch path, no silent absorption fork".

## 7. Pipeline mapping (file by file)

  - **Frontend** `compiler/celfn/function_library.h` — add
    `Backend::kForeignComponent` + `AddForeignComponent(fn_name,
    return_type, params)`. Same contract shape as a host fn.
  - **Checker** `compiler/frontend/` — **unchanged.**
    `MakeOverloadDecl(overload_id, …)`; cel-cpp stamps `overload_id`.
  - **Overload table** `compiler/codegen/overload_table.cc` —
    `RegisterCustom(overload_id, kCelFn, "", helper_name, num_args)`.
  - **Codegen** `compiler/codegen/expr_lower.cc` — **unchanged.**
  - **Eval** (new): `Engine::AddComponent(component_bytes, const
    FunctionLibrary& lib)` — sibling of `AddModule`/`AddFunction`:
    instantiate the component (wasmtime **component API** — new eval
    dep, §17); for each decl register a host callback (existing
    `AddFunction` path) that marshals args -> the typed export ->
    result; validate each export's `FuncType` (unbound / arity mismatch
    are `Plan`-time errors, mirroring `AddModule`).

## 8. Marshaling: typed WIT per fn + codec (default); resource for dynamic

A custom fn has a **concrete** declared signature, and a concrete type
is finite — WIT expresses it directly (`list<map<string,int>>` ->
`list<tuple<string,s64>>`), `wit-bindgen` generates the lift/lower, and
the aggregate crosses **once** (canonical-ABI copy, per-byte cheap per
§4) with the codec lifting it **locally** — one crossing + O(n) memory,
**not N crossings**. Rule by shape:

  - **Concrete-typed custom fn (common):** typed WIT per fn + codec
    (§9-§11). One crossing + local deserialize.
  - **Dynamic / variadic / `any`-typed (rare):** the §5 `value` resource
    (handles), ~500 ns/node — scalars by value, nested via handles,
    proto as bytes, chosen per value.

## 9. The author experience — three layers (validated)

```
 ┌─ author writes (user_fns.cc) ─ native C++ only ─────────────┐
 │ int64_t SumByKey(const std::map<std::string,                │
 │                  std::vector<int64_t>>& m, std::string_view) │
 └─────────────────────────────────────────────────────────────┘
 ┌─ generated by celfnc ──────────────────────────────────────┐
 │ codec.h           {ptr,len}/{f0,f1} structs  <->  std:: types│
 │ generated_stub.cc export -> codec-in -> user fn -> codec-out │
 │ fns.wit           one typed WIT func per custom fn           │
 └─────────────────────────────────────────────────────────────┘
 ┌─ off-the-shelf ─ wit-bindgen / wasm-tools / wac ───────────┘
```

The author edits **only the top box.** `wit/stub-demo/` is all three,
running -> `17 passed, 0 failed` (incl. boundaries: INT64_MIN, empty
list/map/string, embedded NUL, ragged nesting, missing key).

## 10. CEL type <-> WIT type <-> C++ type — every type, in and out

| CEL type | WIT type | C++ (author sees) | Notes |
| --- | --- | --- | --- |
| `bool` | `bool` | `bool` | |
| `int` | `s64` | `int64_t` | |
| `uint` | `u64` | `uint64_t` | |
| `double` | `f64` | `double` | |
| `string` | `string` | `std::string_view` in / `std::string` out | UTF-8 |
| `bytes` | `list<u8>` | `std::vector<uint8_t>` (or `std::string`) | |
| `null` | `option<unit>` / dynamic | `std::monostate` / via `optional` | rarely a param |
| `duration` | `record {seconds:s64,nanos:s32}` | `absl::Duration` | |
| `timestamp` | `record {seconds:s64,nanos:s32}` | `absl::Time` | |
| `type` | `string` | `std::string` (type name) | |
| `optional<T>` | `option<wit T>` | `std::optional<C++ T>` | |
| `list<T>` | `list<wit T>` | `std::vector<C++ T>` | recurses |
| `map<K,V>` | `list<tuple<wit K, wit V>>` | `std::map<C++ K, C++ V>` | no WIT map; K ∈ {bool,int,uint,string} |
| `proto(fqn)` | `list<u8>` | the author's generated message class | serialized bytes (§12) |

Recursion by **concrete expansion**, not a recursive type. Symmetric —
drives return types the same way.

## 11. The codec (generated, mechanical)

`wit/stub-demo/codec.h` is the `celfnc` pattern — a structural walk of
`{ptr,len}` (lists) / `{f0,f1}` (tuples):

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

## 12. Proto handling

`proto(fqn)` -> `list<u8>` (serialized bytes); the codec deserializes
into the author's generated message class (`celfnc` emits
`Parse`/`SerializeToString`), so the author writes `bool Allow(const
acme::User&)`. This lifts m13 §4.5.1's "no protos across the foreign
boundary": a proto that can't cross as an externref **can** cross as
bytes. Cost: serialize + parse (parse dominates) — right when the fn
needs the message, not one field.

## 13. API surface (finalized)

  - **Author** — `user_fns.cc` only: native C++ signatures via §10.
  - **Embedder (C++ host):**
    ```cpp
    auto lib = FunctionLibrary::Builder()
        .AddForeignComponent("rules","allow", BoolType,
            {{true,StringType,"u"},{false,ListType(IntType),"rs"}}).Build();
    compilerBuilder.AddLibrary(lib);             // checker sees the overloads
    engine.AddComponent(component_bytes, lib);   // NEW (§7)
    ```
  - **Generator `celfnc`** (m13 §8.2): from the CEL decls emits
    `fns.wit`, the codec, the stub, and a native-signature `user_fns`
    skeleton.

## 14. Boundary-condition matrix (the e2e suite MUST cover)

`*` = covered by `wit/stub-demo/driver_main.cc` today (17 cases).

| Type | Boundary inputs | Output boundaries |
| --- | --- | --- |
| int | `0,-1,INT64_MIN*,INT64_MAX*` | `INT64_MIN/MAX` |
| uint | `0,1,UINT64_MAX` | `UINT64_MAX` |
| double | `0.0,-0.0,NaN,±Inf,DBL_MIN,DBL_MAX` | `NaN`/`Inf` |
| bool | `true,false` | both |
| string | `""*, ascii, embedded NUL*, UTF-8, long` | empty*, NUL, UTF-8 |
| bytes | `[]*, [0x00], 0xFF run, long` | empty, NUL |
| list | `[]*, [one], [INT64_MIN]*, large` | empty, large |
| map | `{}*, {one}, all key kinds` | empty, large |
| list<list> | `[], [[]]*, ragged*` | nested empty |
| map<.,list> | `{}, {k:[]}, missing-key*` | |
| duration/timestamp | min/max seconds, negative, nanos boundary | |
| optional | `none`, `some(boundary)` | `none`, `some` |
| null | as `optional none`, as map value | |
| proto | empty, all-set, unknown fields, nested | round-trip |

## 15. E2e test plan

  - **Harness:** `e2e/foreign_fn_*_test.cc` builds CEL-shaped args
    host-side, evaluates an expression calling the custom fn through the
    full pipeline (compile -> plan -> `AddComponent` -> eval), asserts
    the `Value`. `wit/stub-demo/driver_main.cc` is the component-level
    seed; the suite drives via `Instance::Eval`.
  - **One fn per type, in and out** (§10) + the §14 boundary matrix.
  - **Negative:** wrong-arity registration, missing export at
    `AddComponent`, fn returning `eval-error` (-> `CEL_ERROR`), a
    component trap (-> host `absl::Status`), 3VL absorption.
  - **Forcing function:** a Go-via-TinyGo component implementing the same
    `fns.wit` (m13 §8.3) — proves the contract is language-agnostic.
  - Regression pins + SKIP discipline per CLAUDE.md.

## 16. Feature-pipeline checklist (files + tests touched)

  - `compiler/celfn/function_library.{h,cc}` (+`_test`) —
    `kForeignComponent` + builder entry.
  - `compiler/codegen/overload_table.cc` — none (reuses `kCelFn`); test
    that a component decl registers as `kCelFn`.
  - `abi/cel_abi.proto` + `abi_decode` — `Backend.FOREIGN` exists;
    round-trip test.
  - `eval/engine.{h,cc}` (+`_test`) — `AddComponent` + binding errors.
  - `eval/internal/cel_component.{h,cc}` (+`_test`) — typed marshaling +
    codec; **exhaustive per-type matrix** (§10, §14) + 3VL/error/unknown.
  - `e2e/foreign_fn_*_test.cc` — the §15 suite + TinyGo forcing fn.
  - `bench/` — `AddComponent` invoke vs `kUserModule` slot-out vs native
    `AddFunction`.
  - `testing-checklist.md` — tick the foreign-component rows.

## 17. Open questions / future work

  - **Component API in `eval/`**: a new eval dependency; verify the
    vendored wasmtime exposes it (C API component surface is thinner than
    Rust's — may force a shim).
  - **`celfnc` codec emission**: §11 is mechanical, but the generator
    must cover every §10 row + arbitrary nesting; `wit/stub-demo`'s codec
    is hand-written proof.
  - **`-fno-exceptions`**: wasm C++ leaf code must avoid the exception
    runtime (`__cxa_throw` unresolved); generated build rules set it.
  - **Return ownership**: lowered strings/lists use `wit-bindgen`'s
    `*_dup_n` / list ctors; `post_return` frees them.
  - **Handle lifetime (dynamic path)**: a `value` handle is scoped to one
    call; deny cross-call stashing in v1.
  - **`-c opt` re-measure** of the ~410 ns baseline and the typed-vs-
    handle path, to make §4/§8 quantitative in-tree.
  - **Regime A's thin-guest compile story**: orthogonal, still m13's.
