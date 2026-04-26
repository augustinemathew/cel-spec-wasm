# `dyn(...)` passthrough plan (Slice 1.5)

Status: **shipped 2026-04-25.**

What landed (one-paragraph summary):
  - **Frontend**: `IsDynPassthroughCall` in `parse_and_check.cc::CheckSubsetNode`
    admits `dyn(call)` whose argument is itself a `dyn(...)` call (recursive
    collapse) OR whose argument's checker-assigned type is a primitive scalar
    or `null`.  Aggregate / message / dyn-typed arguments fall through to
    the standard `UnacceptableLabel` dispatch and reject as before.  The
    plan's pseudocode (recurse-into-arg unconditionally) was tightened to
    a type-guarded recurse so `dyn(msg)` / `dyn([...])` / `dyn({...})` stay
    rejected per the plan's "What stays rejected" list — the unguarded
    pseudocode would have admitted them.
  - **ResolvePass**: `DynPassthroughVisitor` runs after every other resolve
    visitor and copies the argument's `repr` / `field_number` /
    `overload_id` / `local_index` / `attribute_id` / `map_origin` /
    `list_origin` onto the dyn call.  Storage forwarding deferred to
    LayoutPass per the pipeline order (ResolvePass runs before slot
    allocation).
  - **LayoutPass**: `AggregateStorageVisitor::PostVisitCall` detects dyn
    calls and copies the argument's `Storage` onto the call node instead
    of allocating a fresh slot — saving 24 B per dyn call and ensuring
    consumers that read the call's `ann.storage` (e.g. the ternary's
    cond_slot lookup) see the argument's slot.
  - **Codegen**: `expr_lower.cc::Emit` short-circuits `dyn(scalar)` to
    `Emit(arg)` directly — no helper function call, no `to_dyn` arm in
    the OverloadTable.
  - **Tests**: 8 frontend (admits scalar literal / ident / nested
    dyn / scalar select / scalar-across-`==`; rejects list / map /
    message / `dyn(msg).field`), 1 codegen (single
    `cel_equals_at_vv` call, no `cel_to_dyn` helper), 6 e2e
    (`DynPassthroughE2ETest`: cross-numeric `==`, `==`-double,
    string `==`, null `==`, cross-kind `!=`, ternary cond).
  - **Conformance delta**: 509 → 562 (+53 PASS).  Per-fixture:
    `comparisons` 83 → 189 (+106), `fp_math` 24 → 29 (+5),
    `integer_math` 35 → 45 (+10), `lists` 4 → 23 (+19), `fields`
    11 → 19 (+8), `parse` 152 → 157 (+5).  Below the +135–165
    projection — see "Plan-vs-execution delta" below.

> **Plan-vs-execution delta:** the projection assumed all
> `dyn(scalar) <op> other_kind` rows would graduate, but the runtime
> kernels for cross-numeric `<` / `<=` / `>` / `>=` / `in` are not yet
> shipped (only `==` / `!=` route through `cel_equals_at_vv`'s
> polymorphic dispatch).  After Slice 1.5 admits the dyn wrapper,
> those ordering rows reach the runtime and FAIL with kind-error
> instead of SKIPping.  In `comparisons.textproto`: +106 PASS,
> +98 FAIL (SKIP→FAIL).  Net wins are concentrated in equality-
> bearing fixtures.  The +98 visible failures unlock at M5.B step 2b
> (polymorphic ordering ladder).

A surgical relaxation of `RejectDyn` that admits `dyn(scalar)` as a
no-op type-check escape, while continuing to reject genuine
`dyn`-typed program shapes (variables, list/map element types,
field reads, function signatures).  Slot is **between Slice 1
(polymorphic equals, shipped) and Slice 2 (control flow)** in
`conformance-unlock-plan.md`.

## Why

The conformance corpus uses `dyn(...)` extensively as the *type-
checker escape hatch* to defer type-checking of an `==` operand
to the runtime.  cel-cpp's checker has no `equals(int, uint)`
overload, only same-type variants — so the corpus writes
`dyn(1) == 1u` to express what the spec calls "polymorphic
equality" without inventing checker overloads for every kind pair.

Our static-subset rejection of all `dyn` is currently a hard
ceiling on conformance: 150+ tests in `comparisons.textproto`
alone, plus pieces in `parse.textproto`, `fp_math.textproto`, and
`integer_math.textproto`, fail to compile because they thread a
`dyn(scalar)` through `==` / `!=`.

The runtime kernel (`cel_equals_at_vv`, M5.B step 2b) already
implements the cross-kind / cross-numeric equality the spec
defines.  The only thing standing between today and ~+120–150
new conformance pass is the front-end gate.

## Surgical relaxation: `dyn(scalar)` is the identity function

