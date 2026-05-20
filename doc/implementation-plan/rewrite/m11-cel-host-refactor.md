# M11 — `cel_host` refactor + WKT peel-to-runtime + test coverage

Status: **in flight — Slice A + Slice E shipped 2026-05-19; Slices B/C/D/F/G/H/I pending.**

Drafted 2026-05-18.  As-shipped delta vs. as-written plan:

  - **Slice A and Slice E were bundled into one landing.**  The
    plan called for Slice A as a separate half-day landing before
    Slice E.  In practice the Any-of-Any P0 fix and the
    `cel_host_error` extraction were natural co-travellers — both
    touch `cel_host.cc`, and bundling avoided two rebases on the
    same file.  All Slice A tests (`AnyOfAnyTest::*`,
    `AnyOfWrapperKindsTest::*`, `AnyOfWktTimeTest::*`) live in
    `cel_host_test.cc`; all Slice E TU + tests live in
    `cel_host_error.{cc,h}` + `cel_host_error_test.cc`.
  - **A new `:cel_host_hdrs` target was introduced** (headers-only
    `cc_library` wrapping `cel_host.h`).  Not in the original
    plan; emerged during Slice E to break the dep cycle that
    would otherwise force every extracted helper TU to depend on
    `:cel_host` (the impl).  Future cel_host_* TUs (Slices F-I)
    will all depend on `:cel_host_hdrs` instead, and `:cel_host`
    itself becomes a thin top-level dep that fans out to the
    helper TUs + the residual impl.
  - **`cel_host.cc` LOC: 3,246 → 3,167** (−79 net after Slice A
    adding the iterative-loop helpers and Slice E sheding the
    error helpers).  Final target per the plan is ~80 LOC after
    Slices F-I land.

Scope is the simplification, bug-fixing, runtime-relocation of
WKT peel logic, and test-coverage debt that accumulated in
`compiler_v2/api/internal/cel_host.cc` across milestones M3 →
M10.  Conformance ceiling: small (~+3 PASS from Any-of-Any).
Headline gains are architectural:

  1. Monolithic 3,246-LOC TU becomes 6-8 cohesive TUs each
     with a dedicated unit test file.
  2. **WKT peel logic (Any iteration + wrapper unwrap +
     timestamp/duration unwrap) moves from host C++ trampolines
     into a new runtime TU `compiler_v2/runtime/cel_wkt_wire.{c,h}`**
     (hand-rolled wire-format decoder, ~300 LOC).  Eliminates the
     per-layer host roundtrip for Any-of-Any; eliminates 4 of the
     existing host-side WKT trampolines.
  3. Host-side wrapper-field handling (the reflection-based
     read/write on live user-schema `Message*` objects) collapses
     from 5× copy-pasted 9-arm `cpp_type` ladders into one
     table-driven `kWrapperOps` dispatch.

## 1. Why M11

`cel_host.cc` accreted across 8 milestones without an
intermediate rewrite.  An audit on 2026-05-18 (full report in
chat-history) surfaced:

  - **1 P0 correctness bug** — Any-of-Any unwrap silently
    returns a `CEL_MESSAGE(google.protobuf.Any)` instead of
    iterating to the inner scalar.  M7-A's design doc
    (`m7a-any.md` §R3) explicitly called for either an iterative
    unwrap or a depth-bounded `ABSL_CHECK`; neither was
    implemented.  A fixture
    `Foo{single_any: Any{Any{Int32Value{value: 7}}}}` should
    round-trip as `int 7` per langdef §"Dynamic Values" +
    cel-cpp's `AdaptAny` (`well_known_types.cc:1943-2007`);
    currently surfaces as `CEL_MESSAGE`.
  - **WKT peel lives at the wrong layer.**  Today every
    Any-unpack, wrapper-unwrap, and timestamp-unwrap step
    crosses the wasm→host boundary, even though the wire format
    of the 9 wrapper types + Timestamp + Duration + Any is
    spec-fixed and small enough to decode inside `cel_runtime.wasm`
    in ~300 LOC of hand-rolled wire-format code.  Any-of-Any of
    depth N today costs N host roundtrips; with WKT peel in
    runtime, it's one runtime tight-loop call.  cel-cpp's own
    structure mirrors this — `internal/proto_wire.h` is its
    wire-format decoder; the `XReflection` classes are a separate
    layer for live `Message*` callers.
  - **5× duplicated 9-arm `cpp_type` ladder** — `UnpackWrapperMessage`
    (L222-260), `SetWrapperInnerValue` (L2004-2059),
    `ReadNumericField` (L266-292), `SetScalarField` (L2104-2261),
    `AppendRepeatedFromCelValue` (L2309-2420).  Every wrapper-type
    addition touches all five.  After the runtime push, sites
    (1) and (2) move to runtime as table-driven; sites (3)-(5)
    stay on host (they're reflection-based for user-schema
    fields) but unify against one `kWrapperOps` table.
  - **6 function-size violations** — `SetScalarField` (158),
    `InsertHostMapEntry` (136), `AppendRepeatedFromCelValue`
    (112), `AppendRepeatedFromHostListValue` (83),
    `CelSetFieldImpl` (76), `EncodeValue` (63).  Tracked in
    `lint-backlog.md:85-96`; this is the slice that fixes them.
  - **Cross-origin aggregate ops silently return `TYPE_MISMATCH`** —
    `CelListConcatImpl` (L1617-1657) and `CelMapEqImpl`
    (L1755-1758) return a spec-shaped error to the caller for
    mixed host/arena operands instead of the
    `ABSL_CHECK(false) << "... is a stub until M12"` the
    CLAUDE.md rule mandates.
  - **Strict URL prefix not enforced** in `UnpackAnyToValue`.
    cel-cpp requires `type.googleapis.com/` or
    `type.googleprod.com/`; celwasm accepts anything with `/`.
    Quiet divergence.
  - **No direct unit tests for any internal helper.**  100+
    internal functions in `cel_host.cc` exercised only via the
    Compile → Plan → Eval e2e suite.  Per-component-test-coverage
    debt.

