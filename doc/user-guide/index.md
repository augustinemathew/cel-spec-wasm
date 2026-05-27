# CEL → WebAssembly — User Guide

This is the embedder's guide to the CEL-to-WebAssembly AOT compiler: how
to compile a CEL expression to a wasm module, evaluate it, bind
variables, and extend the language with custom functions (host-backed,
CEL-defined, or foreign modules written in Rust/Go/C).

Every code snippet uses the real public API (`compiler/compiler.h`,
`compiler/program.h`, `eval/engine.h`, `eval/instance.h`,
`eval/activation.h`, `eval/value.h`, `shared/type.h`). Where
a surface is declared but not yet fully wired, it is called out with a
**Status** line — this guide documents the *target* API and is explicit
about what evaluates today vs. what is planned.

> **Implementation-status legend.** Throughout: ✅ = shipped + tested;
> 🟡 = surface declared, behavior partial/aspirational; ⛔ = designed,
> not yet implemented. A consolidated status table is in §10.

> **Detailed guides.** Topics that warrant a deep dive with worked
> examples live on their own pages; this index is the overview + the
> compile/run API. So far:
> - [Writing host functions](writing-host-functions.md) — the typed,
>   context, and raw APIs; proto / list / map args; returns; errors;
>   the canonical-type and kind-safety rules.

---

## 1. Mental model: two phases, one serialization boundary

The system splits cleanly into a **compile-time** half and a
**runtime** half, joined by a `Program` (portable wasm bytes):

```
        compile time                    │            run time
  ┌─────────────────────────┐           │     ┌──────────────────────────┐
  │ celwasm::Compiler            │           │     │ celwasm::Engine              │
  │   .Compile("source")     │ ── Program ──►  │   .Plan(program)         │
  │   → celwasm::Program         │  (wasm bytes +  │   → celwasm::Instance        │
  │   (no wasmtime dep)      │   cel.abi)      │     .Eval(activation)    │
  └─────────────────────────┘           │     │     → celwasm::Value          │
                                          │     └──────────────────────────┘
```

- **`celwasm::Compiler`** — pure compile-time. Holds variable + custom-fn
  declarations and the descriptor pool. No wasmtime dependency. One
  Compiler produces many Programs. (`compiler/compiler.h`)
- **`celwasm::Program`** — the compiled artifact: wasm bytes + a `cel.abi`
  custom section. Pure data — copyable, serializable, shippable to
  another process/host. (`compiler/program.h`)
- **`celwasm::Engine`** — pure runtime. Owns the shared `wasm_engine_t` +
  the parsed `cel_runtime.wasm`. **Process-shared and thread-safe for
  `Plan`.** (`eval/engine.h`)
- **`celwasm::Instance`** — one live evaluator: a wasmtime store + the
  instantiated modules. **Thread-owned** (bind one per worker). Holds a
  `shared_ptr` to the Engine state, so it keeps working even after the
  Engine handle is dropped. (`eval/instance.h`)

The split means you **compile once** and **evaluate many times**, and
you can compile in one process and evaluate in another (ship the
`Program` bytes).

---

## 2. Quick start

```cpp
#include "compiler/compiler.h"
#include "eval/engine.h"

// ── Compile time ──
auto builder = celwasm::Compiler::NewBuilder();
builder.DeclareVariable("x", celwasm::CelType::Int());
auto compiler = std::move(builder).Build();              // StatusOr<Compiler>
CHECK_OK(compiler);

auto program = compiler->Compile("x + 1");               // StatusOr<Program>
CHECK_OK(program);

// ── Run time ──
auto engine = celwasm::Engine::NewBuilder().Build();         // StatusOr<Engine>
CHECK_OK(engine);

auto instance = engine->Plan(*program);                  // StatusOr<Instance>
CHECK_OK(instance);

celwasm::Activation act;
act.Bind("x", celwasm::Value::Int(41));
auto result = instance->Eval(act);                       // StatusOr<Value>
CHECK_OK(result);
CHECK_EQ(*result->AsInt(), 42);
```

`Build()`, `Compile()`, `Plan()`, and `Eval()` all return
`absl::StatusOr` — check before dereferencing. Errors carry actionable
messages (parse/type-check failures, unbound variables, kind
mismatches).

---

## 3. The Compiler (compile-time API)

### 3.1 Declaring the environment

Build a `Compiler` once with all variables and custom functions the
expressions will reference:

```cpp
auto b = celwasm::Compiler::NewBuilder();
b.DeclareVariable("name", celwasm::CelType::String());
b.DeclareVariable("age",  celwasm::CelType::Int());
b.DeclareVariable("user", celwasm::CelType::Message("acme.User"));   // proto by FQN
auto compiler = std::move(b).Build();
```

- `DeclareVariable(name, CelType)` — the checker resolves every ident
  against this list; a reference to an undeclared variable fails at
  `Compile` time with `InvalidArgument`.
- `CelType` factories: `Int()`, `Uint()`, `Double()`, `Bool()`,
  `String()`, `Bytes()`, `Message(fqn)`, list/map composites, etc.
  Message types resolve against the process-wide
  `google::protobuf::DescriptorPool::generated_pool()` (statically-linked
  `cc_proto_library` descriptors are reachable automatically).
- `Build()` consumes the builder (`std::move(b).Build()`) and returns
  `StatusOr<Compiler>`. It rejects duplicate variable names, unknown
  (`kUnknown`) types, and message types with empty FQNs.

### 3.2 Compiling

