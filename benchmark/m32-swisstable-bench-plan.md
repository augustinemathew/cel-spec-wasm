# m32 SwissTable map index — benchmark plan

Status: measured 2026-06-27 (Mac.lan, Apple Silicon, `-c opt`).  Owns the
`benchmark/` deliverables for m32 (the SwissTable hash index over arena
maps, design doc `doc/implementation-plan/rewrite/m32-swisstable-map-index.md`).
The kernel crossover sweep (§1.1) and the eval-corpus size-sweep cells
(§1.2) are implemented and run; the threshold decision (§2) is settled.
See §8 "Measured results" at the bottom for the as-run numbers and the
`kCelMapIndexThreshold` verdict.

> Plan-vs-execution delta: the size sweep is capped at **N=256** (plan
> §1.1 listed N up to 1024).  The native bench arena is fixed at
> `CELWASM_ARENA_CAPACITY_BYTES` = 64 KiB (cel_layout.h); an N=1024 int
> map alone needs ~98 KiB (2N CelValue keys+values + an N×48 B entries
> run) and OOMs/segfaults, and the equality benches build two maps.  256
> is the largest N that fits a two-map equality pair and already spans
> the crossover + asymptotic regimes the threshold decision needs.
>
> Plan-vs-execution delta: the A/B `kSwiss`/`kLinear` toggle (§3.1) works
> via the `cel_map_create`+`cel_map_insert` / `cel_map_index_build` seam
> (G1 confirmed), BUT `cel_map_index_build` itself gates on
> `count >= kCelMapIndexThreshold` (cel_map_index.c) — so for N < 8 the
> `kSwiss` arm publishes no index and equals `kLinear`.  The swept
> crossover therefore lands at N=8 for every kernel, which is the
> as-shipped behaviour, not the *true* break-even.  The true break-even
> is read off the flat indexed-probe cost (~6-8 ns, N-independent) vs the
> linear slope (~2.3 ns/entry): it sits at N≈3-4.  Measuring an indexed
> probe below the threshold would need a gate bypass; per CLAUDE.md
> ("report if you need a knob") that runtime change was NOT made — the
> flat-probe extrapolation settles §14 #3 without it.

## 0. What we are demonstrating

m32 replaces the O(n) linear scan in `cel_map_lookup_arena` /
`cel_map_in_arena` / the inner loop of `cel_map_eq_arena` with a
SwissTable index probe (O(1) amortized).  The benches must show three
things:

1. **The crossover.** Below `kIndexThreshold` the linear scan wins
   (cache-resident, no hash + indirection); above it the index wins,
   and the gap widens with N.  This is the headline curve.
2. **The asymptotic win.** `size(map100)` is `126 µs` vs cel-cpp's
   `3.4 µs` today (m31 §1 — that ratio is the *rebuild* cost m31
   fixes; m32 fixes the per-*lookup* O(n) that a comprehension over a
   large map still pays).  A comprehension doing N lookups over an
   N-entry map is O(N²) today → O(N) after m32; the comprehension
   family is where that shows.
3. **No regression on the common case.** Small maps (≤ threshold) and
   iterate/size-only maps must not get slower — they keep the linear
   path (`index_offset == 0`).  The before/after pairing guards this.

Key-type axis matters: **string keys compare by `memcmp`** (the
linear-scan per-compare cost is higher and grows with key length), so
their crossover lands at a *smaller* N than int keys (whose compare is
a single 64-bit op).  Both axes are swept.

## 1. Bench targets — which level, which shapes

Two levels, matching the existing tree's split (README "The
surfaces" / DESIGN §2):

- **Kernel micro-bench** (`benchmark/kernel/kernel_bench.cc`) — native
  `//runtime:cel_runtime` linked, no wasm, no eval.  This is where the
  **linear-vs-swiss crossover curve** lives: it calls
  `cel_map_lookup_arena` / `cel_map_in_arena` / `cel_map_eq_arena`
  directly with a map staged once outside the hot loop, so the timed
  window is the probe alone.  It is the regression-*localisation* tier
  (BUILD comment, kernel/BUILD.bazel:5-12) and the right home for a
  size sweep that would be too many cells for the eval corpus.
- **Eval corpus** (`benchmark/eval/corpus/{maps,index,comprehensions}.yaml`)
  — full Compile→Plan→Eval through wasmtime, three-way vs cel-cpp.
  This is where the **embedder-facing** numbers live: `m[k]`, `k in m`,
  `m1 == m2`, and the comprehension that does N lookups, each at a few
  representative sizes (not the full sweep — that's the kernel tier's
  job).

### 1.1 Kernel tier — the crossover sweep (kernel_bench.cc)

All new BMs go in the existing "Aggregate kernels" section of
`kernel_bench.cc`, beside `BM_MapLookupArenaHit` (line 225) which they
generalize.  Each uses Google Benchmark's `->Arg(N)` /
`->RangeMultiplier` to sweep map size; the map is built once in the
`state` body before the loop (mirroring `BM_MapLookupArenaHit`'s
build-then-loop shape), arena reset once.

Sizes swept (covers below / at / above every candidate threshold and
out to the multi-group-probe regime):
**N ∈ {2, 4, 8, 16, 32, 64, 128, 256, 1024}.**

Key types: **int** (single-op compare) and **string** (memcmp; use a
fixed 8-byte key so the per-compare cost is representative and
constant across N).  String keys are `"key0000".."keyNNNN"` (7 bytes,
distinct).

