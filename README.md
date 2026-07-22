# cel-wasm

[![Conformance](https://img.shields.io/badge/CEL%20conformance-2035%2F2035%20applicable%20·%200%20fail-success)](conformance/README.md)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

**cel-wasm is an ahead-of-time compiler and runtime for
[CEL](https://github.com/google/cel-spec), the expression language that
answers *"is this allowed?"* in Kubernetes, Envoy, and Google Cloud IAM.**
It type-checks a CEL expression, compiles it to a portable, sandboxed
WebAssembly *Program*, and evaluates that Program as native code — no
interpreter in your process, and no way for the expression to escape its
sandbox.

Created and maintained by Augustine Mathew. 

Status: **beta**

The pipeline, sandbox, and conformance results are real and reproducible; the
hardening work that remains is listed plainly, not buried.

📖 **[Documentation site →](https://augustinemathew.github.io/cel-spec-wasm/)**

## CEL in 60 seconds

*Skip ahead if you already use CEL.*

Every production system accumulates rules that change faster than the code
enforcing them: who may call this API, which requests route where, when an
alert should fire. Hard-code them and every policy tweak is a deploy;
embed a scripting language and you have handed rule authors a
Turing-complete foothold inside your process.

CEL — the Common Expression Language, from Google — is the middle path: a
small language for expressing **rules**, usually boolean conditions,
occasionally computed values. A rule is a single expression with familiar
C-like syntax. It is statically typeable, mutation-free, and deliberately
**not Turing-complete**: evaluation runs in linear time and always
terminates. The canonical example from the CEL spec:

```cel
// Approve the withdrawal?
account.balance >= transaction.withdrawal
    || (account.overdraftProtection
    && account.overdraftLimit >= transaction.withdrawal - account.balance)
```

The host application declares the variables (`account`, `transaction`) and
their types up front, binds concrete values at request time, and
evaluates — here, to a `bool`. Because an expression cannot loop forever,
mutate anything, or perform I/O, it is safe to accept from a config file,
an API request, or a customer. That is why CEL is the policy language of
Kubernetes (validation and admission rules), Envoy and Istio (RBAC and
routing), and Google Cloud IAM (conditional access).

## What cel-wasm changes

Each CEL host today embeds its own tree-walking interpreter — `cel-cpp`,
`cel-go`, `cel-rust` — with two costs: the implementations drift in
behavior and performance, and the rule runs *in-process*, where a bad or
hostile expression is a liability.

cel-wasm makes one deliberate design decision — compile the expression to
sandboxed WebAssembly, once — and gets three properties from it:

- **One source of CEL semantics.** The compiler is the only component that
  understands CEL; every host runs the identical `.wasm` bytes, so
  cross-language semantic drift is structurally impossible rather than
  merely tested-for.
- **Sandboxed by construction.** What runs is native code confined to a
  WebAssembly sandbox — bounded linear memory, no syscalls, no I/O, no
  unbounded recursion. The expression cannot reach the host even in
  principle, and the guarantee extends, uniquely, to *custom functions*.
- **Native speed.** Ahead-of-time lowering plus a native JIT removes the
  AST walk and tree dispatch from the eval path; by the time an expression
  is evaluated, it is native code.

## How it works

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

cel-wasm is not unconditionally faster than a tree-walking interpreter,
and there is no single representative multiplier — the result is two-sided
and workload-dependent, so both surfaces are shown directly. Numbers below
are the 2026-06-27 run, static-link mode (the default), identical
expressions and inputs on both engines, `-c opt`, Apple Silicon
([`benchmark/eval/results/2026-06-27-Mac.md`](benchmark/eval/results/2026-06-27-Mac.md)).
The shape is predictable: the per-operator crossover is roughly three to
fourteen operations depending on the family (regression table in
[`benchmark/README.md`](benchmark/README.md)); below it the fixed
sandbox-boundary cost dominates, above it removing the AST walk more than
pays for it.

**Where it wins** — repetition to amortize, and work moved to compile
time:

| Workload | speedup vs cel-cpp |
| --- | :---: |
| 1000-term arithmetic chain over variables (int / double) | 18× / 19× |
| 1000-term string-concat chain | 3.1× |
| constant list folded at compile time (`size([…100])` / `[…1000]`) | 26× / 224× |
| constant-map lookup, 256 entries, baked hash index (`m[k]` / `k in m`) | 118× / 198× |
| comprehension `[1…20].map(x, x * 2)` | 34× |
| complex regex `.matches()` | ~59× † |

† A runtime-configuration difference, not codegen: cel-wasm caches the
compiled RE2 pattern per Instance, while `cel-cpp`'s default runtime
recompiles it per evaluation (its optional precompilation extension would
close most of the gap). Quoted with that caveat on purpose.

**Where it loses** — and why, named honestly:

| Workload | gap | cause |
| --- | :---: | --- |
| single proto / duration accessor (`m.str_to_i32["b"]`, duration `==`) | ~1.1–1.7× slower | each read crosses one host trampoline; amortized as soon as the expression does more than a single accessor. |
| early-exit `in` over a 1000-string activation-bound list | ~14× slower | a bound aggregate is copied into the sandbox every Eval, so a first-element hit still pays the full marshal; cel-cpp reads the host list by reference. |
| `contains()` on a 10 KB string | ~6× slower | cel-cpp reaches the host libc's vectorized substring search; the wasm kernel is a scalar loop. |

Full methodology and per-cell numbers:
[`benchmark/eval/results/`](benchmark/eval/results/) and
[`benchmark/ANALYSIS.md`](benchmark/ANALYSIS.md). Reproduce with
`benchmark/eval/run.sh` (three-way: dynamic / static / cel-cpp).

## Conformance

Scored against the upstream CEL conformance corpus, in both link modes:

| | |
| --- | --- |
| Passing | **2035 / 2035 of applicable rows (100%)** — the 481 inapplicable rows are mostly the `dyn` feature, outside the static subset by design |
| Failing | **0** |
| Inapplicable | 481 — `dyn` (227), check-disabled rows (144), and not-yet-shipped scope (110: extensions 55, check-only 25, spec edges 18, type-env 12) |
| Whole corpus | 2035 / 2516 (80.9%) |

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
  real `cel-cpp` parser, checker, and runtime end-to-end; expected results
  are confirmed against it, not reasoned from memory.
- **Differential fuzzing** drives both engines on generated expressions
  and diffs the results, so divergences from `cel-cpp` are found
  mechanically rather than waited for.
- **Fail closed on the unsupported.** `dyn` and shapes the compiler cannot
  lower are rejected at compile time with a status, never miscompiled
  silently — and every fixed capability limit is pinned by a
  just-inside / just-past test pair (`e2e/limits_test.cc`).
- **Benchmarks are reported two-sided**, every loss with its architectural
  cause, as above.
- **The compiler/runtime boundary is structural.** `Program` is a frozen
  byte artifact; the compiler never links the evaluator, so the artifact
  stays portable and the compiler independently buildable.

Architecture and design docs: [`doc/README.md`](doc/README.md).

## Scope and status

cel-wasm targets CEL's **static subset** by design — variables and
intermediates are statically typed and declared up front. If your workload
needs the full dynamic surface, use
[`cel-cpp`](https://github.com/google/cel-cpp) or
[`cel-go`](https://github.com/google/cel-go); cel-wasm trades that surface
for AOT speed, portability, and the sandbox. It is not a drop-in
replacement.

The known gaps, each pinned by a skipped-with-reason test
(`e2e/known_bugs_test.cc`) or a backlog entry
([`doc/implementation-plan/cleanup-backlog.md`](doc/implementation-plan/cleanup-backlog.md)):

- **No dynamic typing (`dyn`).** The load-bearing 227-row conformance skip
  and a deliberate trade.
- **`@native` CEL-defined helper bodies** type-check today, but body
  codegen has not shipped.
- **Very large constant literals do not compile** — a capability limit,
  not a crash: an oversized constant list or map is rejected *at compile*
  with a graceful `ResourceExhausted`; put large constant data in an
  activation-bound variable instead.
- A handful of named edges: `cel.@block`, proto2 extension-field
  accessors, multi-byte-UTF-8 `size()`.
- **Language bindings beyond C++ are designed, not built** — the `.wasm` +
  `cel.abi` already carry everything a Go/TS/Rust shim needs.
- **Hardening continues** — differential fuzzing and a sanitizer gate ship
  today; allocator caps and a release-versioning policy are still to come.
  See the [security model](doc/user-guide/security-model.md) for the
  current threat-model boundaries.

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
