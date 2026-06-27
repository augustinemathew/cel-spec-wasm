# Independent adversarial review — const-MAP materialization (m31.A / m32.B)

Date: 2026-06-27
Reviewer: independent adversarial pass (read-only; no code changed)
Branch: `m32a-swisstable-map-index` (uncommitted working tree)
Scope: const map literals materialize into rodata with a baked SwissTable
index — `compiler/codegen/static_memory_builder.{h,cc}`,
`compiler/codegen/layout_pass.cc`, `compiler/codegen/expr_lower.cc`, the
paired tests, `wat/74_static_map_swisstable.wat`, and the m31/m32 docs.

---

## VERDICT: MIXED — one P1, no P0. Safe to push after the P1 is dispositioned.

The keystone is real and strong; byte-identity holds empirically across
key kinds and `num_slots` boundaries the committed tests do not cover (I
verified with a throwaway probe — removed). Placement parity, dup-key
handling, eligibility gating, the three modified pre-existing tests, repo
discipline, lint, and conformance (2035/2035 both modes) are all clean.

The one real defect: a **valid** const-map literal whose **string/bytes
key bytes sum to more than the 64 KiB native staging arena** CHECK-aborts
the compiler (not a clean `absl::Status`, not a build-path fallback). I
reproduced this through the full `Compile()` pipeline with an
under-the-codepoint-limit expression.

Top 3 to look at first:
1. **[P1]** `StagePlacementKey` arena-OOM aborts the compiler on a valid
   large-string-key const map (`static_memory_builder.cc:252`). Confirmed
   reachable end-to-end.
2. **[P2]** Keystone test coverage gap: byte-identity is pinned only for
   int (N=4/8/20) + string (N=10) keys. uint/bool/mixed keys and the
   `num_slots` 16→32 / 64→128 transitions (N=15, N=56) are unpinned in the
   committed suite (they pass — I probed them — but nothing guards them).
3. **[P2]** `compiler/codegen/BUILD.bazel` ends with two trailing blank
   lines (cosmetic; not C++-linted).

---

## What I verified (all green)

- `bazel build //...` (native + wasm32 cross-compile): **OK**.
- `bazel test //compiler/... //e2e/... //runtime/... //tools/wat_runner/...`:
  **114/114 PASS**.
- `scripts/check_conformance_monotonic.sh`: **dynamic 2035/2035 FAIL 0;
  static 2035/2035 FAIL 0** — no regression, baseline held in both modes.
- `scripts/lint.sh` on the four touched C++ files (`static_memory_builder.{cc,h}`,
  `layout_pass.cc`, `expr_lower.cc`): **clean, PCH loaded** ("pch=yes", so
  the real warning set, not the false-fire set).
- Throwaway byte-identity probe (added + run + removed): `MaterializeMap`
  baked index is `memcmp`-identical to `cel_map_create + N×cel_map_insert +
  cel_map_index_build` for **uint** keys at N∈{7,8,14,15,16,20,56,57,64},
  **negative-int** keys at N∈{7,8,15,16,64}, and **string** keys at
  N∈{7,8,15,16,64} — i.e. across the 16→32 (N=15) and 64→128 (N=56)
  `num_slots` transitions and the below/at-threshold boundary. All PASS.

### Byte-identity keystone (the gate) — strong

`StaticMemoryBuilderKeystoneTest` (`static_memory_builder_test.cc:698-796`)
builds the same map at runtime via the real runtime kernels, snapshots the
header + 48-B entry run + index block, then materializes the same entries
and `memcmp`s the entry run AND the index block. This is the right gate
and it is honest:
- int keys N=4 (below threshold, no index), N=8 (at threshold), N=20
  (`num_slots`=32) — compares count/cap + full 48·N run + full index block;
- string keys N=10 — index region (offset-independent) compared byte-for-
  byte, with the correct note that entry-run spans legitimately differ by
  base.

The `StagePlacementKey` hashing trick is provably sound: the staged key
(arena copy via `cel_make_string`/`cel_make_bytes`) and the rodata key
hash identically because `cel_map_key_hash`/`cel_value_eq` are
content-based (offset never enters the hash; embedded NUL is included —
`cel_hash_bytes` iterates `s.len`, never `strlen`). int/uint/bool keys
carry payload in-struct and pass through verbatim. The frozen shared
kernel (`runtime/cel_map_hash.h`) is `#include`d by both the bake and the
runtime, so H1/H2/group-probe/clone-mirror are one source of truth.

### Placement parity — identical

`PlaceBakedEntry` (`static_memory_builder.cc:263-296`) mirrors
`index_place_entry` (`cel_map_index.c:111-152`) line-for-line: same
triangular quadratic probe (`seq=(seq+step)&mask; step+=kGroupWidth`), same
`group_match`/`group_match_empty`, same dup-via-`cel_value_eq`, same
control-byte write + clone mirror (`if (slot < kGroupWidth-1)
ctrl[slot+num_slots]=h2`), same first-empty-lane placement. The
`kEmpty`-init of the full `ctrl_total_bytes = num_slots+7` span matches.
Empirically confirmed by the probe (memcmp across all boundaries).

