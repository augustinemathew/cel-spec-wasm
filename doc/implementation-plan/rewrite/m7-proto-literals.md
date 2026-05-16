# M7 — Proto message literals

Status: **shipped 2026-04-25 (slices A–E + §4.5 encoder polish + null-clear).**

> **What landed.**  M7.A–E shipped 2026-04-25:
> `kStructExpr` codegen + `cel_make_message` / `cel_set_field`
> host primitives (Layer-2 + Layer-3) + `cel.abi.types[]` ABI
> table + `OwnedProtoBacking` for owning M7-built proto messages
> + per-cpp_type scalar/repeated/map/oneof/enum/nested-message
> dispatch + `parse_and_check.cc::InlineConstantReferences`
> rewrite for cel-cpp's enum-name-as-constant resolution.
> Same-day follow-up shipped the §4.5 read-side `Instance::Eval`
> decoder for `CEL_LIST_HOST` / `CEL_MAP_HOST` and the CEL_NULL
> arm in `SetScalarField`'s CPPTYPE_MESSAGE path (clear-on-null-
> set per langdef).  Conformance: **+158 PASS** (700 → 858).
> See "Plan-vs-execution delta" callout below for the remaining
> ~+92 vs the plan's `+250` (M8 wrappers, Any, chained-null read,
> enum diagnosis).
>
> **What didn't land in M7.A–E.**  M7.F (formal closeout — see §5
> M7.F) is partially done: this doc closed out + conformance README
> refreshed.  Still pending: `testing-checklist.md` row ticks for
> the new CEL-type × pipeline-stage cells.  The §4.5 read-side
> encoder polish was identified as M7-future and not implemented;
> tracked in §9 "Future work" below.

The plan covers `kStructExpr` codegen for typed message literals
(`Foo{a: 1, b: "x"}`), the matching `cel_make_message` /
`cel_set_field` host primitives, enum literals, and the
activation-marshalling polish needed to graduate the proto fixtures.
**Wrapper-type construction (auto-wrap from scalar) and wrapper-vs-
scalar equivalence are carved out into M8 (`m8-wrapper-types.md`).**
Out of scope here: extension fields, `Any`-unpack with runtime
descriptor lookup, struct literals without `message_name`,
comprehension-driven message construction, and `Timestamp` /
`Duration` constructors — each carved out and named below.

## 1. Why M7

After M5.G + Slices 0/1/1.5/1.55/1.6 + the `eval_error` matcher,
conformance sits at `pass=700 / skip=1274 / fail=480 / total=2454`
(28.5%).  Per `compiler_v2/conformance/README.md` the dominant
remaining unlock is M7 — proto literal construction is the
biggest absolute count if everything lights up.

| Fixture | Today | Post-M7 (estimate) | Driving slice |
|---|---|---|---|
| `proto2.textproto` | 0 / 118 | 60 – 80 | M7.A–E |
| `proto3.textproto` | 0 / 85 | 50 – 65 | M7.A–E |
| `comparisons.textproto` (`kStruct` SKIPs, non-wrapper) | 287 / 406 | ~287 + 30 | M7.A |
| `enums.textproto` | 0 / 85 | 50 – 70 | M7.D |
| `basic.textproto` (aggregate / message rows) | 37 / 43 | 40 – 43 | M7.A–E |
| `fields.textproto` (`object_value` rows) | 26 / 60 | partial | M7.A + harness `object_value` decoder |
| Total projected | — | **+200 – +280 PASS** | — |

(`wrappers.textproto` and the wrapper-shaped rows in
`comparisons.eq_wrapper/*` belong to M8's count, not M7's.)

Lower bound is ~+150 if `proto2_ext` rows in non-`*_ext` fixtures
turn out to gate on extension support.

The 67 `comparisons.textproto` SKIPs are the cleanest test of M7's
landed value: the polymorphic message-equality kernel
(`cel_message_eq`, M5.B step 2b / shipped 2026-04-24) is in place
but unreachable today because no fixture row builds a message
literal.  M7.A makes the non-wrapper rows reachable; M8 makes the
wrapper rows reachable.

## 2. Scope

### 2.1 In-scope (per `langdef.md` + `design.md` §4.7)

  - **Proto message literal construction** (`kStructExpr` with
    non-empty `message_name`).  `Foo{a: 1, b: "x"}` lowers via
    the design §4.7.1 sequence: `cel_host.cel_make_message(type_id,
    out_slot)` then per-entry `cel_host.cel_set_field(msg_slot,
    field_ref_id, value_slot)`.
  - **Field setting during construction** for every non-wrapper
    field-shape cel-cpp accepts:
      - 9 scalar primitive cpp_types (BOOL / INT32 / INT64 / UINT32 /
        UINT64 / FLOAT / DOUBLE / STRING / BYTES) plus ENUM (8th cpp
        slot, treated as INT64 per langdef §"Enumerated Types").
      - Singular submessage fields (recursive `cel_make_message`).
        This includes wrapper-typed fields when the literal supplies
        an explicit wrapper-message RHS (`Foo{w: Int32Value{value: 5}}`
        — handled by recursive M7.A/B/E).  M8 handles the *scalar*
        auto-wrap RHS shape (`Foo{w: 5}`).
      - Repeated fields (RHS = list literal or list-typed expr).
      - Map fields (RHS = map literal or map-typed expr).
      - Oneof discipline (setting one arm clears any sibling).
  - **Enum literals** (`Foo.SomeEnumValue`).  cel-cpp's checker
    resolves enum-name-references to constants; codegen sees an
    int — see §3.3 for the verification a probe-spike will
    confirm.
  - **Message-typed activation bindings**: `Activation::Bind("msg",
    Value::Message(proto*))` already ships at M2.C.  M7 adds:
    list-of-message bindings, plus the activation marshalling for
    repeated / map fields read out of bound messages.  (Wrapper-
    typed activation bindings — auto-wrap from scalar — are M8.)

