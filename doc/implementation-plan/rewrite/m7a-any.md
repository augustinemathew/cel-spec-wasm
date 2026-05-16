# M7-A — `google.protobuf.Any` pack / unpack

Status: **M7-A.A + M7-A.B shipped 2026-05-16.  Pack arm via
`WriteMessageOrPack`; read-side unwrap via `UnpackAnyToValue` in
`ProtoBacking::ReadField`; frontend §3.5.A select-through-Any
carve-out admits `msg.single_any.x` past `RejectDyn`.  Fixture
extended with `single_any / repeated_any / map_str_to_any`.
Conformance: 1058 → 1065 (+7 PASS).  M7-A.C (cel_message_eq peel)
pending.  Depends on M7 (shipped); independent of M8 and M7-B.**

> **Status note.**  This doc is LLD-with-probes: §10 lists empirical
> findings against the running build (conformance + reflection
> behaviour + perf), §11 lists the implementation scaffolds (e2e
> test file, bench file, no WAT trace required) already in tree.
> Production code under `compiler_v2/api/internal/cel_host.cc`
> remains unchanged pending M7-A.A's implementation slice.

The plan covers `google.protobuf.Any` semantics end-to-end — packing
a typed-message RHS into an `Any`-typed field at construction time,
unpacking an `Any`-typed read into a value typed as the underlying
message, and the equality / `type(...)` paths that must look through
the wrapper.  No new ABI tables, no new codegen arms; the entirety
of the work sits in `compiler_v2/api/internal/cel_host.cc` (Layer-2)
and the cross-Instance descriptor-pool plumbing that already lives
on `CelHostBindings`.

**Out of scope:** every other proto-literal concern — M7 owns
`kStructExpr` codegen and the per-cpp_type field-set ladder; M8
owns wrapper auto-wrap and the wrapper `==` peel; M7-B owns
`Timestamp` / `Duration` constructors.  Extension fields,
custom-pool resolution, and dynamic-message factories stay
follow-ups per M7's §9 future-work backlog.

## 1. Why M7-A

After M7.A–E + M9 + M10, conformance sits at `pass=1058 / skip=693
/ fail=703 / total=2454` (43.1%).  Per the
`compiler_v2/conformance/README.md` "Forecast by open milestone"
table, **`Any` packing (M7-future)** is named separately from M7
itself with an estimated **+5–9 PASS**, gating on:

  - `proto3.textproto :: literal_wellknown/any` — the canonical
    `TestAllTypes{single_any: TestAllTypes{single_int32: 1}}` row.
  - `proto2.textproto :: literal_wellknown/any` — proto2
    counterpart.
  - `proto3.textproto :: empty_field/single_any` — read-side
    `TestAllTypes{}.single_any` returning null-on-unset.
    (Already shipped at M7 via the null-clear-on-singular-message
    path — re-asserted as M7-A regression coverage.)
  - Scattered rows in `proto2.textproto` / `proto3.textproto`
    that mix Any with other field types in the same construction
    (~3–5 rows).
  - `dynamic.textproto` rows that build through Any (~1–2 rows).

| Fixture | Today (PASS) | Post-M7-A (estimate) | Driving slice |
|---|---:|---:|---|
| `proto3.textproto` (Any rows) | 1 of ~4 | +3 – +4 | M7-A.A + M7-A.B |
| `proto2.textproto` (Any rows) | 1 of ~4 | +3 – +4 | M7-A.A + M7-A.B |
| `comparisons.textproto` (Any-equality rows) | within 327 / 406 | +1 – +2 | M7-A.C |
| `dynamic.textproto` (`dyn(Any{...})`-shaped rows) | within 4 / 226 | 0 – +1 | static-subset gate may reject before reach |
| **Total projected** | — | **+7 – +11 PASS** | — |

### 1.1 What fails today (probed 2026-05-16)

Ran `bazel run //compiler_v2/conformance:run_conformance --
--max_fail_examples=2000` and grepped for the M7-tripwire string
"`Any packing / wrapper auto-wrap is M7-future / M8`".  Result: **15
distinct callsite hits**, distributed across:

  - 7 hits in `proto2.textproto` + `proto3.textproto`:
    `Foo{single_any: TestAllTypes{...}}` (singular Any from typed
    message) and the proto2 counterpart.
  - 9 hits in `wrappers.textproto`: each of `Foo{single_any:
    <WrapperKind>Value{value: x}}` for the 9 wrapper kinds
    (BoolValue, Int32Value, Int64Value, UInt32Value, UInt64Value,
    FloatValue, DoubleValue, StringValue, BytesValue).  These
    are **Any-packing**, not wrapper-auto-wrap — the runtime sees
    a wrapper *message* on the source side and an Any-typed *field*
    on the destination side.  M7-A.A handles all 9 with no
    wrapper-specific code (Probe A confirmed reflection-pack works
    on any non-Any source descriptor).

In addition, **3 conformance rows** in `dynamic.textproto :: any`
construct an Any *directly* (`google.protobuf.Any{type_url: '...',
value: b'...'}`) — but these are gated by a *different* failure
mode: cel-cpp's checker types `Any{...}` as `dyn`, and the v2
static-subset rejects.  See §10.3 for the probe finding.  These
rows do NOT reach the M7-tripwire; they fail at the frontend.

> **Plan-vs-probe delta.**  The original §1 estimate was "+5–9
> PASS" carved straight from `m7-proto-literals.md` §9.  The probed
> count of 15 distinct callsite hits is consistent with that
> estimate after accounting for one-conformance-row-per-3-callsite
> ratio observed in M7 row-vs-callsite mappings.  Updating §1's
> table to **+7..+11** to reflect the wrapper-packed-into-Any
> cohort (m7-proto-literals.md's M8-deferred wrapper rows that
> are actually Any-shaped, not auto-wrap-shaped).  This is one of
> the load-bearing findings of probe A — the M7-future Any work
> graduates more rows than the original m7-proto-literals.md §9
> bullet predicted.

(Estimate widens above the README's `+5–8` line because the
M7-shipped null-clear arm already graduated several rows that
were counted as "Any-blocked" in the original M7 plan; the
remaining Any-specific rows are pack + unpack + read-side
type-routing, all M7-A.)

