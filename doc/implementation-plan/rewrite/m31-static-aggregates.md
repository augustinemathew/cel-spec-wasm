# m31 — compile-time materialization of constant aggregate literals

Status: shipped 2026-06-17 (const lists); const-map materialization
shipped 2026-06-27 (the m31.A / m32.B follow-up — see §8).  What landed:
const list AND const map literals materialize byte-identically into
rodata and lower to a single `i32.const`; for maps with
`count >= kCelMapIndexThreshold` the SwissTable index is baked
byte-identically to `cel_map_index_build` (so a lookup in a materialized
map resolves through the baked index exactly as in a runtime-built one);
the low-memory window was raised 8 KiB → 256 KiB (§10); the
rodata-overflow gate and ABI v3 bump shipped with it.

> Const-map delta (§8).  As shipped, the entry run is written in
> **source order** (matching `cel_map_create` + `cel_map_insert`), NOT a
> compile-time sort — the earlier "sort + binary-search" and
> "place-by-hash entry run" sketches were both superseded by m32.A's
> index-over-dense-run layout: the dense entry run stays source-ordered
> and a separate SwissTable index block (control bytes + u32 slot array)
> is appended after it.  `MaterializeMap` bakes that index by reusing the
> frozen `cel_map_hash.h` kernel over the const keys (staged into the
> native arena so the kernel reads their bytes), reproducing
> `cel_map_index_build`'s placement bit-for-bit.  Duplicate-key const
> maps return `std::nullopt` from `MaterializeMap` and keep the per-Eval
> build path so the runtime still poisons `CEL_ERR_DUPLICATE_KEY`.

> Plan-vs-execution delta (memory layout, §4).  The as-drafted plan
> placed materialized aggregates in a **dedicated band** between rodata
> and workspace (`aggregate_base … aggregate_end` in the §4 diagram).
> As built, there is **no separate band**: `MaterializeList` writes the
> aggregate's header + run + frame straight into the **rodata** region
> via the same `StaticMemoryBuilder`, and the node carries
> `StorageKind::kStaticRodata` — the identical storage path as a scalar
> constant.  Materialized aggregates *are* rodata, so the static window
> stays two-region (rodata + workspace + 256 B guard); the §4 diagram's
> three-region picture describes the rejected design, not the shipped
> one.  The single overflow gate (`CheckStaticWindowFits` in
> layout_pass.cc) covers the merged rodata extent.

## 1. Problem

Constant list/map literals are rebuilt in the dlmalloc arena on
**every Eval**: codegen emits the per-element construction sequence,
and the kernels re-insert every entry per evaluation.  Measured
(benchmark corpus, 2026-06-12): `size({…100-entry int map})` runs
**126 µs vs cel-cpp's 3.4 µs (0.02×)** — the worst ratio in the
corpus; even `map10` loses 3×.  Separately, large literals cannot
compile at all: the expression's rodata + workspace must fit in the
`[16, 8192)` window below the runtime's `--global-base`, so a
1000-entry string list (~24 KB) is rejected (`celwasm-skip-rodata`
cells).

Both problems have one cause — literal aggregates are *runtime
constructions seeded from rodata* instead of *memory that already
exists* — and one fix.

## 2. Design in one sentence

The compiler writes, at compile time, the **byte-identical** in-memory
representation the runtime kernels would have built (headers + entry
runs + payloads, per `runtime/cel_data.h`), into the module's static
data; the aggregate expression lowers to a single `i32.const`; the
eval side changes **not at all**, because read-only kernels just read
bytes at offsets.

## 3. Pipeline flow

```
            kCreateList / kCreateMap node
                        │
        ┌─── IR pass: const-subtree annotation ───┐
        │ every element/key is a literal or a     │
        │ marked-const aggregate (recursively)?   │
        └───────┬───────────────────────┬─────────┘
            yes │                       │ no (idents, calls)
                ▼                       ▼
   StaticMemoryBuilder::             existing per-Eval
   Materialize{List,Map}             arena-build lowering
   (writes header+run+payloads      (unchanged)
    into the static window,
    innermost-first)
                │
                ▼
   node lowers to  i32.const <header_value_offset>
   (CelValue frame in rodata whose payload.arena_*.header_ptr
    points at the materialized header)
```

## 4. Memory map — before / after, with bases

Every base below 256 KB is compile-time-known and absolute; the
compiler computes them in this order per module (each `RoundUp16`):

