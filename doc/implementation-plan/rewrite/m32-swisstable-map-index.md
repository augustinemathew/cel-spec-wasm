# m32 — SwissTable hash index for arena maps

Status: plan — drafted 2026-06-14, not yet started.

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
open-question #1 ("hash table for larger maps; linear scan is fine for
≤20 entries; past that lookup goes quadratic across repeated
indexing"), with `ArenaMapHeader._pad` explicitly reserved (`cel_data.h:75`)
"for a bucket-table offset when hashing is added."

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

`num_slots` = smallest power of two ≥ `ceil(count / (7/8))`, floored to
`kGroupWidth = 8`. Power-of-two so `H1 & (num_slots-1)` replaces a
modulo. Max load factor **7/8** (Abseil's `kMaxLoadFactor`). Because
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
// Abseil MaskEmptyOrDeleted (empty-only here, since no deletes).
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
(`is_valid_map_key_kind` rejects double on insert), but a **lookup** key
may be a double (`cel_runtime.c:30-33`).

**Canonicalization rule** (hash all numerics by their mathematical
integer value when integral):

- `[0, INT64_MAX]` → hash the canonical `int64` token, so `int 5`,
  `uint 5`, and `double 5.0` collide.
- `(INT64_MAX, UINT64_MAX]` → uint-only token (only a `uint` key, or a
  `double` that truncates here, can match); `uint 2^63` and
  `double(2^63)` take the same token.
- Non-integral / NaN / ±Inf / out-of-range doubles → a fixed
  non-matching sentinel hash. Sound because such a double **cannot
  compare equal to any stored int/uint key** (`cmp_int_vs_double` /
  `cmp_uint_vs_double` only report equal when `(double)stored == d`,
  forcing `d` integral and in range), so it probes, finds no
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
path stays bit-identical to today). **This threshold MUST be
oracle-confirmed** before merge — extend `testdata/cel_cpp_oracle_test.cc`
with `double in {bigInt: v}` cases per CLAUDE.md's oracle discipline;
the oracle outranks any source-reading guess about where cel-cpp's
cross-type equality flips.

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
  `num_slots` sizing boundaries (count = 7, 8, 9 → 16).
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
- Reconcile: `map-list-dispatch.md` §10 #1 (tick), `m31` cross-ref,
  `testing-checklist.md` rows.

## 14. Open questions (for the user — not pre-committed)

1. **Suppress `cel_map_index_build` for iterate/size-only maps?** If
   ResolvePass proves a map is never indexed or `in`'d, codegen could
   skip the build to avoid paying it for never-probed maps. Compile-time
   optimization; out of scope here.
2. **Static-index + dynamic-values variant** (§7) — worth it, or keep
   the mixed case on m31's per-Eval path with a runtime-built index?
3. **Threshold value** — confirm 8 against the bench sweep (§10).

## 15. Future work

- wasm v128 SIMD group scan (the layout already supports it; only the
  scan changes).
- u16 slot array for `capacity < 64k` to shave index memory.
- Selective index suppression (open question #1).