| BM function | kernel under test | op | key type | sweep |
|---|---|---|---|---|
| `BM_MapLookupArenaHit_Int` | `cel_map_lookup_arena` | `m[k]` hit (mid-run key) | int | Arg(N) over the set |
| `BM_MapLookupArenaHit_Str` | `cel_map_lookup_arena` | `m[k]` hit (mid-run key) | string(7B) | Arg(N) |
| `BM_MapLookupArenaMiss_Int` | `cel_map_lookup_arena` | `m[k]` miss (no_such_key) | int | Arg(N) |
| `BM_MapLookupArenaMiss_Str` | `cel_map_lookup_arena` | `m[k]` miss | string(7B) | Arg(N) |
| `BM_MapInArenaHit_Int` | `cel_map_in_arena` | `k in m` true | int | Arg(N) |
| `BM_MapInArenaMiss_Int` | `cel_map_in_arena` | `k in m` false | int | Arg(N) |
| `BM_MapEqArena_Int` | `cel_map_eq_arena` | `m1 == m2` (equal, the O(n²)→O(n) case) | int | Arg(N) |
| `BM_MapEqArena_Str` | `cel_map_eq_arena` | `m1 == m2` (equal) | string(7B) | Arg(N) |

Hit position = **middle** of the insertion run (index `N/2`): that is
the average linear-scan cost (`N/2` compares), the fairest baseline to
race the index against.  A `_first` / `_last` selectivity split is
*not* needed here — the index is position-independent, and linear-scan
best/worst is already bounded by N; middle is the representative
point.  (Miss is the linear-scan worst case: full `N` compares, so
the miss BMs show the widest crossover gap.)

`BM_MapEqArena_*` is the marquee kernel bench: today it is O(n²) (outer
walk × inner linear match, design §1); after m32 the inner match is an
index probe → O(n).  The curve should bend from quadratic to linear —
visible directly in the per-N table.

> Reuse note: `BM_MapLookupArenaHit` (line 225, fixed N=4 string) and
> `BM_MapLookupArenaMiss` (line 245) and `BM_MapEqDispatchArena`
> (line 304, N=4) already exist.  The new sweeping BMs supersede them
> for the crossover story; **keep the existing fixed-N ones** (they are
> the dispatcher-overhead probes the file documents) and add the
> sweeps alongside.  Do not delete — the N=4 dispatcher cells answer a
> different question (kDynamic branch cost).

### 1.2 Eval corpus tier — embedder-facing cells

The `maps` and `index` surfaces already exist and are wired
(`benchmark/eval/celwasm_bench.cc:445` `kCorpusFiles`,
`SURFACE_PREFIXES` in `report.py:40`, the C++ `kSurfacePrefixes` table
at celwasm_bench.cc:296).  **No new surface, no main/report.py
wiring** — new cells drop into the existing files.

The existing `maps.yaml` cells are all 10-entry (`inString` … `inBool`)
and `index.yaml` are 3–5-entry.  m32 needs the size axis added so the
eval tier corroborates the kernel crossover end-to-end.  Per
DESIGN §6.4.2 these form **length-sweep families**.

**File: `benchmark/eval/corpus/index.yaml`** (lookup `m[k]`).  Add a
size-sweep family for int and string keys.  N ∈ {8, 64, 256} (three
points: below-ish threshold, mid, large — enough for the eval table;
the kernel tier carries the dense sweep).  These are `const-only` map
literals with a var key (isolates the probe from key-marshal, matching
the existing `mapInt`/`mapString` convention).  See §6 for the literal
YAML.

| cell id | source shape | key | N | BM name |
|---|---|---|---|---|
| `mapIntN8` | `{1:…,8:…}[k]` | int var | 8 | `BM_idx_mapIntN8` |
| `mapIntN64` | `{…64 entries…}[k]` | int var | 64 | `BM_idx_mapIntN64` |
| `mapIntN256` | `{…256 entries…}[k]` | int var | 256 | `BM_idx_mapIntN256` |
| `mapStrN8` | `{"k00":…,…}[k]` | string var | 8 | `BM_idx_mapStrN8` |
| `mapStrN64` | `{…}[k]` | string var | 64 | `BM_idx_mapStrN64` |
| `mapStrN256` | `{…}[k]` | string var | 256 | `BM_idx_mapStrN256` |

> Rodata caution: a 256-entry map literal is ~256 × 48 B entries +
> payloads of materialized memory.  **Before m31 ships**, large map
> literals may hit the `[16, 8192)` rodata window and need a
> `celwasm-skip-rodata` tag (same gate `arithmetic.yaml` / `size.yaml`
> use).  **After m31** (compile-time materialization) the window is
> 256 KB and N=256 fits.  Tag N=256 (and possibly N=64) cells
> `celwasm-skip-rodata` at authoring time and remove the tag once m31
> lands — see §5.  The kernel tier has no rodata limit, so the
> crossover is fully measurable there regardless of m31 status.

**File: `benchmark/eval/corpus/maps.yaml`** (`k in m`).  Add the same
N ∈ {8, 64, 256} sweep for `k in {…}`, hit case, int + string keys:

| cell id | source | key | N | BM name |
|---|---|---|---|---|
| `inIntN8` … `inIntN256` | `k in {…}` | int var | 8/64/256 | `BM_map_inIntN{8,64,256}` |
| `inStrN8` … `inStrN256` | `k in {…}` | string var | 8/64/256 | `BM_map_inStrN{8,64,256}` |

Plus **two map-equality cells** (the O(n²)→O(n) headline at eval
level), const-only so both maps are literals:

| cell id | source | N | BM name |
|---|---|---|---|
| `eqIntN64` | `{…64…} == {…64…}` | 64 | `BM_map_eqIntN64` |
| `eqIntN256` | `{…256…} == {…256…}` | 256 | `BM_map_eqIntN256` |

