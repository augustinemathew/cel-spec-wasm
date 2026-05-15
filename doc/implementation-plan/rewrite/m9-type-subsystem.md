# M9 — Type subsystem (`type(x)` + type identifiers as values)

Status: **shipped 2026-05-14 (slices A–F + closeout).**

> **What landed.**  All six implementation slices A–F shipped against
> the as-written plan with no architectural delta — `Value::Type` +
> `payload.s` CelSpan replaced the unused `payload.type_id` field
> (§3.3.1); `cel_type_of_at_v` 12-row primitive table + CEL_MESSAGE
> host trampoline (§4.5); `InlineTypeIdentifierReferences` rewrites
> bare type-idents to `kConstantExpr` with `Repr::kType` packing
> (§4.2); `cel_equals` CEL_TYPE arm does memcmp on `payload.s`
> bytes (§3.4); runner widened for `kTypeValue` (`CompareType`) and
> `typed_result` matcher (§4.7).  M9.G closeout — this header
> flip, testing-checklist tick, manual-tagged suite run — wraps
> the milestone.
>
> **As-shipped surface:** `m9_test.cc` (all rows green except the
> 1 GTEST_SKIP for the timestamps-slice cohort, by plan); 47/47
> default-suite + manual-tagged tests pass (the 48th is the
> deliberately-failing M10 placeholder per §"Future work").
> Conformance: live numbers move from M7 baseline as M9 unlocks
> the `type_value:` matcher cohort and the `typed_result:`
> harness path; per-fixture deltas land in the next conformance
> README refresh.

The plan covers the runtime value-of-`type` kind (`CEL_TYPE`) end-
to-end: codegen for the `type(x)` standard function, codegen for
type-identifier idents (`int`, `bool`, `string`, `null_type`,
`list`, `map`, `type`, plus message-FQN type idents), the per-Plan
type-id intern table, host primitives that produce and decode
`CEL_TYPE` values, equality of `CEL_TYPE` values, the runner
`type_value:` and `typed_result:` matchers, and the
`Value::Type(...)` factory + `cel::Value::Kind::kType` user
surface.  **Out of scope:** every spec-out-of-scope corner of the
type system named in §2.2.

## 1. Why M9

After M7.A–E + the §4.5 encoder polish + the M7 envelope/matcher
widen, conformance sits at `pass=921 / skip=843 / fail=690 /
total=2454` (37.5%).  Per the per-fixture SKIP-by-category breakdown
in `compiler_v2/conformance/README.md`, of the 843 SKIPs the largest
**scope-not-yet-shipped** bucket is the 255-row `envelope:`
category — almost entirely (~253 / 255) `type_value:` matcher rows
on `type(...)` tests like `type(true) → "bool"`.  Plus 47 rows in
`type_deduction.textproto` blocked on the related `typed_result:`
matcher (25 `check_only:` + 22 `envelope:`), plus a smaller cohort
of `kFail` rows that depend on `type(x)` machinery to reach a green
state.  Together this is the **single largest scope-not-yet-shipped
category in the corpus** — the next milestone-shaped lever after
M8 wrappers.

| Fixture | Today (PASS) | Post-M9 (estimate) | Driving slice |
|---|---:|---:|---|
| `dynamic.textproto` (`dyn(x)` rows whose value matcher is `type_value`) | 4 / 226 | +30 – +60 | M9.B + M9.E |
| `enums.textproto` (`type(...)` rows) | 46 / 85 | +5 – +12 | M9.B (incl. enum values whose runtime type is `int`) |
| `proto2.textproto` (`type(msg)` / `type(msg.field)` rows) | 55 / 118 | +6 – +12 | M9.C (message-FQN type idents) |
| `proto3.textproto` (same) | 52 / 85 | +6 – +12 | M9.C |
| `basic.textproto` (`type(x)` cohort + `[]` self-eval) | 37 / 43 | +3 – +5 | M9.B |
| `comparisons.textproto` (`type(x) == type(y)` rows) | 325 / 406 | +5 – +12 | M9.D (CEL_TYPE equality) |
| `type_deduction.textproto` (`typed_result:` matcher — no-eval check path) | 0 / 47 | +25 – +47 | M9.F (harness-only sub-slice) |
| **Total projected** | — | **+80 – +160 PASS** | — |

(Exact post-M9 counts depend on which `dyn(...)` rows the
`InlineConstantReferences` rewrite already folds — see §3.6 for
the cohort split.  Lower bound assumes only the strict
non-`dyn`-aggregate cohort lights up; upper bound assumes the
`dyn(scalar)` and `dyn(constant-message)` cohorts also reach
green via the M9.B `type(x) → CEL_TYPE` path.)

The 47 `type_deduction.textproto` rows are a special case:
they're entirely `check_only:true` with a `typed_result:` matcher
(no eval, just type comparison).  M9.F lights up the harness path
that compares the deduced static type without running Eval.  This
is mechanically separate from the runtime `CEL_TYPE` work in
M9.A–E but logically the same surface (both are "the type
subsystem"), so it lands in the same milestone.

## 2. Scope

### 2.1 In-scope (per `langdef.md` §"Type Values" + cel-cpp's `type_conversion_functions.cc`)

  - **`CEL_TYPE` runtime kind.**  The wire kind already exists at
    `compiler_v2/runtime/cel_data.h:CEL_TYPE = 11` with
    `payload.type_id` (a uint32).  M9 fills in everything that
    produces, decodes, compares, prints, and surfaces this kind:
    runtime helpers, codegen, host primitives, runner, public API.

  - **`type(x)` standard function.**  cel-cpp registers it as
    `[](const Value& v) { return TypeValue(v.GetRuntimeType()); }`
    (`type_conversion_functions.cc:464`) — i.e. read the operand's
    runtime kind and return a `CEL_TYPE` value naming it.  Spec
    coverage: `type(true)` → `bool`, `type(1)` → `int`,
    `type(1u)` → `uint`, `type(1.0)` → `double`, `type("a")` →
    `string`, `type(b"a")` → `bytes`, `type(null)` → `null_type`,
    `type([])` → `list`, `type({})` → `map`, `type(msg)` →
    `<msg-fqn>` (e.g. `"google.api.expr.test.v1.proto3.TestAllTypes"`),
    `type(timestamp(...))` → `google.protobuf.Timestamp`,
    `type(duration(...))` → `google.protobuf.Duration`,
    `type(int)` → `type` (the type of a type-value is `type`).

  - **Type-identifier idents as values.**  Per langdef §"Type
    Values": `int`, `bool`, `uint`, `double`, `string`, `bytes`,
    `null_type`, `list`, `map`, `type` used **standalone** are
    expressions of type `type` whose value is the corresponding
    `CEL_TYPE`.  cel-cpp's checker registers each as a global
    variable of type `TypeType(<inner>)` (see
    `third_party/cel-cpp/checker/standard_library.cc:799-829`
    and `TypeOfType()` / `TypeBoolType()` / etc. at lines 53–90
    of the same file).  Our `parse_and_check.cc` must recognise
    these idents and inline a CEL_TYPE-producing rewrite.

  - **Message-FQN type idents.**  `TestAllTypes`,
    `celwasm.testdata.HostMsg3`, `google.protobuf.Timestamp`, and
    so on used standalone.  The checker types these as
    `TypeType(MessageType(<descriptor>))`; our rewriter must
    emit a CEL_TYPE value whose interned name is the FQN.

  - **`CEL_TYPE` equality.**  `type(x) == type(y)`,
    `type(x) == int`, `type(int) == type(string)` etc.  Per
    langdef §"Type Values" + §"Equality": equal iff structurally
    same — for primitives compare by name; for messages compare
    by FQN.  Our codegen reaches this via the polymorphic
    `cel_equals` kernel (M5.B step 2), extended with a CEL_TYPE
    arm.

  - **`Value::Kind::kType` + `Value::Type(name)` factory + `AsType()`
    accessor** on the public surface
    (`compiler_v2/api/value.h`).  Plus `CEL_TYPE` arms in
    `Instance::Eval`'s `DecodeCelValueAt` (read-side encoder)
    and `Activation`'s `EncodeValue` (activation marshalling, so
    `Bind("t", Value::Type("bool"))` works for tests that bind a
    type variable).

  - **Runner `type_value:` matcher.**  `runner.cc::CompareValue`
    grows a `CompareType` arm matching against
    `t.value().type_value()` (a `string`).  `IsInM7Envelope`
    admits `ProtoValue::kTypeValue` so rows graduate from
    envelope-skip to comparison.  Bound type values via
    `binding_marshal::ValueFromProto` light up the `kTypeValue`
    arm so `type_env: { type: TYPE }` declarations and
    `bindings: { value: { type_value: "..." } }` rows work.

  - **Runner `typed_result:` matcher (M9.F harness-only).**
    `RunOne` grows an early-out for `check_only: true` +
    `typed_result:` rows: run the checker, decode the deduced
    type via a sibling of `binding_marshal::CelTypeFromProtoType`
    that maps the test's `deduced_type` matcher to a comparison
    value.  Lands behind a new SKIP-message-prefix-clearing path
    (the `check_only:` envelope SKIP becomes a real comparison).

