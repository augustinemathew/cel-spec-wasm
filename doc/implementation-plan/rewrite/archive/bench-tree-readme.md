# ARCHIVED — `bench/` tree README (final state, 2026-06-11)

> The top-level `bench/` tree was dissolved on 2026-06-11.  Eval-level
> benches now live as corpus cells under `benchmark/eval/corpus/`;
> Compile/Plan benches were ported to `benchmark/compiler/`; the kernel
> microbenches moved to `benchmark/kernel/`; the foreign-component
> bench moved to `benchmark/component/`.  This file is preserved
> verbatim below because it carries dated baseline tables
> (2026-05-14/15/18) and the build-configuration rationale those
> baselines were measured under.

# `bench` — performance benchmarks

Two Google Benchmark binaries, both `manual`-tagged so they stay out of
`bazel test //...`.  Run explicitly when you want numbers.

## How to run

Always build with `-c opt`.  Debug builds blow timing numbers up by
~10× and are useless as a baseline.

```bash
# Runtime-kernel microbenches (cel_arith / cel_compare / cel_convert
# / cel_string_ops / cel_3vl / cel_runtime aggregate kernels).
bazel run -c opt //bench:kernel_bench

# End-to-end pipeline benches (Compiler::Compile / Engine::Plan /
# Instance::Eval).  Pre-stages compiler + program + instance outside
# the timed loop so the Eval-only numbers measure steady-state cost
# without re-paying compile / plan on every iteration.
bazel run -c opt //bench:pipeline_bench
```

Useful flags:

  - `--benchmark_min_time=1.0s` — increase per-bench wall time (default
    is short; longer makes the noise floor tighter).
  - `--benchmark_filter=BM_Eval_` — run a subset.
  - `--benchmark_repetitions=5 --benchmark_report_aggregates_only=true`
    — get mean / median / stddev across multiple runs of each bench.

## Build configuration