```
                 BEFORE (today)                AFTER (m31)
offset 0     ┌────────────────────┐        ┌────────────────────┐
             │ reserved           │        │ reserved           │
offset 16    ├────────────────────┤        ├────────────────────┤  rodata_base = 16
             │ rodata             │        │ rodata             │
             │  (scalar frames    │        │  (scalar frames    │
             │   + payloads)      │        │   + payloads)      │
rodata_end   ├────────────────────┤        ├────────────────────┤  aggregate_base =
             │ workspace          │        │ MATERIALIZED       │   RoundUp16(rodata_end)
             │  (32 B slots)      │        │ AGGREGATES         │
             ├─ guard (256 B) ────┤        │  headers + entry   │
offset 8192  ╞════════════════════╡        │  runs + payloads   │
             │ ← --global-base    │        ├────────────────────┤  workspace_base =
             │ runtime statics +  │        │ workspace          │   RoundUp16(aggregate_end)
             │ libc shadow stack  │        │  (32 B slots)      │
__heap_base  ├────────────────────┤        ├─ guard (256 B) ────┤  workspace_end + 256
             │ dlmalloc heap      │        │ (free headroom)    │   must be ≤ 262144
             │ (arena, activation │        ╞════════════════════╡
             │  buffer, …)        │ 262144 │ ← --global-base    │  RAISED 8192 → 262144
             └────────────────────┘ (256K) │ runtime statics +  │  (one constant, ABI bump)
                                           │ libc shadow stack  │
                                __heap_base├────────────────────┤  build-dependent; exported
                                           │ dlmalloc heap      │   by the runtime, engine
                                           │ (arena, activation │   reads it at instantiate
                                           │  buffer, …)        │
                                           └────────────────────┘
```

Worked example — a module with 1 KB of scalar rodata, one
materialized 1000-entry string list (~24 KB), 40 workspace slots:

| region | base | size | end |
|---|---:|---:|---:|
| reserved | 0 | 16 | 16 |
| rodata | 16 | 1,024 | 1,040 |
| materialized aggregates | 1,040 | 24,576 | 25,616 |
| workspace (40 × 32 B) | 25,616 | 1,280 | 26,896 |
| guard | 26,896 | 256 | 27,152 |
| free headroom | 27,152 | — | 262,144 |
| runtime statics + shadow stack | 262,144 | build-dep. | `__heap_base` |
| dlmalloc heap (arena, …) | `__heap_base` | grows | memory end |

Layout gate: `guard_end ≤ 262144`, same rule as today against the
bigger constant.  Today the same module is REJECTED: 1,040 + 24,576
alone exceeds 8,192 before a single slot is placed.

The boundary raise is `-Wl,--global-base` in `runtime/BUILD.bazel` +
`CELWASM_RESERVED_LOW_MEMORY_BYTES` (`cel_layout.h`) +
`MemoryLayout` mirrors.  It is a runtime-layout change ⇒
`kRuntimeAbiVersion` bump (cheap; pre-ship posture, see
ANALYSIS.md).  Wasm memory is virtually reserved on 64-bit hosts, so
the larger min footprint is address space, not resident pages.

> Probe-confirmed (2026-06-12, throwaway footprint probe — `dynamic`
> link mode, 32→256 instances, vmmap; the probe binary was disposable
> and has since been removed, but the data it produced is recorded
> here): raising the base 8 KiB → 256 KiB moved `__heap_base`
> 283,424 → 537,376 and the declared memory min 5 → 9 pages, but
> per-instance **dirty** memory was byte-identical (VM_ALLOCATE dirty
> 14.0 MB → 87.5 MB in both builds; ~328 KB/inst dirty). The inserted
> window is demand-zero and never written for expressions that don't
> materialize aggregates, and wasmtime does not eagerly commit
> shared-memory minimums.  The resident cost of the raise is zero
> until an expression actually fills the window; §9's open question 1
> reduces to instantiate latency only.
>
> Per-instance footprint (same probe, static vs dynamic link): static
> ~5.2 MB resident/instance, dynamic ~0.55 MB resident/instance
> (dynamic shares the runtime → ~10× cheaper); virtual reservation is
> ~4 GiB/instance regardless of link mode (wasmtime reserves the full
> wasm32 address range per linear memory — reservation, not resident,
> so it does not OOM a 64-bit box).

> Deferred alternative (recorded, NOT in scope): a per-module data
> segment above `__heap_base` with either an ABI-pinned base or
> relocation at instantiate.  Strictly more capable (no fixed cap at
> all) and strictly more complex; revisit only if a real embedder
> outgrows the raised window.

## 5. Materialized layout — worked example

