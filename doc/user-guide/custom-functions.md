# Custom functions — the three backends

The overview of extending CEL with your own functions: the `.celfn` IDL, the
backend prefixes, and each backend's registration surface. Deep dives with
worked examples live on their own pages:
[host functions](writing-host-functions.md) ·
[component functions](writing-component-functions.md).

---

## 1. Extending CEL with custom functions — overview

Custom functions are declared in a small **`.celfn` IDL** and come in three
backends, distinguished by the *shape* of the declaration:

| Backend | Declaration shape | Who provides the body | Registered on |
|---|---|---|---|
| **Host** | `int @host.length(string s);` | your C++ at runtime | `Engine::AddFunction` |
| **CEL-defined** ⛔ | `int @native.addone(int x) = x + 1;` | a CEL expression body | **not yet implemented** — reserved syntax; see §3 |
| **Component** ✅ | `bool @component.allow(string subject, string action);` | a wasm Component-Model component (C++ today, TinyGo/Rust designed) | `Engine::AddComponent` |

The backend is the **module prefix**: `@host` (C++ impl), `@component`
(Component-Model component), or `@native` (reserved for CEL-defined bodies —
unimplemented, §3). Every declaration carries an `@<backend>.` prefix; there
is no unprefixed form. (Grammar reference: `m13-custom-fns.md` §3.0.)

Register declarations on the `Compiler` so call sites type-check:

```cpp
auto b = celwasm::Compiler::NewBuilder();
b.AddFunction("int @host.length(string s);");       // one decl from a string
b.AddLibrary(*celwasm::ParseCelfnSource(celfn_text));   // a whole .celfn file/library (StatusOr — check in real code)
auto compiler = std::move(b).Build();
```

`AddFunction(celfn_source)` parses one (or more) decl from a string;
`AddLibrary(FunctionLibrary)` registers a parsed `.celfn` library (from
`celwasm::ParseCelfnSource(text)` or `FunctionLibrary::Builder`). A call to an
unregistered function fails at compile time with
`"undeclared reference to '<fn>'"`.

The IDL type grammar (`compiler/celfn/function_library.h`):
`bool int uint double string bytes null Duration Timestamp`, `list<T>`,
`map<K,V>` (K ∈ bool/int/uint/string), `proto(<fqn>)`, and a leading `this`
on the first param for method-style dispatch (`x.is_admin()`).

### 1.1 Building a reusable expression library (`.celfn` files)

The IDL's point is a **named, documented, reusable library** of function
declarations — a project "standard library":

```celfn
// policy.celfn  — a library of policy function declarations

/// True if the user is an adult (>= 18) per their proto `age` field.
bool @host.is_adult(proto(acme.User) u) ;

/// Look up today's rate for `currency` from the host rate table.
double @host.rate(string currency) ;
```

Parse the **whole file** into a `FunctionLibrary` and plug it into the
Compiler. **Reading the file is the caller's concern** — the API accepts the
entire IDL as a *string* (no file I/O; that keeps it embeddable + testable):

```cpp
std::string text = ReadFileToString("policy.celfn");   // YOUR file read
auto lib = celwasm::ParseCelfnSource(text);                // StatusOr<FunctionLibrary>

auto b = celwasm::Compiler::NewBuilder();
b.DeclareVariable("u", celwasm::CelType::Message("acme.User"));
b.AddLibrary(*lib);
auto compiler = std::move(b).Build();

// Now expressions reuse the library:
auto p1 = compiler->Compile("is_adult(u) && rate('USD') < 2.0");
auto p2 = compiler->Compile("rate('USD') * 1.05");
```

At eval time, bind each `@host` declaration to its C++ impl on the `Engine`
(§2).

**Doc-comments.** ✅ A `///` run (or `/** … */` block) directly above a
declaration is captured as that function's **description** (sigils + one
leading space stripped, lines joined) and exposed on `CelfnDecl` — generate
reference docs or a function picker from it. 🟡 *Carrying the description into
the Program's `cel.abi` for cross-process introspection is not wired yet;
today it's reachable via `FunctionLibrary::decls()` on the in-process
library.*

**Introspection.** Walk the registered functions:

```cpp
for (const celwasm::FunctionLibrary& lib : compiler->function_libraries()) {
  for (const celwasm::CelfnDecl& d : lib.decls()) {
    // d.fn_name, d.params, d.return_type, d.backend, d.description
  }
}
```

