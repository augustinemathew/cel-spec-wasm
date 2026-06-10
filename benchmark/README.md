# `benchmark/` — adoption-grade comparative benchmarks

The goal of this directory is **publishable, defensible, comparative
performance numbers** for celwasm against the reference CEL
implementations (cel-cpp tree-walker).  Adoption depends on
these numbers — without them, "we're faster than interpretation" is
unverifiable.

## Running it

```bash
# Full three-way comparison (celwasm dynamic + static, cel-cpp) with
# the joined report.  ALWAYS -c opt — debug numbers are ~10x off.
benchmark/eval/run.sh             # full run (min_time=0.5s per cell)
benchmark/eval/run.sh smoke       # faster turnaround (min_time=0.1s)

# Or drive a binary directly:
bazel build -c opt //benchmark/eval:celwasm_bench //benchmark/eval:celcpp_bench
bazel-bin/benchmark/eval/celwasm_bench \
    --link_mode=static \               # or dynamic (default)
    --benchmark_filter='BM_arith_intAdd' \
    --benchmark_min_time=1s
bazel-bin/benchmark/eval/celcpp_bench --benchmark_filter='BM_arith_intAdd'

# Profiling: CELWASM_BENCH_PERFMAP=1 makes wasmtime write a
# /tmp/perf-<pid>.map so samply / perf can symbolicate the JIT.
```

Cells are paired by benchmark name across binaries — the same YAML
corpus drives both, so any filter gives an apples-to-apples slice.
The most recent full-corpus results live in
`doc/implementation-plan/rewrite/m28-bench-results.md`.

This is **NOT** the existing `bench/` directory.  That tree holds
ad-hoc microbenches that grew organically (kernel µbenches, pipeline
shape probes, the `in`-operator headline numbers).  It stays.

This tree adds a **methodology + harness + corpus** layered on top:

  - **Corpus**: YAML-listed CEL expressions, each tagged with surface
    / type / length / activation / variant — the test matrix.
  - **Harness**: reads a corpus, runs each entry through every
    enabled comparator (celwasm / cel-cpp), captures
    structured JSON.  Same expression, same activation, same
    iteration shape — apples-to-apples by construction.
  - **Comparator**: joins the JSONs across comparators by `id`,
    emits long-format CSV + Markdown tables.
  - **Publisher**: regenerates README comparison sections from the
    latest CSV.  Numbers in docs stop drifting from reality.

## Layout

```
benchmark/
├── README.md                  ← this file (methodology overview)
├── compiler/                  ← Compile + Plan benches (deferred)
│   └── TODO.md
└── eval/                      ← Eval steady-state benches (primary)
    ├── README.md              ← how to run
    ├── HARNESS.md             ← apples-to-apples rules
    ├── BUILD.bazel            ← (added when first bench TU lands)
    ├── corpus/                ← the test matrix
    │   ├── arithmetic.yaml
    │   ├── comparisons.yaml
    │   ├── booleans.yaml
    │   └── strings.yaml
    └── results/               ← committed CSV outputs over time
```

## Scope tiers

**Primary today: `eval/`.**  Steady-state Eval is the hot path on the
adoption story (request-time policy eval) and the surface where AOT
compilation should crush tree-walking interpretation most
dramatically.  All work goes here until the surfaces below are
covered.

**Deferred: `compiler/`.**  Compile + Plan benches matter for cold-
start latency (one-shot policy compile) but cel-cpp's "compile" is a
parse + check that produces a tree, not a JIT-compiled module — the
shapes don't compare directly.  Common compiler benchmark cases are
listed in `compiler/TODO.md` for the day this matters; today, no code.

## Axes the corpus varies

| axis | values |
|---|---|
| **surface** | arithmetic / comparison / boolean / string / (later: aggregate, time, proto, extension) |
| **operand type** | int / uint / double / string / bytes / bool / null |
| **expression length** | 1 / 3 / 10 / 50 / 250 / pathological |
| **activation shape** | all-literal / single-var / many-var / proto-msg / nested-proto |
| **variant** | linear / deep-tree / mixed-op / short-circuit-early / no-early-exit |

The grid is the *budget* — most cells stay empty (e.g. boolean has no
"50-term" cell because `&&` chains saturate around 10 in real
workloads).  A surface is "covered" when its row is dense enough that
a reviewer can draw a conclusion.

## Comparators

| name | how it runs | when wired |
|---|---|---|
| **celwasm** | this repo's Engine + Plan + Eval (C++) | day 1 |
| **cel-cpp** | the vendored tree-walking evaluator at `third_party/cel-cpp/runtime/` | day 1 (the pattern at `bench/in_operator_cel_cpp_bench.cc` shows the trick — separate TU, no celwasm `cel::Value` aliases linked in) |

The published table doesn't show comparator-X if it isn't wired.  A
cell is reported `n/a` rather than "missing" — distinguishes "not
tested" from "tested, didn't apply".

## Apples-to-apples rules (non-negotiable)

These move to `eval/HARNESS.md` once it lands.  Summary:

1. **Same machine, same thermals.**  CPU governor pinned, 30 s warm
   between cases.  CI runner is dedicated.
2. **Same expression text** — parse-tree equivalence verified
   before timing.
3. **Same activation** — bound via shared helper, no per-comparator
   drift.
4. **Three timings per cell**: cold (one-shot incl. Compile/Plan),
   warm (Plan amortised), hot (steady-state after K warmups).
5. **N ≥ 5 repetitions; report median + p95.**  Variance > 10%
   blocks publishing — gets a `(noisy)` tag.
6. **Result-parity check** — every comparator's Eval result on a
   cell must agree before timing is reported.  A mismatch is a bug,
   not a number.

## What gets published

The headline output is a single comparison table per surface, sorted
by expression length:

```
Eval steady-state, median ns/call, lower is better.

surface  | id                          | celwasm | cel-cpp | vs cpp
arith    | arith.int.add.linear.3      | 329     | 620     | 1.88×
arith    | arith.int.add.linear.50     | 4200    | 9800    | 2.33×
…
```

Tables get checked in under `eval/results/YYYY-MM-DD-host.csv` (a
running record), and the README of each surface gets the latest
Markdown rendering.

## Status

- **Day 0 (this commit)**: scaffolding only — no `.cc` files, no
  benches running yet.  Corpus YAMLs lay out the Phase 1 row set;
  `eval/HARNESS.md` pins the contract.
- **Day 1**: harness loader + cel-cpp wiring + Phase 1 corpus
  generates BMs, first table published.
- **Phase 2**: composite workloads (auth, header match, range
  predicates) — added as additional corpus YAMLs.
