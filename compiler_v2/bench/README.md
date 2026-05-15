# `compiler_v2/bench` — performance benchmarks

Two Google Benchmark binaries, both `manual`-tagged so they stay out of
`bazel test //compiler_v2/...`.  Run explicitly when you want numbers.

## How to run

Always build with `-c opt`.  Debug builds blow timing numbers up by
~10× and are useless as a baseline.

```bash
# Runtime-kernel microbenches (cel_arith / cel_compare / cel_convert
# / cel_string_ops / cel_3vl / cel_runtime aggregate kernels).
bazel run -c opt //compiler_v2/bench:kernel_bench

# End-to-end pipeline benches (Compiler::Compile / Engine::Plan /
# Instance::Eval).  Pre-stages compiler + program + instance outside
# the timed loop so the Eval-only numbers measure steady-state cost
# without re-paying compile / plan on every iteration.
bazel run -c opt //compiler_v2/bench:pipeline_bench
```

Useful flags:

  - `--benchmark_min_time=1.0s` — increase per-bench wall time (default
    is short; longer makes the noise floor tighter).
  - `--benchmark_filter=BM_Eval_` — run a subset.
  - `--benchmark_repetitions=5 --benchmark_report_aggregates_only=true`
    — get mean / median / stddev across multiple runs of each bench.

## Baseline numbers (2026-05-14, darwin-arm64, Apple M-series)

Captured at `--benchmark_min_time=0.05s` (smoke run; tighten with
repetitions when investigating a suspected regression).  Reported as
ns/call unless otherwise noted.

### Runtime kernel microbenches

| Bench                          | Time (ns) | Notes                                            |
| ------------------------------ | --------: | ------------------------------------------------ |
| `BM_IntAdd`                    |       8.3 | overflow-checked happy path                      |
| `BM_IntMul`                    |       8.9 | overflow-checked happy path                      |
| `BM_IntDiv`                    |       8.1 | happy path                                       |
| `BM_IntDivByZero`              |       8.2 | error-envelope fast-reject                       |
| `BM_DoubleAdd`                 |       8.2 | IEEE 754; no overflow check                      |
| `BM_IntEq`                     |       8.5 | same-kind comparison                             |
| `BM_NumericEqIntUint`          |       8.8 | cross-type ladder (`1 == 1u`)                    |
| `BM_NumericEqIntDouble`        |       9.6 | cross-type ladder (`1 == 1.0`)                   |
| `BM_StringEq/8`                |      10.9 | 0.70 GiB/s; `memcmp` on tiny operands            |
| `BM_StringEq/64`               |      24.3 | 2.46 GiB/s                                       |
| `BM_StringEq/4096`             |       967 | 3.95 GiB/s; saturates memcmp                     |
| `BM_BytesEq/8` /`/64` /`/4096` | 10.9 / 25 / 965 | tracks string_eq within noise              |
| `BM_MapLookupArenaHit`         |      12.9 | linear scan, hit at slot 0                       |
| `BM_MapLookupArenaMiss`        |      20.1 | linear scan, full walk                           |
| `BM_ListAtArena`               |      10.3 | bounds-check + slot load                         |
| `BM_ListEqDispatchArena`       |      32.5 | kDynamic dispatcher + arena fast path            |
| `BM_MapEqDispatchArena`        |      68.7 | kDynamic dispatcher + arena fast path            |
| `BM_AndBoolBool`               |       9.5 | bool×bool 3VL truth-table                        |
| `BM_OrBoolBool`                |       9.0 | bool×bool 3VL truth-table                        |
| `BM_UnknownMerge`              |      25.9 | 1-id+1-id merge + arena alloc                    |
| `BM_UintToInt`                 |       7.2 | overflow-check leaf                              |
| `BM_DoubleToInt`               |       7.2 | NaN + range check                                |
| `BM_StringToInt/1`             |       8.4 | hand-rolled parser, single digit                 |
| `BM_StringToInt/5`             |       9.4 |                                                  |
| `BM_StringToInt/19`            |      17.5 | max int64 digit count                            |
| `BM_IntToString`               |      18.6 | itoa + arena alloc                               |
| `BM_DoubleToString`            |      52.9 | fractional decomposition                         |
| `BM_StringConcat/8`            |      18.3 | arena alloc + copy                               |
| `BM_StringConcat/64`           |      18.7 | 6.4 GiB/s                                        |
| `BM_StringConcat/4096`         |       147 | 51 GiB/s (cache-resident copy)                   |
| `BM_StringContains/8`          |      16.2 | brute-force scan, hit at tail                    |
| `BM_StringContains/64`         |      84.4 |                                                  |
| `BM_StringContains/4096`       |      4726 | 0.83 GiB/s; linear naive search                  |

