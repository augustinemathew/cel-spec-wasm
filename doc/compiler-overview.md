# cel-wasm compiler overview

The compiler turns a CEL source expression into a self-contained wasm
module the host instantiates and runs through wasmtime.  This page is
the orientation guide — start here if you're new to the codebase or you
need to know which knob controls what.

For the full design, see `doc/implementation-plan/rewrite/design.md`
and `doc/implementation-plan/rewrite/cel-host-surface.md`.  For per-
component test coverage and the milestone closeout discipline, see
`doc/implementation-plan/per-component-test-coverage.md`.

## Getting started

Prerequisites, the macOS / Linux setup steps, the `bazel build` / `bazel
test` commands, the CLI, the conformance runner, and the Docker Linux
image all live in the repository-root
[`README.md`](../README.md).  cel-wasm builds on
**macOS (Apple Silicon)** and **Linux (arm64 / x86_64)** with the same
`bazel` invocations — Bazel fetches the wasi-sdk cross-compile toolchain,
binaryen, and wasmtime for the build host automatically.

This page is the orientation guide for working *inside* the compiler:
the layout, the Compile → Plan → Eval lifecycle, the API quickstart,
artifact sizes, and the build/test knobs.  For the lint/format gate and
the compile-db / PCH details, see `doc/contributing.md`.

## Layout

The repo is organised by **lifecycle role** at the top level:

```
compiler/       — compile-time: CEL source → Program (wasm bytes + cel.abi).
                  Public surface: compiler.h (Compiler + CompilerOptions)
                  and program.h.
  frontend/     — parse_and_check.  Wraps cel-cpp's parser + checker;
                  runs RejectDyn; emits the typed AST.
  ir/           — typed_ast + annotations.  Stable mid-layer between
                  the checker and codegen.
  codegen/      — resolve_pass, layout_pass, expr_lower, module,
                  overload_table, static_memory_builder, slot_allocator.
                  Lowers the typed AST → Binaryen IR → wasm bytes.
  celfn/        — the `.celfn` custom-function IDL (parser + library).
  internal/     — compile.{h,cc}, the private pipeline facade
                  (frontend → codegen).  Public callers go through
                  compiler/compiler.h instead.
eval/           — eval-time: Program + Activation → Value (the
                  C++/wasmtime evaluator).  Public surface: engine.h,
                  instance.h, activation.h, value.h, error.h,
                  attribute.h.  host/ (cel_log trampolines) and
                  internal/ (wasmtime glue, abi_decode, cel_host) are
                  private.
shared/         — CelType, the type vocabulary both halves speak.
abi/            — cel.abi custom-section emitter / parser; the pinned
                  wire format the host loader reads to discover rodata /
                  workspace / arena offsets.
runtime/        — the language-agnostic C kernel (cel_arith.c,
                  cel_compare.c, cel_3vl.c, cel_convert.c,
                  cel_string_ops.c, …).  Cross-compiled to
                  cel_runtime.wasm; also linked natively for unit tests
                  and host trampoline callers.
tools/          — CLI utilities.  tools/cel/ is the `cel` command-line
                  driver (eval / check / compile / generate; see
                  tools/cel/README.md).  tools/wat_runner/ assembles +
                  runs WAT traces.
conformance/    — harness that runs the cel-spec conformance corpus
                  against the pipeline.  See conformance/README.md.
e2e/            — full-pipeline integration tests (wasmtime-driven),
                  plus manual-tagged gates like optimize_test.
benchmark/      — comparative benchmark corpus + compiler/kernel/component
                  tiers; see benchmark/README.md.
testdata/       — shared proto fixtures.
spec/           — cel-spec heritage: the .textproto conformance corpus
                  under spec/tests/.
examples/       — runnable embedding examples (see examples/README.md).
```

`compiler/` and `eval/` both depend on `shared/`; neither depends on
the other — `compiler/` stays wasm-targetable (no wasmtime dependency).

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

The benches under `benchmark/` are structured around these boundaries —
re-read `benchmark/README.md` if you need a perf number anchored to a
specific layer.

## Quickstart

End-to-end: compile a CEL expression, plan it, evaluate it.  Every
snippet below is real code lifted from `e2e/`; copy-paste
into a `cc_binary` whose deps include `//compiler:compiler`,
`//eval:engine`, `//eval:instance`, `//eval:activation`,
`//eval:value`, `//shared:type`, and
`@com_google_absl//absl/log:absl_check`.

