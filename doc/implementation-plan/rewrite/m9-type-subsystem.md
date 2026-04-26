# M9 — Type subsystem (`type(x)` + type identifiers as values)

Status: **plan — drafted 2026-04-25, not yet started.**

> **What "done" looks like.**  Greening every test in
> `compiler_v2/e2e/m9_test.cc` (TypeOfPrimitive ×11 boundaries,
> TypeOfAggregate ×4, TypeOfMessage ×4, TypeOfNull, TypeIdentifier
> ×11, TypeEquality ×8, TypeAsRhsOfEquality ×6, TypeReject ×4,
> TypeActivation ×3) plus the conformance unlock targets in §1.

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
| `dynamic.textproto` (`dyn(x)` rows whose value matcher is `type_value`) | 9 / 226 | +30 – +60 | M9.B + M9.E |
| `enums.textproto` (`type(...)` rows) | 46 / 85 | +5 – +12 | M9.B (incl. enum values whose runtime type is `int`) |
| `proto2.textproto` (`type(msg)` / `type(msg.field)` rows) | 55 / 118 | +6 – +12 | M9.C (message-FQN type idents) |
| `proto3.textproto` (same) | 52 / 85 | +6 – +12 | M9.C |
| `basic.textproto` (`type(x)` cohort + `[]` self-eval) | 37 / 43 | +3 – +5 | M9.B |
| `comparisons.textproto` (`type(x) == type(y)` rows) | 334 / 406 | +5 – +12 | M9.D (CEL_TYPE equality) |
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
       - Replace the node with a synthetic
         `kCallExpr(__type_value_of__, [Constant(string=name)])`
         that codegen lowers to a CEL_TYPE write.

The synthetic call uses overload id `__type_value_of__` (a private
codegen helper, not a user-visible function — the underscore prefix
matches the convention of `__not_strictly_false__` from M5.I).
That overload routes through a new `cel_make_type_value(out_slot,
name_slot)` helper in `cel_runtime.c`.

**Why a synthetic call rather than a special `kConstant` storage
kind.**  Two reasons:

  - The CEL `Constant` proto's payload union has no slot for a
    type-value (only bool / int / uint / double / string / bytes /
    null per `cel/expr/syntax.proto`).  Encoding a CEL_TYPE as a
    `string_value` and adding a sidecar storage-kind annotation
    would mean codegen has to remember the kind via a side-channel,
    ResolvePass has to populate that side-channel, and the kConst
    arm has to dispatch on it.  Three points of synchronisation
    failure.
  - The synthetic-call shape reuses the existing kCallExpr +
    OverloadTable + cel_runtime.c machinery wholesale.  One new
    seed in `kBuiltinSeeds`, one new helper body, no synchronisation
    burden.

### 3.4 Equality of type values (`langdef.md` §"Equality")