The shipping value is **not the conformance count** — it is the
removal of the descriptor-mismatch `UnimplementedError` callsites
M7 left in `cel_host.cc` as per-row failure surfaces (eight
locations, all named "Any packing / wrapper auto-wrap is
M7-future / M8" — see §4.1 for the catalogue).  Each one is a
trapdoor: a row that lands on it gets a wasm trap instead of a
clean CEL_ERROR.  M7-A flips them to either real packing
(construction-side) or real unpacking (read-side) and tightens
the residual "wrapper-shaped descriptor mismatch" arm to the
M8 surface only.

## 2. Scope

### 2.1 In-scope (per `langdef.md` §"Protocol Buffer Data Conversion" + cel-cpp's `runtime/standard/equality_functions.cc` Any arm)

  - **Construction-side packing.**  `Foo{single_any: TypedMsg{...}}`
    where `single_any` is `google.protobuf.Any`-typed and the RHS
    is a typed message (anything *not* `google.protobuf.Any`
    itself).  `CelSetFieldImpl`'s CPPTYPE_MESSAGE arm packs via
    `google::protobuf::Any::PackFrom(src)` — sets
    `type_url = "type.googleapis.com/<FQN>"` and serialises
    `src` into `value`.
  - **Construction-side identity.**  `Foo{single_any: Any{...}}`
    where the RHS is *already* an Any — descriptor matches; just
    `CopyFrom` (existing M7 code path, no change).
  - **Read-side unwrapping.**  `msg.single_any` returns a
    `Value::Message(unpacked)` typed as the unwrapped FQN, not
    as `Any`.  Per langdef "Any fields … behave like the
    underlying type at read time."  `ProtoBacking::ReadField`
    (and `OwnedProtoBacking::ReadField` via composition) gain
    an Any-aware branch: when the resolved `FieldDescriptor`'s
    `message_type()` is the Any descriptor, parse `type_url` →
    look up FQN in the per-Instance descriptor pool → parse
    `value` bytes → return a backing wrapping the resolved
    typed message.
  - **Equality across the wrapper.**  `Any{...} == TypedMsg{...}`,
    `TypedMsg{...} == Any{...}`, and `Any{...} == Any{...}` (the
    latter delegates to `MessageDifferencer` with Any-aware
    comparison, which already treats two equally-packed Anys as
    equal).  M5.B step 2b's `cel_message_eq` gets a `peel-Any`
    prelude that mirrors the read-side unpack and re-enters the
    polymorphic ladder with the unwrapped values.
  - **`type(any_msg)` returns the underlying FQN.**  M9's
    `cel_host.resolve_message_type_name` reads the operand's
    `HostMessageBacking::message()->GetDescriptor()->full_name()`.
    Once read-side unwrapping (M7-A.B) lands, the backing
    already points at the unwrapped message, so `type(...)`
    returns the right name with no M7-A.B-specific code in
    M9's path.  (Test coverage required to pin the invariant.)
  - **Null-clear on Any field set.**  `Foo{single_any: null}`
    already routes through M7's `CEL_NULL → ClearField` arm in
    `SetScalarField`'s CPPTYPE_MESSAGE path.  Regression-test
    that M7-A.A's packing branch doesn't shadow this.

### 2.2 Out-of-scope (deferred)

  - **`Any` literal construction `Any{type_url: "...", value:
    "..."}`.**  Treating an Any as a plain proto message with two
    fields — RHS is two bytes-typed kStructEntry rows.  Already
    handled by M7.B's per-cpp_type scalar set (STRING + BYTES).
    Lands as a regression-test, not new code.
  - **Cross-pool descriptor resolution.**  M7-A.B looks up the
    unwrapped FQN against the **same** descriptor pool that
    `BuildCelHostBindings` already passes to `CelHostBindings`
    (today: `DescriptorPool::generated_pool()`).  Embedders that
    load schemas via `SchemaProtoSource` or `SchemaDescriptorSet`
    get a different pool at compile time, and that pool is not
    yet plumbed through to `Engine::Plan` — that's the M7
    "Dynamic-descriptor MessageFactory" future-work item.
    M7-A inherits the same limitation; a `type_url` whose FQN is
    not in `generated_pool()` surfaces as `CEL_ERROR /
    CEL_ERR_FIELD_NOT_FOUND` (re-using the existing code; rename
    deferred to a future error-code slice).
  - **`dyn(Any{...})` admissibility.**  The static-subset gate
    rejects most `dyn(...)` aggregate shapes (Slice 1.5).  M7-A
    doesn't loosen that gate.  If a future milestone admits
    `dyn(Any{...})`, the wrapper-peel becomes runtime-typed and
    needs a `kDynamic` arm — flagged as future work in §9.
  - **`Any` packing of a wrapper message** (`Foo{single_any:
    Int32Value{value: 5}}`).  Falls out of M7-A.A for free as
    long as the wrapper-message is constructed explicitly
    (the RHS is a `kStructExpr` whose result is a typed message,
    same as any other typed RHS).  The *scalar auto-wrap into
    Any* case (`Foo{single_any: 5}`) is an M8 concern (it's the
    auto-wrap path) and a separate spec question (does CEL even
    define this?  cel-cpp does not).  Stays rejected at the
    M8 surface for now.
  - **`Any` of `Any` recursion.**  `Foo{single_any: Any{...}}`
    where the inner Any further wraps another Any.  Mechanically
    falls out of M7-A.B's recursive unwrap if the field-read
    visits the Any-detector again, but no conformance row tests
    this.  Spec-allowed by proto semantics; M7-A.B should not
    `ABSL_CHECK` against it.  Test row required to pin
    behaviour.
  - **`Any` inside `repeated` / `map` fields.**  Per langdef the
    pack/unpack semantics extend uniformly.  Out-of-scope only
    because M7's repeated/map cpp_type-MESSAGE arms call into
    the same descriptor-mismatch guard the singular arm uses;
    a single Any-aware helper covers all three call sites at no
    extra cost.  Tracked inside M7-A.A as "fold the Any branch
    through every CopyFrom site" (§4.1).

### 2.3 Carve from M7's future-work backlog

M7's `m7-proto-literals.md` §9 listed `Any pack/unpack` as a
deferred milestone with the estimate "+5–8 PASS".  That backlog
item *is* M7-A.  The "descriptor-mismatch guard at every
`CopyFrom` site so `TestAllTypes{single_any: BoolValue{...}}`-
shaped rows fail per-row instead of CHECK-aborting the
conformance run" line is the M7-shipped tripwire; M7-A removes
it for Any-shaped mismatches specifically, leaving the
remaining wrapper-shaped mismatch in place for M8 to clear.

## 3. Spec-mandated semantics

Citations from `doc/langdef.md` §"Protocol Buffer Data
Conversion" and `third_party/cel-cpp/runtime/standard/`.

### 3.1 Construction: typed RHS into Any field (langdef §"Protocol Buffer Data Conversion")

Spec wording: an Any-typed field "may be assigned any concrete
message value".  At construction time the runtime packs:

  - `type_url = "type.googleapis.com/" + src->GetDescriptor()->full_name()`.
  - `value = src->SerializeAsString()`.

cel-cpp ships this via `google::protobuf::Any::PackFrom(*src)`
(see `third_party/cel-cpp/internal/proto_wire.cc` and the
field-mutation paths in the runtime; the reflection-level helper
is `Any::PackFrom`).

Three valid shapes for the RHS of an Any-field set:

  1. **Already an Any** — `Foo{single_any: Any{...}}`.  Descriptor
     matches; copy verbatim.  Existing M7 code path.
  2. **A typed message** — `Foo{single_any: TestAllTypes{...}}`.
     Pack: serialise + set type_url + set value.  This is M7-A.A.
  3. **A scalar / list / map / null** — checker rejects (Any is a
     message type; the checker won't admit a scalar literal as a
     message-typed assignment).  Already filtered by cel-cpp's
     type-checker before M7-A sees the AST.

### 3.2 Read: Any field returns the underlying-typed value (langdef §"Field Selection" + §"Protocol Buffer Data Conversion")

Spec wording: a read of an Any-typed field returns "the wrapped
value as if it were of the wrapped type".  i.e. given
`Foo{single_any: Bar{x: 1}}` (or an Any whose type_url names
`Bar`), the expression `foo.single_any.x` reads `1`.

cel-cpp ships this via the `ProtoFieldAccessor::Get` path:
when the field's type is Any, parse `type_url`, look up the FQN
in the descriptor pool, parse `value`, and return a typed proto
value.  See `third_party/cel-cpp/runtime/standard/equality_functions.cc`
and `runtime/internal/runtime_type_provider.cc` for the
descriptor-pool plumbing.

**Error envelope.**

  - `type_url == ""` (unset Any) → field reads as `null` per
    M7's null-on-unset-message rule.  (Already shipped; M7-A.B
    must not regress.)
  - `type_url` doesn't parse as `type.googleapis.com/<FQN>` →
    `CEL_ERROR / CEL_ERR_TYPE_MISMATCH` (cel-cpp: `kInvalidArgument`).
  - `<FQN>` not in the descriptor pool → `CEL_ERROR /
    CEL_ERR_FIELD_NOT_FOUND` (cel-cpp: `kFieldNotFound`-shaped).
  - `value` bytes don't parse as `<FQN>` →
    `CEL_ERROR / CEL_ERR_TYPE_MISMATCH`.

### 3.3 Equality across the wrapper (langdef §"Equality")