!!! note "Grammar status"
    ✅ The `@host`/`@native`/`@component` prefix-module grammar + doc-comment
    capture shown here is the **in-tree grammar** (`m13-custom-fns.md` §3.0).
    The loading model is unchanged: `ParseCelfnSource(text)` takes the whole
    IDL as a string; the caller reads the file.

---

## 2. Host functions (`@host.`)

A host function is implemented by your C++ at runtime. The expression imports
it; you register the impl on the `Engine`.

> **→ Full guide: [Writing host functions](writing-host-functions.md)** — the
> typed API, `HostCallContext` accessors, proto / list / map args, owning
> returns, unknown/error handling, and the canonical-type + kind-safety
> rules, with worked examples. This section is the summary.

### 2.1 The typed API — `AddTypedFunction` ✅ (recommended)

Write a plain C++ lambda over **canonical CEL types**; the binding decodes
each argument, calls you, and encodes the result. No slots, no `memcpy`, no
kind-checking by hand:

```cpp
auto b = celwasm::Compiler::NewBuilder();
b.DeclareVariable("x", celwasm::CelType::Int());
b.AddFunction("int @host.double_it(int x);");        // overload-id: double_it_int
auto program = (*std::move(b).Build()).Compile("double_it(x)");

auto engine = celwasm::Engine::NewBuilder().Build();
engine->AddTypedFunction("double_it_int",
    [](int64_t x) -> absl::StatusOr<int64_t> { return x * 2; });   // ✅
```

The lambda must return `absl::StatusOr<R>`. Only canonical CEL types compile —
`int`/`float`/`char*`/by-value proto are a **compile error**, never a silent
narrowing. Each CEL type maps to exactly one C++ type (`int`→`int64_t`,
`string`/`bytes`→`absl::string_view`, `proto(M)`→`const M&`,
`list<T>`→`HostListView`, `map<K,V>`→`HostMapView`, any→`Value`, …) — the
full table is in [Writing host functions §1.1](writing-host-functions.md).
Proto / list / map arguments and newly-allocated string / aggregate returns
all work; `list<proto(...)>` and `map<…,proto(...)>` compose by recursing into
element/value backings.

### 2.2 The context API — `HostCallContext&` ✅ (per-arg control)

When you need per-argument control (dynamic arity, mixed handling), the
`HostCallback` is `std::function<absl::Status(HostCallContext&)>`; every
accessor (`ctx.ArgInt(0)`, `ctx.ArgString` / `ArgProto` / `ArgList` /
`ArgMap` / `ArgValue`, `ctx.ReturnInt(...)`) is kind-checked and returns
`absl::StatusOr<T>` — worked example in
[Writing host functions §2](writing-host-functions.md).

Unknown / error arguments are **auto-absorbed by the trampoline before your
callback runs** (a body only ever sees all-known args); a function may
explicitly emit an unknown via `ctx.ReturnUnknown()` (stamping
`celwasm::kFunctionUnknownSentinel`). `num_args` for `AddFunction` is
`params + 1`; `AddTypedFunction` derives arity from the lambda.

> **Component backend note:** a `@component` decl crosses into a
> separately-instantiated Component-Model component, so values are marshalled
> across the boundary (proto messages travel as serialized bytes; see §4.5).
> The shared-memory zero-copy path used by `@host` is not available across
> that boundary.

## 3. CEL-defined functions (`@native`) ⛔

> **Not implemented — treat `@native` as reserved syntax.** The grammar
> reserves the `@native` backend for functions whose body is written in CEL
> itself and compiled into the same wasm module as the expression. Today a
> `@native` declaration parses and type-checks (so call sites compile), but no
> body lowering exists: a program that calls one compiles and then fails to
> evaluate. Use `@host` (§2) or `@component` (§4) for function bodies that
> need to run.

---

## 4. Component functions — cross-component linking (Rust / Go / C) ✅

> **→ Full guide: [Writing component functions](writing-component-functions.md)** —
> the `cel_wasm_component` Bazel macro, the C++ author surface, the proto
> path, the type matrix, and the open performance follow-ups. This section is
> the summary.

A component function is implemented by a **Component-Model component** you
produce from another language (Rust, TinyGo, C, …). Unlike a host function, a
component has **its own linear memory** — values are *marshalled* across the
boundary by a host trampoline.

### 4.1 Declaration and registration ✅

A component decl carries the `@component.` prefix; the embedder declares the
same shape on the C++ side (so the engine knows which exports to bind) and
supplies the component bytes at runtime:

