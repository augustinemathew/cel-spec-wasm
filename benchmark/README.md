# `benchmark/` — the numbers, and how to make them

The one-sentence version: **one YAML corpus drives every engine
cell-for-cell, and one command turns it into joined, parity-checked,
published tables — numbers you can defend, not vibes.**

The primary tier is **Eval**: steady-state evaluation cost, compared
three ways on every cell — **cel-cpp** (the tree-walking reference),
**celwasm static** (runtime linked into the expr module; the
production mode), and **celwasm dynamic** (runtime imported).  Two
binaries produce those three columns: `celcpp_bench`, and
`celwasm_bench` run once per `--link_mode`.  Each is a *single binary*
that loads the whole corpus at startup and registers one Google
Benchmark per cell — so any slice of the corpus is just a
`--benchmark_filter` away.  The other tiers (`compiler/`, `kernel/`,
`component/`) exist to localize, not to compare.

You're here for one of three reasons:

**You changed something and want to know if you regressed.** Run the
fast loop:

```bash
benchmark/eval/run.sh smoke            # correctness + parity, minutes
```

Every corpus cell runs on celwasm and cel-cpp; mismatched results are
flagged `⚠️parity` — a mismatch is a bug, not a number.  If a number
moved and you want to know *which layer* is at fault, drop a tier:
`//benchmark/kernel:kernel_bench` times the native runtime kernels with
no wasm in the loop, `//benchmark/compiler:*` times Compile/Plan, and
`//benchmark/component:foreign_component_bench` isolates the
Component-Model call boundary.

**You want publishable numbers.** On an idle machine:

```bash
BENCH_REPS=3 benchmark/eval/run.sh     # medians of 3 reps, ~30+ min
```

This archives `eval/results/<date>-<host>.{md,csv}` (committed — a
running record) and rewrites the Results section at the bottom of this
file.  Published numbers are never hand-edited; if a number is stale,
re-run.

**You're deciding whether celwasm is fast enough for your workload.**
Skip the harness entirely: read the Results section below for the
latest tables and [`ANALYSIS.md`](ANALYSIS.md) for what they mean —
per-op costs, the proto-read trampoline tax by nesting depth, where
celwasm wins (regex, long chains) and where it doesn't yet.  The
`policies` surface is the realistic tier: composite authz/quota
policies, not microbenches.

## How a number is made

```
 eval/corpus/*.yaml ──── the single source of truth: one cell =
        │                (surface, id, expression, bindings, expected)
        ├──────────────┬──────────────────┐
        ▼              ▼                  ▼
 celwasm_bench   celwasm_bench       celcpp_bench        each: BM_<prefix>_<id>,
 (static link)   (dynamic link)      (tree-walker)       Eval-only timed loop,
        │              │                  │              result label stamped
        └──────────────┴────────┬─────────┘
                                ▼
                       eval/report.py ──── join by BM name, attach the
                                │          expression, ratios + slope/setup/
                                │          crossover regression, parity flags
                ┌───────────────┼────────────────┐
                ▼               ▼                ▼
       results/<date>.md  results/<date>.csv  README Results section
```

Bindings a cell can declare: scalars, lists (explicit `values:` or
generated `gen: {range: N}` / `gen: {template: "…%07d", count: N}`),
and proto messages as textproto (`{type: message, message_type,
textproto}`).  Schema and methodology: [`DESIGN.md`](DESIGN.md) §6;
coverage contract: [`eval/corpus/OPERATORS.md`](eval/corpus/OPERATORS.md).

## Quick commands

Build once, then drive the binaries directly (they're plain Google
Benchmark binaries; corpus paths are repo-relative, so run them from
the repo root):

