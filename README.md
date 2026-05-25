# celwasmc — a CEL → WebAssembly AOT compiler

`celwasmc` is an **ahead-of-time compiler** that turns a type-checked
[Common Expression Language](https://github.com/google/cel-spec) (CEL)
expression into a standalone WebAssembly module, plus a C++/wasmtime evaluator
that runs it. The repo began as a fork of the cel-spec language-spec repo; the
compiler is now the centre of it, with the cel-spec heritage (the `.proto` type
definitions and the `.textproto` conformance corpus) corralled as inherited
contract.

The parser and type-checker are reused from
[cel-cpp](https://github.com/google/cel-cpp) (vendored under `third_party/`);
this repo owns everything downstream of a checked AST — IR, codegen, the wasm
runtime, and the host evaluator.

## Layout

Source is organised by **lifecycle role** at the top level (the layout cel-cpp
itself uses — no `api/` umbrella, public/private split by `internal/` +
Bazel `visibility`):

| Dir | Role |
|---|---|
| `compiler/` | **Compile-time.** CEL source → `Program` (wasm bytes + `cel.abi`). `frontend/` (parse + check), `ir/` (typed AST + annotations), `codegen/` (Binaryen lowering), `celfn/` (function library), `internal/` (the private pipeline facade); public face `compiler.{h,cc}` + `program.h`. Stays wasm-targetable (no eval/wasmtime dep) so `compiler.wasm` is reachable. |
| `eval/` | **Eval-time.** `Program` + `Activation` → `Value` (C++/wasmtime evaluator). Public leaves `engine/instance/activation/value/error/attribute`; `host/` + `internal/` are private. |
| `shared/` | `CelType` — the type vocabulary both compile and eval speak. (Named `shared/`, not `common/`, to avoid colliding with vendored cel-cpp's `common/`.) |
| `abi/` | The `cel.abi` wire contract (emit *and* parse). |
| `runtime/` | `cel_runtime.c` → `cel_runtime.wasm` (language-agnostic kernel). |
| `tools/` | The `cel` CLI (eval/check/compile) and `wat_runner`. |
| `conformance/` `e2e/` `bench/` `testdata/` | Conformance harness, integration tests, microbenches, shared fixtures. |
| `spec/` | cel-spec heritage — the `.textproto` conformance corpus under `spec/tests/`. |
| `proto/` | cel-spec proto type definitions (stays at repo root for now; the move under `spec/` rides with a future module rename). |
| `doc/` | Design docs, language definition, the implementation plan. |
| `third_party/` | External-dependency integration (cel-cpp fetch, Binaryen + wasmtime glue, wasi-sdk toolchain, patches). |

`compiler/` and `eval/` both depend on `shared/`; neither depends on the other.
A future `bindings/` (TS/Go) embeds the wasm artifacts rather than
reimplementing the pipeline.

## Build & test

`celwasmc` builds on **macOS (Apple Silicon)** and **Linux (arm64 / x86_64)**
with the same `bazel` invocations. The wasm cross-compile toolchain (wasi-sdk)
and the wasmtime runtime are fetched by Bazel automatically — only the host C++
toolchain and a few CLI tools are system dependencies.

### Prerequisites

| | Install |
|---|---|
| **Bazel** (all platforms) | Pinned to `7.3.2` via `.bazelversion`; install [`bazelisk`](https://github.com/bazelbuild/bazelisk) and call it as `bazel`. |
| **macOS** | `brew install llvm` (provides `clang` + `lld`). |
| **Linux** | `clang`, `lld`, `tzdata-legacy`, `build-essential`, `python3`, `zip`, `unzip`. Or use the [Docker image](#docker-linux). |

Why those exact Linux packages: the runtime C uses clang's
`__attribute__((musttail))` and clang's `_Static_assert` handling (gcc rejects
both); GNU `gold` crashes linking a binary this large, so `.bazelrc` pins
`-fuse-ld=lld`; and named-timezone expressions need the IANA database, whose
legacy aliases live in `tzdata-legacy` on Ubuntu 24.04.

### Commands

> **Use the project-package set, never `//...`.** The vendored
> `third_party/cel-cpp/tools/testdata/BUILD` references an undeclared repo, so
> `bazel … //...` fails to *load*. "Build/test everything" means this explicit
> set (`$PROJ`):
>
> ```
> //compiler/... //eval/... //shared/... //abi/... //runtime/... \
> //tools/... //conformance/... //e2e/... //bench/... //testdata/... //spec/...
> ```

```bash
# 1. Fetch the vendored cel-cpp parser/type-checker (only its pinned SHA is
#    committed). Required before the first build.
third_party/fetch_cel_cpp.sh

# 2. Build everything (host tools + the wasm runtime).
bazel build //compiler/... //eval/... //shared/... //abi/... //runtime/... \
            //tools/... //conformance/... //e2e/... //bench/... //testdata/... //spec/...

# 3. Run the test suite (same package set).
bazel test  //compiler/... //eval/... //shared/... //abi/... //runtime/... \
            //tools/... //conformance/... //e2e/... //bench/... //testdata/... //spec/...
```

### Run the CLI

```bash
bazel run //tools/cel:cel -- eval '1 + 2 + 3'                                # => 6
bazel run //tools/cel:cel -- eval 'a * b' --var a:int=6 --var b:int=7        # => 42
bazel run //tools/cel:cel -- check 'a * b + 1' --var a:int --var b:int       # => OK
bazel run //tools/cel:cel -- compile '1 + 1' --output /tmp/out.wasm          # emit wasm
```

### Conformance

The CEL conformance suite is the canonical "does this behave per spec" gate.
The corpus lives at `spec/tests/`:

```bash
bazel run //conformance:run_conformance     # prints `summary: total=… pass=… …`
scripts/check_conformance_monotonic.sh       # asserts pass count >= baseline
```

### Docker (Linux)

A ready-made Linux build environment is at
[`docker/Dockerfile`](docker/Dockerfile):

```bash
docker build -t celwasmc-linux -f docker/Dockerfile .
docker run --rm -v "$PWD":/src -w /src celwasmc-linux \
  bash -c 'third_party/fetch_cel_cpp.sh && bazel test //compiler/... //eval/... //runtime/...'
```

## More

  - **Contributor workflow** (lint, formatting, the per-feature checklist,
    visibility regime): [`CLAUDE.md`](CLAUDE.md) and
    [`doc/contributing.md`](doc/contributing.md).
  - **Design + plan:** [`doc/implementation-plan/`](doc/implementation-plan/)
    (architecture, milestone plans, testing coverage grid) and
    [`doc/langdef.md`](doc/langdef.md) (the CEL semantics we honour).

Released under the [Apache License](LICENSE).