```cpp
// Compile time: the decl makes `allow(...)` type-check.
b.AddFunction("bool @component.allow(string subject, string action);");

// Run time: supply the component's bytes + the library so the engine
// can two-level-resolve each `@component` decl against the component's
// WIT interface exports (e.g. `cel:customfn/fns@0.1.0#allow-string-string`).
engine->AddComponent(rules_component_bytes, lib);
```

Building the `rules_component_bytes` from a `.celfn` + `user_fns.cc` is one
Bazel macro call — see
[Writing component functions §2](writing-component-functions.md#2-quick-start-c).

### 4.2 One fixed Component-Model ABI + generated shims

Three generated pieces bridge a component call: caller slot glue in the expr
module (the same 24-byte CelValue slot contract `@host` uses), a
language-agnostic host trampoline (hand-written C++ dispatching on CEL type,
not source language), and the per-language component shim `celfnc` produces
from the generated WIT — your function signature looks natural, the wire
contract stays fixed.

The trampoline does a **recursive lift/lower** per the WASI Component Model
canonical ABI: lower the CEL args into the component's memory (allocating via
its exported `cabi_realloc`), call the export, lift the result back. Supported
types: scalars, `string`, `bytes`, `list<T>`, `map<K,V>`, nested aggregates,
`Duration`, `Timestamp`, and **proto messages serialized to bytes** (§4.5).
`type` and `optional` are rejected at the component boundary. A guest trap
fails the Eval cleanly rather than producing a wrong value — §4.6.

### 4.3 WASI vs plain — which toolchain target?

> **What ships today:** the C++ authoring path — `cel_wasm_component`
> compiles your `user_fns.cc` under **wasm32-wasip2**, which emits a
> Component-Model component directly. The rest of this section (and the
> stock-Go / TinyGo material in §4.4) is **probe-validated design background
> for the unshipped Go authoring path**.

Components differ in whether they pull in WASI and whether they
own/initialize their memory:

| Toolchain target | Memory | Init call | Notes |
|---|---|---|---|
| **Plain** `wasm32-unknown-unknown` (Rust `no_std`), `--target=wasm32 -nostdlib` (C) | defines its own, no WASI | none | smallest; closest to hand-WAT; just exports the fn + `cabi_realloc` |
| **WASI reactor** `wasm32-wasip1 -mexec-model=reactor` (C/clang), TinyGo `-target=wasip1 -buildmode=c-shared` | defines its own | **must call `_initialize`** | full libc available; the host calls `_initialize` once after instantiation before any export |
| Stock Go (`GOOS=wasip1`), Rust `wasm32-wasi` | defines + WASI imports | yes | heavier runtime; full WASI preview1 stdlib |

The engine negotiates this at `AddComponent` time: it instantiates the
component in the same store, calls `_initialize` if the component is a WASI
reactor, and binds its exports to the declared `@component` fns. **Plain**
targets are the lightest default for pure compute; choose **WASI** when the
function genuinely needs libc or stdlib facilities.

> **Empirically confirmed (probe — `foreign-go-bindgen-findings.md`).** A
> stock-Go module (`GOOS=wasip1`) is a reactor: `_initialize` is **mandatory**
> (skipping it traps), and it imports a real `wasi_snapshot_preview1` surface
> (10–17 funcs incl. `fd_write`, `random_get`, `clock_time_get`,
> `fd_prestat_*`) — the engine must wire a **full WASI preview1 context**, not
> stubs. **TinyGo carries the scalar/string path only** (118 KB, 2 WASI
> imports) — it **cannot** carry the §4.5 proto path: TinyGo's incomplete
> reflection traps in `proto.Unmarshal`.

> A second, deferred model — reusable *separately-instantiated* library
> modules sharing the runtime's memory via `__memory_base` relocation — is
> prototyped (`modules-and-ffi.md` §4.5) but not the v1 path.

### 4.4 Worked example: a component function in Go (`GOOS=wasip1`) ⛔ design notes

> **Go authoring is not shipped.** The shipped path is C++ via the
> `cel_wasm_component` macro — see
> [Writing component functions](writing-component-functions.md) for the
> working example. Everything below is the probe-validated *target* shape for
> the Go path (`cel generate --language=go` is pending; the in-tree plan
> favours TinyGo for size, stock Go as the proto-capable fallback — see
> writing-component-functions §4).

An authorization predicate `allow(subject, action)` implemented in Go, reused
across many CEL expressions:

**1. Declare it** in your `.celfn`:

```celfn
/// True if `subject` may perform `action`, per the Go policy component.
bool @component.allow(string subject, string action);
```

**2. Write the Go function.** You write a *natural* Go function; the
`celfnc`-generated glue (`rules_celfn.go`) handles the canonical-ABI
marshalling and the wasm export:

```go
// rules.go
package main