**File: `benchmark/eval/corpus/comprehensions.yaml`** (the N-lookups
case — the strongest motivating shape).  A comprehension that probes
the map once per iteration is O(N²) today, O(N) after m32:

| cell id | source | N | BM name |
|---|---|---|---|
| `mapLookupLoop64` | `[k0..k63 list].all(k, m[k] > 0)` over a bound/literal 64-entry map `m` | 64 | `BM_compr_mapLookupLoop64` |
| `mapLookupLoop256` | same at 256 | 256 | `BM_compr_mapLookupLoop256` |

> Activation constraint: the corpus loader's activation values are
> **scalar-only** (corpus_loader.h; noted in every map/comprehension
> yaml header).  So `m` cannot be a bound map; it must be an in-source
> literal, and the iteration list likewise literal.  The clean shape
> that stays inside the loader is a self-referential literal:
> `{1:1,…,64:64}.all(k, {1:1,…,64:64}[k] > 0)` — but that
> materializes the map twice.  **Preferred shape:** iterate the map's
> own keys via the key-iteration comprehension form the runtime
> already supports (`cel_map_iter_*`, cel_map.h:158) and look each key
> back up — `{…}.all(k, {…}[k] > 0)` is still a double literal.  This
> cell has a **real authoring constraint** that depends on whether
> comprehension-over-a-bound-map is expressible; see §5 gap G3.  If it
> isn't, the comprehension N-lookups story lives **kernel-side only**
> (a `BM_MapLookupLoop_Int` that loops `cel_map_lookup_arena` N times
> over an N-entry map in one timed iteration — trivially expressible at
> the kernel tier) and the eval comprehension cell is dropped or
> deferred.

## 2. Threshold sweep (design §10 — pinning `kIndexThreshold`)

§10 ships `kIndexThreshold = 8` as the default and asks the bench to
confirm it over `{4, 8, 16, 32} × {string, int}`.  The crossover is an
**empirical** quantity: the N at which the indexed probe first beats
the linear scan for that key type.

### 2.1 Measurement method

The threshold is *not* itself a bench parameter at measurement time —
it is read **off the crossover curve** the kernel sweep already
produces.  The procedure:

1. Build `kernel_bench` at `-c opt` (mandatory — §3 / CLAUDE.md
   benchmark-config: kernel numbers are production-config only).
2. Run the `BM_MapLookupArenaHit_{Int,Str}` and
   `BM_MapLookupArenaMiss_{Int,Str}` sweeps (§1.1) **twice**: once
   against the current linear-scan kernels (baseline) and once against
   the m32 indexed kernels, using the before/after pairing of §3.
3. For each key type, the crossover N* is the smallest swept N where
   `swiss_ns(N) < linear_ns(N)`.  Because the sweep includes
   {4, 8, 16, 32} exactly, N* lands on one of the §10 candidate
   values directly; if it falls between two swept points, add the
   intermediate `Arg` and re-run (cheap — one kernel BM).
4. **`miss` is the binding case**: a miss is the linear scan's worst
   case (full N compares) but the index's *same* cost as a hit, so the
   miss curve crosses earliest. Pin the threshold to the **hit**
   crossover (the common case) but report both; if they disagree by
   more than one bucket, document why in the constant's comment.
5. **String crosses before int** (memcmp > single-op compare). Expect
   N*_str ≤ N*_int. If the measured N*_str is below 8 and N*_int is at
   or above 8, the design's "8 aligns with kGroupWidth" choice is a
   compromise — record the per-type measured crossover in the comment
   and keep 8 as the single constant (a per-key-type threshold is out
   of scope; §10 ships one constant).

### 2.2 Where the chosen constant is recorded

Per CLAUDE.md ("cite the bench in the constant's comment per the
benchmark-config rules") and design §10:

- The constant lives in the runtime (e.g. `cel_map_index.c` or
  `cel_data.h` — runtime agent's call, **not this agent's file**).
  Its comment must cite **this plan** and the dated kernel run:
  `// kIndexThreshold = 8: crossover measured at N≈<X> (int) / N≈<Y>`
  `// (string) on <host> <date>; see benchmark/m32-swisstable-bench-plan.md §2`
  `// and benchmark/kernel run results.`
- Kernel-tier crossover numbers are **not** auto-published (the
  publish flow is eval-only, §4).  Record the measured crossover table
  in m32's design doc §10 closeout (the **docs agent** owns that edit;
  this plan supplies the numbers via the run, see §7 handoff).

## 3. Before/after comparison method

The repo rule (CLAUDE.md "Benchmark configuration"): *"Comparison
benches … are an explicit deviation — they exist so a reviewer can
read the optimization delta in one table"*, the canonical pattern
being `pipeline_bench.cc`'s `BM_*_Opt2` paired with its unoptimized
variant.  m32 is a runtime-layout change, not a compiler-flag change,
so the linear-vs-swiss pairing is structured differently:

### 3.1 Kernel tier — `index_offset` toggle, in one binary

The index is built by `cel_map_index_build` and gated by
`index_offset != 0` on every kernel (design §3.1, §9). This gives a
**clean in-process A/B without two builds**: stage the same map, then
race the two paths by toggling whether the index is built.

