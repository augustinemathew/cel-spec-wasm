# m28 — Full-corpus three-way benchmark results

Status: measured 2026-06-09, 21:30 (quiet machine, single run).

**Method.** Full `benchmark/eval/corpus/` (232 cells, 13 surfaces) run
three ways on an otherwise-idle Apple Silicon 10-core (P-cores
~3.5 GHz): `celwasm_bench --link_mode=dynamic`, `--link_mode=static`,
and `celcpp_bench` (cel-cpp's tree-walking evaluator; same YAML corpus,
same activations, joined by benchmark name).  All `-c opt`,
`--benchmark_min_time=1s`, Google Benchmark `real_time`.  214 cells run
on both engines; 16 are celcpp-only (`celwasm-skip-rodata` 10 000-char
literals + known-gap cells, tagged in the corpus).  Compile + Plan
happen outside the timed region — these numbers are pure Eval.
Reproduce with `benchmark/eval/run.sh`.

## 1. Headline: per-family geomean, static-celwasm vs cel-cpp

Ratio < 1 means celwasm-static is faster.  Dynamic-mode column for
context (the pre-m28 shape).

| family | cells | static/cel-cpp | wins–losses | dynamic/cel-cpp |
|---|---:|---:|---:|---:|
| comprehensions | 8 | **0.19×** | 8–0 | 3.82× |
| arithmetic | 50 | **0.38×** | 30–20 | 3.30× |
| indexing | 7 | **0.91×** | 5–2 | 4.31× |
| ternary | 4 | 1.02× | 2–2 | 3.87× |
| strings | 23 | 1.29× | 4–19 | 3.23× |
| conversions | 22 | 1.30× | 1–21 | 3.67× |
| comparisons | 50 | 1.35× | 5–45 | 3.72× |
| logic | 13 | 1.52× | 2–11 | 5.19× |
| in (list/map) | 11 | 1.53× | 1–10 | 6.37× |
| time | 14 | 1.62× | 0–14 | 3.51× |
| size | 7 | 1.67× | 2–5 | 7.10× |
| maps | 5 | 2.22× | 1–4 | 5.14× |
| **corpus-wide** | **214** | **0.95×** | **61–153** | **3.9×** |

Reading the win/loss asymmetry honestly: cel-cpp wins **more cells**
(the corpus is dense in single-op N=2 cells, where its ~45–90 ns
interpreter entry beats our 62 ns wasm-boundary floor by 1.2–1.9×);
celwasm-static wins **by far larger margins** where expressions have
length or control flow (10–48×).  The corpus-wide geomean lands at
parity (0.95×) — which cell mix matters for a given embedder decides
which engine is faster for them.

## 2. Largest static-mode wins vs cel-cpp

| cell | static | cel-cpp | faster by |
|---|---:|---:|---:|
| `str_matchesComplex` | 186 ns | 8 939 ns | **48×** † |
| `compr_map20` | 219 ns | 5 391 ns | 25× |
| `arith_doubleAdd1000Terms` | 1 545 ns | 34 607 ns | 22× |
| `arith_intAdd1000Terms` | 1 969 ns | 33 442 ns | 17× |
| `arith_*250Terms` | ~560 ns | ~8 300 ns | ~14–16× |
| `str_matchesCheap` | 101 ns | 1 499 ns | 15× |
| `compr_all100` | 713 ns | 6 610 ns | 9× |

† Cause confirmed (2026-06-09): `runtime/cel_matches.cc` keeps a
per-Instance single-slot compiled-pattern cache — the hot loop pays
RE2 compilation once, then only matches (~186 ns).  cel-cpp's default
runtime (as celcpp_bench configures it) rebuilds the RE2 every
evaluation; the 8.9 µs is mostly regex *compilation*.  cel-cpp ships
an optional regex-precompilation extension that would close most of
this row.  So: real for the compile-once-eval-many workload the bench
models, but it is a cache-vs-no-cache comparison — quote only with
this caveat.  Conformance agrees between engines (including the
empty-pattern cache-poisoning edge rows), so it is not a correctness
shortcut.

The comprehension sweep is the strongest architectural result: a
100-element `all()` is 47× faster than our own dynamic mode and 9×
faster than cel-cpp, because compilation turns the densest
helper-call traffic (4+ calls/iteration in dynamic mode; interpreter
dispatch in cel-cpp) into a raw wasm loop.

## 3. Largest static-mode losses vs cel-cpp (the honest column)