Spec wording: equality "is performed on the underlying value
types".  i.e. `Any{type_url: "...Bar", value: <Bar{x:1}>} ==
Bar{x:1}` is `true`.

cel-cpp ships this in `equality_functions.cc::IsAnyEqual` —
both operands are peeled to their underlying values before the
polymorphic equality ladder.

`cel_message_eq` (the M5.B step 2b polymorphic kernel) is
descriptor-aware on entry (both operands are `kMessage`).  M7-A.C
adds an Any-detector prelude:

  - If both operands have non-Any descriptors → straight to
    `MessageDifferencer::Equals` (existing path).
  - If either operand has the Any descriptor → unwrap it (same
    code path as M7-A.B's read-side unwrap, exposed as a small
    helper `UnpackAnyToBacking`), then re-enter the polymorphic
    equality ladder via `cel_value_eq`.  The recursive entry
    handles the unpacked-to-non-message case (Any wrapping a
    scalar message-wrapper, etc.).

**Considered and rejected: `MessageDifferencer`'s built-in
Any-aware mode.**  Protobuf's `MessageDifferencer` has an option
to treat two Any operands as equal iff their unpacked values
compare equal under a recursive `MessageDifferencer`.  Why not
use that for the Any-vs-Any case:

  - The recursive entry already passes through `cel_value_eq`,
    which honours CEL's cross-type numeric ladder (`1 == 1u`
    semantics) — `MessageDifferencer`'s Any mode does *not*.
    Wrapping the recursion in `cel_value_eq` keeps CEL semantics
    consistent regardless of whether the operands are wrapped in
    Any or not.
  - The Any-vs-typed case (one operand wrapped, one not) is
    NOT a `MessageDifferencer` mode at all — it's a CEL-specific
    rule.  We need the unpack-then-`cel_value_eq` path anyway;
    using it uniformly is simpler than mixing two modes.

### 3.4 `type(any_msg)` returns the unwrapped FQN

Spec wording: implied by §3.2 — once the value is read as the
unwrapped type, `type(...)` over it reads the unwrapped
descriptor.  cel-cpp parity: `type_conversion_functions.cc`
reads `GetRuntimeType()` from the value, which is already the
unwrapped type.

M9's `cel_host.resolve_message_type_name` reads the operand's
`HostMessageBacking::message()->GetDescriptor()->full_name()`.
After M7-A.B's read-side unwrap, the backing already points at
the unwrapped message — so `type(...)` Just Works with no
M9-specific change.  Pinned by a dedicated regression test in
`m7a_test.cc::TypeOfUnpackedAny`.

### 3.5 cel-cpp parity & static-subset (dyn-gate) carve-outs

**Customer-usefulness constraint.**  cel-cpp accepts a set of Any
patterns that v2's static-subset (`RejectDyn`) currently rejects.
Without carve-outs for these, M7-A is shipped-but-unreachable for
the most common customer use cases.  This section enumerates the
gap and the planned mitigations.

**What cel-cpp accepts that v2 rejects today (probed 2026-05-16):**

| Pattern | cel-cpp type | v2 status | Mitigation |
|---|---|---|---|
| `msg.single_any.x` (chained select through Any) | `dyn` | Rejected by `RejectDyn` | Resolved by M7-A.B's read-side unwrap, BUT the checker still types the select as `dyn` — needs a frontend special-case (see §3.5.A) |
| `msg.single_any.type_url` / `.value` | `dyn` | Rejected | §3.5.A — special-case Any field-name selection at the frontend |
| `Foo{repeated_any: [Bar{}, Baz{}]}` (heterogeneous list literal) | `list(dyn)` | Rejected | §3.5.B — admit `list(dyn)` whose target field is `repeated google.protobuf.Any` |
| `Foo{map_str_to_any: {'k': Bar{}}}` | `map(string, dyn)` | Rejected | §3.5.B — admit `map(_, dyn)` whose target field is `map<_, Any>` |
| `Any{type_url: 'x', value: b'y'}` (direct Any literal) | `dyn` | Rejected (probe C) | §3.5.C — admit `google.protobuf.Any{...}` as kStructExpr, not dyn |
| `msg.single_any == TypedMessage{...}` (Any-vs-typed eq) | `dyn`-tainted on the Any side | Rejected | §3.5.A unlocks the LHS; M7-A.C peels at runtime |

**§3.5.A — Frontend Any-aware select pass.**  When the checker types
an expression as `dyn` and the immediate-parent select operand has a
**static** type of `google.protobuf.Any`, type the select chain as
follows: `<any>.type_url` → `string`, `<any>.value` → `bytes`,
`<any>.<arbitrary>` → routed to the M7-A.B unwrap path which surfaces
the wrapped value's concrete CelType.  This is a v2-frontend
extension that mirrors what cel-cpp's runtime already does at eval
time, lifted into the type checker so RejectDyn doesn't see `dyn`.

  - Lives in `compiler_v2/frontend/parse_and_check.cc` as a post-
    check rewrite (or in `compiler_v2/ir/typed_ast.cc`'s
    `ReprOfWellKnown` if cleaner).
  - Decision boundary: a `dyn`-typed expression whose direct
    ancestor in the AST chain has a `well_known` type of Any.
  - Out-of-scope: cross-pool unwrap (M7-A.B's R1).

**§3.5.B — Admit `list(dyn)` / `map(_, dyn)` into `repeated/map
Any`.**  cel-cpp's checker types a list of heterogeneous-but-typed-
message elements as `list(dyn)` when the destination field is
`repeated Any`.  RejectDyn fires on this even though the runtime
path (M7-A.A) packs each element correctly.  Carve-out: when the
target field-set's `FieldDescriptor::message_type()->full_name()`
is `google.protobuf.Any`, accept the source as `list(dyn)` /
`map(_, dyn)` and let the runtime pack arm handle the per-element
descriptor at write time.

  - Lives in `compiler_v2/frontend/parse_and_check.cc::RejectDyn`
    as an opt-in based on the destination field descriptor.

**§3.5.C — Admit direct `Any{type_url, value}` literal.**  cel-cpp
admits this as a typed `google.protobuf.Any` literal.  v2 rejects
because the well-known type maps to `dyn`.  Carve-out: when the
struct literal's resolved descriptor is `google.protobuf.Any`,
admit it as `kStructExpr` typed as `google.protobuf.Any` (not
`dyn`); the existing M7 pack path handles the two-field set.

  - Lives in `compiler_v2/ir/typed_ast.cc::ReprOfWellKnown` — flip
    Any from `dyn` repr to `kMessage` repr keyed on the Any FQN.

**Scope split.**  §3.5.A and §3.5.B land with M7-A.B (so customers
can read packed Any fields through chained select / equality).
§3.5.C lands with M7-A.D as a small frontend carve-out that
unlocks the 3 `dynamic.textproto :: any` rows the inventory in
§12 flags.  M7-A.A is unaffected (its pack arm doesn't itself
emit `dyn`-tainted ASTs).

## 4. Architecture

### 4.1 Layer-2 — `CelSetFieldImpl` Any-pack arm

Eight call sites in `cel_host.cc` currently surface
`UnimplementedError` for descriptor mismatches "M7-future / M8":

  1. `SetScalarField` CPPTYPE_MESSAGE (singular, line ~1789).
  2. `AppendRepeatedFromCelValue` CPPTYPE_MESSAGE
     (repeated of message, line ~1960).
  3. `AppendRepeatedFromHostListValue` CPPTYPE_MESSAGE
     (repeated of message, host source, line ~2055).
  4. `InsertArenaMapEntry` map<_,message> value (line ~2145).
  5. `InsertHostMapEntry` map<_,message> value (line ~2285).
  6–8. Three sibling call sites threaded through map/repeated
       paths.

Each site today does:

```cpp
if (src_msg->GetDescriptor() != field.message_type()) {
  return absl::UnimplementedError(absl::StrCat(
      "..." , " — Any packing / wrapper auto-wrap is M7-future / M8"));
}
google::protobuf::Message* dst = refl->MutableMessage(&msg, &field);
dst->CopyFrom(*src_msg);
```

M7-A.A factors the per-site guard into one helper:

```cpp
// AssignMessageOrPack: write `src` into the singular message slot
// referenced by `field` on `parent`.  Three shapes:
//   (1) src descriptor == field descriptor                  → CopyFrom (M7).
//   (2) field is google.protobuf.Any, src is non-Any        → PackFrom (M7-A.A).
//   (3) descriptor mismatch outside Any                     → Unimplemented (M8).
absl::Status AssignMessageOrPack(
    google::protobuf::Message* parent,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Message& src);
```

Body:

```cpp
const google::protobuf::Descriptor* field_desc = field.message_type();
const google::protobuf::Descriptor* src_desc = src.GetDescriptor();
if (src_desc == field_desc) {
  parent->GetReflection()->MutableMessage(parent, &field)->CopyFrom(src);
  return absl::OkStatus();
}
if (field_desc->full_name() == "google.protobuf.Any") {
  auto* any_out = parent->GetReflection()->MutableMessage(parent, &field);
  if (!any_out->GetReflection()->GetMessage(*any_out, ...)
        // …or via Any-typed dynamic_cast / typed any::PackFrom path
        ) { /* unreachable; descriptor pool yields an Any-shaped descriptor */ }
  if (!static_cast<google::protobuf::Any*>(any_out)->PackFrom(src)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CelSetFieldImpl: PackFrom failed for src descriptor `",
        src_desc->full_name(), "`"));
  }
  return absl::OkStatus();
}
// Wrapper-shaped mismatch — kept as-is for M8 to clear.
return absl::UnimplementedError(absl::StrCat(
    "CelSetFieldImpl: field `", field.name(), "` (type `",
    field_desc->full_name(), "`) source has different descriptor `",
    src_desc->full_name(), "` — wrapper auto-wrap is M8"));