//celfn:export allow
func Allow(subject, action string) bool {
    return subject == "admin" || action == "read"
}

func main() {} // required; a reactor module has no real entry point
```

Under the hood the generated glue produces the fixed-ABI exports the host
trampoline calls — one per function, named by the **overload id** (`fn_name` +
each arg's type token, joined by `_`), plus the canonical ABI allocator the
host uses to place arguments in *this* component's memory:

```go
//go:wasmexport allow_string_string   // export name == overload id (verbatim)
func _allow_string_string(subjPtr, subjLen, actPtr, actLen uint32) uint32 {
    if Allow(strFromMem(subjPtr, subjLen), strFromMem(actPtr, actLen)) {
        return 1
    }
    return 0
}

//go:wasmexport cabi_realloc   // host allocates arg bytes in our memory through this
func _cabi_realloc(ptr, oldLen, align, newLen uint32) uint32 { /* … */ }
```

*(Shown value-only for clarity — the real generated export also carries a
`recover()` guard and a status slot so a panic surfaces as a CEL error, not a
trap or a spurious `false`; see §4.6.)*

**3. Build it** to a component (WASI-reactor core module wrapped with
`wasm-tools component new`):

```bash
GOOS=wasip1 GOARCH=wasm go build -buildmode=c-shared -o rules.core.wasm ./rules
wasm-tools component new rules.core.wasm -o rules.wasm
# TinyGo — far smaller (118 KB vs 1.6 MB) for a SCALAR/STRING fn, but
# cannot carry the §4.5 proto path (reflection trap); also needs
# -buildmode=c-shared for the //go:wasmexport reactor shape:
#   tinygo build -target=wasip1 -buildmode=c-shared -o rules.core.wasm ./rules
```

**4. Register + use** — the decl makes the call type-check at compile time;
the component bytes are supplied to the Engine at run time, along with the
library so the engine knows which decls to bind:

```cpp
auto lib = *celwasm::ParseCelfnSource(
    "bool @component.allow(string subject, string action);");

auto b = celwasm::Compiler::NewBuilder();
b.DeclareVariable("subject", celwasm::CelType::String());
b.AddLibrary(lib);
auto compiler = std::move(b).Build();
auto program  = compiler->Compile(R"(allow(subject, "read"))");

auto engine = celwasm::Engine::NewBuilder().Build();
engine->AddComponent(ReadFileToBytes("rules.wasm"), lib);

auto instance = engine->Plan(*program);
celwasm::Activation act;
act.Bind("subject", celwasm::Value::String("guest"));
auto v = instance->Eval(act);     // host: _initialize(rules) once at AddComponent,
                                  // then lowers the two strings into the component's
                                  // memory, calls allow_string_string, lifts the
                                  // bool → true
