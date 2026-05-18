# Baseline benchmark — pre-WASI-migration

Captured 2026-05-18 on branch `wasi-malloc-migration`, before
any migration code lands.  This is the "before" snapshot the
in-place migration measures against.

## 1 Provenance

  - **Branch**: `wasi-malloc-migration` @ `9685d72` (fork of
    `master`; no migration code yet).
  - **Bench target**: `//compiler_v2/api:cel_pipeline_bench`.
  - **Bazel config**: `-c opt` (full release optimisation).
  - **Bench flag**: `--benchmark_min_time=2s` for the main run,
    `--benchmark_min_time=1s` for the WarmProgram + HotEval
    follow-up.
  - **Machine**: Apple Silicon M1 (10 cores), macOS Darwin
    25.3.0, Xcode 17 clang for native build / brew llvm 21
    for wasm cross-compile.
  - **CPU pinning**: not pinned (machine was idle but had
    background load ~2.2-2.5 load average).
  - **Reproducibility**: ±5% expected on per-Eval; ±10% on
    `BM_Pipeline_Cold` (cold-start variance from wasmtime
    engine construction).

## 2 Static metrics (one-shot)

| Metric | Value |
|---|---:|
| `compiler_v2/` production C/C++ LoC | **25,823** |
| `compiler_v2/` C/C++ LoC including tests/benches | **56,294** |
| `cel_runtime.wasm` size (stripped, `-c opt`) | **60,971 bytes** |
| `cel_runtime.wasm` gzipped | **11,741 bytes** |
| `cel_runtime.wasm` imports | **14** (1 cel_env + 12 cel_host + memory) |
| `cel_runtime.wasm` exports | **162** (incl. memory) |
| Initial wasm memory | **2 pages = 131,072 bytes** |

## 3 Compiler / Engine setup costs

These are amortized one-shot, not per-request.

| Benchmark | Wall ns | CPU ns | Iters | Notes |
|---|---:|---:|---:|---|
| `BM_Compiler_Build` | **2.98** | 2.98 | 938 M | Nearly no-op; Compiler::Build is just struct allocation today |
| `BM_Engine_Build` | **5,730,165** (5.73 ms) | 654,556 (0.65 ms CPU) | 4,535 | Parses cel_runtime.wasm + creates wasmtime engine.  Most of this is wasmtime engine setup (Cranelift). |

The `Engine_Build` wall time (5.73 ms) is **dramatically
higher** than the `~167µs` figure quoted in
`cel_pipeline_bench.cc`'s file header.  The ratio (~34×)
suggests either the header comment is stale or my machine /
wasmtime version is dramatically slower than the original
measurement.  Either way, **trust the numbers in this doc,
not the file header**.

## 4 Per-source compile cost

`BM_Compile` measures the full `Compiler::Compile(src)`
pipeline: parse → check → resolve → layout → module → lower →
assemble.  Wasm is produced as bytes, no wasmtime touched.

| Input | Wall ns | CPU ns |
|---|---:|---:|
| `42` (int) | 294,242 | 257,231 |
| `true` (bool) | 289,530 | 254,301 |
| `3.14` (double) | 293,750 | 257,704 |
| `"hello"` (string) | 295,843 | 259,697 |
| `null` | 298,328 | 260,430 |

**~290 µs per Compile** across all 5 input kinds.  Shape-
agnostic at this level — string vs scalar adds < 5 µs.

## 5 Per-program Plan (instantiation) cost

`BM_Plan_Hot` is what an embedder pays *per request* when
instantiating a fresh `Instance` from a cached `Program`.
Hot path: store + memory + linker + bind cel.memory +
instantiate runtime + bind runtime exports + parse expr
bytes via `wasmtime_module_new` + instantiate expr +
lookup `eval`.

| Input | Wall ns | CPU ns |
|---|---:|---:|
| `42` | 277,658 | 156,299 |
| `true` | 279,304 | 156,828 |
| `3.14` | 276,871 | 156,191 |
| `"hello"` | 279,291 | 156,750 |
| `null` | 280,675 | 156,978 |

**~280 µs per Plan_Hot** wall, **~156 µs CPU**.  The
~120 µs wall/CPU gap is wasmtime-internal blocking (likely
JIT cache lookup or memory allocation latency).

Again, much higher than the `~12 µs` header claim.
**Header is stale.**

## 6 Per-eval cost

`BM_Eval` is the hot loop.  Setup is reused: a pre-`Plan`'d
`Instance` is held; the loop body is one `Instance::Eval()`
call plus `CelValue` decode.

| Input | Wall ns | CPU ns |
|---|---:|---:|
| `42` | 141 | 141 |
| `true` | 141 | 141 |
| `3.14` | 142 | 142 |
| `"hello"` | 157 | 157 |
| `null` | 141 | 141 |