```cpp
celwasm::CompilerOptions opts;
opts.mem_size_bytes = 128 * 1024;   // linear-memory size (default: 2 wasm pages)
opts.container      = "acme";       // optional namespace for short-form idents
opts.optimize_level = 2;            // wasm-opt -O level: 0 (default) … 3

auto program = compiler->Compile("age >= 18 && name.startsWith('A')", opts);
```

**Status mapping** (flows through from the pipeline):

| Code | Cause |
|---|---|
| `InvalidArgument` | parse failure, type-check failure, static-subset violation (DYN / unbound function / type-param), `optimize_level` ∉ [0,3] |
| `Unimplemented` | an AST shape this release doesn't handle yet |
| `ResourceExhausted` | the expression's static footprint (constants + workspace) overruns the reserved low-memory region — simplify it |
| `FailedPrecondition` | internal: emitted module failed validation (compiler bug — file it) |

**`optimize_level`** trades compile cost for eval speed. `0` (default)
is byte-identical, fastest to compile; `2` is the recommended
production setting (compile once, eval many) — roughly halves eval time
on chain-heavy expressions. (`compiler/compiler.h` documents the
per-level trade-offs.)

### 3.3 The Program is portable

```cpp
absl::Span<const uint8_t> bytes = program->wasm_bytes();   // serialize / cache / ship
// elsewhere / later:
celwasm::Program reloaded(std::vector<uint8_t>(bytes.begin(), bytes.end()));
```

A `Program` holds no engine state — copy it, write it to disk, send it
across a process boundary, then `Engine::Plan` it on the far side.

---

## 4. The Eval runtime (Engine + Plan + Instance)

### 4.1 Engine — process-shared

```cpp
auto engine = celwasm::Engine::NewBuilder().Build();   // do this ONCE per process
```

Building an Engine parses `cel_runtime.wasm` into a module and stands up
the shared `wasm_engine_t`. This is the expensive setup; amortize it
across the process. **`Engine::Plan` is safe to call concurrently from
many threads** — each call mints an independent store/linker/memory,
sharing only the (thread-safe) engine + parsed runtime module.

> **Not** thread-safe: `Engine::AddFunction` / `AddModule` (custom-fn
> registration — see §6, §7). Configure those once at startup, *then*
> `Plan` from many threads.

### 4.2 Plan — Program → Instance

```cpp
auto instance = engine->Plan(*program);   // StatusOr<Instance>
```

`Plan` host-allocates the shared linear memory, instantiates
`cel_runtime.wasm` and the expr module against it, wires up the host
trampolines, and looks up the `eval` export. `FailedPrecondition` on
malformed bytes, missing imports, or a trap during instantiation.

### 4.3 Instance — evaluate

`Instance` is **thread-owned**: one per worker thread, reused across
many evals.

```cpp
// Variable-free expression:
auto v = instance->Eval();                   // StatusOr<Value>

// With bindings:
celwasm::Activation act;
act.Bind("age", celwasm::Value::Int(20));
act.Bind("name", celwasm::Value::String("Ann"));
auto v2 = instance->Eval(act);
```

Each `Eval` resets the per-eval arena first, so back-to-back calls on
the same Instance are independent and deterministic. Re-bind a fresh (or
reused) `Activation` per call.

`Eval` returns the decoded `Value` for any scalar result
(null/bool/int/uint/double/string/bytes, plus duration/timestamp).
A declared variable missing from the activation → `FailedPrecondition`;
a bound `Value` whose kind disagrees with the declared type →
`InvalidArgument`.

### 4.4 PartialEval — unknowns

For policy/attribute use cases, evaluate with a set of "unknown"
attribute patterns: a field read matching a pattern short-circuits to an
unknown result instead of descending the value.

```cpp
auto v = instance->PartialEval(act, /*unknowns=*/patterns);   // StatusOr<Value>
// v->IsUnknown() may be true; v->UnknownAttribute() identifies which.
```

### 4.5 Activation and Value

`Activation` maps variable names → `Value`s for one Eval:

```cpp
celwasm::Activation act;
act.Bind("x", celwasm::Value::Int(42))
   .Bind("s", celwasm::Value::String("hi"));     // fluent; overwrites prior binds
```

`Value` is the host-side counterpart to the 24-byte wire value. Build
with named factories, inspect with `StatusOr<T> AsX()`:

```cpp
celwasm::Value::Int(42);                 celwasm::Value::String("hi");
celwasm::Value::Bool(true);              celwasm::Value::Bytes(std::string{...});
celwasm::Value::Double(3.14);            celwasm::Value::Duration(absl::Seconds(5));
celwasm::Value::Message(my_proto);       celwasm::Value::List({Value::Int(1), Value::Int(2)});
celwasm::Value::Map({{Value::String("k"), Value::Int(1)}});

auto i = v.AsInt();        // StatusOr<int64_t>   (InvalidArgument on kind mismatch)
auto s = v.AsString();     // StatusOr<string_view>
bool null   = v.IsNull();
bool unk    = v.IsUnknown();
bool err    = v.IsError();
```

### 4.6 Lifetime & concurrency summary

| Object | Ownership | Concurrency |
|---|---|---|
| `Compiler` | value, copyable | immutable after Build; share freely |
| `Program` | value, copyable | immutable; share/serialize freely |
| `Engine` | one per process | `Plan` concurrent-safe; `AddFunction`/`AddModule` single-thread setup |
| `Instance` | one per worker thread | thread-owned; outlives the Engine handle (shared_ptr) |
| `Activation` | per-eval, reusable | not shared across threads |

---

