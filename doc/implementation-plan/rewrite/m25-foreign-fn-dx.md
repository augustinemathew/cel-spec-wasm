# M25 — Foreign custom-fn developer experience: typed stubs + codec

Status: research / design — drafted 2026-06-03, not yet started.
Validated end-to-end (`wit/stub-demo/`, 17/17 e2e assertions on
wasmtime 45). Builds on [`m23`](m23-foreign-fn-component-abi.md) (ABI +
cost) and [`m24`](m24-foreign-fn-component-backend.md) (compiler
integration) and **refines m24's marshaling** — see §4.

## 0. TL;DR

A custom-fn author writes **only native C++** — `int64_t`,
`std::string`, `std::vector`, `std::map`, nested — against
**auto-generated stubs**. They never see WIT, byte offsets, handles, or
the canonical ABI. The mechanism, validated in `wit/stub-demo/`:

  - **One typed WIT function per custom fn.** A custom fn has a concrete
    declared signature (`list<map<string,int>> -> int`); a concrete type
    is finite, so WIT expresses it directly (`list<tuple<string,s64>> ->
    s64`) and `wit-bindgen` generates the lift/lower. **No `value`
    resource, no per-node handle crossings** for the typed case.
  - **A generated codec** lifts `wit-bindgen`'s `{ptr,len}`/`{f0,f1}`
    structs into `std::` containers and lowers returns back.
  - **A generated stub** wires codec-in -> the author's native fn ->
    codec-out.

The author surface is one file of native-typed functions
(`wit/stub-demo/user_fns.cc`). Everything else is mechanical.

## 1. The three layers (validated)

```
 ┌─ author writes ────────────────────────────────────────────┐
 │ int64_t SumByKey(const std::map<std::string,                │
 │                  std::vector<int64_t>>& m, std::string_view) │  user_fns.cc
 └─────────────────────────────────────────────────────────────┘
 ┌─ generated (celfnc) ───────────────────────────────────────┐
 │ codec.h:  std::map<...> lift(const author_list_tuple2_      │
 │             string_list_s64_t&)   // {ptr,len}/{f0,f1} -> std│
 │ generated_stub.cc:  exports_..._sum_by_key(raw*, key*) {     │
 │     return user::SumByKey(codec::lift(*m), codec::lift(*key));}│
 │ fns.wit:  sum-by-key: func(m: list<tuple<string,list<s64>>>,│
 │             key: string) -> s64;                             │
 └─────────────────────────────────────────────────────────────┘
 ┌─ wit-bindgen + wasm-tools + wac (off-the-shelf) ───────────┐
 │ canonical-ABI lift/lower, component packaging, composition  │
 └─────────────────────────────────────────────────────────────┘
```

The author edits **only the top box.** `wit/stub-demo/` is all three,
running: `wasmtime run app.wasm` -> `17 passed, 0 failed`.

## 2. CEL type <-> WIT type <-> C++ type — every type, in and out

| CEL type | WIT type | C++ (author sees) | Notes |
| --- | --- | --- | --- |
| `bool` | `bool` | `bool` | |
| `int` | `s64` | `int64_t` | |
| `uint` | `u64` | `uint64_t` | |
| `double` | `f64` | `double` | |
| `string` | `string` | `std::string` in / `std::string` (or `std::string_view`) out | UTF-8 |
| `bytes` | `list<u8>` | `std::vector<uint8_t>` (or `std::string`) | |
| `null` | `option<unit>` / dynamic | `std::monostate` / via `optional` | rarely a declared param; usually appears as `optional` |
| `duration` | `record { seconds: s64, nanos: s32 }` | `absl::Duration` (codec converts) | |
| `timestamp` | `record { seconds: s64, nanos: s32 }` | `absl::Time` | |
| `type` | `string` | `std::string` (type name) | |
| `optional<T>` | `option<wit(T)>` | `std::optional<C++(T)>` | |
| `list<T>` | `list<wit(T)>` | `std::vector<C++(T)>` | recurses |
| `map<K,V>` | `list<tuple<wit(K), wit(V)>>` | `std::map<C++(K), C++(V)>` | WIT has no map type; K ∈ {bool,int,uint,string} |
| `proto(fqn)` | `list<u8>` | the author's generated message class | serialized bytes; codec deserializes (§7) |