`[1, "hi", [2, 3]]` materializes innermost-first
(`runtime/cel_data.h`: `ArenaListHeader` 16 B `{count, capacity,
elements_offset, _pad}`; elements stride 24 B = `sizeof(CelValue)`;
map entries stride 48 B `{key, val}`):

```
static window (absolute offsets, illustrative)
┌─────────────────────────────────────────────────────────────┐
│ A: ArenaListHeader{count=2, cap=2, elements_offset=B, _pad}  │ inner [2,3]
│ B: CelValue{kind=INT, 2} CelValue{kind=INT, 3}               │ 2×24 B run
│ C: "hi"                                                      │ payload bytes
│ D: ArenaListHeader{count=3, cap=3, elements_offset=E, _pad}  │ outer header
│ E: CelValue{INT,1}                                           │ outer run,
│    CelValue{STRING, span{ptr=C, len=2}}                      │ 3×24 B
│    CelValue{LIST,  arena_list{header_ptr=A}}      ── inner ──┘
│ F: CelValue{LIST,  arena_list{header_ptr=D}}   ← the rodata  │
│                                  frame the i32.const returns │
└─────────────────────────────────────────────────────────────┘
read path at eval:  cel_list_at_arena(F, i) → loads D → run E →
element i — IDENTICAL code path as an arena-built list; the kernel
cannot tell (and must not be able to tell) the difference.
```

Maps additionally: the materializer **places entries by hash at compile
time** into the open-addressed Swiss-table layout the runtime reads
(§8 m31.B — supersedes the earlier "sort + binary-search" sketch).  The
compile-time image (control bytes + placed slots) is byte-identical to a
runtime-built map.  CEL maps have no observable order, so any
deterministic placement is valid — the only constraint is that the
materializer's hash matches the runtime's bit-for-bit (§8).

## 6. Eligibility and exclusions

Materialize iff every element (and every key) is a literal or an
already-materialized const aggregate — recursively.  Excluded, keep
the per-Eval build path:

  - any ident / call / comprehension anywhere in the subtree;
  - struct literals (host-side proto objects, different machinery);
  - duplicate-key const maps — keep the runtime path so the
    error surfaces with today's exact semantics (or reject at
    compile time iff the checker already would; decide in
    implementation against the oracle).

Load-bearing invariant (already true; m31 pins it): **read-only
kernels never mutate operand aggregates.**  `concat` allocates fresh;
iterators keep their state in the arena (`ListIterState` /
`MapIterState`), never in the list/map.  A materialized aggregate is
effectively rodata; any future kernel that wants to write into an
operand must first copy (tripwire test in §7).

## 7. WAT-first + test plan

  - `doc/…/wat/72_static_aggregate.wat` — hand-written module with a
    pre-materialized `[10,20,30]` + `{…}` in its data segment, $eval
    returning `lst[1]` and `size(m)`; assembled + run through
    wat_runner BEFORE any codegen C++.  Freezes the byte layout.
  - **Byte-identity test (the keystone):** e2e builds the same
    literal via the old arena path and memcmp's header+run+payload
    bytes against the materialized region.  Any drift between the
    materializer and `cel_make_*` fails by name.
  - Codegen golden tests: const list/map → single `i32.const`;
    ident-bearing aggregate → unchanged build sequence; nested const;
    const-inside-non-const (inner still materializes).
  - Conformance: new rows in `conformance/testdata/celwasm_edges.textproto`
    (or sibling m31 file): index/size/`in`/`==`/iteration over
    materialized lists+maps incl. 1000-entry (newly compilable),
    nested, map-key kinds, `==` between a materialized and an
    arena-built equal aggregate, concat of materialized operands
    (mutation tripwire).  Oracle-confirmed expecteds; monotonic
    baseline rises from 2035.
  - Un-skip: every `celwasm-skip-rodata` corpus cell + the e2e
    skips that cite the rodata cap (grep for the tag; each un-skip
    is "delete the skip line, confirm green").
  - Benchmarks: `size.map100` / `map10` / `list100` before/after;
    expectation: rebuild cost → ~0, `map100` flips from 0.02× to a
    large win.

## 8. Follow-up unlocked (separate slice)

**m31.A — const-map materialization, AS a Swiss table (committed: WILL
do).**  Two changes that land together — you don't materialize a
linear-scan map and convert it later; you materialize the final
representation directly.