**~141 ns per Eval** for scalars, **~157 ns** for string
(extra cost is the span-payload decode from linear memory).

The agent assessment in `AGENT_ASSESSMENT.md §4.7` flagged
the bench header's `Eval ~tens of ns` claim as suspicious;
this measurement confirms the suspicion.  The real number
is **141 ns**, which is more in line with what
`wasmtime_func_call` dispatch + result decode should cost.
The header comment is wrong by an order of magnitude.

## 7 End-to-end pipeline costs

Composite benches that measure the full user-facing path
under different reuse profiles.

| Benchmark | Wall ns | CPU ns | Reuse |
|---|---:|---:|---|
| `BM_Pipeline_Cold/0` | 6,516,125 (6.5 ms) | 1,301,054 | New Compiler + Engine + Program + Instance + Eval per iter |
| `BM_Pipeline_WarmEngine/0` | 586,940 (587 µs) | 426,552 | Compiler + Engine reused; new Program + Instance + Eval per iter |
| `BM_Pipeline_WarmProgram/0` | 288,650 (289 µs) | 161,805 | Compiler + Engine + Program reused; new Instance + Eval per iter |
| `BM_Pipeline_HotEval/0` | 141 | 141 | Everything reused; only Eval per iter |

Same ratio across all 5 input kinds (range varies ±2%).

**Headline takeaway**: per-request cost depends entirely on
what you're willing to reuse.  Reusing the `Engine` saves
~6 ms (the wasmtime engine setup); reusing the `Program`
saves the parse-and-instantiate cycle; reusing the `Instance`
costs only the 141 ns Eval.

## 8 Computed deltas (for future WASI-migration comparison)

These are the values the migration's "after" run will be
compared against.  Numbers below the `===` line will be
filled in by the post-migration BASELINE_BENCH.md companion
doc.

```
Metric                    | Baseline (v2) | WASI (v3) | Delta
==========================|===============|===========|========
cel_runtime.wasm size     | 60,971 B      | TBD       | TBD
cel_runtime.wasm gzipped  | 11,741 B      | TBD       | TBD
Initial memory pages      | 2 (128 KB)    | TBD       | TBD
BM_Compiler_Build         | 3 ns          | TBD       | TBD
BM_Engine_Build           | 5.73 ms       | TBD       | TBD
BM_Compile (avg)          | 294 µs        | TBD       | TBD
BM_Plan_Hot (avg)         | 279 µs        | TBD       | TBD
BM_Eval (scalar)          | 141 ns        | TBD       | TBD
BM_Eval (string)          | 157 ns        | TBD       | TBD
BM_Pipeline_Cold          | 6.52 ms       | TBD       | TBD
BM_Pipeline_WarmEngine    | 587 µs        | TBD       | TBD
BM_Pipeline_WarmProgram   | 289 µs        | TBD       | TBD
BM_Pipeline_HotEval       | 141 ns        | TBD       | TBD
```

## 9 Limits of this baseline

  - **Workload is literal-only.**  All 5 inputs are
    rodata-resident scalars/strings.  Arithmetic, bound
    variables (`x + 1`), proto select, comprehensions are
    not exercised here.  The migration's correctness gate
    is conformance (1,144 PASS); this bench only validates
    the very-hot-path.  Per `BENCHMARK_DESIGN.md §1`, a
    13-row expanded workload is the proper comparison;
    that's a deliverable of Phase 7.
  - **Per-Eval numbers don't yet include allocation-heavy
    paths.**  Today's bump arena makes a literal eval do
    zero `cel_alloc` calls; under WASI/malloc those same
    evals will do at least one `malloc` for the workspace
    region.  The 5× ratio budget in
    `BENCHMARK_DESIGN.md §5` is meant to cover that.
  - **No `--benchmark_repetitions` flag.**  Single
    measurement per row.  Variance is best-effort.  Phase 7
    should re-run with `--benchmark_repetitions=3` for
    stable numbers in the comparison doc.
  - **`BM_Engine_Build` measured 5.73 ms** — the bench
    file header claims 167 µs.  Investigate before reading
    too much into the absolute number; might be macOS / M1
    specific (the header was likely written on a different
    machine), or might be wasmtime version drift.

## 10 Reproducing

```sh
git checkout wasi-malloc-migration
bazel run -c opt //compiler_v2/api:cel_pipeline_bench \
  -- --benchmark_min_time=2s
```

For a quick smoke check (single iteration per row,
~10 seconds total):

```sh
bazel run -c opt //compiler_v2/api:cel_pipeline_bench \
  -- --benchmark_min_time=0.1s
```

For the migration comparison after Phase 7:

```sh
# Run on the same machine, same load.
bazel run -c opt //compiler_v2/api:cel_pipeline_bench \
  -- --benchmark_min_time=2s \
     --benchmark_repetitions=3 \
     --benchmark_format=json > post_migration.json
# Compare against this doc's numbers.
```
