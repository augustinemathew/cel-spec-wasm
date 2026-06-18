# m33 — cel_host.cc cleanup + split plan

Status: plan — drafted 2026-06-16, not yet started.

`eval/internal/cel_host.cc` is the per-Instance host-callback layer (Layer 2
of the three-layer split documented in `cel-host-surface.md` §3): the
`cel_host.*` trampolines the wasm guest imports, plus the Layer-1 backing
concretes (`ProtoBacking`, `HostMap`, `ProtoMap`, `HostList`, `ProtoList`)
and the `Value::*` aggregate factories. At **4801 LoC** it is the largest
single TU in `eval/` and bundles ~8 distinct concerns into one
`cc_library` (`//eval:cel_host`).

This doc is **analysis only**. It maps the file by concern, inventories the
dead / test-only-reachable code with evidence, and proposes a concrete file
split. No code changes are part of this doc.

> All symbol references are by NAME, not line number — the file is under
> active edit and line numbers drift. Where a region is cited it is by its
> anonymous-namespace block + the function names that bound it.

---

## 1. Section map

Concerns, top to bottom, with the functions in each and a rough LoC span.
"anon" = lives in a file-anonymous `namespace {}`.

### S1 — WKT / Any / JSON / wrapper / time **decode** (unpack, read-side)
anon block #1 (`UnpackWrapperMessage` … `ProtoHasFieldResolved`),
~820 LoC.

| Symbol | Purpose |
|---|---|
| `UnpackWrapperMessage`, `UnpackWrapperValueField`, `IsWrapperFqn` | peel `google.protobuf.*Value` wrappers → scalar Value |
| `UnpackWellKnownTimeMessage`, `UnpackTimeMessageFields` | Timestamp/Duration → Value |
| `UnpackJsonValueMessage` | `google.protobuf.Value`/`Struct`/`ListValue` → Value |
| `MaybeUnpackWktMessage` | dispatch peeler over the above |
| `ExtractAnyFqn`, `UnpackOneAnyLayer`, `UnpackOneAnyResult`, `UnpackAnyToValueAnonImpl` | `google.protobuf.Any` chain unpack |
| `UnpackAnyToValue` (non-anon, re-exported) | public wrapper over the anon Any unpacker; consumed by `instance.cc` |
| `ReadNumericField` | numeric scalar reflection read |
| `ClassifyResolvedField` | `FieldDescriptor` → `ProtoFieldReadClass` + sub-fields |
| `ReadWrapperMessageArm`, `ReadAnyMessageArm`, `ReadTimeMessageArm` | per-class decode arms |
| `ReadClassifiedMessageField` | switch over `ProtoFieldReadClass` message arms |
| `ReadSingularMessageField` | classify-on-the-fly adapter (no per-site cache) |
| `ReadScalarField` | singular scalar/string/message read → Value |
| `ResolveFieldDescriptor` | number-first, name-fallback, extension-fallback resolve |
| `ReadFieldClassified` | full classified dispatch (map/list/scalar/message) |
| `ResolveFieldThroughSiteCache` | per-access-site `ResolvedFieldCache` resolve+classify |
| `ProtoHasFieldResolved` | shared `has()` tail |

### S2 — Layer-1 backing concretes (read side)
`OwnedProtoBacking` ctor + `ReadField`/`HasField`; `ProtoBacking::ReadField`
/`HasField`; `HostMap` (`MapKeysEqual`, `IsValidMapKeyKind`, `Get`,
`ContainsKey`, `ForEach`); `ProtoMap` (`Get`/`ContainsKey`/`ForEach`);
`HostList` (`At`/`ForEach`); `ProtoList` (`ReadRepeatedElement`, `At`,
`ForEach`). ~430 LoC, interleaved with S3.

### S3 — Value↔CelValue **encode** (wire marshalling)
anon block #2 (`DecodeKey` … `EncodeFieldResult`) + public `EncodeUnknownSet`
+ `EncodeValueToSlot`. ~200 LoC.