```

**Open question:** `Any::PackFrom` is generated-code only; the
descriptor-pool resolved `Descriptor*` may not let us
`static_cast` to `Any`.  Mitigation: fall back to manual pack —
serialise via `src.SerializeAsString()`, set `type_url` +
`value` on the destination message through reflection.  This is
the form cel-cpp uses (it can't assume the generated Any class
either; it operates on `Message*`).  Specifically:

```cpp
const google::protobuf::Reflection* any_refl = any_out->GetReflection();
const google::protobuf::FieldDescriptor* type_url_fd =
    field_desc->FindFieldByName("type_url");
const google::protobuf::FieldDescriptor* value_fd =
    field_desc->FindFieldByName("value");
any_refl->SetString(any_out, type_url_fd,
                    absl::StrCat("type.googleapis.com/", src_desc->full_name()));
any_refl->SetString(any_out, value_fd, src.SerializeAsString());
```

Pick whichever form (typed cast or reflection-driven) compiles
cleanly against the codebase's protobuf version; functionally
equivalent.  WAT-first authoring is not required (no new wasm
ABI; the trampoline already exists).

Threading the helper through the eight sites is mechanical:
each site's old-form guard becomes a call to
`AssignMessageOrPack` and removes the local `CopyFrom`.

### 4.2 Layer-2 — `ProtoBacking::ReadField` Any-unwrap arm

`compiler_v2/api/internal/cel_host.cc::ProtoBacking::ReadField`
(and `OwnedProtoBacking::ReadField` via composition) handles
CPPTYPE_MESSAGE singular reads by returning
`Value::HostMessage(make_shared<ProtoBacking>(*sub))`.

The Any-aware variant:

```cpp
if (sub_descriptor->full_name() == "google.protobuf.Any") {
  ASSIGN_OR_RETURN(auto unpacked, UnpackAnyToBacking(*sub_msg, ctx.bindings));
  // unpacked is a shared_ptr<HostMessageBacking> wrapping the
  // resolved typed message.  Returns CEL_ERROR if type_url
  // can't be parsed, FQN not in pool, or value bytes don't
  // round-trip.
  if (unpacked == nullptr) return Value::Error(...);
  return Value::HostMessage(std::move(unpacked));
}
```

`UnpackAnyToBacking` lives in cel_host.cc:

```cpp
absl::StatusOr<std::shared_ptr<HostMessageBacking>> UnpackAnyToBacking(
    const google::protobuf::Message& any, const CelHostBindings& bindings);
```

Body (sketch):

  1. Read `type_url` + `value` from `any` via reflection.
  2. Parse `type_url`: the prefix before the last `/` is
     discarded (per cel-cpp's `AnyTypeFqn` helper already
     present in
     `compiler_v2/conformance/binding_marshal.cc:35`).  The
     suffix is the FQN.
  3. Look up the FQN in `bindings.descriptor_pool` (new field
     — see §4.4).  Not found → `Value::Error(kFieldNotFound)`.
  4. `MessageFactory::generated_factory()->GetPrototype(desc)
     ->New()` → fresh proto.
  5. `ParseFromString(value)` → not OK → `Value::Error(
     kTypeMismatch)`.
  6. Wrap in `OwnedProtoBacking(unique_ptr<Message>)` (the
     same owning-backing class M7.A introduced for
     `cel_make_message`) and return as shared_ptr.

The existing `binding_marshal::UnpackAny` in
`compiler_v2/conformance/binding_marshal.cc:51` is a near-exact
template for the body — the only differences are (a) it lives
in the conformance harness (test-side, not runtime-side), and
(b) it constructs a `unique_ptr<Message>` and trusts the caller
to wrap.  M7-A.B factors it out: lift the body into
`compiler_v2/api/internal/cel_host.cc`, export the
`HostMessageBacking`-shaped wrapper for runtime use, and have
the conformance harness call into the same helper to keep the
test and production paths from drifting.

### 4.3 Layer-2 — `cel_message_eq` Any-peel arm

`CelMessageEqImpl` (m5-kcall-comprehensions.md §"M5.D step 2 +
cel_message_eq") reads both operand CelValues, dereferences
their `HostMessageBacking::message()` pointers, and compares
via `MessageDifferencer::Equals`.

M7-A.C adds a peel prelude:

```cpp
const google::protobuf::Message* a_msg = a_backing->message();
const google::protobuf::Message* b_msg = b_backing->message();
const bool a_is_any = a_msg->GetDescriptor()->full_name() ==
                     "google.protobuf.Any";
const bool b_is_any = b_msg->GetDescriptor()->full_name() ==
                     "google.protobuf.Any";
