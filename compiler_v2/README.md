# `compiler_v2/` — CEL → WebAssembly AOT compiler

The compiler that turns a CEL source expression into a self-contained
wasm module the host instantiates and runs through wasmtime.  This
README is the orientation page — start here if you're new to the
codebase or you need to know which knob controls what.

For the full design, see `doc/implementation-plan/rewrite/design.md`
and `doc/implementation-plan/rewrite/cel-host-surface.md`.  For per-
component test coverage and the milestone closeout discipline, see
`doc/implementation-plan/per-component-test-coverage.md`.

> The repository-root `README.md` is the upstream cel-spec *language*
> README and is intentionally left untouched; this file is the
> getting-started page for the compiler.

## Getting started

### Platform support (read this first)

| Host                              | Build + non-wasm tests | wasm-execution tests¹ | Lint (clang-tidy) |
|-----------------------------------|:----------------------:|:---------------------:|:-----------------:|
| **macOS, Apple Silicon (arm64)**  | ✅ supported            | ✅ supported           | ✅ brew `llvm`     |
| macOS, Intel (x86_64)             | ⚠️ untested             | ❌ not wired²          | ✅ brew `llvm`     |
| Linux (arm64 / x86_64)            | ⚠️ untested             | ❌ not wired²          | ✅ distro `llvm`   |

¹ "wasm-execution tests" = anything that instantiates the compiled
module under wasmtime: `tools/wat_runner`, the `runtime` / `host` wasm
tests, the `e2e` suites, and the conformance harness.

² **The wasm toolchain is currently pinned to `darwin_arm64`.** The
`@wasmtime_darwin_arm64` dep is hard-referenced across the wasm targets
(`tools/wat_runner`, `runtime`, `host`, probes), and the
`//third_party/wasi_sdk` cc-toolchain + aliases default to
`@wasi_sdk_darwin_arm64`.  The Linux/Intel **wasi-sdk archives are
already declared** in `MODULE.bazel`
(`wasi_sdk_{linux_arm64,linux_x86_64,darwin_x86_64}`), so generalising
is a finite job, but it has not been done — today the only
fully-exercised configuration is **Apple Silicon macOS**.  See "Porting
to another host" at the bottom of this file.

### Prerequisites

| Tool                               | Why                                          | Version |
|------------------------------------|----------------------------------------------|---------|
| `bazel`                            | Build / test driver (install via `bazelisk`).| pinned in `.bazelversion` (currently 7.3.2) |
| LLVM `clang-tidy` / `clang-format` | Lint + format gate (Google C++ style).       | a modern LLVM (≥ 17) |
| `python3`                          | Fallback `compile_commands.json` generator.  | system Python 3 |

Bazel fetches everything else itself — cel-cpp, abseil, protobuf,
**binaryen** (built from source), the **wasi-sdk** cross-compile
sysroot, and **wasmtime**.  You do **not** install a wasm toolchain by
hand.  The first build is long: it compiles cel-cpp + binaryen from
source (see `doc/implementation-plan/dev-loop-performance.md`).

#### macOS (Apple Silicon) — the supported path

```bash
brew install bazelisk llvm
# scripts/lint.sh auto-prepends this when present; add to your shell rc
# so the same clang-tidy is on PATH for command-line use:
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"

bazel test //compiler_v2/...        # builds + runs the full suite
```

#### Linux — host C++ build only (wasm path not yet wired)

```bash
# bazel via bazelisk
curl -L https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
    -o /usr/local/bin/bazel && chmod +x /usr/local/bin/bazel
# LLVM for the lint gate (Debian/Ubuntu; or use https://apt.llvm.org)
sudo apt-get install -y clang-tidy clang-format
```

The frontend / IR / codegen / host-side **unit** tests build and run
with the autodetected system toolchain.  Targets that link
`@wasmtime_darwin_arm64` or cross-compile through the wasi-sdk fail to
configure on Linux until the toolchain is generalised — so
`bazel test //compiler_v2/...` will not pass wholesale on Linux today.