### 2.2 Out-of-scope (deferred)

  - **Wrapper types and wrapper equivalence** (`google.protobuf.
    Int32Value` and friends).  Carved out into M8 — see
    `m8-wrapper-types.md`.  The split is along a clean seam:
    M7 lands struct-construction + non-wrapper field sets +
    enum + nested + repeated/map; M8 adds the wrapper-as-scalar
    auto-wrap path (construction-side and activation-side) plus
    the wrapper-vs-scalar `==` peel.  Note: explicit
    wrapper-message construction (`Foo{w: Int32Value{value: 5}}`)
    is *not* deferred — it falls out of M7's recursive
    `kStructExpr` lower for free.
  - **Struct literals without `message_name`** (`design.md` §4.7.4).
    These are CEL parser sugar for map literals and lower through
    §4.7.2 today (M3, shipped).  No M7 work.
  - **Extension fields** (`msg.[ext_name]`).  `proto2_ext.textproto`
    is gated on this.  Carved out as a follow-up milestone.
  - **Custom functions with message-typed signatures.**  M6
    territory (custom function registration).
  - **Comprehension-driven message-of-message construction**
    (`xs.map(x, Foo{n: x})` building messages).  Comprehension
    follow-on; depends on M5's comprehension surface fully
    landing first.
  - **`google.protobuf.Timestamp` / `Duration` constructors via
    string-formatted `Timestamp{...}`.**  Timestamps slice; the
    proto fixtures don't gate on this.
  - **`google.protobuf.Any` packing / unpacking** with descriptor
    pool lookup at runtime.  Decision: **out of scope for M7.**
    Rationale: `Any` requires a side-channel descriptor pool
    accessible at evaluation time (`cel_host` would need to expose
    a `MessageFactory*`-shaped trampoline beyond `cel_make_message`),
    cel-cpp's coverage is itself partial, and no top-50 conformance
    fixture row depends on Any literal construction.  Re-evaluate
    after M7 lands if a corpus row surfaces.
  - **`type(msg)` / `dyn(msg)` reflective introspection.**  Lives
    with `dyn(...)` passthrough follow-ons.

## 3. Spec-mandated semantics

Citations from `doc/langdef.md` (the source of truth per CLAUDE.md
"Testing principles") and `third_party/cel-cpp/runtime/standard/
{equality,conversion}_functions.cc` (the reference implementation).

### 3.1 Proto message construction (langdef §"Protocol Buffer Data Conversion")

  - Construction `Foo{...}` requires `Foo` to be a registered
    descriptor in the `CompileOptions::descriptor_pool` reachable
    from `parse_and_check.cc`.  cel-cpp's checker rejects
    references to unknown descriptors with `InvalidArgument`; M7
    inherits this — no codegen-time check needed.
  - Each entry's field-name must resolve to a `FieldDescriptor`
    on the type.  Same — checker-rejected before codegen.
  - Field type compatibility is checker-enforced (`int32` field
    accepts `int` literals; `int64` accepts the same; `string`
    field rejects `int`, etc.).  Codegen trusts the checker.

### 3.2 Default-value rules — entirely descriptor-driven (langdef §"Field Selection")

Defaults are **a descriptor-driven, eval-time concern, fully
shipped at M2/M3.**  M7 inherits this without writing any
default-value code:

  - **Proto3 singular scalar**: an unset field reads as the type
    default (`""`, `0`, `false`).  Construction `Foo{}` produces
    a message whose every singular scalar reads as the default.
  - **Proto3 singular message**: read returns `null` if unset.
  - **Proto2 singular field**: explicit-default per descriptor;
    read returns that default.  `has(msg.f)` returns `true` only
    if the field was explicitly set.

These are read-side rules and already shipped (M2/M3) — every
read goes through `Reflection::GetInt64` / `GetString` / etc.,
which honours the descriptor's proto2 vs proto3 default
semantics for free.  M7's construction path produces a message
via `MessageFactory::GetPrototype(desc)->New()` — i.e. a
default-constructed proto — and applies only the fields the
literal names.  Proto's own default-construction handles the
rest; no ABI-level default encoding is needed.

**Considered and rejected: encoding defaults into a per-field
ABI table.**  Would duplicate `FieldDescriptor::default_value_*`
data the descriptor pool already exposes, drift if proto
semantics ever evolve, and add surface for zero benefit.  The
descriptor pool is the source of truth at eval time; it lives on
`Instance` from M2.C onward.  M7's eval-time path goes:
`cel_make_message(type_id) → MessageFactory::GetPrototype(desc)
->New()` — the resulting message already obeys all proto2 vs
proto3 default rules without M7 writing any defaults code.

