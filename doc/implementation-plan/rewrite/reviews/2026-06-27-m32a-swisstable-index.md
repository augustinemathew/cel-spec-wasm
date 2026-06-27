# Adversarial review — m32.A SwissTable map index

Reviewer: independent gate review (adversarial). Branch
`m32a-swisstable-map-index`, commits `8ecc8a45~4..HEAD`
(`ab45c53f` oracle → `7a6bb8a6` kernel → `8ecc8a45` runtime → `07be241e`
codegen). Date 2026-06-27.

## Verdict: CLEAN — no P0 / no P1 found.

I tried to break index↔linear parity (cross-type numerics, the 2^53
boundary, bool/int and string/bytes near-misses, UINT64_MAX, INT64_MIN,
empty/embedded-NUL strings), the SWAR probe (multi-group wrap, H2==0,
cloned-byte mirror), the build path (arena relocation, OOM, dup
re-validation, poison), the ABI surface, the codegen terminal-step
placement (literals + comprehensions + the cel.bind / list-comprehension
exclusions), and repo discipline. Everything held. The change is a
genuinely pure accelerator: every index consumer has an `index_offset ==
0` linear fallback, the host (eval) side never writes or reads
`index_offset`, and the `≥2^53` double-key linear fallback keeps the
index in exact parity with the lossy `cel_value_eq` scan it replaces. The
ABI bump (3→4) is the correct and sufficient compatibility gate (it is
emitted into `cel.abi` at `abi/cel_abi_emit.cc:55` and rejected on
mismatch at `eval/engine.cc:546`). Tests pass; I also ran an 8-case
throwaway parity probe over the highest-risk shapes — all green.

**Top 3 things the lead should still eyeball before pushing** (none
blocking):

1. **The `cel_map_key_hash` `default:` arm returns a sentinel instead of
   `ABSL_CHECK(false)`** (`runtime/cel_map_hash.h:279-287`). This is the
   *correct* choice here (the switch is open — a lookup key may be any
   `CelKind`, and the sentinel-miss matches the linear path), and it is
   commented as such, but it is the one place a reviewer should consciously
   sign off on "open switch, fallback is the spec'd behavior" rather than
   the usual fail-loud rule. Verified parity for list/null/duration lookup
   keys (sentinel → probe miss == linear miss). (P2-doc at most.)

2. **Index bytes are allocated for every host-consumed / proto-set map
   too** — codegen emits `cel_map_index_build` unconditionally as the
   terminal step (per the no-lazy-imports rule), so a map that is built
   then only snapshotted to the host pays one `arena_alloc` of dead index
   bytes. Correctness-neutral (the host read path only touches
   `count`/`entries_offset`); flagged purely as an arena-footprint note.

3. **Pre-existing uncommitted working-tree changes** (`runtime/cel_data.h`
   threshold comment, `benchmark/**` sweep results) are NOT part of the 4
   reviewed commits — they are a separate in-flight benchmarking task. The
   threshold stays 8; no semantics change. Confirm they are intended to
   ride along (or be staged separately) before the push so the commit is
   clean.

---

## What I verified (the load-bearing checks)

### 1. Index vs linear divergence — none found
- **Cross-type numeric keys**: `int N` / `uint N` / integral `double N.0`
  all fold to one token via `cel_hash_nonneg_integer` / the int/neg/uint
  arms (`cel_map_hash.h:160-189, 239-288`). Probed `int 105 → uint 105`
  (hit), `double(base+5) → int` below 2^53 (hit) — index == linear.
- **2^53 boundary**: `key_forces_linear` (`cel_runtime.c:35-41`) trips at
  `d >= 2^53` and `-d >= 2^53`. Boundary is *exactly* right: `2^53` →
  linear, `2^53-1` → index (exact, no rounding range below 2^53). Negative
  side via `-d`; NaN compares false on both (correct — a NaN key never
  matches a stored int/uint under `cel_value_eq`); ±Inf hits the same
  fallback or the double-sentinel. Probed `(double)INT64_MIN` (mag 2^63 →
  linear) and `2^53-20` window (index) — parity held.