Full install detail (compile-db, the PCH gotcha, lint flow) lives in
`doc/contributing.md`.

## Layout

```
compiler_v2/
  api/         — public C++ surface: Compiler, Engine, Instance,
                  Activation, Value, CelType.  This is what hosts
                  link against.
  frontend/    — parse_and_check.  Wraps cel-cpp's parser + checker;
                  runs RejectDyn; emits TypedAst.
  ir/          — typed_ast + annotations.  Stable mid-layer between
                  the checker and codegen.
  codegen/     — resolve_pass, layout_pass, expr_lower, module,
                  overload_table, static_memory_builder, slot_alloc.
                  Lowers TypedAst → Binaryen IR → wasm bytes.
  abi/         — cel.abi custom-section emitter / parser; pinned wire
                  format the host loader reads to discover rodata /
                  workspace / arena offsets.
  runtime/     — cel_runtime.c (split into cel_arith.c, cel_compare.c,
                  cel_3vl.c, cel_convert.c, cel_string_ops.c, … per
                  doc/implementation-plan/rewrite/cel-runtime-c-split-
                  plan.md).  Cross-compiled to cel_runtime.wasm; also
                  linked natively for unit tests and host trampoline
                  callers.
  host/        — host-side trampolines that bridge cel_host imports
                  to user code (cel_log, …).
  conformance/ — harness that runs the cel-spec conformance corpus
                  against the pipeline.  See conformance/README.md.
  e2e/         — full-pipeline integration tests, one per milestone
                  (m2_test, m4_test, m5_test, m7_test, m9_test,
                  m10_test) plus optimize_test (manual-tagged opt-
                  level validation gate).
  bench/       — Google Benchmark microbenches (kernel + pipeline).
                  See bench/README.md.
  tools/       — CLI utilities.  `tools/cel/` is the `cel` command-
                  line driver (subcommands eval / check / compile,
                  --var / --proto / --descriptor_set / --format; see
                  tools/cel/README.md).  `tools/wat_runner/` assembles
                  + runs WAT traces.
  compile.{h,cc} — internal pipeline facade (frontend → codegen).
                  Public callers go through api/compiler.h instead.
```

## Lifecycle

The public surface mirrors three real caching boundaries — design
each call site around them, not around `Compile()` alone.

```
   user code
       │
       │  Compiler::Builder      ◄── one-time per declared-variable
       │   .DeclareVariable(...)     environment.  Pure data; copyable.
       │   .Build() → Compiler
       │
       │  Compiler.Compile(src, opts)
       │   → Program                 ◄── per CEL expression.  Pure
       │                                 wasm bytes + cel.abi metadata.
       │
       │  Engine::Builder            ◄── one-time per process / tenant.
       │   .Build() → Engine             Owns wasm_engine_t + parsed
       │                                 cel_runtime.wasm module.
       │
       │  Engine.Plan(program)
       │   → Instance                ◄── per request / Eval-batch.
       │                                 wasmtime store + memory +
       │                                 bound exports.
       │
       │  Instance.Eval(activation)
       │   → Value                   ◄── many per Instance; arena_reset
       │                                 rewinds the arena at the top
       │                                 of each call.
```

The benches under `bench/` are structured around these boundaries —
re-read `bench/README.md` if you need a perf number anchored to a
specific layer.

## Quickstart

End-to-end: compile a CEL expression, plan it, evaluate it.  Every
snippet below is real code lifted from `compiler_v2/e2e/`; copy-paste
into a `cc_binary` whose deps include `//compiler_v2/api:compiler`,
`//compiler_v2/api:engine`, `//compiler_v2/api:activation`,
`//compiler_v2/api:value`, `//compiler_v2/api:type`, and
`@com_google_absl//absl/log:absl_check`.

### 1. Scalar literal — the simplest "hello, world"