### 1. Scalar literal — the simplest "hello, world"

```cpp
#include "absl/log/absl_check.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"

int main() {
  // Build a Compiler with no declared variables.
  celwasm::Compiler::Builder cb;
  auto compiler = std::move(cb).Build();
  ABSL_CHECK_OK(compiler);

  // Compile a CEL expression to a Program (wasm bytes + ABI).
  auto program = compiler->Compile("1 + 2 + 3");
  ABSL_CHECK_OK(program);

  // Stand up a process-wide Engine (owns wasm_engine_t + the parsed
  // cel_runtime.wasm module).
  auto engine = celwasm::Engine::NewBuilder().Build();
  ABSL_CHECK_OK(engine);

  // Plan the program — produces a fresh Instance ready to Eval.
  auto instance = engine->Plan(*program);
  ABSL_CHECK_OK(instance);

  // Evaluate with an empty Activation (no bound variables).
  auto value = instance->Eval(celwasm::Activation{});
  ABSL_CHECK_OK(value);
  ABSL_CHECK_EQ(*value->AsInt(), 6);
}
```

### 2. Variables + Activation binding

```cpp
// Declare two int variables at Compiler build time.
celwasm::Compiler::Builder cb;
cb.DeclareVariable("a", celwasm::CelType::Int());
cb.DeclareVariable("b", celwasm::CelType::Int());
auto compiler = std::move(cb).Build();
ABSL_CHECK_OK(compiler);

auto program = compiler->Compile("a * b + 1");
ABSL_CHECK_OK(program);

auto engine = celwasm::Engine::NewBuilder().Build();
ABSL_CHECK_OK(engine);
auto instance = engine->Plan(*program);
ABSL_CHECK_OK(instance);

// Bind values for this Eval.  Activation is per-call; reuse the
// same Instance for many Activations.
celwasm::Activation a;
a.Bind("a", celwasm::Value::Int(6));
a.Bind("b", celwasm::Value::Int(7));

auto value = instance->Eval(a);
ABSL_CHECK_OK(value);
ABSL_CHECK_EQ(*value->AsInt(), 43);
```

### 3. Reusing the Compiler / Engine / Instance — the realistic shape

Each layer is independently cacheable.  A real host hits Compile rarely,
Plan per-request, Eval many times per Plan:

```cpp
// Process-wide singletons.  Build once.
static const celwasm::Engine& kEngine = []() -> const celwasm::Engine& {
  auto e = celwasm::Engine::NewBuilder().Build();
  ABSL_CHECK_OK(e);
  return *(new celwasm::Engine(*std::move(e)));
}();

static const celwasm::Compiler& kCompiler = []() -> const celwasm::Compiler& {
  celwasm::Compiler::Builder b;
  b.DeclareVariable("user_age", celwasm::CelType::Int());
  b.DeclareVariable("country", celwasm::CelType::String());
  auto c = std::move(b).Build();
  ABSL_CHECK_OK(c);
  return *(new celwasm::Compiler(*std::move(c)));
}();

// Compile once per CEL rule.  Programs are copyable / serializable —
// cache them keyed on the source string.
static const celwasm::Program& kRule = []() -> const celwasm::Program& {
  celwasm::CompilerOptions opts;
  opts.optimize_level = 2;  // Recommended for request-path queries.
  auto p = kCompiler.Compile(
      "user_age >= 18 && country in ['US', 'CA', 'MX']", opts);
  ABSL_CHECK_OK(p);
  return *(new celwasm::Program(*std::move(p)));
}();

// Per-request: Plan + Eval.
bool EvaluateRule(int64_t age, std::string country) {
  auto instance = kEngine.Plan(kRule);
  ABSL_CHECK_OK(instance);

  celwasm::Activation a;
  a.Bind("user_age", celwasm::Value::Int(age));
  a.Bind("country", celwasm::Value::String(std::move(country)));

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
#include "testdata/e2e_fixture.pb.h"  // your .proto

celwasm::Compiler::Builder cb;
cb.DeclareVariable("c",
                   celwasm::CelType::Message("celwasm.testdata.Customer"));
auto compiler = std::move(cb).Build();
ABSL_CHECK_OK(compiler);

auto program = compiler->Compile("c.name + ' (' + string(c.age) + ')'");
ABSL_CHECK_OK(program);

auto engine = celwasm::Engine::NewBuilder().Build();
ABSL_CHECK_OK(engine);
auto instance = engine->Plan(*program);
ABSL_CHECK_OK(instance);

celwasm::testdata::Customer msg;
msg.set_name("Ada");
msg.set_age(36);

celwasm::Activation a;
a.Bind("c", celwasm::Value::Message(msg));

auto v = instance->Eval(a);
ABSL_CHECK_OK(v);
ABSL_CHECK_EQ(*v->AsString(), "Ada (36)");
```