> *"`type(1) == string` evaluates to `false` … `type(type(1)) ==
> type(string)` evaluates to `true`"*

Two CEL_TYPE values are equal iff their interned names are equal.
Implementation choice (§4.3): keep the names interned per-Plan, and
implement equality as integer comparison of the interned ids — the
table is fixed at Plan time so any two CEL_TYPE values that came
from the same Plan can be compared by id, and cross-Plan equality
is undefined per the implementation contract (no fixture row
crosses Plan boundaries).  An optional fallback for cross-Plan
robustness (string compare) is left for a future milestone if it
ever surfaces.

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
    arm in `ArgIsAdmissibleScalar`, and the `kType` Repr is added
    to `IsScalarRepr` in `compiler_v2/ir/repr.h` (see §4.2).
  - **Considered and rejected: `dyn(message)` admission.**  Out of
    scope per `m7-proto-literals.md` §2.2 carve-out for reflective
    introspection.  An admitted `dyn(msg)` would invite
    `dyn(msg).field` (late-bound message field reads), which today
    crashes codegen.

Many `dynamic.textproto` rows shape `type(dyn(<scalar>)) ==
<typename>` — these graduate at M9.A's `dyn(type-value)`
admission **plus** M9.B's `type(x)` codegen.

## 4. Architecture

### 4.1 Codegen — `type(x)` arm in `expr_lower.cc`

Today `EmitGeneralCall` routes by `ann.overload_id`; the cel-cpp
checker stamps `type` (the kBuiltinName for `type(...)`) onto the
overload id, but our `OverloadTable` has `type` in
`kExplicitlyUnimplementedIds` (`overload_table.cc:353`).  M9.B:

  1. Remove `"type"` from `kExplicitlyUnimplementedIds`.
  2. Add `Seed{"type", {ImportModule::kCelRuntime, "cel_type_of_at_v"}}`
     to `kBuiltinSeeds`.
  3. Implement `cel_type_of_at_v(out_slot, in_slot)` in
     `cel_runtime.c`.

The codegen path is the standard `EmitGeneralCall` one — no
special-case arm in `expr_lower.cc`.

For the type-identifier-rewrite synthetic call (§3.3), add a second
seed:

  - `Seed{"__type_value_of__", {ImportModule::kCelRuntime, "cel_make_type_value_at_v"}}`.

`cel_make_type_value_at_v(out_slot, name_slot)` reads a `CEL_STRING`
at `name_slot`, looks up the interned id in the per-Plan table
(via the `cel_host.cel_intern_type_name` trampoline), and writes
`{kind: CEL_TYPE, payload.type_id: <id>}` into `out_slot`.

**WAT-first.**  Per CLAUDE.md "WAT-first for ABI and codegen
design", author two WAT traces under
`doc/implementation-plan/rewrite/wat/`:

  - `14_type_of_scalar.wat` — the `cel_type_of_at_v` body for an
    int operand (write CEL_TYPE with payload.type_id = id-of-`int`).
  - `15_type_value_of_ident.wat` — the rewrite-target synthetic
    call for `int` standalone.

Walk both through `wat_runner` with stub `cel_intern_type_name`
returning a fixed id, validate end-to-end before any expr_lower
work.

### 4.2 ResolvePass + LayoutPass extensions

  - **ResolvePass (`compiler_v2/codegen/resolve_pass.cc`).**
    `MessageTypeIdVisitor` (M7.A) covers message FQNs from
    `kStructExpr`; M9.A adds **`TypeIdVisitor`**, post-order, that
    interns:
       - one row per primitive type name actually referenced in the
         AST (`int`, `bool`, ...) — into `cel.abi.type_names[]` (a
         new table, see §4.3);
       - one row per message FQN referenced as a type-identifier
         ident (e.g. `TestAllTypes` standalone) — also into
         `cel.abi.type_names[]`, sharing the same id-space as the
         primitives.

    Reuses `cel.abi.types[]` (M7's table) is **rejected** — M7's
    table is keyed on a constructable message type and resolves at
    Plan time to a `Descriptor*`.  M9 needs a table keyed on a CEL
    type-name string with no descriptor-handle requirement, and
    that holds primitive names like `"int"` that aren't proto
    types.  Sibling table is the right separation; same
    `Engine::Plan` decode pattern.

    The `TypeIdVisitor` runs **after** `InlineTypeIdentifierReferences`
    (which is a frontend rewrite, not a ResolvePass step).  By the
    time ResolvePass walks the tree, every type-identifier ident has
    already been rewritten to a synthetic call carrying a string
    constant — so what ResolvePass sees is just rodata strings, the
    same shape M2 already handles.  The intern-into-`type_names[]`
    step happens at the synthetic call site: the visitor recognises
    `__type_value_of__` calls, reads the constant string operand,
    interns it, and stamps the resolved id onto the call node's
    annotation.

  - **LayoutPass.**  No new arms.  Synthetic calls allocate a
    workspace slot via the existing kCallExpr path; the string
    operand is rodata-packed by the existing kConst path.  The
    `cel_type_of_at_v` call (real `type(x)`) likewise allocates a
    workspace slot via the existing kCallExpr path.

  - **`Repr::kType`** added to `compiler_v2/ir/repr.h`.  Mirrors
    `Repr::kMessage` shape — small, scalar-ish, fixed-size.
    `IsScalarRepr` returns true for `kType` (per §3.6).  Variable
    declarations of type `TYPE` get `Repr::kType` per the existing
    `ReprOf(...)` dispatch.

### 4.3 ABI surface — new `type_names[]` table

Additive to `compiler_v2/abi/cel_abi.proto`:

```proto
// One row of the type-name intern table — M9.A.  Populated when
// ResolvePass first encounters a synthetic `__type_value_of__`
// call (carrying a primitive type-name) OR a `kStructExpr` (M7.A —
// reuses the existing `cel.abi.types[]` table for the message FQN).
//
// The two tables overlap intentionally on message FQNs:
//   - `cel.abi.types[]` (M7.A) carries FQNs that are constructable
//     message types — resolved to a `Descriptor*` at Plan time.
//   - `cel.abi.type_names[]` (M9.A) carries every CEL type-name
//     that appears as a runtime CEL_TYPE value — primitives,
//     `null_type`, `list`, `map`, `type`, message FQNs, abstract
//     names like `google.protobuf.Timestamp`.  Decoded at runtime
//     by `cel_intern_type_name` to a uint32 id; the reverse is
//     stored on Instance for the `Instance::Eval` decoder to
//     produce a string out the read-side encoder.
//
//   id      dense index; 0 is the sentinel "no type-name id".
//   name    e.g. "int", "google.api.expr.test.v1.proto3.TestAllTypes",
//           "null_type".
message TypeNameEntry {
  uint32 id = 1;
  string name = 2;
}

