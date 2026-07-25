# CEL → WebAssembly — User Guide

This is the embedder's guide to the CEL-to-WebAssembly AOT compiler: how
to compile a CEL expression to a wasm module, evaluate it, bind
variables, and extend the language with custom functions (host-backed
C++, or sandboxed WebAssembly plugins).

Every code snippet uses the real public API (`compiler/compiler.h`,
`compiler/program.h`, `eval/engine.h`, `eval/instance.h`,
`eval/activation.h`, `eval/value.h`, `shared/type.h`). Where
a surface is declared but not yet fully wired, it is called out with a
**Status** line — this guide documents the *target* API and is explicit
about what evaluates today vs. what is planned.

> **Implementation-status legend.** Throughout: ✅ = shipped + tested;
> 🟡 = surface declared, behavior partial/aspirational; ⛔ = designed,
> not yet implemented. A consolidated status table is in §10.

> **Detailed guides.** This index is the compile/run API. Deep dives
> live on their own pages:
> - [Custom functions — the three backends](custom-functions.md)
> - [Writing host functions](writing-host-functions.md)
> - [Writing plugins](writing-plugins.md)

---

## 1. Mental model: two phases, one serialization boundary

The system splits cleanly into a **compile-time** half and a
**runtime** half, joined by a `Program` (portable wasm bytes):

```
        compile time                                 run time
  ┌────────────────────────────┐             ┌────────────────────────────┐
  │ celwasm::Compiler          │             │ celwasm::Engine            │
  │   .Compile("source")       │ ──Program──►│   .Plan(program)           │
  │   → celwasm::Program       │ (wasm bytes │   → celwasm::Instance      │
  │   (no wasmtime dep)        │  + cel.abi) │     .Eval(activation)      │
  └────────────────────────────┘             │     → celwasm::Value       │
                                             └────────────────────────────┘
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

> **Not** thread-safe: `Engine::AddFunction` / `AddPlugin` (custom-fn
> registration — see [custom functions](custom-functions.md)). Configure those once at startup, *then*
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
| `Engine` | one per process | `Plan` concurrent-safe; `AddFunction`/`AddPlugin` single-thread setup |
| `Instance` | one per worker thread | thread-owned; outlives the Engine handle (shared_ptr) |
| `Activation` | per-eval, reusable | not shared across threads |

---

## 5. Custom functions

CEL is extended with your own functions through a small `.celfn` IDL
with three backends: `@host` (trusted C++ in your process),
`@plugin` (sandboxed WebAssembly, hot-swappable), and `@native`
(reserved, unimplemented). The full overview — declarations,
libraries, registration, and the backend comparison — is on its own
page:

- **[Custom functions — the three backends](custom-functions.md)**
- [Writing host functions](writing-host-functions.md) — worked examples
- [Writing plugins](writing-plugins.md) — build a sandboxed plugin

---

---

## 9. Command-line tool (`cel`)

For one-shot compile / check / eval without writing C++, use the `cel`
CLI (`tools/cel/`, built via
`bazel build //tools/cel:cel`). Four subcommands ship today:

| Subcommand | What it does | Phase |
|---|---|---|
| `cel check <expr>` | parse + type-check; print `OK` or the error | compile only |
| `cel compile <expr>` | compile to wasm bytes (`--output PATH`, else stdout) | compile only |
| `cel eval <expr>` | **compile *and* evaluate** in one shot; print the result | compile + run |
| `cel generate` | emit plugin-function bindings (`fns.wit`, `codec.h`, `generated_stub.cc`, `user_fns.h`) from a `.idl` file — the front half of the `cel_wasm_plugin` macro ([plugin functions](writing-plugins.md)) | codegen only |

```bash
cel eval    "1 + 2 + 3"                              # → 6   (compile + evaluate)
cel eval    "a * b" --var "a:int=6" --var "b:int=7"  # → 42
cel eval    'd > duration("1s")' --var 'd:duration="2s"'
cel check   "u.name" --proto user.proto --var "u:acme.User"   # parse + type-check → OK
cel compile "a * b + 1" --var "a:int" --var "b:int" --output expr.wasm  # emit wasm bytes
cel generate --idl fns.idl --out_dir gen/            # emit plugin-fn bindings
```