- **bool vs int 0/1**: `kCelHashSaltBool` salts bool apart; `cel_value_eq`
  guards bool==bool only. Probed `bool true → {int…}` and `int 1 →
  {bool…}` — both MISS on index and linear.
- **string vs bytes same content**: salted apart
  (`kCelHashSaltStr`/`kCelHashSaltBytes`). Probed `bytes "k0" → {string
  "k0"…}` — MISS both.
- **embedded-NUL / empty string**: `cel_hash_bytes` is length-delimited
  (never `strlen`) and folds the length in; probed empty-string key (hit)
  and the suite covers embedded-NUL.
- **UINT64_MAX**: routes to `cel_hash_uint_high_token`; probed exact hit.

### 2. Hash canonicalization completeness
Every `cel_value_eq`-equal pair hashes identically: numeric equals fold to
two integer token spaces (non-neg `[0,INT64_MAX]`, uint-high
`(INT64_MAX,UINT64_MAX]`) plus a disjoint negative space; bool/string/bytes
get their own salts and never compare equal across the kind boundary, so a
distinct salt is correctness-neutral. The double arm hashes by the exact
integer it represents or returns the non-matching sentinel for
non-integral/NaN/±Inf/out-of-range — sound because such a double cannot
`cel_value_eq` any stored scalar key (range-guarded `cmp_int_vs_double` /
`cmp_uint_vs_double`, `cel_compare.c:132-145`).

### 3. SWAR probe correctness
- Cloned control bytes: first 7 mirrored at `ctrl[slot+num_slots]` on
  write (`cel_map_index.c:142-144`); a group load at the last slot reads 1
  real + 7 cloned bytes == the wrap the `(seq+lane)&mask` math expects.
- Triangular probe (`step += kGroupWidth`) visits every group once on a
  power-of-two table; max load factor 7/8 < 1 guarantees a
  `group_match_empty` stop — no infinite loop. (`num_slots` floored at 8 so
  one group is always a full load.)
- Lane extraction `__builtin_ctzll(mask)>>3` + `(seq+lane)&mask` confirmed
  against the multi-group / H2==0 tests in `cel_map_test.cc` and my probe.

### 4. Index build robustness
- Arena relocation: `m` and `hdr` are re-derived after `arena_alloc`
  (`cel_map_index.c:176-177`); `ctrl`/`slots` derived from the fresh base
  *after* that (183-186). No alloc happens during placement, so no stale
  pointer. `num_slots` is computed from `hdr->count`, which is invariant
  across the alloc — consistent between build and find.
- `index_offset` published strictly last (line 208), after full
  population — a reader seeing `!= 0` gets a complete block.
- `kEmpty` (0x80) init over the *full* cloned-control span (190-193), not
  the zero `arena_alloc` leaves (0x00 is a valid H2).
- Dup during build: poisons `CEL_ERR_DUPLICATE_KEY` and leaves
  `index_offset == 0` (199-203). Tested directly
  (`BuildReValidatesDuplicateOnDirectEntries`). Unreachable via codegen
  (`cel_map_insert` poisons dups first; `cel_map_insert_at` overwrites) —
  correctly defensive.
- OOM (`block_off == 0`): sets `index_offset = 0`, never poisons (178-181).
- `count < threshold`: sets `index_offset = 0`, no alloc (163-166).
- Double-build: harmless (rebuilds identically) and codegen never emits it
  twice — `cel.bind` is `kGeneric`, excluded from the comprehension build
  gate; a map literal builds once in `EmitKMapExpr`.

### 5. Codegen terminal-step placement
- Map literal: build pushed after the last insert, before the i32.const
  trailer (`expr_lower.cc:428`), same `out_slot`. Asserted in
  `expr_lower_test.cc` (slot match + position).
