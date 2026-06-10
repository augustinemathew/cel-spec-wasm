# cel-wasm (Common Expression)

> ⚠️ **This repo is not production ready, yet!**
>
> The pipeline runs end-to-end, the conformance + perf numbers below
> are real, and the architectural pieces (compiler, runtime, JIT,
> sandbox, custom-fn macros) all work. But there is *lots of cleanup
> left* before it's something you'd hand-on-heart deploy to a
> production policy engine:
>
> - **Constant list/map literals are rebuilt every Eval** (cel-cpp
>   folds them at plan time), so `x in [<100 elems>]` and map
>   construction still lose 2–44× — the const-aggregate codegen
>   milestone is the planned fix (see
>   [Performance](#performance--vs-cel-cpp-tree-walking-interpreter)).
>   (The old "arithmetic 2.4× slower" gap is **fixed**: configurable
>   static linking shipped, and arithmetic now wins 17–22× at length.)
> - **Language bindings beyond C++ are designed, not built** — see
>   [Language bindings](#language-bindings).
> - **`@native` CEL-defined helper bodies** parse + type-check today
>   but the codegen for the bodies hasn't shipped.
> - **General-purpose hardening** — error-path coverage, fuzz
>   testing, allocator caps, AOT module cache, the whole production
>   hardening checklist.
>
> Watch the [implementation plan](doc/implementation-plan/) for the
> active milestones. **Pick this if you want to help close those
> gaps** — issues, PRs, and benches all welcome.

> **CEL → Ahead-of-time WASM → Sandboxed JIT → Native.**
>
> Take a [CEL](https://github.com/google/cel-spec) expression, compile
> it ahead-of-time to a tiny self-contained `.wasm` module, then have
> [wasmtime](https://wasmtime.dev/)'s
> [Cranelift](https://cranelift.dev/) JIT it to native machine code
> inside a sandbox. A compiled, sandboxed sibling of
> [`cel-cpp`](https://github.com/google/cel-cpp) /
> [`cel-go`](https://github.com/google/cel-go) — same semantics, the
> same artifact runs from any language.

> *Internal C++ namespace is `celwasm::` and Bazel labels live
> under `cel-wasm` for historical reasons; project name is
> `cel-wasm`, matching `cel-cpp` / `cel-go`.*

```
                ┌─ wasmtime sandbox ────────────────────────┐
                │                                           │
  CEL expr ───► │   Cranelift-emitted native code           │ ──► Value
                │     + cel_runtime kernel                  │
                │                                           │
                │     ┌────────────────────────────────┐    │
                │     │ @component custom fn           │    │ ◄── sandboxed
                │     │ (own linear memory, no I/O)    │    │     custom code:
                │     │  • Rust / TinyGo / C author    │    │     can't escape,
                │     │  • hot-swap at runtime         │    │     no syscalls
                │     └────────────────────────────────┘    │
                │                                           │
                └────────────────────────────┬──────────────┘
                                             │ host import
                                             ▼
                ┌───────────────────────────────────────────┐
                │ @host custom fn — your C++ lambda         │ ◄── trusted but
                │ runs in the EMBEDDER's process / memory   │     RISKY for
                │ (this is what stock CEL gives you)        │     untrusted code
                └───────────────────────────────────────────┘
```

CEL today is an *interpreter, re-shipped per host language*: Kubernetes,
Envoy / Istio, IAM, Firebase — each one links its own
`cel-cpp` / `cel-go` / `cel-rust`, and they drift in behaviour and
performance. **cel-wasm compiles once and runs that artifact verbatim
on every host.**

## Why this is awesome (per Claude)

- **AOT, not interpreted — and the wasm itself gets JIT'd to native
  code.** Two compile steps stack: the CEL compiler lowers the
  expression ahead-of-time to a `.wasm` module (via Binaryen, with
  `-O0..3`), and then [wasmtime](https://wasmtime.dev/)'s
  [**Cranelift**](https://cranelift.dev/) JIT translates that wasm to
  native machine code at `Engine::Plan` time (~240-300 µs per Plan).
  By the time `Instance::Eval()` runs, every branch of `$eval` and
  every `cel_runtime` helper is native code — no wasm interpreter,
  no AST walk, no tree dispatch in the hot path. Plan is amortised
  across calls (one Plan → many Evals).
- **Faster than the tree-walking baseline** on realistic policy
  workloads — see the head-to-head below.
- **Sandboxed by construction.** Bounded linear memory, no syscalls,
  no I/O, no recursion. Safe for untrusted policy.
- **One semantic implementation.** The compiler is the only thing that
  knows CEL. Every host runs the same `.wasm`; semantic drift between
  language runtimes goes away.
- **Extend in any language, with a sandbox boundary.** Custom CEL fns
  can be authored as WebAssembly **Component-Model components** —
  declared in a `.celfn` IDL, registered at runtime via
  `Engine::AddComponent`. The component runs in its own linear memory,
  can't reach the embedder's address space, can't syscall, and can be
  hot-swapped at runtime. (You can also write fns as trusted C++
  callbacks when you don't need the sandbox — see below.)

## Performance — vs `cel-cpp` tree-walking interpreter

Measured 2026-06-09 over a 232-cell corpus (every CEL operator family
× data type; `benchmark/eval/corpus/`), same expressions and inputs on
both engines, `-c opt`, Apple Silicon.  cel-wasm in statically-linked
mode (`LinkMode::kStatic`, the default — runtime helpers merged into
the compiled module, so every helper call is intra-module).  Honest
picture, both directions; **corpus-wide geomean is parity (0.95×)**
with a sharply two-sided distribution.

### Workloads cel-wasm wins ✅

Anything with repetition to amortize — loops, chains, length.  The
crossover is ~10 operations:

| Workload | cel-wasm | cel-cpp | speedup |
| --- | ---: | ---: | :---: |
| 100-elem comprehension `.all(x, …)` | 713 ns | 6.6 µs | **9×** |
| 20-elem `.map(x, x * 2)` | 219 ns | 5.4 µs | **25×** |
| 1000-term arithmetic chain | 2.0 µs | 33.4 µs | **17×** |
| 100-term string concat chain | 2.8 µs | 6.1 µs | **2.2×** |
| `x in <1 M ints>` (activation-bound list) | 3.27 ms | 5.68 ms | **1.7×** |
| regex `.matches()` (complex, hot loop) | 186 ns | 8.9 µs | 48× † |

† The `.matches()` row is a runtime-configuration difference, not
codegen: our runtime caches the compiled RE2 pattern per Instance
(`runtime/cel_matches.cc`), so a hot loop pays the regex compile once;
cel-cpp's default runtime rebuilds it per evaluation (its optional
regex-precompilation extension would close most of this).  Quoted
with that caveat, deliberately.

### Workloads cel-wasm currently loses ⛔

| Workload | cel-wasm | cel-cpp | gap | why |
| --- | ---: | ---: | :---: | --- |
| 100-entry map literal construction | 122 µs | 2.8 µs | **44×** | we rebuild constant aggregates every Eval; cel-cpp folds them at plan time (SwissTable) |
| `x in [<100-elem list literal>]` | 2.7 µs | 1.4 µs | 2× | same — per-eval list construction |
| 1000-char string equality | 604 ns | 75 ns | 8× | byte-loop compare in wasm vs native SIMD memcmp |
| single op (`a == b`, `a && b`, …) | 60–140 ns | 45–95 ns | 1.2–1.9× | per-Eval floor: one wasm boundary crossing + arena reset + decode = 62 ns minimum |

The three loss mechanisms, in order of leverage: **(1)
constant-aggregate folding** — the planned const-list/map codegen
milestone converts most of those rows (the `size(list)` cells, where
construction is cheap arena appends, already win up to 3.7×); **(2)
SIMD string scans** in the wasm runtime; **(3) the 62 ns boundary
floor**, irreducible without bypassing wasmtime's call path and only
visible on expressions too small to amortize it.

Full methodology, per-family geomeans, and the cause of every loss
row:
[`doc/implementation-plan/rewrite/m28-bench-results.md`](doc/implementation-plan/rewrite/m28-bench-results.md).
Reproduce with `benchmark/eval/run.sh` (three-way: dynamic / static /
cel-cpp).

> **Known codegen bug.** The 1000-term polynomial above is the
> working ceiling — at ≳ 2000 terms the emitted wasm traps
> with `wasm trap: unaligned atomic` at Eval. The slot allocator
> appears to misalign at the large slot-count regime; tracked as a
> follow-up, not yet root-caused.

## Conformance

| | |
| --- | --- |
| **Passing** | **1899 / 2454** of the upstream CEL conformance corpus (77.4%) |
| Intentional skips | 463 (227 require DYN/dynamic typing — out of scope by design; 144 disable type-checker; 55 unimplemented extensions; 25 check-only; 12 type-env) |
| **Failing** | **92** (string `size()` returning bytes vs code points on multi-byte UTF-8; timestamp Y9999 max-nanos stringification; a handful of map-null-pruning + null-coerce-to-Duration edges) |

Of expressions we *attempt* (excluding intentional skips), the pass
rate is **1899 / 1991 = 95.4%**. Reproduce with
`bazel run //conformance:run_conformance`.

## Extensions

The big CEL extensions ship today — no caveats on these:

| Extension | Conformance |
| --- | --- |
| `string_ext` (incl. **`strings.format`**, `strings.replace`, `charAt`, `indexOf`, …) | 172 / 216 pass, **0 fails** (44 skips are check-disabled, not unimplemented) |
| `math_ext` (`math.greatest`, `math.least`, `math.abs`, bit ops, …) | 194 / 199, 0 fails |
| `network_ext` (`isIPv4`, `isIPv6`, `isURI`, …) | 69 / 69 |
| `optionals` (`optional.of`, `?` chaining, `orValue`, …) | 22 / 70 (most of the rest need DYN) |

## What's not implemented

cel-wasm targets the **static subset** of CEL by design. If you need
any of these, use [`cel-cpp`](https://github.com/google/cel-cpp):

- **Dynamic typing (`dyn`).** Variables and intermediates have static
  types declared up front. `RejectDyn` is the gate; this is the
  load-bearing 227-row skip block.
- **CEL-defined helper functions (`@native.fn = expr`).** The
  type-check side lands today; codegen for the body is unshipped.
- **`cel.@block(...)` AST sub-expression sharing** (`block_ext`,
  37 conformance rows) — a checker-level optimization shape we
  haven't lowered.
- **proto2 extension-field syntax** (`msg.[pkg.ext_name]`,
  `proto2_ext` — 18 conformance rows). Plain proto2 messages work;
  only the extension-field accessor is missing.
- A handful of specific edges (~10 rows): `size('multi-byte string')`
  returning bytes vs Unicode code points, Y9999 timestamp nanos
  stringification, map null-value pruning, null → `Duration` coerce.
- **TinyGo authoring of component functions.** The Go arm of
  `cel generate` is in design (m26 H.4); the C++ author surface ships.

(Language bindings — Go, TypeScript — are not "missing features", just
unwritten thin shims; see [the bindings section](#language-bindings).)

If your workload needs the full CEL surface area — every extension,
every `dyn` case, the macro evaluator, every language binding —
**reach for [`cel-cpp`](https://github.com/google/cel-cpp) or
[`cel-go`](https://github.com/google/cel-go) instead.** cel-wasm trades
that surface for AOT speed, portability, and the sandbox; it's not a
drop-in replacement.

## Quick start

```bash
# One-time: fetch the vendored parser/type-checker.
third_party/fetch_cel_cpp.sh

# Evaluate a CEL expression end-to-end (compile → wasm → wasmtime).
bazel run //tools/cel:cel -- eval '1 + 2 + 3'                          # => 6
bazel run //tools/cel:cel -- eval 'a * b' --var a:int=6 --var b:int=7  # => 42

# Compile to a portable wasm artifact you can ship and re-run.
bazel run //tools/cel:cel -- compile 'a * b + 1' \
    --var a:int --var b:int --output expr.wasm
```

### Embedding from C++

```cpp
#include "compiler/compiler.h"
#include "eval/engine.h"
using namespace celwasm;

auto compiler = Compiler::NewBuilder()
                    .DeclareVariable("a", CelType::Int())
                    .DeclareVariable("b", CelType::Int())
                    .Build().value();
auto program  = compiler.Compile("a * b + 1").value();
auto engine   = Engine::NewBuilder().Build().value();      // once per process
auto instance = engine.Plan(program).value();              // once per program

Activation act;
act.Bind("a", Value::Int(6));
act.Bind("b", Value::Int(7));
auto result = instance.Eval(act).value();                  // => 43
```

The `Program` is portable bytes — compile in one process, evaluate in
another.

## Language bindings

The compiled `.wasm` is portable — every binding embeds the *same*
artifact and only marshals `Activation` ↔ `Value` on its native side.
Adding a host language is writing a small shim, never another CEL
implementation.

| Language | Status | Runtime under the hood |
| --- | --- | --- |
| **C++** | ✅ **first-class today** — the reference host. Headers at `compiler/{compiler,program}.h`, `eval/{engine,instance,activation,value}.h`. | wasmtime C API (`@wasmtime`) |
| **Go** | ⛔ planned. The bindings target is a small Go package mirroring `declare → compile → plan → bind → eval`; loads the same `.wasm` via [`wazero`](https://github.com/tetratelabs/wazero) (pure-Go, no CGO) for portability and [`wasmtime-go`](https://github.com/bytecodealliance/wasmtime-go) for top-end speed. | wazero or wasmtime-go |
| **TypeScript** | ⛔ planned. A small `npm` package that runs the compiled module on the browser's or Node's native wasm engine (V8 / SpiderMonkey / JSC). Useful for client-side policy. | platform-native wasm |
| **Rust** | ⛔ planned. A `cel-wasm` crate over `wasmtime` (Rust is wasmtime's native API, so this is the thinnest binding). Natural for embedders already in the bytecodealliance ecosystem. | `wasmtime` crate |

Why the wait: each binding is its own surface (idiomatic API, error
mapping, type bindings to platform-native types) — work, but no
compiler changes. The wasm artifact + `cel.abi` schema already carry
everything a binding needs; once a binding is written, the same
expression evaluates identically across all of them.

## Custom functions — two paths

The diagram at the top shows the two backends; the choice between them
is **one decision: do you want the custom-function logic sandboxed
from the embedder's process?**

| | **`@host.` — trusted C++ in-process** | **`@component.` — sandboxed WebAssembly component** |
| --- | --- | --- |
| Where the body runs | In the embedder's address space, as a C++ lambda | In an isolated wasm component instance with its own linear memory |
| Author language | C++ only (typed `AddTypedFunction` callback) | Any language with a `wasm32-wasip2` toolchain (C++ today; TinyGo planned) |
| Memory access | Full access to the embedder's process memory | Cannot read or write the embedder's memory |
| Syscalls / I/O | Whatever the C++ does | Cannot syscall; cannot do I/O; cannot DoS the host |
| Hot-swap at runtime | Re-link / re-deploy your binary | Drop in new component bytes; call `AddComponent` again |
| Per-call cost | ~3 µs | ~4 µs (the canonical-ABI hop) |
| When to pick it | You wrote the fn, you trust it, you want speed | You don't fully trust the code (third-party plugin), or you want polyglot authoring, or you want runtime updates without re-linking |

Both paths declare the function the same way in the `.celfn` IDL — the
backend is the `@<prefix>` token:

```celfn
int @host.length(string s);                            // trusted C++ path
bool @component.allow(string subject, string action);  // sandboxed wasm path
```

**Why we bothered with the sandbox path.** Embedders running policy
typically have *some* functions they trust completely (their own code)
and *some* they want isolated — third-party policy plugins, customer-
authored predicates, code the security team hasn't reviewed yet. The
component path makes the second category safe to load: a malicious or
buggy function can only return `error` or a wrong value, it can't
escape, can't peek at other tenants' data, can't exhaust the host's
file descriptors. It's the same isolation property wasmtime gives the
whole CEL expression — extended to the embedder's own extensions.

Full guides:
[host functions](doc/user-guide/writing-host-functions.md) ·
[component functions](doc/user-guide/writing-component-functions.md).

## Documentation

- **[User guide](doc/user-guide/index.md)** — embedder API in depth.
- **[Writing host functions](doc/user-guide/writing-host-functions.md)** —
  typed C++ callbacks, proto / list / map args.
- **[Writing component functions](doc/user-guide/writing-component-functions.md)** —
  Component-Model custom fns via `cel_wasm_component`.
- **[CEL language reference](doc/langdef.md)** — semantics we honour.

## Build

```bash
bazel build //...
bazel test //...
```

macOS (Apple Silicon) and Linux (arm64 / x86_64). Toolchain (wasi-sdk,
wasmtime, binaryen) fetched by Bazel.
[`bazelisk`](https://github.com/bazelbuild/bazelisk) + `clang` /
`lld` is all you need (`brew install llvm` on macOS;
`apt install clang lld build-essential` on Linux). Docker image at
[`docker/Dockerfile`](docker/Dockerfile).

Released under the [Apache License 2.0](LICENSE).