## 2. Scope

In scope:

  1. **Fix the Any-of-Any P0 immediately, in host** — Slice A,
     before the runtime move.  Iterative unwrap +
     `ABSL_CHECK(depth < 1024)` + strict URL prefix.  Stops the
     bleeding before the larger architectural work lands.  Code
     deletes again in Slice C (when the runtime version takes
     over) but the cost is ~30 lines temporary; it's worth it
     for the immediate correctness fix.
  2. **Hand-roll `compiler_v2/runtime/cel_wkt_wire.{c,h}`** —
     wire-format decoder for the 9 wrapper types + Timestamp +
     Duration + Any.  Pure C, no protobuf-lite dep.  ~300 LOC.
     Exports `cel_any_peel`, `cel_wkt_peel_wrapper`,
     `cel_wkt_peel_time`, and the lower-level wire-format helpers.
  3. **Codegen emits runtime calls for WKT peel** — `kStructExpr`
     for a wrapper FQN literal lowers to `(call $cel_wkt_peel_wrapper ...)`;
     Any-unpack inside `kSelect` / `kCall` lowers to
     `(call $cel_any_peel ...)`.  Host trampolines for these
     paths delete.
  4. **Host trampoline cleanup** — `cel_wkt_unwrap_wrapper`,
     `cel_wkt_unwrap_time`, `cel_any_peel_for_eq` (the WKT-peel
     trampolines from M7-A.C, M7-B, M8.B/C) all delete.
     User-schema fallback (`cel_any_peel_user_schema` — only
     called when the Any payload's FQN is NOT a WKT) is the one
     new trampoline that replaces them, for the descriptor-
     reflection cases the runtime can't do itself.
  5. **Split `cel_host.cc` into 6-7 cohesive TUs** (final shape
     in §4 below).  Each TU gets a dedicated `_test.cc`.
  6. **Unify host-side wrapper-field handling** — pull the 9-arm
     `cpp_type` ladder into one `CelWrapperKind` enum +
     `kWrapperOps[9]` dispatch table on the host (for reflection-
     based user-schema field access; this stays on host because
     it inherently needs descriptors).
  7. **Convert silent `TYPE_MISMATCH`-on-mixed-origin to
     `ABSL_CHECK(false)`** per CLAUDE.md "no silent fallbacks."
  8. **Split the 6 oversized functions** below the 60-line lint
     gate.
  9. **Direct unit-test coverage** for every internal helper
     that ships as a public symbol of one of the new TUs.
     Includes a new test file for the runtime `cel_wkt_wire`
     that drives it via wasmtime against synthetic wire-bytes.

Out of scope (deliberately):

  - **`Struct` / `Value` / `ListValue` WKT decoding.**  Three of
    the 14 spec-defined WKTs.  Their wire format is more complex
    (`Value` is a discriminated union over null/number/string/
    bool/struct/list).  No corpus rows currently exercise these
    in a way M11's work would unlock — punted to M12 if a
    feature surfaces them.
  - **Vendoring protobuf-lite** — rejected (chat-history
    2026-05-18).  Hand-rolling 300 LOC for the 11 simple WKTs is
    a fraction of the cost of importing ~200KB of protobuf-lite
    into the wasm runtime.
  - **Removing the static-subset restriction** — separate
    architectural milestone.  M11 stays inside `RejectDyn`.
  - **User-schema reflection moving to runtime** — would require
    shipping descriptor tables into wasm per Instance.  Not
    happening; the host descriptor pool stays load-bearing for
    user-schema messages.
  - **Performance optimisation beyond what naturally falls out** —
    no new caches, no new fast paths.  (Anomalously, removing
    host roundtrips for Any-of-Any *is* a perf improvement, but
    it's a side effect of correctness work, not the goal.)
  - **New language features.**

## 3. Why both refactor + runtime push in one milestone

Doing them sequentially (M11 = host refactor; M12 = runtime
push) is tempting but wrong:

  - **The host refactor would then immediately delete code in
    M12.**  Slice C (extract `cel_wkt.{cc,h}` as a host-side TU)
    in the original plan would land 300+ LOC of host-side WKT
    handling that M12 would rip out.  Wasteful.
  - **The Any-of-Any fix should live in the runtime, not
    duplicated host-side.**  If M11 fixes it on host and M12
    moves to runtime, the runtime port has to reimplement the
    fix.  Doing it in runtime once is cleaner.
  - **The wrapper-handling test surface diverges if split.**
    M11 host-side tests would assert host behaviour; M12 would
    add runtime tests; mismatches between the two layers become
    silently possible.  Single milestone = one consistent
    contract tested at both layers.
  - **Codegen changes are one diff, not two.**  The same expr_lower
    edit that replaces `cel_host.cel_wkt_unwrap_wrapper` with
    `cel_wkt_peel_wrapper` is most of the codegen work; doing it
    once is half the diff churn of doing it twice.

The combined milestone is bigger (~7-10 days vs ~4 days for
each piece separately) but the net is less work and less risk.

## 4. Final file structure

### 4.1 New runtime file

| File | ~LOC | Contents |
|---|---|---|
| `compiler_v2/runtime/cel_wkt_wire.h` | ~80 | Public ABI: `cel_any_peel(out_slot, in_slot)`, `cel_wkt_peel_wrapper(out_slot, in_slot, wrapper_kind)`, `cel_wkt_peel_time(out_slot, in_slot, is_duration)`.  Wire-format constants (tag numbers, wire-type codes).  `CelWrapperKind` enum mirrors host-side enum (shared via `cel_data.h`). |
| `compiler_v2/runtime/cel_wkt_wire.c` | ~300 | Wire-format decoder for: varint, length-delimited, fixed64, fixed32.  Per-WKT decode: 9 wrapper types (each is `message X { T value = 1; }` — trivial), Timestamp/Duration (`int64 seconds = 1; int32 nanos = 2;`), Any (`string type_url = 1; bytes value = 2;` — recurses for Any-of-Any).  Strict URL prefix check (`type.googleapis.com/` or `type.googleprod.com/`).  Depth `CHECK` at 1024 layers. |

### 4.2 New host files

`cel_host.cc` shrinks from 3,246 LOC to ~80 LOC (just the
`cel::Value::Message/OwnedMessage/Map/HostMap/List/HostList`
factory definitions that need backing-type visibility).
`cel_host.h` stays.

| # | File | ~LOC | Contents |
|---|---|---|---|
| 1 | `cel_host_error.{cc,h}` | 120 | Wire-error helpers (`WireErrorCode`, `WriteWireError`, `WriteWireBool`, `WriteWireInt`), `PoisonCelValue`, `FieldNotFound`, `MakeError`, `KeyTypeMismatch`, `NoSuchKey`, `IndexOutOfBounds`, 3VL absorbers.  No deps. |
| 2 | `cel_host_codec.{cc,h}` | 200 | `DecodeKey`, `EncodeSpan`, `EncodeValue`, `EncodeAggregateIfAny`, `EncodeFieldResult`, `EncodeBackingScalar`.  Depends on `cel_host_error`. |
| 3 | `cel_host_wrappers.{cc,h}` | 200 | `CelWrapperKind` enum + `kWrapperOps[9]` dispatch table for **host-side** wrapper-field reflection (user-schema messages with wrapper-typed fields).  `WrapperKindFromDescriptor` via `Descriptor::well_known_type()`.  This is the host-side counterpart to `cel_wkt_wire.c` — same enum, different code path because it operates on live `Message*` not wire bytes.  Used by `cel_host_proto_read` and `cel_host_proto_write`. |
| 4 | `cel_host_proto_read.{cc,h}` | 250 | `ReadNumericField`, `ReadSingularMessageField`, `ReadScalarField` (uses `kWrapperOps`), `ResolveFieldDescriptor`, `ReadRepeatedElement`, `ProtoBacking`, `OwnedProtoBacking`, `ProtoMap`, `ProtoList`.  For Any fields, returns the Any handle and lets the runtime peel via `cel_any_peel`. |
| 5 | `cel_host_proto_write.{cc,h}` | 600 | `SetScalarField` (table-driven), `SetMapField`, `SetRepeatedField`, `AppendRepeatedFromCelValue` (table-driven via `kWrapperOps`), `InsertHostMapEntry`, all the helpers.  Function-size violations resolved. |
| 6 | `cel_host_equality.{cc,h}` | 250 | `MapKeysEqual`, `IsValidMapKeyKind`, `HostScalarSpanEq`, `HostScalarSameKindEq`, `HostNumericCrossEq`, `HostScalarValueEq`, `WalkListEq`, `WalkMapEq`. |
| 7 | `cel_host_time.{cc,h}` | 150 | `TzAccessorKind`, `ProjectCivilField`, `ResolveTimeZone`, `TzAccessorPrelude`, `CelTimestampTzAccessorImpl`.  (Timestamp/Duration *peel* moves to runtime; this TU keeps the TZ-arithmetic that the runtime doesn't do.) |
| 8 | `cel_host_trampolines.{cc,h}` | 400 | `RunFieldPrelude`, `ResolveFieldRef`, `ResolveAttribute`, `EffectiveSelectAttribute`, `MatchesAnyUnknownPattern`, all the `Cel*Impl` entry points EXCEPT the WKT-peel trampolines (which delete).  New: `CelAnyPeelUserSchemaImpl` (host fallback called by runtime `cel_any_peel` when the inner FQN is not a WKT). |

(`cel_host_wkt.cc` from the original plan is **deleted** —
WKT peel logic moves to the runtime; the residual host-side
wrapper-field reflection becomes `cel_host_wrappers.cc`.)

### 4.3 Codegen change summary

`compiler_v2/codegen/expr_lower.cc` — three call-site edits:

  1. `LowerSelectField` on an Any-typed field: emit
     `(call $cel_any_peel ...)` instead of host trampoline.
  2. `LowerStructExpr` for a wrapper-FQN literal: emit
     `(call $cel_wkt_peel_wrapper ...)` instead of host
     trampoline.
  3. `LowerCallExpr` for `==` against a wrapper or Any operand:
     codegen-time peel via the runtime helpers.

The host trampolines `cel_wkt_unwrap_wrapper`,
`cel_wkt_unwrap_time`, and (the part of) `cel_message_eq` that
peeled Any are now unused — delete in Slice D.

### 4.4 Dependency graph

```
                  ┌─── cel_wkt_wire (runtime) ──────┐
                  │                                  │
[compile-time]    │                                  │ [runtime calls]
expr_lower ───────┤                                  │
                  │      ┌──── cel_host_error ───┐  │
[Cel*Impl]        │      │                        │  │
cel_host_trampolines ────┼──── cel_host_codec ────┤  │
                  │      │                        │  │
                  │      ├──── cel_host_wrappers ─┤  │
                  │      │                        │  │
                  │      ├──── cel_host_proto_read├──┤
                  │      │                        │  │
                  │      ├──── cel_host_proto_write┤  │
                  │      │                        │  │
                  │      ├──── cel_host_equality ─┤  │
                  │      │                        │  │
                  │      └──── cel_host_time ─────┘  │
                  │                                  │
                  └─── (host fallback) ──────────────┘
                       CelAnyPeelUserSchemaImpl
```

### 4.5 Wrapper-type dispatch shape (host side)

```cpp
// cel_host_wrappers.h
enum class CelWrapperKind : uint8_t {
  kBool, kInt32, kInt64, kUint32, kUint64,
  kFloat, kDouble, kString, kBytes,
};

struct WrapperOps {
  absl::StatusOr<cel::Value> (*ReadFromMessage)(
      const google::protobuf::Message& m);
  absl::Status (*WriteToMessage)(
      google::protobuf::Message* m, const CelValue& v,
      absl::Span<const uint8_t> mem);
};
extern const WrapperOps kWrapperOps[9];

std::optional<CelWrapperKind> WrapperKindFromDescriptor(
    const google::protobuf::Descriptor& d);
```

Same enum + same kind numbering as runtime-side `CelWrapperKind`
in `cel_wkt_wire.h` (defined in shared `cel_data.h`).

## 5. Test coverage strategy

The central deliverable.  Today the only `cel_host`-adjacent
tests are `cel_host_test.cc` (~6 cases on `OwnedProtoBacking`
lifecycle).  Internal helpers have zero direct coverage.  M11
adds ~250 tests across 9 new test files (8 host + 1 runtime).

### 5.1 Per-TU test matrix

| TU | Test file | Core scenarios |
|---|---|---|
| `cel_wkt_wire` (runtime) | `cel_wkt_wire_test.cc` (drives runtime via wasmtime) | **Highest-priority test file.**  Per-wrapper-kind peel from raw wire bytes (9 kinds).  Any peel: depth-1 (`Any<Int32Value>`), depth-2 (`Any<Any<Int32Value>>`), depth-3, depth-1023 (passes), depth-1025 (`ABSL_CHECK` aborts via `ASSERT_DEATH`).  Mixed-WKT-in-Any: `Any<Timestamp>`, `Any<Duration>`, `Any<StringValue>`, etc.  Non-WKT Any: payload type_url FQN unknown, runtime returns the "needs host" sentinel.  Negative: malformed wire bytes (truncated varint, missing field tag, wrong wire type), invalid URL (no slash, wrong prefix, empty FQN), depth-overflow.  Boundary: empty `StringValue.value`, `Int32Value{value: INT32_MIN}`, `Int32Value{value: INT32_MAX}`, NaN `DoubleValue.value`, very long `StringValue.value` (1MB). |
| `cel_host_error` | `cel_host_error_test.cc` | `WireErrorCode` round-trip per `cel::ErrorCode`; `WriteWireError` slot encoding; `WriteWireBool`/`WriteWireInt` slot encoding; `PoisonCelValue` produces kind=Poison; `AbsorbUnary`/`AbsorbBinary` propagate error/3VL per langdef §"3VL". |
| `cel_host_codec` | `cel_host_codec_test.cc` | `DecodeKey` per valid map-key kind (bool/int/uint/string); non-scalar key rejection; `EncodeValue` per scalar kind + roundtrip; `EncodeBackingScalar` for proto-backed scalar fields; `EncodeAggregateIfAny`.  Boundary: empty string, INT64_MIN/MAX, NaN, ±Inf, embedded-NUL strings. |
| `cel_host_wrappers` | `cel_host_wrappers_test.cc` | `WrapperKindFromDescriptor` for each of 9 wrapper FQNs + 1 non-wrapper rejection (via `well_known_type()`).  `kWrapperOps[kind].ReadFromMessage` round-trip per kind (live `Int32Value{value: 5}` → `cel::Value::Int(5)`).  `kWrapperOps[kind].WriteToMessage` round-trip per kind (scalar → wrapper-message).  Negative: kind/value mismatch (e.g. write a string to `kInt32`). |
| `cel_host_proto_read` | `cel_host_proto_read_test.cc` | `ReadNumericField` per `CppType`.  `ReadScalarField` for STRING/BYTES/BOOL/ENUM.  `ReadSingularMessageField` for: nested message (set + unset), wrapper field (set → peeled inner via `kWrapperOps`, unset → null per langdef line 484-486), Any field (returns Any handle, runtime peels).  `ProtoBacking::ReadField` end-to-end.  Negative: missing field number, field-number-vs-cpp-type mismatch. |
| `cel_host_proto_write` | `cel_host_proto_write_test.cc` | `SetScalarField` per `CppType` (positive: matching kind; negative: kind mismatch; auto-wrap: scalar → wrapper field via `kWrapperOps`; null → wrapper field clears).  `AppendRepeatedFromCelValue` per `CppType`.  `InsertHostMapEntry` per key-kind × per value-cpp_type matrix.  Boundary: empty list, INT32 overflow on UINT32 field. |
| `cel_host_equality` | `cel_host_equality_test.cc` | `MapKeysEqual` per valid key kind.  `HostScalarSameKindEq` per kind.  `HostNumericCrossEq` for cross-type combinations with boundary values.  `IsValidMapKeyKind` rejects non-key kinds.  `WalkListEq` ordered.  `WalkMapEq` order-agnostic. |
| `cel_host_time` | `cel_host_time_test.cc` | `ResolveTimeZone` for UTC, named, fixed-offset, invalid.  `ProjectCivilField` per `TzAccessorKind`.  Boundary: year 0001, 9999, DST transitions, nanosecond 999999999. |
| `cel_host_trampolines` | `cel_host_trampolines_test.cc` | `RunFieldPrelude`, `ResolveFieldRef`, `MatchesAnyUnknownPattern`.  Each `Cel*Impl` via a thin in-process fixture (no wasmtime) with synthetic `CelHostBindings`.  Includes `CelAnyPeelUserSchemaImpl` — the new host fallback called by runtime `cel_any_peel`. |

### 5.2 Test count target

~250 new tests across 9 files (median 27 per file).  Today's
total cel_host coverage: 6.  Expected runtime: <8s under
`bazel test //compiler_v2/runtime/... //compiler_v2/api/internal/...`.

### 5.3 Shared test fixtures

  - `BuildNestedAny(depth, inner_bytes)` — builds `Any<Any<...<inner>>>`
    at any depth as wire bytes.  Used in `cel_wkt_wire_test.cc`
    for the round-trip cases (depths 1, 2, 3, 1023) and the
    death test (depth 1025).
  - `BuildWrapperMessage(kind, value)` — builds wire-format
    `Int32Value{value: N}` etc. for any wrapper kind.  Shared
    across `cel_wkt_wire_test.cc` and `cel_host_wrappers_test.cc`
    (the latter uses the live-Message form, the former the
    wire-bytes form).
  - `MakeProtoMessage(fqn, fields)` — constructs a live `Message*`
    of any registered FQN with given field values.  Used in
    `cel_host_proto_read_test.cc` and `cel_host_proto_write_test.cc`.

Lives in `compiler_v2/api/internal/cel_host_test_helpers.{cc,h}`
(new file, exported as a `cc_library` with `testonly = True`).

## 6. Slicing

Ordered by dependency + risk.  The P0 fix is Slice A and ships
first, in the current host code; the runtime move follows in
Slice C.

### Slice A — Any-of-Any P0 fix in host (shipped 2026-05-19)

Landed bundled with Slice E (see header note).  Iterative unwrap
+ depth `ABSL_CHECK(depth < 1024)` + strict URL prefix inside
`cel_host.cc::UnpackAnyToValue`, before any of the larger file
splitting (Slices F-I).

What landed:

  - `ExtractAnyFqn(type_url)` — strict-prefix gate accepting
    only `type.googleapis.com/` and `type.googleprod.com/`.
    Non-matching URLs surface as a clean `kFieldNotFound`-shaped
    error instead of silently treating an attacker-supplied URL
    as a pool lookup key.
  - `UnpackOneAnyLayer(any, pool)` — peels exactly one Any layer
    and returns either a terminal `cel::Value` (for the empty-
    type_url / FQN-not-found / parse-failure cases) or the
    unwrapped inner `Message`.
  - `UnpackAnyToValue` — iteratively calls `UnpackOneAnyLayer`,
    looping while the inner type is itself `google.protobuf.Any`.
    Falls through to the wrapper/WKT-time peel on the first
    non-Any inner message.
  - Tests at `cel_host_test.cc::AnyOfAnyTest::*` + `AnyOfWrapperKindsTest::*`
    + `AnyOfWktTimeTest::*` — depth 1/2/3/4 round-trip across
    Int32Value; the 9 wrapper kinds each via Any<Wrapper>;
    Any<Timestamp> + Any<Duration>; malformed payload →
    kTypeMismatch; unknown FQN → kFieldNotFound; URL-without-slash
    + non-standard-prefix rejected; type.googleprod.com prefix
    accepted; empty type_url → null; depth-2 P0 regression
    (the canonical Any<Any<Int32Value>> shape that pre-M11
    surfaced as CEL_MESSAGE).

Conformance delta from Slice A: **0** rows.  Confirmed by
corpus inspection (`tests/simple/testdata/`): every Any-bearing
row in `comparisons.textproto` (30 mentions) and
`dynamic.textproto` (5 mentions) wraps `TestAllTypes` directly,
not another Any.  The depth-≥2 Any chains the P0 fix unblocks
exist for users who construct them in their own expressions —
the conformance suite doesn't.  The P0 correctness bug is
nonetheless sealed, and the kernel-level unit-test matrix
(`AnyOfAnyTest::Depth{1,2,3,4}*` + the malformed-payload /
strict-URL-prefix coverage) locks the iterative loop against
regression.

The code deletes again in Slice C when the runtime version takes
over, but the cost is ~80 lines temporary; worth it to seal the
correctness bug before the larger work.

### Slice B — Hand-roll `cel_wkt_wire.{c,h}` + tests (~2-3 days)

The largest single chunk of new code.  Wire-format decoder in
the runtime TU.  ~300 LOC of C + ~80 LOC of header.  Includes
the per-WKT decode for the 9 wrapper kinds + Timestamp +
Duration + Any.

`cel_wkt_wire_test.cc` ships with this slice — drives the
runtime via the existing wasmtime test harness pattern
(`cel_runtime_wasm_test.cc` shape, expanded).  Death tests for
depth overflow.

Critical care points:
  - Hand-rolled wire-format parsing is error-prone.  Mitigate
    with exhaustive fuzz-style tests against `protoc --encode`
    output for each WKT.
  - The depth-1024 `ABSL_CHECK` doesn't fire in wasm via
    `abort()` cleanly — verify the abort surfaces as a
    `wasm trap` that `cel_runtime_wasm_test` can `ASSERT_DEATH`
    against.  If not, swap to returning an error code at
    depth-overflow rather than crash.

### Slice C — Codegen swap to runtime calls (~1 day)

`expr_lower.cc` edits per §4.3.  Three call sites.  WAT trace
update per CLAUDE.md WAT-first rule (`doc/implementation-plan/rewrite/wat/`).
The host trampolines `cel_wkt_unwrap_wrapper`, `cel_wkt_unwrap_time`,
and the Any-peel-for-eq path become unused; mark them with
`ABSL_CHECK(false) << "deprecated by M11.D"` rather than
delete (so any stray caller crashes traceably).

E2E tests `m7a_test`, `m7b_test`, `m8_test` should pass
unchanged — they exercise the public surface, which is identical.

### Slice D — Delete deprecated host trampolines + tests confirm (~half-day)

Remove the dead trampoline bodies marked in Slice C.  Remove
their wasm import declarations from compile-time
`AddFunctionImport` calls.  Verify e2e tests still green.

### Slice E — Extract `cel_host_error` + tests (shipped 2026-05-19)

Landed bundled with Slice A (see header note).  `cel_host_codec`
deferred to a follow-up slice (the codec helpers are tangled
with the proto-read/write code that Slice G will move; cleaner
to extract codec + read + write together).

What landed:

  - **`cel_host_hdrs` cc_library** (new) — headers-only target
    wrapping `cel_host.h`.  Carries `MemoryView`, `ExternrefTable`,
    `ArenaAllocator`, the backing-type abstract interfaces
    (`HostMessageBacking` / `HostMapBacking` / `HostListBacking`),
    and the `CelHostBindings` struct.  Every future extracted
    TU depends on this for type visibility without pulling in
    `:cel_host` itself.  This was the dep-cycle-breaker the
    original plan didn't anticipate; pulling it out as a separate
    target costs almost nothing and unblocks Slices F-I.
  - **`cel_host_error.{cc,h}`** — wire-error helpers + 3VL
    absorbers.  Two distinct concerns:
      1. `cel::Value` error factories (`FieldNotFound`, `MakeError`,
         `KeyTypeMismatch`, `NoSuchKey`, `IndexOutOfBounds`).
      2. Wire-format slot writers (`WireErrorCode`, `WriteWireError`,
         `WriteWireBool`, `WriteWireInt`, `WriteInvalidArgumentError`,
         `PoisonCelValue`) + 3VL absorbers (`AbsorbUnary`,
         `AbsorbBinary`).
  - **`cel_host_error_test.cc`** — direct unit tests for every
    extracted helper (~17 tests).  Drives the slot writers
    through the existing `test::FakeMemoryView` from
    `cel_host_test_fakes.h`.  Per CLAUDE.md testing principles:
    positive + negative + boundary (INT64_MIN/MAX, empty strings,
    zero-count list); spec citation in comments where the
    propagation rule (first-non-normal-wins for 3VL) is
    spec-mandated.
  - **`cel_host.cc` cleanup** — extracted helpers replaced with
    breadcrumb comments noting their new home + slice (`// X
    moved to cel_host_error.cc (M11 Slice E).`).  All Layer-2
    trampoline call sites now resolve to the extracted symbols
    via include of `cel_host_error.h`.  Dead include of
    `absl/strings/match.h` swept (the strict-URL gate uses
    `absl/strings/strip.h::ConsumePrefix` instead).

Function-size lint warnings on `EncodeValue`,
`SetWrapperInnerValue`, `SetScalarField`,
`AppendRepeatedFromCelValue`, `AppendRepeatedFromHostListValue`,
`InsertArenaMapEntry`, `InsertHostMapEntry`, `CelSetFieldImpl`
remain — these are tracked in `lint-backlog.md:85-96` and
addressed by Slice G's table-driven dispatch refactor.

### Slice F — Extract `cel_host_wrappers` + tests (~1 day)

The host-side `kWrapperOps` table.  5 sites in remaining
`cel_host.cc` updated to call `kWrapperOps` instead of their
own 9-arm ladders.

### Slice G — Extract `cel_host_proto_read` + `cel_host_proto_write` + tests (~2 days)

Largest LOC move (~850).  Function-size fixes for the 5
oversized functions land here, each split along the
table-driven dispatch boundary.  E.1 = read, E.2 = write —
two reviewable diffs.

### Slice H — Extract `cel_host_equality` + `cel_host_time` + tests (~1 day)

Two independent TUs; can land in either order.

### Slice I — Consolidate `cel_host_trampolines` + closeout (~half-day)

All `Cel*Impl` entries land in their own TU including
`CelAnyPeelUserSchemaImpl`.  `cel_host.cc` shrinks to ~80
lines (factory methods only).

Closeout per CLAUDE.md ## Closing out a planning doc.

**Total estimate: 9-10 working days.**  At the user's
AI-assisted pace, ~3-4 calendar days of focused work.

## 7. Risks

  - **Slice B (cel_wkt_wire.c hand-rolled wire format) is
    error-prone.**  Wire-format edge cases: packed repeated
    encoding, unknown fields, varint > 64 bits.  Mitigation:
    exhaustive fuzz against `protoc --encode` reference output
    for each WKT, plus a comparison test that runs the same
    bytes through cel-cpp's `proto_wire.h` and asserts identical
    output.  ~30 fuzz/comparison test cases.
  - **The depth-1024 `ABSL_CHECK` may not abort cleanly inside
    wasm.**  wasi-sdk's libc routes `abort()` through `proc_exit`
    which the harness now stubs.  Verify the test harness can
    `ASSERT_DEATH` on a wasm trap from depth overflow; fall back
    to error-code return if not.
  - **Slice G (proto_read + proto_write) is large** (~850 LOC
    moving across two TUs with the wrapper refactor threaded
    through).  Mitigation: split into G.1 read + G.2 write as
    two independently reviewable diffs.
  - **Mixed-origin `ABSL_CHECK` conversion might break e2e
    paths.**  Today the silent `TYPE_MISMATCH` masks any callers.
    Mitigation: run `bazel test //compiler_v2/...` after the
    conversion; if previously-green tests now crash, that's a
    real bug being unmasked — investigate, don't paper over.
  - **Slice B + Slice C together change the wasm import surface.**
    Adding `cel_wkt_peel_*` exports + removing `cel_wkt_unwrap_*`
    imports means any external embedder of the runtime sees an
    ABI break.  Probably no external embedders yet (M11 is
    pre-1.0), but if there are, document the migration.
  - **`well_known_type()` adoption is upstream-version-dependent.**
    The `Descriptor::WellKnownType` enum was added in protobuf
    3.x.  Verify the vendored protobuf supports the enum values
    we need.

## 8. Open questions

  1. **Should `cel_wkt_wire.c` handle `Struct`/`Value`/`ListValue`?**
    Three more WKTs.  Their wire format is more complex (`Value`
    is a discriminated union).  No conformance rows currently
    require them.  Recommend deferring to M12 if a feature
    surfaces them.
  2. **`CelAnyPeelUserSchemaImpl` host trampoline shape.**  When
    the runtime's `cel_any_peel` finds a non-WKT FQN, it calls
    back to host.  ABI: pass type_url + value bytes as
    (offset, length) pairs?  Or pass a slot reference?  Decide
    in Slice B by trying both and picking the cleaner one.
  3. **Should the `kWrapperOps` table be `constexpr` or
    runtime-built?**  cel-cpp's `XReflection` classes cache
    descriptor pointers (lazy-init).  celwasm could use the
    `well_known_type()` enum for dispatch and avoid caching
    entirely (recompute each call — cheap because the descriptor
    is in the generated pool's hot cache).  Recommend the
    no-cache route for M11; revisit if profiling shows
    descriptor-lookup cost matters.
  4. **Shared test-helper file location.**  `cel_host_test_helpers.{cc,h}`
    lives in `compiler_v2/api/internal/` — but the runtime
    `cel_wkt_wire_test.cc` lives in `compiler_v2/runtime/` and
    also needs `BuildNestedAny`/`BuildWrapperMessage`.  Either
    (a) duplicate the helpers, (b) put them in a shared
    `compiler_v2/test_helpers/` package, or (c) put runtime-
    specific helpers in `compiler_v2/runtime/cel_wkt_wire_test_helpers.cc`.
    Recommend (b) — one shared helpers package.

## 9. Closeout gate (to copy into the PR description)

Per `compiler_v2/conformance/README.md` and CLAUDE.md closeout
discipline:

  - [ ] `bazel test //compiler_v2/...` green.
  - [ ] `bazel test //compiler_v2/runtime/cel_wkt_wire_test` green
        (~50 new tests including the death tests).
  - [ ] `bazel test //compiler_v2/api/internal/...` green (all 8
        new host test files PASS, ~200 new test cases).
  - [ ] `bazel run //compiler_v2/conformance:run_conformance` —
        pass count delta is **+2 to +3** (Any-of-Any rows).  Skip
        and fail counts unchanged.
  - [ ] `scripts/lint.sh` clean across all touched files.
  - [ ] `lint-backlog.md` entries 85-96 deleted.
  - [ ] `per-component-test-coverage.md` rows ticked for: WKT
        wire-format decoder (runtime), wrapper-type dispatch
        (host), wire error codecs, proto-field readers,
        proto-field writers, Any-unwrap, map-key equality,
        timestamp accessors.
  - [ ] `testing-checklist.md` rows ticked for the same
        components × pipeline stages.
  - [ ] `cel-host-surface.md` updated to reflect new ABI
        (removed `cel_wkt_unwrap_*` trampolines, added
        `cel_any_peel_user_schema`).
  - [ ] WAT traces under `doc/implementation-plan/rewrite/wat/`
        updated for the three call-site changes in Slice C.
  - [ ] `language-feature-unlock-analysis.md` headline number
        refreshed (already stale per the 2026-05-18 audit).
  - [ ] Status header on this doc flipped to `shipped <date>`
        with a one-paragraph "what landed" summary.
  - [ ] Future work section appended.

## 10. Future work surfaced (to fill at closeout)

(Populated when the milestone ships.  Candidates already
identified during planning:)

  - **`Struct` / `Value` / `ListValue` WKT decoding** in
    `cel_wkt_wire.c`.  Three remaining WKTs; defer until a
    feature requires them.
  - **Cross-origin aggregate materialiser** (the M12 stub-target
    of the `ABSL_CHECK` conversions).  Lets `host_list + arena_list`
    and `host_map == arena_map` actually work instead of
    crashing.
  - **Move user-schema field reads partially to runtime?**  If
    the runtime had access to a per-Instance descriptor-set
    handle, it could parse user-schema messages directly.  Big
    architectural shift; out of scope but worth a design probe.
  - **Per-component fuzz tests.**  The wire-format decoder is
    an obvious fuzz target — extend the Slice B test coverage
    with a libfuzzer harness if available.
  - **Generalise table-driven dispatch.**  `kWrapperOps` is
    the obvious template; the same pattern applies to
    `cel_host_proto_write`'s repeated-write per-cpp_type
    dispatch and to `cel_host_codec::EncodeValue`'s per-kind
    dispatch.
