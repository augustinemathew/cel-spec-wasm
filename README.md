# cel-wasm

[![Conformance](https://img.shields.io/badge/CEL%20conformance-2035%2F2035%20applicable%20·%200%20fail-success)](conformance/README.md)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

**cel-wasm is an ahead-of-time compiler and runtime for
[CEL](https://github.com/google/cel-spec), the policy expression language
of [Kubernetes](https://kubernetes.io/),
[Envoy](https://www.envoyproxy.io/), and
[Google Cloud IAM](https://cloud.google.com/iam).**
It compiles a type-checked CEL expression to a portable WebAssembly
*Program*, then JITs it to machine code. Evaluation runs native code in a
sandbox. No AST-walking interpreter. No host access beyond explicitly
granted imports.

Status: **beta**

The pipeline, sandbox, and conformance results are reproducible.
Remaining hardening work is listed under [Limitations](#limitations).

📖 **[Documentation site →](https://augustinemathew.github.io/cel-wasm/)**

## CEL in 60 seconds

*Skip ahead if you already use CEL.*

CEL is Google's Common Expression Language: a small,
**non-Turing-complete** language for rules. A rule is one
statically-typed, side-effect-free expression. Evaluation always
terminates. The canonical example from the CEL spec:

```cel
// Approve the withdrawal?
account.balance >= transaction.withdrawal
    || (account.overdraftProtection
    && account.overdraftLimit >= transaction.withdrawal - account.balance)
```

Your service declares `account` and `transaction`, binds values at
request time, and gets back a `bool`. Expressions cannot loop, mutate
state, or do I/O. That makes them safe to accept from a config file or a
customer — and it is why
[Kubernetes](https://kubernetes.io/docs/reference/using-api/cel/),
[Envoy](https://www.envoyproxy.io/) / [Istio](https://istio.io/), and
[Google Cloud IAM](https://cloud.google.com/iam/docs/conditions-overview)
use CEL for policy.

## Quick start

```bash
# One-time: fetch the vendored parser/type-checker, build the CLI.
third_party/fetch_cel_cpp.sh
bazel build //tools/cel:cel

# Evaluate CEL end-to-end: compile → wasm → JIT → native.
bazel-bin/tools/cel/cel eval '1 + 2 + 3'                          # => 6
bazel-bin/tools/cel/cel eval 'a * b' --var a:int=6 --var b:int=7  # => 42

# Compile to a wasm artifact you can ship and evaluate elsewhere.
bazel-bin/tools/cel/cel compile 'a * b + 1' \
    --var a:int --var b:int --output /tmp/expr.wasm

# Or build and run the smallest complete embed:
bazel build //examples:01_hello_world && bazel-bin/examples/01_hello_world
```

Embedding from C++:

```cpp
#include "compiler/compiler.h"
#include "eval/engine.h"

using namespace celwasm;

auto builder = Compiler::NewBuilder();
builder.DeclareVariable("age", CelType::Int())
    .DeclareVariable("country", CelType::String());
auto compiler = std::move(builder).Build().value();

auto program = compiler.Compile("age >= 18 && country in ['US', 'CA']").value();

auto engine   = Engine::NewBuilder().Build().value();  // once per process
auto instance = engine.Plan(program).value();          // JIT, once per program

Activation act;
act.Bind("age", Value::Int(25))
   .Bind("country", Value::String("US"));
bool allowed = instance.Eval(act)->AsBool().value();   // => true
```

This is [`examples/02_variables.cc`](examples/02_variables.cc),
condensed. Every doc snippet is a buildable target, run on each
`bazel test` sweep. More in [`examples/`](examples/): saving and
reloading programs, custom functions, partial evaluation, protobuf
messages, error handling.

## Why compile CEL to WebAssembly?

Every CEL host today embeds its own interpreter: `cel-cpp`, `cel-go`,
`cel-rust`. The implementations drift in behavior and performance. And
policy runs in-process, where a hostile expression is a liability.

cel-wasm compiles the expression once, to sandboxed WebAssembly. That
buys three properties:

- **One source of CEL semantics.** Only the compiler understands CEL.
  Every host runs the identical `.wasm` bytes, so cross-language drift
  is structurally impossible.
- **Sandboxed by construction.** Bounded linear memory. No syscalls, no
  I/O. Host access only through explicitly granted imports. Termination
  is the language's guarantee — CEL has no unbounded loops. The sandbox
  extends to custom functions.
- **Native speed.** Not from WebAssembly itself — from the runtime
  behind it, which emits machine code. What executes per Eval is that
  machine code, not an interpreter loop.
  [How that translates to instructions →](#why-is-it-fast)

## How it works

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/img/pipeline-dark.svg">
  <img alt="Pipeline: CEL source → parse + check (cel-cpp) → lower + codegen (Binaryen) → Program (.wasm + cel.abi, portable bytes) → Plan (Cranelift JIT to machine code) → Eval in the WebAssembly sandbox → Value" src="doc/img/pipeline-light.svg">
</picture>

The five nouns — `Compiler → Program → Engine → Instance → Value` — are
the whole public API. `Program` is plain bytes: compile in one process,
write it to disk, evaluate in another process that never links the
compiler. On the eval side, `Engine` embeds a WebAssembly runtime
([wasmtime](https://wasmtime.dev/), Cranelift). It JITs each Program
once, at `Plan` time.

## Performance

### Why is it fast?

WebAssembly alone adds no speed — it is just a portable instruction
format. The speed comes from what surrounds it: the compiler resolves
every operator to a direct, type-specialized call ahead of time, and the
WebAssembly runtime (wasmtime's Cranelift) emits machine code from that
once, at `Plan` time — so each eval executes straight-line code over
fixed linear-memory slots instead of re-dispatching an expression tree.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/img/why-fast-dark.svg">
  <img alt="1 + 2 evaluated: cel-cpp dispatches per step through boxed values (~32 ns per added operation); cel-wasm's emitted eval body is an arena reset plus one direct call to an overflow-checked add kernel over fixed linear-memory slots (~1.7 ns per added operation)" src="doc/img/why-fast-light.svg">
</picture>

### Measured, both sides

cel-wasm is not unconditionally faster than an interpreter. The result
is workload-dependent, so both sides are shown. All numbers: 2026-07-22
run, identical inputs on both engines, static-link mode, `-c opt`, Apple
Silicon ([full tables](benchmark/eval/results/2026-07-22-Mac.md)).

The crossover sits at roughly 3–14 operations, depending on operator
family ([regression table](benchmark/README.md)). Below it, the fixed
sandbox-boundary cost dominates. Above it, dropping the AST walk wins.

### What it costs to embed

Measured with the public API for `a * b + 1`, with `a`, `b` declared
`int`. Reproduce with `bazel run -c opt //benchmark/compiler:stage_bench`.

| | static link (default) | dynamic link |
| --- | :---: | :---: |
| `Compile` a new expression | ~60 ms | ~0.5 ms |
| `Plan` (JIT) a compiled Program | ~65 ms | ~0.5 ms |
| `Eval`, steady state (floor) | ~50 ns | ~290 ns |
| Program artifact size | ~2.4 MB | ~6.5 KB |

One-time: `Engine` construction is ~70 ms per process. A cold process
reaches its first result in under a second. Each `Instance` owns its
Program's linear memory (two 64 KiB pages by default). `Engine` is
shared; bind one `Instance` per worker thread.

"Link" here refers to the runtime kernel — the ~40 translation units of
C (string ops, list/map kernels, arithmetic, the arena) that every
compiled expression calls into. A Program needs that kernel at eval
time, and the link mode decides where it lives:

- **Static (default):** the kernel is merged into each Program at
  compile time and the pair is optimized as one module. The artifact is
  self-contained (~2.4 MB) and nothing is resolved at run time — which
  is what buys the ~50 ns eval floor.
- **Dynamic:** the Program stays tiny (~6.5 KB — just your expression,
  its constants, and its `cel.abi` self-description) and *imports* the
  kernel from one shared `cel_runtime.wasm` per process, wired up at
  `Plan`. Compiles ~100× faster; the cross-module calls cost ~6× on the
  per-eval floor.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/img/artifacts-dark.svg">
  <img alt="Static link: one self-contained MB-scale program.wasm containing $eval, the merged runtime kernel, baked .rodata constants, and the cel.abi self-description. Dynamic link: a KB-scale program.wasm importing cel.memory and the cel.* helpers from one shared cel_runtime.wasm per process, linked at Plan." src="doc/img/artifacts-light.svg">
</picture>

Both modes share one codegen path and produce byte-identical
conformance results. Rule of thumb: static for compile-once /
eval-many serving; dynamic for compile-heavy services or many-program
fleets.

### Where it wins

Repetition to amortize, and work moved to compile time:

| Workload | cel-cpp | cel-wasm | speedup |
| --- | :---: | :---: | :---: |
| 1000-term int arithmetic chain over variables | 32.5 µs | 1.8 µs | 18× |
| 1000-term string-concat chain | 163 µs | 41 µs | 4.0× |
| constant list folded at compile time (`size([…1000])`) | 11.1 µs | 40 ns | 281× |
| membership in a 1000-element constant int list (`k in […]`, worst hit) | 15.3 µs | 813 ns | 19× |
| constant-map lookup, 256 entries, baked hash index (`m[k]`) | 11.2 µs | 91 ns | 124× |
| constant-map membership, 256 entries (`k in m`) | 17.2 µs | 101 ns | 171× |
| comprehension `[1…20].map(x, x * 2)` inside `size()` | 5.3 µs | 159 ns | 33× |
| complex regex `.matches()` | 12.4 µs | 154 ns | 81× † |

† Configuration, not codegen: cel-wasm caches the compiled RE2 pattern
per Instance; `cel-cpp`'s default runtime recompiles it per eval.

### Where it loses

| Workload | cel-cpp | cel-wasm | gap | cause |
| --- | :---: | :---: | :---: | --- |
| single proto map accessor (`m.str_to_i32["b"]`) | 133 ns | 175 ns | 1.3× slower | each read crosses one host trampoline; amortized once the expression does more than one accessor. |
| early-exit `in` over a 1000-string activation-bound list | 79 ns | 1.1 µs | 14× slower | a bound aggregate is copied into the sandbox every Eval; cel-cpp reads the host list by reference. |
| `contains()` on a 10 KB string | 312 ns | 487 ns | 1.6× slower | was 5.7× with a scalar scan; the wasm kernel now uses a 16-byte SIMD128 anchor scan, and the residual gap is mostly the fixed eval floor. |

Methodology and per-cell numbers:
[`benchmark/eval/results/`](benchmark/eval/results/) ·
[`benchmark/ANALYSIS.md`](benchmark/ANALYSIS.md). Reproduce with
`benchmark/eval/run.sh`.

## Conformance

Scored against the upstream CEL conformance corpus, in both link modes:

| | |
| --- | --- |
| Passing | **2035 / 2035 of applicable rows (100%)** — the 481 inapplicable rows are mostly the `dyn` feature, outside the static subset by design |
| Failing | **0** |
| Inapplicable | 481 — `dyn` (227), check-disabled rows (144), and not-yet-shipped scope (110: extensions 55, check-only 25, spec edges 18, type-env 12) |
| Whole corpus | 2035 / 2516 (80.9%) |

Per-fixture breakdown (autogenerated):
[`conformance/README.md`](conformance/README.md); reproduce with
`bazel run //conformance:run_conformance`.

Extensions implemented: `string_ext` (including `strings.format` —
172/216, 0 fails), `math_ext` (194/199, 0 fails), `network_ext` (69/69),
and `optionals` (26/70; the remainder require `dyn`).

## Custom functions — two trust models

A CEL expression sees only what the host hands it. Custom functions are
the escape hatch: the host registers functions, and rule authors call
them like built-ins.

```cel
// discount_pct is a custom function the host registered.
price - price * discount_pct(tier) / 100
```

One decision picks the flavor: does the function's code belong inside
your process?

- **`@host` — trusted.** Your own C++, in your address space. An
  in-memory cache lookup, a call into a library you already ship.
- **`@component` — untrusted.** Code you didn't write. A
  customer-authored scoring function, a partner's plugin. It runs in its
  own WebAssembly sandbox and can be hot-swapped at runtime.

| | `@host` — trusted C++ | `@component` — sandboxed wasm |
| --- | --- | --- |
| Runs | in your address space, as a C++ lambda | in an isolated wasm instance with its own linear memory |
| Author language | C++ | anything with a `wasm32-wasip2` toolchain (C++ today; TinyGo planned) |
| Can read host memory / syscall | yes — whatever the C++ does | no — cannot escape the sandbox or perform I/O |
| Update | re-link your binary | hot-swap: hand new bytes to `AddComponent` |
| Per-call cost | ~110 ns | ~450 ns |

```celfn
int  @host.length(string s);                           // trusted C++ path
bool @component.allow(string subject, string action);  // sandboxed wasm path
```

Both flavors share the `.celfn` IDL; the `@<prefix>` selects the
backend. A host function binds with the same declaration string the
compiler saw. The signature is validated at registration:

```cpp
engine.BindFunction("int @host.length(string s);",
                    [](absl::string_view s) -> absl::StatusOr<int64_t> {
                      return static_cast<int64_t>(s.size());
                    });
```

Loading not-yet-reviewed third-party code safely is the point. A stock
in-process CEL runtime cannot do it. Guides:
[host functions](doc/user-guide/writing-host-functions.md) ·
[component functions](doc/user-guide/writing-component-functions.md);
runnable: [`examples/04`](examples/04_host_functions.cc),
[`examples/09`](examples/09_component_functions.cc).

## Correctness

Enforced, not asserted:

- **The reference implementation is the oracle.** A test harness links
  the real `cel-cpp` parser, checker, and runtime. Expected results come
  from it, not from memory.
- **Differential fuzzing.** Generated expressions run on both engines;
  any divergence fails.
- **Fail closed.** `dyn` and unsupported shapes are rejected at compile
  time, never miscompiled. Every capability limit is pinned by a
  just-inside / just-past test pair (`e2e/limits_test.cc`).
- **Structural compiler/runtime split.** The compiler never links the
  evaluator, so `Program` stays portable and the compiler independently
  buildable.

Architecture and design docs: [`doc/README.md`](doc/README.md).

## Limitations

cel-wasm targets CEL's **static subset**: variables and intermediates
are typed and declared up front. If you need the full dynamic surface,
use [`cel-cpp`](https://github.com/google/cel-cpp) or
[`cel-go`](https://github.com/google/cel-go). cel-wasm trades that
surface for AOT speed, portability, and the sandbox.

Known gaps, each pinned by a skipped-with-reason test
(`e2e/known_bugs_test.cc`) or a backlog entry
([cleanup backlog](doc/implementation-plan/cleanup-backlog.md)):

- **No dynamic typing (`dyn`).** The 227-row conformance skip. A
  deliberate trade.
- **`@native` CEL-defined helper bodies** type-check, but body codegen
  has not shipped.
- **Very large constant literals are rejected at compile time** with a
  graceful `ResourceExhausted`, never a miscompile. Put large constant
  data in an activation-bound variable.
- Named edges: `cel.@block`, proto2 extension-field accessors,
  multi-byte-UTF-8 `size()`.
- **Bindings beyond C++ are designed, not built.** The `.wasm` +
  `cel.abi` carry everything a Go/TS/Rust shim needs.
- **Hardening continues.** Differential fuzzing and a sanitizer gate
  ship today. Allocator caps, CPU-time limits for component functions,
  and a release-versioning policy are still to come. See the
  [security model](doc/user-guide/security-model.md).

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

macOS (Apple Silicon) and Linux (arm64 / x86_64). Bazel fetches the
toolchain (wasi-sdk, the WebAssembly engine, Binaryen). You need
[`bazelisk`](https://github.com/bazelbuild/bazelisk) plus `clang`/`lld`:
`brew install llvm` on macOS, `apt install clang lld build-essential` on
Linux. Docker image: [`docker/Dockerfile`](docker/Dockerfile).

## Design choices

### Why not LLVM?

Considered, yes. LLVM would emit excellent native code, but it is a
heavyweight toolchain to embed and its compilation times are far
higher — the wrong trade for turning many small policy expressions
around quickly. And native code has no sandbox: the WebAssembly target
is what makes the compiled expression safe to run, with Binaryen and
Cranelift keeping the pipeline light and fast.

### Why wasmtime as the runtime?

There are many WebAssembly runtimes: [V8](https://v8.dev/) (the JIT
inside Chrome and Node), [Wasmer](https://wasmer.io/),
[WAMR](https://github.com/bytecodealliance/wasm-micro-runtime),
[wazero](https://wazero.dev/), and more.
[wasmtime](https://wasmtime.dev/) fits this job best: a standalone
runtime with a mature C API, written in memory-safe Rust — the right
foundation for a security boundary — and its Cranelift backend compiles
fast while emitting code that is plenty good for expression-sized
programs. Cranelift trades a few percent of peak code quality for much
faster compilation: the same trade this project makes everywhere.

## Why I built this

I'm a compiler engineer on a small crusade. The industry keeps
embedding slow, interpreted mini-languages in production hot paths,
and I think we can do better than that — this project is the
compiler-shaped counterargument: bake the hash table at compile time,
vectorize the scan, let the JIT emit real machine code, and publish
the benchmark tables win or lose. CEL is the perfect vehicle — it's
non-Turing-complete on purpose, which is most of the way to being
*provable* (give us Lean proofs one day and this thing is formally
verifiable, too). And honestly, building it solo was tractable because
Claude turned the unglamorous nine-tenths — test matrices, conformance
chasing, bench harnesses — into the easy part.

## Author

cel-wasm is created and maintained by **Augustine Mathew**
([augustine.mathew@gmail.com](mailto:augustine.mathew@gmail.com) ·
[LinkedIn](https://www.linkedin.com/in/augustine-m/)). It is an
independent project, not affiliated with or endorsed by any employer.
See [NOTICE](NOTICE) and [AUTHORS](AUTHORS).

Development is **heavily AI-assisted**: the compiler is designed and
pair-programmed with Claude (Anthropic), with every change gated by the
project's conformance suite, differential fuzzing against cel-cpp, and
the benchmark harness.

**Provenance note:** this repository began as a fork of
[google/cel-spec](https://github.com/google/cel-spec) — the CEL language
definition and the conformance corpus are its heritage. The git history
and GitHub contributor list therefore include upstream cel-spec
contributors; the cel-wasm compiler, runtime, and tooling are the work
of the author above.

*The internal C++ namespace is `celwasm::`; the project name is
`cel-wasm`, matching `cel-cpp` / `cel-go`.*

Released under the [Apache License 2.0](LICENSE).