## 5. Extending CEL with custom functions — overview

Custom functions are declared in a small **`.celfn` IDL** and come in
three backends, distinguished by the *shape* of the declaration:

| Backend | Declaration shape | Who provides the body | Registered on |
|---|---|---|---|
| **Host** | `int @host.length(string s);` | your C++ at runtime | `Engine::AddFunction` |
| **CEL-defined** ⛔ | `int @native.addone(int x) = x + 1;` | a CEL expression body | nothing — *would be* compiled in (parses + type-checks today, does not evaluate — §7) |
| **Foreign** | `bool rules.allow(string subject, string action);` | a Rust/Go/C wasm module | `Engine::AddModule` |

The backend is the **module prefix**: `@host` (C++ impl), `@native`
(CEL body, with `= …`), or a plain alias (foreign wasm module). The `@`
sigil is reserved for the two built-ins. (Grammar reference:
`m13-custom-fns.md` §3.0.)

Register declarations on the `Compiler` so call sites type-check:

```cpp
auto b = celwasm::Compiler::NewBuilder();
b.AddFunction("int @host.length(string s);");       // one decl from a string
b.AddLibrary(*celwasm::ParseCelfnSource(celfn_text));   // a whole .celfn file/library (StatusOr — check in real code)
auto compiler = std::move(b).Build();
```

`AddFunction(celfn_source)` parses one (or more) decl from a string;
`AddLibrary(FunctionLibrary)` registers a parsed `.celfn` library (build
one with `celwasm::ParseCelfnSource(text)` or programmatically via
`FunctionLibrary::Builder`). A call to an unregistered function fails at
compile time with `"undeclared reference to '<fn>'"`.

The IDL type grammar (`compiler/celfn/function_library.h`):
`bool int uint double string bytes null Duration Timestamp`,
`list<T>`, `map<K,V>` (K ∈ bool/int/uint/string), `proto(<fqn>)`, and a
leading `this` on the first param for method-style dispatch
(`x.is_admin()`).

### 5.1 Building a reusable expression library (`.celfn` files)

The point of the IDL is to **refactor ad-hoc CEL expressions into a
named, documented, reusable library** — a project "standard library" of
expressions. You write a `.celfn` file, document each function with a
doc-comment, load the whole file, and register it on the Compiler:

```celfn
// policy.celfn  — a library of reusable policy expressions

/// True if the user is an adult (>= 18) per their proto `age` field.
bool @native.is_adult(this proto(acme.User) u) = u.age >= 18 ;

/// Risk score for a transaction amount: 0 (low) … 2 (high).
int @native.risk(int amount) =
    amount > 10000 ? 2 : (amount > 1000 ? 1 : 0) ;

/// Look up today's rate for `currency` from the host rate table.
double @host.rate(string currency) ;
```

Parse the **whole file** into a `FunctionLibrary` and plug it into the
Compiler. **Reading the file is the caller's concern** — the API
accepts the entire IDL as a *string* (it does no file I/O itself; that
keeps it embeddable + testable):

```cpp
std::string text = ReadFileToString("policy.celfn");   // YOUR file read
auto lib = celwasm::ParseCelfnSource(text);                // StatusOr<FunctionLibrary>

auto b = celwasm::Compiler::NewBuilder();
b.DeclareVariable("u", celwasm::CelType::Message("acme.User"));
b.AddLibrary(*lib);
auto compiler = std::move(b).Build();

// Now expressions reuse the library:
auto p1 = compiler->Compile("is_adult(u) && risk(amount) < 2");
auto p2 = compiler->Compile("rate('USD') * 1.05");
```

**Doc-comments.** ✅ A `///` run (or a `/** … */` block) directly above a
declaration is captured as that function's **description** (sigils + one
leading space stripped, lines joined) and exposed on `CelfnDecl` via the
library's introspection surface — so you can generate reference docs or a
function picker. 🟡 *Carrying the description into the Program's `cel.abi`
for cross-process introspection is not wired yet; today it's reachable
via `FunctionLibrary::decls()` on the in-process library.*

**Introspecting the library.** Walk the registered functions (name,
signature, backend, description) to list or document what's available:

```cpp
for (const celwasm::FunctionLibrary& lib : compiler->function_libraries()) {
  for (const celwasm::CelfnDecl& d : lib.decls()) {
    // d.fn_name, d.params, d.return_type, d.backend, d.description
  }
}
```

> **Grammar status.** ✅ The `@host`/`@native`/`<alias>` prefix-module
> grammar + doc-comment capture shown here is the **in-tree grammar**
> (`m13-custom-fns.md` §3.0, shipped 2026-05-24): the backend is selected
> by the module token, there is no `Module` directive, and a `///` run or
> `/** … */` block above a decl is captured as its `description`. The
> loading model is unchanged: `ParseCelfnSource(text)` takes the whole
> IDL as a string and the caller reads the file.

---

## 6. Host functions (`@host.`)

A host function is implemented by your C++ at runtime. The expression
imports it; you register the impl on the `Engine`.

> **→ Full guide: [Writing host functions](writing-host-functions.md).**
> The typed `AddTypedFunction` API (recommended), the `HostCallContext`
> accessors, proto / list / map args, owning returns, unknown/error
> handling, and the canonical-type + kind-safety rules — all with
> worked examples. This section is the in-index summary.

### 6.1 The typed API — `AddTypedFunction` ✅ (recommended)

Write a plain C++ lambda over **canonical CEL types**; the binding
decodes each argument, calls you, and encodes the result. No slots, no
`memcpy`, no kind-checking by hand:

```cpp
auto b = celwasm::Compiler::NewBuilder();
b.DeclareVariable("x", celwasm::CelType::Int());
b.AddFunction("int @host.double_it(int x);");        // overload-id: double_it_int
auto program = (*std::move(b).Build()).Compile("double_it(x)");

auto engine = celwasm::Engine::NewBuilder().Build();
engine->AddTypedFunction("double_it_int",
    [](int64_t x) -> absl::StatusOr<int64_t> { return x * 2; });   // ✅
```

The lambda must return `absl::StatusOr<R>`. Only canonical CEL types
compile — `int`/`float`/`char*`/by-value proto are a **compile error**,
never a silent narrowing. Each CEL type maps to exactly one C++ type:
`int`→`int64_t`, `uint`→`uint64_t`, `double`→`double`, `bool`→`bool`,
`string`/`bytes`→`absl::string_view` (return `std::string`),
`Duration`→`absl::Duration`, `Timestamp`→`absl::Time`,
`proto(M)`→`const M&` (or `const google::protobuf::Message*` for the
polymorphic, no-cast form; return `std::unique_ptr<M>`, owning),
`list<T>`→`HostListView`, `map<K,V>`→`HostMapView`, any→`Value`.
Proto / list / map arguments and newly-allocated string / aggregate
returns all work — `list<proto(...)>` and `map<…,proto(...)>` compose by
recursing into element/value backings.

### 6.2 The context API — `HostCallContext&` ✅ (per-arg control)

When you need per-argument control (dynamic arity, mixed handling), the
`HostCallback` is `std::function<absl::Status(HostCallContext&)>`; every
accessor is kind-checked and returns `absl::StatusOr<T>`:

```cpp
engine->AddFunction("clamp_int_int_int", /*num_args=*/4,   // 3 params + out_slot
    [](celwasm::HostCallContext& ctx) -> absl::Status {
      auto v = ctx.ArgInt(0);  if (!v.ok()) return v.status();
      // ... ctx.ArgString / ArgProto / ArgList / ArgMap / ArgValue ...
      return ctx.ReturnInt(*v);
    });
```

Unknown / error arguments are **auto-absorbed by the trampoline before
your callback runs** (so a body only ever sees all-known args); a
function may explicitly emit an unknown via `ctx.ReturnUnknown()`
(stamping `celwasm::kFunctionUnknownSentinel`). `num_args` for
`AddFunction` is `params + 1`; `AddTypedFunction` derives arity from the
lambda. **Full detail + worked examples:
[Writing host functions](writing-host-functions.md).**

> ⛔ **Foreign exception:** proto-bearing aggregates are rejected for a
> foreign (`<alias>`) decl — `proto`, `list<proto…>`, and `map<…,proto…>`
> can't cross into a foreign module's separate memory (`MentionsProto`
> recurses to catch the nested cases). They're allowed only for
> `@host`/`@native` (shared memory + interner).

## 7. CEL-defined functions (`@native`) ⛔

A CEL-defined function has a body written in CEL itself. The *intent* is
that it compiles **into the same wasm module** as the expression (no
separate module, no host callback, no runtime registration):

> **Status: designed + declared, not implemented.** A `@native` decl
> **parses and type-checks today** — the grammar accepts it, the
> `FunctionLibrary` captures the body, and the checker registers the
> overload so call sites type-check and `Compile` succeeds. But it does
> **not evaluate**: the body-lowering producer is an unimplemented
> header stub. `compiler/celfn/library_module.h` *declares*
> `CompileLibraryBodies(...)` but there is no `library_module.cc`, no
> BUILD target, and **no caller** — `compiler/internal/compile.cc` never
> populates `CompiledArtifact.library_modules` (`compile.h:113`), and
> `eval/engine.cc`'s `Plan` never registers a CEL-defined library
> module. So compiling `addone(41)` succeeds, but evaluating it cannot
> produce a result on this branch. The earlier "N-functions-in-one-module"
> codegen did not survive the repo reorg; only the `library_module.h`
> scaffold remains. The syntax + API below is the **target** shape.

```celfn
int    @native.addone(int x)      = x + 1;
string @native.greet(string name) = "hi " + name;
bool   @native.is_adult(this proto(acme.User) u) = u.age >= 18;   // method form
```

```cpp
auto b = celwasm::Compiler::NewBuilder();
b.AddFunction("int @native.addone(int x) = x + 1;");
auto compiler = std::move(b).Build();
auto program  = compiler->Compile("addone(41)");           // no Engine::AddFunction

auto engine   = celwasm::Engine::NewBuilder().Build();
auto instance = engine->Plan(*program);
auto v = instance->Eval();                                  // ⛔ target → 42;
                                                            // does not evaluate today
```

Intended properties (target shape — see the Status callout above):

- The body is type-checked with its params injected as variables; it may
  reference **only its own declared params** (not the outer
  expression's variables). (This much is real today — type-checking
  works; it is *body lowering / eval* that is unimplemented.)
- Bodies are intended to lower to internal wasm functions in disjoint
  static memory bands; many small functions would be fine, but the
  expression + all bodies must fit the reserved low-memory region (else
  `ResourceExhausted`).
- **Recursion is rejected.** CEL is a total language; a self- or
  mutually-recursive body (a cycle in the CEL-defined call graph) is
  rejected at compile time with `InvalidArgument`. A non-cyclic call
  chain (`f` calls `g` calls a builtin) is fine.
- A CEL-defined body is designed to be able to call host functions and
  other (non-cyclic) CEL-defined functions.