```cpp
#include "absl/log/absl_check.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/value.h"

int main() {
  // Build a Compiler with no declared variables.
  cel::Compiler::Builder cb;
  auto compiler = std::move(cb).Build();
  ABSL_CHECK_OK(compiler);

  // Compile a CEL expression to a Program (wasm bytes + ABI).
  auto program = compiler->Compile("1 + 2 + 3");
  ABSL_CHECK_OK(program);

  // Stand up a process-wide Engine (owns wasm_engine_t + the parsed
  // cel_runtime.wasm module).
  auto engine = cel::Engine::NewBuilder().Build();
  ABSL_CHECK_OK(engine);

  // Plan the program — produces a fresh Instance ready to Eval.
  auto instance = engine->Plan(*program);
  ABSL_CHECK_OK(instance);

  // Evaluate with an empty Activation (no bound variables).
  auto value = instance->Eval(cel::Activation{});
  ABSL_CHECK_OK(value);
  ABSL_CHECK_EQ(*value->AsInt(), 6);
}
```

### 2. Variables + Activation binding

```cpp
// Declare two int variables at Compiler build time.
cel::Compiler::Builder cb;
cb.DeclareVariable("a", cel::CelType::Int());
cb.DeclareVariable("b", cel::CelType::Int());
auto compiler = std::move(cb).Build();
ABSL_CHECK_OK(compiler);

auto program = compiler->Compile("a * b + 1");
ABSL_CHECK_OK(program);

auto engine = cel::Engine::NewBuilder().Build();
ABSL_CHECK_OK(engine);
auto instance = engine->Plan(*program);
ABSL_CHECK_OK(instance);

// Bind values for this Eval.  Activation is per-call; reuse the
// same Instance for many Activations.
cel::Activation a;
a.Bind("a", cel::Value::Int(6));
a.Bind("b", cel::Value::Int(7));

auto value = instance->Eval(a);
ABSL_CHECK_OK(value);
ABSL_CHECK_EQ(*value->AsInt(), 43);
```

### 3. Reusing the Compiler / Engine / Instance — the realistic shape

Each layer is independently cacheable.  A real host hits Compile rarely,
Plan per-request, Eval many times per Plan:

```cpp
// Process-wide singletons.  Build once.
static const cel::Engine& kEngine = []() -> const cel::Engine& {
  auto e = cel::Engine::NewBuilder().Build();
  ABSL_CHECK_OK(e);
  return *(new cel::Engine(*std::move(e)));
}();

static const cel::Compiler& kCompiler = []() -> const cel::Compiler& {
  cel::Compiler::Builder b;
  b.DeclareVariable("user_age", cel::CelType::Int());
  b.DeclareVariable("country", cel::CelType::String());
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *(new cel::Compiler(*std::move(c)));
}();

// Compile once per CEL rule.  Programs are copyable / serializable —
// cache them keyed on the source string.
static const cel::Program& kRule = []() -> const cel::Program& {
  cel::CompilerOptions opts;
  opts.optimize_level = 2;  // Recommended for request-path queries.
  auto p = kCompiler.Compile(
      "user_age >= 18 && country in ['US', 'CA', 'MX']", opts);
  ABSL_CHECK_OK(p);
  return *(new cel::Program(*std::move(p)));
}();

// Per-request: Plan + Eval.
bool EvaluateRule(int64_t age, std::string country) {
  auto instance = kEngine.Plan(kRule);
  ABSL_CHECK_OK(instance);

  cel::Activation a;
  a.Bind("user_age", cel::Value::Int(age));
  a.Bind("country", cel::Value::String(std::move(country)));

  auto v = instance->Eval(a);
  ABSL_CHECK_OK(v);
  return *v->AsBool();
}
```

For a per-request pattern that runs many Evals on the same Instance
(e.g. evaluating the same rule against a stream of inputs), keep the
Instance alive and call `Eval(activation)` in a loop — `arena_reset`
rewinds the per-Eval arena automatically at the top of each call.

### 4. Proto message variables

CEL's bread-and-butter input shape.  Declare the variable by FQN;
bind a real `google::protobuf::Message` (or a host-side backing
without copying) at Eval time.