**First, change the arena-map representation to an open-addressed Swiss
table** (absl `flat_hash_map`-style).  The linear-scan arena map is the
worst loss vs cel-cpp in the whole corpus (`size(map100)` 0.02× = 42×
slower; `in {…10…}` 0.25×; `map_str_i32` 0.60×) — O(N) probe, O(N²)
build.  The realistic fix is **not** a sorted-run + binary search (only
O(log N), and still needs a sorted build); it's a hash table:

  - **Layout:** `ArenaMapHeader` (`cel_data.h`) gains a power-of-two
    capacity + a **control-byte array** (1 byte/slot: 7-bit H2 hash tag
    + empty / deleted / full markers), followed by the slot array of
    `{key, val}` `CelValue` pairs.  Probe a group of control bytes,
    match the H2 tag, confirm the full key.  O(1) expected probe,
    cache-friendly.  Touches `runtime/cel_map.{c,h}`.
  - **Build:** O(N) insert-by-hash, replacing the O(N²)
    scan-for-last-write-wins; last-write-wins falls out of normal
    insert-overwrite.
  - **Deterministic, host==wasm hash.**  The materializer (host C++) and
    the runtime (wasm) must hash identically, or a materialized table
    won't match a built one.  No per-process-seeded hash (absl's default
    reseeds) — pin a fixed portable hash (seedless wyhash/xxhash).
  - **SIMD caveat:** the classic 16-wide `i8x16` control-byte group scan
    isn't available — the runtime is built without `-msimd128` (and SIMD
    has no portable per-module fallback, see the string-compare
    investigation).  The group probe is a **scalar word-at-a-time
    control scan** (8 control bytes per `i64`, has-zero bit-trick) —
    still O(1) expected, just not vectorized.  A SIMD build variant can
    add `i8x16` later.
  - Benefits **both** runtime-built and materialized maps.

**Then materialize const maps into that layout.**  Add
`StaticMemoryBuilder::MaterializeMap` + `ConstAggregateVisitor::
PostVisitMap`: the compiler computes the final Swiss-table image —
control bytes + hash-placed slots — at compile time and packs it into
rodata, **byte-identical** to what the runtime builder produces (the §2
premise holds; §5 "place entries by hash at compile time").  This also
lights up lists-of-const-maps automatically (the list materializer
already recurses into const-aggregate elements via
`IsConstMaterializable`).

**The full e2e matrix is already staged and GTEST_SKIP'd** in
`e2e/m31_static_aggregate_test.cc` (`ConstMapMaterializationTest`, 18
cases: every value kind, every valid key kind, size / `in` / equality,
nested map, list-of-maps, large map) — each verified to eval correctly
via the build path today; un-skip recipe is in
`SkipPendingMapMaterializer`.  Closing this slice = land the Swiss-table
representation, delete that skip, confirm green, add the codegen pin (no
`cel_map_create` for a const map) in `expr_lower_test`.

(Rejected alternative, kept for the record: sorted-run + binary-search
lookup — simpler but O(log N) probe and no build-cost win.)

## 9. Open questions

1. ~~Exact new window size~~ — **settled 2026-06-16, see §10.**  256 KiB,
   sized for 10K literal elements; resident-safe (demand-zero).
2. Dedup identical literals into one materialization — **promoted to a
   companion of §10** (cheap, orthogonal; collapses `1+1+…+1` with no
   window change).
3. Duplicate-key const maps: compile-error vs runtime-path (settle
   with the oracle, §6).

## 10. Queued: rodata window raise (calculated 2026-06-16)

Empirically grounded sizing (cel CLI `compile`, M4):

  - Each scalar literal occupies a **24-byte `CelValue` frame** in
    rodata (`kCelValueSize`); identical literals are **not deduped**,
    so `1+1+…+1` costs 24 B/term.  Workspace is flat (~32 B — the
    add-chain recycles one slot).
  - Today's window `[16, 8192)` minus the 256 B guard leaves ~7900 B →
    **~330 scalar literals max** (verified: N=340 fails with
    `rodata at [16, 8176)`).
  - **10K literal elements** ≈ `10000 × 24 + 40 (header+outer frame)` ≈
    **240 KiB** rodata.  Round the window to **256 KiB** (4 pages) for
    headroom.  100K ≈ 2.4 MB.

Resident cost (probe data, §4): the inserted window is **demand-zero**
— raising `--global-base` raises the *declared* memory minimum
(address space) but wasmtime does not eagerly commit it; pages go
resident only when written.  8 KiB→256 KiB left per-instance dirty
memory byte-identical (~328 KB/inst).  On 64-bit hosts each linear
memory already reserves ~4 GiB virtual, so the bigger min is noise.
**Static vs dynamic:** baseline differs ~10× (static ~5.2 MB/inst,
runtime merged; dynamic ~0.55 MB/inst, runtime shared) — but the
window raise adds ~0 resident in both, and a materialized 10K list
costs 240 KB resident **per instance that uses it** in both modes
(expr rodata is per-instance regardless of link mode).

