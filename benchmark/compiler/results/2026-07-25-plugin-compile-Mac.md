# plugin_compile_bench — 2026-07-25, Mac (darwin-arm64)

Compile-path plugin costs (m35: `Plugin::Load`, `Builder::Use`
decl registration, required-functions emission).  Eval-side dispatch
numbers live in `plugin_bench` (not re-run here).

Command:

```bash
bazel build -c opt //benchmark/compiler:plugin_compile_bench
bazel-bin/benchmark/plugin/plugin_compile_bench \
    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

Config: `-c opt`; timed compiles at `optimize_level = 2` (production
config) with default `link_mode = kStatic`; the `_Opt0` rows are the
explicit optimize-level comparison pair.  Plugin artifact: the
macro-built `demo_plugin.wasm` (3 decls: `greet`, `add`, `len`;
~50 KB).  5 repetitions, medians reported; all cv ≤ 1.3 %.

| benchmark | median wall | median CPU | note |
|---|---:|---:|---|
| `BM_PluginLoad` | 414 µs | 414 µs | 121 MiB/s; section walk + celfn parse + SHA-256 |
| `BM_CompilerBuild_NoLibrary` | 76 ns | 76 ns | builder + Build(), 2 int vars |
| `BM_CompilerBuild_UsePlugin` | 294 ns | 294 ns | + `Use(plugin)`: 3-decl registration ≈ +218 ns |
| `BM_Compile_NoLibrary` | 378.5 ms | 80.7 ms | `a + b`, no library |
| `BM_Compile_PluginDeclsUncalled` | 393.0 ms | 81.6 ms | `a + b`, plugin Use'd: +3.8 % wall for carrying decls |
| `BM_Compile_PluginCallAdd` | 394.4 ms | 82.2 ms | `add(a, b)`: live call site + 1 required-fn row |
| `BM_Compile_RequiredFnEmission_Opt2/1` | 396.3 ms | 82.7 ms | 1 declared, 1 called |
| `BM_Compile_RequiredFnEmission_Opt2/8` | 397.5 ms | 83.2 ms | 8 declared, 1 called: +1.2 ms |
| `BM_Compile_RequiredFnEmission_Opt0/1` | 57.8 ms | 55.1 ms | opt0: all imports survive |
| `BM_Compile_RequiredFnEmission_Opt0/8` | 57.5 ms | 54.8 ms | 8-row table: delta within noise |

Reading:

  - **`Plugin::Load` is ~415 µs on the ~50 KB demo plugin** —
    per-artifact, paid once at startup; negligible against a single
    production-config Compile (~380 ms).
  - **Decl registration is nanoseconds** (+218 ns at Build for the
    3-decl demo).  Carrying the decls costs ~3.8 % per Compile at
    opt2 (checker env + unconditional import install, later DCE'd).
  - **Required-functions emission is noise-level.**  7 extra declared
    fns add ~1.2 ms at opt2 (where the uncalled imports are optimized
    away and the emitted table keeps 1 row) and are within run noise
    at opt0 (where the table carries all 8 rows) — the §5.2
    post-optimize emission restructure did not add measurable
    compile cost.
  - The ~380 ms opt2 wall floor is the static-link production
    config (Binaryen O2 over the self-contained runtime+expr
    module); the opt0 rows show the same shapes at the ~58 ms
    hot-reload config.  Wall ≫ CPU at opt2 reflects Binaryen's
    worker threads (CPU column counts the main thread).
