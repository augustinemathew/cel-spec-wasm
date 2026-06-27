# m32 — SwissTable hash index for arena maps

Status: m32.A in progress — the shared hash kernel (`cel_map_hash.h`) and the
runtime index kernels (`cel_map_index.c`; `cel_map_lookup/in/eq` dispatch)
have landed, but are DORMANT: no compiled module calls `cel_map_index_build`
yet, so every map still linear-scans until the codegen wiring lands.
Remaining for m32.A: codegen emits the terminal build call, then conformance
(monotonic), benchmarks, independent review.  m32.B (codegen-baked index)
still planned, blocked on the const-map materializer (see the reconciliation
note below).  Drafted 2026-06-14; reviewed + reconciled against mainline
2026-06-27 (dependency clarified, file:line cites refreshed).

> **What landed (m32.A, 2026-06-27).**  `ArenaMapHeader._pad` →
> `index_offset`; `kRuntimeAbiVersion` 3 → 4; the shared kernel
> `runtime/cel_map_hash.h` (already in tree) is consumed by a new
> `runtime/cel_map_index.c` providing `cel_map_index_build` (codegen-export,
> terminal map-construction step) + `cel_map_index_find`.  The keyed kernels
> `cel_map_lookup_arena` / `cel_map_in_arena` / `cel_map_eq_arena` use the
> index when `index_offset != 0` and the key does not force linear (the
> ≥2^53 double-key fallback, §5.1), else the existing linear scan verbatim.
> Below `kCelMapIndexThreshold = 8` no index is built.  Pure accelerator:
> OOM / tiny / dup-on-build all degrade to (or poison exactly as) the
> linear path.  Coverage: index-vs-linear parity over the key×size matrix,
> multi-group probe, H2==0, the ≥2^53 fallback, dup re-validation, and
> indexed `cel_map_eq_arena` in `runtime/cel_map_test.cc`.  The codegen
> emission of `cel_map_index_build` as the terminal construction step
> (`compiler/codegen`, §8) and the WAT trace (§11) are NOT part of this
> runtime slice — the kernel + ABI are frozen here; wiring codegen to call
> the new export is the follow-up that turns the win on for compiled maps.

> **Reconciliation against mainline (2026-06-27).**  The design holds as
> written; two things to fix before reading §§5–9 literally:
>
> 1. **m32.B is blocked on const-map materialization, which has NOT
>    shipped.**  m31 landed const-*list* materialization only
>    (`StaticMemoryBuilder::MaterializeList`); there is **no
>    `MaterializeMap`** yet — it's the queued m31 follow-up
>    (`static_memory_builder.h`: "for now — maps … sibling follow-up").
>    So §7 (codegen-baked index) and §8's "runtime-built index is
>    byte-identical to the codegen-baked one" both ride a prerequisite
>    that isn't in the tree.  Read "m31's `MaterializeMap`" as "the
>    const-map materializer, landed first."  **m32.A (runtime-built
>    index) has no such dependency** and is the bulk of the win — ship it
>    alone; its correctness is checked by index-vs-linear parity (§12),
>    not by byte-identity against a baked index that doesn't exist yet.
>
> 2. **`kRuntimeAbiVersion` is currently 3** (`abi/runtime_catalogue.h`);
>    the layout change bumps it to 4.
>
> Refreshed file:line cites (the originals drifted on mainline): there is
> no `map_keys_equal` function — numeric-aware key equality is
> `cel_value_eq` (`cel_runtime.c:655`), which dispatches to
> `numeric_compare_kernel` (`cel_compare.c:157`); double keys are rejected
> on insert by `is_valid_map_key_kind` (`cel_runtime.c:21`) but reach the
> numeric path on lookup because `cel_map_lookup_arena` (`:177`) does not
> kind-check the key; range-guarded cross-type compares are
> `cmp_int_vs_double` (`cel_compare.c:132`) / `cmp_uint_vs_double` (`:139`).
> `ArenaMapHeader` is `cel_data.h:71-79` (16-byte static_assert intact),
> 4th field still named `_pad` (the `→ index_offset` rename happens at
> implementation); the 48-byte entry stride is `kCelMapEntryStride`
> (`cel_data.h:189`).

