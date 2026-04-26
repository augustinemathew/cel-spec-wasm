# M8 — Wrapper types

Status: **plan — drafted 2026-04-25, not yet started.  Depends on M7.**

The plan covers `google.protobuf.{Bool,Int32,Int64,UInt32,UInt64,
Float,Double,String,Bytes}Value` ("wrapper types") — the
construction-time auto-wrap path (scalar-RHS into wrapper-typed
field), the activation-bind auto-wrap path (`Bind("w",
Value::Int(5))` against a wrapper-typed slot), and the
wrapper-vs-scalar `==` / `!=` peel that satisfies langdef
§"Wrapper Types" equivalence.

**Out of scope:** every other proto literal concern (covered by
M7, including explicit wrapper-message construction
`Foo{w: Int32Value{value: 5}}` which lands at M7.E for free as a
recursive `kStructExpr` lower); `Any`; extensions; Timestamp;
Duration; wrapper coercion in arithmetic.

## 1. Why M8

Wrapper types are a self-contained slice of CEL's type system
that gates two distinct conformance cohorts:

| Fixture / row family | Today | Post-M8 (estimate) |
|---|---|---|
| `wrappers.textproto` | 0 / 36 | 30 – 35 |
| `comparisons.textproto` (`eq_wrapper/*`) | within 287 / 406 | +20 – 30 |
| `fp_math` wrapper rows | small subset | +5 |
| Total projected | — | **+50 – +70 PASS** |

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
  - **`Any` containing a wrapper.**  Any unpacking is a separate
    milestone.

## 3. Spec-mandated semantics

### 3.1 Wrapper equivalence (langdef §"Wrapper Types" + §"Equality")

| Expression | Result | Citation |
|---|---|---|
| `Int32Value{value: 1} == 1` | `true` | §"Wrapper Types": "wrapper types … equal to their underlying value" |
| `Int32Value{} == null` | `true` | §"Wrapper Types": "absent wrapper field is equal to `null`" |
| `Int32Value{value: 0} != null` | `true` | corollary — set wrapper is not null even if zero |
| `Int32Value{value: 0} == 0` | `true` | §"Wrapper Types" |
| `Int32Value{value: 1} == Int32Value{value: 1}` | `true` | wrapper-vs-wrapper falls back to MessageDifferencer (M5.B, shipped — re-run as M8 regression) |
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

## 4. Architecture — codegen vs runtime peel

Two viable options for the `==` / `!=` wrapper peel, scored
against the principles in CLAUDE.md ("compilers miscompile
silently — fail at the call site"):

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

### M8.A — wrapper field set + activation auto-wrap

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

### M8.B — wrapper equivalence peel (Option A)

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

### M8.C — closeout

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