> **Current eval coverage:** none — `@native` bodies do **not** evaluate
> on this branch. `CompileLibraryBodies` (the producer that would lower
> the bodies) is an unimplemented header stub with no codegen,
> registration, or e2e; scalar/string returns are **not** proven
> end-to-end today. When the producer lands, `list`/`map` params/returns
> inside CEL-defined bodies will additionally share the marshalling gap
> described in §6.2.

---

## 8. Foreign functions — cross-module linking (Rust / Go / C) ⛔

> **Status: designed, not yet implemented.** The full design is in
> `doc/implementation-plan/rewrite/modules-and-ffi.md` §5. This section
> describes the intended embedder experience.

A foreign function is implemented by a wasm module **you** produce from
another language (Rust, TinyGo, C, …). Unlike CEL-defined functions
(same module, shared memory) or host functions (C++ in the embedder), a
foreign module has **its own linear memory** — so values must be
*marshalled* across the boundary by a host trampoline.

### 8.1 Declaration and registration

A foreign decl carries an `<alias>.` prefix (no `@`); the alias is
implicit by use:

```
bool rules.allow(string subject, string action);     // alias = "rules"
```

```cpp
// Compile time: the decl makes `rules.allow(...)` type-check.
b.AddFunction("bool rules.allow(string subject, string action);");

// Run time: supply the foreign module's bytes under the alias.
engine->AddModule("rules", rules_wasm_bytes);
```

### 8.2 One fixed C ABI + generated shims

A foreign call is bridged by **three generated pieces**: (1) **caller
slot glue** the compiler emits in the expr module — it reads each
argument's 24-byte CelValue from its slot and writes it into the call's
arg area, and reads the result back from the out_slot; (2) the
**language-agnostic host trampoline** (`cel_call_foreign`) — hand-written
C++ that does the cross-memory lift/lower, dispatching on CEL type, not
source language; (3) the **per-language foreign shim** that `celfnc` (a
mini `wit-bindgen`) generates from the IDL so your Rust/Go function
signature looks natural while the wire contract stays fixed. The
slot read/write (1) is the universal contract — `@host` functions use
the same slot glue, minus the cross-memory copy.

The trampoline does a **recursive lift/lower** (a scoped subset of the
WASI Component Model canonical ABI): it copies (lowers) the CEL argument
values into the foreign module's memory (allocating there via the
module's exported `celfn_realloc`), calls the export, then copies
(lifts) the result back out. Supported types: scalars, `string`,
`bytes`, `list<T>`, `map<K,V>`, nested aggregates, `Duration`,
`Timestamp`. **Proto messages, `type`, and `optional` are (initially)
rejected at the foreign boundary** — a `msg_slot` handle is meaningless
in another module's memory. A future path lifts the proto restriction
by passing the message **serialized to bytes** instead of by handle; see
§8.5. The wire also carries a **status channel** so a guest failure
becomes a CEL error rather than a wrong value — see §8.6.

### 8.3 WASI vs plain — which toolchain target?

Foreign modules differ in whether they pull in WASI and whether they
own/initialize their memory:

| Toolchain target | Memory | Init call | Notes |
|---|---|---|---|
| **Plain** `wasm32-unknown-unknown` (Rust `no_std`), `--target=wasm32 -nostdlib` (C) | defines its own, no WASI | none | smallest; closest to hand-WAT; just exports the fn + `celfn_realloc` |
| **WASI reactor** `wasm32-wasip1 -mexec-model=reactor` (C/clang), TinyGo `-target=wasip1 -buildmode=c-shared` | defines its own | **must call `_initialize`** | full libc available; the host calls `_initialize` once after instantiation before any export |
| Stock Go (`GOOS=wasip1`), Rust `wasm32-wasi` | defines + WASI imports | yes | heavier runtime; full WASI preview1 stdlib |

The engine negotiates this at `AddModule` time: it instantiates the
foreign module in the same store, calls `_initialize` if the module is a
WASI reactor, and binds its exports under the alias. The **plain**
targets are the lightest and the recommended default for a pure
compute function; choose **WASI** when the function genuinely needs libc
or stdlib facilities.

> **Empirically confirmed (probe — `foreign-go-bindgen-findings.md`).**
> A stock-Go module (`GOOS=wasip1`) is a reactor: `_initialize` is
> **mandatory** (skipping it traps), and it imports a real
> `wasi_snapshot_preview1` surface (10–17 funcs incl. `fd_write`,
> `random_get`, `clock_time_get`, `fd_prestat_*`) — so the engine must
> wire a **full WASI preview1 context**, not just call `_initialize` and
> stub the imports. **TinyGo carries the scalar/string path only** (118
> KB, 2 WASI imports) — it **cannot** carry the §8.5 proto path: TinyGo's
> incomplete reflection traps in `proto.Unmarshal` at runtime.

> A second, deferred model — reusable *separately-instantiated* library
> modules sharing the runtime's memory via `__memory_base` relocation —
> is prototyped (`modules-and-ffi.md` §4.5) but not the v1 path.

### 8.4 Worked example: a foreign function in Go (`GOOS=wasip1`)

Say you want an authorization predicate `rules.allow(subject, action)`
implemented in Go and reused across many CEL expressions.

**1. Declare it** in your `.celfn` (the alias `rules` is the Go module):

```celfn
/// True if `subject` may perform `action`, per the Go policy module.
bool rules.allow(string subject, string action);
```