The descriptor pool **is** also needed at compile time, but only
by cel-cpp's checker (already wired through
`CompileOptions::descriptor_pool`).  M7's ResolvePass uses it only
to intern type-ids and field-ref ids — not to read defaults.

### 3.3 Enum semantics (langdef §"Enumerated Types")

  - Enum values are spec-typed as `int`.  `Foo.SomeEnumValue` resolves
    at the checker to a `Constant` `int64_value` carrying the
    declared numeric value.  **Probe-spike required at the start of
    M7.D** — if the checker emits a `kSelect` (operand =
    `IdentExpr("Foo")`, field = `"SomeEnumValue"`) instead of a
    `Constant`, codegen needs an enum-aware kSelect arm; if it
    emits a `Constant`, M7.D is a one-liner.  cross-reference
    `third_party/cel-cpp/checker/internal/type_check_env.cc`'s
    enum-resolution paths.
  - `int(Foo.SomeEnumValue)` and `Foo.SomeEnumValue == 1` work
    transparently because the value is already `int`.
  - Enum-typed proto field set: `Foo{e: Bar.VALUE}` — the RHS is an
    int after checker resolution; `cel_set_field` on an enum field
    reads the int and writes via `Reflection::SetEnumValue`.

### 3.4 Repeated, map, oneof

  - **Repeated**: `Foo{xs: [1, 2, 3]}` — RHS is a `kListExpr`
    (Repr::kArenaList).  `cel_set_field` on a repeated field
    iterates the list and appends each element via
    `Reflection::AddInt64` / `AddBool` / `AddString` / etc.
    Backing wrap (`HostList` from `Activation::Bind`) is also
    accepted — same iteration via `HostListBacking::At`.
  - **Map**: `Foo{m: {"k": "v"}}` — RHS is a `kMapExpr`
    (Repr::kArenaMap).  `cel_set_field` on a map field iterates
    the map and inserts each entry via the proto map-entry
    sub-message pattern.
  - **Oneof**: setting one field of a oneof clears its siblings.
    Proto reflection does this automatically on
    `Reflection::Set...`; M7.B's `cel_set_field` body inherits
    the right behaviour for free.  Test coverage required to
    pin it.

## 4. Architecture

### 4.1 Codegen — `kStructExpr` arm in `expr_lower.cc`

Today's `expr_lower.cc:871` returns `Unimplemented` for
`kStructExpr`.  Replace with the design §4.7.1 sequence:

```cpp
case cel::ExprKindCase::kStructExpr: {
  const cel::StructExpr& s = expr.struct_expr();
  ABSL_CHECK(!s.name().empty())
      << "kStructExpr with empty name should have lowered as kMapExpr "
         "in earlier pipeline stage (design.md §4.7.4); reached codegen "
         "with id=" << expr.id();
  // ann->message_type_id is stamped at LayoutPass; resolves to a
  // descriptor at Plan time (see §4.2 below).
  std::vector<BinaryenExpressionRef> stmts;
  // 1. cel_host.cel_make_message(type_id, out_slot).
  stmts.push_back(EmitMakeMessage(ctx, ann->message_type_id,
                                  ann->storage.slot));
  // 2 + 3. For each entry: lower value into its own scratch slot,
  //        then cel_host.cel_set_field(msg_slot, field_ref_id,
  //        value_slot).
  for (const cel::StructExpr::Entry& entry : s.entries()) {
    BinaryenExpressionRef value_emit;
    ASSIGN_OR_RETURN(value_emit, Emit(ctx, entry.value()));
    stmts.push_back(value_emit);
    const NodeAnnotation& v_ann = ctx.layout.AnnotationFor(entry.value());
    stmts.push_back(EmitSetField(ctx, ann->storage.slot,
                                 entry.field_ref_id, v_ann.storage.slot));
  }
  return BinaryenBlock(/*name=*/nullptr, stmts.data(), stmts.size(),
                       BinaryenTypeNone());
}
```

Two helper emitters added to `expr_lower.cc` (private free functions):

  - `EmitMakeMessage(ctx, type_id, out_slot)` — emits a single
    `(call $cel_host.cel_make_message ...)` with the two i32 args.
  - `EmitSetField(ctx, msg_slot, field_ref_id, value_slot)` —
    emits a single `(call $cel_host.cel_set_field ...)` with three
    i32 args.

### 4.2 ResolvePass + LayoutPass extensions

  - **ResolvePass**: a new visitor `MessageTypeIdVisitor` runs in
    the post-order resolve pass.  For each `kStructExpr` node with
    non-empty `message_name`, it interns `message_name` against
    a new `cel.abi.types[]` table (parallel to `cel.abi.fields[]`,
    same FQN-resolved-at-Plan-time pattern).  The interned id is
    stamped on the node's `NodeAnnotation::message_type_id`.
    Field-ref ids for each entry are interned through the existing
    `cel.abi.fields[]` table (already populated for `kSelect`
    field reads at M2 — re-use it without schema changes; the
    resolved `FieldDescriptor*` carries the cpp_type that
    `cel_set_field` dispatches on at runtime).
  - **LayoutPass**: `AggregateStorageVisitor::PostVisitStruct`
    allocates a workspace slot for the constructed message
    (`Storage::kWorkspaceSlot`).  Per-entry value slots are
    allocated by the recursive visit; M7 doesn't change that.
    Release-after-set discipline matches the kCreateMap /
    kCreateList pattern: the entry-value slot is released after
    the `cel_set_field` consumes it.  `*_origin` annotation:
    `Origin::kHost` (messages live host-side regardless of how
    constructed).