- Map comprehension: build pushed to the `guarded` block after the loop,
  gated on `kMapInsert/InsertIf/MapMerge/MapMergeIf`
  (`expr_lower_comprehension.cc:1122-1134`), targeting `c.accu_slot()`.
  On the range-absorption path the guard branches past the whole block, so
  it never runs on a poison accu (and `cel_map_index_build` no-ops on
  non-`CEL_MAP_ARENA` anyway).
- List comprehensions: not a kMap* shape → no build emitted (asserted
  `EXPECT_FALSE(... cel_map_index_build)` in `MapMacroEmitsListAppendAt`).

### 6. ABI bump
`kRuntimeAbiVersion` 3→4 is the right and sufficient gate: emitted into
`cel.abi` (`cel_abi_emit.cc:55`), checked at load (`engine.cc:546`). No
consumer reads the old `_pad` of `ArenaMapHeader` — host reads
(`ReadArenaMapHeader`, `ForEachArenaMapEntry`) only touch
`count`/`entries_offset`; the `_pad`-write sites at `cel_runtime.c:387/459/968`,
`static_memory_builder.cc:73/169`, `cel_host.cc:1289/1323` are all
`ArenaListHeader`/`CelValue`/time structs, NOT the map header. The
`_Static_assert(sizeof(ArenaMapHeader)==16)` is intact.

### 7. Test coverage
Suite covers parity over key×size, below/at threshold, num_slots
boundaries, multi-group probe, H2==0, the ≥2^53 fallback, cross-kind dup
poison, direct-entry dup re-validation, indexed `cel_map_eq_arena`,
embedded-NUL strings, e2e both link modes, oracle (21 cross-type cases),
WAT trace. My added probe covered bool/int and string/bytes near-misses,
INT64_MIN/UINT64_MAX/empty-string boundaries (the few shapes not already
named explicitly in `cel_map_test.cc`) — all parity-clean. Assertions are
pinned to `cel_value_eq`/oracle behavior, not guesses.

### 8. Repo discipline
- No bare milestone-tracker refs in new code — only design-doc *path*
  citations (`m32-swisstable-map-index.md §5.1`), which CLAUDE.md permits
  for non-obvious invariants (the §5.1 fallback qualifies).
- New functions all < 60 lines (`cel_map_index_build` 56, `find` 44,
  `index_place_entry` 42).
- C-header constraints honored: `cel_map_hash.h` is `static inline` +
  `extern "C"`, no ABSL (freestanding-safe), `memcpy` group load
  (strict-aliasing clean).
- `clang-tidy` clean on the C++ TUs; `cel_map_index.c` only trips the known
  `-mtail-call` host/exec-config infra error (lint-pch-exec-config gap),
  not a real finding.
- Tests run green: `//runtime:cel_map_hash_test`, `//runtime:cel_map_test`,
  `//tools/wat_runner:wat_runner_test`, `//compiler/codegen:expr_lower_test`,
  `//compiler/internal:compile_test`, `//testdata:cel_cpp_oracle_test`,
  `//e2e:m32_swisstable_index_test_{static,dynamic}`.

---

## Findings by severity

### P0 (must fix before push)
None.

### P1
None.

### P2 (cleanup-when-touched)
- **P2-1 `cel_map_hash.h:279` open-switch default** — returns a sentinel
  rather than `ABSL_CHECK(false)`. This is correct (open switch over an
  untrusted lookup-key kind; sentinel-miss == linear-scan miss) and is
  commented, but is the one spot where the fail-loud rule is intentionally
  relaxed. No action needed beyond a conscious sign-off.
- **P2-2 unconditional index alloc on host-consumed maps** — a map built
  then only snapshotted to the host still allocs an index block (dead
  bytes). Correctness-neutral; an arena-footprint micro-optimization
  (decision §14 #1 already defers selective suppression).
- **P2-3 pre-existing uncommitted `benchmark/**` + `cel_data.h` threshold
  comment** — outside the 4 reviewed commits; ensure the push stages only
  the intended files (CLAUDE.md: stage explicitly, never `git add -A`).