**2. Write the Go function.** You write a *natural* Go function; the
`celfnc`-generated glue (`rules_celfn.go`) handles the canonical-ABI
marshalling and the wasm export, so you don't hand-write pointer math:

```go
// rules.go
package main

//celfn:export rules.allow
func Allow(subject, action string) bool {
    return subject == "admin" || action == "read"
}

func main() {} // required; a reactor module has no real entry point
```

Under the hood the generated glue produces the fixed-ABI exports the
host trampoline calls — one per function, named by the **overload id**
(`fn_name` + each arg's type token, joined by `_`; the `rules` alias is
the CEL-side / `AddModule` key, *not* part of the export name), plus the
allocator the host uses to place arguments in *this* module's memory:

```go
//go:wasmexport allow_string_string   // export name == overload id (verbatim)
func _allow_string_string(subjPtr, subjLen, actPtr, actLen uint32) uint32 {
    if Allow(strFromMem(subjPtr, subjLen), strFromMem(actPtr, actLen)) {
        return 1
    }
    return 0
}

//go:wasmexport celfn_realloc   // host allocates arg bytes in our memory through this
func _celfn_realloc(ptr, oldLen, align, newLen uint32) uint32 { /* … */ }
```

*(Shown value-only for clarity — the real generated export also carries
a `recover()` guard and a status slot so a panic surfaces as a CEL
error, not a trap or a spurious `false`; see §8.6.)*

**3. Build it** to a WASI-reactor wasm module:

```bash
GOOS=wasip1 GOARCH=wasm go build -buildmode=c-shared -o rules.wasm ./rules
# TinyGo — far smaller (118 KB vs 1.6 MB) for a SCALAR/STRING fn, but
# cannot carry the §8.5 proto path (reflection trap); also needs
# -buildmode=c-shared for the //go:wasmexport reactor shape:
#   tinygo build -target=wasip1 -buildmode=c-shared -o rules.wasm ./rules
```

**4. Register + use** — the decl makes the call type-check at compile
time; the bytes are supplied to the Engine at run time:

```cpp
auto b = celwasm::Compiler::NewBuilder();
b.DeclareVariable("subject", celwasm::CelType::String());
b.AddFunction("bool rules.allow(string subject, string action);");
auto compiler = std::move(b).Build();
auto program  = compiler->Compile(R"(rules.allow(subject, "read"))");

auto engine = celwasm::Engine::NewBuilder().Build();
engine->AddModule("rules", ReadFileToBytes("rules.wasm"));   // alias → module bytes

auto instance = engine->Plan(*program);
celwasm::Activation act;
act.Bind("subject", celwasm::Value::String("guest"));
auto v = instance->Eval(act);     // host: _initialize(rules) once at Plan, then
                                  // lowers the two strings into rules' memory,
                                  // calls allow_string_string, lifts the bool → true
```

At `Plan`/`AddModule` the engine instantiates `rules.wasm` in the same
store **with a full WASI preview1 context** (a stock-Go module imports
`fd_write`/`random_get`/`clock_time_get`/`fd_prestat_*`… — these must be
provided, not stubbed), calls `_initialize` (Go wasip1 is a reactor —
mandatory, skipping it traps), and binds its exports under `rules`. Per
call, the host trampoline lowers the CEL `string` args into the Go
module's memory (via `celfn_realloc`), invokes `allow_string_string`, and lifts
the `bool` result back. *(All confirmed by the probe —
`foreign-go-bindgen-findings.md`.)*

### 8.5 Lifting the proto restriction — pass the message as bytes

The initial foreign ABI rejects `proto(...)` because a message lives in
the host's interner, not in the foreign module's memory. The planned way
to lift that is **serialization**: instead of a handle, the host passes
the proto's **binary-serialized bytes** (a `(ptr, len)` byte reference
allocated in the foreign memory via `celfn_realloc`), and the foreign
side's generated glue **deserializes** them into that language's
generated message type. The wire is plain protobuf binary —
language-agnostic — so it works for any foreign language with a protobuf
runtime:

```celfn
/// ⛔ today; ✅ once foreign-proto serialization lands.
bool rules.is_admin(proto(acme.User) u);
```

```go
//celfn:export rules.is_admin
func IsAdmin(u *acmepb.User) bool { return u.GetRole() == "admin" }
// generated glue: var u acmepb.User; proto.Unmarshal(argBytes, &u); → IsAdmin(&u)
// (argBytes = the host-serialized acme.User, copied into our memory)
```

Host side: serialize `u` → bytes → `celfn_realloc` + copy into the
module → pass `(ptr, len)`. A returned proto is symmetric: the foreign
side marshals, the host deserializes against the descriptor. The trade
vs. `@host`/`@native` (which pass a zero-copy `msg_slot` handle) is a
serialize/deserialize **per call** — which is exactly why it's opt-in
for the foreign boundary and not the default. Both sides need the proto
generated from the same `.proto`.

> **Probe-confirmed, with two real costs.** `proto.Unmarshal` does link
> and run inside stock-Go wasip1 wasm (validated end-to-end). But: (1) it
> pulls the **Go protobuf runtime into the module — ~+4.7 MB** (a 1.6 MB
> string module → 6.4 MB) and ~7 extra WASI imports; (2) it requires
> **stock Go — NOT TinyGo** (TinyGo's incomplete reflection traps in
> `proto.Unmarshal` at `reflect.NewAt`). So a proto-bearing foreign
> module is stock-Go, multi-MB, opt-in. See
> `foreign-go-bindgen-findings.md`.