Concrete change set (one focused commit, ABI bump):

  - `runtime/cel_layout.h`: `CELWASM_RESERVED_LOW_MEMORY_BYTES`
    8192 → 262144; `CELWASM_INITIAL_MEMORY_PAGES` 2 → 5 (keeps the
    `_Static_assert reserved < initial_pages × 64K`: 262144 < 327680 ✓).
  - `runtime/BUILD.bazel`: `-Wl,--global-base=8192` → `=262144`.
  - `MemoryLayout` mirrors (eval/compiler) + `kRuntimeAbiVersion` bump.
  - Un-skip every `celwasm-skip-rodata` corpus cell; full conformance.

Companion (no window/ABI change): **dedup identical literal frames**
in `StaticMemoryBuilder` — `1+1+…+1` (10K identical) collapses
240 KB → 24 B.  Does nothing for 10K *distinct* elements (those need
the window); orthogonal, ship either order.

## 11. Pre-close cleanup checklist (track to completion before closing)

The feature is functionally complete + e2e-verified.  Shipped as a PR
with the implementation as the first commit and these cleanups as
follow-up commits (squash-merge gives the one-commit history).

Lint / code shape:
  - [x] Split `LayoutPass` — extracted `PackRodata` + `CheckStaticWindowFits`.
  - [x] `IsConstMaterializable` loop → `std::all_of`.
  - [x] Parenthesize `r.elements_offset + (i * stride)`.
  - [x] Collapse `ConstAggregateVisitor`'s `consumed_` set vs. the
    element-stamping — `consumed_` removed; `ConstLayoutVisitor` now
    skips any const whose node already carries non-`kNone` storage
    (the element-stamping is the single mechanism).
  - [ ] `scripts/lint.sh --branch` clean over the whole slice.

Waste (correct but suboptimal — deferred, tracked):
  - [ ] String/bytes list elements waste a 24-byte frame each
    (`ConstToCelValue` → `AllocateString` writes an unused frame + the
    payload).  Add a payload-only allocate to drop the dead frame.
  - [ ] (Optional, §10 companion) dedup identical literal frames.

Stale `8192` → `262144` comments:
  - [x] `compiler/memory_layout.h`, `compiler/codegen/layout_pass.h`,
    `runtime/cel_layout.h`, `compiler/internal/compile.cc`,
    `eval/engine_test.cc`, `e2e/{limits,known_bugs}_test.cc`,
    `runtime/BUILD.bazel`, and the bench corpus headers / OPERATORS.md.

Tests / conformance:
  - [x] Retune `compile_test` rodata/workspace over-budget to the
    256 KiB window — rodata via a 12K-int list (a single string can't
    reach 256 KiB without tripping the ~100K source codepoint cap);
    workspace via a 9K distinct-variable list (nesting can't exhaust it
    within the 2048 parse-depth limit anymore).
  - [x] `known_bugs` `LiteralIntListInScan*` — bug FIXED by the window
    raise; converted to a now-evals regression (10K in-list).
  - [x] `engine_test` window-boundary cases retuned to 262144.
  - [x] e2e coverage for EVERY materializable element kind: null, bool,
    int, uint, double, string, bytes, nested list (m31 e2e suite).
  - [x] e2e nesting cross product — {list, map, struct} × {list, map,
    struct} (9 combos) + triples; list-only chains materialize, the
    rest build per-Eval, all eval correctly
    (AggregateNestingCrossProductTest + UnmaterializedElementTypeTest).
  - [x] Un-skip every `celwasm-skip-rodata` corpus cell (8 cells:
    arith `*1000TermsConst`, comprehensions `all1000`, long_strings
    `*_N10000*`, plus the lists/size cells) — each verified to compile +
    eval on celwasm via the cel CLI.  Conformance monotonic
    (2035 PASS / 0 FAIL both link modes).
  - [x] Also closed `celwasm-skip-arena-overflow`
    (`concatChain1000Terms`) — now evals (the arena grows in chunks).
  - [x] Tick `testing-checklist.md` m31 rows.

Docs:
  - [x] m31 status line → shipped; plan-vs-execution delta on the §4
    memory map; close-out per CLAUDE.md.  (wat-traces §72 already added.)

ABI:
  - [x] `kRuntimeAbiVersion` 2 → 3.  (No real users — compat irrelevant;
    kept because the runtime memory layout genuinely changed.)
