# M8 — Wrapper types

Status: **active development 2026-05-16.  Reconciled against
empirical probe + conformance baseline; original draft sections
preserved with `Plan-vs-execution delta:` callouts.**

> **Plan-vs-execution delta (2026-05-16 reconciliation):** the
> drafted plan undercounts unlock by ~2× and is missing one of
> the three required arms.  Three independent empirical findings
> reshaped the doc:
>
> 1.  **Conformance baseline** (master c33dab1, full
>     `bazel run //compiler_v2/conformance`): **153 wrapper-related
>     FAIL rows** across 7 fixtures.  `dynamic.textproto` carries 95
>     of them — the original plan didn't enumerate this fixture.
>     Net M8-deliverable unlock is **+151 PASS** (~2× the original
>     "+50-70" estimate).  Per-arm split: A=89, B=24, C=38.  See
>     §6.0 for the row inventory.
> 2.  **cel-cpp empirical probe** (throwaway branch
>     `throwaway/m8-wrapper-probe`, PR #4): drove cel-cpp's
>     `StandardRuntime` against an exhaustive matrix and surfaced
>     a critical option toggle —
>     `RuntimeOptions::enable_empty_wrapper_null_unboxing`
>     (default `false`).  When `false`, cel-cpp peels unset
>     wrapper fields to scalar zero; when `true`, to null.  The
>     conformance corpus is generated with `true`; we must
>     implement `true`-semantics to match (langdef §"Dynamic
>     Values" line 484-486 mandates this too).  The probe report
>     also confirms wrapper LITERALS (`BoolValue{}`) are
>     non-null-message values that peel to the inner scalar
>     default on comparison — distinct from UNSET FIELDs.
> 3.  **`typed_ast.cc:56` already maps wrapper-Repr to scalar-Repr**
>     (`if (type.has_wrapper()) return ReprOfPrimitive(type.wrapper())`).
>     This eliminates the original §4 Option-A-vs-Option-B
>     debate: codegen at `_==_` already lowers as scalar
>     equality.  The actual work is at the boundaries
>     (literal construction, field read, write set) — exactly
>     mirroring the m7b WKT-time pattern.
>
> The revised three-arm architecture (A=write-side, B=read-side
> + Any-chain, C=kStructExpr tail-unwrap) is documented inline
> in §2 and §4-§5 below.

The plan covers `google.protobuf.{Bool,Int32,Int64,UInt32,UInt64,
Float,Double,String,Bytes}Value` ("wrapper types") across **three
arms**:

  - **Arm A — write-side auto-wrap.**  `cel_set_field` for a
    wrapper-typed proto field accepts a matching scalar `CelValue`
    and synthesises the wrapper message via reflection.  Also
    handles activation marshalling (`Activation::Bind("w",
    Value::Int(5))` against a wrapper-typed binding) and
    null-into-wrapper-field (clears the field).
  - **Arm B — read-side auto-peel + Any-chain.**
    `ProtoBacking::ReadField` for a wrapper-typed CPPTYPE_MESSAGE
    field: unset → `cel::Value::Null()`; set → peeled inner
    scalar.  `UnpackAnyToValue` chains wrapper-peel after its own
    Any unwrap so `Any{Int32Value{value:1}}` surfaces as `int 1`.
    Mirrors the m7b `UnpackWellKnownTimeMessage` helper shape.
  - **Arm C — kStructExpr tail-unwrap.**  New
    `cel_host.cel_wkt_unwrap_wrapper` trampoline; `expr_lower.cc`
    emits a tail call after wrapper-FQN struct literals so
    `Int32Value{value: 5}` materialises as `CEL_INT(5)` rather
    than a `CEL_MESSAGE` slot codegen downstream doesn't expect.
    Direct clone of m7b's `cel_wkt_unwrap_time` shape.

**Out of scope:** every other proto literal concern (covered by
M7, including explicit wrapper-message construction
`Foo{w: Int32Value{value: 5}}` which after Arm C lowers as
`Foo{w: 5}` and goes through Arm A's auto-wrap); `Any` (Arm B
chains the inner wrapper-peel but Any-unwrap itself was M7-A.B);
extensions; Timestamp / Duration (M7B); wrapper coercion in
arithmetic (`Int32Value{value:1} + 2` still rejected).

## 1. Why M8

Wrapper types are a self-contained slice of CEL's type system
that gates multiple conformance cohorts.

> **Plan-vs-execution delta (2026-05-16):** the original
> "+50-70 PASS" estimate was based on a `wrappers.textproto` +
> `comparisons.eq_wrapper/*` + `fp_math` count.  Conformance
> baseline turned up `dynamic.textproto` (95 rows, the
> load-bearing wrapper fixture not enumerated in the original
> draft), plus rows in `proto2.textproto`, `proto3.textproto`,
> and `parse.textproto`.  Revised total: **+151 PASS**.

| Fixture / row family | Today | Post-M8 |
|---|---|---|
| `wrappers.textproto :: */to_any` | 0 / 9 | 9 / 9 (Arm B Any-chain) |
| `wrappers.textproto :: */to_null` | 9 / 9 | unchanged |
| `wrappers.textproto :: */to_json` (Value WKT) | 0 / 18 | non-M8 |
| `comparisons.eq_wrapper` | 18 / 45 | 45 / 45 |
| `dynamic.textproto` wrapper rows | 0 / 95 (9 SKIP non-M8) | 95 / 95 |
| `proto2.textproto :: literal_wellknown` | 0 / 9 | 9 / 9 (Arm A) |
| `proto2.textproto :: empty_field/wkt` | 0 / 1 | 1 / 1 (Arm B) |
| `proto3.textproto :: literal_wellknown` | 0 / 9 | 9 / 9 (Arm A) |
| `parse.textproto :: repeat/message_literal` | 0 / 1 | 1 / 1 (Arm A) |
| **Net M8-deliverable rows** | — | **+151 PASS** |

Non-M8 wrapper-adjacent rows (excluded from the unlock target):
38 rows split across `wrappers.textproto` misc-WKT (18:
google.protobuf.Value / ListValue / Struct / FieldMask / Empty);
`dynamic.textproto :: literal_no_field_access` (9 — harness
disable_check); `type_deduction.textproto` wrapper rows (9 —
harness check_only); `optionals.textproto` (2 — `?` syntax).
See `compiler_v2/conformance/README.md` and the baseline report
at `/tmp/m8_baseline_report.md` for the row-by-row inventory.

Carved out of M7 (originally M7.C in the now-superseded combined
plan) because:

  - **Wrappers are orthogonal to struct construction.**  M7 ships
    proto literal construction including explicit-wrapper-message
    fields (`Foo{w: Int32Value{value: 5}}`).  M8 is exactly the
    delta to support `Foo{w: 5}` (auto-wrap from scalar) and
    `Int32Value{value: 1} == 1` (peel for equivalence).
  - **The architectural choice (codegen-peel vs runtime-peel)
    deserves its own design slot** — see §4 below.  Folding it
    inside M7 muddied the M7 design conversation.
  - **Conformance cohort is contained.**  The 36-row
    `wrappers.textproto` plus the wrapper rows in `comparisons`
    move together; M7 can ship +200 PASS without M8 landing.

## 2. Scope

### 2.1 In-scope

  - **Wrapper field auto-wrap on construction.**
    `Foo{w: 5}` where `w` is `google.protobuf.Int32Value`-typed
    — `cel_set_field` synthesises an `Int32Value{value: 5}`
    proto and assigns it to the field.
  - **Wrapper-typed activation binding auto-wrap.**
    `Activation::Bind("w", Value::Int(5))` against an expression
    where `w` is checker-typed `Int32Value` — `Instance::EncodeMessage`
    synthesises the wrapper proto on the way in.
  - **Wrapper-vs-scalar equivalence peel for `==` / `!=`.**
    `Int32Value{value: 1} == 1`, `Int32Value{} == null`,
    `Int32Value{value: 0} != null`, `Int32Value{value: 0} == 0`,
    and the per-kind counterparts.  Either operand may be the
    wrapper.
  - **9 wrapper kinds:** `Bool`, `Int32`, `Int64`, `UInt32`,
    `UInt64`, `Float`, `Double`, `String`, `Bytes` Value.

### 2.2 Out-of-scope (deferred)

  - **Wrapper construction via explicit message literal**
    (`Foo{w: Int32Value{value: 5}}`).  Already shipped at M7.E
    as a normal recursive `kStructExpr` lower.
  - **Wrapper-vs-wrapper `==`** (`Int32Value{value:1} ==
    Int32Value{value:1}`).  Falls back to `cel_message_eq` /
    `MessageDifferencer::Equals` — already shipped at M5.B step
    2b.  M8 doesn't touch this path; it does include it in the
    regression matrix (§6.1).
  - **`dyn(wrappermsg)`-erased comparisons.**  The static-subset
    rejects these at the frontend gate (Slice 1.5's admissibility
    narrowed to scalar arguments).  If a later milestone broadens
    `dyn` admissibility to wrappers, the codegen peel needs a
    runtime fallback (Option B, §4); flagged as future work.
  - **Wrapper coercion in arithmetic** (`Int32Value{value:1} + 2`
    → `3`).  cel-cpp ships this; conformance fixtures rarely
    exercise it.  Out of scope for M8; revisit if a fixture row
    surfaces.
  - **`Any` containing a wrapper.**  Any unpacking shipped at
    M7-A (`UnpackAnyToValue` in `ProtoBacking::ReadField`); a
    wrapper-typed message read from an Any field today returns the
    typed wrapper message (e.g. `Int32Value{value: 1}`) rather than
    auto-unwrapping to the scalar.  M8.A's `WriteMessageOrPack`
    extension (the seam M7-A.A factored) is the natural place to
    add the wrapper auto-unwrap arm — the helper already dispatches
    on src/dst descriptors and can grow a `wrapper-message + scalar-
    field` arm without rewriting the cpp_type table.

## 3. Spec-mandated semantics

### 3.1 Wrapper equivalence (langdef §"Wrapper Types" + §"Equality")

> **Plan-vs-execution delta:** the original table claimed
> `Int32Value{} == null` → `true`.  Empirical probe + conformance
> fixture (`comparisons.eq_X_not_null`: `BoolValue{} != null` →
> `true`) both refute this.  An empty WRAPPER LITERAL is a
> present message that peels to the inner scalar default —
> `BoolValue{} == false` is `true`, `BoolValue{} == null` is
> `false`.  The "absent wrapper field equals null" rule applies
> only to UNSET FIELD READS, not to constructed literals.

| Expression | Result | Citation |
|---|---|---|
| `Int32Value{value: 1} == 1` | `true` | §"Wrapper Types": "wrapper types … equal to their underlying value" |
| `Int32Value{} == 0` | `true` | empty literal peels to inner scalar default (= `0` for int wrappers) |
| `Int32Value{} == null` | `false` | empty wrapper literal is a present message, not null |
| `Int32Value{} != null` | `true` | corollary; matches `eq_X_not_null` rows |
| `Int32Value{value: 1} != null` | `true` | set wrapper literal is not null |
| `Int32Value{value: 0} == 0` | `true` | set-to-default-still-peels-to-default |
| `TestAllTypes{}.single_int32_wrapper == null` | `true` | UNSET wrapper field read evaluates to null (langdef line 484-486; cel-cpp option `enable_empty_wrapper_null_unboxing=true`) |
| `TestAllTypes{single_int32_wrapper: 0}.single_int32_wrapper == 0` | `true` | SET wrapper field read auto-peels to inner scalar (Arm B) |
| `TestAllTypes{single_int32_wrapper: 0}.single_int32_wrapper == null` | `false` | SET-with-default is still "set"; reads as scalar `0`, not null |
| `has(TestAllTypes{single_int32_wrapper: 0}.single_int32_wrapper)` | `true` | presence is independent of value |
| `has(TestAllTypes{}.single_int32_wrapper)` | `false` | unset |
| `Int32Value{value: 1} == Int32Value{value: 1}` | `true` | both peel to `1`; scalar-vs-scalar equality (M5.B's `cel_message_eq` is NOT reached because Arm C peels both operands) |
| `BoolValue{value: true} == true` | `true` | per-kind counterpart |
| `StringValue{value: "x"} == "x"` | `true` | per-kind counterpart |
| `BytesValue{value: b"x"} == b"x"` | `true` | per-kind counterpart |
| `DoubleValue{value: 1.5} == 1.5` | `true` | per-kind counterpart |

cel-cpp's `equality_functions.cc::IsAnyEqual` handles this by
peeling wrapper messages to their wrapped value before the
polymorphic dispatch.  See §4 for which layer M8 puts the peel
in.

### 3.2 Activation marshalling — auto-wrap decision

When the user calls `Activation::Bind("w", Value::Int(5))` and the
expression's checker-typed `w` is `google.protobuf.Int32Value`,
three options:

  - **Auto-wrap** (cel-cpp behaviour): synthesise an `Int32Value{
    value: 5}` proto on the fly; bind that.
  - **Reject** at bind time: surface "type mismatch — w expected
    Int32Value, got int".
  - **Require explicit `Value::Message`** wrapping the proto.

**Recommendation: auto-wrap.**  Rationale:

  - Matches cel-cpp; minimises divergence the conformance fixtures
    care about.
  - The activation API is the high-level user-facing surface — every
    embedder that wants a primitive bound today would otherwise
    need to construct a `WrapperValue` proto themselves on every
    call site, which leaks the wrapper-vs-scalar semantic into
    user code.
  - The wrap is cheap — one descriptor lookup, one
    `MessageFactory::GetPrototype`, one `Reflection::SetField` —
    and the per-`Plan` descriptor cache makes the lookup amortised.
  - Auto-wrap composes cleanly with the equivalence peel
    (§4): bound int → wrapper proto on the way in, peel back
    to int on the way out at `==`.

Implementation site: `Instance::EncodeMessage` in
`compiler_v2/api/instance.cc` (line ~428) gains an "if the
declared type is a wrapper message and the bound `Value` is a
matching scalar, synthesise the wrapper" prelude.

### 3.3 Construction-side auto-wrap

`Foo{w: 5}` where `w` is `Int32Value`-typed: cel-cpp's checker
either coerces the literal int to a wrapper (in which case
codegen sees `Int32Value{value: 5}`) or stamps the entry with a
"needs auto-wrap" annotation that codegen reads.  **Probe-spike
required at start of M8.A** — author a one-line cel-cpp
roundtrip on `Foo{w: 5}` and inspect the resulting `CheckedExpr`.
Two outcomes:

  - **Checker auto-promotes** the literal to a wrapper-message
    construction: M8 needs no extra construction-side codegen
    (M7.E already handles explicit wrapper construction).  M8.A
    becomes activation-only.
  - **Checker leaves it as scalar with a coercion marker**:
    `cel_set_field` for a wrapper-typed field needs an extra
    arm that synthesises the wrapper before the
    `Reflection::SetField` call.

In either outcome, M8.A's actual code change is small.

## 4. Architecture — three-arm pattern (revised 2026-05-16)

> **Plan-vs-execution delta:** the original §4 weighed two
> wrapper-equality peel options (codegen-emit at `_==_` vs
> runtime-peel in `cel_message_eq`).  Both are unnecessary.
> `compiler_v2/ir/typed_ast.cc:56` already maps every wrapper
> Repr to its eponymous scalar Repr, so codegen at `_==_`
> already lowers as a scalar comparison.  The breakage is at
> the OPERAND boundaries: literal wrapper construction emits
> `CEL_MESSAGE` into a scalar slot, and field-read of a
> wrapper field emits `CEL_MESSAGE` instead of the inner
> scalar.  Fix the boundaries, leave equality alone.
>
> The old §4 (Options A/B) is preserved below as historical
> context; the actual architecture is the three-arm pattern
> described here, which mirrors m7b's WKT-time work exactly.

### 4.1 The boundaries

```
                ┌─── Arm A ─────┐
   activation ─→│ EncodeMessage │─→ CEL_MESSAGE slot ─→ Reflection.SetField
   scalar bind  │ (instance.cc) │                       (wrapped scalar)
                └───────────────┘

                ┌─── Arm A ─────┐
   kStructExpr  │ cel_set_field │─→ Reflection.SetField
   `Foo{w: 5}`  │ (cel_host.cc) │   (synth wrapper from scalar)
                └───────────────┘

                ┌─── Arm B ─────┐
   field-read   │ ReadScalarField │─→ inner scalar (or null if unset)
   `Foo.w`      │ (cel_host.cc)   │
                └─────────────────┘

                ┌─── Arm B (Any-chain) ─┐
   Any-read     │ UnpackAnyToValue      │─→ inner scalar
   `m.any_w`    │ → wrapper detection   │
                └───────────────────────┘

                ┌─── Arm C ───┐
   kStructExpr  │ tail-call    │─→ CEL_INT/BOOL/STRING/...
   `IntXValue{}`│ cel_wkt_unwrap_wrapper │
                └────────────────────────┘
```

### 4.2 Why this works without a codegen `==` peel

Trace `google.protobuf.Int32Value{value: 1} == 1` through the
pipeline:

1.  cel-cpp checker stamps the kStructExpr type as
    `wrapper(Int32)`; the int literal as `int`.
2.  `typed_ast.cc:56` maps `wrapper(Int32)` → `Repr::kInt`;
    int literal → `Repr::kInt`.
3.  Codegen sees both operands as `Repr::kInt`, resolves `_==_`
    to `equals_int` overload, emits scalar `cel_int_equals_at_vv`
    — no peel needed at the equality site.
4.  At the LHS kStructExpr lowering, Arm C tail-calls
    `cel_wkt_unwrap_wrapper(out_slot, msg_slot,
    wrapper_kind)`, which writes `CEL_INT(1)` to `out_slot`.
    Matches what step 3 expects.
5.  RHS literal emits `CEL_INT(1)`.  Done.

Trace `TestAllTypes{}.single_int32_wrapper == null`:

1.  Checker stamps `.single_int32_wrapper` as `wrapper(Int32)`;
    `null` as `null_type`.  `_==_` is polymorphic across
    null and scalar in cel-cpp (the `equals` overload
    accepts `null_type` on either side).
2.  At runtime, Arm B's read-side peel: field is unset
    → `cel::Value::Null()` → `CEL_NULL` slot.
3.  `cel_equals_at_vv` sees `CEL_NULL == CEL_NULL` → `true`.

Trace `TestAllTypes{single_int32_wrapper: 5}.single_int32_wrapper`:

1.  Outer kStructExpr lowering: Arm A's `cel_set_field`
    receives a scalar value for a wrapper-typed field;
    synthesises `Int32Value{value: 5}` via reflection and
    assigns.
2.  Outer kSelect lowering: Arm B's read-side peel returns
    inner scalar `5` as `CEL_INT(5)`.

### 4.3 Original §4 (superseded) — codegen-vs-runtime peel debate

Preserved for context: the original draft weighed two options for
the wrapper `==` peel.  Both became moot once the `typed_ast.cc:56`
mapping was understood.

### Option A — codegen peels at `_==_` lowering site

  - At `EmitGeneralCall` for `_==_` / `_!=_`: if either operand's
    checker-stamped CEL type is a wrapper message, emit a
    `cel_host.cel_unwrap_wrapper(out_slot, in_slot)` call before
    the polymorphic equality dispatch.  `cel_unwrap_wrapper`
    returns either the wrapped scalar CelValue or
    `Value::Null()` if the wrapper is unset.
  - **Pros.**  Runtime kernel stays simple (`cel_equals_at_vv`'s
    message arm continues to call `cel_message_eq` for non-
    wrapper messages — no per-call descriptor lookup on the hot
    path).  The peel is decided at compile time from static
    types; cel-cpp's checker has already stamped the wrapper
    type so the codegen test is just a `CelType::IsWrapper()`
    check.  No new runtime branch.
  - **Cons.**  One new host import (`cel_unwrap_wrapper`) and
    one new emit site.  Doesn't help dynamic-typed comparisons
    where `dyn(wrapperVar) == 1` reaches codegen with both
    operands typed `dyn` (cel-cpp's checker may erase wrapper
    type info under `dyn`).  However, M8 doesn't admit
    `dyn(wrappermsg)` at the static-subset gate (per Slice 1.5),
    so this case can't arise.

### Option B — runtime peels in `cel_equals_at_vv`

  - `cel_equals_at_vv`'s message arm checks each operand's
    descriptor against a "is wrapper" predicate (one descriptor
    pointer comparison against an interned `Int32ValueDescriptor*`
    cache); if either operand is a wrapper, peel via
    reflection and re-dispatch through the polymorphic ladder.
    Falls back to `cel_message_eq` only when both operands are
    non-wrapper messages.
  - **Pros.**  Single change — `cel_equals_at_vv` only.  Handles
    every reach uniformly (codegen + activation-bound + dyn-
    passed).  No new emit site.
  - **Cons.**  Adds a per-message-`==` descriptor lookup on the
    hot path (cacheable but non-zero).  Spreads wrapper semantics
    into the runtime kernel (the kernel today is descriptor-
    free; this would be its first descriptor consumer).

### Recommendation — Option A

Codegen peel.  Reasons:

  - The static-subset rejects `dyn(wrappermsg)`-shaped programs at
    the frontend gate (Slice 1.5's admissibility narrowed to scalar
    arguments only), so the dyn-erasure concern doesn't bite.
  - Keeps `cel_runtime.c`'s `cel_equals_at_vv` descriptor-free —
    matches the design principle that "host imports are the only
    descriptor consumer" (§4.7.6 of `design.md`).
  - The peel is a no-op for non-wrapper messages, so the cost is
    zero for the common case.
  - One new host import (`cel_unwrap_wrapper`) is cheaper than
    threading descriptor pointers through the runtime ABI.

Cross-fixture sanity check before committing to Option A: confirm
that no `wrappers.textproto` row exercises a `dyn(...)`-erased
wrapper comparison.  If a row does, fall back to Option B; the two
options are not mutually exclusive (Option A peel + Option B fallback
is a third design that pays the full cost of both).

## 5. Sequencing — slices

> **Plan-vs-execution delta:** revised to three slices (A / B / C)
> matching the boundary architecture in §4.  The original draft
> had two (auto-wrap + equivalence-peel).  Implementation order:
> **B → C → A → closeout**.  Rationale: B is smallest and
> isolates the read-side peel (also retires the proto3-incidental-
> pass which currently disguises the bug); C mirrors m7b's tail-
> unwrap so the codegen + host trampoline lands with maximum
> pattern reuse; A is the largest and depends on Arm B existing
> so the round-trip test rows can light up end-to-end.

### M8.B — read-side wrapper auto-peel + Any-chain  *(first)*

  - **Layer-1 helper.**  Add `UnpackWrapperMessage(const Message&)
    → optional<cel::Value>` to the anonymous namespace in
    `compiler_v2/api/internal/cel_host.cc`.  Mirrors the existing
    `UnpackWellKnownTimeMessage`: descriptor `full_name()` against
    the 9 wrapper FQNs; on match, reflection-read field number 1
    (`value`); return matching `cel::Value::{Bool, Int, Uint,
    Double, String, Bytes}`; on non-wrapper return `nullopt`.
  - **Wire into `ReadScalarField`.**  CPPTYPE_MESSAGE arm:
    after the existing Any-unwrap and WKT-time check, call
    `UnpackWrapperMessage(sub)`; if it returns a value, use it.
    The proto3-`!HasField` → Null branch already covers
    proto3-unset; broaden to ALL syntaxes for wrapper fields
    (langdef line 484-486 mandates this exception across
    proto2 and proto3 — currently `proto2_null` rows FAIL
    while `proto3_null` rows incidentally PASS).
  - **Chain into Any.**  Extend `UnpackAnyToValue`: after the
    `sub->ParseFromString(bytes)` succeeds, call
    `UnpackWrapperMessage(*sub)` and `UnpackWellKnownTimeMessage(*sub)`
    in order; if either returns a value, surface it (Any-of-
    wrapper / Any-of-Timestamp peels through transparently).
    Fallback to `OwnedMessage(sub)` as today.
  - **Test matrix.**  9 wrapper kinds × {set, unset, set-to-zero,
    set-default-empty-string, set-default-empty-bytes} × {proto2,
    proto3}.  Plus Any-of-wrapper for each kind.  Plus negative:
    non-wrapper message stays as `HostMessage`.
  - **Conformance unlock.**  +24 standalone rows
    (`*_proto2_null` × 9 + `wrappers/<w>/to_any` × 9 +
    `dynamic/<w>/field_read_proto2_unset` × 5 + `proto2/empty_field/wkt`
    × 1).  Plus completes the read-half of 16 dynamic round-trip
    rows that A unblocks.
  - **Effort.**  Small.

### M8.C — kStructExpr wrapper tail-unwrap  *(second)*

  - **WAT-first.** *Shipped 2026-05-16 (commit 56cac56)*.
    `doc/implementation-plan/rewrite/wat/56_wrapper_kstruct_unwrap.wat`
    captures the codegen shape for
    `google.protobuf.Int32Value{value: 5}`: `cel_make_message` →
    `cel_set_field(value)` → `cel_wkt_unwrap_wrapper(out_slot,
    msg_slot, wrapper_kind)` tail call, returning the same slot.
    The slot is overwritten in place (msg_slot == out_slot) —
    same pattern as m7b's `cel_wkt_unwrap_time`.  `wat_runner`
    gained a stub for the new trampoline + a
    `WrapperKStructTailUnwrapProducesCelInt` test verifies the
    ABI args arrive and a stub-emitted `CEL_INT(5)` round-trips
    through `$eval`.  See `wat-traces.md` §56 for the
    walkthrough.
  - **ABI.**  `cel_host.cel_wkt_unwrap_wrapper(out_slot,
    msg_slot, wrapper_kind) → ()`.  Three i32 args; the third
    is the matching `CelKind` for the inner scalar
    (`CEL_BOOL=1`, `CEL_INT=2`, `CEL_UINT=3`, `CEL_DOUBLE=4`,
    `CEL_STRING=5`, `CEL_BYTES=6`).  **Wrapper-kind = CelKind is
    intentional load-bearing collision** (per WAT design pass):
    Layer-2's switch on `wrapper_kind` selects both the
    descriptor FQN to cross-check AND the matching
    `CelValue.kind` to emit, with no duplicated dispatch table.
    `Int32Value` and `Int64Value` collapse onto `CEL_INT`;
    `UInt32Value` and `UInt64Value` onto `CEL_UINT`;
    `FloatValue` and `DoubleValue` onto `CEL_DOUBLE` — matches
    CEL's value algebra (no 32-vs-64 distinction).  Codegen
    knows the kind statically from the kStructExpr FQN; no
    per-call descriptor walk.  Layer-2
    `CelWktUnwrapWrapperImpl` reads `msg_slot`, expects
    `CEL_MESSAGE`, dereferences via
    `ExternrefTable::Lookup` → `HostMessageBacking::message()`,
    cross-checks `descriptor()->full_name()` matches
    `wrapper_kind`, reflection-reads `value` field, writes
    matching scalar `CelValue` to `out_slot`.  Wrong descriptor
    → `CEL_ERROR(kTypeMismatch)` (defence in depth; codegen
    only emits the call when the FQN matches).
  - **Codegen.**  Extend `expr_lower.cc::MaybeEmitWktUnwrapTailCall`
    (already refactored from `EmitKStructExpr` during m7b review
    nits — perfect seam) to dispatch on wrapper FQNs in addition
    to Timestamp/Duration.  Returns either the existing
    `cel_wkt_unwrap_time` ExpressionRef or a new
    `cel_wkt_unwrap_wrapper` ExpressionRef.
  - **Overload table.**  Add `cel_wkt_unwrap_wrapper` import to
    `kBuiltinSeeds` (kCelHost).  Add `compile.cc::InstallStructImports`
    to install the import alongside `cel_wkt_unwrap_time`.
  - **Test matrix.**  9 wrapper FQNs × {set-to-value,
    set-to-zero, empty-construct} = 27 rows.  Plus negative
    via direct trampoline test:
    `CelWktUnwrapWrapperImpl(msg_slot=non-wrapper)` →
    `CEL_ERROR`.
  - **Conformance unlock.**  +38 rows (`comparisons.eq_X` × 9 +
    `comparisons.eq_X_empty` × 9 + `dynamic/<w>/literal*` × 20).
  - **Effort.**  Medium.

### M8.A — write-side auto-wrap + activation auto-wrap  *(third)*

  - **Probe-spike outcome.**  The cel-cpp probe (PR #4) confirmed
    that `TestAllTypes{single_int32_wrapper: 5}` is admitted at
    check time — the checker auto-promotes scalar literals into
    wrapper-typed message fields.  `compiler_v2/codegen` therefore
    sees the source-AST as a struct-with-int-field, which means
    `cel_set_field` is called with a scalar `CelValue` against a
    `CPPTYPE_MESSAGE` field.  No frontend / typed_ast change is
    needed; the work is entirely in Layer 2 + Instance.
  - **`SetScalarField` wrapper arm.**  CPPTYPE_MESSAGE case:
    after the existing null-clears-field branch, if
    `field.message_type()->full_name()` is a wrapper FQN AND
    `value.kind` matches the wrapper's inner scalar kind,
    synthesise the wrapper via `MessageFactory::generated_factory()
    ->GetPrototype(field.message_type())->New()`, reflection-set
    its `value` field from the CelValue, then
    `MutableMessage(&msg, &field)->CopyFrom(*wrapper)`.  On
    kind mismatch (scalar of wrong kind), `InvalidArgumentError`.
    On non-wrapper non-Any descriptor mismatch, fall through
    to the existing `WriteMessageOrPack` → Unimplemented(M8)
    path (now empty — wrapper case is handled by this new
    arm).
  - **`WriteMessageOrPack` cleanup.**  Remove the
    `wrapper auto-wrap is M8` Unimplemented branch — it
    becomes truly unreachable once the scalar wrapper-arm
    above handles every reachable case.  Replace with
    `ABSL_CHECK(false)` + actionable message.
  - **`Instance::EncodeMessage` activation auto-wrap.**  When
    a declared variable's checker-stamped CelType is a
    wrapper and the bound `cel::Value` is the matching scalar
    kind, synthesise the wrapper proto on the way in.  When
    the bound value is `Null`, leave the field unset.
    Implementation site: `compiler_v2/api/instance.cc`
    (alongside the existing m7b `TryEncodeWktTimeMessage`
    helper — extract a shared `TryEncodeWktAnyMessage`
    super-helper that dispatches by FQN).
  - **Test matrix.**  9 wrappers × 2 entry points
    (cel_set_field + activation Encode) = 18 baseline cells;
    add boundary values per kind (INT32_MIN/MAX, UINT64_MAX,
    NaN, INF, embedded NUL, multi-byte UTF-8, empty
    string/bytes) = ~50 rows; negative matrix (wrong-kind,
    explicit-message-of-wrong-FQN, null-into-wrapper-field-
    leaves-unset).
  - **Conformance unlock.**  +89 rows including the 16
    round-trip rows that B already half-unblocked.
  - **Effort.**  Medium-large.

### M8.D — closeout

  - Run full conformance + record post-M8 numbers in
    `compiler_v2/conformance/README.md`.
  - Run `scripts/run_full_suite.sh`.
  - Reconcile `cel-host-surface.md` (new `cel_wkt_unwrap_wrapper`
    trampoline doc); `m7-proto-literals.md` (drop M8-placeholder
    references); `design.md` §11.5 (flip wrapper rows to
    shipped).
  - Flip this doc's status header.
  - Tick `testing-checklist.md` rows under "Rewrite M8".
  - Append a "Future work" section: wrapper coercion in
    arithmetic (`Int32Value{value:1} + 2`); `dyn(wrappermsg)`
    runtime fallback if static-subset broadens.
  - Throwaway probe branch + PR #4 stays open as a reference;
    do not merge.

### Original §5 (superseded)

The original two-slice plan (M8.A wrapper-field-set + activation
auto-wrap; M8.B wrapper-equivalence-peel) is preserved verbatim
below.  Its content remains useful as background; the as-shipped
sequencing is the three-arm split above.

### Original M8.A — wrapper field set + activation auto-wrap

  - **Probe-spike** (§3.3): determine whether cel-cpp's checker
    auto-promotes scalar-into-wrapper-field literals.
  - **`cel_set_field` wrapper arm** (only if the spike shows
    checker leaves it scalar): a wrapper-typed field whose source
    is a scalar CelValue synthesises the wrapper proto via
    `MessageFactory::GetPrototype(wrapper_descriptor)->New() +
    Reflection::SetField`, then assigns to the outer message.
  - **Activation auto-wrap.**  `Instance::EncodeMessage` detects
    declared-type-is-wrapper + bound-value-is-scalar and
    synthesises the wrapper proto.
  - **Test matrix.**  9 wrapper kinds × 2 entry points
    (construction + activation) = 18 cells, plus 9 ×
    matching-scalar-kind regression rows.
  - **Conformance unlock estimate.**  +5–10 PASS in `wrappers`
    (auto-wrap rows specifically).
  - **Effort.**  Small.

### Original M8.B — wrapper equivalence peel (Option A)

  - **WAT-first.**  Author `doc/implementation-plan/rewrite/wat/
    14_wrapper_equivalence.wat` showing the
    `cel_host.cel_unwrap_wrapper(out_slot, in_slot)` call shape
    inserted before `cel_equals_at_vv`.  Stub
    `cel_unwrap_wrapper` in `wat_runner`.
  - **ABI.**  New host import `cel_unwrap_wrapper`.
    Layer-2 `CelUnwrapWrapperImpl` + Layer-3 trampoline
    registration + `cel_runtime.c` extern decl.  Layer-2: read
    CelValue at `in_slot`, expect `kMessage` whose descriptor
    matches one of the 9 wrapper descriptors (cached pointer
    comparison), read the `value` field via Reflection, write
    the scalar CelValue (or `Null` if unset) to `out_slot`.
    Wrapper-descriptor cache lives on `Instance`, populated at
    Plan time from the descriptor pool's well-known-type table.
  - **Codegen.**  In `_==_` / `_!=_` lowering: if either
    operand's checker-stamped type `IsWrapper()`, emit a
    `cel_unwrap_wrapper` call before the equality dispatch.
  - **Test matrix.**  9 wrapper types × 5 equivalence rows from
    §3.1 = 45 parameterised rows, plus the wrapper-vs-wrapper
    regression (already shipped at M5.B, re-run as M8 regression).
  - **Conformance unlock estimate.**  +25–35 PASS in `wrappers`
    + `comparisons.eq_wrapper/*`.
  - **Effort.**  Medium.

### Original M8.C — closeout

  - Run `bazel run //compiler_v2/conformance:run_conformance` and
    record the post-M8 deltas in `compiler_v2/conformance/README.md`.
  - Run `scripts/run_full_suite.sh`.
  - Flip `design.md` §11.5 wrapper rows to "shipped".
  - Flip this doc's status header to `shipped YYYY-MM-DD`.
  - Tick `testing-checklist.md` rows under "Rewrite M8".
  - Reconcile sibling docs (`cel-host-surface.md` for the new
    host import, M7 doc if it cited M8 placeholders).
  - Append "Future work" — wrapper coercion in arithmetic,
    `dyn(wrappermsg)` runtime fallback if a later milestone
    needs it.

## 6. Test matrix

### 6.1 Equivalence positive matrix

9 wrapper types × 5 equivalence shapes = 45 cells:

| Wrapper | scalar-equal | unset-eq-null | set-zero-neq-null | set-zero-eq-zero | wrapper-vs-wrapper (regression) |
|---|---|---|---|---|---|
| BoolValue | ✓ | ✓ | ✓ (false vs null) | ✓ | ✓ |
| Int32Value | ✓ | ✓ | ✓ | ✓ | ✓ |
| Int64Value | ✓ | ✓ | ✓ | ✓ | ✓ |
| UInt32Value | ✓ | ✓ | ✓ | ✓ | ✓ |
| UInt64Value | ✓ | ✓ | ✓ | ✓ | ✓ |
| FloatValue | ✓ | ✓ | ✓ | ✓ | ✓ |
| DoubleValue | ✓ | ✓ | ✓ | ✓ | ✓ |
| StringValue | ✓ | ✓ | ✓ ("" vs null) | ✓ ("" eq "") | ✓ |
| BytesValue | ✓ | ✓ | ✓ (b"" vs null) | ✓ (b"" eq b"") | ✓ |

### 6.2 Auto-wrap positive matrix

  - 9 × construction-site auto-wrap (`Foo{w: scalar}`).
  - 9 × activation-bind auto-wrap (`Bind("w", Value::Scalar(...))`).
  - 9 × explicit-wrapper-construction regression
    (`Foo{w: WrapperType{value: scalar}}`) — shipped at M7.E,
    re-run as a guard against accidental peel-on-construction.

### 6.3 Negative / rejection matrix

  - Wrapper field bound with wrong scalar kind:
    `Activation::Bind("w", Value::String("x"))` against
    `Int32Value`-typed `w` → encoder returns `InvalidArgument`.
  - Auto-wrap from non-scalar (list / map / message-not-this-
    wrapper) → encoder returns `InvalidArgument`.
  - Wrapper-vs-wrong-kind-scalar (`Int32Value{value: 1} ==
    "1"`) — checker rejects (mixed-type `==` is type error in
    static subset); regression-test that it's the checker, not
    codegen, that catches.
  - Peel against a non-wrapper message: `cel_unwrap_wrapper`
    called with a `kMessage` whose descriptor isn't in the
    wrapper-descriptor cache → `InvalidArgument` trap (this
    can only arise if the codegen peel-emit guard is wrong;
    catching it as a runtime trap is the tripwire).

### 6.4 Test placement

  - `compiler_v2/api/internal/cel_host_test.cc` — Layer-2
    `CelUnwrapWrapperImpl` table (per wrapper kind, set / unset /
    wrong-descriptor).
  - `compiler_v2/codegen/expr_lower_test.cc` — `_==_` / `_!=_`
    emits `cel_unwrap_wrapper` when either operand's type
    `IsWrapper()`; doesn't emit it for non-wrapper message
    operands.
  - `compiler_v2/api/instance_test.cc` — activation auto-wrap
    table (9 wrappers × matching scalars).
  - `compiler_v2/e2e/m8_test.cc` (new) — every conformance-row-
    shape, parameterised against the matrix above.
  - `doc/implementation-plan/rewrite/wat/14_wrapper_equivalence.wat`.

## 7. Risks + open questions

  - **R1 — Wrapper auto-wrap behaviour at activation bind time.**
    Pinned to "auto-wrap" in §3.2.  If a conformance row turns out
    to require explicit-wrap (cel-cpp diverges from the obvious
    interpretation), surface during M8.A and revisit.  Mitigation:
    document the choice explicitly in the activation API doc;
    wire a CHECK that catches scalar-vs-non-wrapper-message
    mismatches.
  - **R2 — Checker auto-promotion shape (probe-spike).**  See
    §3.3.  Mitigation: write the spike before any M8.A code.
  - **R3 — Cached wrapper-descriptor pointer staleness.**  M8.B's
    `CelUnwrapWrapperImpl` caches the 9 wrapper `Descriptor*`s at
    Plan time.  If the descriptor pool is mutated between Plan
    and Eval, the cache is stale.  Mitigation: matches M7's R1
    (descriptor-pool lifetime) — same CHECK-on-instance, same
    invariant.
  - **R4 — `dyn(wrappermsg)` admissibility in the future.**
    Static subset rejects this today.  If a future milestone
    broadens `dyn`, M8.B's codegen peel needs a runtime
    fallback (Option B from §4).  Mitigation: flag in Future
    Work; not blocking for M8.

## 8. Out-of-scope (re-stated)

  - All non-wrapper proto literal concerns — see M7.
  - Explicit wrapper-message construction
    (`Foo{w: Int32Value{value: 5}}`) — shipped at M7.E.
  - `dyn(wrappermsg)`-erased comparisons.
  - Wrapper coercion in arithmetic.
  - `Any` containing a wrapper.

## 9. Future work

  - **Wrapper coercion in arithmetic** (`Int32Value{value:1} + 2`)
    if a fixture row surfaces.
  - **Runtime peel fallback (Option B)** if `dyn(wrappermsg)` is
    admitted at a later static-subset broadening.
