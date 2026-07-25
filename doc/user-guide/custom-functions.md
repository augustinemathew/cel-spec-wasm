# Custom functions — host functions vs wasm plugins

A CEL expression sees only what the host hands it. Custom functions are
the escape hatch: you register functions, and rule authors call them
like built-ins. There are **two shipped mechanisms**, and one decision
picks between them: *does the function's code belong inside your
process?*

| | `@host.` — host function | `@plugin.` — plugin function |
|---|---|---|
| Trust model | **fully trusted** — your C++, your address space, your privileges | **untrusted OK** — sealed in its own wasm sandbox |
| Body lives in | a C++ lambda in your binary | a sandboxed wasm plugin (own linear memory) |
| Declared as | `int @host.length(string s);` | `bool @plugin.allow(string s, string a);` |
| Compile-side registration | `Builder::AddFunction(decl)` / `DeclareFunctions(lib)` | `Builder::Use(plugin)` |
| Eval-side registration | `Engine::BindFunction(decl, lambda)` | `Engine::Use(plugin)` |
| Values cross by | zero-copy slots in shared memory | marshalled across the boundary (protos as serialized bytes) |
| Update | re-link your binary | hand new bytes to a fresh `Engine` |
| Per-call cost | ~110 ns | ~450 ns |
| Guide | [Writing host functions](writing-host-functions.md) | [Writing plugins](writing-plugins.md) |

Rule of thumb: **a function body you wrote → `@host`; a function body
you didn't → `@plugin`.** (A third prefix, `@native`, is reserved for
CEL-defined bodies and unimplemented — §3.)

Both mechanisms share the same `.celfn` IDL, the same synthesized
overload-ids, and the same call-site experience — an expression cannot
tell them apart. The trust boundary is the only difference that
matters. Precise sandbox guarantees: [security model](security-model.md).

---

## 1. The `.celfn` IDL — declarations first

Custom functions are declared in a small IDL; the **backend prefix**
(`@host.` / `@plugin.` / `@native.`) selects the mechanism. Every
declaration carries a prefix; there is no unprefixed form.

The type grammar (`compiler/celfn/function_library.h`):
`bool int uint double string bytes null Duration Timestamp`, `list<T>`,
`map<K,V>` (K ∈ bool/int/uint/string), `proto(<fqn>)`, and a leading
`this` on the first param for method-style dispatch (`x.is_admin()`).

Register declarations on the `Compiler` so call sites type-check:

```cpp
auto b = celwasm::Compiler::NewBuilder();
b.AddFunction("int @host.length(string s);");       // one decl from a string
b.DeclareFunctions(*celwasm::ParseCelfnSource(celfn_text));   // a whole .celfn file/library (StatusOr — check in real code)
b.Use(plugin);                                      // a Plugin's embedded declarations (§4)
auto compiler = std::move(b).Build();
```

`AddFunction(celfn_source)` parses one (or more) decl from a string;
`DeclareFunctions(FunctionLibrary)` registers a parsed `.celfn` library
(from `celwasm::ParseCelfnSource(text)` or `FunctionLibrary::Builder`);
`Use(plugin)` registers a plugin's own embedded declarations (§4). A
call to an unregistered function fails at compile time with
`"undeclared reference to '<fn>'"`.

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
b.DeclareFunctions(*lib);
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

---

## 2. Host functions (`@host.`) — trusted, in-process

A host function is implemented by your C++ at runtime. The expression
imports it; you register the impl on the `Engine`. It runs in your
address space with your privileges — the sandbox does nothing for you
here, which is exactly right for code you already trust (an in-memory
cache lookup, a call into a library you already ship).

> **→ Full guide: [Writing host functions](writing-host-functions.md)** — the
> typed API, `HostCallContext` accessors, proto / list / map args, owning
> returns, unknown/error handling, and the canonical-type + kind-safety
> rules, with worked examples. This section is the summary.

The recommended surface is **declaration-first**: one `.celfn` string is
the single source of truth, used verbatim on both sides — the compiler
declares it, the engine binds it, and the lambda's signature is
validated against the declaration at registration:

```cpp
const char* kDecl = "int @host.discount_pct(string tier);";

builder.AddFunction(kDecl);              // compile side: declare it
engine.BindFunction(kDecl,              // eval side: implement it
    [](absl::string_view tier) -> absl::StatusOr<int64_t> {
      return tier == "gold" ? 20 : 5;
    });
```

Only canonical CEL types compile — `int`/`float`/`char*`/by-value proto
are a **compile error**, never a silent narrowing. Each CEL type maps to
exactly one C++ type (`int`→`int64_t`, `string`/`bytes`→
`absl::string_view`, `proto(M)`→`const M&`, `list<T>`→`HostListView`,
`map<K,V>`→`HostMapView`, any→`Value`, …) — the full table is in
[Writing host functions §1.1](writing-host-functions.md).

The lower-level surfaces: `AddTypedFunction(overload_id, lambda)` skips
the decl string (you spell the synthesized overload-id yourself), and
`AddFunction(overload_id, num_args, callback)` is the raw
`HostCallContext&` layer for per-argument control — every accessor
(`ctx.ArgInt(0)`, `ctx.ArgString` / `ArgProto` / `ArgList` / `ArgMap` /
`ArgValue`, `ctx.ReturnInt(...)`) is kind-checked and returns
`absl::StatusOr<T>`.

