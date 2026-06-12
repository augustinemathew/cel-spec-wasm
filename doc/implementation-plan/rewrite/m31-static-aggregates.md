# m31 — compile-time materialization of constant aggregate literals

Status: plan — drafted 2026-06-12, not yet started.

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

Maps additionally: the materializer **sorts entries at compile time**,
which is free here and unlocks the §8 follow-up (binary-search lookup
above a size threshold) without a second layout change.  Until that
kernel lands, sorted order is also valid linear-scan input — the
runtime's lookup semantics do not depend on insertion order (CEL maps
have no observable order; equality/iteration semantics pinned by
conformance rows below).

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

m31.B — sorted-run binary-search lookup for materialized maps above
~32 entries (`cel_map_lookup_arena` gains a sorted-flag arm or a
header bit).  Compile-time sorting in §5 already produces the input.

## 9. Open questions

1. Exact new window size (256 KB proposed; measure instantiate cost
   at 1 MB before choosing).
2. Dedup identical literals into one materialization (cheap win,
   decide during implementation).
3. Duplicate-key const maps: compile-error vs runtime-path (settle
   with the oracle, §6).