> **Status of §8.4/§8.5:** the entire foreign backend (trampoline,
> `celfnc` shim generator, `AddModule` wiring, and this serialization
> path) is **designed, not implemented** (`modules-and-ffi.md` §5). The
> shapes above are now **probe-validated** (Go 1.24, wasmtime) — see
> `foreign-go-bindgen-findings.md` for the working experiment.

### 8.6 When a foreign function fails — panics, traps, and the error channel

A foreign function can fail in ways a `@host`/`@native` function can't:
it runs untrusted guest code in its own memory, and a Go `panic` (or a
runtime fault — nil deref, index out of range) **unwinds to a wasm
`unreachable`**, surfacing to the host as a trap, not a return. The
design handles this so a failure becomes a CEL **error value**, never a
crash and never a wrong answer:

- **A Go panic is a wasm trap (`TrapCode.UNREACHABLE`), not a WASI
  `proc_exit`.** The host trampoline catches it as an ordinary
  `wasmtime::Trap` and maps it to a CEL error (`kError`) for that eval —
  the embedding process does **not** crash, and the trap unwinds cleanly
  back to the caller. *(Probe-confirmed for explicit `panic()`, nil
  deref, and index-OOB — `foreign-go-bindgen-findings.md`.)*

- **The generated shim wraps your function in `recover()`**, so the
  common case never even traps:

  ```go
  //go:wasmexport allow_string_string   // export name == overload id (verbatim)
  func _allow_string_string(...) (status uint32) {   // status: 0 = ok, 1 = foreign error
      defer func() {
          if recover() != nil { status = 1 }   // panic → typed error, no trap
      }()
      // … lift args, call your Allow(...), lower the result …
      return
  }
  ```

  `celfnc` emits this guard around every user call. It catches **both**
  explicit `panic()` and runtime panics, turning them into a clean
  `status = 1` return rather than a trap — so the instance is never left
  in the ambiguous post-abort state.

- **The fixed ABI carries a `status` slot alongside the value**, so a
  foreign error is **distinguishable from a legitimate `false` / `0` /
  empty result**. This matters: a predicate `bool rules.allow(...)` that
  *fails* must not masquerade as `allow → false`. On `status != 0` the
  trampoline writes a CEL error; your expression then sees an error
  (which propagates per CEL's error semantics), not a spurious `false`.
  *(This is an ABI addition over the bare value-return — `modules-and-ffi.md`
  §5.3; surfaced by the panic probe.)*

- **Instance recovery policy.** On an *uncaught* foreign trap (one the
  shim's `recover()` didn't catch), the engine writes `kError` and
  **re-instantiates the foreign module** before the next eval (a fresh
  `_initialize` — cheap). Go's `fatalpanic` is nominally fatal, so the
  engine does not trust reuse of a module that has aborted, even though
  reuse *appeared* safe in the probe. The `recover()` shim makes uncaught
  traps rare regardless.

The takeaway for an embedder: **write your Go function as if a panic is
caught and reported as a CEL error.** Don't swallow bad input to `false`
— if your function genuinely can't produce a value (e.g. a malformed
serialized proto in the §8.5 path), `panic` (or return the error path)
and let it surface as a CEL error, rather than returning a plausible
wrong answer.

---

## 9. Command-line tool (`cel`)

For one-shot compile / check / eval without writing C++, use the `cel`
CLI (`tools/cel/`, built via
`bazel build //tools/cel:cel`). Three subcommands ship today:

| Subcommand | What it does | Phase |
|---|---|---|
| `cel check <expr>` | parse + type-check; print `OK` or the error | compile only |
| `cel compile <expr>` | compile to wasm bytes (`--output PATH`, else stdout) | compile only |
| `cel eval <expr>` | **compile *and* evaluate** in one shot; print the result | compile + run |

```bash
cel eval    "1 + 2 + 3"                              # → 6   (compile + evaluate)
cel eval    "a * b" --var "a:int=6" --var "b:int=7"  # → 42
cel eval    'd > duration("1s")' --var 'd:duration="2s"'
cel check   "u.name" --proto user.proto --var "u:acme.User"   # parse + type-check → OK
cel compile "a * b + 1" --var "a:int" --var "b:int" --output expr.wasm  # emit wasm bytes
```

Note the split: `eval` is the *whole* pipeline (it compiles the
expression in-process, then runs it — `cel.cc:RunEval`), while `compile`
stops at wasm bytes. **There is no subcommand today that evaluates an
*already-compiled* `.wasm`** — so `compile` currently produces an
artifact the CLI itself can't consume back.

> **Missing: evaluate a precompiled program (`cel run`) ⛔.** The
> portable `Program` (§3.3) is meant to be compiled once and run many
> times — possibly in another process — but the CLI has no
> `cel run expr.wasm --var …` to close that loop. The planned command
> instantiates the wasm under an `Engine`, reads the variable schema from
> the program's `cel.abi` section (so it binds `--var` without
> re-declaring types), and evaluates:
> ```bash
> cel compile "a * b + 1" --var "a:int" --var "b:int" --output expr.wasm  # ✅ today
> cel run     expr.wasm   --var "a:int=6"  --var "b:int=7"                 # ⛔ planned → 43
> ```
> Until it lands, use `cel eval` (which recompiles each time) or the C++
> `Engine::Plan(Program)` path (§4) to run a precompiled program.

Flags: `--var name:Type[=value]` (typed binding — the literal parser is
type-directed), `--proto <file>` / `--descriptor_set <file>` (schema for
message-typed vars), `--container`, `--O <0..3>` (optimize level),
`--mem_size_bytes`, `--output` (compile target; stdout if omitted),
`--format textproto|json|cel` (`eval` result rendering). Exit codes: `0`
ok, `1` compile/eval failure, `2` usage; diagnostics on stderr, the
`eval` result body on stdout.