```cpp
// In kernel_bench.cc, "Aggregate kernels" section.  One helper builds
// the N-entry map; a bool selects linear (skip index build / zero
// index_offset) vs swiss (call cel_map_index_build).

enum class MapPath { kLinear, kSwiss };

// Build an N-entry int-keyed arena map; for kSwiss, build the index;
// for kLinear, leave index_offset == 0 so every kernel linear-scans.
uint32_t BuildIntMap(int64_t n, MapPath path);   // returns map slot

void BM_MapLookupArenaHit_Int(benchmark::State& state) {
  const int64_t n = state.range(0);
  const auto path = static_cast<MapPath>(state.range(1));  // 0=linear,1=swiss
  ResetArena();
  uint32_t m = BuildIntMap(n, path);
  uint32_t key = cel_make_int(n / 2);          // middle-of-run hit
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    cel_map_lookup_arena(out, m, key);
    benchmark::DoNotOptimize(out);
  }
}
// Sweep N × {linear, swiss}.  ArgsProduct gives the full A/B grid in
// one registration; the BM name suffix encodes both (…/N/0 vs …/N/1).
BENCHMARK(BM_MapLookupArenaHit_Int)
    ->ArgsProduct({{2, 4, 8, 16, 32, 64, 128, 256, 1024}, {0, 1}});
```

Reading the result: Google Benchmark prints one row per `(N, path)`
pair (`BM_MapLookupArenaHit_Int/8/0` = linear N=8,
`/8/1` = swiss N=8). The reviewer reads the linear/swiss ratio down
the N column and sees exactly where swiss overtakes — the crossover,
in one table, one binary, no rebuild. This is the
"explicit-deviation comparison bench" the rule sanctions.

> If `BuildIntMap(n, kLinear)` cannot suppress the index post-m32
> (e.g. `cel_map_index_build` is unconditional in the construction
> sequence), expose a build-time switch the bench can call — a
> `cel_map_create` that defers index build, which already exists in
> the design (§9 `cel_map_create` "Sets index_offset (0 until
> built)"). The bench builds entries with `cel_map_insert` and only
> calls `cel_map_index_build` for the `kSwiss` arm. **This is the
> intended seam; confirm it survives implementation (§5 gap G1).**

### 3.2 Eval tier — git-baseline diff, not in-binary toggle

The eval corpus cannot toggle `index_offset` (it goes through the full
compile→eval path, the index is built by codegen/runtime
transparently). The before/after there is the **standard
`tools/compare.py` / report.py baseline diff** the suite already does
for any change (DESIGN §15, README "regressed"): run
`benchmark/eval/run.sh` on `master` (pre-m32) to capture a baseline
table, then on the m32 branch, and diff. The new `mapIntN*` /
`inIntN*` / `eqIntN*` cells will show the eval-level speedup; the
`size`/iterate cells (existing `BM_size_map*`) must show **no
regression** (they keep the linear path; design §9 "unchanged").

No special pairing cells are needed at the eval tier — the corpus
cell *is* the measurement, and the baseline is the prior commit. This
matches how every other eval-level perf change is validated.

## 4. Publication flow

Eval-tier cells flow through the **existing** auto-publish pipeline
unchanged (README "How a number is made", DESIGN §12.7):

```
corpus/*.yaml ─► celwasm_bench (static+dynamic) + celcpp_bench
              ─► report.py (join, ratios, parity) ─► results/<date>-<host>.{md,csv}
              ─► README Results section (--update-readme)
```