```cpp
#include "compiler_v2/testdata/e2e_fixture.pb.h"  // your .proto

cel::Compiler::Builder cb;
cb.DeclareVariable("c",
                   cel::CelType::Message("celwasm.testdata.Customer"));
auto compiler = std::move(cb).Build();
ABSL_CHECK_OK(compiler);

auto program = compiler->Compile("c.name + ' (' + string(c.age) + ')'");
ABSL_CHECK_OK(program);

auto engine = cel::Engine::NewBuilder().Build();
ABSL_CHECK_OK(engine);
auto instance = engine->Plan(*program);
ABSL_CHECK_OK(instance);

celwasm::testdata::Customer msg;
msg.set_name("Ada");
msg.set_age(36);

cel::Activation a;
a.Bind("c", cel::Value::Message(msg));

auto v = instance->Eval(a);
ABSL_CHECK_OK(v);
ABSL_CHECK_EQ(*v->AsString(), "Ada (36)");
```

`cel::Value::Message(msg)` snapshots a const reference to the
message; the caller must keep `msg` alive across the Eval call.
For ownership-transfer use `cel::Value::OwnedMessage(std::unique_ptr<…>)`.

### 5. Inspecting results

`Value::kind()` returns the `Kind` enum; `AsX()` accessors return an
`absl::StatusOr<T>` that errors with `InvalidArgument` if the kind
doesn't match.

```cpp
auto v = instance->Eval(a);
ABSL_CHECK_OK(v);
switch (v->kind()) {
  case cel::Value::Kind::kBool:    use(*v->AsBool());   break;
  case cel::Value::Kind::kInt:     use(*v->AsInt());    break;
  case cel::Value::Kind::kUint:    use(*v->AsUint());   break;
  case cel::Value::Kind::kDouble:  use(*v->AsDouble()); break;
  case cel::Value::Kind::kString:  use(*v->AsString()); break;
  case cel::Value::Kind::kError:   /* read v->AsError() — CEL error */ break;
  case cel::Value::Kind::kUnknown: /* partial-eval surfaced */ break;
  default: /* … list / map / message — see api/value.h */ break;
}
```

### 6. Saving and reloading a Program

A `cel::Program` is **pure data**: the compiled wasm bytes plus the
embedded `cel.abi` custom section.  No wasmtime state, no descriptor
pool, no engine handles — everything needed to evaluate the program
lives in the byte buffer.  That makes save/reload a thin wrapper
around two existing API surfaces:

  - **Save**: `program.wasm_bytes()` returns a `Span<const uint8_t>`.
    Copy into your storage of choice — disk, an in-memory cache, a
    remote object store, a database BLOB column, the wire to another
    process.
  - **Reload**: `cel::Program(std::vector<uint8_t> bytes)` reconstructs
    a Program from the saved bytes.  The constructor is intentionally
    non-validating; the wasmtime parse happens later in
    `Engine::Plan`, which surfaces malformed bytes as a
    `FailedPrecondition` rather than a crash.

```cpp
#include <fstream>
#include "compiler_v2/api/program.h"
// … other includes from snippet 1 …

// Compile once, save to disk.
cel::Compiler::Builder cb;
cb.DeclareVariable("user_age", cel::CelType::Int());
auto compiler = std::move(cb).Build();
ABSL_CHECK_OK(compiler);

cel::CompilerOptions opts;
opts.optimize_level = 2;  // Optimized bytes round-trip cleanly too.
auto program = compiler->Compile("user_age >= 18", opts);
ABSL_CHECK_OK(program);

// --- Save ---
{
  auto bytes = program->wasm_bytes();
  std::ofstream out("/var/cache/rules/age_check.wasm", std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// --- Reload (potentially in a different process / next day) ---
std::vector<uint8_t> bytes;
{
  std::ifstream in("/var/cache/rules/age_check.wasm", std::ios::binary);
  bytes.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
}
cel::Program reloaded(std::move(bytes));

// Plan + Eval as usual — no Compile needed.
auto engine = cel::Engine::NewBuilder().Build();
ABSL_CHECK_OK(engine);
auto instance = engine->Plan(reloaded);
ABSL_CHECK_OK(instance);

cel::Activation a;
a.Bind("user_age", cel::Value::Int(20));
auto v = instance->Eval(a);
ABSL_CHECK_OK(v);
ABSL_CHECK(*v->AsBool());
```