// In CelAbi (additive — field number 6, locked):
//   repeated TypeNameEntry type_names = 6;
```

`Engine::Plan` builds two parallel maps from `type_names[]`: an
`id → string` map for the read-side encoder, and a
`string → id` map for the runtime trampoline (`cel_intern_type_name`).

**Why not extend `TypeEntry` with a `kind` field?**  Considered.
`TypeEntry` carries an FQN keyed on "this is a constructable
message type — resolve a Descriptor at Plan time".  Adding a
`kind=primitive` discriminator would force every consumer of
`TypeEntry` (today only Plan-time descriptor resolution) to filter
on the discriminator, and the descriptor-resolution code would
have to skip primitive rows.  Sibling table keeps each table's
contract sharp and avoids retrofitting M7's consumers.

### 4.4 Host primitives — Layer-2 + Layer-3

`compiler_v2/api/internal/cel_host.{h,cc}` grows one trampoline:

```cpp
// Looks up `name_slot` (a CEL_STRING) in the per-Instance type-name
// intern map and writes {kind: CEL_TYPE, payload.type_id: <id>}
// into out_slot.  Returns CEL_ERR_TYPE_MISMATCH if name_slot
// isn't a CEL_STRING; returns a fresh kError if the name isn't in
// the intern table (which is a Plan-time invariant violation —
// an unrenamed assertion we leave loud).
ABSL_MUST_USE_RESULT absl::Status CelInternTypeNameImpl(
    uint32_t out_slot, uint32_t name_slot,
    const TrampolineContext& ctx);