### 2.2 Out-of-scope (deferred or rejected)

  - **Parameterised type values for `list<T>` / `map<K,V>`.**
    Per langdef §"Type Values" the spec defines `type(list)`
    as the bare `list` (not `list<int>`).  cel-cpp's reference
    impl returns `ListType` (i.e. `list`, no parameter); our
    `CEL_TYPE` mirrors this.  Parameterised type comparison
    (`type([1,2]) == type([3,4])`) reduces to `list == list` →
    true under both semantics; we ship the bare-name behaviour
    only.

  - **Custom abstract types** beyond `google.protobuf.Timestamp` /
    `google.protobuf.Duration`.  Spec §"Abstract Types" allows
    embedders to register new abstract type names; out of scope
    for M9.  Re-evaluate when a custom-types milestone surfaces.

  - **`type(.)` operator on a fully-qualified name** without a
    parent expression — e.g. `.int`.  cel-cpp's parser handles
    leading-dot fully-qualified resolution; M9 inherits whatever
    cel-cpp emits.  No special M9 work.

  - **`type(any_value)` of a `google.protobuf.Any`-typed value.**
    Per spec, `Any` is unpacked before comparison (langdef §"Proto
    Equality").  The unpack path lives downstream of `Any`
    construction, which is out of scope per
    `m7-proto-literals.md` §2.2.  M9 inherits the M7 boundary —
    `type(any_field)` returns the unpacked-message FQN where the
    field's type-url is resolvable, else surfaces a clean
    type-mismatch error.

  - **Run-time descriptor-pool extension via `cel_host`.**  M9's
    type-id intern table is per-Plan (resolved at
    `Engine::Plan` time).  A Plan can't grow its type-id table
    after Plan-time without re-Planning; this matches M7's
    `cel.abi.types[]` model.  Hot-loading new descriptors at
    eval time is out of scope.

  - **`dyn(x)` as a no-op rewrite for already-`type`-valued
    operands.**  M5's `IsDynPassthroughCall` admits scalar /
    null `dyn(...)` for the static-subset gate (see
    `parse_and_check.cc::ArgIsAdmissibleScalar`).  CEL_TYPE
    operands are not yet on that admission set.  Decision:
    **add CEL_TYPE to the admissible set in M9.A** (one line in
    `ArgIsAdmissibleScalar`) so `dyn(int)` / `dyn(type(x))`
    rows in `dynamic.textproto` reach the runtime kernel.  This
    is a small additive change that piggybacks on M9 without
    introducing a sibling rewriter pass.

  - **`type(x)` on `CEL_OPTIONAL` / `CEL_UNKNOWN` / `CEL_ERROR`
    values.**  Per langdef §"Error propagation" + the cel-cpp
    reference impl, `type(error)` propagates the error;
    `type(unknown)` propagates the unknown; `type(opt)` returns
    the `optional_type` abstract.  M9 ships error/unknown
    propagation through `type(...)` for free (the helper's
    epilogue handles the absorbing-kind contract via the same
    pattern every M5.F arm uses).  `optional_type` is
    optionals-pass territory and stays SKIP'd for now.

  - **Run-time deconstruction of `CEL_TYPE` values.**  Spec doesn't
    expose any operation other than equality; we don't need a
    `name(t)` / `parametersOf(t)` accessor.  `Value::AsType()`
    on the public surface is read-only.

## 3. Spec-mandated semantics

Citations from `doc/langdef.md` (source of truth per CLAUDE.md
"Testing principles") + `third_party/cel-cpp/runtime/standard/
type_conversion_functions.cc` (reference implementation) +
`third_party/cel-cpp/checker/standard_library.cc` (compile-time
type bindings).

### 3.1 The set of type names (`langdef.md` §"Values")

The spec's exhaustive list of CEL types:

| Type name | What it represents |
|---|---|
| `int`        | 64-bit signed integer |
| `uint`       | 64-bit unsigned integer |
| `double`     | 64-bit IEEE float |
| `bool`       | Boolean |
| `string`     | Unicode string |
| `bytes`      | Byte sequence |
| `list`       | List of values (bare; not `list<T>` per langdef §"Type Values") |
| `map`        | Associative array (bare; not `map<K,V>`) |
| `null_type`  | The value `null` (note: spelled `null_type`, with underscore) |
| `<message-fqn>` | Protobuf message types — the type's fully-qualified name |
| `type`       | The type of a type-value itself |
| `google.protobuf.Timestamp` | Wrapped abstract; treated as a type-name (FQN) |
| `google.protobuf.Duration`  | Same as Timestamp |

**Pinned naming.** `null_type` (underscore), `google.protobuf.Timestamp` /
`Duration` (full FQN), no parameterisation on `list` / `map`.  Test rows
in §6 cite these names verbatim.

### 3.2 `type(x)` semantics (`langdef.md` §"Type Values" + cel-cpp ref impl)

  - Strictly unary (single positional arg, no receiver form).
  - Argument is any CEL value of any kind.
  - Returns the runtime type of the argument as a `CEL_TYPE`
    value.  cel-cpp impl:
    `[](const Value& v) { return TypeValue(v.GetRuntimeType()); }`.
  - Absorbing-kind contract: error and unknown propagate through
    `type(...)` per langdef §"Error propagation".  Per cel-cpp's
    `UnaryFunctionAdapter`, the registered overload only sees
    well-typed values; the runtime's pre-call short-circuit is
    what makes this work.  Our codegen achieves the same via the
    standard slot-out helper epilogue (read input slot kind first;
    if `CEL_ERROR` or `CEL_UNKNOWN`, copy through; else dispatch).

### 3.3 Type identifier semantics (`langdef.md` §"Type Values")

> *"As types are values, those values (`int`, `string`, etc.) also
> have a type: the type `type`, which is an expression by itself
> which in turn also has type `type`."*

cel-cpp's checker registers each of the 11 spec type names + the
abstract `dyn` as a global variable whose type is `TypeType(<inner>)`.
The `Reference` for an `IdentExpr("int")` resolves to a
`VariableReference` carrying `name = "int"` but **no
`Reference::value()`** — the variable is a synthetic, with no
constant value.  Our `parse_and_check.cc::InlineConstantReferences`
only handles `Reference::has_value()` (M7.D enum-name path), so the
type-ident case is currently a hole — codegen sees a kIdentExpr
that has no `LaidOutVariable` (the variable was never declared by
the embedder), and emission falls over.

M9.A adds a sibling rewrite, **`InlineTypeIdentifierReferences`**,
that runs before `RejectDyn`:

  1. Walk the AST.
  2. For every `kIdentExpr` whose `reference_map[expr.id()]` carries
     a `VariableReference` with no `value()` AND whose checker-
     assigned type in `type_map` is `TypeType(<inner>)`:
       - Resolve the inner type's spec-name (per the §3.1 table —
         e.g. `IntType` → `"int"`, `MessageType(d)` → `d.full_name()`,
         `BoolType` → `"bool"`, `NullType` → `"null_type"`).
       - Rewrite the node in place as a `kConstantExpr` carrying
         `string_value = <name>`.  The node's `Repr` annotation,
         which is already `Repr::kType` per the existing
         `ReprOf(cel::Type)` mapping at `typed_ast.cc:97-98`,
         tells PackPass to write a **CEL_TYPE-kinded** CelValue
         into rodata (not a CEL_STRING-kinded one).