### Dup-key handling — correct, every path

- ≥ threshold: `BakeIndexBlock` → `PlaceBakedEntry` returns false →
  `std::nullopt`; below threshold: O(n²) `cel_value_eq` scan, same
  predicate `cel_map_insert` uses. Both routes → `MaterializeMap` returns
  `nullopt` → node keeps the per-Eval build path → runtime poisons
  `CEL_ERR_DUPLICATE_KEY` with today's exact semantics.
- Cross-type numeric dup (int 1 vs uint 1) covered by
  `MaterializeMapCrossTypeNumericDuplicate`; the predicate is the same
  `cel_value_eq` for both bake and runtime, so all cross-type equal pairs
  the runtime would poison are also rejected here.
- A dup nested deep in a const subtree: `ElementValue` →
  `MaterializeMap`/`MaterializeList` propagate `nullopt` upward
  (`layout_pass.cc` `ElementValue` + the `if (!k.has_value() ...)` guards),
  so the WHOLE enclosing subtree falls back to the build path. Verified by
  reading; `ConstMapInsideNonConstListInnerStillMaterializes` covers the
  adjacent "inner const stays materialized, outer non-const builds" case.
- A non-dup map can't be falsely rejected: placement only returns false on
  a confirmed `cel_value_eq` match; the byte-identity probe (distinct keys
  at every N) never spuriously rejected.

### Arena-as-scratch hazard — safe (except the P1 OOM)

`StageAndDecideIndex` `arena_init` + `arena_reset`s the **native** host
arena each call. I confirmed the native arena is used by **no other
compiler component during layout** — every other `arena_*` reference in
`compiler/` is *emitting a wasm import call*, not calling the host arena
(grep: only `static_memory_builder.cc` calls the host arena). `MaterializeList`
never touches the arena, so list/map ordering is independent. For nested
maps the inner map's bytes are already copied into `buf_` (entry-run keys
point at rodata, not the arena) before the outer `arena_reset`, so resetting
between maps is safe. No corruption.

### Eligibility / gating — correct

- `IsConstMaterializable` kMapExpr arm requires every key AND value
  const-materializable and `!optional()` — optional/`?:` entries correctly
  excluded.
- A materialized map takes NO workspace slot: `AggregateStorageVisitor::
  PreVisitExpr` now shares the kListExpr `kStaticRodata` short-circuit with
  kMapExpr (the fix moved kMapExpr above the `[[fallthrough]]`).
- `EmitKMapExpr` lowers a `kStaticRodata` map to a single `i32.const`
  (mirrors the list arm), else CHECK-requires a workspace slot. A map with
  any ident/call/comprehension value stays on the build path (verified:
  `MapWithIdentValueKeepsBuildSequence`, plus the three pre-existing tests
  switched to non-const).
- Comprehension-accumulator maps excluded via `excluded_accu_` (records
  `accu_init().id()`), so a `transformMap` `{}` accu is not materialized.
- Empty map → `entries_offset=0`/`index_offset=0` (matches
  `cel_map_create(out,0)`); single entry → no index, no dup possible
  (`StageAndDecideIndex` `n<=1` early return). Both covered.

### The 3 modified pre-existing tests — legitimate, not papering

