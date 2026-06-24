# cel-wasm

[![CI](https://github.com/augustinemathew/cel-spec-wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/augustinemathew/cel-spec-wasm/actions/workflows/ci.yml)
[![Docs](https://github.com/augustinemathew/cel-spec-wasm/actions/workflows/pages.yml/badge.svg)](https://augustinemathew.github.io/cel-spec-wasm/)
[![Conformance](https://img.shields.io/badge/CEL%20conformance-2035%20pass%20·%200%20fail-success)](conformance/README.md)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

**cel-wasm is an ahead-of-time compiler and runtime for
[CEL](https://github.com/google/cel-spec).** It type-checks a CEL
expression, compiles it to a portable, sandboxed WebAssembly *Program*,
and evaluates that Program as native code — so a host can run sensitive or
untrusted policy expressions at native speed without embedding a CEL
interpreter, and without the expression being able to escape its sandbox.

Created and maintained by Augustine Mathew. Status: **beta** — the
pipeline, sandbox, and conformance results are real and reproducible; the
hardening work that remains is listed plainly, not buried.

📖 **[Documentation site →](https://augustinemathew.github.io/cel-spec-wasm/)**

## The problem it addresses

CEL is the expression language Kubernetes, Envoy, Istio, and IAM use to
answer *"is this allowed?"*. Today each host embeds its own interpreter —
`cel-cpp`, `cel-go`, `cel-rust` — and those implementations drift in
behavior and performance, while running policy logic *in-process*, where a
bad or hostile expression is a liability.

cel-wasm changes the shape of the problem. One compiler lowers the
expression to a portable `.wasm` module **once**; any host then runs the
identical bytes. That yields two properties at the same time:

- **One source of CEL semantics.** The compiler is the only component that
  understands CEL. Every host executes the same artifact, so cross-language
  semantic drift is structurally impossible rather than merely tested-for.
- **Sandboxed by construction.** What runs is native code confined to a
  WebAssembly sandbox — bounded linear memory, no syscalls, no I/O, no
  unbounded recursion. The expression cannot reach the host even in
  principle, and that guarantee extends, uniquely, to *custom functions*.

## Design philosophy — speed and security from one decision

The defining choice is to make the compile target do double duty.
Compiling to sandboxed WebAssembly is a deliberate compromise that buys
speed *and* security from a single design, rather than trading one for the
other:

- **Security comes from the target.** A CEL expression lowered to a
  sandboxed `.wasm` *physically cannot* make a syscall, read host memory,
  perform I/O, recurse without bound, or hang the process. Untrusted custom
  functions can run as isolated components with their own linear memory.
  This is a stronger guarantee than an in-process interpreter can offer.
- **Speed comes from the same target.** Ahead-of-time lowering plus a
  native JIT removes the AST walk and tree dispatch from the eval path. By
  the time an expression is evaluated, it is native code.
- **The cost, stated up front.** Serving both goals from one artifact costs
  the **static subset** — no `dyn`, variables typed in advance — and a
  per-Eval boundary that some workloads amortize and some don't. That is a
  conscious trade, documented in
  [Performance](#performance-a-crossover-not-a-headline) and
  [What cel-wasm doesn't do](#what-cel-wasm-doesnt-do), not papered over.

## How it works

A CEL expression travels through one compile pipeline to a `Program`, and
that `Program` is planned once and evaluated many times:

```
  CEL source  +  variable & function declarations
        │
        │   cel-cpp parser + type-checker          (reused, not reimplemented)
        ▼
  type-checked AST
        │
        │   static-subset validation               (reject `dyn` & unsupported shapes)
        ▼
  typed IR  +  annotations
        │
        │   lowering · memory layout · codegen      (Binaryen)
        ▼
  Program  =  .wasm bytes  +  cel.abi               ← portable, inspectable artifact
        │
        │   Engine.Plan → JIT to native code        (one JIT per Program)
        ▼
  ┌ WebAssembly sandbox ────────────────────────────
  │   Instance.Eval(Activation)
  │     native code + cel runtime kernel
  │     no syscalls · no I/O · no unbounded loops
  │     @component custom functions run here, isolated
  └─────────────────────────────────────────────────
        │
        ▼
  Value
```

The five nouns — `Compiler → Program → Engine → Instance → Value` — are the
whole public API surface. `Program` is the compatibility boundary: it is
plain bytes, so you can compile in one process, write it to disk, and
evaluate it in a process that never links the compiler.

## Try it

```bash
# One-time: fetch the vendored parser/type-checker.
third_party/fetch_cel_cpp.sh

# Evaluate CEL end-to-end: compile → wasm → JIT → native.
bazel run //tools/cel:cel -- eval '1 + 2 + 3'                          # => 6
bazel run //tools/cel:cel -- eval 'a * b' --var a:int=6 --var b:int=7  # => 42

# Compile to a wasm artifact you can ship and evaluate elsewhere.
bazel run //tools/cel:cel -- compile 'a * b + 1' \
    --var a:int --var b:int --output /tmp/expr.wasm

# Or run the smallest complete embed:
bazel run //examples:01_hello_world
```

Embedding from C++:

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
condensed. Every example is a buildable target run by
`//examples:examples_smoke_test` on each `bazel test` sweep, so the code in
the docs is code that executes. The [`examples/`](examples/) directory
covers variables, saving and reloading compiled programs, custom host
functions, partial evaluation with unknowns, protobuf messages, error
handling, and a sandboxed component function.

## Performance: a crossover, not a headline

cel-wasm is not unconditionally faster than a tree-walking interpreter, and
there is no single representative multiplier — the result is two-sided and
workload-dependent, so both surfaces are shown directly. Numbers below are
the 2026-06-17 run, static-link mode (the default), identical expressions
and inputs on both engines, `-c opt`, Apple Silicon
([`benchmark/eval/results/2026-06-17-Mac.md`](benchmark/eval/results/2026-06-17-Mac.md)).
The shape is predictable: the crossover is roughly ten operations. Above
it, removing the AST walk more than pays for the one-time wasm boundary
crossing; at the bottom, basic single operations now land at **parity**
(`a == b` ≈ 1.0×, after the boundary cost was optimized down).

**Where it wins** — repetition to amortize, and constants folded at compile
time:

| Workload | speedup vs cel-cpp |
| --- | :---: |
| 1000-term arithmetic chain (int / double) | 18× / 20× |
| 1000-term string-concat chain | 3.1× |
| constant list literal folded to a constant (`size([…100])` / `[…1000]`) | 25× / 220× |
| complex regex `.matches()` | ~59× † |

† This is a runtime-configuration difference, not codegen: cel-wasm caches
the compiled RE2 pattern per Instance, while `cel-cpp`'s default runtime
recompiles it per evaluation (its optional precompilation extension would
close most of the gap). Quoted with that caveat on purpose.

**Where it loses** — and why, named honestly:

| Workload | gap | cause |
| --- | :---: | --- |
| 100-entry **map** literal construction | ~43× slower | constant **map** literals are still rebuilt every Eval; constant **list** literals were fixed in m31 (they now fold to a compile-time constant), and const-map folding is the queued follow-up. |
| single proto / timestamp accessor (`m.f64`, `ts - ts`) | ~1.5–2× slower | each read crosses one host trampoline; the cost is amortized away as soon as the expression does more than a single field read. |

Full methodology and per-cell numbers:
[`benchmark/eval/results/`](benchmark/eval/results/) and
[`benchmark/ANALYSIS.md`](benchmark/ANALYSIS.md). Reproduce with
`benchmark/eval/run.sh` (three-way: dynamic / static / cel-cpp).

## Conformance

Scored against the upstream CEL conformance corpus, in both link modes:

| | |
| --- | --- |
| Passing | **2035 / 2516** of the corpus (80.9%) |
| Of rows attempted | **2035 / 2035 = 100%** (excludes the by-design skips below) |
| Intentional skips | 481 — static subset / `dyn` (227, outside the static subset by design), check-disabled rows (144), and not-yet-shipped scope (110: extensions 55, check-only 25, spec edges 18, type-env 12) |
| Failing | **0** |

Skips are categorized, not hidden: the 227 `dyn` rows are a deliberate
scope decision; the rest are tracked implementation work. Live per-fixture
breakdown (autogenerated): [`conformance/README.md`](conformance/README.md);
reproduce with `bazel run //conformance:run_conformance`.

Extensions implemented today: `string_ext` (including `strings.format` —
172/216, 0 fails), `math_ext` (194/199, 0 fails), `network_ext` (69/69),
and `optionals` (26/70; the remainder require `dyn`).

## Custom functions — two trust models

A single decision governs extension: does the function's code need to be
sandboxed away from your process?

| | `@host` — trusted C++ | `@component` — sandboxed wasm |
| --- | --- | --- |
| Runs | in your address space, as a C++ lambda | in an isolated wasm instance with its own linear memory |
| Author language | C++ | anything with a `wasm32-wasip2` toolchain (C++ today; TinyGo planned) |
| Can read host memory / syscall | yes — whatever the C++ does | no — cannot escape, perform I/O, or starve the host |
| Update | re-link your binary | hot-swap: hand new bytes to `AddComponent` |
| Per-call cost | ~3 µs | ~4 µs |

```celfn
int  @host.length(string s);                           // trusted C++ path
bool @component.allow(string subject, string action);  // sandboxed wasm path
```

Both are declared in the same `.celfn` IDL; the backend is the `@<prefix>`
token. A host function binds with the **same declaration string** the
compiler saw, and the lambda's signature is validated against it at
registration:

```cpp
engine.BindFunction("int @host.length(string s);",
                    [](absl::string_view s) -> absl::StatusOr<int64_t> {
                      return static_cast<int64_t>(s.size());
                    });
```

The `@component` path is what makes third-party policy plugins,
customer-authored predicates, and not-yet-reviewed code safe to load at
all — a capability a stock in-process CEL runtime does not provide. Guides:
[host functions](doc/user-guide/writing-host-functions.md) ·
[component functions](doc/user-guide/writing-component-functions.md);
runnable: [`examples/04`](examples/04_host_functions.cc),
[`examples/09`](examples/09_component_functions.cc).

## Engineering discipline

The properties above are the point of the project, so they are enforced
rather than asserted:

- **The reference implementation is the oracle.** A test harness links the
  real `cel-cpp` parser, checker, and runtime and evaluates expressions
  end-to-end; it is the authoritative tiebreaker for any question of
  semantics — value, canonical form, error-vs-value, rounding, overflow.
  Expected results are confirmed against it, not reasoned from memory.
- **Differential fuzzing.** A fuzzer drives both engines on generated
  expressions and diffs the results, so divergences from `cel-cpp` are
  found mechanically rather than waited for.
- **Fail closed on the unsupported.** The static subset is enforced at a
  single gate; `dyn` and shapes the compiler cannot lower are rejected at
  compile time with a status, never miscompiled silently.
- **Benchmarks are reported two-sided.** Every regression versus `cel-cpp`
  is published with its architectural cause, as above.
- **The compiler/runtime boundary is structural.** `Program` is a frozen
  byte artifact; the compiler depends only on shared type vocabulary, never
  on the evaluator — so the compiler stays independently buildable and the
  artifact stays portable.

Architecture and design docs: [`doc/README.md`](doc/README.md).

## What cel-wasm doesn't do

cel-wasm targets CEL's **static subset** by design — variables and
intermediates are statically typed and declared up front:

- **No dynamic typing (`dyn`).** This is the load-bearing 227-row
  conformance skip and a deliberate trade.
- **`@native` CEL-defined helper bodies** type-check today, but body
  codegen has not shipped.
- A handful of named edges: `cel.@block`, proto2 extension-field accessors,
  multi-byte-UTF-8 `size()`.

If your workload needs the full dynamic surface, use
[`cel-cpp`](https://github.com/google/cel-cpp) or
[`cel-go`](https://github.com/google/cel-go). cel-wasm trades that surface
for AOT speed, portability, and the sandbox; it is not a drop-in
replacement.

## Status and limitations

Beta — usable and honest about what remains. The known gaps:

- **Constant `map` literals are rebuilt every Eval** (the ~43× row above).
  Constant `list` literals already fold to a compile-time constant (m31);
  const-map folding is the queued follow-up.
- **Very large constant `map` literals do not compile** — a capability
  limit, not a crash: an oversized literal is rejected *at compile* with a
  graceful `ResourceExhausted`; put large constant data in an
  activation-bound variable instead. (Large constant `list` literals now
  compile, after m31 raised the static window.)
- **Language bindings beyond C++ are designed, not built** — the `.wasm` +
  `cel.abi` already carry everything a Go/TS/Rust shim needs.
- **Hardening continues** — differential fuzzing and a sanitizer gate ship
  today; allocator caps and a release-versioning policy are still to come.
  See the [security model](doc/user-guide/security-model.md) for the
  current threat-model boundaries.

Every known gap is pinned by a skipped-with-reason test
(`e2e/known_bugs_test.cc`) or a backlog entry
([`doc/implementation-plan/cleanup-backlog.md`](doc/implementation-plan/cleanup-backlog.md)).

## Documentation

Using cel-wasm:
- [Getting started](doc/user-guide/getting-started.md) — install to first eval
- [Examples](examples/) — nine runnable programs
- [User guide](doc/user-guide/index.md) — the embedder API in depth
- [FAQ](doc/user-guide/faq.md) · [Security model](doc/user-guide/security-model.md)
- [CEL language definition](doc/langdef.md)

Contributing:
- [Contributing guide](doc/contributing.md) — workflow, lint, and test gates
- [Architecture & design docs](doc/README.md) — the design-doc index

## Build

```bash
bazel build //...
bazel test //...
```

macOS (Apple Silicon) and Linux (arm64 / x86_64). The toolchain (wasi-sdk,
the WebAssembly engine, Binaryen) is fetched by Bazel —
[`bazelisk`](https://github.com/bazelbuild/bazelisk) plus `clang`/`lld` is
all you need (`brew install llvm` on macOS; `apt install clang lld
build-essential` on Linux). Docker image at
[`docker/Dockerfile`](docker/Dockerfile).

## Author

cel-wasm is created and maintained by **Augustine Mathew**
([augustine.mathew@gmail.com](mailto:augustine.mathew@gmail.com)). It is an
independent project, not affiliated with or endorsed by any employer. See
[NOTICE](NOTICE) and [AUTHORS](AUTHORS).

*The internal C++ namespace is `celwasm::`; the project name is `cel-wasm`,
matching `cel-cpp` / `cel-go`.*

Released under the [Apache License 2.0](LICENSE).