`dyn(x)` is a no-op at the value level — its observable runtime
behaviour is "return `x` unchanged" — and the *only* thing it
does at compile time is widen the static type of its argument to
`dyn` so the checker permits broader operand pairings downstream.

If we admit `dyn(scalar)` and treat it as **transparent at
codegen time** (`dyn_op(x)` lowers to whatever `x` lowers to),
then:

  - The checker accepts `dyn(1) == 1u` because the `==` operands
    typed `(dyn, uint)` find an overload.
  - Codegen emits two operand reads (one per slot) and a single
    `cel_equals_at_vv` call.  At runtime the kernel switches on
    `(CEL_INT, CEL_UINT)` → `cel_numeric_eq_at_vv` → returns
    `false` (langdef numeric ladder, `1 != 1u` is the wrong test
    — should return `true` for `1 == 1u`).
  - No new runtime helper, no new ABI, no new codegen arm.

## What gets admitted

A `dyn` *expression* whose argument is one of:

  - **Literal scalar.**  `dyn(1)`, `dyn(1u)`, `dyn(1.0)`,
    `dyn(true)`, `dyn(null)`, `dyn("foo")`, `dyn(b"bar")`.
  - **Identifier** with declared scalar type.
    `dyn(x)` where `x:int|uint|double|bool|string|bytes|null_type`.
  - **Nested arithmetic / compare expression** whose result type
    is scalar.  `dyn(a + b)`, `dyn(a < b)`.
  - **`dyn(dyn(x))` collapse.**  Recursive admission — the inner
    `dyn` is admitted if its operand is admitted.

## What stays rejected (deliberate)

  - **`dyn`-typed variable declarations.**  `b.DeclareVariable("x", CelType::Dyn())`
    → continues to reject.  We don't have a "carry a `dyn`-typed
    `cel::Value` through the activation marshaller" story.
  - **`dyn` as the result of a non-scalar value.**
    `dyn({"a":1})`, `dyn([1,2,3])`, `dyn(msg)` — REJECT.  The
    runtime kernel can compare scalar values cross-kind, but it
    cannot late-bind aggregate operations (`dyn(msg).field`,
    `dyn(list)[0]`, comprehensions over `dyn` ranges).  Admitting
    these opens M7+ scope.
  - **Field access on a `dyn` operand.**  `dyn(msg).field` —
    REJECT.  Late-bound field reads are an M7 surface
    (`HostMessageBacking::ReadField` is typed today).
  - **`list<dyn>` / `map<*, dyn>` / `map<dyn, *>` element types.**
    `RejectDyn` already rejects these via the recursive
    `UnacceptableLabel`; that rejection stays.  An admitted
    `dyn(scalar)` expression has scalar result type, NOT
    `dyn` — the type after the elision is the actual operand
    type.  See "Codegen treatment" below.
  - **`dyn`-typed function signatures.**  Custom function
    parameter / return types of `dyn` — REJECT.  M6 customs
    surface; not on the M5 critical path.

## Codegen treatment

`dyn(x)` is lowered as **the operand `x`'s slot directly** — no
helper call, no allocation.  Concretely, in `expr_lower.cc`:

```cpp
// In Emit(...) under kCallExpr arm, before EmitGeneralCall:
if (call.function() == "dyn" && call.args_size() == 1) {
  return Emit(ctx, call.args(0));  // identity passthrough
}
```

The emitted slot offset is the operand's slot.  The `NodeAnnotation`
for the `dyn(...)` node itself can carry the operand's `repr` and
`storage` — LayoutPass already populates these from the type_map,
which the checker has set to `dyn` for the call site.  We override:
when the call site is `dyn` with a scalar argument, copy the
argument's annotations onto the call node.  This keeps every
downstream consumer (operand reads in `==`, arithmetic chains,
comparison chains) seeing the underlying scalar type, NOT `dyn`.

## Frontend treatment (`RejectDyn`)

The change lives in `compiler_v2/frontend/parse_and_check.cc`:

  1. **Special-case `dyn` calls in `CheckSubsetNode`.**  Before
     the `UnacceptableLabel` lookup, detect a `kCallExpr` whose
     `function == "dyn"` and arg-count is 1.  If the *argument's*
     type passes `UnacceptableLabel` (i.e. is itself a scalar /
     allowed type), skip the `dyn` violation for the call node
     itself.  Recurse into the argument as normal.
  2. **Recurse into `dyn(dyn(x))`.**  The recursive descent
     already visits all children; the argument's own check
     determines whether the chain is admissible.
  3. **Tighten the catch-all.**  An expression whose checked type
     is `dyn` but whose AST shape isn't a `dyn(...)` call at the
     root remains rejected (e.g. an `IdentExpr` whose type is
     `dyn` because the variable was declared `dyn`-typed).

Pseudocode:

```cpp
// In CheckSubsetNode, before UnacceptableLabel:
if (node.has_call_expr() &&
    node.call_expr().function() == "dyn" &&
    node.call_expr().args_size() == 1) {
  const auto& arg = node.call_expr().args(0);
  // Recurse into arg only — skip checking the call node's own
  // type (which is `dyn` by construction).
  CheckSubsetNode(arg, types, out);
  return;
}
```

## LayoutPass / ResolvePass treatment

The pipeline already runs from `cel::Expr` nodes; the `dyn(...)`
call has its own `expr_id`.  We need its `NodeAnnotation` to
forward the argument's repr / storage so downstream consumers
work on the underlying scalar.

Two implementation options:

  **Option A — propagate in ResolvePass (preferred).**
  ResolvePass already walks the AST stamping annotations.  Add
  a `PostVisitCall` arm that special-cases `dyn`: copy the
  argument's `repr`, `storage`, `local_index`, `attribute_id`,
  `overload_id` onto the call node's annotation.

  **Option B — substitute in expr_lower.**  Skip the call node
  entirely and emit the argument directly (the `if` snippet in
  "Codegen treatment" above).  Layout still allocates a slot for
  the call node (LayoutPass doesn't know to skip), but the slot
  is unused.

Option B is the smaller change but wastes a workspace slot per
`dyn(...)` call (24 B each).  Option A is structurally correct
and easier to maintain.  **Recommended: Option A.**

## Testing

### Frontend (`parse_and_check_test.cc`)

  - `RejectDynAdmitsDynScalarLiteral` — `dyn(1)` parses + checks
    + passes `RejectDyn`.
  - `RejectDynAdmitsDynIdent` — variable `x:int`, expression
    `dyn(x)` admitted.
  - `RejectDynAdmitsNestedDyn` — `dyn(dyn(1))` admitted.
  - `RejectDynStillRejectsDynVariable` — `b.DeclareVariable("x", CelType::Dyn())`
    REJECTS.
  - `RejectDynStillRejectsDynMessage` — `dyn(msg)` where `msg`
    is a message-typed bound var REJECTS.
  - `RejectDynStillRejectsDynList` — `dyn([1,2,3])` REJECTS.
  - `RejectDynStillRejectsDynFieldAccess` — `dyn(msg).field`
    REJECTS (the inner `dyn(msg)` already fails before the
    field access).

### Codegen (`expr_lower_test.cc`)

  - `KCallDynPassthroughEmitsArgumentSlot` — `dyn(1) == 1u`
    lowers; the emitted body contains a single
    `cel_equals_at_vv` call with operands pointing at the
    rodata-resident `1` and `1u` (no helper call for the
    `dyn(...)`).

### E2E (`compiler_v2/e2e/m5_test.cc`)

Under a new fixture `DynPassthroughE2ETest`:
  - `DynScalarEqualsCrossNumeric` — `dyn(1) == 1u` → `true`.
  - `DynScalarEqualsDouble` — `dyn(1) == 1.0` → `true`.
  - `DynStringEqualsString` — `dyn("foo") == "foo"` → `true`.
  - `DynNullEqualsNull` — `dyn(null) == null` → `true`.
  - `DynNotEqualsAcrossKinds` — `dyn(1) != "1"` → `true`
    (cross-kind, kernel returns false on `==`, flipped).
  - `DynBoolPassthroughInArith` — `dyn(true) ? 1 : 2` (gates on
    Slice 2; SKIP-ish until then or re-express via a non-ternary
    shape).

### Conformance

After landing, expected:
  - `comparisons.textproto`: 144 → ~280–290 (+~140 by graduating
    every `dyn(scalar) == scalar` row).
  - `fp_math.textproto`, `integer_math.textproto`: small bumps
    (5–10 each) where cross-numeric `dyn(...)` rows live.
  - `parse.textproto`: ~10 rows that thread `dyn` through
    arithmetic / equality.

**Total projection: 486 → ~620–650 (+135–165 PASS), ~25–26%.**

## Out of scope

  - **Late-bound field reads.**  `dyn(msg).field` stays
    rejected.  M7 surface.
  - **`dyn` as a runtime kind.**  The runtime never observes a
    `CelValue` of kind `CEL_DYN` — there isn't one.  Every
    `dyn(...)` admitted by this slice resolves to its argument's
    underlying scalar kind at codegen time.
  - **`dyn` over aggregates** (`dyn([1,2])`, `dyn({"a":1})`).
    Stays rejected.  Admitting these opens cross-origin / mixed-
    kind container semantics that M5/M6 doesn't address.
  - **The conformance harness's existing classification.**  No
    changes to `runner.cc` — every `dyn(...)`-using test that
    *was* SKIPped at the static-subset gate will, after this
    slice, either PASS (the runtime kernel handles it) or FAIL
    (kernel produces wrong answer / hits an unimplemented arm).
    Either outcome is more useful than today's wholesale SKIP.

## Risks & mitigations

  1. **Risk: admitting `dyn(scalar)` quietly admits `dyn(x.field)`
     where `x.field` is scalar.**
     `dyn(msg.age)` has scalar result type and would pass our gate.
     This is OK and desirable — the field read is itself a normal
     scalar `kSelect`, the `dyn` wrapping it is a no-op identity.
     The rejection of `dyn(msg).field` (note operand order) still
     fires because the inner `dyn(msg)` has aggregate operand.

  2. **Risk: `dyn` collapsing changes type_map invariants relied
     on by LayoutPass.**
     LayoutPass reads `type_map` to assign `Repr`.  The `dyn(...)`
     call node has type `dyn` in the type_map, but ResolvePass
     overrides the annotation with the argument's repr.  Need to
     verify `LayoutPass` consults the annotation (not the
     type_map directly) for slot sizing.  **Mitigation:** read
     `LayoutPass::AssignSlot` and confirm; if it dereferences
     `type_map` directly, factor through `NodeAnnotation::repr`
     first (small refactor).

  3. **Risk: cel-cpp's checker assigns a "function" type to the
     call site itself, not `dyn`, causing UnacceptableLabel to
     fire on the function signature.**
     Need to verify what cel-cpp's `dyn` overload resolution
     emits in `type_map[dyn_call.id()]`.  **Mitigation:**
     write a probe test that compiles `dyn(1)` and inspects
     `type_map[root.id()]` — if it's `dyn`, we're good; if it's
     `function`, special-case it before the `UnacceptableLabel`
     dispatch.

     > **Probe spike (2026-04-25):** Risk does NOT materialize.
     > `dyn(1)` produces:
     >   - `type_map[root]` = `dyn` (the call site is typed as
     >     `dyn`, not `function`).  Note: cel-cpp's `ast.h`
     >     comment claims dyn entries are omitted to save space,
     >     but the call-site entry is materialised.
     >   - `reference_map[root]` = `name="dyn"`,
     >     `overload_id=["to_dyn"]`.
     >   - The `int` argument lives at `id=2` with
     >     `type_map[2]=int`.
     > `dyn(dyn(1))` recursively yields `dyn` for both call
     > nodes and `int` for the inner literal.
     > `dyn(1) == 1u` makes the outer `_==_` typed `bool` with
     > overload `equals`; the inner `dyn(1)` is typed `dyn` and
     > carries `to_dyn` in the reference map.  No
     > `function`-typed surprises.  Conclusion: ast-shape
     > matching (`node.has_call_expr() &&
     > node.call_expr().function() == "dyn" &&
     > node.call_expr().args().size() == 1`) is the right hook
     > and Risk #3's defensive fallback (special-case before
     > UnacceptableLabel dispatch) is unnecessary as a separate
     > branch — the special-case IS the dispatch hook.

  4. **Risk: `dyn(...)` with zero or 2+ args silently passes the
     special-case and gets emitted as a regular `kCall`.**
     **Mitigation:** the `args_size() == 1` guard plus the
     fallthrough path (no special case → goes through normal
     `UnacceptableLabel`) catches this.  Ill-formed `dyn` calls
     reach the normal type-check path which rejects on
     `function` type.