`LayoutPassMapTest.{ScalarMapLiteralGetsOneSlot…,MapLiteralIndexingReuses…}`,
`CompileMapTest.MapLiteralProgramLayoutReservesWorkspaceForMap`,
`ExprLowerMapTest.{ScalarMapLiteralEmitsCreateAndInserts,…}`, and
`NonOptionalMapLiteralEmitsOnlyPlainInsert` switched their const map to a
map with a variable value. This is correct: the old assertions ("a const
map gets a workspace slot / emits cel_map_create") are no longer true by
design, and the const-map case is now covered by the new e2e
`ConstMapMaterializationTest` (18 rows un-skipped) + the codegen-IR pins
(`ConstMapLiteralLowersToI32ConstNoBuild`). The variable-value variants
still exercise the build path each test was written to guard. No
regression is hidden.

### Repo discipline — clean

- No bare milestone refs in NEW code comments
  (`static_memory_builder.{cc,h}`, `layout_pass.cc`, `expr_lower.cc`); the
  m31/m32 mentions are confined to test/doc files, consistent with house
  style.
- Fail-loud: unreachable `ElementValue` default → `ABSL_CHECK(false)`;
  alignment / size invariants `ABSL_CHECK`'d. The ONE fallback that is
  invariant-true (arena OOM) is the P1 below — it CHECK-aborts, which is
  "loud" but at codegen time on valid input is the wrong loudness.
- Function sizes: all new functions well under the 60-line/40-stmt gate;
  lint confirms no `readability-function-size`.
- WAT trace `74_static_map_swisstable.wat` is documented, re-run by
  `wat_runner_test`, and its clone-mirror bytes (line 78) correctly equal
  ctrl[0..6] (line 77).
- Docs (`m31`, `m32`, `testing-checklist.md`, `wat-traces.md`) updated
  honestly in the same change, with the source-order-vs-sort delta called
  out and conformance 2035/2035 recorded.

---

## Findings by severity

### P1 — large-string-key const map aborts the compiler

**File:** `compiler/codegen/static_memory_builder.cc:252` (in
`StagePlacementKey`), reached via `StageAndDecideIndex` →
`MaterializeMap`.

**What:** Staging copies every string/bytes **key** into the native host
arena (fixed 64 KiB, `CELWASM_ARENA_CAPACITY_BYTES`; the native build's
`arena_alloc` does NOT grow — `cel_arena.c:196-197` `return 0` on the
`#else` path). When the sum of key bytes exceeds the arena,
`cel_make_string`/`cel_make_bytes` returns 0 and the
`ABSL_CHECK_NE(off, 0u) << "...arena OOM during codegen"` **aborts the
process**.

**Repro (confirmed end-to-end, not just the unit API):** a const map of
100 string keys ~700 B each (source well under the 100 000-codepoint
frontend limit, key bytes ~70 KB > 64 KB) drives `Compile()` to:
```
F static_memory_builder.cc:252] Check failed: off != 0u (0 vs. 0)
  StagePlacementKey: arena staging of a const map key failed (arena OOM
  during codegen)
```
The same map written with int keys compiles fine; the same map as a
*non-const* (build-path) map does not abort. So const-materialization is
*more* restrictive than the build path AND fails as an abort rather than a
clean status or a graceful fallback.

**Why it matters:** This is a valid, accepted-by-the-checker CEL
expression that crashes the compiler. Per CLAUDE.md the abort is at least
loud and named — but it is the wrong disposition for *valid user input*: a
fallible compile path should return `absl::Status` (or, cleaner, fall back
to the per-Eval build path the same way a dup-key map does). The fix is
small in spirit: treat staging OOM like the dup case — return `nullopt`
from `MaterializeMap` so the map keeps the build path — OR thread an
`absl::Status` out. (Note the OOM also half-fills the arena before
aborting; a `nullopt` fallback would need the arena state to be
inconsequential, which it is, since the next consumer re-inits.)

**Disposition for the gate:** I do not consider this *push-blocking* on its
own — it requires an adversarially large literal and fails loudly, not
silently. But it is a real correctness-of-disposition bug on valid input
and should get a tracked fix + a pinned case in `e2e/limits_test.cc`
(every fixed boundary belongs there per CLAUDE.md "Compilation limits").
Recommend: fix before merge if cheap, else file as P1 in the milestone's
pre-close cleanup with the limits_test case staged.

### P2 — keystone byte-identity coverage gap (uint/bool/mixed + num_slots transitions)

The committed keystone pins int (N=4/8/20) and string (N=10) only. uint,
bool, mixed-kind maps, and the N=15 (16→32) / N=56 (64→128) `num_slots`
transitions are unpinned. I verified all pass via a throwaway probe, so
there is no live bug — but the gate doesn't guard them, and these are
exactly the axes a future kernel/layout change could silently break.
Recommend adding a parameterized keystone over {int, uint, string} × N ∈
{7,8,15,16,56,64} (bool is degenerate above N=2 — drop or special-case).

### P2 — BUILD.bazel trailing blank lines

`compiler/codegen/BUILD.bazel` gains two trailing blank lines at EOF
(diff hunk `@@ -243,3 +245,5 @@`). Cosmetic; buildifier would strip them.

---

## Things I explicitly checked and found NOT to be problems

- Embedded-NUL string keys: hashed over `s.len`, never `strlen` — fine.
- `key_forces_linear` (double lookup key ≥ 2^53): applies only to *lookup*
  keys; stored/baked keys are bool/int/uint/string, so the baked index is
  never consulted with a forced-linear key in a way that diverges. No
  interaction with the bake.
- Nested const map inside const list inside const map: innermost-first
  PostVisit ordering + `frames_` memoization + `nullopt` propagation all
  correct; `MaterializeNestedList`/`MaterializeMapNestedListValue` and the
  e2e `AggregateNestingCrossProductTest` rows cover it.
- The clone-mirror wraparound (group load reaching `ctrl[num_slots..+6]`):
  init fills the full `num_slots+7` span to kEmpty; placement mirrors
  slot<7; probe across the boundary memcmp-passed.
- index block 8-alignment after the 48-B run: `AppendBakedIndex`
  `ABSL_CHECK_EQ(buf.size()%8,0)` then `PadTo(8)` — matches `arena_alloc`'s
  8-aligned placement.
