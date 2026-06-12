# Benchmark analysis — what the numbers say

Living document: interprets the latest published run under
`eval/results/`.  Current basis: **2026-06-11-Mac** (3 repetitions,
medians, idle machine; full tables in
`eval/results/2026-06-11-Mac.md`, long-format data in the `.csv`).
Re-run `BENCH_REPS=3 benchmark/eval/run.sh` and rewrite the
conclusions here when they materially change.

## Reading guide

- **celwasm-static vs celwasm-dynamic**: static links the runtime
  kernels into the expr module (cross-module inlining); dynamic
  imports them per call.  Static is the production-recommended mode.
- **×cel-cpp** > 1.0 = celwasm faster than the tree-walking reference.

## Findings (2026-06-11 medians)

### 1. celwasm-static turns per-op cost into noise — crossover at ~6–11 terms

The headline regression: celwasm-static's arithmetic slope is
**1.7–3.9 ns/op** vs cel-cpp's **35–37 ns/op** (dynamic: 82–86).
Concretely, `doubleAdd1000Terms` is 1.9 µs static vs 36.9 µs cel-cpp —
**19.5×** — and the win grows with length.  Comprehension loops are
even starker: slope 7.1 vs 89.6 ns/element (`always wins`).  Static
loses only below ~6–11 terms, where the fixed per-Eval floor (§5)
dominates.  Dynamic never catches cel-cpp on chains: the per-op
import-call overhead (~80 ns) exceeds cel-cpp's whole tree-walk step.

### 2. The proto trampoline tax is the cost driver on policy workloads

Every proto read crosses wasm→host and pays host-side reflection:

- singular read `m.i64`: 156 ns static vs 82 cel-cpp;
  proto map lookup `c.metadata["b"]`: 438 vs 137; repeated index
  `c.tags[2]`: 369 vs 131.
- select-depth sweep: ~104 ns/hop static vs ~40 cel-cpp, linear
  through depth 16 (no cliff) — `select_depth16` 1.72 µs vs 0.69 µs.
- the policy stress cell `policy.mega100` (25-arm nested ternary,
  every condition a depth-1..8 select + map lookup + repeated index;
  159 proto accesses per Eval): **23.3 µs static vs 10.0 µs cel-cpp**
  (0.43×) — almost exactly 159 × the per-access delta, i.e. policy
  cost composes additively from per-read cost.
- the same operations on **arena literals** flip the sign:
  `pair_list_arena` 106 ns vs 151 (1.42×), `pair_map_arena` 169 vs
  231 (1.37×).

Implication: proto-heavy policies are bounded by
(reads × depth × trampoline), and the lever is the host boundary —
batch/cache field reads (or open-code hot accessors), not codegen.
Until then, cel-cpp wins the realistic `policies` surface (0.34–0.98×
static) while celwasm wins everything compute-shaped.

### 3. Where celwasm wins big today

- **Regex**: `matchesComplex` 213 ns static vs 11.0 µs cel-cpp
  (**51.8×**) — celwasm compiles the pattern once; cel-cpp re-enters
  RE2 per eval.  Even `matchesCheap` is ~13×.
- **Long chains / comprehensions / constant folding**: §1; const
  variants at 1000 terms reach ~12–20×.
- **Bound 1M-element `in` scan**: 3.36 ms dynamic / 3.73 static vs
  3.75 cel-cpp — parity at memory-bandwidth scale, with celwasm's
  setup amortised away (`always wins` per the regression).

### 4. `size()` on big arena literals is pathological — top optimization candidate

`size({…100-entry int map literal})`: **134 µs static vs 3.4 µs
cel-cpp (0.03×)**.  The aggregate literal is rebuilt in the arena on
every Eval and map construction is quadratic-ish in the linear-scan
kernel.  Same shape, smaller magnitude, on 100-entry lists.  Fix
direction: hoist loop-invariant aggregate literals into static
memory/rodata instead of per-Eval rebuild.

### 5. The fixed per-Eval floor

`literals` surface (`42` as the whole program): **67 ns static /
152 dynamic / 41 cel-cpp** — that's activation marshal + trampoline +
unwind, the price of the sandbox boundary.  It explains every
short-expression loss; ratios cross 1.0 as soon as the body does
~10+ ops of real work.

## Post-cutover re-profile (2026-06-12) — the story moved

All 21 cel_host trampolines now register through the unchecked ABI
(`UncheckedHostThunk`, compile-time i32 proof).  Measured (5-rep
medians, static, same load): `mega100` 19,680 → **13,585 ns (−31%)**,
`select_depth16` −29%, single proto reads −17..−24%.  Gap to cel-cpp
on `mega100`: 2.2× → **1.4×**.

Fresh `mega100` flame (~85 ns/access now):