> **Missing: `.celfn` IDL input (`--celfn`) 🟡.** The CLI today
> compiles/evaluates standalone expressions only — there is no way to
> point it at a custom-function library. The planned `--celfn <file>`
> flag has the CLI read the file and feed `ParseCelfnSource` →
> `AddLibrary` (file reading is the CLI's job, not the library's — §5.1);
> until then, use the C++ API (§5.1) for custom functions.

### 9.1 Does evaluation need the `.celfn` IDL? (the compile/run split)

**No — the `.celfn` IDL is a *compile-time-only* input.** It is consumed
by the `Compiler` to type-check call sites and, for `@native` functions,
to lower their bodies; none of it is needed to *run* a compiled
`Program`. What a precompiled `.wasm` needs at run time depends on the
backend of the functions it calls:

| Function backend | Needed at run time (eval) | `.celfn` IDL needed at run time? |
|---|---|---|
| **`@native`** (CEL-defined) ⛔ | nothing — the body is *intended* to be compiled *into* the wasm (single-module); body lowering is unimplemented today (§7), so a `@native`-using program does not evaluate yet | **No** (by design — would be fully self-contained) |
| **Foreign** (`<alias>`) | the foreign module's **bytes**, supplied under the alias (`Engine::AddModule` / a planned `--module alias=path.wasm`) | **No** — the call is already lowered to a trampoline keyed by alias + overload id; you supply *bytes*, not the IDL |
| **`@host`** | a **C++ impl** registered via `Engine::AddFunction` | **No, but** — the IDL only declares the *signature*; the *behavior* is C++ the generic CLI can't supply, so a wasm with host imports isn't runnable by stock `cel` at all |

So for the common non-host cases the answer is clean: a `@native`-heavy
expression compiles to a self-contained `.wasm` that `cel run` (when it
lands) or `Engine::Plan` evaluates with **no IDL and no extra modules**;
a foreign-using expression additionally needs the foreign module bytes,
but still not the IDL. The variable schema needed to bind `--var` travels
in the program's `cel.abi` section, so the run side is self-describing
for variables too (§3.3).

> **Target CLI design.** The planned surface — `run`, `inspect`,
> `--celfn`, `--module`, `--activation`, and `celfn gen --lang <…>` — is
> specified in `doc/implementation-plan/rewrite/cel-cli-design.md`
> (design-only). The high-value first slice is `run` + `inspect` (pure
> run-time, no compiler changes), which closes the compile-once /
> run-many loop.

---

## 10. Implementation status at a glance

| Capability | Status |
|---|---|
| Compile scalars / strings / arithmetic / comprehensions / proto reads | ✅ |
| `Engine` / `Plan` / `Instance` / `Eval` / `Activation` / `Value` | ✅ |
| `PartialEval` with unknown patterns | ✅ |
| **Host fns** — scalar + string/bytes args, scalar/bool return | ✅ (typed `AddTypedFunction` / `HostCallContext`) |
| **Host fns** — proto / list / map args, aggregate / new-string returns | ✅ (m21) |
| Typed `AddTypedFunction` + `HostCallContext` adapter | ✅ (m21); raw 4-arg `HostCallback` removed |
| **CEL-defined fns** (`@native`) — parse + type-check (call sites compile) | ✅ |
| **CEL-defined fns** (`@native`) — body lowering + eval (scalar/string/any return) | ⛔ `CompileLibraryBodies` is an unimplemented header stub — no `.cc`, no BUILD target, no caller; never registered in `Plan`. Does not evaluate (§7) |
| **CEL-defined fns** (`@native`) — list/map params/returns | ⛔ blocked on the body-lowering producer above (the host-side marshalling those would reuse is now shipped — see §6) |
| **Foreign fns** (Rust/Go/C, fixed C ABI + shims, WASI/plain) | ⛔ designed (`modules-and-ffi.md` §5), not implemented |
| `cel` CLI — `eval` / `check` / `compile` standalone expressions | ✅ |
| `cel run <file.wasm>` — evaluate a *precompiled* program (no recompile) | ⛔ no subcommand today; `eval` recompiles each time (§9) |
| `.celfn` IDL accepted as a whole-file string (`ParseCelfnSource`); caller does the file read | ✅ |
| `.celfn` grammar v2 (`@native`, prefix-module, drop `Module`) + doc-comment capture | ✅ shipped (`m13-custom-fns.md` §3.0) |
| Doc-comment → `cel.abi` (cross-process introspection) / `--celfn` CLI flag | 🟡 description on `CelfnDecl` only; ABI carriage + CLI flag pending |

---

## 11. Where to look next

- **API headers** (the source of truth for signatures):
  `compiler/{compiler,program}.h`, `eval/{engine,instance,activation,value}.h`,
  `shared/type.h`.
- **`.celfn` IDL + types:** `compiler/celfn/function_library.h`.
- **Custom-fn design + status tracker:**
  `doc/implementation-plan/rewrite/m13-custom-fns.md` (§0.5 current
  state, §14 testing strategy).
- **Memory model:** `doc/implementation-plan/rewrite/memory-layout-design.md`.
- **Modules + FFI (foreign backend):**
  `doc/implementation-plan/rewrite/modules-and-ffi.md`.
- **CLI:** `tools/cel/` (compile/eval from the command line).
