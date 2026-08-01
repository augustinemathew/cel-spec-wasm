# Compiler overview

The compiler turns a CEL expression into a self-contained wasm module
that the host instantiates and evaluates. This page is the embedding
reference: lifecycle, API quickstart, artifact sizes, and options.

Prerequisites, build commands, and a first eval are in
[Getting started](user-guide/getting-started.md). The architecture —
roles, contracts, threading, repo layout — is in
[design/00-architecture.md](design/00-architecture.md); the pass
pipeline inside `Compile` is in [design/01-compiler.md](design/01-compiler.md).

## Lifecycle

The public surface mirrors three real caching boundaries. Design each
call site around them, not around `Compile()` alone.

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

## Quickstart

Your first embed — declare variables, compile, plan, eval — is in
[Getting started §4](user-guide/getting-started.md); the snippets
below cover the patterns beyond that. Deps for a `cc_binary`:
`//compiler:compiler`, `//eval:engine`, `//eval:instance`,
`//eval:activation`, `//eval:value`, `//shared:type`, and
`@com_google_absl//absl/log:absl_check`.

### Reusing each layer — the realistic shape

Each layer is independently cacheable. A real host hits Compile
rarely, Plan per request, Eval many times per Plan:

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

To run many Evals against one rule (a stream of inputs), keep the
Instance alive and call `Eval(activation)` in a loop — the per-Eval
arena rewinds automatically at the top of each call.

### Proto message variables

Declare the variable by fully-qualified message name; bind a real
`google::protobuf::Message` at Eval time.

```cpp
#include "testdata/e2e_fixture.pb.h"  // your .proto

celwasm::Compiler::Builder cb;
cb.DeclareVariable("c",
                   celwasm::CelType::Message("celwasm.testdata.Customer"));
auto compiler = std::move(cb).Build();
ABSL_CHECK_OK(compiler);

auto program = compiler->Compile("c.name + ' (' + string(c.age) + ')'");
ABSL_CHECK_OK(program);
// ... Engine::NewBuilder().Build() + engine->Plan(*program) as above ...

celwasm::testdata::Customer msg;
msg.set_name("Ada");
msg.set_age(36);

celwasm::Activation a;
a.Bind("c", celwasm::Value::Message(msg));

auto v = instance->Eval(a);
ABSL_CHECK_OK(v);
ABSL_CHECK_EQ(*v->AsString(), "Ada (36)");
```

`Value::Message(msg)` snapshots a const reference — keep `msg` alive
across the Eval; for ownership transfer use
`Value::OwnedMessage(std::unique_ptr<…>)`.

### Inspecting results

`Value::kind()` returns the `Kind` enum — `kBool`, `kInt`, `kUint`,
`kDouble`, `kString`, the aggregate kinds (see `eval/value.h`),
`kError` (a CEL evaluation error; read it with `AsError()`), and
`kUnknown` (surfaced by partial evaluation). The `AsX()` accessors
return an `absl::StatusOr<T>` that errors with `InvalidArgument` on a
kind mismatch.

### Saving and reloading a Program

A `Program` is pure data: wasm bytes plus the embedded `cel.abi`
custom section. No wasmtime state, no descriptor pool, no engine
handles.

- **Save**: `program.wasm_bytes()` returns a `Span<const uint8_t>` —
  copy it to disk, a cache, a database column, or the wire.
- **Reload**: `celwasm::Program(std::vector<uint8_t> bytes)`
  reconstructs the Program; then `Engine::Plan` + `Eval` as usual, no
  Compile needed. The constructor is non-validating — malformed bytes
  surface as a `FailedPrecondition` at `Engine::Plan`, not a crash.

The round-trip preserves (pinned by `e2e/program_roundtrip_test.cc`)
the complete compiled module, the declared-variable schema (encoded in
`cel.abi`), and any Binaryen optimization applied at Compile time. It
does not preserve host state: the `Engine`, the `Activation`, and the
protobuf descriptor pool — message types the program references must
be linked into the reload-side process, same as the compile side.

!!! warning "ABI versioning"
    The wasm bytes encode the runtime ABI implicitly via the symbols
    they import. If a runtime upgrade renames or removes an import,
    reloading old bytes fails at `Engine::Plan` with a missing-import
    error. Keep saved bytes paired with a runtime-version marker and
    recompile on runtime upgrade.

## Artifact sizes

Measured 2026-05-15 on darwin-arm64, `-c opt`. Reproduce via
`bazel run -c opt //benchmark/compiler:program_size_main`.