Recursion is by **concrete expansion**, not a recursive type:
`list<map<string,list<int>>>` -> `list<tuple<string,list<s64>>>` nested
structs -> `std::vector<std::map<std::string,std::vector<int64_t>>>`.
The `value` resource (m23/m24) is **only** for a genuinely dynamic /
variadic custom fn whose arg types aren't known at declare time (§4).

The mapping is symmetric: the same table drives **return** types
(`-> s64` from `int64_t`, `-> string` from `std::string`, etc.),
confirmed by `shout` (string out) and the `s64`-returning aggregates in
the prototype.

## 3. The codec (generated, mechanical)

`wit/stub-demo/codec.h` is the pattern celfnc emits. Every conversion is
a structural walk of `{ptr,len}` (lists) and `{f0,f1}` (tuples):

```cpp
std::vector<int64_t>  lift(const author_list_s64_t& l){ return {l.ptr, l.ptr+l.len}; }
std::map<std::string,int64_t> lift(const author_list_tuple2_string_s64_t& m){
  std::map<std::string,int64_t> r;
  for (size_t i=0;i<m.len;i++) r.emplace(std::string((const char*)m.ptr[i].f0.ptr, m.ptr[i].f0.len), m.ptr[i].f1);
  return r;
}
void lower(author_string_t* ret, std::string_view s){ author_string_dup_n(ret, s.data(), s.size()); }
```

Because the type is concrete, the codec is finite, total, and trivially
generatable per signature. No reflection, no dynamic dispatch.

## 4. Why typed-per-fn beats the `value` resource (reconciles m24)

> **Refinement of m24 §4.** m24's marshaling adapter leaned on the
> `value` resource (handles + pull accessors) for aggregates. For a
> **concretely-typed** custom fn that is the wrong default: it costs
> ~500 ns *per node visited* (m23 §5 measured). Typed-per-fn WIT crosses
> the whole aggregate **once** (canonical-ABI copy of a flat
> `list<...>`, ~per-byte cheap per m23's arg-cost sweep) and the codec
> lifts it **locally** — O(n) memory, **one** crossing, not N.

So the rule, by argument shape:

  - **Concrete-typed custom fn (the overwhelming common case)** -> typed
    WIT per fn + codec (this doc). One crossing + local deserialize.
  - **Dynamic / variadic / `any`-typed custom fn (rare)** -> the `value`
    resource from m23/m24 (handles), accepting ~500 ns/node.

m24's host-callback dispatch (§2 there) is unchanged — a typed component
fn is still bound through `Engine::AddComponent` and dispatched via the
`cel_fn` trampoline; only the *marshaling* is typed-WIT instead of
handle-based.

## 5. API surface (finalized)

### 5.1 Author surface — `user_fns.cc` only
Native C++ signatures matching the declared CEL types via §2. That is
the **entire** thing a custom-fn author writes. (Other languages: the
same model — TinyGo/Rust authors write native types against their
generated stubs; the codec is language-specific, the WIT is shared.)

### 5.2 Embedder surface (C++ host)
Reuses m24 / the shipped `function_library.h` + `engine.h`:

```cpp
auto lib = FunctionLibrary::Builder()
    .AddForeignComponent("rules", "allow", BoolType,
        {{true, StringType, "u"}, {false, ListType(IntType), "rs"}})
    .Build();                                  // declares contract + CEL types
compilerBuilder.AddLibrary(lib);              // checker sees the overloads
// ... compile ...
engineBuilder/engine.AddComponent(component_bytes, lib);  // NEW (m24 §3.5)
```

`AddComponent` instantiates the foreign component, validates each
declared fn is exported with the matching `FuncType`, and binds it via
the existing `cel_fn` host-callback path.

### 5.3 Generator surface — `celfnc` (m13 §8.2)
Input: the CEL function decls (a `.celfn` IDL or programmatic
`FunctionLibrary`). Output, per target language: `fns.wit`, the codec,
the stub, and a `user_fns` skeleton with the native signatures for the
author to fill. The author runs `celfnc`, fills the bodies, builds.

## 6. Boundary-condition matrix (the e2e suite MUST cover)

Per the repo's "cover the edge-case matrix — this is a compiler" rule,
every type gets in + out + boundary coverage. The prototype seeds the
starred rows; the suite fills the rest.

| Type | Boundary inputs to test | Output boundaries |
| --- | --- | --- |
| int | `0, -1, INT64_MIN*, INT64_MAX*` | return `INT64_MIN/MAX` |
| uint | `0, 1, UINT64_MAX` | return `UINT64_MAX` |
| double | `0.0, -0.0, NaN, +/-Inf, DBL_MIN, DBL_MAX` | return `NaN`/`Inf` |
| bool | `true, false` | both |
| string | `"" *, ascii, embedded NUL *, multi-byte UTF-8, long` | empty *, NUL, UTF-8 |
| bytes | `[] *, [0x00], 0xFF run, long` | empty, NUL bytes |
| list | `[] *, [one], [INT64_MIN] *, large` | empty, large |
| map | `{} *, {one}, dup-ish keys, all key kinds (bool/int/uint/string)` | empty, large |
| list<list> | `[] , [[]] *, ragged *` | nested empty |
| map<.,list> | `{} , {k:[]}, missing-key lookup *` | |
| duration/timestamp | min/max seconds, negative, nanos boundary | |
| optional | `none`, `some(boundary)` | `none`, `some` |
| null | as `optional none`, as map value | |
| proto | empty message, all-fields-set, unknown fields, nested message | round-trip |

`*` = covered by `wit/stub-demo/driver_main.cc` today (17 cases).

## 7. Proto handling

A `proto(fqn)` arg/return maps to `list<u8>` (serialized wire bytes).
The codec deserializes into the author's generated message class
(`celfnc` emits the `Parse`/`SerializeToString` calls), so the author
writes `bool Allow(const acme::User& u)`. This is how m25 lifts m13
§4.5.1's "no protos across the foreign boundary" rule: a proto that
can't cross as an externref **can** cross as bytes — one copy, decoded
by the author's own proto runtime. Cost: serialize + parse (m-proto
bench on the other branch measured this; parse dominates, so it's the
right call only when the fn needs the message, vs. pulling a field).