`celwasm::Value::Message(msg)` snapshots a const reference to the
message; the caller must keep `msg` alive across the Eval call.
For ownership-transfer use `celwasm::Value::OwnedMessage(std::unique_ptr<…>)`.

### 5. Inspecting results

`Value::kind()` returns the `Kind` enum; `AsX()` accessors return an
`absl::StatusOr<T>` that errors with `InvalidArgument` if the kind
doesn't match.

```cpp
auto v = instance->Eval(a);
ABSL_CHECK_OK(v);
switch (v->kind()) {
  case celwasm::Value::Kind::kBool:    use(*v->AsBool());   break;
  case celwasm::Value::Kind::kInt:     use(*v->AsInt());    break;
  case celwasm::Value::Kind::kUint:    use(*v->AsUint());   break;
  case celwasm::Value::Kind::kDouble:  use(*v->AsDouble()); break;
  case celwasm::Value::Kind::kString:  use(*v->AsString()); break;
  case celwasm::Value::Kind::kError:   /* read v->AsError() — CEL error */ break;
  case celwasm::Value::Kind::kUnknown: /* partial-eval surfaced */ break;
  default: /* … list / map / message — see eval/value.h */ break;
}
```

### 6. Saving and reloading a Program

A `celwasm::Program` is **pure data**: the compiled wasm bytes plus the
embedded `cel.abi` custom section.  No wasmtime state, no descriptor
pool, no engine handles — everything needed to evaluate the program
lives in the byte buffer.  That makes save/reload a thin wrapper
around two existing API surfaces:

  - **Save**: `program.wasm_bytes()` returns a `Span<const uint8_t>`.
    Copy into your storage of choice — disk, an in-memory cache, a
    remote object store, a database BLOB column, the wire to another
    process.
  - **Reload**: `celwasm::Program(std::vector<uint8_t> bytes)` reconstructs
    a Program from the saved bytes.  The constructor is intentionally
    non-validating; the wasmtime parse happens later in
    `Engine::Plan`, which surfaces malformed bytes as a
    `FailedPrecondition` rather than a crash.

```cpp
#include <fstream>
#include "compiler/program.h"
// … other includes from snippet 1 …

// Compile once, save to disk.
celwasm::Compiler::Builder cb;
cb.DeclareVariable("user_age", celwasm::CelType::Int());
auto compiler = std::move(cb).Build();
ABSL_CHECK_OK(compiler);

celwasm::CompilerOptions opts;
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
celwasm::Program reloaded(std::move(bytes));

// Plan + Eval as usual — no Compile needed.
auto engine = celwasm::Engine::NewBuilder().Build();
ABSL_CHECK_OK(engine);
auto instance = engine->Plan(reloaded);
ABSL_CHECK_OK(instance);

celwasm::Activation a;
a.Bind("user_age", celwasm::Value::Int(20));
auto v = instance->Eval(a);
ABSL_CHECK_OK(v);
ABSL_CHECK(*v->AsBool());
```