Three orthogonal axes affect every number in this file:

  - **Runtime build flags.**  As of 2026-05-15 both the native
    `cc_library` and the wasm32 cross-compile build with `-O3 +
    -flto`.  Pre-2026-05-15 the build was `-O2` (no LTO).  LTO is
    genuinely load-bearing after the `cel_runtime.c` split shipped
    2026-05-14: the per-topic `.c` files (cel_arith / cel_compare /
    cel_3vl / cel_convert / cel_string_ops / cel_make / cel_memory /
    cel_log / cel_type) only cross-inline through LTO.  After
    2026-05-18 the wasm32 build uses wasi-sdk + dlmalloc instead of
    a freestanding fixed-cursor arena (see
    `doc/implementation-plan/rewrite/wasi/DESIGN.md`).
  - **`CEL_LOG_DISABLED`** (gated by `config_setting "opt_mode"` in
    `runtime/BUILD.bazel`).  Every public runtime helper
    begins with `CEL_LOG("enter")`, which on the wasm runtime is a
    host-import trampoline that ends in `fprintf(stderr, ...)`.  In
    `-c opt` builds the define is active and `CEL_LOG` expands to
    `((void)0)`; in `-c dbg` / `-c fastbuild` the macro is live so
    the dead-code audit (see `cel_log.h`'s opening comment) still
    works.  Effect on Eval: 1.4×–5.7× faster across the
    aggregate-heavy rows.  Bench numbers below are the opt-build
    numbers.
  - **Binaryen `optimize_level`**
    (`cel::CompilerOptions::optimize_level`).  Runs the canonical
    `wasm-opt -O<n>` pass list on the emitted expr module before
    Cranelift sees it.  Default `0` (no-op, byte-identical output);
    production callers typically want `2` — see the trade-off
    table further down.

## How JIT compilation fits in

`cel::Engine::Plan` is where Cranelift translates wasm bytes to
native machine code (~240–300 µs of the per-Plan cost).  Every
`$eval` body and every imported `cel_*` helper is native code by
the time `Instance::Eval()` runs — no interpreter loop in the hot
path.  Concretely:

  - **Kernel µbenches** (`BM_IntAdd` etc.) bypass wasmtime entirely
    — they link the native `cc_library` and call kernels directly.
    These numbers are the raw kernel cost; the delta vs the
    matching Eval bench is the trampoline + codegen wrapper, NOT
    interpretation overhead.
  - **Pipeline Eval benches** (`BM_Eval_*`) call through
    wasmtime's host-to-wasm trampoline into Cranelift-emitted
    native code that in turn calls the imported `cel_*` helpers.

No Winch / Pulley / AOT cache today — pure Cranelift at the
default "speed" setting.  `Engine::precompile_module` + caching
the module bytes is the natural lever if Plan cost ever becomes
load-bearing (per `engine.h:24`'s existing docstring).

## Baseline numbers (2026-05-15, darwin-arm64, Apple M-series)

Captured at `--benchmark_min_time=0.05s` (smoke run; tighten with
repetitions when investigating a suspected regression).  Reported as
ns/call unless otherwise noted.

### Runtime kernel microbenches

Two columns — **2026-05-14 (`-O2`, no LTO)** and **2026-05-15
(`-O3 + -flto`)** — make the runtime-flag delta visible.  Most
leaf kernels dropped 60–80% on the upgrade.  LTO inlines the
static-inline helpers in `cel_internal.h` (`poison` /
`absorb_3vl_*` / `write_*` / `require_kinds`) into kernel call
sites AND lets the per-topic `.c` files cross-inline; `-O3`
widens loop / branch optimization aggressiveness for the parse
loops + `utf8_valid`.

| Bench                          | -O2 (ns) | -O3 + -flto (ns) | Δ      | Notes                                            |
| ------------------------------ | -------: | ---------------: | -----: | ------------------------------------------------ |
| `BM_IntAdd`                    |      8.3 |             2.29 | −72%   | overflow-checked happy path                      |
| `BM_IntMul`                    |      8.9 |             3.30 | −63%   | overflow-checked happy path                      |
| `BM_IntDiv`                    |      8.1 |             1.91 | −76%   | happy path                                       |
| `BM_IntDivByZero`              |      8.2 |             2.08 | −75%   | error-envelope fast-reject                       |
| `BM_DoubleAdd`                 |      8.2 |             2.09 | −74%   | IEEE 754; no overflow check                      |
| `BM_IntEq`                     |      8.5 |             2.16 | −75%   | same-kind comparison                             |
| `BM_NumericEqIntUint`          |      8.8 |             3.00 | −66%   | cross-type ladder (`1 == 1u`)                    |
| `BM_NumericEqIntDouble`        |      9.6 |             3.50 | −64%   | cross-type ladder (`1 == 1.0`)                   |
| `BM_StringEq/8`                |     10.9 |             3.76 | −65%   | tiny-operand `memcmp`                            |
| `BM_StringEq/64`               |     24.3 |             17.0 | −30%   | 3.50 GiB/s                                       |
| `BM_StringEq/4096`             |      967 |              949 | −2%    | 4.02 GiB/s; saturates memcmp                     |
| `BM_BytesEq/8` /`/64` /`/4096` | 10.9 / 25 / 965 | 3.57 / 16.8 / 952 | — | tracks string_eq within noise           |
| `BM_MapLookupArenaHit`         |     12.9 |             3.60 | −72%   | linear scan, hit at slot 0                       |
| `BM_MapLookupArenaMiss`        |     20.1 |             9.78 | −51%   | linear scan, full walk                           |
| `BM_ListAtArena`               |     10.3 |             2.08 | −80%   | bounds-check + slot load                         |
| `BM_ListEqDispatchArena`       |     32.5 |             10.1 | −69%   | kDynamic dispatcher + arena fast path            |
| `BM_MapEqDispatchArena`        |     68.7 |             32.2 | −53%   | kDynamic dispatcher + arena fast path            |
| `BM_AndBoolBool`               |      9.5 |             1.65 | **−83%** | bool×bool 3VL truth-table                        |
| `BM_OrBoolBool`                |      9.0 |             1.44 | **−84%** | bool×bool 3VL truth-table                        |
| `BM_UnknownMerge`              |     25.9 |             5.21 | −80%   | 1-id+1-id merge + arena alloc                    |
| `BM_UintToInt`                 |      7.2 |             1.42 | −80%   | overflow-check leaf                              |
| `BM_DoubleToInt`               |      7.2 |             1.65 | −77%   | NaN + range check                                |
| `BM_StringToInt/1`             |      8.4 |             2.14 | −75%   | hand-rolled parser, single digit                 |
| `BM_StringToInt/5`             |      9.4 |             3.93 | −58%   |                                                  |
| `BM_StringToInt/19`            |     17.5 |             12.0 | −31%   | max int64 digit count                            |
| `BM_IntToString`               |     18.6 |             9.29 | −50%   | itoa + arena alloc                               |
| `BM_DoubleToString`            |     52.9 |             36.8 | −30%   | fractional decomposition                         |
| `BM_StringConcat/8`            |     18.3 |             8.08 | −56%   | arena alloc + copy                               |
| `BM_StringConcat/64`           |     18.7 |             8.30 | −56%   | 14 GiB/s                                         |
| `BM_StringConcat/4096`         |      147 |              156 | +6%    | 49 GiB/s; memcpy noise                           |
| `BM_StringContains/8`          |     16.2 |             4.86 | −70%   | brute-force scan, hit at tail                    |
| `BM_StringContains/64`         |     84.4 |             17.9 | −79%   |                                                  |
| `BM_StringContains/4096`       |     4726 |              984 | −79%   | 3.88 GiB/s; linear naive search                  |

### Pipeline benches

Two columns:

  - **2026-05-15** is the pre-WASI baseline at `-O3 + -flto`, CEL_LOG
    live (every public helper did a `wasm→host fprintf(stderr)`
    trampoline on every invocation).
  - **2026-05-18** is post-WASI (M2 + M3 + M5 + M6 + M7 merged
    2026-05-18, switched the wasm runtime to wasi-sdk + dlmalloc
    arena + runtime-exported memory) and **CEL_LOG disabled in
    opt builds** (`config_setting` "opt_mode" in
    `runtime/BUILD.bazel` → `-DCEL_LOG_DISABLED`).  Net
    of those two changes the Eval rows uniformly improved — the
    WASI tax on each `wasm→host` trampoline call was being paid 60+
    times per Eval through CEL_LOG; disabling CEL_LOG cuts that
    entirely.  The `BM_Compile` / `BM_Plan` rows went the other
    way (~17% / ~33% slower) because both pay the wasi-sdk runtime's
    larger instantiation cost (cel_runtime.wasm: 60 KB → 241 KB).
    See `doc/implementation-plan/rewrite/wasi/POST_MIGRATION_BENCH.md` for
    the full migration accounting on the narrow api bench.

| Bench                          | 2026-05-15 (CEL_LOG live) | 2026-05-18 (post-WASI, CEL_LOG off) | Δ      | Notes                                          |
| ------------------------------ | ------------------------: | ----------------------------------: | -----: | ---------------------------------------------- |
| `BM_Compile_Literal`           |                    253 us |                              296 us | +17%   | wasi-sdk runtime instantiation overhead        |
| `BM_Compile_ThreeTermArith`    |                    266 us |                              304 us | +14%   |                                                |
| `BM_Compile_TwentyTermCompare` |                    395 us |                              450 us | +14%   | 20-term `a < b && b < c && ...` chain          |
| `BM_Compile_TypeOfEqInt`       |                    262 us |                              306 us | +17%   | `type(x) == int`                               |
| `BM_Compile_IntFromString`     |                    248 us |                              304 us | +23%   | `int(string(123))`                             |
| `BM_Compile_StructLiteral`     |                    260 us |                              303 us | +17%   | `Customer{name: "Ada"}`                        |
| `BM_Plan_Literal`              |                    253 us |                              337 us | +33%   | bigger wasm → longer cranelift Plan            |
| `BM_Plan_ThreeTermArith`       |                    240 us |                              337 us | +40%   | ~constant in body size                         |
| `BM_Eval_Literal`              |                    160 ns |                              137 ns | −14%   | rodata-only; no kernel calls                   |
| `BM_Eval_Select`               |                    462 ns |                              404 ns | −13%   | proto field read via cel_host trampoline       |
| `BM_Eval_ThreeTermArith`       |                    704 ns |                              329 ns | −53%   | 2× `cel_int_add` + activation marshal          |
| `BM_Eval_TwentyTermCompare`    |                 10 973 ns |                            2 867 ns | **−74%** | 20-term chain — most kernel calls per Eval     |
| `BM_Eval_TypeOfEqInt`          |                  1 031 ns |                              336 ns | −67%   | `type(x) == int`                               |
| `BM_Eval_CreateList`           |                  1 901 ns |                              689 ns | −64%   | 5-element arena list literal                   |
| `BM_Eval_CreateMap`            |                  1 364 ns |                              505 ns | −63%   | 2-entry arena map literal                      |
| `BM_Eval_ListAt_Arena`         |                  2 091 ns |                              654 ns | −69%   | 5-element list literal + index                 |
| `BM_Eval_ListAt_Proto`         |                    543 ns |                              465 ns | −14%   | `c.tags[2]` via cel_host trampoline            |
| `BM_Eval_MapLookup_Arena`      |                  1 977 ns |                              633 ns | −68%   | 3-entry map literal + lookup                   |
| `BM_Eval_MapLookup_Proto`      |                    576 ns |                              544 ns | −6%    | `c.metadata["b"]` via cel_host trampoline      |
| `BM_Eval_StructLiteral`        |                    381 ns |                              348 ns | −9%    | `Customer{name: "Ada"}` (host-side build)      |
| `BM_Eval_IntFromString`        |                    770 ns |                              312 ns | −59%   | `int(string(123))`                             |

Two patterns to read in the deltas:

  - The biggest Eval gains land on benches with the most kernel
    calls per Eval (TwentyTermCompare, CreateList, ListAt_Arena) —
    each kernel previously fired a CEL_LOG. CEL_LOG was the
    dominant cost and its host-trampoline cost roughly doubled
    under wasi-sdk's call-prologue convention.
  - Proto-path Eval benches (`_Proto`, `Select`, `StructLiteral`)
    move the least — those use `cel_host.*` trampolines whose
    impls don't emit CEL_LOG.  They still pay the modest
    wasi-libc per-call overhead (visible in the +14% to −14% range).

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
    Reuse across many `Eval(activation)` calls — `arena_reset` rewinds
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

## M7B time benchmarks

The runtime-kernel microbenches for the timestamp / duration surface
scoped in
[`doc/implementation-plan/rewrite/m7b-duration-timestamp.md`][m7b]
live in **`bench/kernel_bench.cc`** (`//bench:kernel_bench`)
alongside the other runtime-kernel BMs.  No separate bench binary
per milestone: kernels go in `kernel_bench`, pipeline scenarios go
in `pipeline_bench`.  Today most M7B kernel BMs are guarded behind
`CELWASM_M7B_SHIPPED` since the kernels themselves don't exist yet;
they turn on row-by-row as M7B.B / M7B.C land.

Bench names + per-bench purpose:

  - `BM_DurationAdd` (M7B.B) — `dur(60.5s) + dur(120.75s)`, exercises
    the nanos-carry arm of `cel_dur_add_at_vv`.  Expected: within
    ~2× of `BM_IntAddBaseline`.
  - `BM_DurationSub` (M7B.B) — `dur(120s) - dur(60.000000001s)`,
    exercises the nanos-borrow arm.
  - `BM_TimestampSubTimestamp` (M7B.B) — `ts - ts → dur` returning a
    full normalised duration; baseline for the
    `subtract_timestamp_timestamp` overload-id path.
  - `BM_TimestampAddDuration` (M7B.B) — the most common ts+dur shape
    a CEL policy can hit (rate-limit-style `ts + dur('1m')`).
  - `BM_TimestampYearUtc` (M7B.C) — pure-wasm civil-calendar walk
    via `cel_civil_from_seconds` projected to `getYear()`.  The
    hot-path perf budget for any expression that chains UTC
    accessors.  Expected: 5–10× a numeric kernel call (the integer-
    divide cascade in `civil_from_days`).  Probe A confirmed
    bit-correctness vs `absl::ToCivilSecond`; this bench measures
    the cost.
  - `BM_TimestampYearUtcLangdefMax` (M7B.C) — Y9999 worst-case
    civil walk.  Surfaces any era-loop hotspot at the upper bound;
    sibling-deviation from `BM_TimestampYearUtc` >2× indicates an
    algorithmic asymmetry.
  - `BM_TimestampDayOfWeekUtc` (M7B.C) — different field projection
    out of the same `CelCivil` struct.  Should sit in the same cost
    band as `BM_TimestampYearUtc`.
  - `BM_DurationHours` (M7B.C) — truncating int-division on
    `seconds`.  No civil walk; should sit in the
    `BM_IntAddBaseline` cost band.

The host-trampoline parse bench (`cel_host.cel_timestamp_parse` for
M7B.D) belongs in `pipeline_bench.cc` because the trampoline is only
reachable through wasmtime; it lands when M7B.D ships.  Expected per
the plan §11: 1–2 orders of magnitude slower than the kernel benches
(per-call wasm↔host boundary cross + `absl::ParseTime` state machine
+ Layer-2 post-validation against the CEL admit-set identified by
Probe B).

## M7-A Any pack / unpack benchmarks

The runtime-kernel microbenches for `google.protobuf.Any` pack /
unpack / equality scoped in
[`doc/implementation-plan/rewrite/m7a-any.md`][m7a] also live in
`bench/kernel_bench.cc`.  Gated by `kM7aShipped = false` today;
turn on when M7-A.A/B/C ship.  Cohort:

  - `BM_AnyPack_SingularField_Reflection` — recommended pack path
    (probe A; reflection-via-SetString).
  - `BM_AnyPack_SingularField_TypedCast` — comparand (typed
    `dynamic_cast<Any*>` + `PackFrom`).  Faster for generated
    descriptors but fails on dynamic pools; bench measures the
    delta to decide whether to grow a fast-path.
  - `BM_AnyPack_SingularField_BaselineCopyFrom` — non-Any baseline.
  - `BM_AnyUnpack_SingularRead`, `_BaselineNonAny`,
    `_RepeatedAnyForEach` — read-side unwrap costs.
  - `BM_AnyEq_AnyVsTyped`, `_AnyVsAny`, `_BaselineNonAny` — the
    peel + recursive `cel_value_eq` cost vs M5.B step 2b baseline.
  - `BM_AnyTypeUrlParse_HappyPath`, `_NoSlash` — active today; pure
    string-slice kernel.  Probe D measured ~10.5 ns / 3.6 ns
    (well below the descriptor-pool / factory costs).

[m7a]: ../../doc/implementation-plan/rewrite/m7a-any.md
[m7b]: ../../doc/implementation-plan/rewrite/m7b-duration-timestamp.md

Run M7B + M7-A kernel benches together:

```bash
bazel run -c opt //bench:kernel_bench -- \
    --benchmark_filter='BM_(Duration|Timestamp|Any)'
```

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