## 8. The e2e test plan

  - **Harness shape:** `e2e/foreign_fn_*_test.cc` builds CEL-shaped args
    host-side, evaluates an expression that calls the custom fn through
    the full pipeline (compile -> plan -> AddComponent -> eval), asserts
    the `Value` result. `wit/stub-demo/driver_main.cc` is the
    component-level seed (17 cases); the real suite drives through
    `Instance::Eval`.
  - **One fn per type, in and out** (§2 rows) + the §6 boundary matrix.
  - **Negative coverage:** wrong-arity registration, missing export at
    `AddComponent`, a fn returning an `eval-error` (-> `CEL_ERROR`), a
    component trap (-> host `absl::Status`), 3VL absorption (error/
    unknown arg short-circuits before marshaling).
  - **The forcing function:** a Go-via-TinyGo component implementing the
    same `fns.wit`, proving the contract is language-agnostic (m13 §8.3).
  - **Regression pins:** each fixed bug / unlocked type gets a named
    case; SKIP discipline per CLAUDE.md (reasoned `GTEST_SKIP`, never a
    silent omission).

## 9. Open questions / generator notes

  - **`celfnc` codec emission**: the §3 pattern is mechanical, but the
    full generator must handle every §2 row + arbitrary nesting; the
    `wit/stub-demo` codec is hand-written proof, not the generator.
  - **`-fno-exceptions` requirement**: wasm C++ leaf code must avoid the
    exception runtime (`__cxa_throw` unresolved). The generated build
    rules must set it; document for authors who add `throw`.
  - **`std::string_view` vs `std::string` for in-params**: prototype
    uses `string_view` (zero-copy view into the lifted buffer, valid for
    the call). Generator should default to `string_view` in, `std::string`
    out.
  - **Return ownership**: lowered return strings/lists are allocated via
    `wit-bindgen`'s `*_dup_n` / list ctors; the canonical ABI's
    `post_return` frees them. The codec must use those, not bare malloc.
  - **Map key kinds**: WIT tuples accept any key; CEL restricts to
    bool/int/uint/string. The generator rejects other key kinds at
    decl time (matches m23 `map-key`).
  - **`-c opt` re-measure** of the one-crossing aggregate path vs the
    handle path, to make §4's "one copy beats N crossings" quantitative
    in-tree.
