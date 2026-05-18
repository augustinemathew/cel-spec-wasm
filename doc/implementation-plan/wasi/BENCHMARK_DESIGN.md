# Benchmark design — compiler_v2 vs compiler_v3 (WASI/malloc)

Status: design — drafted 2026-05-17.  Companion to
`HANDOFF.md` and `WORK_PLAN.md`.

**Purpose.**  Validate that the simplification dividend
from the WASI/malloc migration (gone: bump arena, cursor
slot, host string arena, cel_reset prologue, mem_size
threading) doesn't cost more per-Eval latency than is
acceptable.

This document specifies:

  - The workload set (13 representative expressions).
  - The metrics captured.
  - The harness shape.
  - The output format and acceptance criteria.

## 1 Workload set

Each row covers a distinct codegen path or kernel family.
Listed roughly by complexity.

| # | Source | Bound vars | Exercises | Milestone gated |
|---|---|---|---|---|
| 1 | `42` | — | kConst, pure rodata | M1 |
| 2 | `true && false` | — | kCall(`_&&_`), short-circuit | M5 |
| 3 | `x` | `x: int` | kIdent, activation marshalling (scalar) | M2 |
| 4 | `x + y` | `x,y: int` | kCall(arith), 2 kIdent | M5 |
| 5 | `'foo' + 'bar'` | — | string concat (heap allocation in result) | M5C |
| 6 | `s.contains('hello')` | `s: string` | kCall receiver, string ops | M5C |
| 7 | `msg.field` | `msg: Customer` | kSelect, proto field read trampoline | M3 |
| 8 | `msg.scopes.exists(v, v == 'admin')` | `msg: Customer` | comprehension, repeated-field iter | M5b (pending) |
| 9 | `[1, 2, 3].size()` | — | list literal, list size | M4 |
| 10 | `{'a':1,'b':2}.size()` | — | map literal, map size | M3 |
| 11 | `timestamp('2026-05-17T00:00:00Z') > now` | `now: timestamp` | M7B time arithmetic | M7B |
| 12 | `int(msg.f) > 0` | `msg: Customer` | type conversion + arith | M10 |
| 13 | `cel.bind(n, msg.count, n > 0 && n < 100)` | `msg: Customer` | cel.bind degenerate comprehension | M5b (pending) |

Note: rows 8 and 13 depend on M5 comprehensions follow-on
shipping first.  If the migration starts before M5b lands,
substitute simpler test cases — but loss of comprehension
coverage is real.

Each row also has a **golden output** (the expected
`cel::Value`) pre-computed and asserted on every run to
catch correctness regressions.

## 2 Metrics captured per (workload, compiler)

| Metric | Definition | Tool |
|---|---|---|
| `compile_ns` | Wall time of `Compiler::Compile(src)` — parse → check → resolve → layout → module → lower → assemble. | google/benchmark |
| `plan_cold_ns` | First `Engine::Plan(program)` invocation — includes parsing the wasm bytes wasmtime hasn't seen before. | google/benchmark |
| `plan_warm_ns` | Steady-state `Plan` after wasmtime has seen the program once. | google/benchmark |
| `eval_ns` | `Instance::Eval(activation)` — wasmtime call + result decode.  Mean over ≥1000 iterations. | google/benchmark |
| `eval_p99_ns` | Same call, 99th percentile.  Catches GC-like pauses in dlmalloc. | google/benchmark histograms |
| `memory_initial_kb` | `wasmtime_memory_data_size` after first eval. | wasmtime API |
| `memory_peak_kb` | Peak `wasmtime_memory_data_size` across 1000 evals. | wasmtime API |
| `allocations_per_eval` | New counter, instrumented in both runtimes.  Number of `cel_alloc`/`malloc` calls per eval. | runtime-side counter |
| `binary_size_bytes` | `cel_runtime.wasm` stripped size. | filesystem |
| `binary_size_gz_bytes` | `cel_runtime.wasm` gzipped. | filesystem |

### Why two `plan_*` numbers