```bash
bazel build -c opt //benchmark/eval:celwasm_bench //benchmark/eval:celcpp_bench

# One surface, all three columns (here: the policies surface):
bazel-bin/benchmark/eval/celwasm_bench --link_mode=static  --benchmark_filter='^BM_policy_'
bazel-bin/benchmark/eval/celwasm_bench --link_mode=dynamic --benchmark_filter='^BM_policy_'
bazel-bin/benchmark/eval/celcpp_bench                      --benchmark_filter='^BM_policy_'

# One cell:
bazel-bin/benchmark/eval/celwasm_bench --link_mode=static --benchmark_filter='^BM_proto_select_depth16$'

# Or without touching bazel-bin, via bazel run:
bazel run -c opt //benchmark/eval:celwasm_bench -- --link_mode=static --benchmark_filter='^BM_str_matches'

# Tighter numbers on a slice you're investigating:
bazel-bin/benchmark/eval/celwasm_bench --link_mode=static \
    --benchmark_filter='^BM_map_' --benchmark_min_time=1s \
    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

## The surfaces

`python3 benchmark/eval/report.py --list-surfaces` prints this
enumeration live from the corpus (with cell counts); the standing set:

| surface | filter prefix | what it measures |
|---|---|---|
| `arithmetic` | `BM_arith_` | int/uint/double ops, 2→1000-term chains, the 1000-term polynomial |
| `comparisons` | `BM_cmp_` | `== != < <= > >=` across every operand type; 20-term chains |
| `comprehensions` | `BM_compr_` | `all/exists/map/filter` macro loops |
| `conversions` | `BM_conv_` | `int()/string()/type()` casts, nested forms |
| `index` | `BM_idx_` | list/map indexing on arena literals |
| `lists` | `BM_in_list_` | the `in` operator: literal + bound lists, 5→1M elements, hit/first/miss |
| `literals` | `BM_lit_` | bare scalar evals — the per-Eval fixed-cost floor |
| `logic` | `BM_logic_` | `&& \|\|` 3VL, short-circuit vs not |
| `long_strings` / `strings` | `BM_str_` | eq/concat/contains/matches, payloads 10B→10KB |
| `maps` | `BM_map_` | map literals + lookups per key kind |
| `policies` | `BM_policy_` | **the realistic tier** — composite authz/quota/routing policies (ternaries × proto reads × in-lists) |
| `proto` | `BM_proto_` | per-field-kind reads, select depth 1→16, repeated/map fields, arena-vs-proto pairs |
| `size` | `BM_size_` | `size()` over strings/lists/maps |
| `ternary` | `BM_ternary_` | `?:` shapes |
| `time` | `BM_time_` | timestamp/duration arithmetic + accessors |

## Adding a cell

1. Add it to `eval/corpus/<surface>.yaml` — that's usually the whole
   change.  A new *surface* (new file) also touches: `kCorpusFiles` +
   the prefix table in both bench mains, the `data` lists in
   `eval/BUILD.bazel`, and `SURFACE_PREFIXES` in `report.py`.
2. Tick the row in `eval/corpus/OPERATORS.md`.
3. `benchmark/eval/run.sh smoke` — the cell must run (or carry a
   reasoned `celwasm-skip-*` / `celcpp-skip-*` tag) on every
   comparator, with matching result labels.

## Results

<!-- BEGIN AUTO-GENERATED RESULTS (benchmark/eval/report.py) -->
_Last run: 2026-06-17 on Mac (full tables: `benchmark/eval/results/`)._

### Per-operator headline — T(N) = setup + N·per_op

Linear regression over each length-sweep family; slope is the steady-state cost of one more operation, crossover is the expression length where the comparator overtakes cel-cpp.

| surface | operator family | points | cel-cpp slope | cel-cpp setup | celwasm-static slope | celwasm-static setup | celwasm-static crossover vs cel-cpp |
|---|---|---|---|---|---|---|---|
| arithmetic | doubleAdd | 5 | 33.0 | -198 | 1.5 | 114 | N ≈ 10 |
| arithmetic | intAdd | 5 | 32.2 | -66 | 1.7 | 109 | N ≈ 6 |
| arithmetic | intMul | 5 | 30.8 | -34 | 2.7 | 118 | N ≈ 5 |
| arithmetic | intSub | 5 | 31.2 | -48 | 1.7 | 114 | N ≈ 5 |
| comprehensions | all | 4 | 64.6 | 159 | 4.5 | 44 | always wins |
| lists | bound | 5 | 3.5 | 110 | 3.2 | 75 | always wins |
| long_strings | containsLong_N | 4 | 0.0 | 65 | 0.1 | 50 | never wins |
| proto | reads | 3 | 63.8 | 17 | 22.1 | 36 | N ≈ 0 |
| proto | select_depth | 5 | 34.9 | 36 | 39.2 | 28 | never wins |
| size | list | 3 | 8.7 | 127 | -0.0 | 40 | always wins |
| strings | concatChain | 3 | 129.9 | -3,585 | 41.8 | -832 | N ≈ 31 |


<!-- END AUTO-GENERATED RESULTS -->