### Pipeline benches

| Bench                            | Time      | Notes                                          |
| -------------------------------- | --------: | ---------------------------------------------- |
| `BM_Compile_Literal`             |    253 us | floor for any `Compile` (parser + checker)     |
| `BM_Compile_ThreeTermArith`      |    266 us |                                                |
| `BM_Compile_TwentyTermCompare`   |    395 us | 20-term `a < b && b < c && ...` chain          |
| `BM_Compile_TypeOfEqInt`         |    262 us | `type(x) == int`                               |
| `BM_Compile_IntFromString`       |    248 us | `int(string(123))`                             |
| `BM_Compile_StructLiteral`       |    260 us | `Customer{name: "Ada"}`                        |
| `BM_Plan_Literal`                |    253 us | wasmtime instantiate                           |
| `BM_Plan_ThreeTermArith`         |    240 us | ~constant in body size                         |
| `BM_Eval_Literal`                |    160 ns | rodata-only; no kernel calls                   |
| `BM_Eval_Select`                 |    462 ns | proto field read via cel_host trampoline       |
| `BM_Eval_ThreeTermArith`         |    704 ns | 2× `cel_int_add` + activation marshal          |
| `BM_Eval_TwentyTermCompare`     | 10 973 ns | 20-term chain — biggest body in the table      |
| `BM_Eval_TypeOfEqInt`            |  1 031 ns | `type(x) == int`                               |
| `BM_Eval_CreateList`             |  1 901 ns | 5-element arena list literal                   |
| `BM_Eval_CreateMap`              |  1 364 ns | 2-entry arena map literal                      |
| `BM_Eval_ListAt_Arena`           |  2 091 ns | 5-element list literal + index                 |
| `BM_Eval_ListAt_Proto`           |    543 ns | `c.tags[2]` via cel_host trampoline            |
| `BM_Eval_MapLookup_Arena`        |  1 977 ns | 3-entry map literal + lookup                   |
| `BM_Eval_MapLookup_Proto`        |    576 ns | `c.metadata["b"]` via cel_host trampoline      |
| `BM_Eval_StructLiteral`          |    381 ns | `Customer{name: "Ada"}` (host-side build)      |
| `BM_Eval_IntFromString`          |    770 ns | `int(string(123))`                             |

The arena-vs-proto crossover (rows `BM_Eval_*At_Arena` /
`BM_Eval_*Lookup_Arena` vs `_Proto`) is worth a moment: at the e2e
level the arena variants are ~3-4× SLOWER than the proto variants,
because the arena form rebuilds the literal aggregate on every Eval
(CreateList + ListAt costs add), while the proto form just dereferences
a CelValue that was marshalled in at Activation-bind time.  The kernel
microbench `BM_MapLookupArenaHit` at 13 ns is the apples-to-apples
arena lookup cost in isolation; the e2e number folds in the literal
materialisation cost the codegen emits before the index op.

### Caching boundaries we exercise

The pipeline bench is structured around the three caches a real host
gets to amortise:

  - **`cel::Compiler`** — pure data after `Builder::Build()`.  Construct
    once per declared-variable set, reuse for every `Compile(source)`
    call.  Each `BM_Compile_*` does this.
  - **`cel::Program`** — wasm bytes + ABI.  Reuse across many `Plan`
    calls when re-instantiating per-request.  `BM_Plan_*` measures the
    incremental cost of a fresh `wasmtime_module_new` + `instantiate`
    given a pre-compiled `Program`.
  - **`cel::Instance`** — wasmtime store + memory + bound exports.
    Reuse across many `Eval(activation)` calls — `cel_reset` rewinds
    the arena at the top of each Eval so the instance is stateless
    between calls.  Each `BM_Eval_*` does this.