| ns/access | bucket | next fix |
|---:|---|---|
| ~23 | field/dispatch machinery (incl. outlined fragments) | context/view work below + P3 |
| ~14 | malloc/copy — incl. a `shared_ptr<HostMessageBacking>` intern per chain hop | P2 |
| ~8 + WKT checks | reflection metadata + `UnpackWellKnownTime/WrapperMessage` per read | **P1 (next)** |
| ~5 | `wasmtime_sharedmemory_data/_size` + `Size()` re-fetch per memory access | done — per-Eval base/size cache gated on the grow test (see "P3" below) |
| ~2.5 | wasmtime call ABI | done — was ~29% |

Priority after this data: **P1** (cache `FieldDescriptor*` + WKT/wrapper
classification per field — pure host-side, no safety story), then the
grow test + per-Eval base/size caching, then P2 (copies + backing
intern), then P3.

## P3 — per-Eval memory base/size cache (2026-06-12)

The "~5 ns/access" row above is closed.  The grow regression test
landed first (it gates the cache): `eval/internal/
memory_grow_stability_test.cc` forces a real `memory.grow` mid-$eval
(8-term string concat over a 256 KiB binding ⇒ ~8.75 MiB of arena
intermediates against the 64 KiB seed arena) and pins (a) the base
pointer IDENTICAL across the grow, (b) `data_size` increased by
multiple MiB (provably in-$eval, not marshal), (c) byte-exact results
— plus a `c.metadata[<2 MiB concat key>]` case where the cel_map_lookup
trampoline reads the key span out of pages grown mid-Eval.  API-level
grow + view-refresh cases live in `wasmtime_memory_view_e2e_test.cc`.

The cache itself: `CelHostCallbackEnv::{mem_base, mem_size}` — base
fetched once at Plan (stable across grow per the test), size re-seeded
per Eval and refreshed by `WasmtimeMemoryView::IsInBounds` on a bounds
miss (re-fetch, then re-test; stale size only ever under-approximates,
so the failure mode without the refresh is a false REJECT of freshly
grown pages, never an OOB accept).

Measured (interleaved A/B vs the post-P2 binary, 3-rep medians @ 2 s,
static): on a quiet machine `mega100` 8,335→7,785 / 8,373→7,841 ns
(**−6.5%**), `select_depth16` −1.5..−3%, `authz_deep8` −1..−2%,
single-read and map cells parity (their per-Eval floor has few memory
accesses).  Earlier same-day rounds on a noisier machine showed wider
spreads in both directions; the quiet-machine interleave is the
representative number.  Fresh `mega100` flame: the
`wasmtime_sharedmemory_data/_size` + `Size()` bucket fell from ~280
sampled frames (of 6,118) to **1** (of 4,499); `ReadCelValue` /
`WriteCelValue` are now pure bounds-check + memcpy.  Remaining cost
order: CelGetFieldImpl dispatch/prelude, ProtoMap::Get +
GetMetadata reflection, MapKeysEqual/memcmp, EncodeFieldResult.

## P3 (batched select chains) — `cel_get_field_path` (2026-06-12)

The "P3 — batch select chains into one crossing" candidate below is
done.  Codegen now collapses every contiguous message-typed kSelect
chain (>= 2 hops, non-test_only, intermediates statically
`kMessagePlain` — optional / map / WKT intermediates keep the per-hop
lowering) into ONE `cel_host.cel_get_field_path(out_slot, msg_slot,
path_ref_id)` crossing; the interned `cel.abi.paths[]` row carries
per-hop `(field_ref_id, attribute_id)` pairs, and the trampoline
walks `Reflection::GetMessage` hop-to-hop in C++ — no intermediate
CelValue encode, externref intern, or wasm re-entry.  Per-hop
partial-eval unknown semantics are byte-identical (each hop keeps the
attribute id its unbatched call would have passed).  Design surface:
`rewrite/wat/71_get_field_path.wat` + `rewrite/wat-traces.md` §71.

Measured (interleaved A/B vs the post-view-cache binary, 3-rep
medians @ 2 s, static, two rounds):

| cell | before (r1/r2) | after (r1/r2) | delta |
|---|---|---|---|
| `select_depth16` | 759 / 763 ns | 257 / 257 ns | **−66%** |
| `policy.authz_deep8` | 1034 / 1016 ns | 599 / 601 ns | **−42%** |
| `policy.mega100` | 7785 / 7775 ns | 6699 / 6651 ns | **−14%** |
| `select_depth1` | 111 / 110 ns | 114 / 114 ns | +3 ns (single select untouched; code-layout shift) |
| `proto.metadata_b` | 321 / 314 ns | 322 / 320 ns | parity |
| `proto.tags_at2` | 264 / 263 ns | 265 / 273 ns | parity (r2 cv 3.8%) |

