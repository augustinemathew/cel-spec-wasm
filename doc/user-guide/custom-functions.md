# Custom functions

A CEL expression sees only what the host hands it. Custom functions are
the escape hatch: you register functions, and rule authors call them
like built-ins.

Custom functions run as **native host callbacks** — C++ lambdas
registered on the `Engine`, executing in the embedder's process with the
embedder's privileges. Sandboxed wasm plugins are not offered: the
expression itself is sandboxed, but a custom function is your code,
running unsandboxed in your address space, and you must trust it
accordingly. The precise trust boundaries are in the
[security model](security-model.md).

The shortest complete example — declare a function at compile time,
implement it at eval time, call it from CEL:

```cpp
// Compile side: declare the function so call sites type-check.
auto b = celwasm::Compiler::NewBuilder();
b.AddFunction("int @host.double_it(int x);");
b.DeclareVariable("x", celwasm::CelType::Int());
auto compiler = std::move(b).Build().value();
auto program  = compiler.Compile("double_it(x)").value();

// Eval side: register the implementation by its overload id.
auto engine = celwasm::Engine::NewBuilder().Build().value();
CHECK_OK(engine.AddTypedFunction("double_it_int",
    [](int64_t x) -> absl::StatusOr<int64_t> { return x * 2; }));

auto instance = engine.Plan(program).value();
celwasm::Activation act;
act.Bind("x", celwasm::Value::Int(21));
auto v = instance.Eval(act);            // → 42
```

The full how-to — the typed API, `HostCallContext`, proto / list / map
arguments, errors and unknowns — is
[Writing host functions](writing-host-functions.md). This page covers
the declaration side: the `.celfn` IDL and how declarations reach the
compiler and the engine.

---

## 1. The `.celfn` IDL — declarations first

Custom functions are declared in a small IDL. Every declaration carries
the `@host.` backend prefix (there is no unprefixed form, and no other
backend):

```celfn
int @host.length(string s);
```

The type grammar (`compiler/celfn/function_library.h`):
`bool int uint double string bytes null Duration Timestamp`, `list<T>`,
`map<K,V>` (K ∈ bool/int/uint/string), `proto(<fqn>)`, and a leading
`this` on the first param for method-style dispatch (`x.is_admin()`).

Register declarations on the `Compiler` so call sites type-check:

```cpp
auto b = celwasm::Compiler::NewBuilder();
b.AddFunction("int @host.length(string s);");       // one decl from a string
b.DeclareFunctions(*celwasm::ParseCelfnSource(celfn_text));   // a whole .celfn file/library (StatusOr — check in real code)
auto compiler = std::move(b).Build();
```

`AddFunction(celfn_source)` parses one (or more) decl from a string;
`DeclareFunctions(FunctionLibrary)` registers a parsed `.celfn` library
(from `celwasm::ParseCelfnSource(text)` or `FunctionLibrary::Builder`).
A call to an unregistered function fails at compile time with
`"undeclared reference to '<fn>'"`.

### 1.1 Building a reusable expression library (`.celfn` files)

The IDL's point is a **named, reusable library** of function
declarations — a project "standard library":

```celfn
// policy.celfn  — a library of policy function declarations

bool @host.is_adult(proto(acme.User) u) ;
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

At eval time, bind each declaration to its C++ impl on the `Engine`
(§2). Comments (`//` and `/* … */`) are permitted in `.celfn` source
and skipped by the parser; they are **not** captured as machine-readable
descriptions — `CelfnDecl` carries no description field.

**Introspection.** Walk the registered functions:

```cpp
for (const celwasm::FunctionLibrary& lib : compiler->function_libraries()) {
  for (const celwasm::CelfnDecl& d : lib.decls()) {
    // d.fn_name, d.overload_id, d.params, d.return_type
  }
}
```

---

## 2. Implementing a function — the three registration surfaces

A custom function is implemented by your C++ at runtime. The expression
imports it; you register the impl on the `Engine`. It runs in your
address space with your privileges — the sandbox does nothing for you
here, so register only code you trust (an in-memory cache lookup, a
call into a library you already ship), and validate its inputs as you
would any externally reachable surface.

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

---

## 3. Verification at Plan time

A compiled `Program` records every custom function it calls — name plus
full signature — in its `cel.abi` section (`required_functions`). At
`Engine::Plan`, each one is verified against the engine's registry: a
missing registration, or one whose signature differs from what the
program was compiled against, is a clean `FailedPrecondition` naming
the function, before any traffic. Because the implementations are C++
in the embedder's process, a program that requires custom functions can
only run through the C++ API; the generic `cel` CLI refuses such a
program up front and lists the required signatures
([CLI reference](index.md#9-command-line-tool-cel)).
