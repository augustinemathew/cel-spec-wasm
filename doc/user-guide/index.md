# CEL → WebAssembly — User Guide

This is the embedder's guide to the CEL-to-WebAssembly AOT compiler: how
to compile a CEL expression to a wasm module, evaluate it, bind
variables, and extend the language with custom functions (native C++
callbacks in the embedder's process).

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
> - [Custom functions](custom-functions.md) — declarations + the `.celfn` IDL
> - [Writing host functions](writing-host-functions.md) — the typed callback API

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

> **Not** thread-safe: the registration family — `BindFunction` /
> `AddFunction` / `AddTypedFunction` (custom-fn registration — see
> [custom functions](custom-functions.md)). Configure those once at
> startup, *then* `Plan` from many threads.

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

Bind a value **lazily** when producing it is expensive and you'd rather
not pay unless the program actually declares the variable — a fetch, a
decode, a database read:

```cpp
act.BindLazy("profile", [&]() -> absl::StatusOr<celwasm::Value> {
  return LoadProfile(user_id);          // runs only if the program declares `profile`
});
```

The callback runs **at most once per `Eval`**, and only for a variable
the compiled program declares (and that no unknown pattern blanks during
a `PartialEval`). Its result is memoized for that evaluation; the next
`Eval` calls it again. If it returns a non-OK status, evaluation stops
and you get that status back from `Eval` unchanged.

One caveat worth being precise about: the binder fires when the program
*declares* the variable, not when the expression first *reads* it.
Variable slots are written into linear memory before the expression
runs, so `BindLazy` saves the cost of a variable the program never
declared — not one it declares but never reaches.

`Activation` is **move-only** (a binder is not copyable) and not
thread-safe; use one per evaluation.

`Value` is the host-side counterpart to the 24-byte wire value. Build
with named factories, inspect with `StatusOr<T> AsX()`:

```cpp
celwasm::Value::Int(42);                 celwasm::Value::String("hi");
celwasm::Value::Bool(true);              celwasm::Value::Bytes(std::string{...});
celwasm::Value::Double(3.14);            celwasm::Value::Duration(absl::Seconds(5));
celwasm::Value::Message(my_proto);       celwasm::Value::List({Value::Int(1), Value::Int(2)});
celwasm::Value::Map({{Value::String("k"), Value::Int(1)}});

auto i = v.AsInt();        // StatusOr<int64_t>   (InvalidArgument on kind mismatch)
auto s = v.AsString();     // StatusOr<string_view>  — borrows from `v`
bool null   = v.IsNull();
bool unk    = v.IsUnknown();
bool err    = v.IsError();
```

### 4.6 Lifetime & concurrency summary

| Object | Ownership | Concurrency |
|---|---|---|
| `Compiler` | value, copyable | immutable after Build; share freely |
| `Program` | value, copyable | immutable; share/serialize freely |
| `Engine` | one per process | `Plan` concurrent-safe; `BindFunction`/`AddFunction`/`AddTypedFunction` single-thread setup |
| `Instance` | one per worker thread | thread-owned; outlives the Engine handle (shared_ptr) |
| `Activation` | per-eval, reusable | not shared across threads |

`AsString`, `AsBytes`, `AsType` and `UnknownAttributes` **borrow** from
the `Value` you call them on — the returned `string_view` / `Span` is
valid only while that `Value` is alive. Calling one on a temporary and
keeping the result reads freed memory, and because the bytes often
survive unclobbered it tends to produce a plausible wrong answer rather
than a crash:

```cpp
auto got = *Eval(...).AsBytes();               // WRONG — view dangles
auto v = Eval(...); auto got = *v.AsBytes();   // right — `v` outlives the view
```

The scalar accessors (`AsInt`, `AsBool`, `AsDouble`, …) return by value
and carry no such constraint.

---

## 5. Custom functions

CEL is extended with your own functions through a small `.celfn` IDL.
A function is declared with the `@host.` prefix on the `Compiler` (so
call sites type-check) and implemented as a C++ callback registered on
the `Engine` (`BindFunction` / `AddTypedFunction` / `AddFunction`);
`Plan` verifies every function the program calls is registered with an
exactly matching signature before anything runs. The callbacks are
native code in your process — the wasm sandbox covers the expression,
not the functions you register, and sandboxed wasm plugins are not
offered ([security model](security-model.md)). The full overview —
declarations, libraries, registration — is on its own page:

- **[Custom functions](custom-functions.md)** — the `.celfn` IDL + registration
- [Writing host functions](writing-host-functions.md) — worked examples

---

## 9. Command-line tool (`cel`)

For one-shot compile / check / eval without writing C++, use the `cel`
CLI (`tools/cel/`, built via
`bazel build //tools/cel:cel`). Five subcommands ship today:

| Subcommand | What it does | Phase |
|---|---|---|
| `cel check <expr>` | parse + type-check; print `OK` or the error | compile only |
| `cel compile <expr>` | compile to wasm bytes (`--output PATH`, else stdout) | compile only |
| `cel eval <expr>` | **compile *and* evaluate** in one shot; print the result | compile + run |
| `cel run <prog.wasm>` | evaluate a **precompiled** program — no recompile | run only |
| `cel inspect <prog.wasm>` | print what a program declares (vars, link mode, ABI versions) | neither |

```bash
cel eval    "1 + 2 + 3"                              # → 6   (compile + evaluate)
cel eval    "a * b" --var "a:int=6" --var "b:int=7"  # → 42
cel eval    'd > duration("1s")' --var 'd:duration="2s"'
cel check   "u.name" --proto user.proto --var "u:acme.User"   # parse + type-check → OK
cel compile "a * b + 1" --var "a:int" --var "b:int" --output expr.wasm  # emit wasm bytes
```

Note the split: `eval` is the *whole* pipeline (it compiles the
expression in-process, then runs it), while `compile` stops at wasm
bytes and `run` picks them back up:

```bash
cel compile "a * b + 1" --var "a:int" --var "b:int" --output expr.wasm
cel inspect expr.wasm                        # vars, required fns, link mode
cel run     expr.wasm --var "a=6" --var "b=7"    # → 43
```

On `run`, `--var` supplies **values only** — each variable's full
declared type travels with the program in its `cel.abi` section, so
declarations are never repeated, and aggregates and messages bind the
same way as scalars. `inspect` prints those types in the `--var`
grammar, so a line of its output pastes straight into a binding:
`xs:list<int>`, `m:map<string,int>`, `r:acme.Request`.

**Custom functions and the CLI.** A program that calls custom
(`@host`) functions cannot be run by the stock CLI — the
implementations are C++ callbacks in the embedder's process, so no
generic binary can supply them. The program records its own
requirements in `cel.abi` (`required_functions`), so `cel run` refuses
such a program up front and names the required signatures, and
`cel inspect` shows them ahead of time (`host fns:`). Evaluate such
programs through the [C++ API](custom-functions.md) instead. The CLI
also has no flag to *declare* custom functions at compile time — it
compiles standalone expressions only.

Flags: `--var name:Type[=value]` (typed binding — the literal parser is
type-directed), `--proto <file>` / `--descriptor_set <file>` (schema for
message-typed vars), `--container`, `--O <0..3>` (optimize level),
`--output` (compile target; stdout if omitted),
`--format textproto|json|cel` (`eval` result rendering).

Message-typed variables take a textproto or JSON payload, inline or from
a file (the extension picks the codec):

```bash
cel eval 'r.user' --proto req.proto \
  --var 'r:acme.Request=txtpb:user:"alice" quantity:3'      # inline textproto
cel eval 'r.quantity * 2' --proto req.proto \
  --var 'r:acme.Request=json:{"user":"bob","quantity":21}'  # inline JSON
cel eval 'r.user' --proto req.proto \
  --var 'r:acme.Request=@/tmp/req.json'                     # @file (.json/.txtpb/.pb)
```

**Exit codes.** `0` success · `1` the expression or program failed ·
`2` usage error. Diagnostics go to stderr; only a successful result body
goes to stdout, so the CLI is safe to branch on:

```bash
if result=$(cel eval "$expr" --var "n:int=$n"); then
  echo "ok: $result"
else
  echo "failed with $?" >&2      # 1 = CEL said no, 2 = bad invocation
fi
```

A CEL error (`1/0`, a missing key, overflow) is a legitimate *value* in
the C++ API — catchable with `||` or `?:` — but at the process boundary
it means no result was produced, so `cel` reports it on stderr and exits
`1` rather than printing it as if it were an answer.

### 9.1 Does evaluation need the `.celfn` IDL? (the compile/run split)

**No — the `.celfn` IDL is a *compile-time-only* input.** It is consumed
by the `Compiler` to type-check call sites; none of it is needed to
*run* a compiled `Program`. A pure-CEL expression compiles to a
self-contained `.wasm` that `Engine::Plan` evaluates with **no IDL and
no extra modules**. An expression that calls custom functions needs
their **C++ implementations** registered on the engine at run time
(`Engine::BindFunction` / `AddFunction`) — the IDL only declares the
*signature*; the *behavior* is C++ the generic CLI can't supply, which
is why `cel run` refuses such a program. The program's own `cel.abi`
records every custom function it calls (name + full signature), which
`Plan` verifies against the registry, and the variable schema needed to
bind `--var` travels in the same `cel.abi` section — the run side is
self-describing (§3.3).

---

## 10. Implementation status at a glance

| Capability | Status |
|---|---|
| Compile scalars / strings / arithmetic / comprehensions / proto reads | ✅ |
| `Engine` / `Plan` / `Instance` / `Eval` / `Activation` / `Value` | ✅ |
| `PartialEval` with unknown patterns | ✅ |
| **Custom fns** — scalar + string/bytes args, scalar/bool return | ✅ (typed `AddTypedFunction` / `HostCallContext`) |
| **Custom fns** — proto / list / map args, aggregate / new-string returns | ✅ |
| Declaration-first registration (`BindFunction`) + typed `AddTypedFunction` + `HostCallContext` adapter | ✅; raw 4-arg `HostCallback` removed |
| **Plan-time verification** — every custom function the program calls is checked against the registry (existence + full signature) at `Plan` | ✅ |
| `cel` CLI — `eval` / `check` / `compile` standalone expressions | ✅ |
| `cel run <file.wasm>` — evaluate a *precompiled* program (no recompile) | ✅ |
| `cel inspect <file.wasm>` — print a program's declared variables + link mode | ✅ |
| `.celfn` IDL accepted as a whole-file string (`ParseCelfnSource`); caller does the file read | ✅ |
| `--celfn` CLI flag (declare a custom-fn library at CLI compile time) | ⛔ not offered — the CLI compiles standalone expressions only; use the C++ API |
| Sandboxed wasm plugin functions | ⛔ not offered — custom functions are native host callbacks in the embedder's process ([security model](security-model.md)) |

---

## 11. Where to look next

- **API headers** (the source of truth for signatures):
  `compiler/{compiler,program}.h`, `eval/{engine,instance,activation,value}.h`,
  `shared/type.h`.
- **`.celfn` IDL + types:** `compiler/celfn/function_library.h`.
- **Custom-fn how-to:** [Writing host functions](writing-host-functions.md).
- **Memory model:** `doc/implementation-plan/rewrite/memory-layout-design.md`.
- **CLI:** `tools/cel/` (compile/eval from the command line).