What the round-trip preserves (the load-bearing assertion lives at
`e2e/program_roundtrip_test.cc`):

  - The complete compiled module — codegen output + cel.abi section.
  - The declared-variable schema (it's encoded in cel.abi).
  - The Binaryen optimization that was applied at Compile time — the
    saved bytes ARE the optimized bytes; no re-optimization at reload.

What the round-trip does NOT preserve (these are host-state, never
in the bytes):

  - The `celwasm::Engine` (wasm engine + parsed runtime module) — build
    one per process / tenant, shared across reloads.
  - The `celwasm::Activation` (per-eval variable bindings) — built fresh
    per Eval call.
  - The protobuf descriptor pool — message types referenced by the
    program must be linked into the reload-side process (the same
    way they had to be linked into the Compile-side process).  If
    you compile against `generated_pool()` and reload against
    `generated_pool()`, this is automatic.

**Versioning caveat.**  The wasm bytes encode the runtime ABI version
implicitly via the symbols they import from `cel.cel_*`.  If a future
runtime bump renames or removes an import (e.g. a future release
reshapes `cel_unknown_merge`), reloading a Program compiled against
the old ABI will fail at `Engine::Plan` time with a missing-import
error.  In practice this means: keep saved bytes paired with a
runtime-version marker (e.g. a sidecar file or a key prefix), and
recompile on runtime upgrade.  The `cel.abi` custom section carries
enough metadata to plumb a version field through; that work has not
been done yet.

### 7. How big are the artifacts?

Measured numbers as of 2026-05-15, darwin-arm64, `-c opt` build.
Reproduce via `bazel run -c opt //benchmark/compiler:program_size_main`.

> **These are `LinkMode::kDynamic` numbers** — the small expr module
> that imports the shared runtime.  Under the default
> `LinkMode::kStatic`, the runtime is merged into every Program
> (~800 KB self-contained artifact) and there is no separate
> `cel_runtime.wasm` to amortise; see "Static vs. dynamic linking"
> below for the tradeoff.

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
| `celwasm::Program`      |  24 B  | + `std::vector<uint8_t>` data (~140 B - 1 KB per expr at opt2; see table above). |
| `celwasm::Compiler`     |  24 B  | + `std::vector<VariableDeclaration>` (one per `DeclareVariable` call). |
| `celwasm::CompilerOptions` | 40 B | Plain-old-data, copy freely. |
| `celwasm::Value`        |  40 B  | Discriminated union; aggregates (List/Map/Message) own a shared_ptr to a backing. |
| `celwasm::Activation`   |  32 B  | + a `flat_hash_map<string, Value>` for bound variables. |
| `CelValue` (wire)   |  24 B  | Arena-resident; size pinned by `static_assert`.  This is the 24-byte slot every codegen-emitted load/store reads & writes. |

#### Eval-time memory footprint (per Instance)

Each `celwasm::Instance` owns a wasmtime store + linear memory.  The
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
`tools/cel/`.  It wraps the full pipeline (compile →
plan → eval) and has four subcommands — `eval`, `check`, `compile`,
and `generate` (emit custom-function bindings from a `.idl` file):

```bash
# Evaluate.
bazel run //tools/cel:cel -- eval '1 + 2 + 3'        # → 6
bazel run //tools/cel:cel -- eval 'a * b' \
  --var 'a:int=6' --var 'b:int=7'                                # → 42

# Parse + type-check only.
bazel run //tools/cel:cel -- check 'size("héllo")'   # → OK
bazel run //tools/cel:cel -- check 'user_age >= 18' \
  --var 'user_age:int'                                           # → OK

# Emit wasm bytes to a file (or stdout with no --output).
bazel run //tools/cel:cel -- compile '1 + 2' \
  --output /tmp/expr.wasm
```

`--var name:Type[=value]` declares (and optionally binds) a variable;
`--proto` / `--descriptor_set` load message schemas; `--container`
sets the ident-resolution prefix; `--O 0..3` is the Binaryen optimizer
level; `--format textproto|json|cel` picks message output rendering.
See `tools/cel/README.md` for the full flag surface,
`--var` grammar, and the proto-schema loading rules.

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

User-tunable via `celwasm::CompilerOptions` (`compiler/compiler.h`).
The main knobs:

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
    half on chain-heavy bodies (see
    `doc/implementation-plan/rewrite/archive/bench-tree-readme.md` for
    the table).
    Recommended production setting is 2 on the request path;
    Compile cost amortises across many Eval calls.
  - **`link_mode`** (default `kStatic`) — whether the runtime helpers
    are statically linked into the Program or imported from a shared
    runtime instance at Plan time.  See "Static vs. dynamic linking"
    below.

`compiler/internal/compile.h` has the internal `celwasm::CompileOptions`
with four additional pipeline-only knobs (`eval_internal_name`,
`eval_export_name`, `validate`, `serialize`) that public callers
can't set — they're plumbing for tests and the CLI.

### Static vs. dynamic linking (`link_mode`)

`CompilerOptions::link_mode` picks how the runtime helpers
(`cel_int_add_at_vv`, slot accessors, arena ops, …) reach the emitted
Program:

  - **`LinkMode::kStatic` (default)** — the runtime is **merged into
    the Program at Compile time**.  The Program is self-contained
    (~800 KB: runtime body + the codegen'd `$eval`); `Engine::Plan`
    instantiates it directly — no separate runtime instance, no
    import resolution against `"cel"`.  Statically-linked Programs
    also skip the wasi-libc command-mode wrapper chain that the
    dynamic path pays per call — on long arithmetic chains that is
    the difference between 78 µs and ~1 µs per Eval
    (`intAdd1000Terms`; see
    `doc/implementation-plan/rewrite/m28-bench-results.md`).  Use it
    when an expression is compiled once and evaluated many times on a
    latency-critical path.
  - **`LinkMode::kDynamic`** — the Program **imports** each helper
    from the `"cel"` module.  `Engine::Plan` instantiates
    `cel_runtime.wasm` as a separate instance in the same store and
    binds its exports so the imports resolve.  The Program stays
    small (a few KB — just the codegen'd `$eval`), and many cached
    Programs share one ~48 KB runtime.  Use it when you cache lots of
    distinct expressions or ship Program bytes over the wire and the
    artifact size matters more than per-call overhead.

```cpp
celwasm::CompilerOptions opts;
opts.optimize_level = 2;

// Self-contained Program (the default): runtime statically linked in.
opts.link_mode = celwasm::CompilerOptions::LinkMode::kStatic;
auto hot = compiler->Compile("a + b * 2", opts);

// Small Program: helpers imported from a shared runtime instance.
opts.link_mode = celwasm::CompilerOptions::LinkMode::kDynamic;
auto small = compiler->Compile("a + b * 2", opts);
```

The choice is invisible at run time: `Engine::Plan` detects which
shape it was handed by inspecting the module's import list and routes
accordingly — the Instance API (`Eval` / `PartialEval`) and the
results are identical either way (conformance runs byte-identical
under both modes).  Full design + tradeoff table:
`doc/implementation-plan/rewrite/m28-configurable-linking.md`;
production numbers: `m28-bench-results.md`.

## Running the suite

```bash
# Default fast suite — every gtest binary not tagged `manual`.
bazel test //...

# Full closeout gate — adds the manual e2e tests
# (wasmtime-driven, optimize_test, wat_runner, cel_runtime_wasm_test)
# and walks the whole cel-spec conformance corpus.
scripts/run_full_suite.sh

# Quick variant — skips conformance (the slow part).
scripts/run_full_suite.sh --quick
```

Per CLAUDE.md, **`bazel test //...` being green does NOT
mean a milestone is done** — the manual-tagged e2e tests carry the
load-bearing assertions and must be run explicitly via
`scripts/run_full_suite.sh`.  See
`doc/implementation-plan/per-component-test-coverage.md` for the
discipline.

## Perf

`doc/implementation-plan/rewrite/archive/bench-tree-readme.md` has the
full table.  Headline numbers as of
2026-05-15, darwin-arm64, `-c opt` build:

  - Kernel microbench leaves (`cel_int_add_at_vv`, `cel_int_eq_at_vv`,
    `cel_uint_to_int_at_v`, …): 7-10 ns/call.
  - Pipeline Compile: 248-395 us per CEL source expression.
  - Pipeline Plan: 240-253 us (wasmtime instantiate dominates).
  - Pipeline Eval steady-state: 160 ns (literal) to 11 us (20-term
    comparison chain).  Setting `optimize_level = 2` cuts the chain
    case to 5.4 us at +120% Compile cost.

## Pointers

  - Top-level design: `doc/implementation-plan/rewrite/design.md`
    + `doc/implementation-plan/rewrite/cel-host-surface.md`.
  - Per-milestone plans: `doc/implementation-plan/rewrite/m*.md`.
  - Testing checklist: `doc/implementation-plan/testing-checklist.md`.
  - Per-component test coverage: `doc/implementation-plan/per-
    component-test-coverage.md` — the closeout gate definition.
  - CEL language spec we honour: `doc/langdef.md`.
  - Repo-wide rules for changes: `CLAUDE.md` at the repo root.