If you suspect a regression in any of those layers, the matching
`BM_Compile_` / `BM_Plan_` / `BM_Eval_` numbers should localise it
before you start `perf`-ing.

### Binaryen `optimize_level` trade-off (2026-05-15)

Each `_Opt2` row compiles the same expression at `CompilerOptions.
optimize_level = 2` (runs Binaryen's canonical `wasm-opt -O2` pass
list: DCE + constant folding + simplify-locals + vacuum + merge-blocks
+ reorder-functions etc., shrink-level pinned at 0).  Default is still
0 (byte-identical output); production callers should set 2 when the
expression body has nontrivial structure.

| Bench                                | Default | Opt2  | Delta             |
| ------------------------------------ | ------: | ----: | ----------------- |
| `BM_Compile_ThreeTermArith`          |  255 us | 662 us | **Compile +159%** |
| `BM_Compile_TwentyTermCompare`       |  381 us | 842 us | **Compile +121%** |
| `BM_Eval_ThreeTermArith`             |  716 ns | 696 ns | -3% (within noise) |
| `BM_Eval_TwentyTermCompare`          | 11186 ns | 5414 ns | **Eval -52%**    |

Reading the table: opt2 is a clear win for chain-heavy bodies — the
20-term compare chain pays ~2.2× the Compile cost ONCE in exchange
for cutting every Eval roughly in half.  Short bodies (3-term arith)
have nothing for the optimizer to fold; opt2 is a wash on Eval and
pure Compile-time penalty.

Production rule of thumb: enable opt2 unconditionally on the request
path (Compile is amortised across many Evals via `Engine::Plan`
caching); disable opt2 for one-off / hot-reload compiles where Compile
latency dominates.

## How to interpret a regression

  - **Kernel microbenches.**  A >10% slowdown on a single kernel
    without an architectural change is a real regression worth a
    bisect.  A <5% drift between two runs on the same machine is
    noise — measurement variance from CPU thermal throttling, other
    processes on the box, sandbox / load-average noise; tighten with
    `--benchmark_repetitions=10 --benchmark_report_aggregates_only=true`
    and compare medians.
  - **Pipeline benches.**  Compile / Plan numbers move at the ~10%
    level with checker / codegen changes (more passes, more
    Binaryen IR built, etc.) — that's expected, not a regression in
    itself.  The Eval-only numbers are the tight ones: a 10% slowdown
    on `BM_Eval_ThreeTermArith` is meaningful, because that bench's
    body is dominated by 2 kernel calls + the wasmtime trampoline
    overhead.  A 2× regression on an Eval bench without a matching
    regression in the equivalent kernel microbench points at the
    instantiate / call / unwind path, not at codegen.

When in doubt: run the matching kernel bench too.  A pipeline-level
regression that doesn't show up in any kernel is the wasmtime
trampoline or the host marshal path; a regression that DOES show up
in a kernel is the kernel's fault.

## Future work

  - **wasm32-side bench.**  Google Benchmark doesn't trivially link
    freestanding; the wasm32 cross-compile path has no `<chrono>` or
    `clock_gettime`.  Native-host numbers are what the rewrite has so
    far; a wasm32 bench would need a thin in-house timer + a
    wasmtime-side host probe and is out of scope until the kernel
    perf becomes a load-bearing question (the host trampoline
    overhead dominates pipeline cost today, per the table above).
  - **Comprehension benches.**  Comprehensions ship at M11; once they
    do, the matching `BM_Eval_Comprehension_*` rows go into
    `pipeline_bench.cc` next to the existing list / map cases.
  - **Variance-stable runs.**  CI doesn't have a bench mode yet — the
    numbers here come from one-off `bazel run -c opt`.  When perf
    becomes load-bearing, a stable-environment CI lane (or a
    `--benchmark_repetitions`-driven daily run) is the natural next
    step.
