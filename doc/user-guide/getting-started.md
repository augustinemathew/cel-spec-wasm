# Getting started

From clone to evaluating CEL in about two minutes, then your first
C++ embed. Every snippet on this page is backed by a buildable target
under [`examples/`](https://github.com/augustinemathew/cel-wasm/tree/master/examples) that CI runs — nothing here can
silently rot.

## 1. Prerequisites

- [`bazelisk`](https://github.com/bazelbuild/bazelisk) (or a recent Bazel)
- `clang` + `lld` — `brew install llvm` on macOS,
  `apt install clang lld build-essential` on Linux

macOS (Apple Silicon) and Linux (arm64 / x86_64) are supported. The
rest of the toolchain — wasi-sdk, wasmtime, Binaryen — is fetched by
Bazel. Prefer Docker? Use [`docker/Dockerfile`](https://github.com/augustinemathew/cel-wasm/blob/master/docker/Dockerfile).

## 2. Clone and fetch the front end

```bash
git clone https://github.com/augustinemathew/cel-wasm.git
cd cel-wasm
third_party/fetch_cel_cpp.sh   # one-time: the vendored parser/type-checker
```

## 3. First eval — the CLI

```bash
bazel build //tools/cel:cel   # once; then invoke the binary directly

bazel-bin/tools/cel/cel eval '1 + 2 + 3'
# => 6

bazel-bin/tools/cel/cel eval 'a * b' --var a:int=6 --var b:int=7
# => 42

bazel-bin/tools/cel/cel eval "[1, 3, 5, 7].exists(x, x > 5)"
# => true
```

The first build compiles the vendored cel-cpp front end (several
minutes, once). After that, the loop is seconds.

To produce a portable artifact, use `compile`:

```bash
bazel-bin/tools/cel/cel compile 'a * b + 1' \
    --var a:int --var b:int --output /tmp/expr.wasm
```

## 4. First embed — C++

The object model is five nouns, in a straight line:

```
Compiler ──Compile(source)──► Program ──Engine::Plan──► Instance ──Eval(Activation)──► Value
(declares variables,          (portable               (JIT'd native               (your result)
 owns no engine state)         wasm bytes)             code, per-thread)
```

- **`Compiler`** — pure compile time. Declare variable types up front;
  build once, compile many expressions.
- **`Program`** — pure bytes. Serialize it, cache it, ship it to
  another process or machine.
- **`Engine`** — the runtime machinery. Build **once per process**;
  `Plan()` JITs a Program to native code and is safe to call from many
  threads.
- **`Instance`** — the live evaluator. **One per worker thread**; reuse
  it across evals (its arena resets automatically).

```cpp
#include "compiler/compiler.h"
#include "eval/engine.h"

auto builder = celwasm::Compiler::NewBuilder();
builder.DeclareVariable("age", celwasm::CelType::Int())
    .DeclareVariable("country", celwasm::CelType::String());
auto compiler = std::move(builder).Build().value();

auto program = compiler.Compile("age >= 18 && country in ['US', 'CA']").value();

auto engine   = celwasm::Engine::NewBuilder().Build().value();
auto instance = engine.Plan(program).value();

celwasm::Activation act;
act.Bind("age", celwasm::Value::Int(25))
   .Bind("country", celwasm::Value::String("US"));
bool allowed = instance.Eval(act)->AsBool().value();   // => true
```

Run the full version:

```bash
bazel run //examples:02_variables
```

Your BUILD dependencies are the public API targets only:

```python
deps = [
    "//compiler:compiler",
    "//compiler:program",
    "//eval:activation",
    "//eval:engine",
    "//eval:instance",
    "//eval:value",
    "//shared:type",
]
```

(Production code checks every `absl::StatusOr` instead of calling
`.value()` — see
[`examples/07_error_handling.cc`](https://github.com/augustinemathew/cel-wasm/blob/master/examples/07_error_handling.cc)
for the error-handling layers.)

## 5. Static vs dynamic linking — the one choice that matters

Every compiled expression calls into a runtime kernel (string ops,
list/map kernels, arithmetic, the arena). `link_mode` decides where
that kernel lives:

- **`kStatic` (default)** merges the kernel into each Program at
  compile time. Self-contained ~2.4 MB artifact, ~60 ms compile,
  fastest eval (~50 ns floor). Pick this when you compile once and
  evaluate many times — the common serving shape.
- **`kDynamic`** keeps the Program tiny (~6.5 KB) and links it to one
  shared `cel_runtime.wasm` per process at `Plan`. ~0.5 ms compile,
  ~290 ns eval floor. Pick this when you compile constantly or cache
  thousands of Programs.

![Compilation artifacts under each link mode](../img/artifacts-light.svg#only-light)
![Compilation artifacts under each link mode](../img/artifacts-dark.svg#only-dark)

You never handle the difference at eval time — `Engine::Plan` detects
the mode from the Program's imports and wires either shape. Both modes
produce byte-identical results on the conformance corpus.

## 6. Tuning a compile — `CompilerOptions`

Declarations live on the `Builder`; `CompilerOptions` tunes how one
expression is lowered:

| Option | Default | Notes |
| --- | --- | --- |
| `link_mode` | `kStatic` | `kStatic`: runtime merged in — self-contained ~2.4 MB Program, fastest eval, ~60 ms compile. `kDynamic`: ~6.5 KB Program importing a shared runtime — ~0.5 ms compile, better for many cached expressions. `Engine::Plan` handles both transparently. |
| `optimize_level` | `0` | Binaryen `-O0..3` on the emitted wasm. Use `2` in production (compile cost ~2-3×, eval up to 2× faster on long bodies); `0` when compile latency dominates. |
| `mem_size_bytes` | 128 KiB | Linear memory (the per-eval arena lives here). Raise it for heavy string/list construction within a single eval. |
| `container` | `""` | CEL namespace container for name resolution, as in cel-go. |

```cpp
celwasm::CompilerOptions opts;
opts.optimize_level = 2;
auto program = compiler.Compile("a * b + 1", opts).value();
```

## 7. Where to go next

| You want to… | Go to |
| --- | --- |
| See every core feature as a ~60-line runnable program | [`examples/`](https://github.com/augustinemathew/cel-wasm/tree/master/examples) |
| Ship the compiled `.wasm` and evaluate it elsewhere | [`examples/03_compile_once_run_anywhere.cc`](https://github.com/augustinemathew/cel-wasm/blob/master/examples/03_compile_once_run_anywhere.cc) |
| Add your own functions to CEL (trusted C++) | [Writing host functions](writing-host-functions.md) |
| Add **sandboxed** functions (untrusted plugins) | [Writing component functions](writing-component-functions.md) |
| Understand the whole embedder API | [User guide](index.md) |
| Quick answers (dyn? thread-safety? sizes? speed?) | [FAQ](faq.md) |
| The security story, precisely stated | [Security model](security-model.md) |