```

Layer-3 (`cel_host_wasmtime.cc` `RegisterCelHostImports`):

```cpp
{"cel_intern_type_name", 2, &CelInternTypeNameTrampoline},
```

`compiler_v2/runtime/cel_runtime.c` adds the matching extern decl.
The `cel_type_of_at_v(out_slot, in_slot)` and
`cel_make_type_value_at_v(out_slot, name_slot)` bodies live in
`cel_runtime.c` (pure-runtime — no host call needed for the body
of `type_of`, just a `kind` read and a slot write; the
host-trampoline call is only for `cel_make_type_value`'s string-
to-id lookup).

`cel_type_of_at_v` is **all-runtime** because the type-id of the
operand's runtime kind is statically known per kind.  The runtime
helper does:

```c
void cel_type_of_at_v(uint32_t out_slot, uint32_t in_slot) {
  CelValue* in = (CelValue*)((uint8_t*)__memory + in_slot);
  CelValue* out = (CelValue*)((uint8_t*)__memory + out_slot);
  if (in->kind == CEL_ERROR || in->kind == CEL_UNKNOWN) {
    *out = *in;  // propagate
    return;
  }
  // Look up the kind's primitive type-id from the per-Plan
  // primitive-id map (see below).  For CEL_MESSAGE, dispatch
  // to a host trampoline that resolves the message's FQN to
  // an id.
  if (in->kind == CEL_MESSAGE) {
    // Host call: cel_host_resolve_message_type_id(out_slot, in_slot).
    cel_host_resolve_message_type_id(out_slot, in_slot);
    return;
  }
  out->kind = CEL_TYPE;
  out->_pad = 0;
  out->payload.type_id = primitive_type_id_for(in->kind);
}
```

Two host trampolines vs one:

  - `cel_intern_type_name(out, name)` — for the
    `__type_value_of__` rewrite path (operand is a string
    constant in rodata; resolve-once-then-cache shape).
  - `cel_host_resolve_message_type_id(out, in)` — for the
    `type(message)` runtime path (operand is a CEL_MESSAGE; need
    the descriptor pool to resolve FQN → id at runtime).  Reuses
    the `ExternrefTable` lookup machinery M3 established.

`primitive_type_id_for(kind)` is a pure-runtime constant table:
the M9.A ResolvePass guarantees that for every primitive kind in
the AST, the corresponding type-name is interned with a stable
small id.  We pre-populate the table for **every** primitive kind
unconditionally (12 rows: int, uint, double, bool, string, bytes,
null_type, list, map, type, google.protobuf.Timestamp, google.protobuf.Duration),
so the runtime helper doesn't need to know whether a given
primitive name appears in the AST — the per-Plan id is always
present.  Cost: 12 × 24 bytes ≈ 300 bytes per Plan.  Acceptable.

### 4.5 Activation marshalling

  - **Bind a type value**: `Activation::Bind("t",
    Value::Type("bool"))`.  `EncodeValue` for `kType` resolves the
    name through the intern map and writes
    `{CEL_TYPE, type_id: <id>}` into the variable's slot.

  - **Decode a type value (read-side)**: `Instance::Eval`'s
    `DecodeCelValueAt` for `CEL_TYPE` reads the type_id, looks up
    the name through the per-Instance reverse-intern map, returns
    `Value::Type(<name>)`.

`compiler_v2/conformance/binding_marshal.cc::ValueFromProto` grows
a `kTypeValue` arm: reads the proto's `type_value` (a string),
returns `Value::Type(name)`.

### 4.6 `cel::Value::Kind::kType` + `Value::Type(name)` factory

`compiler_v2/api/value.h`:

```cpp
enum class Kind : uint8_t {
  // ...
  kType = 13,  // wire CEL_TYPE = 11; user-surface kType = 13
              // (the user-facing enum has independent numbering;
              // kDuration = 11 already).
};
```

```cpp
static Value Type(std::string name);   // Construct from name.
absl::StatusOr<absl::string_view> AsType() const;
```

Internal payload: a `std::string` (the name).  No interning at the
`cel::Value` level — interning is a per-Plan ABI concern, not a
user-surface one; user code hands names around as strings.  The
type-id appears only inside the wire `CelValue`.

`StructurallyEquals` for kType: equal iff the names are byte-equal.

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

### M9.A — `CEL_TYPE` runtime kind end-to-end + `Value::Type`

Bring `CEL_TYPE` out of "wire-kind-allocated-but-unreachable" status.
Land the public `Value::Kind::kType`, the `Value::Type(name)`
factory, the `AsType()` accessor, the `ValueKindName("type")`
mapping, the `Repr::kType` IR mirror, the `cel.abi.type_names[]`
ABI table, and the per-Instance intern map.  No codegen for
`type(x)` yet — the runtime kind exists, can be bound through
Activation, can be read out through `Instance::Eval`, can be
compared via the runner.

  - **WAT-first.**  Author `wat/14_type_of_scalar.wat` sketching
    the `cel_type_of_at_v` shape (operand-kind read + out-slot
    write).  Stub `cel_intern_type_name` in `wat_runner`.
  - **ABI.**  Add `TypeNameEntry` + `repeated TypeNameEntry
    type_names = 6` to `cel_abi.proto`.  Update
    `cel-host-surface.md` §6.
  - **Runtime.**  Add `Repr::kType` to `repr.h`; extend
    `IsScalarRepr` to include `kType`; extend `ReprOf` to map
    `cel::TypeType` → `Repr::kType`.  Define
    `primitive_type_id_for` in `cel_runtime.c` (12-row table).
  - **Public API.**  `Value::Type(name)`, `Value::AsType()`,
    `Value::Kind::kType` (independent numbering — kType = 13;
    kDuration is at 11 in the user-facing enum).  Update
    `value.cc` payload (`std::string` arm).  `StructurallyEquals`
    for kType.
  - **Encoder / decoder.**  `EncodeValue` for `kType` writes
    `{CEL_TYPE, type_id}`.  `DecodeCelValueAt` for `CEL_TYPE`
    reads back through the per-Instance reverse map.
  - **Activation marshalling.**  `binding_marshal::ValueFromProto`
    `kTypeValue` arm.  `CelTypeFromProtoType` for `TYPE`.
  - **Runner.**  `IsAggregateOrObjectMatcherKind` includes
    `kTypeValue`; `CompareValue` `kTypeValue` arm dispatching
    to `CompareType`.  `EnvelopeRejectReason` no longer mentions
    `type_value`.
  - **`dyn(type-value)` admission.**  `ArgIsAdmissibleScalar`
    grows a `t.has_type()` arm.
  - **Tests.**  `Value::Type` / `AsType` unit
    (`compiler_v2/api/value_test.cc` — round-trip + invalid-kind
    accessor + `StructurallyEquals`).  Wire-encoding unit
    (`compiler_v2/api/instance_test.cc` — `Bind("t",
    Value::Type("bool"))` → eval `t == t` returns true).  Runner
    unit (`compiler_v2/conformance/binding_marshal_test.cc` —
    `ValueFromProto` for `type_value` returns
    `Value::Type(name)`).
  - **Conformance unlock.**  ~+5 PASS — limited because no
    code path produces a CEL_TYPE without M9.B (`type(x)`).
    The wins here are the activation rows that
    `Bind("t", Value::Type(...))` → `t == int` shape.
  - **Effort.**  Large.

### M9.B — `type(x)` codegen + per-kind helper

Light up the `type(x)` standard function for every primitive-kind
operand.

  - **WAT-first.**  `wat/14_type_of_scalar.wat` — already drafted
    in M9.A; finalise + run through `wat_runner` end-to-end.
  - **OverloadTable.**  Remove `"type"` from
    `kExplicitlyUnimplementedIds` (`overload_table.cc:353`);
    add `Seed{"type", {ImportModule::kCelRuntime,
    "cel_type_of_at_v"}}` to `kBuiltinSeeds` (size 85 → 86).
  - **Runtime.**  `cel_type_of_at_v` body in `cel_runtime.c` —
    one switch over `in->kind`, write `{CEL_TYPE, primitive_id}`
    into out.  Absorb CEL_ERROR / CEL_UNKNOWN.
  - **Codegen.**  No `expr_lower.cc` change — routes through
    `EmitGeneralCall` automatically once the seed lands.
  - **Test matrix.**  Per `langdef.md` §"Type Values" + §3.2 of
    this doc: `type(x)` for x = each of bool / int / uint /
    double / string / bytes / null / list / map / type / error /
    unknown.  12 cases, parameterised in
    `m9_test.cc::TypeOfPrimitiveE2ETest`.  Plus boundary rows
    for each numeric kind (INT_MIN/MAX, UINT_MAX, +0.0/-0.0,
    NaN — all return the same type-id, but the test asserts
    boundary stability).
  - **Conformance unlock.**  ~+30–60 PASS in
    `dynamic.textproto` (the `type(dyn(<scalar>)) == "<name>"`
    cohort) + ~+5 in `enums.textproto` + ~+3 in
    `basic.textproto`.
  - **Effort.**  Medium.

### M9.C — `type(message)` + message-FQN type idents

Land the `type(msg)` path that resolves the message's descriptor
FQN at runtime, and the `MessageFqn` standalone-ident path that
produces a CEL_TYPE for the FQN at compile time.

  - **WAT-first.**  `wat/15_type_of_message.wat` for the host
    trampoline call shape.
  - **Frontend rewrite.**  `parse_and_check.cc::
    InlineTypeIdentifierReferences` pass.  Walks the AST, finds
    `kIdentExpr` nodes whose `reference_map` entry has no
    `value()` AND whose `type_map` entry is `TypeType(...)`.
    Resolves the inner type name (per §3.1 table — primitives
    by enum tag, messages by `MessageType::full_name()`).
    Replaces with synthetic `__type_value_of__(string)` call.
  - **OverloadTable.**  Add `Seed{"__type_value_of__",
    {ImportModule::kCelRuntime, "cel_make_type_value_at_v"}}`.
  - **Host primitives.**  `CelInternTypeNameImpl`,
    `CelMakeTypeValueImpl` (the runtime body's host call),
    `CelHostResolveMessageTypeIdImpl` (the `type(message)`
    runtime body's host call).  Trampolines wired through
    `cel_host_wasmtime.cc::RegisterCelHostImports`.
  - **ResolvePass.**  `TypeIdVisitor` recognises
    `__type_value_of__` calls, interns the constant string,
    stamps the resolved id.
  - **Runtime.**  `cel_make_type_value_at_v` and the message-
    resolution arm of `cel_type_of_at_v`.
  - **Test matrix.**  `m9_test.cc::TypeOfMessageE2ETest`
    (`type(HostMsg3{}) == "celwasm.testdata.HostMsg3"`),
    `TypeIdentifierExpressionE2ETest` (each of the 11 spec
    type idents standalone — `int == int`, `bool == bool`, ...,
    `HostMsg3 == HostMsg3`).
  - **Conformance unlock.**  ~+6–12 PASS in `proto2` +
    `proto3` (`type(msg)` cohort) + ~+5 in
    `comparisons.textproto` (cross-shape `type(x) == type(y)`
    rows that need both message-FQN and primitive-FQN).
  - **Effort.**  Medium.

### M9.D — CEL_TYPE equality + `type(x) == typename` ergonomic

Polymorphic `cel_equals` kernel (M5.B step 2) grows a CEL_TYPE
arm: equal iff `payload.type_id` matches.  This makes
`type(x) == int`, `type(int) == type(string)`, `type(x) ==
type(y)` etc. work at runtime.

  - **Runtime.**  Extend `cel_equals` (in `cel_runtime.c`) with
    a CEL_TYPE arm.  Cross-kind (`CEL_TYPE == CEL_INT`)
    returns `false` (existing behaviour, just a sanity-check
    test).
  - **Codegen.**  No change — `cel_equals` is the polymorphic
    dispatcher.
  - **Test matrix.**  `TypeEqualityE2ETest` (8 cases) +
    `TypeAsRhsOfEqualityE2ETest` (6 cases — `type(true) ==
    bool`, `type(1) == int`, `type(b"a") == bytes`, ...).
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

  - **R1 — Per-Plan vs. per-Instance lifetime of the type-name
    intern table.**  M7's `cel.abi.types[]` is decoded into the
    Plan; descriptor pool resolution happens once.  M9's
    type-name table is the same shape — decoded at Plan time.
    But the *reverse* map (id → name, used by the read-side
    decoder) lives on `Instance`, not `Plan`, because one Plan
    can drive many Instances and the decoder runs per-Eval.
    Mitigation: ResolvePass + ABI emit produce the table once;
    Plan resolves it; Instance copies (or borrows under
    shared_ptr) the resolved map.  Pin this in the design doc
    delta sections of `cel-host-surface.md`.

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
    `__type_value_of__("int") == __type_value_of__("int")` — a
    `kCallExpr` whose return type is `type` (CEL_TYPE).  The
    `==` overload's checker resolution gives an `equals` call
    with both operands typed `type`.  cel-cpp resolves this to
    the polymorphic `equals` (with operand type `dyn`-like in
    the resolved decl).  Our `RejectDyn` gate must not reject
    the resulting AST.  Mitigation: TypeType isn't on the
    `UnacceptableLabel` rejection list; verify by probe in
    M9.A.

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

  - **Cross-Plan CEL_TYPE equality.**  Today CEL_TYPE equality
    is by interned id; two CEL_TYPE values built under
    different Plans aren't comparable.  No fixture row reaches
    this; if one ever does, fall back to string-compare.