### 4.3 ABI surface — new tables

One additive change to `cel.abi` (a new `TypeEntry` table on the
`CelAbi` top-level message), matching the existing `FieldEntry`
shape from `compiler_v2/abi/cel_abi.proto`:

```proto
// One row of the message-type intern table — M7.A.  Populated
// when ResolvePass first encounters a kStructExpr; consumed by
// Engine::Plan to resolve `fully_qualified_name` against the
// process-wide DescriptorPool, exactly as FieldEntry.owner_fqn
// is resolved today.
message TypeEntry {
  uint32 id = 1;                      // dense; 0 is sentinel.
  string fully_qualified_name = 2;    // e.g. "google.api.expr.test.v1.proto3.TestAllTypes".
}

// In CelAbi:
//   repeated TypeEntry types = 5;    // M7.A.
```

**No descriptor handle on the wire.**  `FieldEntry` already
demonstrates the pattern: it stores `owner_fqn` (a string) and
`Engine::Plan` resolves the `Descriptor*` against the
`DescriptorPool` held on `CompileOptions` at load time.
`TypeEntry` follows the same rule — the descriptor pool is the
single source of truth at Plan time; embedding a handle would
duplicate that pointer in two places and be a drift hazard if
the pool is rebuilt between Compile and Plan.

`cel.abi.fields[]` is **not** extended for M7.  The existing
`FieldEntry` row (`id`, `field_number`, `name`, `owner_fqn`)
already carries everything `cel_set_field` needs at runtime —
the `Reflection::Set...` dispatch reads `cpp_type` directly off
the resolved `FieldDescriptor*` (no need to encode it on the
wire), and read-vs-write is purely a function of which trampoline
the codegen emits, not metadata.

The ABI bytes are described in `cel-host-surface.md` §6; M7
appends a new sub-section there for `cel.abi.types[]`.

**Defaults are intentionally not in the ABI.**  See §3.2 — the
descriptor pool is the source of truth at eval time, reached
through `Reflection`.  Duplicating defaults into `cel.abi`
would be redundant and a drift hazard.

### 4.4 Host primitives — new Layer-2 trampolines

`compiler_v2/api/internal/cel_host.{h,cc}` grows two functions
matching the existing `CelGetFieldImpl` / `CelHasFieldImpl` shape:

```cpp
// Allocates a default-constructed Message of the given type and
// interns it into the per-Instance ExternrefTable; writes
// {kind:CEL_MESSAGE, payload.msg_slot=<id>} into out_slot at the
// provided MemoryView.
ABSL_MUST_USE_RESULT absl::Status CelMakeMessageImpl(
    uint32_t type_id, uint32_t out_slot,
    const TrampolineContext& ctx);

// Reads the CelValue at value_slot, decodes per the field's
// FieldDescriptor::cpp_type, and sets the field on the message at
// msg_slot via Reflection.  Repeated fields iterate the source list
// (Repr::kArenaList | kHostList accepted); map fields iterate the
// source map (Repr::kArenaMap | kHostMap accepted).  Mismatches
// return non-OK Status and the trampoline surfaces an
// InvalidArgument trap to wasm.
ABSL_MUST_USE_RESULT absl::Status CelSetFieldImpl(
    uint32_t msg_slot, uint32_t field_ref_id,
    uint32_t value_slot,
    const TrampolineContext& ctx);
```

Layer-3 — `cel_host_wasmtime.cc` `RegisterCelHostImports` table grows
two rows:

```cpp
{"cel_make_message", 2, &CelMakeMessageTrampoline},
{"cel_set_field",    3, &CelSetFieldTrampoline},
```

`CelMakeMessageTrampoline` / `CelSetFieldTrampoline` follow the
existing `CelGetFieldTrampoline` shape — unwrap wasmtime args,
borrow the memory view, call the Layer-2 impl, surface non-OK
Status as a wasm trap.

`compiler_v2/runtime/cel_runtime.c` adds two
`__attribute__((import_module, import_name))` extern declarations
so Binaryen sees the imports during link.