Unknown / error arguments are **auto-absorbed by the trampoline before
your callback runs** (a body only ever sees all-known args); a function
may explicitly emit an unknown via `ctx.ReturnUnknown()` (stamping
`celwasm::kFunctionUnknownSentinel`). `num_args` for the raw
`AddFunction` is `params + 1`; the typed layers derive arity from the
lambda.

## 3. CEL-defined functions (`@native`) ⛔

> **Not implemented — treat `@native` as reserved syntax.** The grammar
> reserves the `@native` backend for functions whose body is written in CEL
> itself and compiled into the same wasm module as the expression. Today a
> `@native` declaration parses and type-checks (so call sites compile), but no
> body lowering exists: a program that calls one compiles and then fails to
> evaluate. Use `@host` (§2) or `@plugin` (§4) for function bodies that
> need to run.

---

## 4. Plugin functions (`@plugin.`) — sandboxed wasm ✅

> **→ Full guide: [Writing plugins](writing-plugins.md)** — the
> quickstart, the `cel_wasm_plugin` Bazel macro, the sharing model, the
> proto path, the type matrix, and how verification works. This section
> is the summary.

A plugin function is implemented inside a **sandboxed WebAssembly
plugin** — a separate wasm artifact with its own linear memory, no
syscalls, and no access to your process. It is the path for code you
didn't write: a customer-authored scoring function, a partner's
predicate, anything not yet reviewed.

### 4.1 One noun, both sides

A plugin built with the `cel_wasm_plugin` Bazel macro is
**self-describing**: the macro embeds the `.idl` declaration text
verbatim in a `cel.fns` custom section inside the `.wasm`.
`Plugin::Load` reads it back out, so the declarations provably describe
the deployed bytes — there is no hand-written C++ mirror to drift:

```cpp
#include "abi/plugin.h"

auto plugin = celwasm::Plugin::Load(plugin_bytes).value();
// plugin.decls()      — the parsed declarations
// plugin.hash_hex()   — SHA-256 over (bytes ‖ declarations)

// Compile side: call sites type-check against the artifact's decls.
auto b = celwasm::Compiler::NewBuilder();
b.Use(*plugin);
auto compiler = std::move(b).Build();

// Eval side (possibly another process): the same noun registers the
// sandboxed backend.  Registration statically checks the plugin
// actually exports every declared function — a bad upload fails
// here, not at traffic time.
CHECK_OK(engine.Use(*plugin));
```

At `Plan`, the engine verifies every custom function the program calls
exists in its registry with an **exactly matching signature** (recorded
in the program's `cel.abi`), then instantiates only the plugins the
program actually needs — each into its own sandbox. A missing or
drifted plugin is a clean `FailedPrecondition` at `Plan`, before any
traffic.

### 4.2 What crosses the boundary

Unlike a host function (zero-copy slots in shared memory), a plugin has
its own linear memory, so a host trampoline **marshals** every call:
scalars, `string`, `bytes`, `list<T>`, `map<K,V>`, nested aggregates,
`Duration`, `Timestamp` — and **proto messages as serialized bytes**
(both sides compile the same `.proto`; the generated codec
de/serializes). `type` and `optional<T>` are rejected at the plugin
boundary. The full matrix: [Writing plugins §5](writing-plugins.md#5-type-matrix).

### 4.3 When a plugin fails

A plugin function that traps mid-call surfaces as a **failed Eval** (a
non-OK `absl::Status`) — the embedding process does not crash, and the
failure cannot masquerade as a legitimate value (pinned by
`e2e/plugin_dispatch_test.cc`, `TrappingPluginFnFailsEvalCleanly`).
Plugin state (anything `static` in the plugin, caches, allocations) is
**per-`Instance`** — see the sharing model in
[Writing plugins §4](writing-plugins.md#4-the-sharing-model-where-plugin-state-lives).

### 4.4 Legacy escape hatch: `Engine::AddPlugin(bytes, lib)`

Before plugins were self-describing, registration took the raw bytes
*plus* a hand-built `FunctionLibrary` mirroring the plugin's
declarations. That surface remains as
`Engine::AddPlugin(plugin_bytes, lib)` for exactly one audience:
**pre-`cel.fns` artifacts** — hand-built or pure-WAT plugins that carry
no embedded declarations. It validates less (no static export check;
export resolution is Plan-time only) and keeps the drift risk `Use`
was built to end. If you have such an artifact, prefer re-embedding its
declarations with `cel embed-decls` and loading it as a `Plugin`
([Writing plugins §6](writing-plugins.md#6-plugins-built-outside-the-macro-cel-embed-decls));
reach for `AddPlugin` only when you can't.

### 4.5 What about `Engine::AddModule`?

`Engine::AddModule(alias, wasm_bytes)` — an alias-keyed "register a
core wasm module" API — is reserved for the unimplemented `@native`
backend (§3) and is **not** a plugin registration path. Use
`Engine::Use` (or the `AddPlugin` escape hatch) for every
plugin-backed function.

---

## 5. Authoring languages

The shipped authoring path is **C++** via the `cel_wasm_plugin` macro
(`wasm32-wasip2`). A Go path (TinyGo for scalar/string functions, stock
Go where proto support is needed) is probe-validated design — see
[Writing plugins §7](writing-plugins.md#7-go-authoring-designed-not-implemented)
and `doc/implementation-plan/rewrite/foreign-go-bindgen-findings.md`.
