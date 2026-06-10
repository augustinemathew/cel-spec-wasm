# cel-wasm

**Compile [CEL](https://github.com/google/cel-spec) to WebAssembly,
ahead of time. JIT it to native code. Run the same artifact on every
host — sandboxed.**

> Status: **beta**. The pipeline, sandbox, benchmarks, and conformance
> numbers below are real and reproducible; the remaining hardening gaps
> are listed honestly in [Production readiness](#production-readiness).

CEL today is an interpreter, re-implemented per host language:
Kubernetes, Envoy, Istio, IAM, Firebase each link their own `cel-cpp` /
`cel-go` / `cel-rust`, and the implementations drift in behavior and
performance. cel-wasm takes a different shape: **one compiler** lowers
the expression to a portable `.wasm` module, and
[wasmtime](https://wasmtime.dev/)'s [Cranelift](https://cranelift.dev/)
JIT turns that into native machine code inside a sandbox — on any host.
Compile once; every runtime executes the identical artifact.

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

## Try it in 60 seconds

```bash
# One-time: fetch the vendored parser/type-checker.
third_party/fetch_cel_cpp.sh

# Evaluate CEL end-to-end: compile → wasm → Cranelift JIT → native.
bazel run //tools/cel:cel -- eval '1 + 2 + 3'                          # => 6
bazel run //tools/cel:cel -- eval 'a * b' --var a:int=6 --var b:int=7  # => 42

# Compile to a wasm artifact you can ship and evaluate elsewhere.
bazel run //tools/cel:cel -- compile 'a * b + 1' \
    --var a:int --var b:int --output /tmp/expr.wasm
```

Or run the smallest complete embed:

```bash
bazel run //examples:01_hello_world
```

## Embedding from C++

```cpp
#include "compiler/compiler.h"
#include "eval/engine.h"

auto builder = celwasm::Compiler::NewBuilder();
builder.DeclareVariable("age", celwasm::CelType::Int())
    .DeclareVariable("country", celwasm::CelType::String());
auto compiler = std::move(builder).Build().value();

auto program = compiler.Compile("age >= 18 && country in ['US', 'CA']").value();

auto engine   = celwasm::Engine::NewBuilder().Build().value();  // once per process
auto instance = engine.Plan(program).value();                   // JIT, once per program

celwasm::Activation act;
act.Bind("age", celwasm::Value::Int(25))
   .Bind("country", celwasm::Value::String("US"));
bool allowed = instance.Eval(act)->AsBool().value();            // => true
```

This snippet is [`examples/02_variables.cc`](examples/02_variables.cc),
condensed — every example is a buildable target run by CI, so the code
you read here is code that executes. A `Program` is pure bytes: compile
it in one process, write it to disk, evaluate it in a process that
never links the compiler
([`examples/03_compile_once_run_anywhere.cc`](examples/03_compile_once_run_anywhere.cc)).

**More runnable examples — [`examples/`](examples/):** variables,
saving/loading compiled programs, custom host functions, partial
evaluation with unknowns, protobuf messages, error handling,
functions returning errors/unknowns, and a sandboxed component
function.

## Why cel-wasm

- **Two compilers deep, zero interpreters.** The CEL compiler lowers
  the expression to wasm ahead of time (Binaryen, `-O0..3`); Cranelift
  JITs that wasm to native machine code at `Plan` time (~240–300 µs,
  amortized across evals). By the time `Eval()` runs there is no AST
  walk, no tree dispatch, no wasm interpreter — just native code.
- **Sandboxed by construction.** Bounded linear memory, no syscalls,
  no I/O, no recursion. Policy expressions — and, uniquely, *custom
  functions* — can come from people you don't fully trust.
- **One semantic implementation.** The compiler is the only component
  that knows CEL. Every host runs the same bytes; cross-language
  semantic drift is structurally impossible.
- **Extend in any language.** Custom functions are either trusted C++
  lambdas (`@host`) or WebAssembly Component-Model components
  (`@component`) with their own linear memory — authored in any
  language with a `wasm32-wasip2` toolchain, hot-swappable at runtime.

## Performance — vs the `cel-cpp` tree-walking interpreter

Measured 2026-06-09 over a 232-cell corpus (every CEL operator family ×
data type, `benchmark/eval/corpus/`), same expressions and inputs on
both engines, `-c opt`, Apple Silicon, statically-linked mode (the
default). Corpus-wide geomean is **parity (0.95×)** with a sharply
two-sided distribution — both sides shown, deliberately.

**Where cel-wasm wins** — anything with repetition to amortize (the
crossover is ~10 operations):

| Workload | cel-wasm | cel-cpp | speedup |
| --- | ---: | ---: | :---: |
| 100-elem comprehension `.all(x, …)` | 713 ns | 6.6 µs | **9×** |
| 20-elem `.map(x, x * 2)` | 219 ns | 5.4 µs | **25×** |
| 1000-term arithmetic chain | 2.0 µs | 33.4 µs | **17×** |
| 100-term string concat chain | 2.8 µs | 6.1 µs | **2.2×** |
| `x in <1 M ints>` (activation-bound list) | 3.27 ms | 5.68 ms | **1.7×** |
| regex `.matches()` (complex, hot loop) | 186 ns | 8.9 µs | 48× † |

† A runtime-configuration difference, not codegen: we cache the
compiled RE2 pattern per Instance; cel-cpp's default runtime recompiles
per evaluation (its optional precompilation extension would close most
of this). Quoted with that caveat, deliberately.

**Where cel-wasm currently loses:**

| Workload | gap | why |
| --- | :---: | --- |
| 100-entry map literal construction | **44×** | constant aggregates are rebuilt every Eval; cel-cpp folds them at plan time. The const-aggregate codegen milestone is the planned fix. |
| `x in [<100-elem list literal>]` | 2× | same per-eval construction cost |
| 1000-char string equality | 8× | byte-loop compare in wasm vs native SIMD memcmp |
| single op (`a == b`, …) | 1.2–1.9× | the per-Eval floor: one wasm boundary crossing + arena reset = 62 ns minimum, visible only on expressions too small to amortize it |

Full methodology, per-family geomeans, and the cause of every loss row:
[`m28-bench-results.md`](doc/implementation-plan/rewrite/m28-bench-results.md).
Reproduce with `benchmark/eval/run.sh` (three-way: dynamic / static /
cel-cpp).

## Conformance

| | |
| --- | --- |
| **Passing** | **1899 / 2454** of the upstream CEL conformance corpus (77.4%) |
| Of expressions attempted | **1899 / 1991 = 95.4%** (excludes the by-design skips below) |
| Intentional skips | 463 — mostly DYN/dynamic typing (227, out of scope by design), check-disabled rows (144), unimplemented extensions (55) |
| Failing | 92 — multi-byte-UTF-8 `size()`, Y9999 timestamp stringification, a few map-null-pruning edges |

Reproduce with `bazel run //conformance:run_conformance`.

**Extensions ship today**: `string_ext` (incl. `strings.format` — 172/216,
0 fails), `math_ext` (194/199, 0 fails), `network_ext` (69/69),
`optionals` (22/70; the rest need DYN).

## Custom functions — two trust models

One decision: **does the function's code need to be sandboxed away from
your process?**

| | `@host.` — trusted C++ | `@component.` — sandboxed wasm component |
| --- | --- | --- |
| Runs | in your address space, as a C++ lambda | in an isolated wasm instance, own linear memory |
| Author language | C++ | anything with a `wasm32-wasip2` toolchain (C++ today, TinyGo planned) |
| Can read your memory / syscall | yes — whatever the C++ does | **no** — can't escape, can't I/O, can't DoS the host |
| Update | re-link your binary | hot-swap: hand new bytes to `AddComponent` |
| Per-call cost | ~3 µs | ~4 µs |

```celfn
int @host.length(string s);                            // trusted C++ path
bool @component.allow(string subject, string action);  // sandboxed wasm path
```

Both are declared in the same `.celfn` IDL; the backend is the
`@<prefix>` token. A host function binds with the **same declaration
string** the compiler saw — the lambda's signature is validated against
it at registration, not at eval:

```cpp
engine.BindFunction("int @host.length(string s);",
                    [](absl::string_view s) -> absl::StatusOr<int64_t> {
                      return static_cast<int64_t>(s.size());
                    });
```

Stock CEL only gives you the trusted-C++ row; the component row is what
makes third-party policy plugins, customer-authored predicates, and
not-yet-security-reviewed code safe to load. Guides:
[host functions](doc/user-guide/writing-host-functions.md) ·
[component functions](doc/user-guide/writing-component-functions.md) ·
runnable: [`examples/04`](examples/04_host_functions.cc) and
[`examples/09`](examples/09_component_functions.cc).

## What cel-wasm doesn't do

cel-wasm targets CEL's **static subset** by design. Variables and
intermediates are statically typed, declared up front:

- **No dynamic typing (`dyn`).** This is the load-bearing 227-row
  conformance skip and a deliberate trade.
- **`@native` CEL-defined helper bodies** type-check today; body
  codegen hasn't shipped.
- A handful of named edges: `cel.@block`, proto2 extension-field
  accessors, multi-byte-UTF-8 `size()`.

If your workload needs the full dynamic surface — every extension,
every `dyn` case — use
[`cel-cpp`](https://github.com/google/cel-cpp) or
[`cel-go`](https://github.com/google/cel-go). cel-wasm trades that
surface for AOT speed, portability, and the sandbox; it's not a drop-in
replacement.

## Production readiness

Not yet — and we'd rather tell you exactly why than have you find out:

- **Constant list/map literals are rebuilt every Eval** (the 44× loss
  row above); const-aggregate folding is the planned fix.
- **Oversized literal aggregates can trap the runtime** instead of
  returning a graceful error (tracked, with a pinned regression test).
- **Language bindings beyond C++ are designed, not built** — the
  `.wasm` + `cel.abi` already carry everything a Go/TS/Rust shim needs.
- **General hardening** — fuzzing, allocator caps, CI gates, release
  versioning.

Every known gap is pinned by a skipped-with-reason test
(`e2e/known_bugs_test.cc`) or a backlog entry
(`doc/implementation-plan/cleanup-backlog.md`). Issues, PRs, and
benches welcome — **pick this project if you want to help close the
list.**

## Documentation

**Using cel-wasm** (embedders):
- [Getting started](doc/user-guide/getting-started.md) — install to first eval
- [Examples](examples/) — seven runnable programs
- [User guide](doc/user-guide/index.md) — the embedder API in depth
- [FAQ](doc/user-guide/faq.md) · [Security model](doc/user-guide/security-model.md)
- [CEL language definition](doc/langdef.md)

**Contributing** (compiler hackers):
- [Contributing guide](doc/contributing.md) — workflow, lint, test gates
- [Architecture & design docs](doc/README.md) — the design-doc index

## Build

```bash
bazel build //...
bazel test //...
```

macOS (Apple Silicon) and Linux (arm64 / x86_64). The toolchain
(wasi-sdk, wasmtime, binaryen) is fetched by Bazel —
[`bazelisk`](https://github.com/bazelbuild/bazelisk) + `clang`/`lld` is
all you need (`brew install llvm` on macOS; `apt install clang lld
build-essential` on Linux). Docker image at
[`docker/Dockerfile`](docker/Dockerfile).

*Internal C++ namespace is `celwasm::`; the project name is `cel-wasm`,
matching `cel-cpp` / `cel-go`.*

Released under the [Apache License 2.0](LICENSE).
