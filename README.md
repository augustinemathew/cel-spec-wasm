# celwasmc — compile CEL to WebAssembly, run it anywhere

**Write a [CEL](https://github.com/google/cel-spec) expression once, compile it
to a tiny standalone WebAssembly module, and run it in any language that can
load wasm — at native speed, in a sandbox, with no per-host interpreter to
reimplement or keep in sync.**

[Common Expression Language](https://github.com/google/cel-spec) is the small,
safe expression language behind Kubernetes admission policy, Envoy/Istio
authorization, IAM conditions, and Firebase rules. Today every host that wants
to run CEL embeds a full language implementation — a parser, type-checker, and
evaluator — in *its own* language, and those implementations drift. `celwasmc`
takes a different path:

- **Compile, don't interpret.** A CEL expression is type-checked once and
  lowered ahead-of-time to a self-contained `.wasm` module (plus a small
  `cel.abi` describing its inputs). There is no AST walk at evaluation time —
  the logic *is* compiled wasm.
- **One artifact, every language.** The same compiled module runs unchanged
  wherever wasm runs. Hosts don't reimplement CEL; they embed a thin runtime
  shim and call in. No semantic drift between languages, because there is only
  one implementation of the semantics — the compiler.
- **Fast and sandboxed.** Compiled wasm evaluates far faster than a tree-walk
  interpreter, inside wasm's memory-safe sandbox, with bounded, predictable
  resource use — ideal for hot paths and untrusted policy.
- **Spec-faithful.** The parser and type-checker are reused from
  [cel-cpp](https://github.com/google/cel-cpp), and behavior is gated
  byte-for-byte against the upstream CEL conformance corpus.

## Bindings — coming soon

The compiler emits a portable artifact, and host bindings *embed* it rather
than reimplement CEL — so the roadmap is thin runtime shims, one per ecosystem:

| Language | Status |
|---|---|
| **C++** | first-class today (the reference host: `eval/`, wasmtime) |
| **Go** | planned — embed the compiled module via a wazero/wasmtime shim |
| **TypeScript** | planned — run in the browser or Node on the native wasm engine |

Each binding loads the *same* `.wasm` + `cel.abi`, binds inputs, and reads back
a result. Adding a language is writing a small marshalling shim — never another
CEL implementation. (The compiler stays wasm-targetable with no evaluator
dependency precisely so the pipeline itself can also ship as `compiler.wasm`.)

## Getting started

```bash
# 1. Fetch the vendored cel-cpp parser/type-checker (only its pinned SHA is
#    committed) — required once before the first build.
third_party/fetch_cel_cpp.sh

# 2. Evaluate a CEL expression through the full compile → wasm → run pipeline.
bazel run //tools/cel:cel -- eval '1 + 2 + 3'                          # => 6
bazel run //tools/cel:cel -- eval 'a * b' --var a:int=6 --var b:int=7  # => 42
```

That's the whole loop: the `cel` CLI parses + type-checks the expression,
compiles it to a wasm module, instantiates it under wasmtime, and prints the
result.

### Prerequisites

`celwasmc` builds on **macOS (Apple Silicon)** and **Linux (arm64 / x86_64)**
with the same `bazel` invocations. The wasm cross-compile toolchain (wasi-sdk)
and the wasmtime runtime are fetched by Bazel automatically.

| | Install |
|---|---|
| **Bazel** (all platforms) | Pinned via `.bazelversion`; install [`bazelisk`](https://github.com/bazelbuild/bazelisk) and call it as `bazel`. |
| **macOS** | `brew install llvm` (provides `clang` + `lld`). |
| **Linux** | `clang`, `lld`, `tzdata-legacy`, `build-essential`, `python3`, `zip`, `unzip` — or use the [Docker image](#docker-linux). |

## Build, test, compile

```bash
# Build everything (host tools + the wasm runtime).
bazel build //...

# Run the whole test suite.
bazel test //...
```

`//...` works directly — `.bazelignore` excludes the vendored cel-cpp module so
the main repo's target expansion doesn't trip on it. (`$PROJ`, the explicit
role-package set listed in [`CLAUDE.md`](CLAUDE.md), is still available when you
want only the first-party packages without the `doc/**` probe targets.)

### Compile an expression to wasm

```bash
# Emit the wasm module (+ embedded cel.abi) for an expression.
bazel run //tools/cel:cel -- compile 'a * b + 1' \
    --var a:int --var b:int --output /tmp/out.wasm

# Type-check only (no codegen).
bazel run //tools/cel:cel -- check 'a * b + 1' --var a:int --var b:int   # => OK
```

The `cel` CLI has three subcommands — `eval`, `check`, `compile` — sharing these
flags:

| Flag | Meaning |
|---|---|
| `--var name:Type[=value]` | declare (and optionally bind) a free variable; repeatable |
| `--proto PATH` / `--descriptor_set PATH` | message types from a `.proto` source or a `FileDescriptorSet` |
| `--container PKG` | name-resolution container |
| `--format textproto\|json\|cel` | (`eval`) result rendering; repeatable |
| `--O 0..3` | Binaryen optimize level |
| `--output PATH` | (`compile`) wasm output path |

Run `bazel run //tools/cel:cel -- --help` for the full list.

### Conformance

The CEL conformance suite is the canonical "does this behave per spec" gate; the
corpus lives at `spec/tests/`.

```bash
bazel run //conformance:run_conformance     # prints `summary: total=… pass=… …`
scripts/check_conformance_monotonic.sh        # asserts pass count >= baseline
```

### Docker (Linux)

```bash
docker build -t celwasmc-linux -f docker/Dockerfile .
docker run --rm -v "$PWD":/src -w /src celwasmc-linux \
  bash -c 'third_party/fetch_cel_cpp.sh && bazel test //...'
```

## Documentation

**Start with the [documentation index](doc/README.md)** — the navigable map of
the docs tree. The most useful entry points:

  - **[`doc/intro.md`](doc/intro.md)** — CEL usage by example (the user guide).
  - **[`doc/langdef.md`](doc/langdef.md)** — the CEL language reference (the
    semantics this compiler honours).
  - **[`doc/compiler-overview.md`](doc/compiler-overview.md)** — how the
    compiler pipeline fits together.
  - **[`doc/implementation-plan/`](doc/implementation-plan/)** — architecture,
    milestone plans, and the testing-coverage grid.
  - **Contributing** (lint, formatting, the per-feature checklist, the
    visibility regime): [`CLAUDE.md`](CLAUDE.md) and
    [`doc/contributing.md`](doc/contributing.md).

## Layout

Source is organised by **lifecycle role** at the top level (the layout cel-cpp
itself uses — public/private split by `internal/` + Bazel `visibility`):

| Dir | Role |
|---|---|
| `compiler/` | **Compile-time.** CEL source → `Program` (wasm bytes + `cel.abi`): `frontend/` (parse + check), `ir/` (typed AST), `codegen/` (Binaryen lowering), `celfn/` (function library). Stays wasm-targetable (no eval/wasmtime dep). |
| `eval/` | **Eval-time.** `Program` + `Activation` → `Value` (the C++/wasmtime evaluator — the reference host). |
| `shared/` | `CelType` — the type vocabulary both compile and eval speak. |
| `abi/` | The `cel.abi` wire contract (emit *and* parse) — the seam a binding marshals against. |
| `runtime/` | `cel_runtime.c` → `cel_runtime.wasm` (language-agnostic kernel). |
| `tools/` | The `cel` CLI (`eval`/`check`/`compile`) and `wat_runner`. |
| `conformance/` `e2e/` `bench/` `testdata/` | Conformance harness, integration tests, microbenches, shared fixtures. |
| `spec/` | cel-spec heritage — the `.textproto` conformance corpus under `spec/tests/`. |
| `doc/` | Design docs, language definition, the implementation plan. |
| `third_party/` | External-dependency integration (cel-cpp fetch, Binaryen + wasmtime glue, wasi-sdk toolchain, patches). |

`compiler/` and `eval/` both depend on `shared/`; neither depends on the other.
A future `bindings/` (Go, C++, TypeScript) embeds the wasm artifacts rather than
reimplementing the pipeline.

Released under the [Apache License](LICENSE).