That's the entire frontend story.  No synthetic call, no overload
seed, no runtime helper for the type-ident path: a constant
type-name is just a different CelValue *shape* in rodata, and
the existing kConstant codegen arm (`EmitKConstLoad`) emits the
same `(i32.const <celvalue_offset>)` it always has.

### 3.3.1 Wire representation of `CEL_TYPE`

`cel_data.h::CelValue.payload` already has a `CelSpan s` arm
(used for `CEL_STRING`).  M9 reuses it for `CEL_TYPE`: the
payload.s field stores `{ptr, len}` of the type-name string in
linear memory.  No new payload field; no `payload.type_id`
(today's stub field is renamed and repurposed — see §4.1).

The string itself lives in one of three places, depending on
where the CEL_TYPE value originated:

  - **Rodata** for compile-time-knowable names.  PackPass writes
    type-name bytes into the same data segment as string literals;
    the rodata-resident CelValue carries the `(ptr, len)` into
    that data segment.  Source: type-ident standalones (`int`,
    `bool`, `<msg-FQN>`) AND the per-kind constant strings the
    `cel_type_of_at_v` helper writes for primitive operands.
  - **Per-Eval arena** for runtime-resolved names.  Source:
    `type(<message>)` — the host trampoline writes the descriptor's
    `full_name()` into the per-Eval arena and stamps the CelSpan
    into out_slot.  Lives only for the duration of the Eval
    (consistent with arena lifetime).
  - **Activation-supplied workspace** for `Bind("t",
    Value::Type(name))`.  The encoder copies the user-supplied
    name into the variable's workspace region.

All three are reachable from linear memory; consumers do not
need to know the source.

### 3.4 Equality of type values (`langdef.md` §"Equality")

> *"`type(1) == string` evaluates to `false` … `type(type(1)) ==
> type(string)` evaluates to `true`"*

Two `CEL_TYPE` values are equal iff their `payload.s` byte
sequences are byte-equal.  The polymorphic `cel_equals` kernel
(M5.B step 2) grows a CEL_TYPE arm: same-length + `memcmp` ==
0.  Type names in M9's scope are pure ASCII (no normalisation
nuance); a future spec ext that admits non-ASCII type names
would still be byte-comparable since cel-cpp's checker passes
through the source bytes verbatim.

This is **trivially cross-Plan robust** — there is no per-Plan
intern state to coordinate between two Plans' CEL_TYPE values.
Two CEL_TYPE values from any source compare by name, full stop.

Cross-CEL_TYPE-vs-non-CEL_TYPE equality is `false` per the spec's
heterogeneous-equality rules — `type(1) == 1` is `false`, not an
error.  The polymorphic `cel_equals` kernel already short-circuits
on kind-mismatch to `false`; M9.D adds the CEL_TYPE arm that
runs when both operands are CEL_TYPE.

### 3.5 Error / unknown propagation (`langdef.md` §"Error propagation" + §"Unknowns")

`type(error)` propagates the error; `type(unknown)` propagates
the unknown.  Implementation: the `cel_type_of` helper's prelude
short-circuits on `in_slot.kind == CEL_ERROR` or `CEL_UNKNOWN` and
copies the operand through to `out_slot`.  Same shape as every
M5.F slot-out helper.

### 3.6 `dyn(...)` interaction (cross-reference: `dyn-passthrough-plan.md`)

cel-cpp's checker types `dyn(x)` as `dyn`; our `RejectDyn` gate
admits `dyn(scalar) | dyn(null) | dyn(dyn(...))` per
`ArgIsAdmissibleScalar` at `parse_and_check.cc:447`.  M9.A extends
the admissible set:

  - `dyn(type-value)` admits — same shape as scalar, just a CEL_TYPE
    instead of CEL_INT.  Trivial one-line addition: `t.has_type()`
    arm in `ArgIsAdmissibleScalar` (`parse_and_check.cc:447`).
    No sibling Repr-level helper is involved (`IsScalarRepr`
    doesn't exist; the gate is purely at the cel-cpp type-spec
    level).
  - **Considered and rejected: `dyn(message)` admission.**  Out of
    scope per `m7-proto-literals.md` §2.2 carve-out for reflective
    introspection.  An admitted `dyn(msg)` would invite
    `dyn(msg).field` (late-bound message field reads), which today
    crashes codegen.

Many `dynamic.textproto` rows shape `type(dyn(<scalar>)) ==
<typename>` — these graduate at M9.A's `dyn(type-value)`
admission **plus** M9.B's `type(x)` codegen.

## 4. Architecture

### 4.1 Wire payload — repurpose the `payload.s` arm

`cel_data.h::CelValue` already has a `payload.s` field (`CelSpan`,
8 bytes — `{ptr, len}`) used for `CEL_STRING`.  M9 reuses it for
`CEL_TYPE` instead of inventing new wire shape:

  - **Drop** the existing stub `uint32_t type_id;` field from the
    payload union (it predated this design and was never read by
    anyone; the conformance grep confirms zero call sites).
  - **Document** in `cel_data.h`'s comment block that for `kind ==
    CEL_TYPE`, `payload.s` carries `(ptr, len)` of the type-name
    string in linear memory (rodata, per-Eval arena, or workspace,
    depending on origin — see §3.3.1).

The 24-byte `CelValue` size is unchanged; the `_Static_assert`
at `cel_data.h:137` continues to hold.

### 4.2 Codegen — `type(x)` arm + type-ident kConstant rewrite

**`type(x)` codegen.**  Today `EmitGeneralCall` routes by
`ann.overload_id`; the cel-cpp checker stamps `type` (the
kBuiltinName for `type(...)`) onto the overload id, but our
`OverloadTable` has `type` in `kExplicitlyUnimplementedIds`
(`overload_table.cc:353`).  M9.B:

  1. Remove `"type"` from `kExplicitlyUnimplementedIds` AND
     update its `std::array<absl::string_view, N>` size constant
     from 81 to 80 — the size is locked by template parameter,
     not deduced.  See `overload_table.cc:268`.
  2. Add `Seed{"type", {ImportModule::kCelRuntime, "cel_type_of_at_v"}}`
     to `kBuiltinSeeds` AND bump its size constant from 85 to 86
     (`overload_table.cc:82`).
  3. Implement `cel_type_of_at_v(out_slot, in_slot)` in
     `cel_runtime.c`.

The codegen path is the standard `EmitGeneralCall` one — no
special-case arm in `expr_lower.cc`.

**Type-ident rewrite** — `parse_and_check.cc` runs the new
`InlineTypeIdentifierReferences` pass (§3.3) AFTER the existing
`InlineConstantReferences` and BEFORE `RejectDyn`.  The
rewriter replaces a kIdentExpr with a kConstantExpr carrying
`string_value = <type-name>`.  The downstream pipeline:

  - **PopulateAnnotations** (`typed_ast.cc:170`) walks the
    type_map and stamps `Repr::kType` on the rewritten node
    (cel-cpp's checker still types it as `TypeType(...)`,
    which `ReprOf(TypeSpec)` already maps to `Repr::kType`).
    No change — already works.
  - **PackPass** (the rodata packer; see
    `compiler_v2/codegen/static_memory_builder.cc`) sees a
    `kConstantExpr` with `Repr::kType` and writes a
    `{kind: CEL_TYPE, payload.s: {ptr: <data_offset>, len}}`
    CelValue into rodata (instead of the CEL_STRING shape it
    writes for `Repr::kString`).  This is the **one new dispatch
    arm** — PackPass must dispatch on `Repr` to pick the right
    CelValue kind, not blindly assume kConst+string=CEL_STRING.
  - **`EmitKConstLoad`** is unchanged: emits `(i32.const
    <celvalue_offset>)`.  The rodata-resident CelValue's `kind`
    field tells the consumer it's a CEL_TYPE.

This collapses §3.3's "synthetic call" design into a
**zero-runtime-helper** path for type idents.  Compile-time-
knowable type names cost exactly one rodata CelValue and one
`(i32.const)` emit at codegen.

**WAT-first.**  Per CLAUDE.md "WAT-first for ABI and codegen
design", author two WAT traces under
`doc/implementation-plan/rewrite/wat/`:

  - `14_type_of_scalar.wat` — the `cel_type_of_at_v` body for an
    int operand (read in.kind, write `{CEL_TYPE, payload.s:
    rodata_span_for_int}`).
  - `15_type_ident_kconst.wat` — the rodata-CelValue layout for
    `int` standalone, plus the codegen emit (`i32.const
    <offset>`).  No host call to validate.

Walk both through `wat_runner`; `cel_type_of_at_v` is pure-runtime
(no stubs needed) and the type-ident emit doesn't even reach
wasm — it's just a rodata pre-pack that the consumer reads.

### 4.3 ResolvePass + LayoutPass extensions

  - **No new ResolvePass visitor.**  M7.A's `MessageTypeIdVisitor`
    handles `kStructExpr` for the constructable-message-type table.
    M9 adds nothing to ResolvePass: type-ident standalones rewrite
    to kConstants, which ResolvePass already handles; runtime
    `type(x)` calls route through `EmitGeneralCall`'s overload-id
    dispatch, also unchanged.  This is a direct consequence of
    dropping the type-name intern table — there is no per-AST
    "interesting type names" set to compute.

  - **PackPass dispatches on Repr for kConstants.**  Today
    PackPass packs every `kConstant` with a `string_value` as a
    `CEL_STRING`.  After M9.A: PackPass checks the node's
    `Repr` annotation; if `Repr::kType`, the packed CelValue uses
    `kind = CEL_TYPE` (payload still carries the same span — the
    raw bytes pointed at by the rodata are reusable).  See
    `static_memory_builder.cc::PackKConstant` (or the equivalent
    method).  The bytes layout for the name string itself is
    identical to the string-literal case — same data segment,
    same `(ptr, len)` accounting.

  - **LayoutPass.**  No new arms.  `cel_type_of_at_v` calls allocate
    a workspace slot via the existing kCallExpr path; rewritten
    kConstant nodes get rodata storage via the existing kConst
    path.

  - **`Repr::kType`** is **already declared** at
    `compiler_v2/ir/annotations.h:32` (predates M9).  `ReprName`
    already returns `"type"` (`annotations.cc:38`); `ReprOf(cel::Type)`
    already maps `TypeKind::kType → Repr::kType`
    (`typed_ast.cc:97-98`); `ReprOf(cel::TypeSpec)` already maps
    `type.has_type() → Repr::kType` (`typed_ast.cc:62`).  The
    activation encoder already has a `case Repr::kType:` arm at
    `instance.cc:590` returning Unimplemented.  M9.A's actual
    work is **filling in the bodies** that today return
    Unimplemented or are missing — not adding new enum values
    or new switch arms.

  - **`dyn(type-value)` admission.**  `parse_and_check.cc::
    ArgIsAdmissibleScalar` (line 447) currently returns true iff
    the arg type `has_primitive() || has_null()`.  M9.A adds a
    `t.has_type()` arm (one line).  No `IsScalarRepr` helper
    needed — the gate is at the type-spec level, not the Repr
    level.

### 4.4 ABI surface — no new ABI table

Significantly simpler than the M7 precedent: M9 adds **no rows**
to `cel_abi.proto`.  The `TypeEntry` already carries
constructable-message-type FQNs (M7.A's `cel.abi.types[]`); M9
needs no parallel intern table because:

  - Compile-time-knowable type names live in rodata as
    pre-packed CelValues (§4.2 PackPass dispatch).  Plan-time
    decode is just data-segment loading — same path as string
    literals.
  - Runtime-resolved type names (the `type(message)` path) write
    the descriptor's `full_name()` straight into per-Eval arena
    memory; nothing to intern.

Compared to the M7-style intern-table design: M9 ships **zero
new ABI rows, zero new wire schema, zero Plan-time map
construction, zero per-Instance reverse-map construction**.

The earlier draft of this doc spec'd a `cel.abi.type_names[]`
sibling table and a `cel_intern_type_name` host trampoline.
Both are dropped: the CelSpan-in-payload design makes them
unnecessary.  See "Plan-vs-execution delta" callout in §9 if
this ever flips back (e.g. if a future feature needs Plan-time
type-name resolution).

### 4.5 Host primitives — one trampoline

`compiler_v2/api/internal/cel_host.{h,cc}` grows exactly **one**
trampoline (down from the earlier draft's two):

```cpp
// Resolves the FQN of the message at `in_slot` (a CEL_MESSAGE
// CelValue whose payload.msg_slot indexes into the per-Instance
// ExternrefTable), copies the FQN into the per-Eval arena, and
// writes {kind: CEL_TYPE, payload.s: {arena_ptr, len}} into
// out_slot.  Returns CEL_ERR_TYPE_MISMATCH if in_slot isn't a
// CEL_MESSAGE; CEL_ERR_HOST_ADAPTER_ERROR if the externref
// dereference fails.
ABSL_MUST_USE_RESULT absl::Status CelHostResolveMessageTypeNameImpl(
    uint32_t out_slot, uint32_t in_slot,
    const TrampolineContext& ctx);
```

Layer-3 (`cel_host_wasmtime.cc::RegisterCelHostImports`):

```cpp
{"cel_host_resolve_message_type_name", 2,
 &CelHostResolveMessageTypeNameTrampoline},
```

`cel_runtime.c` adds the matching extern decl.

`cel_type_of_at_v(out_slot, in_slot)` is **all-runtime** for
every primitive kind — it picks among 12 pre-baked rodata strings
(see "primitive type-name table" below) and writes the matching
CelSpan into out_slot.  Only `CEL_MESSAGE` operands hop to the
host:

```c
// Pre-baked rodata: 12 type-name CelValues, one per primitive
// CelKind.  Linker emits the strings into .rodata; the CelValues
// holding the spans into .rodata too.  Indexed by CelKind.
extern const CelValue* const kPrimitiveTypeValues[];
// {CEL_NULL: &type_null_type_celvalue,
//  CEL_BOOL: &type_bool_celvalue,
//  CEL_INT:  &type_int_celvalue, ... CEL_TYPE: &type_type_celvalue}.

void cel_type_of_at_v(uint32_t out_slot, uint32_t in_slot) {
  CelValue* in = (CelValue*)((uint8_t*)__memory + in_slot);
  CelValue* out = (CelValue*)((uint8_t*)__memory + out_slot);
  if (in->kind == CEL_ERROR || in->kind == CEL_UNKNOWN) {
    *out = *in;  // propagate
    return;
  }
  if (in->kind == CEL_MESSAGE) {
    cel_host_resolve_message_type_name(out_slot, in_slot);
    return;
  }
  // Every other kind has a pre-baked rodata CelValue.
  *out = *kPrimitiveTypeValues[in->kind];
}
```

The 12-slot table costs ~300 bytes of static rodata per module
(12 × CelValue + 12 × short type-name string).  Acceptable;
amortised across every `type(<scalar>)` call.

### 4.6 Activation marshalling

  - **Bind a type value**: `Activation::Bind("t",
    Value::Type("bool"))`.  `EncodeValue` for `kType` (the
    existing-but-Unimplemented arm at `instance.cc:590`) copies
    the user-supplied name into the variable's workspace region
    of linear memory and writes `{CEL_TYPE, payload.s:
    {workspace_ptr, len}}`.  No interning, no host call.

  - **Decode a type value (read-side)**: `DecodeCelValueAt` for
    `CEL_TYPE` reads `payload.s`, copies the bytes out of
    linear memory into a `std::string`, returns
    `Value::Type(name)`.

`compiler_v2/conformance/binding_marshal.cc::ValueFromProto` grows
a `kTypeValue` arm: reads the proto's `type_value` (a string),
returns `Value::Type(name)`.

### 4.7 `cel::Value::Kind::kType` + `Value::Type(name)` factory

`compiler_v2/api/value.h`:

```cpp
enum class Kind : uint8_t {
  // Numeric values: kDuration=11 (wire CEL_DURATION=12), kTimestamp=12
  // (wire CEL_TIMESTAMP=13).  Slot 13 in the user enum is free; CEL_TYPE
  // is wire-value 11 (predates this enum).  M9.A picks 13 to keep a
  // stable user-facing layout; the user enum has always had numbering
  // independent of the wire enum (see Kind::kDuration vs CelKind::CEL_DURATION).
  kType = 13,
};
```

```cpp
static Value Type(std::string name);
absl::StatusOr<absl::string_view> AsType() const;
```

Internal payload: a `std::string` (the name) added to the existing
`std::variant` payload alternatives.  `StructurallyEquals` for
kType: equal iff `name == other.name` (byte-equality).
`ValueKindName` adds `"type"` for `kType`.

### 4.7 Conformance runner — `CompareType` arm + `IsInM7Envelope` widen

`compiler_v2/conformance/runner.cc`:

  - `CompareValue` grows `case ProtoValue::kTypeValue: return
    CompareType(got, want.type_value());`.  `CompareType`
    asserts `got.kind() == kType` and `*got.AsType() == name`.
  - `IsAggregateOrObjectMatcherKind` grows `kTypeValue`.
  - `EnvelopeRejectReason` no longer flags `kTypeValue` —
    the type_value matcher arm in the SKIP message is removed
    once M9.D admits the matcher.
  - `binding_marshal::ValueFromProto` lights up `kTypeValue`
    (string read → `Value::Type(name)`).
  - `binding_marshal::TypeSpecFragment` + `CelTypeFromProtoType`
    light up `TYPE` decls in `type_env:` → emit `type` for the
    spec parser.

The new SKIP-message taxonomy (per
`compiler_v2/conformance/README.md` "SKIP-message taxonomy"):
M9 adds no new SKIP categories — it removes the
"value matcher kind `type_value` not in scope" instance from the
existing `envelope:` category by graduating those rows to
real comparisons.

### 4.8 `typed_result:` matcher (M9.F — harness-only)

`runner.cc::RunOne` today early-outs `check_only:` to an
Unsupported.  M9.F replaces that early-out with:

  1. If `t.check_only()` and `t.result_matcher_case() ==
     SimpleTest::kTypedResult`: route through a new
     `RunCheckOnlyBranch(t, compiler)`.
  2. `RunCheckOnlyBranch`: parse + check the expression (skipping
     Plan + Eval).  Recover the deduced root-type from the
     `cel::Ast::TypeMap` at `root_expr().id()`.  Compare against
     `t.typed_result().deduced_type()` via a sibling of
     `binding_marshal::CelTypeFromProtoType` that compares two
     `cel::expr::Type` shapes structurally (descend through
     `list_type` / `map_type` / `message_type`; for primitive
     types compare the enum value).
  3. If both `result {}` AND `deduced_type {}` are present, run
     the Eval after the type comparison and additionally compare
     the runtime value (so `typed_result:` rows that ALSO carry
     a value matcher can pass on both axes).

This is mechanically separate from M9.A–E (no codegen / runtime
work), but lives in M9 because it shares the type-subsystem
surface and the same author-context (you'd otherwise pull the
M9 author back in to ship just this).

## 5. Sequencing — slices

Six slices, each shippable independently with its own test class
that turns green.  Effort sized as small (one focused change,
≤200 LoC + tests) / medium (≤500 LoC + tests + WAT trace) / large
(cross-cutting change touching ResolvePass + LayoutPass + codegen +
host + ABI + tests).

### M9.A — `CEL_TYPE` payload + `Value::Type` + activation roundtrip

Bring `CEL_TYPE` out of "wire-kind-allocated-but-unreachable"
status.  Land the public `Value::Kind::kType`, the `Value::Type(name)`
factory, the `AsType()` accessor, the `ValueKindName("type")`
mapping, the `payload.s`-as-CEL_TYPE-name convention in
`cel_data.h`, the activation encoder/decoder bodies, and the
runner widen.  No codegen for `type(x)` yet — the runtime kind
exists, can be bound through Activation, can be read out through
`Instance::Eval`, can be compared via the runner.

  - **WAT-first.**  Author `wat/14_type_of_scalar.wat` sketching
    `cel_type_of_at_v`'s pure-runtime shape (read in.kind, copy
    rodata-resident type-name CelValue into out_slot).  No host
    stubs needed.
  - **`cel_data.h`.**  Drop the unused `uint32_t type_id;` field
    from `CelValue.payload`.  Add a comment block clarifying that
    for `kind == CEL_TYPE`, `payload.s` holds `(ptr, len)` of the
    type-name string in linear memory.
  - **Public API.**  `Value::Type(name)`, `Value::AsType()`,
    `Value::Kind::kType = 13`.  Add a `std::string` alternative
    to the `Payload` variant (`value.h:200`-ish).
    `StructurallyEquals` for kType: byte-equality of names.
    `ValueKindName(kType) → "type"`.
  - **Encoder / decoder.**  Fill in the existing-but-Unimplemented
    `case Repr::kType:` arm at `instance.cc:590` (encoder writes
    name into workspace, stamps `{CEL_TYPE, payload.s}`).  Add
    a `case CEL_TYPE:` arm to `DecodeCelValueAt` (read payload.s,
    copy into `std::string`, return `Value::Type(name)`).
  - **Activation marshalling.**  `binding_marshal::ValueFromProto`
    `kTypeValue` arm: read proto string → `Value::Type(name)`.
    `CelTypeFromProtoType` for `TYPE` decls in `type_env:`.
  - **Runner.**  `IsAggregateOrObjectMatcherKind` includes
    `kTypeValue`; `CompareValue` `kTypeValue` arm dispatching
    to a new `CompareType` arm (assert `got.kind() == kType` +
    `*got.AsType() == name`).  `EnvelopeRejectReason` no longer
    mentions `type_value`.
  - **`dyn(type-value)` admission.**  `ArgIsAdmissibleScalar`
    grows a `t.has_type()` arm.  **Closing assertion of M9.A
    (R8 mitigation):** `dyn(int) == int` MUST green before the
    slice is marked done — but it depends on M9.D's equality
    arm; defer the green to M9.D and pin only the
    `ArgIsAdmissibleScalar` admit-but-no-rejection step at
    M9.A.
  - **Tests.**  `Value::Type` / `AsType` unit
    (`compiler_v2/api/value_test.cc` — round-trip + invalid-kind
    accessor + `StructurallyEquals` byte-equal + `static_assert
    static_cast<int>(Value::Kind::kType) == 13` invariant).
    Wire-encoding unit (`compiler_v2/api/instance_test.cc` —
    `Bind("t", Value::Type("bool"))` → eval returns
    `Value::Type("bool")` round-trip).  Runner unit
    (`compiler_v2/conformance/binding_marshal_test.cc` —
    `ValueFromProto` for `type_value`).
  - **Conformance unlock.**  ~+5 PASS — limited because nothing
    codegen-side produces a CEL_TYPE yet.  Activation-bound
    `Bind("t", ...)` rows do, plus the harness's typed-result
    decode.
  - **Effort.**  Large.

### M9.B — `type(x)` codegen + 12-row primitive table

Light up the `type(x)` standard function for every primitive-kind
operand.

  - **WAT-first.**  `wat/14_type_of_scalar.wat` — finalise +
    run through `wat_runner` end-to-end (no host stubs needed).
  - **OverloadTable.**  Remove `"type"` from
    `kExplicitlyUnimplementedIds` (`overload_table.cc:353`) AND
    fix the size constant (81 → 80, line 268); add
    `Seed{"type", {ImportModule::kCelRuntime,
    "cel_type_of_at_v"}}` to `kBuiltinSeeds` (size 85 → 86, line
    82).
  - **Runtime.**  `cel_type_of_at_v` body in `cel_runtime.c` —
    pre-bake the 12-row `kPrimitiveTypeValues[]` table of
    rodata CelValues (one per CelKind that's a primitive); body
    is a kind-indexed table read + slot copy.  Absorbs
    CEL_ERROR / CEL_UNKNOWN.  CEL_MESSAGE arm calls into the
    M9.C host trampoline (declared as extern; stub in `wat_runner`
    until M9.C lands the real body).
  - **Codegen.**  No `expr_lower.cc` change — routes through
    `EmitGeneralCall` automatically once the seed lands.
  - **Test matrix.**  Per `langdef.md` §"Type Values" + §3.2 of
    this doc: `type(x)` for every primitive kind × representative
    boundary values.  Parameterised in
    `m9_test.cc::TypeOfPrimitiveE2ETest` (already drafted; ~22
    rows).  Plus `type(<host-list>)` and `type(<host-map>)`
    rows that exercise the CEL_LIST_HOST / CEL_MAP_HOST paths
    via Activation binding (Gap 1 from review).
  - **Conformance unlock.**  ~+30–60 PASS in
    `dynamic.textproto` (the `type(dyn(<scalar>)) == "<name>"`
    cohort) + ~+5 in `enums.textproto` + ~+3 in
    `basic.textproto`.
  - **Effort.**  Medium.

### M9.C — `type(message)` + type-ident kConstant rewrite

Land the `type(msg)` host trampoline AND the
`InlineTypeIdentifierReferences` frontend rewrite that lets
`int` / `bool` / `<msg-FQN>` standalone produce a CEL_TYPE.

  - **WAT-first.**  `wat/15_type_of_message.wat` for the host
    trampoline call shape (operand → externref-table → Descriptor
    → arena-write FQN → out_slot CelSpan stamp).
  - **Frontend rewrite.**  `parse_and_check.cc::
    InlineTypeIdentifierReferences` pass (runs after
    `InlineConstantReferences`, before `RejectDyn`).  Walks the
    AST, finds `kIdentExpr` nodes whose `reference_map` entry
    has no `value()` AND whose `type_map` entry is
    `TypeType(...)`.  Resolves the inner type name (primitives
    by enum tag, messages by `MessageType::full_name()`).
    **Rewrites in place to a `kConstantExpr` with `string_value
    = <name>`** — no synthetic call, no overload seed.
  - **PackPass dispatch on Repr.**  PackPass
    (`static_memory_builder.cc::PackKConstant`) currently packs
    every kConstant-with-string as `CEL_STRING`.  Add a
    `Repr::kType` arm that packs the same string-bytes layout
    but stamps `kind = CEL_TYPE` on the rodata-resident
    CelValue.  This is the **only Codegen-pipeline change** for
    type-idents.
  - **Host primitive.**  `CelHostResolveMessageTypeNameImpl`
    (Layer-2) + `CelHostResolveMessageTypeNameTrampoline`
    (Layer-3 in `cel_host_wasmtime.cc`).  Reuses the
    `ExternrefTable` lookup machinery M3 established.
  - **Runtime.**  `cel_type_of_at_v`'s `CEL_MESSAGE` arm
    transitions from "stub host call" (M9.B) to real
    trampoline call.
  - **Test matrix.**  `m9_test.cc::TypeOfMessageE2ETest`
    (`type(HostMsg3{}) == celwasm.testdata.HostMsg3`),
    `TypeIdentifierExpressionE2ETest` (each of the 11 spec
    type idents standalone — `int == int`, `bool == bool`, ...,
    `HostMsg3 == HostMsg3`).
  - **Conformance unlock.**  ~+6–12 PASS in `proto2` +
    `proto3` (`type(msg)` cohort) + ~+5 in
    `comparisons.textproto` (cross-shape `type(x) == type(y)`
    rows that need both message-FQN and primitive-name).
  - **Effort.**  Medium.

### M9.D — CEL_TYPE equality + `type(x) == typename` ergonomic

Polymorphic `cel_equals` kernel (M5.B step 2) grows a CEL_TYPE
arm: equal iff `payload.s` byte sequences match (memcmp).  This
makes `type(x) == int`, `type(int) == type(string)`, `type(x) ==
type(y)` etc. work at runtime.

  - **Runtime.**  Extend `cel_equals` (in `cel_runtime.c`) with
    a CEL_TYPE arm: same-length + memcmp == 0.  Cross-kind
    (`CEL_TYPE == CEL_INT`) returns `false` (existing kind-
    mismatch short-circuit).
  - **Codegen.**  No change — `cel_equals` is the polymorphic
    dispatcher.
  - **Test matrix.**  `TypeEqualityE2ETest` (8 cases) +
    `TypeAsRhsOfEqualityE2ETest` (6 cases — `type(true) ==
    bool`, `type(1) == int`, `type(b"a") == bytes`, ...) +
    M9.A's deferred `dyn(int) == int` regression.
  - **Conformance unlock.**  ~+10–15 PASS — primarily the
    `comparisons.textproto` `type(x) == type(y)` rows whose
    operand chain reaches a CEL_TYPE on both sides.
  - **Effort.**  Small.

### M9.E — `type(null)` + `type(list)` / `type(map)` polish

Verify-and-test slice for the bare-aggregate type names.

  - `type(null)` → `null_type` (note underscore per langdef
    §"Type Values") — already covered by M9.B's all-kinds
    helper, but pin a dedicated test row.
  - `type([1,2,3])` → `list` — also covered, but pin.
  - `type({"k": 1})` → `map` — also covered.
  - `type(timestamp("2020-01-01T00:00:00Z"))` →
    `google.protobuf.Timestamp` — once timestamp construction
    is unblocked (separate slice, not M9).  Test row stays
    `GTEST_SKIP` until then.
  - **Conformance unlock.**  ~+3 PASS — the few `type(...)`
    rows specifically targeting null / list / map matchers
    that aren't covered by M9.B / M9.C.
  - **Effort.**  Small.

### M9.F — `typed_result:` matcher (harness)

Light up `type_deduction.textproto`'s 47 `check_only:` +
`typed_result:` rows.  Pure runner work — no codegen / runtime.

  - **Runner.**  `RunCheckOnlyBranch` runs `Compiler::Compile`
    only (no `Engine::Plan`, no `Eval`); recovers the deduced
    root type from `cel::Ast::TypeMap`; compares against the
    test's `deduced_type` matcher.
  - **Type-comparison helper.**  Sibling of
    `binding_marshal::CelTypeFromProtoType` that walks the
    `cel::expr::Type` recursively and asserts shape-equality
    against a `cel::Type`.
  - **Test placement.**  Lives in
    `compiler_v2/conformance/runner_test.cc` (typed_result
    matcher unit).  No m9_test.cc rows for this slice — m9_test
    asserts capabilities, not harness behaviour.
  - **Conformance unlock.**  ~+25–47 PASS in
    `type_deduction.textproto` (depends on how many rows have
    cleanly-comparable deduced-type shapes vs. shapes the
    matcher doesn't fully cover).
  - **Effort.**  Medium.

### M9.G — closeout

  - Run `bazel run //compiler_v2/conformance:run_conformance`
    and record post-M9 deltas in `compiler_v2/conformance/
    README.md`.
  - Run `scripts/run_full_suite.sh` (closeout gate per
    CLAUDE.md "manual-tagged tests carry the load-bearing
    e2e assertions").
  - Flip this doc's status header to `shipped YYYY-MM-DD` with
    the "what landed" paragraph (per CLAUDE.md "Closing out a
    planning doc").
  - Tick `testing-checklist.md` rows under "Rewrite M9":
    every new CEL-type × pipeline-stage cell M9 lit up
    (CEL_TYPE × ResolvePass / LayoutPass / codegen / runtime
    helper / Layer-2 trampoline / Layer-3 wasmtime / public
    API / activation encoder / read-side decoder / runner
    comparator).
  - Reconcile sibling docs: `cel-host-surface.md` (new
    trampolines), `design.md` §4.7 (if it referred to M9
    placeholders), `m7-proto-literals.md` §9 (the "type(...)"
    bullet under "Out-of-scope-per-plan deferrals" graduates to
    "shipped — see m9-type-subsystem.md").
  - Append a "Future work" section: parameterised
    `list<T>` / `map<K,V>` names; abstract / custom types;
    `type(any_value)` post-Any-unpack; cross-Plan CEL_TYPE
    equality robustness.

## 6. Test matrix (load-bearing)

Per CLAUDE.md "Cover the edge-case matrix — this is a compiler",
every combination below MUST have at least one explicit test
(parameterised or longhand).  Negative coverage (rejection cases)
is ≥ 30% of the total per the same rule.

### 6.1 `type(x)` positive matrix (M9.B)

| Operand kind | Source shape | Expected name |
|---|---|---|
| bool | `type(true)` | `bool` |
| bool | `type(false)` | `bool` |
| int | `type(0)` / `type(-1)` / `type(INT64_MIN)` / `type(INT64_MAX)` | `int` |
| uint | `type(0u)` / `type(UINT64_MAX)` | `uint` |
| double | `type(0.0)` / `type(-0.0)` / `type(NaN)` / `type(Infinity)` | `double` |
| string | `type("")` / `type("a")` / `type("☃")` | `string` |
| bytes | `type(b"")` / `type(b"\\x00")` | `bytes` |
| null | `type(null)` | `null_type` |
| list | `type([1, 2, 3])` / `type([])` (rejected by RejectDyn — see §6.4) | `list` |
| map | `type({"k": 1})` | `map` |
| message | `type(HostMsg3{})` / `type(HostMsg3{i32: 1})` | `celwasm.testdata.HostMsg3` |
| type | `type(int)` / `type(type)` / `type(type(1))` | `type` |
| error | `type(1/0)` propagates error | (Value::IsError) |
| unknown | `type(unknown_var)` propagates unknown | (Value::IsUnknown) |

Combinatorial size with boundaries: ~25 cases.  Parameterised
in `m9_test.cc::TypeOfPrimitiveE2ETest`.

### 6.2 Type-identifier-as-expression matrix (M9.C)

Per langdef §"Type Values", every spec type-name used standalone
is itself a CEL_TYPE value:

| Ident | Expected `name()` |
|---|---|
| `bool` | `bool` |
| `int` | `int` |
| `uint` | `uint` |
| `double` | `double` |
| `string` | `string` |
| `bytes` | `bytes` |
| `null_type` | `null_type` |
| `list` | `list` |
| `map` | `map` |
| `type` | `type` |
| `<message FQN>` | `<that FQN>` |

11 rows in `TypeIdentifierExpressionE2ETest`.

### 6.3 CEL_TYPE equality matrix (M9.D)

| LHS | RHS | Expected |
|---|---|---|
| `int` | `int` | true |
| `int` | `string` | false |
| `type(1)` | `int` | true |
| `type(1)` | `string` | false |
| `type(type(1))` | `type(string)` | true (both are `type`) |
| `type(int)` | `type` | true |
| `type(HostMsg3{})` | `type(HostMsg3{i32: 1})` | true (same FQN) |
| `type(HostMsg3{})` | `type(HostMsg2{})` | false (different FQN) |

8 rows in `TypeEqualityE2ETest`.  Plus 6 rows in
`TypeAsRhsOfEqualityE2ETest` for the `type(x) == typename` ergonomic.

### 6.4 Negative / rejection matrix (M9 §2.2 + §3 boundaries)

  - **`[]` standalone** — typed `list<dyn>` by checker; rejected by
    `RejectDyn` per M5.A.  Verify the diagnostic.
  - **`type(x, y)`** — two-arg; checker rejects (only one overload).
  - **`type` keyword used as a value type** — `1 == type` is a
    `bool == type` comparison; cel-cpp's checker rejects with a
    type error (no overload).  Verify.
  - **Reading `Value::AsType()` on a non-type Value** —
    `Value::Int(7).AsType()` returns `InvalidArgument`.
  - **Constructing `Value::Type("not_a_real_kind")`** — the public
    factory accepts any string (no validation; consistent with
    `Value::Message` accepting any FQN).  But `Bind`'ing it and
    referencing through Eval surfaces a Plan-time
    "type-name not in intern table" status — covered in
    `TypeRejectE2ETest`.
  - **Cross-kind `==`** — `int == "int"` (CEL_TYPE vs CEL_STRING)
    returns `false`, not error (per §3.4).  Pin in the test.

4–6 rows in `TypeRejectE2ETest` (depending on which checker
diagnostics survive cel-cpp version-bumps; pin the invariants
not the error strings).

### 6.5 Activation matrix (M9.A)

  - `Bind("t", Value::Type("bool"))` → `t == bool` returns true.
  - `Bind("t", Value::Type("int"))` → `t == type(7)` returns true.
  - List of types: `Bind("ts", Value::List({Value::Type("int"),
    Value::Type("string")}))` → `ts[0] == int` returns true.

3 rows in `TypeActivationE2ETest`.

### 6.6 Test placement

  - `compiler_v2/api/value_test.cc` — `Value::Type` / `AsType`
    / `StructurallyEquals` / `ValueKindName` unit.
  - `compiler_v2/api/internal/cel_host_test.cc` — Layer-2
    trampolines (`CelInternTypeNameImpl`,
    `CelHostResolveMessageTypeIdImpl`).
  - `compiler_v2/codegen/expr_lower_test.cc` — `type(x)` and
    `__type_value_of__` codegen shape (asserts the emitted
    wasm matches the WAT trace byte-for-byte).
  - `compiler_v2/api/instance_test.cc` — activation encoding
    + read-side decoding for kType.
  - `compiler_v2/conformance/binding_marshal_test.cc` —
    `ValueFromProto` `kTypeValue` arm; `CelTypeFromProtoType`
    `TYPE` arm.
  - `compiler_v2/conformance/runner_test.cc` —
    `RunCheckOnlyBranch` (M9.F).
  - `compiler_v2/e2e/m9_test.cc` (new) — the load-bearing e2e
    spec; classes per §6.1–§6.5 above.
  - `doc/implementation-plan/rewrite/wat/14_type_of_scalar.wat`
    (M9.A) + `15_type_value_of_ident.wat` (M9.C).

## 7. Risks + open questions

Ranked highest → lowest.

  - **R1 — `payload.s` lifetime in cross-Eval CEL_TYPE values.**
    The CelSpan in a CEL_TYPE points into linear memory; the
    pointed-at bytes live in rodata (compile-time constants),
    workspace (activation-bound), or per-Eval arena (host-
    resolved).  The arena-resident case has narrower lifetime
    than the others — once `cel_reset` runs at the start of
    a fresh Eval, the previous Eval's arena bytes are
    overwritable.  Mitigation: `Instance::Eval`'s read-side
    decoder COPIES `payload.s` bytes out to a `std::string`
    in `Value::Type(name)` before returning, so user code
    never holds a CelSpan into a stale arena.  Pin this in a
    test (Eval, save the returned Value, run a second Eval,
    inspect the saved Value's name).

  - **R2 — `InlineTypeIdentifierReferences` ordering vs M7.D's
    `InlineConstantReferences`.**  M7.D runs after the checker
    and inlines enum-name `Constant` values.  M9.A runs in the
    same pre-`RejectDyn` window.  They MUST run in a stable
    order: M9 must run AFTER M7.D — otherwise an enum-name
    ident whose checker-assigned type happens to be `TypeType`
    (impossible per cel-cpp's grammar, but let's be defensive)
    could be misclassified.  Mitigation: in `ParseAndCheck`,
    call `InlineConstantReferences` first, then
    `InlineTypeIdentifierReferences`.  Add a CHECK in the
    latter that `Reference::has_value() == false` to surface a
    misordering loudly.

  - **R3 — cel-cpp checker probe-spike for type-ident references.**
    The plan assumes cel-cpp emits `Reference{name="int",
    has_value=false}` for `int` standalone, with the AST node's
    type stamped as `TypeType(IntType)`.  Verified by reading
    `checker/standard_library.cc:799-829`, but not by running a
    probe.  **Probe-spike required at the start of M9.A**:
    parse + check `int == int`, dump the `reference_map` and
    `type_map`, confirm shape.  If cel-cpp emits a different
    shape (e.g. inlines the type ref into a synthetic constant),
    M9.A's rewriter target changes.

  - **R4 — `type(x)` for an empty `cel::Activation` (ident
    operand whose value never bound).**  cel-cpp behaviour:
    surfaces an `unknown_attribute` error.  Our path: PartialEval
    surfaces an `Unknown` value; the absorbing-kind contract
    in `cel_type_of_at_v` propagates it.  Should "just work" but
    pin a test row.

  - **R5 — Wire-vs-user enum numbering.**  `Value::Kind::kType =
    13` per §4.6 (because the user enum already uses 11 for
    kDuration); the wire `CEL_TYPE = 11` per `cel_data.h`.
    Future readers will trip on this.  Mitigation: add a
    one-line comment in `value.h` next to `kType = 13`
    cross-referencing `cel_data.h:CEL_TYPE = 11` and the M7
    precedent (kDuration vs CEL_DURATION = 12).

  - **R6 — Static-subset gate interaction with type-typed
    operand of `==`.**  `int == int` after the rewrite is
    `__type_value_of__(<id>) == __type_value_of__(<id>)` — a
    `kCallExpr` whose return type is `type` (CEL_TYPE).  The
    `==` overload's checker resolution gives an `equals` call
    with both operands typed `type`.  cel-cpp resolves this to
    the polymorphic `equals` (with operand type `dyn`-like in
    the resolved decl).  Our `RejectDyn` gate must not reject
    the resulting AST.  Mitigation: TypeType isn't on the
    `UnacceptableLabel` rejection list; verify by probe in
    M9.A.

  - **R8 — `dyn(type-value)` admission probe.**  After the
    `t.has_type()` arm lands in `ArgIsAdmissibleScalar`, the
    canonical regression `dyn(int) == int` MUST green.
    Mitigation: pin this as the closing assertion of M9.A
    (before any M9.B work begins).  If it red-fails, the
    `dyn`-typed result of the dyn-passthrough call is reaching
    a downstream operator that doesn't admit CEL_TYPE — a
    bigger surface than the one-line admission expected.

  - **R9 — Host-trampoline cost on `type(message)`.**  Every
    `type(msg)` call hops to the host via
    `cel_host_resolve_message_type_name` to walk the descriptor
    pool.  For an expression like
    `type(msg) == HostMsg3 || type(msg) == HostMsg2`, that's
    two host calls per Eval against an unchanging FQN.
    Mitigation tracked in §9 future work: a per-Instance
    "msg_slot → arena-resident type-name span" memoization cache
    — small bound, invalidated on `Instance::Reset()`.  Not
    blocking M9.C, but surface if a perf bench shows hot-path
    regressions.  Note this is the **only** host-trampoline cost
    in the M9 design — type-ident standalones are pure
    rodata-CelValue loads.

  - **R7 — `cel.abi.type_names[]` size bounded by per-Plan
    AST.**  Worst case: an AST that names every primitive
    type-ident + N message FQNs.  N is bounded by the AST size
    (linear).  No mitigation needed — same shape as
    `cel.abi.fields[]`.

## 8. Out-of-scope (re-stated)

  - Parameterised type values (`list<int>` / `map<string, T>`).
  - Custom abstract types beyond Timestamp/Duration.
  - `type(any_value)` post-Any-unpack (gates on `Any` packing,
    M7-future).
  - `dyn(message)` admission (carved out per
    `m7-proto-literals.md` §2.2).
  - `type(opt)` returning `optional_type` (optionals-pass
    territory).
  - Hot-loading new type names at eval time.
  - Cross-Plan CEL_TYPE equality robustness (single-Plan only;
    no fixture row crosses Plans).

## 9. Future work

(Captured pre-shipping per the M7 precedent — these become
"deferred" rows in §9 of the post-ship update if they're still
open at closeout.)

  - **Parameterised `list<T>` / `map<K,V>` type values.**  Per
    langdef §"Type Values" the spec defines bare names (no
    parameters), so this is a cel-cpp-extension item not a
    spec gap.  Surface if a fixture row demands it.

  - **`type(any_value)` after Any-unpack.**  Gates on Any-pack,
    deferred per `m7-proto-literals.md` §2.2.

  - **Optional type-of-optional.**  `optional.of(1).hasValue()`
    is optionals-pass; `type(optional.of(1))` returns
    `optional_type` per spec.  Lands with optionals-pass.

  - **Custom type registration via embedder.**  Spec admits
    new abstract types (langdef §"Abstract Types"); we don't
    expose a registration surface.  Embedder demand-driven.

  - **Per-Instance `type(message) → name` memoization** (R9).
    Every `type(msg)` host trampoline today re-walks the
    descriptor pool and re-allocates the FQN into the per-Eval
    arena.  An Instance-scoped cache (key: msg_slot, value:
    interned arena offset of the type-name) bounded by
    ExternrefTable capacity would amortise the cost across a
    tight loop.  Invalidate on `Instance::Reset()`.  Surface
    only when a bench demonstrates measurable cost.

  - **Plan-time type-name interning.**  This M9 design
    intentionally avoids ABI-level interning (§4.4).  If a
    future feature needs Plan-time type-name resolution
    (e.g. checker-driven type-name canonicalisation across
    sibling sub-expressions, or persistent plan artifacts that
    benefit from de-duplicated rodata), reintroduce the
    `cel.abi.type_names[]` table.  The wire layout (CelSpan
    payload) is forward-compatible: an interned id can be
    encoded as a span pointing into a Plan-resident name table
    without changing CelValue size.