## Sequencing

  1. **Probe spike.**  Write a 30-line test that compiles
     `dyn(1)` end-to-end with `RejectDyn` disabled and inspects
     the resulting AST + type_map.  Confirms that cel-cpp's
     checker does what we expect.  ~30 min.
  2. **Frontend admission.**  Add the special case to
     `CheckSubsetNode`.  Add the 7 unit tests.  ~50 LoC.
  3. **ResolvePass annotation forward.**  Add the
     `dyn`-detection arm that copies argument annotations to
     the call node.  ~30 LoC + 2 unit tests.
  4. **Codegen identity.**  Either a special arm in
     `expr_lower.cc::Emit` (Option B) or rely on the
     ResolvePass forward (Option A — preferred).  ~10 LoC.
  5. **E2E + conformance.**  6 e2e tests; run full corpus,
     update `conformance/README.md`, update
     `conformance-unlock-plan.md`.

Total: ~120 LoC + ~15 tests.  Estimate: half-day.

## Future work that this enables

  - Once `dyn(scalar)` is admitted, the next obvious extension
    is `dyn(scalar_field_read)` — already covered by Risk #1's
    transitive admission.
  - The classifier tightening (Slice 3) becomes more useful —
    "type check failed on `dyn(...)`" reclassifies cleanly.
  - When M7 lands message-typed activation marshalling,
    `dyn(msg)` may become admissible; track as a follow-up.