!!! note
    These are `LinkMode::kDynamic` numbers — the small expr module
    that imports the shared runtime. Under the default
    `LinkMode::kStatic`, the runtime is merged into every Program
    (~800 KB self-contained artifact). See [Link modes](#link-modes).

### Compiled program vs. CEL AST proto

For scale, each row compares our wasm module against the
`cel.expr.CheckedExpr` proto other CEL implementations cache:

| Expression                       | AST proto | wasm opt=0 | wasm opt=2 | opt2 / AST |
| -------------------------------- | --------: | ---------: | ---------: | ---------: |
| `42` (literal int)               |    36 B   |   2516 B   |   **136 B** | 3.78× |
| `"hello"` (literal string)       |    41 B   |   2524 B   |   **144 B** | 3.51× |
| `"hello, " + s` (string concat)  |   123 B   |   2557 B   |   **207 B** | 1.68× |
| `s.contains("world")`            |   136 B   |   2557 B   |   **209 B** | 1.54× |
| `int(string(123))` (conversion)  |   148 B   |   2531 B   |   **209 B** | 1.41× |
| `[1,2,3,4,5]` (list lit, 5 elt)  |   148 B   |   2669 B   |   **336 B** | 2.27× |
| `{"a":1, "b":2}` (map lit, 2 e.) |   152 B   |   2633 B   |   **301 B** | 1.98× |
| `type(x) == int` (type test)     |   176 B   |   2565 B   |   **231 B** | 1.31× |
| `a + b + c` (3-term arith)       |   204 B   |   2580 B   |   **191 B** | **0.94×** |
| `{...3 entries...}["b"]` (lookup)|   279 B   |   2745 B   |   **440 B** | 1.58× |
| 20-term `a<b && b<c && …` chain  |  3094 B   |   3339 B   |   **933 B** | **0.30×** |

**Always set `optimize_level = 2` when storing or shipping a
program** — unoptimized modules are dominated by import-table
boilerplate that Binaryen DCEs away. The crossover is at ~200 B of
AST: below that the AST proto wins (the wasm module has a fixed
~130 B floor of preamble, `cel.abi` section, and `$eval` header);
above it the wasm wins, and the margin grows with complexity — the
proto pays per-node overhead (node id, `type_map` and `reference_map`
entries) while the lowered wasm body costs 2-3 bytes per AST node.
**Takeaway:** cache the wasm bytes for any real expression — at most
~1 KB for typical workloads, and reload (`Program(bytes)` → `Plan`)
skips parsing, type-checking, and codegen entirely.

### Runtime module (one-time cost per process)

| Artifact                       | Size    | Notes |
| ------------------------------ | ------: | ----- |
| `cel_runtime.wasm`             | **49,611 B** (~48 KB) | Parsed once by `Engine::Builder::Build`.  Imported by every dynamic-mode expr module via `(import "cel" ...)`. |
| `libcel_runtime.a` (native)    | **128,296 B** (~125 KB) | The same C source linked natively (unit tests, host trampolines).  Larger because native code is less compact than wasm encoding. |

Both are built with `-O3 -flto` unconditionally (not user-tunable):
the runtime's inner loops are the hottest part of every Eval, and LTO
inlines leaf kernels across its per-topic `.c` files.

### In-memory C++ objects

The handles are small (`sizeof()` below); the storage they own is
what matters:

| Type                | sizeof | What it owns / points to |
| ------------------- | -----: | ------------------------ |
| `celwasm::Program`      |  24 B  | + `std::vector<uint8_t>` data (~140 B - 1 KB per expr at opt2; see table above). |
| `celwasm::Compiler`     |  24 B  | + `std::vector<VariableDeclaration>` (one per `DeclareVariable` call). |
| `celwasm::CompilerOptions` | 40 B | Plain-old-data, copy freely. |
| `celwasm::Value`        |  40 B  | Discriminated union; aggregates (List/Map/Message) own a shared_ptr to a backing. |
| `celwasm::Activation`   |  32 B  | + a `flat_hash_map<string, Value>` for bound variables. |
| `CelValue` (wire)   |  24 B  | Arena-resident; size pinned by `static_assert`.  The 24-byte slot every codegen-emitted load/store reads and writes. |

### Per-Instance memory

Each `Instance` owns a wasmtime store and linear memory. Wasm pages
are 64 KiB, so the minimum per-Instance footprint is ~128 KiB even
for a single-literal program — the budget to track when running many
concurrent Instances (share one Engine; `Plan` a fresh Instance per
request and drop it after). The low 8 KiB
(`CELWASM_RESERVED_LOW_MEMORY_BYTES`, `runtime/cel_layout.h`) holds
the expression's rodata and workspace; the per-Eval bump arena is a
64 KiB buffer (`CELWASM_ARENA_CAPACITY_BYTES`) malloc'd once per
Instance and rewound — not freed — by `arena_reset` on every Eval.
Full memory map: [design/03-abi-and-memory.md](design/03-abi-and-memory.md).

## Link modes

`CompilerOptions::link_mode` picks how the runtime kernel reaches the
Program:

- **`kStatic` (default)** — the runtime is merged into the Program at
  Compile time. Self-contained (~800 KB), no import resolution at
  Plan, and the fastest per-Eval shape: on long arithmetic chains the
  static path is the difference between 78 µs and ~1 µs per Eval
  (`intAdd1000Terms`). Use it when an expression compiles once and
  evaluates many times on a latency-critical path.
- **`kDynamic`** — the Program imports each helper from the `"cel"`
  module; `Engine::Plan` instantiates `cel_runtime.wasm` alongside it.
  Programs stay small (a few KB) and many cached Programs share one
  ~48 KB runtime. Use it when artifact size or one-runtime-many-exprs
  sharing matters more than per-call overhead.

The choice is invisible at run time: `Engine::Plan` detects the shape
from the module's import list, and results are identical either way
(conformance runs byte-identical under both modes). Full rationale:
[design/00-architecture.md §3](design/00-architecture.md#3-link-modes).

## Compiler options

Set via `celwasm::CompilerOptions` (`compiler/compiler.h`):

- **`optimize_level`** (default 0) — Binaryen optimization. 0 is a
  byte-identical no-op; 2 is the recommended production setting:
  ~2-3× the Compile cost, but roughly half the Eval time on
  chain-heavy bodies. Compile cost amortises across many Evals.
- **`link_mode`** (default `kStatic`) — see [Link modes](#link-modes).
- **`container`** — ident-resolution prefix, forwarded to the cel-cpp
  checker. Equivalent to cel-go's `container` option.

The knob-by-knob contract — including the concurrency hazard when
`optimize_level > 0` — is in
[design/01-compiler.md §9](design/01-compiler.md#7-public-surface-and-options).

## CLI

For a one-off check without writing C++, use the `cel` driver under
`tools/cel/`. Four subcommands: `eval`, `check` (parse + type-check
only), `compile` (emit wasm bytes), `generate` (function bindings).

```bash
bazel run //tools/cel:cel -- eval 'a * b' \
  --var 'a:int=6' --var 'b:int=7'                    # → 42
bazel run //tools/cel:cel -- compile '1 + 2' \
  --output /tmp/expr.wasm
```

`--var name:Type[=value]` declares (and optionally binds) a variable;
`--proto` / `--descriptor_set` load message schemas; `--O 0..3` is
the optimizer level. Full flag surface: `tools/cel/README.md`.

## Running the tests

```bash
bazel test //...              # fast suite — every non-`manual` gtest binary
scripts/run_full_suite.sh     # full gate — adds the manual e2e tests
                              # + the cel-spec conformance corpus
scripts/run_full_suite.sh --quick   # skips conformance (the slow part)
```

!!! note
    `bazel test //...` green is not the full story — the
    manual-tagged e2e tests (wasmtime-driven, `optimize_test`,
    `wat_runner`, `cel_runtime_wasm_test`) carry the load-bearing
    assertions and only run via `scripts/run_full_suite.sh`. See
    [design/06-testing-strategy.md](design/06-testing-strategy.md).

## Performance

Headline numbers, 2026-05-15, darwin-arm64, `-c opt`:

- Kernel microbench leaves (`cel_int_add_at_vv`, `cel_equals_at_vv`,
  `cel_uint_to_int_at_v`, …): 7-10 ns/call.
- Pipeline Compile: 248-395 µs per CEL source expression.
- Pipeline Plan: 240-253 µs (wasmtime instantiate dominates).
- Pipeline Eval steady-state: 160 ns (literal) to 11 µs (20-term
  comparison chain); `optimize_level = 2` cuts the chain case to
  5.4 µs at +120% Compile cost.

Methodology: [design/07-benchmarking.md](design/07-benchmarking.md).
Current comparative results are auto-published in `benchmark/README.md`.

## Further reading

- [Evaluator internals](design/02-evaluator.md) — Plan/Eval, host
  calls, marshalling.
- [User guide](user-guide/index.md) — custom functions, security
  model, FAQ.
- [CEL language definition](langdef.md) — the semantics we honour.
- [Contributing](contributing.md) — lint/format gate, dev workflow.