Same-minute head-to-head: `select_depth16` celwasm-static **256 ns**
vs cel-cpp **623 ns** — the depth sweep flipped from 1.3× slower to
**2.4× faster** (per-hop cost is now a C++ reflection walk, not a
boundary crossing); `mega100` 6,664 vs 9,965 ns (1.5× faster).
Result labels identical across baseline/new binaries.

Fresh `depth16` flame: the per-hop trampoline stack is gone;
remaining order is `CelGetFieldPathImpl` + `Reflection::GetMessage`
+ `MatchesAnyUnknownPattern` (the per-hop empty-pattern-set probe —
a future micro-win: hoist the `unknown_patterns.empty()` check above
the hop loop) + per-Eval entry costs (`MarshalActivation`,
wasmtime `Func::call`, `RegisteredType` churn → still P5).
`mega100`'s flame is now dominated by its ~59 non-chain crossings
(map lookups, list indexes, single reads) — also P5 territory.

## Corpus dedupe (applied 2026-06-11, after the published run)

The 296-cell corpus had exactly three true duplicates, all `proto.yaml`
sweep anchors (the published 2026-06-11 tables still show them):

| kept | removed | rationale |
|---|---|---|
| `proto.metadata_b` | `proto.pair_map_proto` | identical cell; the arena-vs-proto pair reads `metadata_b` as its proto half |
| `proto.tags_at2` | `proto.pair_list_proto` | identical cell; pair cites `tags_at2` |
| `proto.select_depth1` | `proto.read_i64`, `proto.reads1` | three `m.i64` anchors (per-kind / depth-1 / width-1); one cell serves all three sweeps |

Shared-source cells that are NOT duplicates (and must stay): per-type
matrices (`a == b` across int/uint/double/string/bytes/bool — the
activation types differ), size sweeps (`x in xs` × 100…1M — the bound
list differs), and short-circuit pairs.

## Iteration / repetition configuration

`BENCH_REPS=N benchmark/eval/run.sh` publishes medians of N reps
(`--benchmark_report_aggregates_only`); per-rep CVs on the 2026-06-11
run sat at 1–7%.  Both bench mains cache the compiled+planned cell
runtime across repetitions (single-slot, keyed by BM name), so reps
multiply only the timed Eval loop — not Compile/Plan.  Convention:
smoke = 1 rep @ 0.01 s, static-only; published = 3+ reps @ 0.5 s,
idle machine.  Surface-scoped runs (`run.sh smoke policies proto`)
never publish.

## Optimization candidates — profiled, in priority order (2026-06-11)

**Goal: strictly faster than cel-cpp, surface by surface.**  Compute-
shaped surfaces already win; the items below are what flips the
proto/policy surfaces and the worst single ratio (`size.map100`,
38.9× — the aggregate-literal rebuild, P4).

Basis: 7–8 s `/usr/bin/sample` of `celwasm_bench --link_mode=static`,
one profile per cell (eval-thread top-of-stack buckets; medians from
the 2026-06-11 published run).  Reproduce any row with:

```bash
bazel build -c opt //benchmark/eval:celwasm_bench
bazel-bin/benchmark/eval/celwasm_bench --link_mode=static \
    --benchmark_filter='^BM_<cell>$' --benchmark_min_time=30s &
sleep 5 && /usr/bin/sample $! 8 -file /tmp/prof.txt
```


| cell (expression) | static ns | wasm→host call ABI | field/dispatch machinery | malloc/copy | proto reflection | JIT wasm | per-Eval entry + other¹ |
|---|---:|---:|---:|---:|---:|---:|---:|
| `m.i64` (`select_depth1`) | 156 | 26.3% | 12.8% | 4.3% | 2.3% | 3.6% | 49.4% |
| `c.name` (`cust_name`, string read) | ~170 | 16.4% | 8.7% | 13.1% | 0.9% | 2.9% | 57.5% |
| `c.tags[2]` (`tags_at2`) | 369 | 19.3% | 10.2% | 12.5% | 2.5% | 5.2% | 49.8% |
| `c.metadata["b"]` (`metadata_b`) | 438 | 15.3% | 10.9% | 14.7% | 4.0% | 5.0% | 49.0% |
| `m.inner…(x15).i64` (`select_depth16`) | 1,721 | 28.4% | 21.5% | 11.0% | 7.5% | 0.8% | 29.1% |
| `policy.mega100` (159 accesses) | 23,300 | 28.7% | 20.7% | 11.2% | 7.4% | 6.4% | 25.6% |
| `size({…100-entry map})` (`size.map100`) | 134,000 | 0.0% | 0.0% | 0.1% | 0.0% | **99.9%** | 0.0% |

¹ For the short cells, "entry + other" is dominated by the host→wasm
**entry** path per Eval: `wasmtime Func::call_impl_do_call` +
`call_impl_check_args`, a `RegisteredType::root`/`Drop` pair (type-
registry refcount churn on every call), trap-handler TLS swap,
`DecodeCelValueAt` result decode, and `HostExternrefTable::Reset`.
That is the ~67–150 ns per-Eval floor measured by the `literals`
surface, seen from inside.