Synthesized from three parallel design explorations (index-over-entries
layout, inline-SwissTable layout, codegen-time materialization + shared
hash kernel). The competing-layout exploration converged: **both layout
agents recommend the index-over-entries design** (§3), because arena
maps are pre-sized and never grow — the inline-table's only structural
advantage is moot here, while its blast radius (sparse-walk rewrites
across `runtime/` + `eval/` + fixtures + hash-fragile WAT traces) is
maximal.

## 1. Problem

Arena maps (`runtime/cel_data.h` `ArenaMapHeader`; kernels in
`runtime/cel_runtime.c`) do **O(n) linear scan** for every keyed
operation: `cel_map_lookup_arena`, `cel_map_in_arena`, the
duplicate-key check in `cel_map_insert`, the collision check in
`cel_map_insert_at`, and the inner match loop of `cel_map_eq_arena`
(making map `==` O(n²)). This is `map-list-dispatch.md` §10
open-question #1 ("Hash table for larger maps. Linear scan in
`cel_map_lookup_arena` is fine for ≤20 entries; past that, lookup goes
quadratic across repeated indexing"), which already reserves the
`ArenaMapHeader._pad` field (`cel_data.h:75`) "for a bucket-table offset
if a bench motivates adding one" (`map-list-dispatch.md` §10 #1,
mirrored in the struct comment at lines 232–233).

m31 (compile-time materialization) fixes the *rebuild* cost of constant
literals but leaves every *lookup* O(n). A comprehension like
`m.exists(k, m[k] > threshold)` over a 100-entry host/arena map still
pays a linear scan per iteration.

## 2. Design in one sentence

Add an Abseil-style SwissTable (open addressing + control bytes +
portable SWAR group probing) as a **separate index over the existing
insertion-ordered entries run**; for constant map literals the compiler
**computes the index at codegen time and bakes it into static memory**
(riding m31), so the runtime does zero index-construction work and the
hot lookup path makes one branch (`index_offset != 0`) decided once at
construction — never per operation.

## 3. Chosen layout: index over insertion-ordered entries

The 48-byte `(key, val)` entries run at `entries_offset` stays exactly
as today, in insertion order. A separate index in the arena maps a
hash → an **entry index** `[0, count)` into that run.

### 3.1 Header — `_pad` → `index_offset` (stays 16 bytes)

```c
typedef struct {
  uint32_t count;          // unchanged: live entries
  uint32_t capacity;       // unchanged: entries-run slots (pre-sized)
  uint32_t entries_offset; // unchanged: 48B (key,val) run, insertion order
  uint32_t index_offset;   // was _pad. 0 => no index built; linear-scan fallback.
} ArenaMapHeader;           // still 16 bytes; _Static_assert(sizeof==16) preserved
```

`index_offset == 0` is the universal fallback sentinel (arena offset 0
is never a valid allocation — it is the absent sentinel everywhere in
this runtime). Every consumer checks `hdr->index_offset != 0` before
probing; otherwise it runs today's linear scan unchanged. The feature
is therefore **purely additive and risk-bounded**: OOM, tiny maps, or
any unhandled shape degrade to current correct behavior.