### 4.5 Activation marshalling — non-wrapper polish

  - **List-of-message bindings**: today `EncodeList` in
    `compiler_v2/api/instance.cc` iterates a list and encodes each
    element via the existing recursive `EncodeValue`.  Verify that
    `EncodeValue` for `kMessage` works — it should, since
    `kMessage` ships at M2.C — and add a regression test if any
    proto-typed list binding exercises a missing path.
  - **Map-typed bindings** (`Activation::Bind("m", Value::Map(...))`):
    today `EncodeValue` for `kMap` is a stubbed kind in the
    encoder per `compiler_v2/conformance/README.md`'s outstanding-
    activation-marshalling list ("Still SKIP at the encoder: kMap,
    kDuration, kTimestamp, kEnum, kType, kUnknown").  M7 doesn't
    have to ship `kMap` — but if `proto2.textproto` rows include a
    map binding, M7.C pulls it in.  Pin the matrix during M7.A's
    probe-spike and decide.

(Wrapper-typed activation auto-wrap is M8.)

## 5. Sequencing — slices

Six slices, each shippable independently.  Effort sized as small
(one focused change, ≤200 LoC + tests) / medium (≤500 LoC + tests
+ WAT trace / probe spike) / large (cross-cutting change touching
ResolvePass + LayoutPass + codegen + host + ABI + tests).

### M7.A — `kStructExpr` admission + `cel_make_message`

Bring `kStructExpr` codegen out of `Unimplemented`.  Build a
default-constructed proto and return it as a `kMessage` CelValue.
No field-set yet — `Foo{}` works, `Foo{a: 1}` rejects with
`Unimplemented` from the entry loop.

  - **WAT-first.**  Author `doc/implementation-plan/rewrite/wat/
    11_kstruct_make_message.wat` showing the
    `cel_host.cel_make_message(type_id, out_slot)` call shape.
    Stub `cel_make_message` in `wat_runner` that allocates a
    `TestAllTypes` proto and writes a fake msg_slot.  Document in
    `wat-traces.md`.
  - **ABI.**  Extend the ABI envelope with `cel.abi.types[]`.
    Update `cel-host-surface.md` §6.
  - **ResolvePass.**  `MessageTypeIdVisitor` populates type id
    table + stamps `NodeAnnotation::message_type_id`.
  - **LayoutPass.**  `AggregateStorageVisitor` handles
    `kStructExpr` — workspace slot for the message.
  - **Codegen.**  `expr_lower.cc:871` emits
    `cel_host.cel_make_message(type_id, out_slot)` for empty
    `Foo{}`; entry-loop body is ABSL_CHECK-stub until M7.B.
  - **Host primitives.**  Layer-2 `CelMakeMessageImpl` + Layer-3
    trampoline registration + `cel_runtime.c` extern decl.  Layer
    -2 takes `type_id`, looks up the descriptor, allocates via
    `MessageFactory`, interns into `ExternrefTable`, writes the
    msg_slot CelValue into the linear-memory `out_slot`.
  - **Tests.**  Layer-2 unit (`cel_host_test`); codegen unit
    (`expr_lower_test`); end-to-end (`m7_test.cc::ProtoLiteralE2ETest`)
    covering `TestAllTypes{}.single_int32 == 0` (proto3 zero default)
    and `Customer{}.name == ""` and proto2 explicit-default rows.
  - **E2E check.**  `proto2.textproto` rows that just construct
    an empty message graduate (~10 rows expected).
  - **Conformance unlock estimate.**  +10 PASS, +5 FAIL→PASS.
  - **Effort.**  Large.

### M7.B — `cel_set_field` for scalar fields

Per-cpp_type dispatch in `CelSetFieldImpl`.  Codegen entry-loop
body emits one `cel_set_field` per literal entry.

  - Scalar cpp_types: BOOL / INT32 / INT64 / UINT32 / UINT64 / FLOAT
    / DOUBLE / STRING / BYTES / ENUM (10 paths in the dispatch
    switch; the `default:` arm is `ABSL_CHECK(false)` per
    CLAUDE.md).
  - **Test matrix per cpp_type.**  Set / unset / boundary
    (`INT64_MIN`, `INT64_MAX`, `UINT64_MAX`, empty string,
    embedded NUL, multi-byte UTF-8) → 24 cases × 10 cpp_types =
    240 parameterised rows.  Consolidated into a single TEST_P
    in `m7_test.cc::ProtoLiteralScalarE2ETest`.
  - **Conformance unlock estimate.**  +60 PASS in `proto2` /
    `proto3` (every scalar-only row graduates).
  - **Effort.**  Medium.

### M7.C — repeated + map fields

`cel_set_field` iterates list / map sources and inserts each
element via `Reflection::Add...` / proto map-entry pattern.

  - **Repeated**: source = `Repr::kArenaList | kHostList`.  Per
    cpp_type dispatch (matches M7.B's switch).
  - **Map**: source = `Repr::kArenaMap | kHostMap`.  Iterate via
    the M3 / M4 backing interfaces.
  - **Cross-coordinates with M5.D step 2's host-list / host-map
    dispatcher.**  Both of those land before M7.
  - **Test matrix.**  8 repeated-of-primitive cases + 4 map-key-
    kinds × 8 map-value-kinds = 32 cases + 5 mixed-arena/host
    crossover cases.
  - **Conformance unlock estimate.**  +20 PASS in `proto2` /
    `proto3` repeated/map rows.
  - **Effort.**  Medium.

### M7.D — enum literals

Probe-spike first to confirm cel-cpp checker behaviour for
`Foo.SomeEnumValue`.  Two outcomes:

  - **If checker emits `Constant(int64)`**: M7.D is a one-test
    sanity check that nothing rejects the constant at the
    static-subset gate.
  - **If checker emits `Select(Ident(Foo), "SomeEnumValue")`**:
    add an enum-aware kSelect arm in `expr_lower.cc` that resolves
    the operand to a descriptor + looks up the field's enum value.
  - **Test matrix.**  3 enum field cases (set / unset / round-trip
    via select).  Plus enum-as-RHS-of-set: `Foo{e: Bar.VALUE}` →
    int → `Reflection::SetEnumValue`.
  - **Conformance unlock estimate.**  +50 PASS in `enums`.
  - **Effort.**  Small (probably) or medium (if checker emits
    select).

### M7.E — message field nesting

`Foo{nested: Bar{...}}` — the entry-value emit is itself a
recursive `kStructExpr` lower.  Should "just work" once M7.A–B
are in, because the codegen arm is recursive by construction.
M7.E is a verify-and-test slice.

This slice also covers the case `Foo{w: Int32Value{value: 5}}`
where `w` is a wrapper-typed field and the RHS is an *explicit*
wrapper-message literal.  No special-case codegen needed — the
nested `Int32Value{value: 5}` is just another `kStructExpr`
lower.  (Scalar-RHS-into-wrapper-field auto-wrap, e.g.
`Foo{w: 5}`, is M8.A.)

  - **Test matrix.**  5 nested-message rows: deep nest (3 levels);
    nested with scalar siblings; nested with repeated sibling;
    nested message in a list literal (`[Foo{}, Foo{}]`);
    explicit-wrapper-as-nested (`Foo{w: Int32Value{value: 1}}`).
  - **Conformance unlock estimate.**  +10 PASS across `proto2` /
    `proto3` nested rows; final clean-up of `comparisons` 67-SKIP
    cohort (non-wrapper portion).
  - **Effort.**  Small.

### M7.F — closeout

  - Run `bazel run //compiler_v2/conformance:run_conformance` and
    record the post-M7 deltas in `compiler_v2/conformance/README.md`.
  - Run `scripts/run_full_suite.sh` (the closeout gate per CLAUDE.md
    "manual-tagged tests carry the load-bearing e2e assertions").
  - Flip `design.md` §4.7.1 / §4.7.5 row status to "shipped".
  - Flip Slice 9 in §11.5 to "shipped" for the proto-construction
    portion (wrapper rows stay open under M8).
  - Flip this doc's status header to `shipped YYYY-MM-DD` with the
    "what landed" paragraph.
  - Tick `testing-checklist.md` rows under "Rewrite M7": every
    new CEL-type × pipeline-stage cell that M7 lit up.
  - Append M7's "Future work" section (extensions, Any, Timestamp,
    comprehension-in-message; defer wrapper follow-ups to M8).
  - Reconcile sibling docs (`cel-host-surface.md`, the ABI
    surface, the M5 plan if it cited M7 placeholders, the M8 plan
    if its "depends on M7" notes need updating).

## 6. Test matrix (load-bearing)

Per CLAUDE.md "Cover the edge-case matrix — this is a compiler",
every combination below MUST have at least one explicit test
(parameterised or longhand).  Negative coverage (rejection cases)
is ≥ 30% of the total per the same rule.

### 6.1 Field-set positive matrix

| Dimension | Values | Count |
|---|---|---|
| cpp_type (scalar) | bool / int32 / int64 / uint32 / uint64 / float / double / string / bytes / enum | 10 |
| Boundary value | `0` / `-1` / type-min / type-max / empty / embedded-NUL / multi-byte UTF-8 (string only) | up to 7 |
| Proto syntax | proto2 explicit-default / proto2 explicit-set / proto3 set / proto3 zero-default | 4 |
| Source operand | literal / ident / computed expr | 3 |

Combinatorial size is ~840; the parameterised TEST_P collapses
structurally-identical cells to a per-cpp_type × per-boundary table
(~70 rows).

### 6.2 Aggregate-set matrix

| Field shape | Source | Element kinds covered |
|---|---|---|
| Repeated of scalar | list literal | every cpp_type |
| Repeated of message | list literal | nested `Foo{}` |
| Map<string, scalar> | map literal | all 8 scalar cpp_types as values |
| Map<int, scalar> | map literal | 4 numeric value kinds |
| Map<bool, scalar> | map literal | 2 element kinds |
| Map<uint, scalar> | map literal | 2 element kinds |
| Repeated bound from activation | host-list | scalar + message elements |
| Map bound from activation | host-map | scalar values |
| Oneof | sequential set of two siblings | proto2 + proto3 |

### 6.3 Default-value semantic regression matrix (read-side; M2/M3 paths re-asserted)

Every M7-constructed message must, when read back through the
M2/M3 read paths, observe the spec's proto2 vs proto3 default
rules.  Tests live in `m7_test.cc` (regression of M2/M3 paths
under M7-constructed messages):

  - proto3 zero-default scalar: `TestAllTypes{}.single_int32 == 0`.
  - proto3 unset message: `TestAllTypes{}.single_nested_message ==
    null`.
  - proto2 explicit-default (e.g. a field with `[default = 42]`):
    `Customer{}.discount == 42`.
  - proto2 has() on unset: `has(Customer{}.discount) == false`.
  - proto2 has() on explicitly-set-to-default:
    `has(Customer{discount: 42}.discount) == true`.

These rows confirm the construction-path doesn't accidentally
shortcut around Reflection.

### 6.4 Negative / rejection matrix

  - Unregistered descriptor: `Unknown{x: 1}` → checker rejects.
    Verify the static-subset gate's diagnostic is the cel-cpp one.
  - Unknown field on known descriptor: `Foo{not_a_field: 1}` →
    checker rejects.
  - Type mismatch: `Foo{int32_field: "string"}` → checker rejects.
  - Repeated field set with non-list source: `Foo{xs: 1}` →
    checker rejects, but if checker types it as `dyn` and the
    static-subset admits it, codegen rejects with `Unimplemented`
    + `ABSL_CHECK(false) << "..."`.
  - Map field set with key-kind mismatch — checker rejects.
  - Oneof: setting two siblings, read both → only the second is
    set.

### 6.5 Test placement

  - `compiler_v2/api/internal/cel_host_test.cc` — Layer-2
    `CelMakeMessageImpl` + `CelSetFieldImpl` parameterised tables
    (per cpp_type, per boundary).
  - `compiler_v2/codegen/expr_lower_test.cc` — kStructExpr
    lowering shape (asserts the emitted wasm matches the WAT
    trace byte-for-byte modulo Binaryen-assigned names).
  - `compiler_v2/api/instance_test.cc` — activation marshalling for
    list-of-message and map bindings.
  - `compiler_v2/e2e/m7_test.cc` (new) — every conformance-row-
    shape, parameterised against the matrix above, plus the §6.3
    default-value regression rows.
  - `doc/implementation-plan/rewrite/wat/11_kstruct_make_message
    .wat` (M7.A) + `12_kstruct_set_scalar.wat` (M7.B) +
    `13_kstruct_repeated_map.wat` (M7.C).

## 7. Risks + open questions

Ranked highest → lowest.

  - **R1 — Descriptor-pool lifetime under `Engine::Plan`.**
    `cel.abi.types[]` resolves at Plan time against the descriptor
    pool held on `CompileOptions`.  M2.C established that the pool
    must outlive the Plan; M7 inherits that constraint and adds
    type_id rows that hold weak descriptor pointers.  Mitigation:
    a Plan-construction CHECK that the pool is non-null + an
    instance-side CHECK before each `cel_make_message` call that
    the resolved descriptor is still live.  If a corpus row
    exercises a multi-pool / shared-pool shape (unlikely; the
    conformance harness loads one pool), surface during M7.A's
    probe.
  - **R2 — Oneof clear-on-set semantics.**  Proto reflection
    handles this automatically via `Reflection::Set...`, but
    untrusted: behaviour under `ClearField` + `SetField` ordering
    is subtle.  Mitigation: dedicated test in M7.C; assert that
    `Foo{a_oneof_a: 1, a_oneof_b: 2}.a_oneof_a` returns
    field-default (i.e. `b` won), and `has(...a_oneof_a)` returns
    `false`.
  - **R3 — proto2 explicit-default vs proto3 implicit-zero rules
    in the test matrix.**  Test rows that hard-code "default
    value" need to know which syntax the descriptor came from.
    Defaults themselves are not in our code path (§3.2) — we
    rely on protobuf's own default-construction + Reflection —
    but the *test expectations* still need to know what a given
    descriptor's default is.  Mitigation: maintain separate
    proto2 and proto3 test descriptor sets in
    `compiler_v2/testdata/`; tag every row with its syntax
    source.
  - **R4 — Enum checker emit shape**.  Probe-spike at start of
    M7.D (§3.3).  Mitigation: write the spike before any M7.D
    code; if the checker emits a `kSelect`, M7.D grows in scope
    by one codegen arm.
  - **R5 — `cel.abi` table extension breaks on-disk plan
    artifacts** (if any are persisted).  Verify: today plans are
    in-memory only, so this is purely a forward concern.  If a
    future milestone ships persistent plan artifacts, the ABI
    schema needs a version bump.
  - **R6 — Cross-pool descriptor resolution under
    `SchemaProtoSource` vs `SchemaDescriptorSet`.**  Both are
    supported at M3; M7 should not regress either.  Mitigation:
    verify both schema-source modes exercise M7 in the e2e
    suite.

## 8. Out-of-scope (re-stated)

  - **Wrapper types** — see `m8-wrapper-types.md`.  (Explicit
    wrapper construction `Foo{w: Int32Value{value: 5}}` falls
    out of M7.E for free; auto-wrap from scalar and the
    equivalence peel are M8.)
  - Extension fields (`msg.[ext_name]`) — `proto2_ext.textproto`
    follow-up.
  - `Any` packing / unpacking — separate slice.
  - Struct literals without `message_name` — already shipped as
    map literals at M3.
  - Custom functions with message-typed signatures — M6.
  - Comprehension-driven message construction — comprehension
    follow-on.
  - `Timestamp` / `Duration` constructors — separate timestamps
    slice.
  - `type(msg)` / `dyn(msg)` reflective introspection.

## 9. Future work

Captured post-shipping to close the +131 vs +250 plan-estimate gap.
Each bullet names a concrete unblocking work item, the rows it
graduates, and the milestone-or-slice that picks it up.

### Immediate unblockers (small scope, high yield)

  - **§4.5 read-side encoder polish** — **shipped** (+10 PASS).
    Added `DecodeHostListAt` + `DecodeHostMapAt` in
    `compiler_v2/api/instance.cc`; thread `ExternrefTable&` through
    the recursive decoder; new `CEL_LIST_HOST` / `CEL_MAP_HOST`
    arms in `DecodeCelValueAt`.  Walks the per-Instance backing
    via `ForEach`, wraps elements in fresh vector-backed
    `Value::List` / `Value::Map`.  Graduated `empty_field/
    repeated_*` and `empty_field/map` rows in proto2/proto3 (+8)
    plus 2 in enums.

  - **Null-clear on singular message field set** — **shipped**
    (+17 PASS).  Added `value.kind == CEL_NULL → ClearField`
    arm in `SetScalarField`'s CPPTYPE_MESSAGE path so
    `Foo{m: null} == Foo{}` per langdef + cel-cpp behaviour.
    Graduated `set_null/single_message`, `set_null/single_any`,
    `set_null/single_duration`, `set_null/single_timestamp`
    rows in each of proto2/proto3 (8 rows × 2 fixtures + spillover).

  - **Chained-null read fix** (~+2 PASS, *not yet shipped*).
    M7.A added the `!HasField(msg, &field) → Value::Null()` arm
    in `ProtoBacking::ReadField` for the immediate read
    (`{}.inner == null` → `true`).  But chained selects through
    the unset message — `TestAllTypes{}.single_nested_message.bb`
    — still return `CEL_ERROR` because `CelGetFieldImpl`'s
    prelude rejects `msg_cv.kind != CEL_MESSAGE`.  cel-cpp's
    behaviour is null-propagation-with-default-instance: select
    on an unset-message returns the leaf field's default value
    via the descriptor.  Fix is more involved than a simple
    null arm — needs the descriptor of the leaf-most field
    threaded through.  Tracked separately.

  - **`testing-checklist.md` row ticks** (~0 PASS but
    closeout-blocking).  Per CLAUDE.md "Closing out a planning
    doc" §5: tick every CEL-type × pipeline-stage cell M7 lit up.
    Rows to flip: kStructExpr × ResolvePass, kStructExpr ×
    LayoutPass, kStructExpr × codegen, kStructExpr × Layer-2
    trampoline, kStructExpr × Layer-3 wasmtime; per-cpp_type
    field-set rows (10 cpp_types × set-arm); repeated/map/oneof/
    enum/nested rows.

### Next milestone: M8 (separate doc)

  - **M8 wrappers** (~+50–60 PASS).  Wrapper-vs-scalar `==` peel,
    auto-wrap on construction (`Foo{w: 5}`), auto-wrap on
    activation bind (`Bind("w", Value::Int(5))` against an
    `Int32Value`-typed slot).  See `m8-wrapper-types.md`.  The
    M7.A wrapper-message construction path admits wrapper
    literals at parse time (`wrappers.textproto` rows graduated
    from envelope-skip to compile-FAIL), so M8 has a clean
    surface to land on.

### Out-of-scope-per-plan deferrals (still future work)

  - **`Any` pack/unpack** — **shipped at M7-A** (2026-05-16, +7
    PASS).  `WriteMessageOrPack` replaces the M7 descriptor-mismatch
    guards on the pack side; `UnpackAnyToValue` in
    `ProtoBacking::ReadField` handles read-side unwrap;
    `PeelAnyForEq` in `CelMessageEqImpl` handles direct-Any-literal
    equality.  See `m7a-any.md`.

  - **Extensions support** (`proto2_ext.textproto` graduation).
    Adds a host import `cel_get_extension(out_slot, msg_slot,
    extension_id)` plus an extension-id intern table.  Part of
    the broader extensions-pass milestone — see conformance
    README.

  - **`cel.abi.types[]` versioning**.  When persistent plan
    artifacts ship, the ABI table format gains a version bump.

  - **Multi-pool / shared-pool descriptor resolution.**  M7
    assumes one pool per Plan; if a future milestone introduces
    multi-pool resolution, the type-id table needs a pool-id
    column.

  - **Dynamic-descriptor `MessageFactory`.**  M7's
    `CelMakeMessageImpl` uses
    `MessageFactory::generated_factory()` which knows about
    statically-linked `cc_proto_library` descriptors only.
    Schemas loaded via `SchemaProtoSource` / `SchemaDescriptorSet`
    at compile time would need a `DynamicMessageFactory` pinned
    to the per-Plan descriptor pool.  Surfaced as `kTypeMismatch`
    today (clean failure mode) — unlock when a fixture row
    requires it.

  - **`Timestamp` / `Duration` literal construction**.  Surfaces
    after M7 lands; small scope.

### Plan-vs-execution delta (numbers)

| Bucket | Plan estimate (§1 table) | Actual M7.A–E | Δ vs estimate |
|---|---:|---:|---:|
| `proto2.textproto` | 60–80 | 29 | −31 to −51 |
| `proto3.textproto` | 50–65 | 26 | −24 to −39 |
| `comparisons.textproto` | ~317 | 334 | +17 to +47 |
| `enums.textproto` | 50–70 | 20 | −30 to −50 |
| `basic.textproto` | 40–43 | 37 | −3 to −6 |
| `wrappers.textproto` | (M8 — not in M7 estimate) | 0 | — |
| **Total** | **+200 to +280** | **+131** | **−69 to −149** |

The shortfall is concentrated in `proto2`/`proto3`/`enums` and
maps cleanly to the §4.5 encoder polish (~+15–25), M8 (~+50–60),
chained-null (~+2), Any (~+5–8), summing to ~+72–95 — closing
the gap to within the plan's lower-bound estimate once the
follow-ups land.  `comparisons` overshot because the M7.A
empty-construction path made the message-eq kernel reachable
faster than the plan's "+30" line item assumed.