Three regimes fall out of the table:

- **single-read cells**: ~half the time is the per-Eval entry floor;
  the read itself splits between the host-call ABI and our machinery.
- **read-heavy cells** (`depth16`, `mega100`): the floor amortises
  away; the wasm→host **exit** path (untyped ABI boxing + field
  machinery + per-read reflection/copies) is ~70% combined.
- **`size.map100`**: entirely inside JIT'd wasm — the per-Eval arena
  rebuild of the literal + the linear-scan map kernel; no boundary
  involvement at all.

**P0 — switch host trampolines off the untyped wasmtime C-API path.**
Every `cel_host.*` call boxes each param/result through
`wasmtime_val_t`; ~29% of on-CPU time is pure calling-convention
overhead.  Register via `wasmtime_linker_define_func_unchecked`
(raw `wasmtime_val_raw_t` array) instead.  Benefits every host
crossing — proto reads, map lookups, string ops — and shrinks the
per-Eval floor.

> Measured ceiling (2026-06-11, `//benchmark/boundary:wasmtime_call_bench`
> — a minimal 4×i32→i32 import called from a wasm loop): boxed
> ~82 ns/call CPU vs unchecked ~7 ns/call — **~12×**; and for the
> host→wasm entry (P5), `wasmtime_func_call` ~123 ns vs
> `wasmtime_func_call_unchecked` ~27 ns — **~4.7×**.  Our cel_host
> signatures are all-i32 (no externref), so the unchecked path is
> safe.  Production change sites: `DefineHostFunc`
> (eval/internal/cel_host_wasmtime.cc) + the trampoline arg unpack,
> and the `$eval` / `cel_malloc` invocations in instance.cc /
> WasmtimeArenaAllocator.

**P1 — cache `FieldDescriptor*`/`Reflection*` per access site.**
`ProtoBacking::ReadField` re-resolves the field by number on every
read (`FindFieldByNumber` + `GetMetadata`).  The field is static at
compile time — resolve once at Plan/bind and stash on the annotation
(or memoize in `ProtoBacking`).  Also covers `IsWrapperFqn`, which
string-compares the message type name per read.

**P2 — eliminate per-read payload copies.**  String/bytes field reads
build an owning `Value::String(std::string)` (malloc + copy + free per
read).  The bound message outlives the Eval — return views (or
arena-back the transient in the existing eval arena).

**P3 — batch select chains into one crossing.**  ~~Depth sweep is linear
at ~104 ns/hop because codegen emits one `cel_get_field` trampoline per
hop (`select_depth16` = 1.7 µs).  Emit a single path-read host call
carrying the field-number path; N crossings become 1.  Biggest lever
for deep-nested policy shapes (`authz_deep8`, `mega100`).~~ — done
2026-06-12 (`cel_host.cel_get_field_path`; see "P3 (batched select
chains)" above: depth16 −66%, authz_deep8 −42%, mega100 −14%).

**P4 — arena aggregate-literal hoisting.**  Orthogonal to proto:
`size.map100` at 134 µs (0.03× cel-cpp) rebuilds the literal per Eval
over a linear-scan map kernel — the profile is 99.9% inside the JIT'd
wasm, so this is a codegen/kernel fix, not a boundary fix.  Hoist
loop-invariant aggregate literals into static memory; consider a
sorted/hashed layout above ~32 entries.

**P5 — the host→wasm entry floor.**  ~half the cost of every short
Eval: wasmtime `Func::call` argument checking, per-call
`RegisteredType` refcount churn, trap-handler TLS swap, result decode.
Candidates: cache a pre-checked typed function handle per Instance
(or use the unchecked call entry), and pool/elide
`HostExternrefTable::Reset` when no externrefs were created.  This is
what moves the `literals` floor (67 ns) toward cel-cpp's 41 ns and
flips the many sub-300 ns cells where static currently loses.

Expected compound effect on `mega100` (23.3 µs today): P0+P1+P2
attack ~55% of its profile; P3 removes a large fraction of the 159
crossings outright.  Parity with cel-cpp's 10 µs looks reachable
without touching codegen.

## Open measurement gaps

- Error-path benches (div-by-zero, map-miss) are kernel-tier only
  (`//benchmark/kernel`): the eval harness asserts Ok results.
- Unknown/partial-eval shapes: no corpus representation yet.
- 10k-element *literal* lists trap at eval (rodata cap); compile-side
  covered by `//benchmark/compiler:in_operator_compile_bench`.
- `size.map1000`, 1000-term const chains, 10 KB string cells:
  celwasm-skip (rodata cap) — cel-cpp-only columns until the cap
  lifts.