- New map/index/comprehension cells are picked up automatically:
  `run.sh` full run builds both benches `-c opt`, runs all three
  columns, `report.py` joins by BM name and writes the dated tables +
  rewrites README. **No hand-editing** (CLAUDE.md: "Never hand-edit
  published numbers — re-run the harness").
- Smoke/parity first: `benchmark/eval/run.sh smoke` must show every
  new cell running on all comparators with matching result labels
  (DESIGN §6.4 "Adding a cell" step 3). A new map cell whose celwasm
  result diverges from cel-cpp is a **correctness bug, not a number**
  (the parity gate) — exactly the safety the m32 design's "semantics
  identical to linear scan" claim (§12 "Conformance: unaffected")
  must survive.
- Which corpus file: lookups → `index.yaml`; membership + equality →
  `maps.yaml`; the N-lookup loop → `comprehensions.yaml` (§1.2). Tick
  the corresponding rows in `benchmark/eval/corpus/OPERATORS.md` when
  the cells land (the coverage ledger).

**Kernel-tier numbers are NOT auto-published** — `kernel_bench` is
`manual`-tagged (kernel/BUILD.bazel:14, "run explicitly, always
-c opt"). The crossover table is run by hand, read into §2's threshold
decision, and pasted into m32's design-doc §10 closeout by the docs
agent (§7 handoff). This is the same posture `kernel_bench`'s existing
BMs have.

## 5. Gaps — benches that cannot be written until m32 lands

Each gap names the bench precisely so it is drop-in at implementation
time, per CLAUDE.md "report bugs/gaps via tests" discipline (here:
via dormant benches).

- **G1 — linear-path suppression seam (kernel A/B).** The §3.1 toggle
  needs `BuildIntMap(n, kLinear)` to produce a map with
  `index_offset == 0` even post-m32. The design (§9 `cel_map_create`
  "0 until built", `cel_map_index_build` as a separate terminal step)
  *says* this seam exists, but it is unconfirmed until the runtime
  agent writes it. **Bench is written now with the `MapPath` enum and
  `ArgsProduct({…,{0,1}})` grid; the `kLinear` arm calls
  `cel_map_create` + `cel_map_insert` and skips `cel_map_index_build`.**
  If implementation makes index-build unconditional, the bench's
  `kLinear` arm is dropped and the before/after reverts to a
  git-baseline diff (§3.2) at the kernel tier too. Flag at review.

- **G2 — kernels don't exist yet.** `cel_map_index_build` and the
  `index_offset`-gated branches in `cel_map_lookup_arena` /
  `cel_map_in_arena` / `cel_map_eq_arena` are m32 code. Until they
  land, the kernel sweep BMs (§1.1) **measure only the linear path**
  (they compile and run today against the existing kernels — that is
  in fact the *baseline* half of §2's two-run procedure). Write them
  now; they produce the linear-scan baseline curve immediately and the
  swiss curve the moment the kernels gain the index branch. **No
  guard / SkipWithError needed** — they exercise public kernels that
  exist; the `kSwiss` arm simply equals the `kLinear` arm until the
  index branch is wired (then it diverges, which is the whole point).

- **G3 — comprehension-over-bound-map shape (eval).** The N-lookup
  comprehension cell (§1.2) needs a map `m` the loop body indexes.
  The corpus loader is scalar-activation-only, so `m` must be a
  literal, forcing a double-materialized literal
  (`{…}.all(k, {…}[k] …)`) or relying on a map-self-iteration form.
  **Whether the clean shape compiles is unconfirmed** (depends on the
  Select-on-map gap noted in maps.yaml header — `{…}.field` currently
  errors, cleanup-backlog #9; `{…}[k]` indexing is fine, so the
  double-literal `[k-list].all(k, mapLiteral[k] > 0)` shape *should*
  work). **Resolution:** author the eval comprehension cell as the
  double-literal index form `[0,1,…,63].all(i, {0:1,…,63:64}[i] > 0)`
  and run it through `run.sh smoke` first; if it parity-passes, keep
  it; if it hits a Select/index gap, `GTEST_SKIP`-equivalent it via a
  `celwasm-skip-*` tag with the reason and rely on the
  **kernel-tier** `BM_MapLookupLoop_Int` (loops `cel_map_lookup_arena`
  N times in one timed iteration — no compile-path dependency) for the
  O(N²)→O(N) story. The kernel-tier loop bench has **no gap** and is
  the primary evidence; the eval comprehension cell is corroborating.

- **G4 — m31 rodata window (eval large-N).** N=256 (and maybe N=64)
  map-literal cells exceed the pre-m31 `[16, 8192)` rodata window
  (§1.2 caution). **Author them now tagged `celwasm-skip-rodata`;
  remove the tag in the commit that confirms m31's 256 KB window lets
  them compile.** The kernel tier (no window) carries large-N until
  then.

## 6. Drop-in scaffold

### 6.1 Kernel BMs (kernel_bench.cc, "Aggregate kernels" section)

Stub signatures (NOT compiled — drop in when m32's kernels land;
see G1/G2 for the `MapPath` seam):

```cpp
// ── m32 SwissTable crossover sweep ──────────────────────────────
// Race the index probe against the linear scan over a map-size sweep.
// state.range(0) = entry count N; state.range(1) = MapPath (0=linear,
// 1=swiss).  Crossover N* read off the linear/swiss ratio per N column.
// Pins kIndexThreshold (design §10); see benchmark/m32-swisstable-bench-plan.md.

enum class MapPath { kLinear = 0, kSwiss = 1 };

// N-entry int-keyed arena map, keys 0..N-1 in insertion order.
// kSwiss → call cel_map_index_build; kLinear → leave index_offset==0.
uint32_t BuildIntMap(int64_t n, MapPath path);
// N-entry string-keyed map, keys "key0000".."keyNNNN" (7B each).
uint32_t BuildStrMap(int64_t n, MapPath path);

constexpr int kCrossoverSizes[] = {2, 4, 8, 16, 32, 64, 128, 256, 1024};

void BM_MapLookupArenaHit_Int(benchmark::State& state) {
  const int64_t n = state.range(0);
  uint32_t m = BuildIntMap(n, static_cast<MapPath>(state.range(1)));
  uint32_t key = cel_make_int(n / 2);            // middle-of-run hit
  uint32_t out = AllocSlot();
  for (auto _ : state) { cel_map_lookup_arena(out, m, key);
                         benchmark::DoNotOptimize(out); }
}
BENCHMARK(BM_MapLookupArenaHit_Int)
    ->ArgsProduct({{2,4,8,16,32,64,128,256,1024}, {0,1}});

void BM_MapLookupArenaHit_Str(benchmark::State& state) {
  const int64_t n = state.range(0);
  uint32_t m = BuildStrMap(n, static_cast<MapPath>(state.range(1)));
  std::string mid = absl::StrFormat("key%04d", n / 2);  // 7B
  uint32_t key = cel_make_string(mid.data(), 7u);
  uint32_t out = AllocSlot();
  for (auto _ : state) { cel_map_lookup_arena(out, m, key);
                         benchmark::DoNotOptimize(out); }
}
BENCHMARK(BM_MapLookupArenaHit_Str)
    ->ArgsProduct({{2,4,8,16,32,64,128,256,1024}, {0,1}});

// Miss = linear-scan worst case (full N compares) → widest crossover.
void BM_MapLookupArenaMiss_Int(benchmark::State& state) {
  const int64_t n = state.range(0);
  uint32_t m = BuildIntMap(n, static_cast<MapPath>(state.range(1)));
  uint32_t key = cel_make_int(n + 1);            // never present
  uint32_t out = AllocSlot();
  for (auto _ : state) { cel_map_lookup_arena(out, m, key);
                         benchmark::DoNotOptimize(out); }
}
BENCHMARK(BM_MapLookupArenaMiss_Int)
    ->ArgsProduct({{2,4,8,16,32,64,128,256,1024}, {0,1}});
// (… _Str miss, _In hit/miss int: same body, cel_map_in_arena …)

// Map equality: O(n²) linear inner match → O(n) indexed.  The bend
// from quadratic to linear is the marquee result.  Both operands
// equal N-entry maps; kSwiss indexes operand b (inner-matched side).
void BM_MapEqArena_Int(benchmark::State& state) {
  const int64_t n = state.range(0);
  auto path = static_cast<MapPath>(state.range(1));
  uint32_t a = BuildIntMap(n, MapPath::kLinear);   // outer walk; never indexed
  uint32_t b = BuildIntMap(n, path);               // inner-matched; index toggled
  uint32_t out = AllocSlot();
  for (auto _ : state) { cel_map_eq_arena(out, a, b);
                         benchmark::DoNotOptimize(out); }
}
BENCHMARK(BM_MapEqArena_Int)
    ->ArgsProduct({{2,4,8,16,32,64,128,256,1024}, {0,1}});

// N-lookups loop — the comprehension proxy with no compile-path
// dependency (covers G3).  One timed iteration probes all N keys.
void BM_MapLookupLoop_Int(benchmark::State& state) {
  const int64_t n = state.range(0);
  uint32_t m = BuildIntMap(n, static_cast<MapPath>(state.range(1)));
  uint32_t out = AllocSlot();
  for (auto _ : state) {
    for (int64_t i = 0; i < n; ++i) {
      cel_map_lookup_arena(out, m, cel_make_int(i));  // arena reset per state, not per probe
      benchmark::DoNotOptimize(out);
    }
  }
}
BENCHMARK(BM_MapLookupLoop_Int)
    ->ArgsProduct({{8,16,32,64,128,256,1024}, {0,1}});
```

> `cel_make_int(i)` inside the inner loop bumps the arena; the existing
> file resets the arena once per `state` (ResetArena at BM top). For
> the loop bench, reset the arena cursor at the top of each `state`
> iteration as `BM_UnknownMerge`/`BM_StringConcat` do (capture
> `rewind` at `cel_mem_base()+8`, restore each iteration) so N key
> mints per outer iteration don't accumulate. Add that rewind to the
> loop bench at implementation time.

### 6.2 Eval corpus cells

`index.yaml` (append; N=8 shown — generate 64/256 the same way; tag
N≥64 `celwasm-skip-rodata` until m31, per G4):

```yaml
  # ── m32 lookup size-sweep family (int keys) ──
  - id: mapIntN8
    source: '{0:0,1:1,2:2,3:3,4:4,5:5,6:6,7:7}[k]'
    activation:
      k: { type: int, value: 4 }
    expected: { type: int, value: 4 }
    purpose: "Lookup length-sweep at N=8 (== kIndexThreshold default).
             Eval-level corroboration of the kernel crossover; pairs
             with mapIntN64/N256 for the slope."
    tags: [phase2, bench-shape, m32-index]

  # ── string keys (memcmp compare; crossover lands lower) ──
  - id: mapStrN8
    source: '{"k00":0,"k01":1,"k02":2,"k03":3,"k04":4,"k05":5,"k06":6,"k07":7}[k]'
    activation:
      k: { type: string, value: "k04" }
    expected: { type: int, value: 4 }
    purpose: "Lookup length-sweep at N=8, string keys.  Sibling of
             mapIntN8 on the key-type axis — string memcmp compare
             crosses to the index at smaller N than int."
    tags: [phase2, bench-shape, m32-index]
```

`maps.yaml` (append; membership + equality):

```yaml
  # ── m32 membership size-sweep (int) ──
  - id: inIntN64
    source: 'k in {0:0, …, 63:63}'        # 64 entries, generated
    activation:
      k: { type: int, value: 32 }
    expected: { type: bool, value: true }
    purpose: "`in` length-sweep at N=64.  Membership probe is the same
             index path as lookup; pairs with inIntN8/N256."
    tags: [phase2, bench-shape, m32-index, celwasm-skip-rodata]  # untag post-m31

  # ── m32 map equality: O(n^2) → O(n) ──
  - id: eqIntN64
    source: '{0:0, …, 63:63} == {0:0, …, 63:63}'
    expected: { type: bool, value: true }
    purpose: "Map `==` at N=64.  Inner match goes index-probed after
             m32: the headline O(n^2)→O(n) bend at eval level."
    tags: [phase2, bench-shape, m32-index, const-only, skip-source-check, celwasm-skip-rodata]
```

`comprehensions.yaml` (append; the N-lookup loop — author per G3 and
smoke-test before keeping):

```yaml
  - id: mapLookupLoop64
    source: '[0,1,2, … ,63].all(i, {0:1,1:2, … ,63:64}[i] > 0)'
    expected: { type: bool, value: true }
    purpose: "N-lookups-over-N-entry-map comprehension: O(N^2) today,
             O(N) after the m32 index.  Strongest eval-level motivating
             shape.  See benchmark/m32-swisstable-bench-plan.md G3 — if
             the double-literal index shape hits a Select/index gap,
             this cell is skip-tagged and the kernel BM_MapLookupLoop_Int
             carries the story."
    tags: [phase2, bench-shape, m32-index, const-only, skip-source-check, celwasm-skip-rodata]
```

## 7. Handoff / cross-references (this agent does NOT edit those files)

- **Threshold numbers → m32 design doc §10 closeout** (docs agent).
  After the §2 kernel run, hand the measured per-type crossover table
  to whoever closes m32's §10; they paste it and tick design §14 open
  question #3. This plan supplies the run + numbers, not the doc edit.
- **`OPERATORS.md` coverage ticks** — when the eval cells land, the
  rows for `maps`/`index`/`comprehensions` size-sweeps get ticked
  (whoever lands the cells does this; it's a `benchmark/` file so it's
  in this agent's scope if cells are authored here).
- **`kIndexThreshold` constant comment** — runtime agent owns the
  constant; its comment must cite this plan §2 (see §2.2).
- **No edits** to `m31-static-aggregates.md`, `m32-*.md`,
  `map-list-dispatch.md`, `testing-checklist.md`,
  `feature-pipeline-checklist.md` from this agent.
```

## 8. Measured results (Mac.lan, Apple Silicon, `-c opt`, 2026-06-27)

Host: Mac.lan (Apple Silicon, 10 cores), `bazel run -c opt`,
Google Benchmark 1.9.1, `--benchmark_min_time=0.2s`.  Kernel sweep:
`benchmark/kernel:kernel_bench` (native `//runtime:cel_runtime`, no
wasm).  All numbers are CPU ns/op.

### 8.1 Implemented benches

**Kernel tier** (`benchmark/kernel/kernel_bench.cc`, "SwissTable
map-index crossover sweep" section): `BM_MapLookupArenaHit_{Int,Str}`,
`BM_MapLookupArenaMiss_{Int,Str}`, `BM_MapInArena{Hit,Miss}_Int`,
`BM_MapEqArena_{Int,Str}`, `BM_MapLookupLoop_Int`.  Each is an A/B grid
`->ArgsProduct({{2,4,8,16,32,64,128,256},{0,1}})` where `range(1)`
selects the path (`0`=linear, no `cel_map_index_build`; `1`=swiss,
terminal `cel_map_index_build`).  Map built once per `state`; the timed
window is the kernel probe alone.

**Eval tier** (corpus cells, run through Compile→Plan→Eval vs cel-cpp):
`index.yaml` → `mapIntN{8,64,256}` / `mapStrN{8,64,256}`;
`maps.yaml` → `inIntN{8,64,256}` / `inStrN{8,64,256}` /
`eqIntN{64,256}`; `comprehensions.yaml` → `mapLookupLoop{64,256}`.
All 16 cells parity-pass (identical result label on celwasm + cel-cpp).

### 8.2 Kernel crossover sweep (linear vs indexed, ns/op)

#### `m[k]` hit, int keys (middle-of-run) — `BM_MapLookupArenaHit_Int`

| N | linear ns | indexed ns | linear/indexed |
|---:|---:|---:|---:|
| 2 | 6.3 | 6.2 | 1.0× |
| 4 | 9.1 | 9.0 | 1.0× |
| 8 | 14.4 | 6.8 | 2.1× |
| 16 | 23.1 | 7.2 | 3.2× |
| 32 | 45.4 | 7.2 | 6.3× |
| 64 | 80.9 | 7.6 | 10.7× |
| 128 | 152.1 | 8.4 | 18.1× |
| 256 | 294.0 | 7.9 | 37.0× |

#### `m[k]` hit, string keys (7 B, memcmp) — `BM_MapLookupArenaHit_Str`

| N | linear ns | indexed ns | linear/indexed |
|---:|---:|---:|---:|
| 2 | 8.4 | 9.9 | 0.8× |
| 4 | 12.1 | 11.9 | 1.0× |
| 8 | 18.3 | 10.4 | 1.8× |
| 16 | 29.5 | 10.2 | 2.9× |
| 32 | 52.4 | 10.7 | 4.9× |
| 64 | 107.3 | 11.4 | 9.4× |
| 128 | 189.5 | 10.9 | 17.4× |
| 256 | 359.6 | 11.3 | 31.7× |

#### `m[k]` miss, int keys (linear worst case) — `BM_MapLookupArenaMiss_Int`

| N | linear ns | indexed ns | linear/indexed |
|---:|---:|---:|---:|
| 2 | 6.5 | 6.2 | 1.0× |
| 4 | 12.2 | 12.1 | 1.0× |
| 8 | 20.9 | 5.0 | 4.2× |
| 16 | 41.4 | 5.4 | 7.6× |
| 32 | 77.2 | 5.6 | 13.7× |
| 64 | 148.6 | 6.0 | 24.9× |
| 128 | 290.9 | 6.8 | 42.7× |
| 256 | 577.7 | 6.5 | 88.9× |

#### `m[k]` miss, string keys — `BM_MapLookupArenaMiss_Str`

| N | linear ns | indexed ns | linear/indexed |
|---:|---:|---:|---:|
| 2 | 4.7 | 4.7 | 1.0× |
| 4 | 8.4 | 8.4 | 1.0× |
| 8 | 13.1 | 7.7 | 1.7× |
| 16 | 24.8 | 7.8 | 3.2× |
| 32 | 54.6 | 7.9 | 6.9× |
| 64 | 102.3 | 8.1 | 12.6× |
| 128 | 197.6 | 8.4 | 23.5× |
| 256 | 388.1 | 8.8 | 43.9× |

#### `k in m` true, int keys — `BM_MapInArenaHit_Int`

| N | linear ns | indexed ns | linear/indexed |
|---:|---:|---:|---:|
| 2 | 7.0 | 7.0 | 1.0× |
| 4 | 9.6 | 9.6 | 1.0× |
| 8 | 14.4 | 7.2 | 2.0× |
| 16 | 23.1 | 7.5 | 3.1× |
| 32 | 45.9 | 7.8 | 5.9× |
| 64 | 81.7 | 8.0 | 10.3× |
| 128 | 153.1 | 8.1 | 19.0× |
| 256 | 296.7 | 8.3 | 35.9× |

#### `k in m` false, int keys — `BM_MapInArenaMiss_Int`

| N | linear ns | indexed ns | linear/indexed |
|---:|---:|---:|---:|
| 2 | 6.7 | 6.7 | 1.0× |
| 4 | 12.2 | 12.2 | 1.0× |
| 8 | 21.4 | 5.4 | 4.0× |
| 16 | 41.9 | 5.7 | 7.4× |
| 32 | 77.5 | 5.7 | 13.6× |
| 64 | 148.8 | 6.3 | 23.8× |
| 128 | 291.9 | 6.2 | 47.0× |
| 256 | 578.4 | 6.5 | 89.6× |

#### `m1 == m2` equal, int keys (O(n²)→O(n)) — `BM_MapEqArena_Int`

| N | linear ns | indexed ns | linear/indexed |
|---:|---:|---:|---:|
| 2 | 16.8 | 16.1 | 1.0× |
| 4 | 40.6 | 41.0 | 1.0× |
| 8 | 118.5 | 70.6 | 1.7× |
| 16 | 415.4 | 135.7 | 3.1× |
| 32 | 1424.7 | 281.9 | 5.1× |
| 64 | 5124.3 | 559.8 | 9.2× |
| 128 | 19464.9 | 1121.0 | 17.4× |
| 256 | 75494.9 | 2330.5 | 32.4× |

#### `m1 == m2` equal, string keys — `BM_MapEqArena_Str`

| N | linear ns | indexed ns | linear/indexed |
|---:|---:|---:|---:|
| 2 | 19.7 | 19.7 | 1.0× |
| 4 | 48.8 | 49.6 | 1.0× |
| 8 | 156.0 | 100.3 | 1.6× |
| 16 | 548.3 | 192.4 | 2.9× |
| 32 | 2069.6 | 377.8 | 5.5× |
| 64 | 7178.2 | 757.4 | 9.5× |
| 128 | 25588.8 | 1542.0 | 16.6× |
| 256 | 93496.3 | 3160.7 | 29.6× |

#### N lookups over an N-entry map (comprehension proxy) — `BM_MapLookupLoop_Int`

| N | linear ns | indexed ns | linear/indexed |
|---:|---:|---:|---:|
| 8 | 125.9 | 38.7 | 3.3× |
| 16 | 438.8 | 87.2 | 5.0× |
| 32 | 1718.7 | 166.2 | 10.3× |
| 64 | 6480.9 | 351.3 | 18.4× |
| 128 | 25225.4 | 736.7 | 34.2× |
| 256 | 98988.7 | 1515.5 | 65.3× |

### 8.3 Crossover analysis & threshold verdict

The indexed probe is **flat at ~6-8 ns (int) / ~10-11 ns (str)
regardless of N** — O(1) as designed.  The linear scan grows
~2.3 ns/entry (int hit) up to the full-N worst case on a miss.

`cel_map_index_build` gates on `count >= kCelMapIndexThreshold` (= 8),
so for N < 8 the swiss arm publishes **no** index and equals the linear
arm — the swept "crossover" therefore lands at N=8 for every kernel.
That is the as-shipped behaviour, not the *true* break-even.  The true
break-even is read off the flat indexed cost vs the linear slope:

- **int keys:** linear hit is 6.3 ns @ N=2, 9.1 ns @ N=4, 14.4 ns @
  N=8; the indexed probe is ~6.8 ns.  Break-even ≈ **N=3-4**.
- **string keys:** linear hit is 8.4 ns @ N=2, 12.1 ns @ N=4, 18.3 ns
  @ N=8; the indexed probe is ~10.4 ns.  Break-even ≈ **N=3-4** (the
  memcmp per-compare cost is higher, so string crosses no later than
  int — consistent with the plan's §2.1 expectation).

**Verdict: `kCelMapIndexThreshold` HELD AT 8.**  Rationale (per §14 #3,
"prefer the lower/string-key crossover for a single constant"):

1. 8 sits *at or just above* the true break-even (~3-4) for BOTH key
   types — so no map below the threshold ever pays index overhead, and
   every map at the threshold already wins ≥1.8× (int 14.4→6.8 ns,
   str 18.3→10.4 ns at N=8; misses win ≥1.7×).
2. 8 aligns with `kGroupWidth` (one SWAR group), keeping the index
   geometry clean.
3. Lowering to 4 would buy a marginal win only in the N=4-7 band, where
   the linear scan is already cheap (9-14 ns) and the per-build index
   cost (not amortised for a one-shot literal) would erode it.  Not
   worth a non-power-of-two, non-group-aligned constant.

Measuring an indexed probe *below* the gate would require bypassing the
`count >= kCelMapIndexThreshold` check inside `cel_map_index_build` —
i.e. a runtime knob.  Per CLAUDE.md ("report if you need a knob") that
runtime change was NOT made; the flat-probe extrapolation above settles
the question without it.  Hit and miss crossovers agree (both at the
gate; both flat-probe break-even ≈ 3-4), so no >1-bucket disagreement to
document beyond this.

### 8.4 Marquee result — O(n²) → O(n)

`BM_MapEqArena_Int` (equal N-entry maps, inner match indexed) bends from
quadratic to linear: **N=256 goes 75.5 µs → 2.3 µs (32×)**; string keys
93.5 µs → 3.2 µs (30×).  `BM_MapLookupLoop_Int` (N lookups over an
N-entry map — the comprehension proxy) is **99 µs → 1.5 µs at N=256
(65×)**.  These are the per-lookup O(n) wins a comprehension over a
large map now realises.

### 8.5 Eval-tier note

The eval-corpus cells run the *full* Compile→Plan→Eval, and the var-key
cells re-materialize the literal map + rebuild the index on **every**
Eval (the map header is not const-folded for a `m[k]` with a bound key).
So at large N these cells are **materialization-dominated**, not
lookup-dominated, and read slower than cel-cpp — which is exactly why the
lookup-isolation crossover lives at the kernel tier (map staged once
outside the timed loop).  The eval cells are corroborating (correctness
+ end-to-end parity), not the crossover evidence.  Numbers flow through
the standard auto-publish pipeline (`results/<date>-<host>.md` + the
README Results section).