> This is a runtime-layout change → bump `kRuntimeAbiVersion` (same
> posture as m31's global-base bump). `cel_map_size_arena`,
> `cel_map_count`, and the `cel_map_iter_*` kernels read only
> `count`/`entries_offset` and are **unaffected**.

### 3.2 Index block layout

One contiguous arena allocation: `[control bytes][pad to 4B][slot array]`.

```
index_offset + 0                  : ctrl[0 .. num_slots-1]      (1 byte each)
index_offset + num_slots          : ctrl clone[0 .. 6]          (mirror first 7 bytes)
index_offset + align_up_4(...)    : slot[0 .. num_slots-1]      (u32 each = entry index)
```

- **Control bytes**: `num_slots + (kGroupWidth-1)` bytes. The trailing
  `kGroupWidth-1 = 7` bytes **mirror** the first 7 control bytes
  (Abseil's "cloned control bytes"), so an 8-byte SWAR group load
  starting at any slot reads valid bytes without a wrap branch.
- **Slot array**: `num_slots × u32`, each holding the entry index for a
  full slot (undefined for empty slots — emptiness is decided solely by
  the control byte). u32 (not u16) keeps it simple; ~5 bytes/slot total.

`num_slots` = smallest power of two ≥ `ceil(count / (7/8))`, with a
floor of `kGroupWidth = 8` (so one group is always a full load). Worked:
count 8→16, 14→16, 15→32, 56→64, 64→128. Power-of-two so
`H1 & (num_slots-1)` replaces a modulo. Max load factor **7/8**
(Abseil's `kMaxLoadFactor`). Because
arena maps are pre-sized and never grow, `num_slots` is computed once
from the final `count` — **no rehash, no tombstones, `kDeleted` is
never written** (the hardest half of a general SwissTable drops out).

### 3.3 Memory cost (vs. rejected inline design)

For `{0:0 … 63:63}` (N=64): index hybrid = header 16 + entries 3072 +
ctrl 135 + slots `128*4`=512 = **3735 B**, entries run byte-identical to
today. The rejected inline design = 16 + ctrl 135 + `128*48`=6144 =
**6295 B** (half the 48B-wide slot array empty). The index waste is
~5 B/slot vs the inline design's 24–48 B/slot.

## 4. SWAR group probing (portable, no SIMD)

We use Abseil's `GroupPortableImpl` (8-byte SWAR via `uint64_t`), **not**
SSE2 and **not** the wasm SIMD proposal — the runtime is wasm32 and we
do not assume SIMD. The *layout* is what must be frozen; the scan
strategy is an implementation detail (wasm v128 is a future option).

```c
#define kGroupWidth 8u
#define kEmpty   ((uint8_t)0x80)   // high bit set; empty slot
#define kDeleted ((uint8_t)0xFE)   // never written here (no deletes)
static const uint64_t kLsbs = 0x0101010101010101ULL;
static const uint64_t kMsbs = 0x8080808080808080ULL;

static inline uint64_t cel_h1(uint64_t h) { return h >> 7; }       // slot index bits
static inline uint8_t  cel_h2(uint64_t h) { return (uint8_t)(h & 0x7F); } // ctrl byte (top bit 0)

// Abseil Match: MSB of byte i set iff ctrl byte i == h2.
static inline uint64_t group_match(uint64_t ctrl, uint8_t h2) {
  uint64_t x = ctrl ^ (kLsbs * (uint64_t)h2);
  return (x - kLsbs) & ~x & kMsbs;
}
// Abseil GroupPortableImpl::MaskEmpty (shift-6: matches kEmpty only).
// We never write kDeleted, so MaskEmpty — not MaskEmptyOrDeleted
// (shift-7, which also matches kDeleted) — is the correct stop test.
static inline uint64_t group_match_empty(uint64_t ctrl) {
  return ctrl & (~ctrl << 6) & kMsbs;
}
```

A full slot's control byte is `cel_h2(h)` (range `0x00..0x7F`, top bit
clear), so a 7-bit H2 can never spuriously match `kEmpty`. Lane index =
`__builtin_ctzll(mask) >> 3` (lowers to wasm `i64.ctz`); clear with
`mask &= mask - 1`. The group load uses `memcpy` into a `uint64_t`
(strict-aliasing-clean; host UBSan-safe). LE is build-guarded
(`cel_data.h:208`).

Probe sequence = Abseil's triangular quadratic over groups
(`seq = (seq + step) & mask; step += kGroupWidth`), which visits every
group exactly once on a power-of-two table; load factor < 1 guarantees a
`group_match_empty` stop, bounding probe length.

## 5. The load-bearing correctness point: numeric-key hash canonicalization

`map_keys_equal` (`cel_runtime.c:36`) treats `int N`, `uint N`, and
`double N.0` as **the same key** via `numeric_compare_kernel`. The
SwissTable invariant is absolute: **keys that compare equal MUST hash
identically**, or `1.0 in {1:'a'}` probes the wrong group and returns a
spurious `no_such_key`. Stored keys are only bool/int/uint/string
(`is_valid_map_key_kind`, `cel_runtime.c:18`, rejects double on insert),
but a **lookup** key may be a double (`cel_runtime.c:29-34`).

**Canonicalization rule** (hash all numerics by their mathematical
integer value when integral):

- `[0, INT64_MAX]` → hash the canonical `int64` token, so `int 5`,
  `uint 5`, and `double 5.0` collide.
- `(INT64_MAX, UINT64_MAX]` → uint-only token (only a `uint` key, or a
  `double` that truncates here, can match); `uint 2^63` and
  `double(2^63)` take the same token.
- Non-integral / NaN / ±Inf / out-of-range doubles → a fixed
  non-matching sentinel hash. Sound because such a double **cannot
  compare equal to any stored int/uint key** (`cmp_int_vs_double`
  `cel_compare.c:135` / `cmp_uint_vs_double` `:142` reduce to
  `cmp_double((double)stored, d)` only after range-guarding, so they
  report equal only when `(double)stored == d` — i.e. `d` is a
  representable in-range integer), so it probes, finds no
  `map_keys_equal` confirmation, and correctly misses.
- `bool` → canonical `0`/`1` with a kind salt (collides harmlessly with
  numeric `0`/`1`; `map_keys_equal` rejects across the bool boundary).
- `string`/`bytes` → byte hash over `[s.ptr, s.ptr+s.len)` read through
  `cel_memory_base_()` (length-delimited; embedded NULs and multibyte
  UTF-8 included — never `strlen`).

**Equality always gates the hit.** Hash collisions are expected and
fine: after an H2 group match, `map_keys_equal(slot_key, query_key)` is
the authority. Canonicalization only guarantees *no false misses*.

`-INT64_MIN` magnitude is computed without UB
(`(uint64_t)(-(v+1)) + 1`). The mixer (`mix64` / byte hash) need not
match Abseil's — it is private to one map built and queried by the same
hash kernel — but it MUST NOT be `absl::Hash`/`std::hash` (seeded,
non-portable) because the same kernel runs in two builds (§6).

### 5.1 The P0 edge: large-int vs. rounding-double

For `|value| ≥ 2^53`, an integral double `d` can compare-equal to a
*range* of int64s that all round to `d`, while truncation picks only
one — so a double lookup near a rounding boundary could canonicalize to
a different token than an int it would compare-equal to under linear
scan. **Resolution**: when a double lookup key canonicalizes with
magnitude `≥ 2^53`, the lookup kernel **falls back to linear scan for
that single call** (one compare; common path stays O(1); pathological
path stays bit-identical to today).

> **Oracle verdict (2026-06-27) — keep the fallback; it matches our
> runtime.**  Confirmed against real cel-cpp (`cel_cpp_oracle_test.cc`,
> `MapKeyNumericCrossType`, 21 cases).  Two facts:
> - **Our runtime's map-key equality is the lossy `==` (`cel_value_eq`,
>   used at `cel_runtime.c:113/163/196`):** `(int64)v` is cast to double
>   and compared, so the double `2^53.0` is map-equal to BOTH `2^53` and
>   `2^53+1`.  The SwissTable index must agree with this linear scan, and
>   no single hash token can land a double on a *range* of ints — so the
>   `≥ 2^53` double-lookup-key linear fallback **stays**.  Its rationale
>   is "match the current runtime," and it is what keeps index↔linear
>   parity.  Below `2^53` everything is exact and the index handles it.
> - **cel-cpp itself is *exact* for map keys** (`dyn(2^53.0) in
>   {2^53+1:'a'}` → **false**, though `dyn(2^53+1) == 2^53.0` → **true** —
>   `==` is lossy, map lookup is not).  So our lossy `cel_value_eq`
>   map-key path is a **latent conformance gap** vs cel-cpp — but the
>   cross-type numeric key path is reachable only via `dyn` (no
>   homogeneous `int==double` / `uint in map<int>` checker overload),
>   which our static subset rejects, so it is unreachable through our
>   compiler today (live only in the shared runtime kernel).  Fixing it
>   (an *exact* map-key equality, distinct from the `==` operator, used by
>   both scan and index → drop the fallback) is **future work, out of
>   m32's pure-accelerator scope** (§15).  Likewise the insert-time
>   dup-key asymmetry — cel-cpp accepts `{1:'a', 1u:'b'}` (int/uint
>   distinct) but folds `{1:'a', 1.0:'b'}` to a dup error — is a
>   `cel_value_eq` property m32 inherits unchanged.

## 6. Shared hash kernel — `runtime/cel_map_hash.h`

The compiler (host C++, native arch) and the runtime (wasm32) must
compute **byte-identical** hash / H1 / H2 / probe placement / control
bytes, or a codegen-baked index won't match runtime lookups — a silent
miscompile surfacing as spurious `no_such_key`.

**One source of truth, header-only, included by both sides:**

| Artifact | Path |
|---|---|
| Shared hash kernel | `runtime/cel_map_hash.h` (`static inline`, `extern "C"`, no `.c` TU) |
| Target | `//runtime:cel_map_hash` (`hdrs=[...]`, `visibility=["//:internal"]`) |
| Kernel test | `runtime/cel_map_hash_test.cc` |

Layering is clean: `compiler/codegen` **already** deps
`//runtime:cel_runtime` (`static_memory_builder.h` includes
`runtime/cel_runtime.h`); consuming a runtime-rooted *data/layout*
header is the same edge as `compiler/memory_layout.h`'s
`static_assert`-parity against `//runtime:cel_layout`. The rule forbids
`compiler/ → eval/` and `compiler/ → runtime internals` (wasmtime glue,
dispatchers), not `compiler/ → runtime data headers`. Header-only
`static inline` (vs a `.c` compiled twice) removes the second build
target and any optimizer-drift surface; it mirrors the existing
`runtime/cel_internal.h` `spans_equal` / `numeric_compare_kernel`
pattern.

## 7. Codegen-time index (m32.B) — the headline goal

m31's `StaticMemoryBuilder::MaterializeMap` writes the `ArenaMapHeader`
+ entries run into static memory. m32.B **appends**, in the same static
window, immediately after the entries run:

```
[ ArenaMapHeader{count, capacity, entries_offset, index_offset} ]
[ entries run: capacity × 48B {key,val} ]            (m31)
[ control bytes: num_slots + 7 ]                     (m32.B)
[ slot array: num_slots × 4B = entry index ]         (m32.B)
```

The compiler instantiates the **same `cel_map_hash.h` kernel** over the
compile-time-constant keys, computes each key's slot placement and
control byte exactly as the runtime would, writes them, and sets
`header.index_offset` to the control-block offset. Runtime index work
for static maps = **zero**; the lookup hot path makes one branch
(`index_offset != 0`) — satisfying "lowest choice time."

**Const-keys vs const-values**: the index needs only constant *keys*.
m31 already gates on both keys and values constant; reuse that gate. A
"static index + runtime-filled values" variant (bake keys+ctrl+slots,
runtime-write values) is **out of scope** — it splits the contiguous
48B key/val pairing and adds a third byte-identity surface. The clean
wins are all-constant maps (m32.B) and all-runtime maps (m32.A);
constant-key/dynamic-value maps keep m31's per-Eval build path with a
runtime-built index (m32.A). Surfaced for the user to decide; not
pre-committed.

## 8. Runtime-built index (m32.A) — fallback + byte-identity reference

The dynamic path (comprehension accumulators via `cel_map_insert_at`,
and any non-constant map) builds the index at runtime via a new
`cel_map_index_build(map_slot)` kernel emitted by codegen as the
**terminal map-construction step**. Build is one O(count) pass:
`memset` control to `kEmpty`, then for each entry hash → probe for an
empty slot (or a confirming dup → poison `CEL_ERR_DUPLICATE_KEY`) →
write `ctrl[slot] = H2` (+ clone if `slot < 7`), `slot_array[slot] = i`.
On OOM or `count < kIndexThreshold`, leave `index_offset = 0`
(linear-scan; the index is a pure accelerator — never poison for
index-build failure).

To keep m31's keystone byte-identity test valid over the **whole**
materialized region (including the index), the runtime literal builder
inserts in the **same canonical sorted order** m31 uses, so a
codegen-baked index and a runtime-built index are byte-identical (not
merely semantically equivalent). This pins both producers to one
auditable layout.

**Ship order**: m32.A first (no m31 dependency; freezes the kernel +
layout; delivers the O(1) lookup win for every map). m32.B after m31's
`MaterializeMap` lands. If m31 slips, m32.A still ships the bulk of the
win; only "baked at codegen time" waits.

## 9. Kernel changes (each with `index_offset == 0` linear-scan fallback)

Shared helper `static uint32_t map_index_find(ArenaMapHeader*, const
CelValue* key)` → entry index `[0,count)` on hit, `UINT32_MAX` on miss.

| Kernel | Change |
|---|---|
| `cel_map_create` | optionally pre-allocate the index block sized from capacity (or defer to `cel_map_index_build`). Sets `index_offset` (0 until built). |
| `cel_map_insert` | per-insert dup check stays linear (index not built yet); `cel_map_index_build` re-validates uniqueness as it places, poisoning duplicates before the map is consumed. |
| `cel_map_insert_at` | linear collision-overwrite during the comprehension (index built at end). `num_slots` sized from final `count`, not capacity. |
| `cel_map_lookup_arena` | after 3VL/kind guards: `index_offset ? map_index_find : linear`. Double-key `≥ 2^53` → linear (§5.1). |
| `cel_map_in_arena` | same probe; `write_bool(out, hit)`. |
| `cel_map_eq_arena` | outer walk over `a` unchanged (dense run); inner match against `b` uses `map_index_find(hb, ka)` when `hb` indexed → O(n²)→O(n). |
| `cel_map_size_arena`, `cel_map_count`, `cel_map_iter_*` | **unchanged** (read `count`/`entries_offset`; walk dense run). |
| `cel_map_index_build` (new) | builds the index; `cel:codegen-export`. |

**No host-side changes**: `DecodeArenaMapEntries`, `SnapshotMapEntries`,
the proto-`Struct`-set path, and `AppendMapCanonical` (`%s`) all walk the
**dense entries run**, which is untouched — the decisive advantage of the
index-over-entries layout over the rejected inline design.

## 10. Threshold

`kIndexThreshold = 8` (default; bench-tunable). Below 8 entries,
`cel_map_index_build` is a no-op (`index_offset` stays 0) and all
kernels linear-scan: ≤8 × 48B entries fit in a few cache lines and the
scan beats hash + indirection. 8 aligns with `kGroupWidth`. A
`benchmark/eval` sweep over `{4,8,16,32}` × {string,int} keys pins the
constant (string keys' `memcmp` equality likely lowers the crossover);
cite the bench in the constant's comment per the benchmark-config rules.

## 11. WAT-first applicability

The emitted **codegen call shape is unchanged**: m32.B lowers a static
map to one `i32.const <header_offset>` (m31); m32.A calls
`cel_map_create`/`cel_map_insert`/`cel_map_index_build` with existing
signatures. No new ABI surface or instruction sequence. But WAT-first
governs *frozen memory layouts* too: add
`doc/.../wat/NN_map_swisstable_index.wat` (a hand-built map with a
pre-baked control-byte + slot array) run through `wat_runner`, asserting
`m[k]` resolves via the index — extending m31's static-aggregate WAT.
Freeze the index layout in WAT before any codegen C++.

## 12. Test matrix (interface → tests → implementation)

- `runtime/cel_map_hash_test.cc`: SWAR `group_match`/`group_match_empty`
  units; hash-canonicalization matrix — `int N`/`uint N`/`double N.0`
  collide for N ∈ {0, 1, -1, INT64_MAX, INT64_MIN, 2^53, UINT64_MAX};
  bool 0/1; embedded-NUL + multibyte-UTF-8 string keys; the `2^53` edge;
  `num_slots` sizing boundaries — `num_slots = max(8, next_pow2(ceil(8·count/7)))`,
  so count = 8, 9, …, 14 → 16 and count = 15 → 32 (count < `kIndexThreshold`
  = 8 builds no index, so the smallest indexed map sits at num_slots 16,
  load 0.5).
- `runtime/cel_map_test.cc` additions: index-vs-linear **parity** over
  the full key matrix (every assertion holds with and without the
  index); probe-collision (two keys whose H1 collide, both findable);
  H2 == 0 control byte; duplicate `{1:1, 1u:2}` still poisons; large-map
  lookup forcing multi-group probe.
- `testdata/cel_cpp_oracle_test.cc`: `1u in {1:'a'}`, `1.0 in {1:'a'}`,
  cross-kind dup key, and the §5.1 large-int/rounding-double cases —
  oracle-confirmed before locking the threshold.
- e2e: `1.0 in {1:'a'}`; comprehension over a large map; byte-identity
  cross-check (runtime-built index region `memcmp` == compiler-built).
- Conformance: **unaffected** — semantics are identical to linear scan;
  this is a pure lookup accelerator. No `.textproto` rows change.

## 13. Files touched

- `runtime/cel_data.h` — `_pad` → `index_offset`; `kRuntimeAbiVersion` bump.
- `runtime/cel_map_hash.h` (new) + `//runtime:cel_map_hash` + test.
- `runtime/cel_map.h` — declare `cel_map_index_build` (`cel:codegen-export`).
- `runtime/cel_runtime.c` (or new `runtime/cel_map_index.c` TU) —
  `map_index_find`, `cel_map_index_build`, edits to lookup/in/eq/insert.
- `compiler/codegen/static_memory_builder.{h,cc}` — emit index bytes
  (m32.B); add `//runtime:cel_map_hash` dep.
- `compiler/codegen` — emit `cel_map_index_build` as terminal
  construction step (m32.A).
- `doc/.../wat/NN_map_swisstable_index.wat` + `wat-traces.md`.
- `runtime/cel_map_hash_test.cc`, `runtime/cel_map_test.cc` additions,
  oracle + e2e cases.
- Reconcile (on ship): resolve `map-list-dispatch.md` §10 #1, drop
  `m31` §8 (m31.B), tick `testing-checklist.md` rows. (As a plan, m32
  only forward-references these today.)

## 14. Decisions of record (2026-06-27)

The questions the design left open are now resolved.  Each is stated as
the decision + rationale; revisit only with a reason.

1. **Index-build suppression for iterate/size-only maps — deferred, not
   done.**  A map ResolvePass proves is only iterated / `size`d could skip
   `cel_map_index_build`, but that is a compile-time optimization
   orthogonal to this runtime layout and is addable later off the existing
   usage annotations.  m32 does not gate on it.

2. **Static-index + dynamic-values variant — dropped.**  Constant-key /
   dynamic-value maps stay on the per-Eval build path with a runtime-built
   index (m32.A).  Baking keys+index while writing values at runtime would
   split the contiguous 48B key/val pairing and add a third byte-identity
   surface for a rare shape — cost outweighs the win.

3. **Threshold = 8 by default, locked from the bench, measured per key
   type.**  Linear scan below 8 entries, hash index at/above.  String keys
   pay a `memcmp` per linear probe and so cross to the hash path at a lower
   count than int keys; with a single threshold the §10 `benchmark/eval`
   sweep ({4,8,16,32} × {string,int}) picks the string-key crossover (the
   lower one) so string-keyed maps benefit earliest.  Cite the bench in
   the constant's comment.

4. **The `≥ 2^53` boundary — RESOLVED by the oracle (2026-06-27): keep
   the fallback.**  Confirmed against real cel-cpp
   (`cel_cpp_oracle_test.cc::MapKeyNumericCrossType`, 21 cases; see §5.1's
   callout).  Our runtime's map-key equality is the lossy `cel_value_eq`,
   so a double `2^53.0` is map-equal to a *range* of ints — the
   `≥ 2^53` double-lookup-key **linear-scan fallback stays**, to keep the
   index in parity with the scan.  cel-cpp itself is exact, so our lossy
   path is a latent gap, but it is `dyn`-only (rejected by the static
   subset) → unreachable through our compiler and out of m32 scope.
   Making map-key equality exact (and dropping the fallback) is future
   work (§15).

5. **Ship m32.A first; m32.B waits on const-map materialization.**  m32.A
   (runtime-built index) is self-contained, freezes the kernel + index
   layout, and delivers O(1) lookup for every map; ship it alone (its
   correctness is index-vs-linear parity, §12).  m32.B (codegen-baked
   index) rides the const-map materializer
   (`StaticMemoryBuilder::MaterializeMap`), which has NOT shipped — that
   materializer is m32.B's explicit prerequisite, scheduled first if m32.B
   is wanted.

## 15. Future work

- wasm v128 SIMD group scan (the layout already supports it; only the
  scan changes).
- u16 slot array for `capacity < 64k` to shave index memory.
- Selective index suppression (open question #1).
- **Exact map-key equality** distinct from the lossy `==` operator
  (oracle-confirmed: cel-cpp matches map keys exactly and treats int/uint
  as distinct insert keys — `cel_cpp_oracle_test.cc::MapKeyNumericCrossType`).
  Replacing `cel_value_eq` on the map-key path with an exact comparator
  (used by both scan and index) closes the latent `≥ 2^53` / int-vs-uint
  conformance gap and lets the index drop the `≥ 2^53` linear fallback.
  Currently `dyn`-only, so unreachable through the static subset — a
  conformance-fix, not an m32 accelerator concern.

> **Supersedes m31 §8 (m31.B).** m31's follow-up proposed a sorted-run
> binary-search lookup (`cel_map_lookup_arena` gains a sorted-flag arm)
> as the >32-entry accelerator. m32's O(1) SwissTable subsumes that O(log
> n) path and keeps the entries run in insertion order (m31.B would have
> required compile-time sorting). If m32 ships, m31.B is dropped; if m32
> slips, m31.B remains the interim option. Reconcile m31 §8 when m32
> lands.
