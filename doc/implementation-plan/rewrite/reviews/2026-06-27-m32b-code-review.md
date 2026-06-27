# m32.B const-map materialization — code review (2026-06-27)

## Verdict: YES (ready)

The const-map materialization slice is readable, no more complex than the
problem requires, and I found no correctness defects. The byte-identity
keystone is the right gate and it is backed by structure tests, behavioral
e2e tests (including a baked-index N=100 map exercised through real wasm),
and a fail-loud-vs-graceful-fallback regression pin in `limits_test`. The
two findings below are P2 test-coverage gaps, not blockers.

Top 3 things to look at first:

1. The staging mechanism (`StagePlacementKey` + `StageAndDecideIndex`) and
   its `arena_init`/`arena_reset`-per-map reuse — confirmed sound; the
   native `cel_memory_base_()` is a fixed static buffer so staged-key
   pointers never dangle through a bake (`runtime/cel_memory.c:57-60`).
2. The two dup-detection paths agreeing across the threshold boundary —
   below-threshold O(n²) `cel_value_eq` scan vs at/above-threshold
   SwissTable index-placement detection. They agree *because* the §5
   hash-canonicalization invariant holds; I verified a cross-type
   (int 3 / uint 3) dup at N=8 is correctly rejected by the baked path
   (probe; see P2 #1).
3. `MaterializeMap`'s header-field write order + `AppendBakedIndex`'s
   in-place `index_offset` patch — placement/alignment parity with
   `cel_map_index_build` confirmed.

---

## What I verified (since the verdict is YES)

- **Byte-identity bake vs `cel_map_index_build`.** `PlaceBakedEntry`
  (`static_memory_builder.cc:270`) mirrors `index_place_entry`
  (`cel_map_index.c:111`) line-for-line: same triangular probe
  (`seq`/`step += kGroupWidth`), same `group_match` candidate loop with
  `cel_value_eq` re-check, same `group_match_empty` stop, same
  `ctrl[slot]=h2` + cloned-mirror write
  (`if (slot < kGroupWidth-1) ctrl[slot+num_slots]=h2`), same `slots[slot]=i`.
  `BakeIndexBlock` kEmpty-inits the full `ctrl_total_bytes` span (incl. the
  7 clone bytes) exactly as the runtime's init loop
  (`cel_map_index.c:190-193`). Geometry helpers (`IndexCtrlTotalBytes`,
  `IndexAlignUp4`, `IndexSlotArrayOffset`, `IndexBlockBytes`) are exact
  re-derivations of the file-static helpers in `cel_map_index.c:41-57`.
- **Hash/eq content-parity for span keys.** The bake hashes the *staged*
  key (bytes copied into the codegen arena via `cel_make_string/bytes`),
  the runtime hashes the entry-run key; both `cel_hash_bytes`
  (`cel_map_hash.h:195`) and `cel_byteptr_equal_` (`cel_internal.h:192`)
  read content + length and never fold in `s.ptr`, so the offset
  difference is invisible. Confirmed by the keystone matrix's String rows
  comparing the index block bit-for-bit.
- **Header field writes.** count/capacity = N, `entries_offset` = abs run
  offset (0 when empty), `index_offset` = 0 placeholder patched by
  `AppendBakedIndex` (or left 0 below threshold / empty). Matches
  `ArenaMapHeader` layout {count@0, cap@4, entries_offset@8,
  index_offset@12} and `cel_map_index_build`'s index_offset=0 path for
  count < threshold.
- **Alignment.** Header sits 8-aligned (CHECKed); 48·N run is 8-aligned;
  `AppendBakedIndex` CHECKs `buf.size()%8==0` before placing the block, so
  the block lands 8-aligned exactly as `arena_alloc` would. Slot array is
  align_up_4 within the block on both sides.
- **Empty / single-entry / nested.** Empty → no run, entries_offset=0,
  index_offset=0 (matches `cel_map_create(out,0)` + index_build's
  count<threshold). N=1 → `StageAndDecideIndex` short-circuits (`n<=1`),
  no dup possible, no index. Nested map/list values embed by `.frame`;
  the bottom-up memoized `ConstAggregateVisitor` materializes inner first.
- **Dup detection, cross-type, both paths.** Below threshold: O(n²)
  `cel_value_eq` (tests `...DuplicateKeyBelowThreshold`,
  `...CrossTypeNumericDuplicate`). At threshold: index-placement dup
  (test `...DuplicateKeyAtThreshold`, same-type). I ran a throwaway probe
  for the *cross-type* at-threshold case (int 0..6 + uint 3): correctly
  `materializable=0`; 8 distinct ints → `materializable=1`. Removed the
  probe after.
- **Staging arena reuse.** `arena_init(SAME_CAP)` is idempotent;
  `arena_reset()` per map is safe because each bake completes (copies the
  block into the host `buf_`) before returning and nothing holds live
  arena pointers across calls. Native base never relocates.
- **Visitor gating.** `AggregateStorageVisitor::PreVisitExpr`
  (`layout_pass.cc:491`) skips the workspace-slot Acquire when a list/map
  is already `kStaticRodata` — so a materialized const map costs no slot;
  `expr_lower.cc:410` (`EmitKMapExpr`) returns `i32.const` of the frame
  offset, mirroring the existing list arm (`expr_lower.cc:542`). The
  comprehension accu-init exclusion (`excluded_accu_`) keeps a mutated
  accumulator off rodata. Optional entries (`en.optional()`) and
  `Repr::kType` consts excluded in `IsConstMaterializable`.
- **Fail-loud discipline.** OOM in key staging returns nullopt → caller
  keeps the per-Eval build path (NOT an abort); pinned in
  `limits_test.cc` (`LongStringKeyMapLiteral`). All other invariant
  violations are `ABSL_CHECK` (cursor alignment, span-key bounds,
  closed-set constant variant). No silent fallbacks.
- **Repo hygiene.** No milestone refs in new code comments (doc-path
  citations only). All new functions ≤ 49 lines, within the 60/40/15/6/5
  gate. `Unwrap` test helper uses `ABSL_CHECK(has_value())` so the
  optional-access checker models the guard — sound.

---

## Findings

### P0 — none.

### P1 — none.

### P2 (cleanup-when-touched)

- **#1 — No automated test pins the cross-type-numeric dup at/above
  threshold.** `static_memory_builder_test.cc:649` covers cross-type
  (int/uint) dup only *below* threshold (N=2), and
  `...DuplicateKeyAtThreshold` covers only a *same-type* dup at N=8. The
  load-bearing §5 invariant (int/uint/double of the same integer hash
  identically, so the baked index placement detects them as dups) is
  therefore exercised at threshold only by the live conformance corpus,
  not by a focused unit test. I confirmed the behavior is correct with a
  throwaway probe (int 0..6 + uint 3 at N=8 → not materializable). Add a
  `MaterializeMapCrossTypeNumericDuplicateAtThreshold` case (and ideally
  an integral-double-vs-int variant) so the invariant has a regression
  pin at the index path, not just the linear path.
  Repro for the pin: 7 distinct int keys + `{uint key == one int key}`
  as the 8th entry; assert `MaterializeMap(...).has_value() == false`.

- **#2 — `wat_runner_test.cc` / `wat/74_static_map_swisstable.wat`
  not cross-read in depth.** The WAT trace + runner test were added but I
  reviewed only that they exist and the codegen-IR pin lives in
  `expr_lower_test.cc`. The byte-identity keystone + e2e suite already
  prove the runtime behavior, so this is a documentation-completeness note,
  not a coverage hole. Confirm the WAT disassembly assertion (if any)
  tolerates Binaryen-assigned names per the WAT-first workflow.

---

## Doc drift

None blocking. `m31-static-aggregates.md` and `m32-swisstable-map-index.md`
are updated in the same change set; the e2e suite's "DEFERRED sibling"
banner was correctly rewritten to "Const-map materialization" and the
`SkipPendingMapMaterializer` macro + its skips were deleted (the un-skip
recipe was honored). `testing-checklist.md` rows updated.

## Note for the caller

The task asked me to write to `2026-06-27-m32b-code-review.md`; a sibling
file `2026-06-27-m32b-const-map-materialization.md` already exists in the
reviews dir (likely an earlier design/closeout note). I did not touch it.