What the round-trip preserves (the load-bearing assertion lives at
`compiler_v2/e2e/program_roundtrip_test.cc`):

  - The complete compiled module — codegen output + cel.abi section.
  - The declared-variable schema (it's encoded in cel.abi).
  - The Binaryen optimization that was applied at Compile time — the
    saved bytes ARE the optimized bytes; no re-optimization at reload.

What the round-trip does NOT preserve (these are host-state, never
in the bytes):

  - The `cel::Engine` (wasm engine + parsed runtime module) — build
    one per process / tenant, shared across reloads.
  - The `cel::Activation` (per-eval variable bindings) — built fresh
    per Eval call.
  - The protobuf descriptor pool — message types referenced by the
    program must be linked into the reload-side process (the same
    way they had to be linked into the Compile-side process).  If
    you compile against `generated_pool()` and reload against
    `generated_pool()`, this is automatic.

**Versioning caveat.**  The wasm bytes encode the runtime ABI version
implicitly via the symbols they import from `cel.cel_*`.  If a future
runtime bump renames or removes an import (e.g. a later milestone
reshapes `cel_unknown_merge`), reloading a Program compiled against
the old ABI will fail at `Engine::Plan` time with a missing-import
error.  In practice this means: keep saved bytes paired with a
runtime-version marker (e.g. a sidecar file or a key prefix), and
recompile on runtime upgrade.  The `cel.abi` custom section carries
enough metadata to plumb a version field through; that's a future-
milestone slice.

### 7. How big are the artifacts?

Measured numbers as of 2026-05-15, darwin-arm64, `-c opt` build.
Reproduce via `bazel run -c opt //compiler_v2/bench:program_size_main`.

#### Compiled program (per-expression wasm bytes) vs. CEL AST proto

The most useful comparison for save/reload: how big is our compiled
wasm module vs. the `cel.expr.CheckedExpr` proto that other CEL
implementations (cel-go, cel-java, cel-cpp) cache and ship?

**Always set `optimize_level = 2` when storing or shipping a
compiled program.**  Unopt modules are dominated by import-table
boilerplate (every `cel.cel_*` runtime symbol is declared even if
the body doesn't call it, per the "codegen always links the runtime
fully" rule); Binaryen's `remove-unused-module-elements` DCE's the
imports the body doesn't call, so the optimized size is dominated
by the actual expression body.

| Expression                       | AST proto | wasm opt=0 | wasm opt=2 | opt2 / AST |
| -------------------------------- | --------: | ---------: | ---------: | ---------: |
| `42` (literal int)               |    36 B   |   2516 B   |   **136 B** | 3.78× |
| `"hello"` (literal string)       |    41 B   |   2524 B   |   **144 B** | 3.51× |
| `"hello, " + s` (string concat)  |   123 B   |   2557 B   |   **207 B** | 1.68× |
| `s.contains("world")`            |   136 B   |   2557 B   |   **209 B** | 1.54× |
| `int(string(123))` (M10 conv)    |   148 B   |   2531 B   |   **209 B** | 1.41× |
| `[1,2,3,4,5]` (list lit, 5 elt)  |   148 B   |   2669 B   |   **336 B** | 2.27× |
| `{"a":1, "b":2}` (map lit, 2 e.) |   152 B   |   2633 B   |   **301 B** | 1.98× |
| `type(x) == int` (M9 type)       |   176 B   |   2565 B   |   **231 B** | 1.31× |
| `a + b + c` (3-term arith)       |   204 B   |   2580 B   |   **191 B** | **0.94×** |
| `{...3 entries...}["b"]` (lookup)|   279 B   |   2745 B   |   **440 B** | 1.58× |
| 20-term `a<b && b<c && …` chain  |  3094 B   |   3339 B   |   **933 B** | **0.30×** |

The crossover is at expression size ~200 B AST.  **Below that, the
AST proto is smaller; above, our opt=2 wasm is smaller** (and the
margin grows with expression complexity — the 20-term chain is over
3× smaller as wasm).

Why the AST proto loses on big expressions: every `Expr` node in
the proto carries a node-id, an entry in `type_map`, and an entry
in `reference_map` (with the resolved overload / decl name).  The
20-term chain has ~80 AST nodes; the per-node overhead dominates.
The wasm body is a flat sequence of `local.get` + `i32.lt_s` +
`i32.and` instructions — 2-3 bytes per AST node after lowering.

Why the AST proto wins on tiny expressions: a single literal is
3 nodes (the literal + its id + its type); our wasm has unavoidable
fixed overhead from the module preamble, `cel.abi` custom section,
memory declaration, and `$eval` function header.  Floor at
`optimize_level=2` is ~130 B regardless of body content.

**Practical takeaway.**  Cache / ship the wasm bytes for any real
expression — they're at most ~1 KB for typical workloads, and the
reload path is `Program(bytes) → Plan` which skips parsing, type-
checking, AST-rewriting, and codegen.  Caching the AST proto only
makes sense if you also need to do post-load transformations (e.g.
re-target a different runtime); the per-Eval reload cost is higher
because you have to re-run the full Compile pipeline.

#### Runtime module (one-time cost per process)

| Artifact                       | Size    | Notes |
| ------------------------------ | ------: | ----- |
| `cel_runtime.wasm`             | **49,611 B** (~48 KB) | What `Engine::Builder::Build` parses once at process start.  Imported by every expr module via `(import "cel" ...)`. |
| `libcel_runtime.a` (native)    | **128,296 B** (~125 KB) | Static archive of the same C source for in-process callers (kernel_bench, runtime unit tests, host trampolines).  Larger than the wasm because it carries x86_64 / arm64 native code, not the more compact wasm encoding. |

Both are built unconditionally with `-O3 -flto` (see "Build-time vs.
compile-time knobs" below).

#### In-memory C++ objects

The handles you pass around are small; the storage they own is what
matters.  All sizes are `sizeof()`:

| Type                | sizeof | What it owns / points to |
| ------------------- | -----: | ------------------------ |
| `cel::Program`      |  24 B  | + `std::vector<uint8_t>` data (~140 B - 1 KB per expr at opt2; see table above). |
| `cel::Compiler`     |  24 B  | + `std::vector<VariableDeclaration>` (one per `DeclareVariable` call). |
| `cel::CompilerOptions` | 40 B | Plain-old-data, copy freely. |
| `cel::Value`        |  40 B  | Discriminated union; aggregates (List/Map/Message) own a shared_ptr to a backing. |
| `cel::Activation`   |  32 B  | + a `flat_hash_map<string, Value>` for bound variables. |
| `CelValue` (wire)   |  24 B  | Arena-resident; size pinned by `static_assert`.  This is the 24-byte slot every codegen-emitted load/store reads & writes. |

#### Eval-time memory footprint (per Instance)

Each `cel::Instance` owns a wasmtime store + linear memory.  The
linear memory's size is set by `CompilerOptions::mem_size_bytes`,
rounded up to the next wasm page:

| `mem_size_bytes` setting | Actual allocation | Use case |
| -----------------------: | ----------------: | -------- |
| **128 KiB (default)**    | 2 wasm pages = 128 KiB | Most workloads — fits scalar / small-aggregate evals comfortably. |
| 256 KiB                  | 4 pages = 256 KiB | Heavy string concat or 100s-of-element list construction. |
| 1 MiB                    | 16 pages = 1 MiB | Stress / fuzzing; not a realistic production setting. |

The low `CELWASM_RESERVED_LOW_MEMORY_BYTES` (8 KiB, see
`runtime/cel_layout.h`) of linear memory is reserved for the expr
module's rodata + workspace data segments; wasi-libc places its own
static data, stack, and dlmalloc heap above that.  The per-Eval
bump arena is **not** a fixed slice of linear memory — it's a
`CELWASM_ARENA_CAPACITY_BYTES` (64 KiB) buffer `malloc`'d once per
Instance via `arena_init`, with its cursor/capacity living in a BSS
struct (`runtime/cel_arena.c`).  `arena_reset` rewinds the cursor to
zero at the top of every Eval — it does not free the buffer.  (This
replaced the pre-Phase-C design where the arena cursor lived at fixed
linear-memory bytes 8/12 and the arena was a bump region carved out
of the imported memory; see `design.md` §"Phase C delta" callouts and
`wasi/DESIGN.md` §4–§5 for the migration.)

A wasm page is 64 KiB by spec; you can't allocate fractional pages.
That makes the minimum per-Instance memory cost ~128 KiB even for a
program that evaluates a single literal.  If you instantiate
thousands of Instances concurrently this is the per-tenant memory
budget to track — share an Engine, but `Plan` a fresh Instance per
request and let it drop at end-of-request.

### 8. CLI

For a one-off "does this even compile / what does it evaluate to"
check without writing C++, use the `cel` driver under
`compiler_v2/tools/cel/`.  It wraps the full v2 pipeline (compile →
plan → eval) and has three subcommands — `eval`, `check`, `compile`:

```bash
# Evaluate.
bazel run //compiler_v2/tools/cel:cel -- eval '1 + 2 + 3'        # → 6
bazel run //compiler_v2/tools/cel:cel -- eval 'a * b' \
  --var 'a:int=6' --var 'b:int=7'                                # → 42

# Parse + type-check only.
bazel run //compiler_v2/tools/cel:cel -- check 'size("héllo")'   # → OK
bazel run //compiler_v2/tools/cel:cel -- check 'user_age >= 18' \
  --var 'user_age:int'                                           # → OK

# Emit wasm bytes to a file (or stdout with no --output).
bazel run //compiler_v2/tools/cel:cel -- compile '1 + 2' \
  --output /tmp/expr.wasm
```

`--var name:Type[=value]` declares (and optionally binds) a variable;
`--proto` / `--descriptor_set` load message schemas; `--container`
sets the ident-resolution prefix; `--O 0..3` is the Binaryen optimizer
level; `--format textproto|json|cel` picks message output rendering.
See `compiler_v2/tools/cel/README.md` for the full flag surface,
`--var` grammar, and the proto-schema loading rules.

(The pre-rewrite v1 `compiler/cli/celwasmc` driver has been deleted;
`compiler_v2` has no `cli/` directory — the CLI lives under
`tools/cel/`.)

## Build-time vs. compile-time knobs

The compiler exposes two distinct optimization surfaces; they get
confused if you don't read this section.

### Runtime (`cel_runtime.wasm` + native `:cel_runtime` cc_library)

**Always built with `-O3 -flto`.**  Not user-tunable.  Hardcoded into
both the native cc_library and the wasm32 genrule in
`runtime/BUILD.bazel`.  The runtime ships once per compiler build, is
shared across every Compile, and its inner loops (`utf8_valid`,
`parse_*`, `cel_int_add_at_vv`, …) live in the hottest part of every
Eval — there's no scenario where shipping it unoptimized is the right
call.  LTO matters specifically because the runtime is now ~10 per-
topic `.c` files after the cel-runtime-c-split (cel_arith.c,
cel_compare.c, cel_3vl.c, cel_convert.c, cel_string_ops.c, …); without
LTO the leaf kernels can't inline across TU boundaries into the
codegen-emitted call sites that follow them.

Native and wasm32 builds use the same flags so unit tests and the
shipped runtime exercise the same code shape.  An `_Alignas(8)` on
the host-side memory backing (`runtime/cel_memory.c`) is load-bearing
under `-O3 -flto`: the linker stops auto-padding to 8 once LTO sees
the static is small, and CelValue access traps without it.

### Compile-time (per-expression EXPR module)

User-tunable via `cel::CompilerOptions` (api/compiler.h).  Three knobs:

  - **`mem_size_bytes`** (default 128 KiB / two wasm pages) — total
    linear-memory size.  Raise when a single Eval needs a larger
    arena (heavy string concat, large list construction).  Rounded
    up to the next wasm page at emit time.
  - **`container`** (default empty) — package-style ident resolver
    prefix.  Equivalent to CEL-Go's `container` option.
  - **`optimize_level`** (default 0) — Binaryen `wasm-opt -O<n>` on
    the emitted EXPR module before serialization.  0 = no-op (byte-
    identical output, preserves codegen golden tests).  2 = canonical
    pipeline; ~2-3× the Compile cost but cuts Eval time roughly in
    half on chain-heavy bodies (see `bench/README.md` for the table).
    Recommended production setting is 2 on the request path;
    Compile cost amortises across many Eval calls.

`compiler_v2/compile.h` has the internal `celwasm::CompileOptions`
with four additional pipeline-only knobs (`eval_internal_name`,
`eval_export_name`, `validate`, `serialize`) that public callers
can't set — they're plumbing for tests and the CLI.

## Running the suite

```bash
# Default fast suite — every gtest binary not tagged `manual`.
bazel test //compiler_v2/...

# Full closeout gate — adds the manual e2e tests
# (wasmtime-driven, optimize_test, wat_runner, cel_runtime_wasm_test)
# and walks the whole cel-spec conformance corpus.
scripts/run_full_suite.sh

# Quick variant — skips conformance (the slow part).
scripts/run_full_suite.sh --quick
```

Per CLAUDE.md, **`bazel test //compiler_v2/...` being green does NOT
mean a milestone is done** — the manual-tagged e2e tests carry the
load-bearing assertions and must be run explicitly via
`scripts/run_full_suite.sh`.  See
`doc/implementation-plan/per-component-test-coverage.md` for the
discipline.

## Perf

`bench/README.md` has the full table.  Headline numbers as of
2026-05-15, darwin-arm64, `-c opt` build:

  - Kernel microbench leaves (`cel_int_add_at_vv`, `cel_int_eq_at_vv`,
    `cel_uint_to_int_at_v`, …): 7-10 ns/call.
  - Pipeline Compile: 248-395 us per CEL source expression.
  - Pipeline Plan: 240-253 us (wasmtime instantiate dominates).
  - Pipeline Eval steady-state: 160 ns (literal) to 11 us (20-term
    comparison chain).  Setting `optimize_level = 2` cuts the chain
    case to 5.4 us at +120% Compile cost.

## Porting to another host (the wasm path)

To unblock Linux / Intel macOS for the wasm-execution targets:

1. Add a `wasmtime_<host>` `http_archive` in `MODULE.bazel` (only
   `wasmtime_darwin_arm64` exists today) and `select()` it per-host
   everywhere the wasm targets reference `@wasmtime_darwin_arm64`
   (`tools/wat_runner`, `runtime`, `host`, `probes/*`).
2. Replace the `@wasi_sdk_darwin_arm64` defaults in
   `//third_party/wasi_sdk:BUILD.bazel` — the cc-toolchain
   `sysroot_path` / `cxx_builtin_include_directories` and the legacy
   aliases — with a host `select()`, registering one `toolchain()` per
   host with `exec_compatible_with` (the cc_toolchain_config file's own
   header notes this as the intended pattern).  The Linux/Intel
   `wasi_sdk_*` archives are already declared in `MODULE.bazel`.

## Pointers

  - Top-level design: `doc/implementation-plan/rewrite/design.md`
    + `doc/implementation-plan/rewrite/cel-host-surface.md`.
  - Per-milestone plans: `doc/implementation-plan/rewrite/m*.md`.
  - Testing checklist: `doc/implementation-plan/testing-checklist.md`.
  - Per-component test coverage: `doc/implementation-plan/per-
    component-test-coverage.md` — the closeout gate definition.
  - CEL language spec we honour: `doc/langdef.md`.
  - Repo-wide rules for changes: `CLAUDE.md` at the repo root.