if (a_is_any || b_is_any) {
  // Unpack the Any operand(s) and re-enter `cel_value_eq`,
  // which routes back through the polymorphic ladder
  // (cross-numeric, message, etc.).
  ASSIGN_OR_RETURN(auto a_peeled,
                   a_is_any ? UnpackAnyToBacking(*a_msg, ctx.bindings)
                            : Pass-through(a_backing));
  ASSIGN_OR_RETURN(auto b_peeled,
                   b_is_any ? UnpackAnyToBacking(*b_msg, ctx.bindings)
                            : Pass-through(b_backing));
  return CompareViaCelValueEq(...);  // CelValue-rewrap + cel_value_eq.
}
return MessageDifferencer::Equals(...);   // existing path.
```

Both peels share the same `UnpackAnyToBacking` from §4.2.

### 4.4 `CelHostBindings` — expose the descriptor pool

Today `CelHostBindings` (`cel_host.h`) carries
`field_refs`, `attributes`, `unknown_patterns`, `message_types`.
The descriptor pool is reached *only* at `Engine::Plan` time via
`BuildCelHostBindings(abi, pool, host_env)`
(`engine.cc:344`) — the pool resolves type ids; the pool
pointer itself isn't stored.

M7-A.B needs runtime pool access (to resolve a `type_url`'s FQN
that may not be in `cel.abi.types[]`).  One additive field:

```cpp
struct CelHostBindings {
  // ... existing fields ...
  // M7-A.B: descriptor pool the Layer-2 Any-unwrap path consults
  // when resolving an Any's type_url to a concrete Descriptor*.
  // Identical to the pool BuildCelHostBindings used to resolve
  // `cel.abi.types[]` — held by raw pointer because the pool is
  // owned by the embedder (the generated pool for
  // statically-linked descriptors; a SchemaProtoSource-built
  // pool for dynamic schemas) and must outlive the Instance.
  const google::protobuf::DescriptorPool* absl_nullable
      descriptor_pool = nullptr;
};
```

Nullable so single-file unit tests that don't use Any can leave
it at its default.  Layer-2 callsites that need the pool
(M7-A.B and M7-A.C) check non-null on entry and surface a
clean error otherwise.

No ABI-wire change — the pool is host-side state, never
serialised.

### 4.5 No codegen / no frontend changes

  - **Codegen** — `kStructExpr` lowering at M7 already emits
    `cel_set_field` for every entry.  The trampoline does the
    pack; codegen sees nothing new.
  - **Frontend** — cel-cpp's checker admits `Foo{single_any:
    TypedMsg{...}}` because Any is the proto well-known "open"
    type; no v2-side checker extension required.
  - **ABI** — no new tables.  Existing `cel.abi.types[]` carries
    descriptors the program *references*; Any unwrap may
    encounter descriptors named only in the embedder's pool.
    The pool plumbing (§4.4) is the only surface.
  - **No new ABI section / no new ABI version bump.**  The
    `descriptor_pool` field on `CelHostBindings` is host-side
    runtime state populated at `Engine::Plan` from the same
    embedder-supplied pool the existing code already consumes
    — no wire-format change.

## 5. Sequencing — slices

Four slices, each shippable independently.  Effort sized
small / medium / large per the m7-proto-literals.md convention.

### M7-A.A — `cel_set_field` Any-pack arm

`AssignMessageOrPack` helper + threaded through eight call
sites.  Removes the M7-tripwire `UnimplementedError` for
Any-shaped mismatches; preserves it for wrapper-shaped
mismatches (M8 surface).

  - **No WAT trace required** — no new wasm imports / no new
    ABI surface; the trampoline body changes only.
  - **No ResolvePass / LayoutPass / codegen change.**
  - **Tests.**
    - `cel_host_test.cc` — parameterised over (Any field on
      proto2 / proto3) × (singular / repeated-arena /
      repeated-host / map<_,_>-arena / map<_,_>-host) ×
      (typed-message RHS / Any-as-RHS / null RHS).
    - `m7a_test.cc::AnyPackE2ETest` — the
      `proto3.literal_wellknown/any` conformance row plus its
      proto2 sibling + a deep-nested case
      (`Foo{single_any: Bar{nested: Baz{...}}}`).
  - **Closeout-row criterion.**  `proto3.literal_wellknown/any`
    flips PASS.
  - **Conformance unlock estimate.**  +3–5 PASS.
  - **Effort.**  Medium.

### M7-A.B — `ProtoBacking::ReadField` Any-unwrap arm

Lift `binding_marshal::UnpackAny` from the conformance harness
into `cel_host.cc` (rename to `UnpackAnyToBacking`).  Wire the
new `descriptor_pool` field on `CelHostBindings` from
`BuildCelHostBindings`.  Add the unwrap arm in `ReadField`'s
CPPTYPE_MESSAGE path (and the matching arm in
`AppendRepeatedFromCelValue` for repeated-of-Any reads, the
matching arm in `InsertArenaMapEntry` for map<_,Any>, etc. —
mirror M7-A.A's eight-site coverage on the read side).

  - **No WAT trace required.**
  - **Tests.**
    - `cel_host_test.cc` — every read site × (set-with-pack /
      set-with-CopyFrom-as-Any / unset / type_url-not-in-pool
      / value-bytes-corrupt).
    - `m7a_test.cc::AnyUnpackE2ETest` — `msg.single_any.x`
      where `single_any` was packed at construction; plus the
      regression `TestAllTypes{single_any: null}.single_any
      == null` row.
    - `m7a_test.cc::TypeOfUnpackedAny` — `type(msg.single_any)`
      returns the unwrapped FQN (regression on M9's
      type-name resolver; pinned even though M9's code is
      unchanged).
  - **Conformance unlock estimate.**  +3–4 PASS.
  - **Effort.**  Medium.

### M7-A.C — `cel_message_eq` Any-peel arm

`CelMessageEqImpl` peel prelude using `UnpackAnyToBacking`.
Both operands peeled if either is an Any; re-enter
`cel_value_eq`.

  - **No WAT trace required.**
  - **Tests.**
    - `cel_host_test.cc::CelMessageEqAnyPeelTable` — 9 cases:
      Any-vs-typed-equal, Any-vs-typed-different, typed-vs-Any
      (symmetric), Any-vs-Any-same-payload, Any-vs-Any-
      different-payload, Any-vs-Any-different-type_url, plus
      three null / unset combinations.
    - `m7a_test.cc::AnyEqualityE2ETest` — `Foo{single_any:
      Bar{...}} == Bar{...}` and the symmetric form.
  - **Conformance unlock estimate.**  +1–2 PASS.
  - **Effort.**  Small.

### M7-A.D — closeout

  - Run `bazel run //compiler_v2/conformance:run_conformance` and
    record the post-M7-A deltas in
    `compiler_v2/conformance/README.md`.
  - Run `scripts/run_full_suite.sh` (the closeout gate per
    CLAUDE.md "manual-tagged tests carry the load-bearing e2e
    assertions").
  - Flip this doc's status header to `shipped YYYY-MM-DD` with
    the "what landed" paragraph.
  - Tick `testing-checklist.md` rows under "Rewrite M7-A": every
    new CEL-type × pipeline-stage cell M7-A lit up (Any-pack
    at Layer-2, Any-unwrap at Layer-2, Any-peel at
    `cel_message_eq`, type-of-unpacked-Any at M9 regression).
  - Reconcile sibling docs:
    - `m7-proto-literals.md` §9 future-work — strike the
      `Any pack/unpack` bullet and replace with
      `→ shipped at M7-A`.
    - `compiler_v2/conformance/README.md` "Forecast by open
      milestone" — remove the `Any packing (M7-future)` row.
    - `cel-host-surface.md` §6 — note the
      `CelHostBindings.descriptor_pool` field (host-side, not
      ABI-wire).
    - `m8-wrapper-types.md` — note that M7-A's helper
      `AssignMessageOrPack` is the seam M8 extends for wrapper
      auto-wrap (M8.A becomes "add the wrapper arm to the
      same helper" instead of adding a third call site).
  - Append M7-A's "Future work" section.

  - **Effort.**  Small.

### 5.5 Exit criteria — minimize code, factor tests carefully

The user-facing exit bar for every M7-A slice (in addition to the
matrix in §6):

  - **Total code added is the minimum that makes the slice work.**
    Helper functions are factored so each call site loses more
    lines than the helper adds.  No new abstraction is shipped
    unless the slice has 3+ duplicate call sites that benefit.
    M7-A.A's `WriteMessageOrPack` is the canonical shape: one
    helper takes the resolved `dst` Message pointer (the caller
    already picked `MutableMessage` vs `AddMessage`), and the
    helper handles the three-shape dispatch (CopyFrom / Any-pack /
    Unimplemented) — replacing ~50 lines of per-site descriptor-
    mismatch guards with a single ~30-line function.
  - **e2e tests assert only what the static-subset gate can
    reach.**  cel-cpp's checker types selections through Any as
    `dyn`; the v2 dyn-gate rejects them today (see §3.5).  e2e
    coverage here uses `has(...)` and `size(...)` over packed Any
    fields — assertions that *don't* select through the Any.
    Byte-level pack invariants (type_url suffix, value round-
    trip) move to Layer-2 unit tests in
    `compiler_v2/api/internal/cel_host_test.cc`, which drive
    `CelSetFieldImpl` directly without the checker.
  - **Test factoring.**  Structural matrices use `TEST_P` +
    `INSTANTIATE_TEST_SUITE_P`; one-off invariants stay as
    `TEST_F` so the test name reads as the assertion.  Helper
    structs (`PackHarness` in cel_host_test.cc) own the staging
    boilerplate so each `TEST_F` body is ≤ 10 lines of intent.
  - **Coverage is extensive but the inventory in §12 drives
    selection.**  Every conformance row in §12's Cat 1/2/3 maps
    to at least one Layer-2 or e2e test once the slice it gates
    on ships.

## 6. Test matrix (load-bearing)

Per CLAUDE.md "Cover the edge-case matrix — this is a
compiler", every combination below MUST have at least one
explicit test (parameterised or longhand).  Negative coverage
(rejection cases) is ≥ 30% of the total per the same rule.

### 6.1 Pack positive matrix

| Dimension | Values | Count |
|---|---|---|
| Field shape | singular / repeated-arena-source / repeated-host-source / map<_,_>-arena / map<_,_>-host | 5 |
| RHS shape | typed-message / Any-as-RHS / null | 3 |
| RHS source | `kStructExpr` literal / ident-bound message / nested `kStructExpr` (`Foo{any: Bar{baz: Qux{...}}}`) | 3 |
| Proto syntax | proto2 / proto3 | 2 |

Combinatorial size is 90; the parameterised `TEST_P`
collapses structurally-identical cells to ~25 rows.

### 6.2 Unpack positive matrix

  - Singular Any field on the construction-time-packed message:
    `Foo{single_any: Bar{x: 1}}.single_any.x == 1`.
  - Singular Any field on an activation-bound proto message
    (the existing M2.C bind path; Any was packed by the
    embedder before binding).
  - Repeated Any: `Foo{repeated_any: [Bar{}, Qux{}]}[0].x` —
    cross-type access via index.
  - Map<string, Any>: `Foo{map_any: {"k": Bar{x:1}}}["k"].x`.
  - Round-trip via Any: `Foo{single_any: Bar{x: 1}}.single_any
    == Bar{x: 1}` (combines unpack + Any-peel equality).
  - `type(Foo{single_any: Bar{x: 1}}.single_any) ==
    "cel.expr.conformance.proto3.Bar"`.
  - Unset Any: `TestAllTypes{}.single_any == null`.

### 6.3 Negative / rejection matrix

  - `type_url == ""` on a set-Any (synthesised via `Any{}` or
    `Any{value: <bytes>}` without type_url) → read returns
    `CEL_ERROR / CEL_ERR_TYPE_MISMATCH`.
  - `type_url` doesn't have the `type.googleapis.com/` prefix
    (or any `/`) → `CEL_ERROR / CEL_ERR_TYPE_MISMATCH`.
  - `<FQN>` not in the embedder's descriptor pool →
    `CEL_ERROR / CEL_ERR_FIELD_NOT_FOUND`.
  - `value` bytes don't parse against the resolved descriptor
    → `CEL_ERROR / CEL_ERR_TYPE_MISMATCH`.
  - Pack: RHS is a scalar / list / map (checker rejects;
    regression-test that the diagnostic is the cel-cpp one,
    not an M7-A trap).
  - Pack: RHS is a non-Any message but the field is *not*
    Any-typed (wrapper-shaped mismatch) → still
    `Unimplemented` per M8 surface — regression-test that
    M7-A didn't accidentally graduate this case.
  - Equality: Any-vs-typed where the Any wraps a *different*
    typed message → `false` (not `Error`).
  - Equality: Any-vs-Any where both `type_url`s differ but the
    encoded bytes happen to be equal → `false` (Any equality
    requires type_url equality even after the peel — the peel
    produces values of different types, which compare unequal).

### 6.4 Test placement

  - `compiler_v2/api/internal/cel_host_test.cc` — Layer-2
    parameterised tables: `CelSetFieldImplAnyPackTable`,
    `CelGetFieldImplAnyUnwrapTable`, `CelMessageEqAnyPeelTable`.
  - `compiler_v2/conformance/binding_marshal_test.cc` —
    regression that `binding_marshal::UnpackAny` and
    `cel_host::UnpackAnyToBacking` agree on every test row
    (the two paths share the lifted body, so this is mostly a
    smoke check).
  - `compiler_v2/e2e/m7a_test.cc` (new) — every conformance-
    row-shape, parameterised against §6.1–6.3.

## 7. Risks + open questions

Ranked highest → lowest.

  - **R1 — `MessageFactory` reach across pools.**
    M7-A.B's `UnpackAnyToBacking` calls
    `MessageFactory::generated_factory()->GetPrototype(desc)`.
    If the descriptor came from a non-generated pool
    (`SchemaProtoSource` / `SchemaDescriptorSet`),
    `generated_factory()` returns nullptr.  Mitigation: use
    `MessageFactory::generated_factory()` only when the
    descriptor's pool *is* the generated pool; otherwise
    fall back to a per-`Instance` `DynamicMessageFactory`
    pinned to the embedder's pool.  Same constraint M7's R1
    raised — M7-A inherits it.  If the fallback turns out
    non-trivial to wire, scope M7-A.B to generated-pool-only
    for the first ship and track dynamic-pool unwrap as a
    sub-bullet under §9.
  - **R2 — `Any::PackFrom` on a reflection-resolved Any
    descriptor.**  Cannot rely on the typed Any class; must
    pack via reflection (`SetString` on type_url + value).
    Mitigation: §4.1 sketches the reflection path; cel-cpp
    uses the same trick.  Risk that protobuf's wire format
    for Any has subtleties (URL-encoding of FQNs?) —
    cross-check against `cel-cpp/internal/proto_wire.cc`
    before M7-A.A lands.
  - **R3 — `Any` of `Any` recursion stack depth.**  Spec-allowed
    but pathological.  Mitigation: the recursion is bounded by
    the depth of the proto literal at compile time (no runtime
    construction of Any-of-Any from inside an eval).  Add a
    parametrised test row at depth 3 and a CHECK at depth >
    1024 in `UnpackAnyToBacking` so an adversarial fixture
    can't blow the host stack.
  - **R4 — `cel.abi.types[]` doesn't list the unpacked types.**
    A program that constructs `Foo{single_any: Bar{...}}`
    interns `Foo` and `Bar` in `cel.abi.types[]` (Bar is
    visited by `ResolvePass`'s `MessageTypeIdVisitor`).  A
    program that *only reads* an Any field whose type_url
    points at `Baz` (e.g. against an activation-bound Foo)
    does NOT intern `Baz`.  Mitigation: M7-A.B doesn't need
    the types table — it resolves against the descriptor pool
    directly.  Test row that exercises this case to pin the
    invariant (`Activation::Bind` a Foo carrying a pre-packed
    Any of Baz; read `foo.single_any.x`).
  - **R5 — `OwnedProtoBacking` lifetime under nested Any
    unwrap.**  An Any-unwrap that returns an
    `OwnedProtoBacking` wraps a fresh proto allocated by the
    factory.  The backing's lifetime is the
    ExternrefTable-interned `shared_ptr`; the per-Eval
    `Reset()` frees it.  Mitigation: same lifetime model as
    M7's `cel_make_message`; M7-A.B re-uses
    `OwnedProtoBacking`, so the contract is unchanged.
    Test that an Any unwrapped from a transient construction
    survives long enough to be read in the same Eval.
  - **R6 — Equality recursion termination.**  M7-A.C's peel
    re-enters `cel_value_eq`.  An adversarial pair
    (`Any{Any{...}} == Any{Any{...}}`) recurses twice through
    the peel; cel_value_eq's polymorphic ladder is finite
    per kind so termination is guaranteed.  Pin with a depth-3
    Any-of-Any-of-Any equality row.

## 8. Out-of-scope (re-stated)

  - Wrapper auto-wrap (`Foo{w: 5}` where `w` is `Int32Value`-
    typed) — see `m8-wrapper-types.md`.
  - Cross-pool descriptor resolution — same as M7's R1.
  - `dyn(Any{...})` admissibility — static-subset gate.
  - Extension fields (`msg.[ext_name]`) — separate slice.
  - Per-eval current-time `now()` injection.
  - `Timestamp` / `Duration` field reads from Any.  Lands at
    M7-B (timestamps/durations) — the Any-unwrap arm at M7-A.B
    returns a `HostMessageBacking` wrapping the typed message
    regardless of what type it is; if the unwrapped type is
    `google.protobuf.Timestamp`, M7-B's read-path picks it up
    transparently with no M7-A involvement.

## 9. Future work

  - **Wrapper auto-wrap into Any** (`Foo{single_any: 5}` →
    pack into `Int32Value{value: 5}` → pack THAT into Any).
    Out-of-scope; depends on M8.A landing first.  Once M8.A
    is in, the same `AssignMessageOrPack` helper grows a
    "scalar RHS + wrapper field" arm; the Any branch reaches
    through it for free.
  - **Cross-pool unwrap.**  When `SchemaProtoSource` /
    `SchemaDescriptorSet` plumbs a non-generated pool through
    `BuildCelHostBindings`, M7-A.B's resolution needs a
    `DynamicMessageFactory` rather than the generated
    factory.  Tracked alongside M7's "Dynamic-descriptor
    `MessageFactory`" future-work item.
  - **`Any` with `type_url` schemes other than
    `type.googleapis.com/`.**  Spec allows alternative
    schemes; cel-cpp accepts any prefix and strips before the
    last `/`.  M7-A.B mirrors this.  If a fixture row needs
    a structurally different scheme (e.g. an SDK that uses
    `type.example.com/...`), no code change is required —
    flag here for the failure case (FQN not in pool) so the
    diagnostic is grokkable.
  - **Streaming-write packs.**  M7-A.A serialises via
    `SerializeAsString` → `SetString`.  A large RHS allocates
    twice (proto's internal serialiser buffer + the
    SetString copy).  If a benchmark surfaces this as a hot
    spot, switch to `Any::PackFrom` reflection-style with
    `SerializePartialToZeroCopyStream` and a stream-backed
    sink.  Out of scope at first ship.
  - **`MessageDifferencer` Any-aware mode as an alternate
    equality path.**  Considered and rejected in §3.3.  Note
    that if a future milestone needs Any-equality without the
    `cel_value_eq` re-entry (e.g. host-only comparison for
    `Activation::Bind` dedup), the protobuf-native option
    exists.

## 10. Probes (run 2026-05-16)

Empirical findings against the running build.  Each probe was a
small C++ scratch program built and run out-of-tree (under
`tmp_probe/` during authoring, deleted before commit).  Findings
→ load-bearing for the implementation slices.

### 10.1 Probe A — `Any::PackFrom` vs reflection pack

Question: §4.1 sketched two pack paths.  Both viable?  Which to
use?

Probe packed a `TestAllTypes{single_int32: 42}` five ways:

  1. Typed `Any::PackFrom(src)`.
  2. Pure reflection via `DynamicMessageFactory` +
     `SetString(type_url)` + `SetString(value,
     src.SerializeAsString())`.
  3. Round-trip via typed `Any::UnpackTo` against reflection-built
     bytes.
  4. `Reflection::MutableMessage(outer, single_any_fd)` +
     `dynamic_cast<google::protobuf::Any*>` + `PackFrom`.
  5. Same as 4 but pure reflection on the MutableMessage.

Findings: all five paths produce identical `type_url` and 2-byte
value payload.  Path 3 round-trip succeeds (`single_int32 == 42`).
Path 4's `dynamic_cast<Any*>` returns non-null **because the
outer message is a generated proto** — for dynamic-pool
descriptors (`DynamicMessageFactory`-backed) the dynamic_cast
would fail; reflection (path 5) works for both.

**Conclusion.**  Implementation MUST use the reflection path
(§4.1's two-`SetString` form) to stay portable across generated
and dynamic descriptor pools.

### 10.2 Probe B — Unpack error envelope

Walked 6 type_url shapes through `rfind('/') + substr` +
`DescriptorPool::generated_pool()->FindMessageTypeByName`:

| type_url | parsed FQN | descriptor lookup |
|---|---|---|
| `""` | `""` | `<null>` |
| `"TestAllTypes"` (no slash) | `"TestAllTypes"` | `<null>` |
| `type.googleapis.com/cel...TestAllTypes` | `cel...TestAllTypes` | resolved |
| `type.example.com/cel...TestAllTypes` | `cel...TestAllTypes` | resolved (prefix stripped) |
| `type.googleapis.com/com.nope.Unknown` | `com.nope.Unknown` | `<null>` |
| `x/y/cel...TestAllTypes` | `cel...TestAllTypes` | resolved (last-slash wins) |

Plus: corrupt `value` bytes against a valid descriptor →
`UnpackTo` returns `false`.  Empty Any: `type_url == ""` +
`value_size == 0` (null-read semantics).

**Pinned error envelope** (consumed by `m7a_test.cc` ::
`AnyRejectE2ETest`):

  - `type_url.empty()` → return null (NOT error).  Same as M7's
    unset-message-field rule.
  - No `/` in `type_url` → FQN parse yields whole string; lookup
    fails → `Value::Error(kFieldNotFound)`.
  - FQN not in pool → `Value::Error(kFieldNotFound)`.
  - Value bytes don't parse → `Value::Error(kTypeMismatch)`.
  - Non-`type.googleapis.com/` prefix → accepted; cel-cpp parity.

### 10.3 Probe C — `Any{...}` direct construction reach

Question: §2.1 noted "Already an Any" RHS as a path.  Does direct
Any literal construction (`google.protobuf.Any{type_url:'x',
value:b'y'}`) actually reach the runtime today?

Probe (`m7a_test.cc::AnyLiteralRoundTripE2ETest::
DirectAnyLiteralCurrentlyDynRejected` — currently PASS):
compiling `google.protobuf.Any{type_url:..., value:...}` fails at
`compiler.Compile()` with `InvalidArgument: expression is not in
the static subset: expr id=N is dyn (dyn)`.  cel-cpp's checker
types Any literals as `dyn`; v2's `RejectDyn` gate rejects.

**Implications:**

  - The "Any-as-RHS in `Foo{single_any: Any{...}}`" shape is
    unreachable today (direct Any construction is dyn-rejected).
    M7-A.A's "Already an Any" CopyFrom branch is dead code; keep
    the trivial same-descriptor handling but don't spend test
    budget on the literal-Any path.
  - The 3 `dynamic.textproto :: any/*` rows that depend on this
    shape stay SKIP unless the static-subset gate broadens.
    Independent unlock, flagged in §9.
  - The reachable shape: `Foo{single_any: TypedMessage{...}}`
    where TypedMessage is NOT `google.protobuf.Any`.  All 15
    callsite hits in §1.1 fall here.

### 10.4 Probe D — TypeUrl parse micro-perf

Bench results from `compiler_v2/bench/kernel_bench.cc`
(`BM_AnyTypeUrlParse_HappyPath` / `BM_AnyTypeUrlParse_NoSlash`,
`bazel -c opt`):

```
BM_AnyTypeUrlParse_HappyPath       10.5 ns         10.5 ns      1329535
BM_AnyTypeUrlParse_NoSlash         3.62 ns         3.62 ns      3862069
```

Order of magnitude smaller than `DescriptorPool::Find...` (100-300
ns) and `MessageFactory::GetPrototype` (>1 µs cold).  Conclusion:
no perf concern; do not cache type_url parses.

### 10.5 Assumptions challenged — `descriptor_pool` plumbing shape

Original §4.4 proposed adding a `descriptor_pool` field to
`CelHostBindings`.  Re-read of
`compiler_v2/api/engine.cc::Plan` confirmed
`BuildCelHostBindings(impl->abi, pool, impl->host_env)` already
threads the pool — but the pointer isn't stored on the bindings
struct.  Two viable shapes:

  - **Add `descriptor_pool` field to `CelHostBindings`** (the
    original §4.4 proposal).  Pros: trampoline impls reach the
    pool via `ctx.bindings.descriptor_pool`.  Cons: third
    raw-pointer field; lifetime invariant for each must hold.
  - **Pre-compute `flat_hash_map<string, const Descriptor*>` at
    Plan time.**  No good way to enumerate "every type_url the
    runtime might see" — Any unwrap can hit unprogrammed FQNs.
    Pre-computing is incomplete; rejected.

Recommendation: **option A** (raw pointer).  `DescriptorPool`
already provides O(1) lookup; pre-computing is redundant.
Lifetime is identical to M7-shipped `MessageTypeEntry.descriptor`
which is a `Descriptor*`-from-pool.

## 11. Implementation scaffolding (in tree as of 2026-05-16)

### 11.1 Test fixture extension

`compiler/testdata/host_fixture_proto3.proto::HostMsg3` does NOT
have an `Any` field.  M7-A.A's e2e tests need one before they can
run.  Two paths:

  - **Add `google.protobuf.Any single_any = 30;` +
    `repeated google.protobuf.Any repeated_any = 31;` to
    HostMsg3.**  Mirror the M3.G / M4.D pattern.
  - **Use `cel.expr.conformance.proto3.TestAllTypes` directly.**
    Adds the conformance proto's `cc_proto_library` to
    `compiler_v2/e2e/BUILD.bazel`.

Recommend option 1 (fixture extension); follows M3/M4 precedent.
M7-A.A includes the proto change as a sub-step.

Until the fixture extension lands, every M7-A test in
`compiler_v2/e2e/m7a_test.cc` skips with
`kFixtureExtensionPending`.  31 tests scaffolded; 2 PASS (probe C
dyn-rejection regression) + 29 SKIP today.

### 11.2 E2e tests — `compiler_v2/e2e/m7a_test.cc`

Comprehensive coverage matrix per §6, organised into 7 test
classes:

| Test class | Coverage | Today |
|---|---|---|
| `AnyPackE2ETest` (8 tests) | typed-message into Any across singular / repeated-arena / repeated-host / map-value / Any-as-RHS / wrapper-message / proto2-cross-syntax / null-RHS | SKIP — M7-A.A + fixture |
| `AnyUnpackE2ETest` (6 tests) | read-side unwrap on singular / activation-bound / unset / repeated / map / chained-select | SKIP — M7-A.B |
| `AnyEqualityE2ETest` (6 tests) | Any-vs-typed / typed-vs-Any / Any-vs-Any / different-type_url-unequal / unset-equals-null | SKIP — M7-A.C |
| `AnyTypeOfE2ETest` (2 tests) | `type(any.unwrapped)`; `type(direct-Any-literal)` | SKIP |
| `AnyRejectE2ETest` (5 tests) | error envelope from probe B | SKIP — M7-A.B |
| `AnyNullClearE2ETest` (2 tests) | M7-shipped null-clear regression | SKIP — fixture |
| `AnyLiteralRoundTripE2ETest` (2 tests) | **PASS** — probe C dyn-rejection invariant | PASS (active today) |

Build target: `bazel build //compiler_v2/e2e:m7a_test` (green).
Run: `./bazel-bin/compiler_v2/e2e/m7a_test` →
`31 tests from 7 test suites ran. [ PASSED ] 2 tests.`

### 11.3 Bench — collocated in `compiler_v2/bench/kernel_bench.cc`

Per the bench discipline (one binary per bench tier; kernel
microbenches live in `kernel_bench`, pipeline-shaped scenarios in
`pipeline_bench`), M7-A benches are added to `kernel_bench.cc`
not a new file.  11 `BM_*` functions added under the M7-A
section, gated by `kM7aShipped = false`.  Today only the TypeUrl
parse micros execute (no production-code dependency); all others
`state.SkipWithError(...)` cleanly.

  - `BM_AnyPack_SingularField_Reflection` — recommended path
    (probe A).
  - `BM_AnyPack_SingularField_TypedCast` — comparand.
  - `BM_AnyPack_SingularField_BaselineCopyFrom` — non-Any
    baseline.
  - `BM_AnyUnpack_SingularRead` / `_BaselineNonAny` /
    `_RepeatedAnyForEach`.
  - `BM_AnyEq_AnyVsTyped` / `_AnyVsAny` / `_BaselineNonAny`.
  - `BM_AnyTypeUrlParse_HappyPath` / `_NoSlash` (run today).

Build target: `bazel build -c opt //compiler_v2/bench:kernel_bench`
(green).  Run: `./bazel-bin/compiler_v2/bench/kernel_bench
--benchmark_filter='BM_Any'`.

Pipeline-shaped Any scenarios (`Compile + Plan + Eval` of an
expression that pack-unpacks) belong in `pipeline_bench.cc` when
M7-A ships — not added today (no production code to measure
yet).

### 11.4 WAT trace

**None required.**  M7-A introduces zero new wasm imports —
every Any operation routes through existing
`cel_host.cel_set_field` / `cel_host.cel_get_field` trampolines
(descriptor-dispatch happens inside Layer-2, not at the wasm ABI
surface).  Codegen emits the same `cel_set_field` call shape as
M7's non-Any path.  Documented explicitly to forestall "you
forgot the WAT" review feedback.

### 11.5 Production-code touchpoints (pending implementation)

For M7-A.A/B/C the touchpoints are:

  - `compiler_v2/api/internal/cel_host.cc` — add
    `AssignMessageOrPack` helper (8 callsite replacements per
    §4.1).  Add `UnpackAnyToBacking` helper.  Add
    `ProtoBacking::ReadField` Any-aware arm.  Add
    `CelMessageEqImpl` peel prelude.
  - `compiler_v2/api/internal/cel_host.h` — add
    `descriptor_pool` field to `CelHostBindings` (per §10.5: raw
    `const DescriptorPool*` pointer).
  - `compiler_v2/api/engine.cc` — pass pool pointer through
    `BuildCelHostBindings`.
  - `compiler/testdata/host_fixture_proto3.proto` — add
    `google.protobuf.Any single_any = 30;` +
    `repeated google.protobuf.Any repeated_any = 31;`.
  - `compiler_v2/conformance/binding_marshal.cc::UnpackAny` —
    consolidate with `cel_host.cc::UnpackAnyToBacking` to avoid
    double-implementation.

None of these were touched during 2026-05-16 LLD work; they're
the M7-A.A/B/C implementation scope.

## 12. Conformance inventory (M7-A target rows)

Surveyed 2026-05-16 against `tests/simple/testdata/*.textproto` (the
in-tree conformance corpus; `compiler_v2/conformance/runner.cc`
consumes the same set).  39 distinct rows across 5 fixtures, grouped
by the slice that unlocks them.  M7-A.A's pack arm unlocks rows
that exercise pack-side construction *without* selecting through
the Any (Cat 1 below).  Cat 2/3/4 require the §3.5 carve-outs +
M7-A.B/C.

**Per-slice unlock projection:**

| Slice | Unlocks | Row count |
|---|---|---:|
| M7-A.A (this slice) | Cat 1 pack-side rows where the harness reads the packed Any via `binding_marshal::UnpackAny`, not via CEL select | ~6 of 16 |
| M7-A.A + §3.5.B (admit list/map dyn into Any) | Remaining Cat 1 (heterogeneous list/map literal sources) | +~4 |
| M7-A.B + §3.5.A (frontend Any select carve-out) | Cat 2 read-side select rows + Cat 1 wrapper `*/to_any` rows | +~16 |
| M7-A.C (cel_message_eq peel) | Cat 3 equality rows (unpack-equal + bytewise-fallback) | +12 |
| §3.5.C (admit Any literal) + M7-A.B | Cat 4 (direct Any literal) + Cat 5 (var binding of Any) | +4 |

### 12.1 Cat 1 — pack-side construction (`Foo{single_any: TypedMsg{...}}`) — 16 rows

  - `proto2.textproto :: literal_wellknown :: any` — `TestAllTypes{single_any: TestAllTypes{single_int32: 1}}`.
  - `proto3.textproto :: literal_wellknown :: any` — same shape, proto3 outer.
  - `dynamic.textproto :: any :: field_assign_proto2` — pack into proto2 TestAllTypes.
  - `dynamic.textproto :: any :: field_assign_proto3` — pack into proto3 TestAllTypes.
  - `wrappers.textproto :: <kind> :: to_any` for each of 9 wrapper kinds (`bool / int32 / int64 / uint32 / uint64 / float / double / bytes / string`) — pack a wrapper-message into single_any, then read-back via `<that>.single_any`.  Read-back gated on M7-A.B + §3.5.A.
  - `wrappers.textproto :: value :: default_to_json` — pack `Value{}` JSON null.
  - `wrappers.textproto :: list_value :: literal_to_any` — pack empty `ListValue`.
  - `wrappers.textproto :: struct :: literal_to_any` — pack empty `Struct`.

### 12.2 Cat 2 — read-side select (`msg.single_any.<…>`) — 7 rows

  - `dynamic.textproto :: any :: field_read_proto2` — `TestAllTypes{single_any: TestAllTypes{single_int32: 150}}.single_any`.
  - `dynamic.textproto :: any :: field_read_proto3` — same, proto3.
  - `dynamic.textproto :: complex :: any_list_map` — `TestAllTypes{single_any: [{'almost': 'done'}]}.single_any`.
  - `proto2.textproto :: set_null :: single_any` — null clear regression.
  - `proto3.textproto :: set_null :: single_any` — same.
  - `proto2.textproto :: set_null :: repeated_field_anytype_null_retained` — `repeated_any: [1, null]` retains nulls.
  - `proto3.textproto :: set_null :: repeated_field_anytype_null_retained` — same.

### 12.3 Cat 3 — equality / inequality over packed Any — 12 rows

All in `comparisons.textproto`:

  - `eq_wrapper :: eq_proto2_any_unpack_equal` / `_not_equal` / `_bytewise_fallback_not_equal` / `_bytewise_fallback_equal`.
  - `eq_wrapper :: eq_proto3_any_unpack_equal` / `_not_equal` / `_bytewise_fallback_not_equal` / `_bytewise_fallback_equal`.
  - `ne_literal :: ne_proto2_any_unpack` / `_bytewise_fallback`.
  - `ne_literal :: ne_proto3_any_unpack` / `_bytewise_fallback`.

The "bytewise_fallback" suffix is the discriminator — same type_url +
identical wire bytes compare equal under bytewise mode; the
"unpack_equal" rows require recursive MessageDifferencer with
Any-aware mode (or our M7-A.C peel + cel_value_eq re-entry).

### 12.4 Cat 4 — direct Any literal — 3 rows

  - `dynamic.textproto :: any :: literal` — `google.protobuf.Any{type_url: '...proto2.TestAllTypes', value: b'\x08\x96\x01'}` → unpacks to proto2 TestAllTypes{single_int32: 150}.
  - `dynamic.textproto :: any :: literal_no_field_access` — `disable_check: true`; `.type_url` on a literal Any expects `no_matching_overload` eval-error.
  - `dynamic.textproto :: any :: literal_empty` — `Any{}` expects `eval_error: conversion`.

### 12.5 Cat 5 — variable binding of Any — 1 row

  - `dynamic.textproto :: any :: var` — `type_env: {x: message_type "google.protobuf.Any"}` bound to a packed proto2 TestAllTypes; expects unwrapped TestAllTypes{single_int32: 150}.

### 12.6 Not exercised today (coverage gap)

  - `type(msg.single_any)` against an unpacked descriptor — no fixture row.  M7-A.B's regression test in `m7a_test.cc::TypeOfUnpackedAny` is the only coverage; ship it as part of M7-A.B even though no conformance row drives it.

### 12.7 Inventory source

  - Generated 2026-05-16 via repo-wide grep on `google.protobuf.Any`, `single_any`, `repeated_any`, `type.googleapis.com` (filtering out non-CEL-level uses, which are mostly the textproto-Any escape syntax used in expected-value trees).
  - Re-run before M7-A.D closeout to catch corpus drift.
  - Note: this repo's conformance corpus lives at `tests/simple/testdata/` not at the originally-assumed `third_party/cel-spec/tests/...` path.