`generate` takes no positional `<expr>` — its input is `--idl`; flags:
`--out_dir` (required), `--language` (`cpp` today; `go` planned),
`--include` (extra `#include`s for the generated sources). The WIT
package name is always derived from the IDL's `Module` directive
(`cel:<module>`, fallback `cel:customfn`) — there is no override.
Normally you don't run it by hand — the `cel_wasm_plugin` Bazel macro
drives it (and finishes by running `cel embed-decls`, which stamps the
verbatim `.idl` text into the plugin as its `cel.fns` custom section).

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
> `AddLibrary` (file reading is the CLI's job, not the library's);
> until then, use the [C++ API](custom-functions.md) for custom functions.

### 9.1 Does evaluation need the `.celfn` IDL? (the compile/run split)

**No — the `.celfn` IDL is a *compile-time-only* input.** It is consumed
by the `Compiler` to type-check call sites; none of it is needed to
*run* a compiled `Program`. What a precompiled `.wasm` needs at run time depends on the
backend of the functions it calls:

| Function backend | Needed at run time (eval) | `.celfn` IDL needed at run time? |
|---|---|---|
| **`@native`** (CEL-defined) ⛔ | n/a — the backend is unimplemented ([details](custom-functions.md#3-cel-defined-functions-native)); a program that calls one does not evaluate | **No** |
| **`@plugin`** ⛔ | the plugin's **bytes**, plus the `FunctionLibrary` so the engine knows which decls to bind (`Engine::AddPlugin` / a planned `--component path.wasm`) | **Partially** — `AddPlugin` takes the library so it can bind every `@plugin` decl to a matching export; you supply both the *bytes* and the parsed IDL |
| **`@host`** | a **C++ impl** registered via `Engine::AddFunction` | **No, but** — the IDL only declares the *signature*; the *behavior* is C++ the generic CLI can't supply, so a wasm with host imports isn't runnable by stock `cel` at all |

So the answer is clean: a pure-CEL expression compiles to a
self-contained `.wasm` that `Engine::Plan` evaluates with **no IDL and
no extra modules**;
a `@plugin`-using expression additionally needs the plugin bytes
**and the library** (so the engine knows which `@plugin` decls to bind
to which exports). The variable schema needed to bind `--var` travels in
the program's `cel.abi` section, so the run side is self-describing for
variables too (§3.3).

> **Target CLI design.** The planned surface — `run`, `inspect`,
> `--celfn`, `--component`, `--activation`, and `celfn gen --lang <…>` — is
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
| **CEL-defined fns** (`@native`) — body lowering + eval | ⛔ not implemented ([details](custom-functions.md#3-cel-defined-functions-native)); a `@native`-using program does not evaluate |
| **Plugin fns** (`@plugin`, C++ via the `cel_wasm_plugin` Bazel macro) | ✅ scalar / int / bool round-trips; plugin built end-to-end and dispatched via `Engine::AddPlugin`; proto args/returns via the manual-tagged `demo_plugin_proto` fixture; plugin-side string *returns* currently blocked by a libc++ trap (see the skipped `GreetRoundTripsString`) |
| **Plugin fns** — Go authoring (TinyGo wasip2) | ⛔ designed; `cel generate --language=go` arm pending |
| `cel` CLI — `eval` / `check` / `compile` standalone expressions | ✅ |
| `cel run <file.wasm>` — evaluate a *precompiled* program (no recompile) | ⛔ no subcommand today; `eval` recompiles each time (§9) |
| `.celfn` IDL accepted as a whole-file string (`ParseCelfnSource`); caller does the file read | ✅ |
| `.celfn` grammar v2 (`@native`, prefix-module) + doc-comment capture; the `Module foo;` directive remains current (it names `@native` wasm modules and seeds the WIT package name for `@plugin` builds) | ✅ shipped (`m13-custom-fns.md` §3.0) |
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
- **Modules + FFI (`@plugin` backend):**
  `doc/implementation-plan/rewrite/modules-and-ffi.md`.
- **CLI:** `tools/cel/` (compile/eval from the command line).
