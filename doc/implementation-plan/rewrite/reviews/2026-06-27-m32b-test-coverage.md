# m32.B const-map materialization — test-coverage review (2026-06-27)

Reviewer: test-coverage gate. Diff under review: uncommitted working tree +
staged changes on top of `origin/master` (HEAD `c1d3a5e7`), branch
`m32a-swisstable-map-index`. **Report only — no code changed.**

## VERDICT: YES (re-review 2026-06-27)

All six gap-fixes (B2–B6) plus the code-review item (P2#1) have landed as
substantive, non-skipped, green tests; B1 was investigated and correctly
rejected (bytes is not a valid CEL map-key type). m32.B coverage is now
complete against the valid-key-kind set {bool, int, uint, string} and the
repo doctrine (component-wise units positive+negative+boundary; e2e in both
link modes). **Cleared to push.**

### Re-review confirmation (component → test → file:line, all verified RUN/OK, not skipped)

| Gap | Test | File:line | Asserts | Status |
|---|---|---|---|---|
| B2 (layout-pass positive) | `LayoutPassMapTest.ConstMapLiteralMaterializesToRodata` | layout_pass_test.cc:561 | root kMapExpr `storage.kind == kStaticRodata`, `workspace_bytes == 0`, offset in rodata window | RUN/OK |
| B3 (layout-pass negative) | `LayoutPassMapTest.NonConstMapLiteralGetsOneWorkspaceSlot` | layout_pass_test.cc:585 | `{1: v}` → root `storage.kind == kWorkspaceSlot` | RUN/OK |
| B4 (comprehension value) | `ExprLowerMapTest.MapWithComprehensionValueKeepsBuildSequence` | expr_lower_test.cc:883 | `{1: [1,2,3].map(x,x)}` emits `cel_map_create` + `cel_map_index_build` (build path, not i32.const) | RUN/OK |
| B5 (call value) | `ExprLowerMapTest.MapWithCallValueKeepsBuildSequence` | expr_lower_test.cc:899 | `{1: 1 + 1}` emits `cel_map_create` (not bare i32.const) | RUN/OK |
| B6 (e2e baked-index lookup, both modes) | `ConstMapMaterializationTest.LargeConstMapLookupHit` / `LargeConstMapLookupMiss` | m31_static_aggregate_test.cc:587 / :599 | `IntMapLiteral(100)[42] == 42` (hit via baked index, N=100 >= threshold 8) and `[9999]` IsError (miss); ran in `_static` AND `_dynamic` | RUN/OK ×2 modes |
| P2#1 (cross-type dup at index path) | `StaticMemoryBuilderTest.MaterializeMapCrossTypeNumericDuplicateAtThreshold` | static_memory_builder_test.cc:668 | int 0..6 + uint 3 at N==8 → `MaterializeMap` nullopt (canonicalization dup detected on the SwissTable index path, not the linear scan) | RUN/OK |

### B1 — correctly out of scope (NOT a gap)

Bytes is **not** a valid CEL map-key type, verified three ways: `doc/langdef.md:341`
("Key values must be an allowed key type: `int`, `uint`, `bool`, or `string`" —
bytes absent); `runtime/cel_runtime.c:21` `is_valid_map_key_kind` returns true
only for CEL_BOOL/CEL_INT/CEL_UINT/CEL_STRING; and the runtime empirically
rejects bytes keys. Adding bytes-key tests would assert non-spec behavior and
permanently fail. The valid key-kind set {bool, int, uint, string} is fully
covered by the byte-identity keystone matrix (static_memory_builder_test.cc:869).
**B1 is withdrawn — do not re-raise.**

---

## Original first-pass verdict (superseded by the YES above): NO

Coverage is strong on the keystone (byte-identity) and the OOM fallback, and
all touched targets build + pass green. But there are concrete, load-bearing
gaps: **bytes-keyed maps are untested across the entire pipeline**, the
**layout-pass gating for maps has no direct positive/negative test**, the
**negative gating for non-ident non-const map values** (comprehension / call
value) is missing, and the **e2e index-baked (N >= 8) lookup hit/miss** is not
exercised end-to-end (only `size()` is). Per the repo doctrine (every map-key
type positive+negative; every touched source file gated positively AND
negatively; boundary matrix exhausted), these must be filled before the push.

> Re-review note: B1 (bytes key) was subsequently investigated and **rejected**
> — bytes is not a valid CEL map-key type (see "B1 — correctly out of scope"
> above). B2–B6 and P2#1 were filled. Verdict flipped to YES.

---

## Files in the diff

Source (working tree):
- `compiler/codegen/static_memory_builder.{h,cc}` — `MaterializeMap`, `MapEntry`,
  `MaterializedMap`, baked-index helpers, `StagePlacementKey` (OOM nullopt).
- `compiler/codegen/layout_pass.cc` — `IsConstMaterializable` map arm,
  `PostVisitMap`, `MaterializeMap`/`StampMapEntryStorage`, `ElementValue` map arm,
  `AggregateStorageVisitor` kStaticRodata guard for kMapExpr.
- `compiler/codegen/expr_lower.cc` — `EmitKMapExpr` kStaticRodata → i32.const arm.

Tests (working tree + staged):
- `compiler/codegen/static_memory_builder_test.cc`
- `compiler/codegen/layout_pass_test.cc`
- `compiler/codegen/expr_lower_test.cc`
- `compiler/internal/compile_test.cc`
- `e2e/m31_static_aggregate_test.cc`
- `e2e/limits_test.cc` (staged)
- `tools/wat_runner/wat_runner_test.cc`
- `compiler/codegen/BUILD.bazel` (staged: cel_map_hash dep), `e2e/limits_test.cc` BUILD untouched.

Docs / WAT: `wat/74_static_map_swisstable.wat`, `wat-traces.md`, milestone docs,
`testing-checklist.md`.

Test run (fastbuild) — all green:
- `//compiler/codegen:{static_memory_builder,layout_pass,expr_lower}_test` PASS
- `//compiler/internal:compile_test` PASS
- `//e2e:m31_static_aggregate_test_{static,dynamic}` PASS
- `//e2e:e2e_limits_{static,dynamic}` PASS
- `//tools/wat_runner:wat_runner_test` PASS

---

## Component-by-component

### 1. `StaticMemoryBuilder::MaterializeMap` (static_memory_builder.cc)

| Behavior | Covered? | Test (file:line) |
|---|---|---|
| empty map | YES | `MaterializeMapEmpty` (static_memory_builder_test.cc:428) |
| N=1 (no index) | YES | `MaterializeMapSingleIntEntryNoIndex` (:453) |
| N below threshold, no index, index_offset=0 | YES | `MaterializeMapBelowThresholdNoIndex` (:478) |
| N at threshold, index baked | YES | `MaterializeMapAtThresholdBakesIndex` (:562) |
| string key payload staging | YES | `MaterializeMapStringKeyPayloadAdjacency` (:588); keystone String_N* |
| int / uint / bool keys | YES | keystone matrix `KeystoneCases` (:874); Bool_N2 (:886) |
| nested aggregate value | YES | `MaterializeMapNestedListValue` (:608) |
| duplicate key → nullopt (below threshold) | YES | `MaterializeMapDuplicateKeyBelowThreshold` (:627) |
| duplicate key → nullopt (at threshold) | YES | `MaterializeMapDuplicateKeyAtThreshold` (:634) |
| cross-type numeric dup (int 1 == uint 1) → nullopt | YES | `MaterializeMapCrossTypeNumericDuplicate` (:649) |
| byte-identity keystone × key kind × num_slots {7,8,15,16,56,64} | YES | `StaticMemoryBuilderKeystoneMatrix.MaterializedIndexMatchesRuntime` (:869) |
| key-staging-OOM → nullopt fallback (the P1 fix) | PARTIAL — e2e only | `e2e/limits_test.cc:LimitsTest.KeyStagingArena_*` (3 cases); no builder-unit test |
| **bytes key** (`b"..."` key, span-staging path) | **NO** | — gap B1 |

### 2. Visitor gating (layout_pass.cc)

| Behavior | Covered? | Test |
|---|---|---|
| const map materializes → i32.const, no build calls | YES (codegen IR) | `ExprLowerMapTest.ConstMapLiteralLowersToI32ConstNoBuild` (expr_lower_test.cc:810) |
| nested const map → i32.const | YES | `ExprLowerMapTest.ConstMapNestedLowersToI32Const` (:830) |
| const map inside non-const list — inner still materializes | YES | `ExprLowerMapTest.ConstMapInsideNonConstListInnerStillMaterializes` (:847) |
| map with **ident value** keeps build path | YES | `ExprLowerMapTest.MapWithIdentValueKeepsBuildSequence` (:869) |
| dup-key map keeps build path (no materialize) | YES (unit, nullopt) | builder dup tests above; e2e relies on existing dup-key conformance |
| empty map | YES | builder `MaterializeMapEmpty`; e2e n/a |
| **const map node carries kStaticRodata** (layout-pass positive) | **NO** | — gap B2 (lists have `ConstListLiteralMaterializesToRodata`; maps have no equivalent) |
| **non-const map node carries kWorkspaceSlot** (layout-pass negative) | **NO** | — gap B3 (lists have `NonConstListLiteralGetsOneWorkspaceSlot`; maps only assert byte counts) |
| map with a **comprehension value** keeps build path | **NO** | — gap B4 |
| map with a **call value** (`{1: 1+1}`) keeps build path | **NO** | — gap B5 |

### 3. e2e across BOTH link modes (m31_static_aggregate_test.cc)

Both `_static` and `_dynamic` instantiated via `link_mode_e2e_cc_test` (BUILD
e2e/BUILD.bazel:116). Covered: value kinds int/uint/double/bool/string/bytes/null
(:431–:484), key kinds int/uint/bool/string (:487–:514), size/in-hit/in-miss/eq
(:523–:549), nested + list-of-maps (:557, :567), large map `size()` N=100 (:573).

| Behavior | Covered? | Note |
|---|---|---|
| small const map lookup (int + string key) | YES | KeyInt/KeyString etc. |
| `in` hit / miss | YES | InKeyPresent / InKeyAbsent (N=2, below threshold) |
| equality | YES | EqualityOrderIndependent |
| large const map (N>=threshold) — **lookup hit through baked index** | **NO** | — gap B6 (N=100 only calls `size()`; no `mapN[k]` hit + miss e2e) |
| large-string-key OOM fallback (limits) | YES | `e2e/limits_test.cc` 3 cases (compiles, hit, miss) — both link modes |
| **bytes key** e2e | **NO** | — gap B1 (bytes only tested as VALUE, never as KEY) |

### 4. Codegen golden (expr_lower_test.cc)

Covered: const map → bare i32.const, no `cel_map_create`/`cel_map_insert`/
`cel_map_index_build` (`ConstMapLiteralLowersToI32ConstNoBuild` :810). Non-const
map keeps create+insert+index_build (`ScalarMapLiteralEmitsCreateAndInserts` :762,
`MapWithIdentValueKeepsBuildSequence` :869). List-comprehension does not emit
`cel_map_index_build` — verified indirectly by existing m32.A tests; the
map-producing-comprehension still emits the terminal index_build
(expr_lower_test.cc:1111). Adequate.

### 5. WAT trace

`wat/74_static_map_swisstable.wat` (9-entry int map, baked index) +
`WatRunnerMapTest.MaterializedMapWithBakedIndexResolvesLookup`
(wat_runner_test.cc:1447) — asserts m[5]==105 AND `index_offset != 0`. Good.

---

## GAP LIST (each = the missing test, the file, what it asserts)

**B1 — bytes-keyed const map is untested across the whole pipeline.**
Bytes is a valid CEL map-key type and routes through the same span-staging path
as string keys (`StagePlacementKey`, CEL_BYTES arm), but it is exercised
*nowhere*: the keystone enum is `{kInt, kUint, kString, kBool}` with no `kBytes`,
and the e2e suite tests bytes only as a *value* (`ValueBytes`), never as a key.
- Add `KeyKind::kBytes` to the keystone matrix in
  `compiler/codegen/static_memory_builder_test.cc` (extend `MakeRuntimeKey`,
  `MakeBuilderKey`, `KeyKindIsSpan`, and `KeystoneCases` over {7,8,15,16,56,64}),
  asserting the baked index + run are byte-identical to the runtime build for
  bytes keys (the load-bearing span-key parity check).
- Add `ConstMapMaterializationTest.KeyBytes` to
  `e2e/m31_static_aggregate_test.cc`: `R"({b"\x07": 1}[b"\x07"])"` evals to 1.

**B2 — no layout-pass positive test that a const map carries kStaticRodata.**
The map gating in `layout_pass.cc` (`PostVisitMap` / `IsConstMaterializable` map
arm) is verified only indirectly through codegen IR. Lists have
`LayoutPassListTest.ConstListLiteralMaterializesToRodata`; maps have no
equivalent.
- Add `LayoutPassMapTest.ConstMapLiteralMaterializesToRodata` to
  `compiler/codegen/layout_pass_test.cc`: parse+check `{1: 10, 2: 20}`, run
  ResolvePass+LayoutPass, assert the root kMapExpr node's
  `storage.kind == StorageKind::kStaticRodata` and `workspace_bytes == 0`.

**B3 — no layout-pass negative test that a non-const map keeps a workspace slot.**
Lists have `NonConstListLiteralGetsOneWorkspaceSlot`; the map equivalent only
asserts byte counts, not the storage kind on the map node directly.
- Add `LayoutPassMapTest.NonConstMapLiteralGetsOneWorkspaceSlot` to
  `compiler/codegen/layout_pass_test.cc`: `{1: v}` with `v:int`, assert the
  kMapExpr node's `storage.kind == StorageKind::kWorkspaceSlot`.

**B4 — no negative gating test for a map with a comprehension value.**
Only an *ident* value (`{1: a}`) is tested as the disqualifier; a comprehension
value is a distinct AST shape `IsConstMaterializable` must reject.
- Add `ExprLowerMapTest.MapWithComprehensionValueKeepsBuildSequence` to
  `compiler/codegen/expr_lower_test.cc`: `{1: [1,2,3].map(x, x)}` (or a const
  list source comprehension) must emit `cel_map_create` + `cel_map_index_build`
  (build path, not i32.const).

**B5 — no negative gating test for a map with a call value.**
`{1: 1 + 1}` is syntactically a call value; `IsConstMaterializable` rejects calls
(no compile-time eval), so it must keep the build path. Currently untested.
- Add `ExprLowerMapTest.MapWithCallValueKeepsBuildSequence` to
  `compiler/codegen/expr_lower_test.cc`: `{1: 1 + 1}` must emit `cel_map_create`
  (not a bare i32.const).

**B6 — no e2e lookup hit/miss through the baked index (N >= threshold).**
The large const map (N=100) only calls `size()`. The baked-index *lookup* path
on a materialized map is pinned at the builder level (keystone) and the WAT level
(N=9), but never end-to-end through both link modes — the headline correctness
claim ("a materialized index-baked map resolves lookups identically to a
runtime-built one") has no e2e proof for N>=8.
- Add `ConstMapMaterializationTest.LargeConstMapLookupHit` and
  `...LookupMiss` to `e2e/m31_static_aggregate_test.cc`: with
  `IntMapLiteral(100)`, assert `mapN[42] == 42` (hit, resolves via baked index)
  and `9999 in mapN == false` / a missing-key lookup errors (miss). Runs in both
  link modes automatically.

---

## What is solid (verified)

- Byte-identity keystone (int/uint/string/bool × {7,8,15,16,56,64}) — the m32.B
  gate — is thorough and passes (static_memory_builder_test.cc:869).
- The P1 key-staging-OOM fallback is pinned e2e in both link modes
  (limits_test.cc, 3 cases: compiles / hit / miss) — the abort regression cannot
  recur silently.
- Empty / N=1 / below-threshold / at-threshold index geometry, dup-key (both
  branches) + cross-type numeric dup → nullopt are all covered.
- Const-map → i32.const golden (no create/insert/index_build), nested, and
  const-inside-non-const are pinned at the codegen-IR level.
- The WAT trace exists, runs through wat_runner, and asserts index_offset != 0.
- Both `_static` and `_dynamic` e2e variants are wired and green.
