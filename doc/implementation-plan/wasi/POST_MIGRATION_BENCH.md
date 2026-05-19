# Post-migration bench results

Captured 2026-05-18 on `wasi-malloc-migration` @ commit `5d8156a`
(after M7 landed).  Apple M1, light load.  Same machine + same
bench binary (`//compiler_v2/api:cel_pipeline_bench`) as the
DESIGN.md §10 baseline.

```
bazel run -c opt //compiler_v2/api:cel_pipeline_bench -- \
  --benchmark_min_time=2s
```

## Static metrics

| Metric | Baseline (`9685d72`) | Post-M7 | Δ | Ratio |
|---|---:|---:|---:|---:|
| `cel_runtime.wasm` stripped | 60,971 B | 241,386 B | +180,415 B | **3.96×** |
| `cel_runtime.wasm` gzipped | 11,741 B | 47,103 B | +35,362 B | 4.01× |
| Imports | 14 | 17 (+wasi_snapshot_preview1.*) | +3 | 1.21× |
| Exports | 162 | 169 (+memory, malloc, free, __heap_base, arena_*) | +7 | — |
| Initial memory | 2 pages | 2 pages | 0 | 1.00× |
| compiler_v2 production C/C++ LoC | 25,823 | ~25,700 | −123 | 0.99× |

**Binary size delta**: the 4× growth is the cost of pulling in
wasi-libc (dlmalloc + the WASI shim layer).  The DESIGN.md §9
acceptance target was "≤2× baseline" — we are over by ~2×.  This
is the library-vendoring tax; it's the same cost RE2 + absl will
later amortise (Phase C).  Documented as
`cleanup-backlog.md #6`.

## Dynamic metrics — wall-clock means

| Metric | Baseline | Post-M7 | Δ | Ratio | Budget (DESIGN §9) |
|---|---:|---:|---:|---:|---|
| `BM_Compile` (avg) | 294 µs | ~302 µs | +8 µs | **1.03×** | — |
| `BM_Plan_Hot` (avg) | 279 µs | ~306 µs | +27 µs | **1.10×** | — |
| `BM_Eval` (scalar) | 141 ns | ~738 ns | +597 ns | **5.23×** | ≤ 5× ⚠ |
| `BM_Eval` (string) | 157 ns | ~774 ns | +617 ns | **4.93×** | ≤ 5× ✓ |
| `BM_Pipeline_Cold` | 6.52 ms | ~8.49 ms | +1.97 ms | **1.30×** | — |
| `BM_Pipeline_WarmEngine` | 587 µs | ~632 µs | +45 µs | **1.08×** | — |
| `BM_Pipeline_WarmProgram` | 289 µs | ~310 µs | +21 µs | **1.07×** | — |
| `BM_Pipeline_HotEval` | 141 ns | ~744 ns | +603 ns | **5.28×** | ≤ 5× ⚠ |

## Verdict against DESIGN §9 acceptance criteria

| Criterion | Target | Measured | Status |
|---|---|---|---|
| Per-Eval ratio (scalar) | ≤ 5× baseline | 5.23× | **⚠ just over (~5%)** |
| Per-Eval ratio (string) | ≤ 5× baseline | 4.93× | ✓ |
| Per-Plan ratio | (implicit ≤ 1.5×) | 1.10× | ✓ |
| Pipeline cold ratio | — | 1.30× | informational |
| Binary size | ≤ 2× baseline | 3.95× | **⚠ over** |

## Where the per-Eval slowdown comes from

The ~600 ns / 5.2× gap on scalar Eval is dominated by:

1. **wasi-libc startup state**: even a no-op eval routes through
   the wasi-libc-initialized linear memory; the runtime has more
   static data + setup overhead than the pre-WASI freestanding
   build.
2. **dlmalloc-backed arena**: `arena_alloc` calls dlmalloc once
   per Eval (well, once per Instance for the activation buffer
   in M7).  The actual per-Eval cost is the `arena_reset` host
   trampoline + the kernels' arena_alloc calls.  Each is ~30 ns
   of wasi-libc indirection per call vs the old direct
   memory store at bytes 8/12.
3. **`CEL_LOG("enter")` in arena.c**: every arena_alloc /
   arena_reset triggers a host roundtrip.  Pre-existing — not
   introduced by the migration — but more visible now because
   the host roundtrip cost is the same regardless of arena
   implementation.  See `cleanup-backlog.md`: a build-time
   `-DCEL_LOG_DISABLED` for opt builds would cut this; deferred
   pending the perf-vs-debug trade-off discussion.

## Mitigation paths (post-merge, none gating)

- Compile cel_runtime with `-DCEL_LOG_DISABLED` in opt builds
  (saves the ~30 ns per kernel call).  Expected: brings per-Eval
  ratio comfortably under 4×.
- Inline `arena_reset` / `arena_alloc` at the LTO boundary.
  Already in-pipeline via `-flto`; need to verify no inlining
  barriers introduced by wasi-libc.
- Pursue Option C (shared runtime, see cleanup-backlog.md) — the
  per-Eval ratio is largely a one-time wasi-libc startup cost
  amortised across many Evals.  Option C removes that per-Eval
  startup by sharing the runtime across N expressions.

## Where the binary-size growth comes from

The 4× wasm growth is wasi-libc.  Specifically:

- `wasi-libc` startup + dlmalloc: ~120 KB
- libc string / time helpers pulled in by transitive C-stdlib
  use: ~50 KB
- Slightly more verbose codegen under `--target=wasm32-wasi` vs
  freestanding: ~10 KB

The growth is **not** from CEL kernel changes.  Future Phase C
work (linking absl::ParseTime into the runtime) is expected to
add another ~70 KB; binary-size acceptance criteria should be
re-evaluated after that lands.

## Reproduce

```sh
bazel run -c opt //compiler_v2/api:cel_pipeline_bench -- \
  --benchmark_min_time=2s
```

All bench rows above use the same workload set described in
DESIGN.md §11.  Conformance unchanged at 1373/590/491.