| Symbol | Purpose |
|---|---|
| `DecodeKey` | wire CelValue → map-key Value |
| `EncodeSpan`, `EncodeDurationValue`, `EncodeTimestampValue` | scalar/temporal encode |
| `EncodeValue` | Value → CelValue (scalars/null/error/temporal) |
| `EncodeAggregateIfAny`, `EncodeFieldResult` | aggregate intern + unified marshal |
| `EncodeUnknownSet` (public) | mint UnknownSet descriptor |
| `EncodeValueToSlot` (public) | host-call return marshalling |

### S4 — Aggregate-index trampolines (list/map At / lookup / iter-open)
`CelListAtImpl`, `CelListIterOpenImpl`, `CelMapLookupImpl`,
`CelMapIterOpenImpl`. ~330 LoC.

### S5 — Field-read prelude + get/has trampolines
anon block (`ResolveFieldRef`, `ResolveAttribute`, `EffectiveSelectAttribute`,
`MatchesAnyUnknownPattern`, `FieldDispatchPrelude`, `RunFieldPrelude`,
`TryEncodeAggregateFieldFast`); `WriteScalarFieldToSlot`; `CelGetFieldImpl`,
`CelHasFieldImpl`. ~360 LoC. **This is the per-Eval hot path and the section
the owner just refactored.**

### S6 — Aggregate-op trampolines (size / in / eq / concat) + equality core
anon block (`HostScalarSpanEq`, `HostScalarSameKindEq`, `HostNumericCrossEq`,
`HostScalarValueEq`, `ReadArenaListElement`, `ReadArenaListCount`,
`EncodeBackingScalar`); `CelListSizeImpl`, `BackingValueEqualsQuery`,
`CelListInImpl`, `CelListEqImpl` (+ `ListEqElement` walk helpers:
`ListLength`, `ReadArenaListEqElement`, `ReadHostListEqElement`,
`ReadListEqElementAt`, `ListEqElementEquals`, `WalkListEq`),
`CelListConcatImpl`, `CelMapSizeImpl`, `CelMapInImpl`, map-eq helpers
(`ReadArenaMapHeader`, `MapEntryCount`, `SnapshotMapEntries`,
`NormalizedMapEntryMatches`, `NormalizedMapEq`), `CelMapEqImpl`. ~640 LoC.