```

At `AddComponent` the engine instantiates `rules.wasm` **with a full WASI
preview1 context**, calls `_initialize` (mandatory for a Go wasip1 reactor),
and binds every `@component` decl to a matching export. Per call, the
trampoline lowers the CEL `string` args (via `cabi_realloc`), invokes
`allow_string_string`, and lifts the `bool` back. *(Probe-confirmed —
`foreign-go-bindgen-findings.md`.)*

### 4.5 Proto messages cross as serialized bytes

A proto message lives in the host's interner, not the component's memory, so
it cannot cross by handle. The ABI carries it as **serialized bytes**: the
host passes a `(ptr, len)` reference allocated via `cabi_realloc`; the
component's generated glue **deserializes** into that language's message type.
The wire is plain protobuf binary — any language with a protobuf runtime
works:

```celfn
/// ✅ shipped on the C++ path (the `demo_component_proto` fixture,
/// manual-tagged — libprotobuf under wasm32-wasip2 is a slow build);
/// the Go snippet below is design notes (§4.4).
bool @component.is_admin(proto(acme.User) u);
```

```go
//celfn:export is_admin
func IsAdmin(u *acmepb.User) bool { return u.GetRole() == "admin" }
// generated glue: var u acmepb.User; proto.Unmarshal(argBytes, &u); → IsAdmin(&u)
// (argBytes = the host-serialized acme.User, copied into our memory)
```

Host side: serialize `u` → bytes → `cabi_realloc` + copy into the component →
pass `(ptr, len)`. A returned proto is symmetric. The trade vs. `@host`
(which passes a zero-copy `msg_slot` handle) is a serialize/deserialize **per
call** — inherent to the component-memory boundary. Both sides need the proto
generated from the same `.proto`.

> **Probe-confirmed, with two real costs.** `proto.Unmarshal` does link and
> run inside stock-Go wasip1 wasm (validated end-to-end). But: (1) it pulls
> the **Go protobuf runtime into the module — ~+4.7 MB** (a 1.6 MB string
> module → 6.4 MB) and ~7 extra WASI imports; (2) it requires **stock Go —
> NOT TinyGo** (TinyGo's incomplete reflection traps in `proto.Unmarshal` at
> `reflect.NewAt`). So a proto-bearing foreign module is stock-Go, multi-MB,
> opt-in. See `foreign-go-bindgen-findings.md`.

> **Status of §4.4/§4.5:** the component backend is **shipped for C++
> authoring** — host trampoline, `celfnc` C++ emitters, the
> `cel_wasm_component` macro, `Engine::AddComponent` (`eval/engine.cc`), and
> the proto-as-serialized-bytes path all run end-to-end
> (`e2e/foreign_component_fixtures/cel_wasm_component_demo/`; proto via the
> manual-tagged `demo_component_proto` target). The **Go authoring path is
> designed, not implemented** — probe-validated (Go 1.24, wasmtime;
> `foreign-go-bindgen-findings.md`) but `cel generate --language=go` does not
> exist yet.

### 4.6 When a component function fails — panics, traps, and the error channel

> **What ships today:** a component function that traps mid-call surfaces as a
> **failed Eval** (a non-OK `absl::Status`) — the embedding process does not
> crash, and the failure cannot masquerade as a legitimate value (pinned by
> `e2e/foreign_component_dispatch_test.cc`,
> `TrappingComponentFnFailsEvalCleanly`). The `recover()` shim, the ABI
> `status` slot, and the re-instantiate policy below are **design notes for
> the unshipped Go authoring path** (§4.4).

A Go `panic` (or a runtime fault — nil deref, index out of range) **unwinds
to a wasm `unreachable`**, surfacing as a trap. The design turns a failure
into a CEL **error value**, never a crash and never a wrong answer:

- **A Go panic is a wasm trap (`TrapCode.UNREACHABLE`), not a WASI
  `proc_exit`.** The host trampoline catches it as an ordinary
  `wasmtime::Trap` and maps it to a CEL error (`kError`) for that eval.
  *(Probe-confirmed for explicit `panic()`, nil deref, and index-OOB —
  `foreign-go-bindgen-findings.md`.)*

- **The generated shim wraps your function in `recover()`**, so the common
  case never even traps:

  ```go
  //go:wasmexport allow_string_string   // export name == overload id (verbatim)
  func _allow_string_string(...) (status uint32) {   // status: 0 = ok, 1 = component error
      defer func() {
          if recover() != nil { status = 1 }   // panic → typed error, no trap
      }()
      // … lift args, call your Allow(...), lower the result …
      return
  }
  ```

  `celfnc` emits this guard around every user call, catching **both**
  explicit `panic()` and runtime panics.

- **The fixed ABI carries a `status` slot alongside the value**, so a
  component error is **distinguishable from a legitimate `false` / `0` /
  empty result**; on `status != 0` the trampoline writes a CEL error.
  *(`modules-and-ffi.md` §5.3; surfaced by the panic probe.)*

- **Instance recovery policy.** On an *uncaught* component trap, the engine
  writes `kError` and **re-instantiates the component** before the next eval
  (a fresh `_initialize` — cheap); Go's `fatalpanic` is nominally fatal, so
  the engine does not trust reuse of an aborted component.

Takeaway: **write your Go function as if a panic is caught and reported as a
CEL error.** If it genuinely can't produce a value, `panic` (or return the
error path) rather than returning a plausible wrong answer.

---

### 4.7 What about `Engine::AddModule`?

`Engine::AddModule(alias, wasm_bytes)` — a non-component, alias-keyed
"register a core wasm module" API — is reserved for the unimplemented
`@native` backend (§3) and is **not** the registration path for `@component`
decls. Use `AddComponent` for every component-backed function.