`plan_cold` includes wasmtime's parse cost for the
expression module.  Under WASI/malloc, the runtime
binary is bigger, but the per-expression bytes are the
same — the cold delta should be small.

`plan_warm` excludes module parsing — it's per-Plan cost
that the embedder actually pays at request-time.

## 3 The harness shape

A new bench target:

```
//compiler_v3/bench:v2_vs_v3_bench
```

```cpp
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v3/api/compiler.h"
#include "compiler_v3/api/engine.h"
#include "benchmark/benchmark.h"

namespace v2 = cel::v2;
namespace v3 = cel::v3;

struct Workload {
  std::string name;
  std::string source;
  cel::Activation activation;
  cel::Value expected;
};

const std::vector<Workload>& kWorkloads();  // 13 rows from §1

// Helper: assert v2 and v3 produce identical results.
void AssertSemanticParity(const Workload& w) {
  auto v2_compiler = v2::Compiler::NewBuilder().Build();
  auto v3_compiler = v3::Compiler::NewBuilder().Build();
  auto v2_engine = v2::Engine::NewBuilder().Build();
  auto v3_engine = v3::Engine::NewBuilder().Build();
  auto v2_program = v2_compiler->Compile(w.source);
  auto v3_program = v3_compiler->Compile(w.source);
  auto v2_inst = v2_engine->Plan(*v2_program);
  auto v3_inst = v3_engine->Plan(*v3_program);
  auto v2_result = v2_inst->Eval(w.activation);
  auto v3_result = v3_inst->Eval(w.activation);
  ABSL_CHECK(*v2_result == *v3_result)
      << "semantic divergence on workload " << w.name;
  ABSL_CHECK(*v2_result == w.expected)
      << "golden mismatch on workload " << w.name;
}

// For each metric × each compiler × each workload — a BENCHMARK.
//
// Example (per workload, per compiler, eval):
BENCHMARK_TEMPLATE_DEFINE_F(VxBench, V2_Eval, v2::Engine)(
    benchmark::State& state) {
  // Setup: compile + plan once.  Loop: Eval only.
  auto engine = v2::Engine::NewBuilder().Build();
  auto compiler = v2::Compiler::NewBuilder().Build();
  const auto& w = kWorkloads()[state.range(0)];
  auto program = compiler->Compile(w.source);
  auto inst = engine->Plan(*program);
  for ([[maybe_unused]] auto _ : state) {
    auto v = inst->Eval(w.activation);
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK_REGISTER_F(VxBench, V2_Eval)->DenseRange(0, 12);

// Same shape for V3_Eval, V2_Plan_Hot, V3_Plan_Hot, etc.
```

### Methodology constraints

  - **Run on a quiet machine.**  No browser, no IDE, no
    background jobs.  Benchmark machine should be the dev
    box rebooted clean, or a CI box dedicated to bench.
  - **Pin to a single CPU core** if thermal variance is an
    issue (`taskset -c 1 ./bench`).  Apple Silicon
    occasionally shifts between performance and efficiency
    cores — bind to a P-core.
  - **≥ 1000 iterations per measurement** for stable
    timing at the ns scale.  google/benchmark's
    `--benchmark_min_time=5s` is a good default.
  - **Repeat 3× and take median** — guards against
    one-off noise.
  - **Capture stddev / p99** — not just mean.  Per-Eval
    is where outliers matter (dlmalloc's geometric heap
    grow occasionally costs ~100µs on the unlucky call).

## 4 Output format

A CSV like:

```
workload, metric,                  v2_ns,    v3_ns,    delta_pct
literal_42,         eval_mean,        15,       42,       +180%
literal_42,         eval_p99,         28,       180,      +542%
literal_42,         memory_initial,   131072,   86016,    -34%
literal_42,         allocations,      0,        2,        new metric
...
```

Plus a markdown summary in `RESULTS.md`:

```markdown
## Per-Eval latency (mean ns)

| Workload | v2 | v3 | delta |
|---|---:|---:|---:|
| literal_42 | 15 | 42 | +2.8× |
| bool_short_circuit | 22 | 58 | +2.6× |
| ident_x | 31 | 78 | +2.5× |
| ... |

## Per-Instance memory (initial KB)

| Workload | v2 | v3 | delta |
|---|---:|---:|---:|
| literal_42 | 128 | 84 | −34% |
| ...
```

## 5 Acceptance criteria

The migration is **acceptable** if all of:

  - [ ] **Correctness**: every workload produces an
    identical `cel::Value` under v2 and v3.  This is the
    bench's auto-assert; no row may skip it.
  - [ ] **Conformance**: `bazel run
    //compiler_v3/conformance:run_conformance` reports
    1144 PASS (matches v2 baseline; see
    `compiler_v2/conformance/README.md`).
  - [ ] **Eval cost ratio ≤ 5×** for every workload.
    Today's per-Eval ranges ~15-200 ns; v3 ceiling
    therefore 75-1000 ns.  Beyond 5× requires explicit
    user sign-off.
  - [ ] **Eval p99 ratio ≤ 10×**.  dlmalloc geometric
    grow events count toward this; if p99 is much worse
    than mean, that's a real signal.
  - [ ] **Per-Instance memory ≤ 1.5× v2**.  Expected:
    drops (no host string arena, runtime owns memory and
    grows organically).
  - [ ] **Compile cost within ±10%**.  Compiler is
    unchanged; this would indicate a measurement error.
  - [ ] **Binary size**: v3 < 1.5× v2.  Today's runtime
    is ~80 KB; v3 expected ~120-150 KB (wasi-libc +
    dlmalloc).
  - [ ] **RE2 smoke-test** (one bonus workload, after the
    main 13): `'hello' matches '^[a-z]+$'` compiles, plans,
    evals to `true`.  Proves the simplification's
    downstream payoff (library vendoring works).

The migration is **rejected** if any of:

  - Per-Eval cost ratio > 5× on any workload.
  - Eval p99 > 10× on any workload.
  - Memory baseline > 1.5×.
  - Any conformance row regresses.
  - Any e2e test in `compiler_v3/e2e/` regresses.

## 6 What the bench is NOT for

  - **Not** a head-to-head with cel-cpp's native evaluator.
    Different problem.
  - **Not** for measuring compile time of cel-cpp parsing
    (we use cel-cpp's parser unchanged in both).
  - **Not** for measuring conformance pass count
    (correctness is a precondition, not a measurement).
  - **Not** for measuring RE2 specifically (only the smoke
    test).  RE2-vs-host-regex perf is a separate
    investigation.

## 7 If acceptance fails — diagnostic protocol

The likely failure mode: per-Eval cost ratio > 5× on
literal-heavy workloads (rows 1-3) because the
malloc+free pair is ~100 ns dominating a 15-30 ns eval.

Diagnostic order:

1. **Profile a per-Eval cycle** — what fraction is
   malloc vs the wasm call vs the result decode?  If
   malloc is the entire delta, that's expected; if other
   overhead crept in, fix that.
2. **Switch to per-Instance mspace_reset** (cached
   mspace, just reset between evals instead of destroying
   + recreating).  Often gets per-Eval back to ~30 ns.
3. **Eval-prologue elision** — codegen detects "this
   expression makes 0 dynamic allocations" and skips
   the workspace malloc.  Knocks ~30 ns off rows 1-2.
4. **If still > 5×** — that's the architectural cost.
   Decide whether to ship anyway (simplification wins) or
   roll back.

## 8 Test methodology footnote

The "Apple Silicon performance vs efficiency core" caveat
is real.  When running:

```sh
sudo nice -n -20 taskset -c 0 ./bazel-bin/compiler_v3/bench/v2_vs_v3_bench \
  --benchmark_min_time=5s --benchmark_repetitions=3
```

…ensure the kernel doesn't migrate the bench process.  On
macOS, `taskset` isn't native — use
`renice`+`high-perf-mode` or simply close everything else.

Sample variance > 5% on per-Eval = method problem, not a
signal.