### S7 — Message equality + is-zero + make-message
`PeelAnyForEq`; `ProtoMessageEqOutcome` + `CompareProtoMessages` (fwd-declared
in S6's first anon block, defined here); `CelMessageEqImpl`,
`CelMessageIsZeroImpl`, `CelMakeMessageImpl`. ~130 LoC.

### S8 — Proto field **set** (literal construction write side)
anon block (`ReadInt64`/`ReadUInt64`/`ReadDouble`, `CheckInt32Range`,
`CheckUint32Range`, `ReadSpanString`, `WriteMessageOrPack`,
`SetWrapperInnerValue`, `SetWrapperFieldFromScalar`, `SetScalarField`,
`ForEachArenaListElement`, `ForEachArenaMapEntry`, `NewWktMessage`,
`PackDurationOrTimestamp`, `PackStruct`, `PackListValue`,
`PackCelValueIntoJsonValue`, `AnyInnerFqn`, `IsAggregateInnerFqn`,
`PackAnyFromScalar`, `MaybePackWktMessage`, `MaybeSetWktMessageField`,
`AppendRepeatedFromCelValue`, `AppendRepeatedFromHostListValue`,
`SetRepeatedField`, `InsertArenaMapEntry`, `InsertHostMapEntry`,
`SetMapField`); `CelSetFieldImpl`; `CelResolveMessageTypeNameImpl`.
~1260 LoC. **Largest single concern by far.**

### S9 — Timestamp-with-timezone accessor + WKT-unwrap bridges
anon blocks (`ProjectCivilField`; `ResolveTimeZone`, `TzAccessorPrelude`);
`CelTimestampTzAccessorImpl`, `CelWktUnwrapWrapperImpl`,
`CelWktUnwrapTimeImpl`. ~250 LoC.

### S10 — `Value::*` aggregate factories
`Value::Message`, `Value::OwnedMessage`, `Value::Map`, `Value::HostMap`,
`Value::List`, `Value::HostList`. ~40 LoC. These live here (not in
`value.cc`) because they `make_shared` the backing concretes
(`ProtoBacking`/`HostMap`/`HostList`) defined in this TU.

---

## 2. Dead-code / test-only-reachable inventory

Reachability was checked with `grep -rn` across `eval/` + `compiler/` +
`shared/` and by tracing the production call graph from the two get/has
trampolines. The decisive structural fact:

> **In production, every `HostMessageBacking` is a `ProtoBacking` or
> `OwnedProtoBacking`, both of which return a non-null `message()`.** The
> only non-proto `HostMessageBacking` subclasses in the tree —
> `JsonLikeBacking`, `NonProtoBacking`, `FakeJsonBacking` — all live in
> `*_test.cc`. Evidence:
> ```
> $ grep -rn ": public HostMessageBacking\|: HostMessageBacking" --include=*.cc --include=*.h .
> eval/internal/cel_host_test.cc:297:   class JsonLikeBacking : public HostMessageBacking {
> eval/internal/cel_list_eq_impl_test.cc:303: class NonProtoBacking final : public HostMessageBacking {
> eval/internal/cel_host_wasmtime_test.cc:34: class FakeJsonBacking final : public HostMessageBacking {
> eval/internal/cel_host.h: ProtoBacking final / OwnedProtoBacking final
> ```

### Finding D1 — `ProtoBacking::ReadField` / `OwnedProtoBacking::ReadField` (the virtuals) are bypassed in production. **[Investigate / keep-for-test]**

`CelGetFieldImpl` (the refactored hot path) branches on
`prelude_or->backing->message()`:

- `message() != nullptr` (the ONLY production case for proto backings):
  resolves via `ResolveFieldThroughSiteCache`, then dispatches through
  `TryEncodeAggregateFieldFast` (kMap/kRepeated/kMessagePlain),
  `WriteScalarFieldToSlot` (kScalar), or `ReadClassifiedMessageField`
  (kMessageWrapper/Any/Timestamp/Duration/Json). It **never calls
  `backing->ReadField`**.
- `message() == nullptr`: falls through to
  `prelude_or->backing->ReadField(...)`. This is the only call site of the
  virtual in non-test code.

Since no production backing returns null `message()`, the virtual
`ReadField` path is reached **only by tests** (the three test backings
above) and by `OwnedProtoBacking::ReadField` → `inner_.ReadField`
delegation (itself test-only-reached). Evidence:

```
$ grep -rn "\->ReadField\|\.ReadField" eval/ | grep -v _test.cc
eval/internal/cel_host.cc:  return ReadFieldClassified(*msg_, *field, local);   # ProtoBacking::ReadField body
eval/internal/cel_host.cc:  return inner_.ReadField(...);                       # OwnedProtoBacking deleg
eval/internal/cel_host.cc:  auto v_or = prelude_or->backing->ReadField(...);    # CelGetFieldImpl, message()==null arm only
```

All other `->ReadField` invocations are in `*_test.cc`
(`cel_host_test.cc`, `proto_map_test.cc`, `proto_list_test.cc`).

**Disposition: KEEP, do not delete.** The virtual `ReadField` is a public
Layer-1 contract (`cel_host.h` `HostMessageBacking::ReadField` is pure
virtual; it is the documented embedder plug-in point for non-proto custom
backings — JSON, struct-of-structs). The production proto fast path is an
*optimization* layered over it, not a replacement of the contract. Deleting
it would break the documented embedder extension surface and the test
backings that exercise it. The right move is the **split** (§3): isolate
this test-only-in-practice path into its own TU so the hot-path TU shrinks,
and the cost of keeping the contract alive is visible. **Severity: P2** (no
correctness or footprint issue; it is dead-weight only relative to a
hypothetical "proto-only" build).

### Finding D2 — `ReadFieldClassified` reached only via the D1 virtual. **[Keep-for-test, tied to D1]**

`ReadFieldClassified` has exactly one call site: `ProtoBacking::ReadField`.
Because of D1, it is production-unreachable for proto backings.

```
$ grep -rn "ReadFieldClassified" eval/ --include=*.cc --include=*.h
cel_host.cc:  return ReadFieldClassified(*msg_, *field, local);   # only call
(plus comment references)
```

Its `kScalar` arm delegates to `ReadScalarField`, its `kMap`/`kRepeated`
arms construct `ProtoMap`/`ProtoList`, and its message arms call
`ReadClassifiedMessageField`. **All of those callees are independently
production-live** (see D3) — so `ReadFieldClassified` is a thin
test-only-reached *dispatcher* over live helpers, not a block of dead logic.
Disposition: **keep, move with D1** (it is the body of the kept virtual).
**Severity: P2.**

### Finding D3 — the scalar/message read helpers are NOT dead (correction to a tempting over-read). **[Keep — production-live]**

It is tempting to conclude the whole `ReadScalarField` /
`ReadClassifiedMessageField` family died with the `WriteScalarFieldToSlot`
fast path. It did not — they are reached through a *different* production
door: **proto map/list element reads**.

- `ReadScalarField` is called by `ProtoMap::Get`/`ProtoMap::ForEach`
  (map value reads) and indirectly by `ReadRepeatedElement` →
  `ProtoList::At`/`ForEach`. Those are reached in production whenever a
  proto `map<>`/`repeated` field is indexed/iterated:
  ```
  $ grep -n "backing->Get(\|backing->At(" eval/internal/cel_host.cc
  CelMapLookupImpl: backing->Get(...)
  CelListAtImpl / CelListIterOpenImpl: backing->At(...)
  ```
- `ReadClassifiedMessageField` is called by `CelGetFieldImpl` (line in the
  `message()!=nullptr` fast path) for the kMessage* classes — fully
  production-live.
- `ReadSingularMessageField` is reached via `ReadScalarField`'s message arm
  (proto map/list elements that are themselves messages) — production-live.
- `ReadNumericField` is called only by `ReadScalarField` — production-live
  through the same door.

**Disposition: KEEP all of D3.** They are load-bearing for proto
aggregate-element reads. Their presence is the reason D1/D2 can't simply be
deleted: the virtual path and the fast path share these leaf helpers.

### Finding D4 — `CelListConcatImpl` mixed-origin arm is a documented scope stub, not dead. **[Keep]**

`cel_host.h` documents that cross-origin / both-host concat currently
POISON with `TYPE_MISMATCH` (full materialisation is M5 follow-up). This is
a *reachable* poison arm with a test pin, not dead code — leave as-is.
**Severity: n/a.**

### Finding D5 — two redundant forward declarations flagged with NOLINT. **[P2, resolved by the split]**

Two `// NOLINTNEXTLINE(readability-redundant-declaration)` forward decls
exist (one near the top anon block for the WKT peelers, one in S8 near
`PackCelValueIntoJsonValue`). They exist because callers precede definitions
within the single TU. The split (§3) puts the definitions behind an internal
header, so these in-TU forward decls + their NOLINT suppressions can be
deleted. **Severity: P2.** Lines saved: ~4.

### Finding D6 — `CompareProtoMessages` forward-declared in S6, defined ~410 lines later in S7. **[P2, resolved by the split]**

The `ProtoMessageEqOutcome` enum + `CompareProtoMessages` decl sit in S6's
list-eq anon block; the definition is in S7. This split-by-distance is only
necessary because list-eq (S6) and message-eq (S7) share the comparator
inside one TU. Moving both into a shared equality TU (§3, `cel_host_eq`)
co-locates decl + definition and removes the long-range forward decl.
**Severity: P2.**

### Runtime `cel_*` kernels — NO dead kernels found. **[clean]**

Cross-checked all 218 `// cel:codegen-export`-marked kernels in `runtime/`
plus the 26 host-only symbols in `runtime/wasm_exports.txt` against
`compiler/codegen/` emitters (`overload_table.cc` + `BinaryenCall` sites)
and `eval/` references. Every exported symbol is reachable:

- 217 directly referenced by codegen (`overload_table.cc` / emitted calls);
- `cel_unknown_merge` is not emitted directly but is called internally by
  `cel_and`/`cel_or` (which codegen emits).
- Dispatcher tail-call targets (`cel_host_*`, `*_arena` variants,
  `cel_map_iter_*`) are all reached via their dispatchers.

The export set is authoritative (generated from the in-source markers,
validated by `//abi:runtime_catalogue_consistency_test`). **No runtime
cleanup is warranted.**

### Dead-code summary

| Finding | Symbol(s) | Disposition | Severity | LoC if removed |
|---|---|---|---|---|
| D1 | `ProtoBacking::ReadField`, `OwnedProtoBacking::ReadField` | Keep (public contract); isolate via split | P2 | 0 (kept) |
| D2 | `ReadFieldClassified` | Keep (body of D1) | P2 | 0 (kept) |
| D3 | `ReadScalarField`/`ReadClassifiedMessageField`/… | Keep (production-live via map/list elements) | — | 0 |
| D4 | `CelListConcatImpl` mixed arm | Keep (documented stub) | — | 0 |
| D5 | 2× redundant fwd-decl + NOLINT | Delete (after split) | P2 | ~4 |
| D6 | `CompareProtoMessages` fwd-decl | Co-locate (after split) | P2 | ~3 |
| runtime | — | None | — | 0 |

**Net deletable LoC is small (~7).** The genuine win here is **not deletion
— it is the split**: there is essentially no rotted dead code, but ~4800
LoC of live code spanning 8 concerns in one TU. The cleanup value is
structural (reviewability, the function-size/TU-granularity rules in
CLAUDE.md), not garbage-collection.

---

## 3. Proposed file split

Today: one `cc_library //eval:cel_host` (`internal/cel_host.cc` +
`internal/cel_host.h`), with `//eval:cel_host_hdrs` (header-only) and
`//eval:cel_host_error` already factored out. The split follows that
existing precedent (the M11 Slice E `cel_host_error` extraction is the
template) and the CLAUDE.md "one logical unit per TU, granular BUILD
targets, `internal/` convention" rule.

### Tight-coupling constraints (what resists splitting)

1. **`TrampolineContext` / `MemoryView` / `ExternrefTable` /
   `ArenaAllocator` / the backing classes / `ProtoFieldReadClass` /
   `ResolvedFieldCache` / `FieldRefEntry` etc.** already live in the
   *public* `cel_host.h` — so every proposed TU just `#include`s it. No new
   surface needed for those. Good: the split is mostly mechanical.
2. **The anon-namespace encode/decode helpers are shared across TUs.**
   `EncodeValue` / `EncodeFieldResult` / `EncodeSpan` / `EncodeBackingScalar`
   (S3, S6) are used by the field-read path, the aggregate-op path, AND the
   set-field path. `ReadScalarField` / `ReadClassifiedMessageField` /
   `ClassifyResolvedField` / `ResolveFieldThroughSiteCache` (S1) are used by
   both the get/has trampolines (S5) and the backing concretes (S2). These
   must move to a **shared internal header** (`cel_host_internal.h`) +
   their own TU so multiple TUs can call them without re-introducing the
   anon-namespace privacy. This is the main piece of design work.
3. **`UnpackAnyToValue` is already public** in `cel_host.h` (consumed by
   `instance.cc`); its anon impl + the WKT peelers move with S1.
4. **`Value::*` factories (S10)** must stay linked wherever
   `ProtoBacking`/`HostMap`/`HostList` ctors are defined — they
   `make_shared` those. Keep them with the backing concretes (S2 TU) or in
   a tiny `cel_host_value_factories.cc` that deps the S2 TU.
5. **No file-static mutable state.** The only "shared state" is
   `ResolvedFieldCache`, which lives per-`FieldRefEntry` (passed through
   `TrampolineContext.bindings`), not as a TU global — so splitting does not
   fracture any singleton. Confirmed: `grep -n "^static .*=" cel_host.cc`
   finds only `static` *functions*, no `static` data.

### Target TUs

All new TUs are `internal/`, `//:internal` visibility, deps on
`:cel_host_hdrs` + `:cel_host_error` + the new `:cel_host_internal`. The
public-surface contract in CLAUDE.md is unaffected: `cel_host.h` stays the
single public header; only the `.cc` is fragmented (and a new
*internal-only* header is added).

| New TU | Holds (sections) | Est. LoC | Notes |
|---|---|---|---|
| `cel_host_internal.{h,cc}` | shared helpers: S3 encode (`EncodeValue`/`EncodeSpan`/`EncodeFieldResult`/`EncodeAggregateIfAny`/`DecodeKey`/`EncodeDuration/TimestampValue`/`EncodeBackingScalar`), `EncodeUnknownSet`, `EncodeValueToSlot` | ~280 | The seam every other TU depends on. Promote the anon helpers to declared (internal-header) symbols. |
| `cel_host_proto_read.{h,cc}` | S1 (WKT/Any/JSON/wrapper/time decode, `Classify*`, `ReadScalarField`, `ReadClassifiedMessageField`, `ReadFieldClassified`, `ResolveFieldDescriptor`, `ResolveFieldThroughSiteCache`, `ProtoHasFieldResolved`, `UnpackAnyToValue`) | ~820 | The proto-reflection read core. Declares the few symbols S2/S5 call. |
| `cel_host_backings.cc` | S2 (`ProtoBacking`/`OwnedProtoBacking`/`HostMap`/`ProtoMap`/`HostList`/`ProtoList` members) + S10 (`Value::*` factories) | ~470 | Backing-concrete bodies + factories. The D1/D2 test-only-reached virtuals live here, isolated. |
| `cel_host_field_dispatch.cc` | S5 (`RunFieldPrelude` + attr/unknown helpers, `WriteScalarFieldToSlot`, `TryEncodeAggregateFieldFast`, `CelGetFieldImpl`, `CelHasFieldImpl`) | ~360 | **The per-Eval hot path** — the owner's active edit target. Smallest, most-churned TU; benefits most from isolation. |
| `cel_host_aggregates.cc` | S4 (`CelListAtImpl`/`CelListIterOpenImpl`/`CelMapLookupImpl`/`CelMapIterOpenImpl`) + S6 (size/in/eq/concat + list-eq + map-eq walks + scalar-eq matchers) | ~970 | The aggregate-op trampolines + their equality walks. Largest of the new TUs; could sub-split into `_aggregate_index` (S4) and `_aggregate_ops` (S6) if it trips the function-size budget. |
| `cel_host_eq.{h,cc}` | S7 (`PeelAnyForEq`, `ProtoMessageEqOutcome`, `CompareProtoMessages`, `CelMessageEqImpl`/`IsZeroImpl`/`MakeMessageImpl`) | ~130 | Resolves D6 (decl+def co-located). Declares `CompareProtoMessages` for the list-eq walk in `cel_host_aggregates.cc`. |
| `cel_host_set.cc` | S8 (all the proto-field-set / WKT-pack / repeated / map setters, `CelSetFieldImpl`, `CelResolveMessageTypeNameImpl`) | ~1260 | **Biggest single extraction.** Self-contained write side; touches encode helpers (S3) for span reads. |
| `cel_host_time.cc` | S9 (`ProjectCivilField`, `ResolveTimeZone`, `TzAccessorPrelude`, `CelTimestampTzAccessorImpl`, `CelWktUnwrapWrapperImpl`/`TimeImpl`) | ~250 | Timezone accessors + WKT-time bridges. |

After the split, **`cel_host.cc` either disappears** (all bodies rehomed) or
shrinks to a thin shim. The original `//eval:cel_host` `cc_library` becomes
an umbrella `cc_library` with no `srcs` (or a tiny one) that `deps` the new
TUs, so existing `:cel_host` consumers (the wasmtime glue, the tests) keep
their single dep edge unchanged — no churn in downstream BUILD files.

### BUILD / visibility implications

- Add `cel_host_internal` as `//:internal`, depended on by every new TU.
  It is NOT public (it is the anon-helper promotion); embedders never see it.
- New `_proto_read` / `_eq` headers are `//:internal` too (they declare the
  cross-TU seams). `cel_host.h` stays the only `//visibility:public`-eligible
  header (it currently is `//:internal`-scoped via `cel_host_hdrs`; that is
  unchanged).
- The umbrella `:cel_host` keeps its current name + visibility so
  `instance_impl`, `cel_host_wasmtime`, and the test targets that
  `deps = [":cel_host"]` need zero edits.
- Each new TU gets a paired `_test.cc` **only where one does not already
  cover it** — the existing suite already partitions cleanly along these
  lines (`proto_map_test.cc`, `proto_list_test.cc`, `cel_list_eq_impl_test.cc`,
  `cel_map_eq_impl_test.cc`, `cel_list_at_impl_test.cc`,
  `cel_map_lookup_impl_test.cc`, `cel_iter_open_impl_test.cc`,
  `host_list_test.cc`, `host_map_test.cc`, `cel_host_test.cc`). Re-point
  each test's `deps` from `:cel_host` to the umbrella (no change) or to the
  specific new TU (tighter). The set-field path and the time path currently
  ride on `cel_host_test.cc` + e2e; a dedicated `cel_host_set_test.cc` /
  `cel_host_time_test.cc` is the gap to fill if those concerns get their own
  TU (per "every source file gets its own `_test.cc`").

---

## 4. Do-first / do-later ordering

The split is the bulk of the work; sequence it lowest-risk-first so each
step is independently green.

1. **Do first — `cel_host_internal.{h,cc}` (the encode seam).** Promote the
   S3 anon helpers to an internal header + TU. Lowest risk (pure code move,
   no logic change), and it unblocks every other extraction. Verify
   `bazel test //eval:...` green.
2. **`cel_host_set.cc` (S8).** Biggest LoC win, fully self-contained write
   side, only depends on the S3 seam from step 1. Add `cel_host_set_test.cc`
   (gap fill).
3. **`cel_host_time.cc` (S9).** Small, self-contained. Add
   `cel_host_time_test.cc` (gap fill).
4. **`cel_host_eq.{h,cc}` (S7).** Resolves D6 (co-locate
   `CompareProtoMessages`). Must precede / accompany the aggregate split
   since the list-eq walk calls it.
5. **`cel_host_proto_read.{h,cc}` (S1).** The read core. Resolves D5 (one
   redundant fwd-decl). Carries the D1/D2/D3 helpers.
6. **`cel_host_backings.cc` (S2 + S10).** Backing-concrete bodies +
   factories; depends on S1 (read helpers) and S3 (encode). Isolates the
   D1/D2 test-only-reached virtuals.
7. **`cel_host_aggregates.cc` (S4 + S6).** Depends on S3, S7. Sub-split into
   index (S4) + ops (S6) if the function-size budget trips.
8. **Do last — `cel_host_field_dispatch.cc` (S5).** The hot path the owner
   is actively editing. Extract it LAST so the owner's in-flight edits land
   first and the move is a clean lift of the settled code. Resolves D5's
   second fwd-decl.

P2 cleanups (D5, D6) fall out of the split itself — they are not separate
commits. There are no P0/P1 findings: nothing ships-breaking, nothing
must-fix-before-next-milestone. The only "keep" caveats (D1/D2) are
contract-preservation, not bugs.

---

## 5. Footprint / impact note

- **Deletable LoC: ~7** (D5 + D6 forward-decls + NOLINTs). The file has no
  rotted dead code.
- **Restructured LoC: ~4800**, split into ~8 TUs of ~130–1260 LoC each,
  none exceeding the per-concern cohesion the existing test partition
  already implies.
- **Risk:** each extraction is a code-move with no logic change; the
  function-by-function reachability above confirms no behavior is being
  dropped. The umbrella-`:cel_host` shim keeps downstream BUILD edges
  stable, so blast radius is contained to `eval/BUILD.bazel` + the moved
  `.cc`s.
- **Coverage gap to close during the split:** `cel_host_set.cc` and
  `cel_host_time.cc` lack dedicated `_test.cc` files today (they ride on
  `cel_host_test.cc` + e2e); the split is the moment to add them per the
  per-file-test rule.

## Future work

- If a "proto-only" build configuration is ever wanted (no custom-backing
  embedders), the D1/D2 virtual `ReadField` path + `ReadFieldClassified`
  become genuinely deletable (~430 LoC incl. their share of S1). Out of
  scope here — they are live contract today.
- `cel_host_aggregates.cc` may need a further index/ops sub-split if the
  function-size gate trips; left as a measure-then-decide during step 7.
