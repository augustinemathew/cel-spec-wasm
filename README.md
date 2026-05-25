# Common Expression Language

The Common Expression Language (CEL) implements common semantics for expression
evaluation, enabling different applications to more easily interoperate.

> **This repository hosts `celwasmc`** — an ahead-of-time compiler that turns a
> type-checked CEL expression into a standalone WebAssembly module. The compiler
> lives under [`compiler_v2/`](compiler_v2/); the cel-spec artefacts (protos,
> conformance tests, `doc/langdef.md`) are upstream and unchanged. See
> [Getting started](#getting-started) below.

## Getting started

`celwasmc` builds on **macOS (Apple Silicon)** and **Linux (arm64 / x86_64)**
with the same `bazel` invocations. The wasm cross-compile toolchain (wasi-sdk)
and the wasmtime runtime are fetched by Bazel automatically — only the host C++
toolchain and a few CLI tools are system dependencies.

### Prerequisites

| | Install |
|---|---|
| **Bazel** (all platforms) | Pinned to `7.3.2` via `.bazelversion`; install [`bazelisk`](https://github.com/bazelbuild/bazelisk) and call it as `bazel`. |
| **macOS** | `brew install llvm` (provides `clang` + `lld`). |
| **Linux** | `clang`, `lld`, `tzdata-legacy`, `build-essential`, `python3`, `zip`, `unzip` (e.g. `apt-get install …`). Or skip all of this and use the [Docker image](#docker-linux). |

Why those exact Linux packages: the runtime C uses clang's `__attribute__((musttail))`
and clang's `_Static_assert` handling (gcc rejects both), so the build uses
`clang`; GNU `gold` crashes linking a binary this large, so `.bazelrc` pins
`-fuse-ld=lld`; and named-timezone expressions (`timestamp(...).getDayOfMonth('US/Central')`)
need the IANA database, whose legacy aliases live in `tzdata-legacy` on Ubuntu 24.04.

### Build & test

```bash
# 1. Fetch the vendored cel-cpp parser/type-checker (only its pinned SHA is
#    committed — this clones it at that SHA). Required before the first build.
third_party/fetch_cel_cpp.sh

# 2. Build everything (host tools + the wasm runtime).
bazel build //compiler_v2/...

# 3. Run the test suite.
bazel test //compiler_v2/...
```

### Run the CLI

```bash
bazel run //compiler_v2/tools/cel:cel -- eval '1 + 2 + 3'                 # => 6
bazel run //compiler_v2/tools/cel:cel -- eval 'a * b' --var a:int=6 --var b:int=7   # => 42
bazel run //compiler_v2/tools/cel:cel -- check 'a * b + 1' --var a:int --var b:int  # => OK
bazel run //compiler_v2/tools/cel:cel -- compile '1 + 1' --output /tmp/out.wasm     # emit wasm
```

### Conformance

The CEL conformance suite is the canonical "does this behave per spec" gate:

```bash
bazel run //compiler_v2/conformance:run_conformance   # prints `summary: total=… pass=… …`
scripts/check_conformance_monotonic.sh                # asserts pass count >= baseline
```

### Docker (Linux)

A ready-made Linux build environment with the full toolset is at
[`docker/Dockerfile`](docker/Dockerfile):

```bash
docker build -t celwasmc-linux -f docker/Dockerfile .
docker run --rm -v "$PWD":/src -w /src celwasmc-linux \
  bash -c 'third_party/fetch_cel_cpp.sh && bazel test //compiler_v2/...'
```

For the full contributor workflow (lint, formatting, the per-feature checklist),
see [`doc/contributing.md`](doc/contributing.md) and [`CLAUDE.md`](CLAUDE.md).

Key Applications

*   Security policy: organizations have complex infrastructure and need common
    tooling to reason about the system as a whole
*   Protocols: expressions are a useful data type and require interoperability
    across programming languages and platforms.


Guiding philosophy:

1.  Keep it small & fast.
    *   CEL evaluates in linear time, is mutation free, and not Turing-complete.
        This limitation is a feature of the language design, which allows the
        implementation to evaluate orders of magnitude faster than equivalently
        sandboxed JavaScript.
2.  Make it extensible.
    *   CEL is designed to be embedded in applications, and allows for
        extensibility via its context which allows for functions and data to be
        provided by the software that embeds it.
3.  Developer-friendly.
    *   The language is approachable to developers. The initial spec was based
        on the experience of developing Firebase Rules and usability testing
        many prior iterations.
    *   The library itself and accompanying toolings should be easy to adopt by
        teams that seek to integrate CEL into their platforms.

The required components of a system that supports CEL are:

*   The textual representation of an expression as written by a developer. It is
    of similar syntax to expressions in C/C++/Java/JavaScript
*   A representation of the program's abstract syntax tree (AST).
*   A compiler library that converts the textual representation to the binary
    representation. This can be done ahead of time (in the control plane) or
    just before evaluation (in the data plane).
*   A context containing one or more typed variables, often protobuf messages.
    Most use-cases will use `attribute_context.proto`
*   An evaluator library that takes the binary format in the context and
    produces a result, usually a Boolean.

For use cases which require persistence or cross-process communcation, it is
highly recommended to serialize the type-checked expression as a protocol
buffer. The CEL team will maintains canonical protocol buffers for ASTs and
will keep these versions identical and wire-compatible in perpetuity:

*  [CEL canonical](https://github.com/google/cel-spec/tree/master/proto/cel/expr)
*  [CEL v1alpha1](https://github.com/googleapis/googleapis/tree/master/google/api/expr/v1alpha1)


Example of boolean conditions and object construction:

``` c
// Condition
account.balance >= transaction.withdrawal
    || (account.overdraftProtection
    && account.overdraftLimit >= transaction.withdrawal  - account.balance)

// Object construction
common.GeoPoint{ latitude: 10.0, longitude: -5.5 }
```

For more detail, see:

*   [Introduction](doc/intro.md)
*   [Language Definition](doc/langdef.md)

Released under the [Apache License](LICENSE).