| cell | static | cel-cpp | slower by | architectural cause |
|---|---:|---:|---:|---|
| `size_map100` | 122 µs | 2.8 µs | **44×** | per-eval construction of a 100-entry map literal (~1.2 µs/entry) vs cel-cpp folding the constant aggregate once + SwissTable |
| `str_eqLong_N1000` (match & last-byte mismatch) | ~604 ns | ~75 ns | 8× | byte-loop string compare in the wasm runtime (~0.6 ns/byte) vs native SIMD memcmp (~0.07 ns/byte); wasm-SIMD memcmp is an open lever |
| `str_containsLong_N1000` | 581 ns | 81 ns | 7× | same — substring search without SIMD |
| `map_in{Int,Uint}` | ~1.5 µs | ~380 ns | ~4× | per-eval map construction dominates |
| `size_map10` | 1.2 µs | 362 ns | 3.4× | same |
| `logic_andShortCircuit` | 128 ns | 48 ns | 2.6× | floor-bound: two ops can't amortize the boundary intercept |
| `in_list_100` | 2.7 µs | 1.4 µs | 2× | list literal rebuilt per eval vs cel-cpp constant folding |
| single-op cells broadly (cmp/conv/str/time) | 60–140 ns | 45–95 ns | 1.2–1.9× | per-Eval floor: one wasmtime boundary crossing + arena reset + result decode = 62 ns minimum |

Three distinct loss mechanisms, in order of leverage:

1. **Constant-aggregate construction** (`in_list`, `map_*`, `size_map*`):
   we rebuild list/map literals every Eval; cel-cpp folds them at plan
   time.  The const-list/map-literal codegen milestone (already named
   in `m28-configurable-linking.md` §10) converts most of these rows
   to wins — `size(list)` cells, where construction is cheap arena
   appends, already win up to 3.7×.
2. **SIMD-less byte work** (`eqLong`, `containsLong`): the wasm
   runtime's string compare runs ~8× slower per byte than native
   SIMD memcmp.  First-byte-mismatch cells confirm both engines
   early-exit correctly; the gap is pure scan throughput.
3. **The 62 ns boundary floor** (every N=2 cell): irreducible without
   bypassing wasmtime's call path; affects only expressions too small
   to amortize it.  Crossover to celwasm-win is at roughly N≈10 ops.

## 4. FINDINGS §11.4 reproduction verdict

The pre-production claim ("intAdd1000Terms ~1 µs static, ~31× faster
than cel-cpp") does **not** reproduce within ±10%:

| | claimed (prototype, 2026-06-06) | measured (production, 2026-06-09) |
|---|---:|---:|
| intAdd1000Terms static | ~1 µs | 1 969 ns (1 713 ns in an earlier same-day run; ~15 % run-to-run variance) |
| vs cel-cpp | ~31× | **17×** (int) / **22×** (double) |

Candidate causes for the delta: the prototype pipeline ran `wasm-opt
-O3` over the merged module vs production `optimize_level=2`; different
machine state; and the prototype measured hand-built merged artifacts
rather than the in-Compile merge.  The honest publishable number for
1000-term arithmetic chains is **17–22× faster than cel-cpp**; use
that, not 30×.  (An `optimize_level=3` comparison run remains open if
the gap matters.)

## 5. Context numbers

- **Per-Eval floor**: 62 ns static / 230 ns dynamic (boundary call +
  arena reset + result decode).  cel-cpp's equivalent: ~45 ns.
- **Per-op slope, dependent int-add chain**: ~1.5–1.9 ns static
  (~5 cycles — slot loads/stores around a 1-cycle add; register
  allocation of scalars is the remaining codegen lever), ~32 ns
  cel-cpp (~110 cycles: dispatch + unbox + rebox), ~80 ns dynamic
  (wrapper chain, removed by m28).
- **Static vs our own dynamic mode: every family wins**, geomeans
  2.2× (time) to 19.8× (comprehensions), max 51× — m28's reason to
  exist, fully confirmed at corpus scale.
- Raw outputs: `/tmp/final_{dynamic,static,celcpp}.json` (single run;
  regenerate via `benchmark/eval/run.sh`).

## 6. Caveats

- `size_mapN` / `map_inX` / `in_list_N` cells time **literal
  construction + the operation**, by design (the corpus measures the
  expression as written).  Rows are labeled with their cause above so
  the comparison is understood; an activation-bound-aggregate variant
  (construction outside the timed region) would isolate pure lookup
  cost and is a natural corpus extension.
- Single run per cell (min_time=1s).  Same-day repeat on
  intAdd1000Terms static varied ~15 % (1 713 → 1 969 ns); treat
  sub-20 % differences as noise.
- `str_matchesComplex` 48× pending cause verification (§2 †).
