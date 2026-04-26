# Rewrite M5 — kCall built-in overload set

Status: **shipped 2026-04-25.**

What landed (closeout summary):

  - **M5.A** (recursive RejectDyn over container element types) —
    `UnacceptableLabel` recurses through list / map / abstract type
    parameters.
  - **M5.B step 1** (same-kind arithmetic + comparison) — full
    int / uint / double × add/sub/mul/div/mod/neg + per-kind
    eq/ne/lt/le/gt/ge.
  - **M5.B step 2** (cross-type numeric ladder + bool / string /
    bytes ordering tail) — `cel_numeric_*_at_vv` helpers + bool
    ordering + string/bytes le/gt/ge.
  - **M5.B step 2b** (polymorphic `equals` / `not_equals`) —
    `cel_equals_at_vv` dispatcher across all kinds.
  - **M5.C** (string + bytes ops) — concat / size / contains /
    startsWith / endsWith plus bytes equivalents.
  - **M5.D step 1** (aggregate kArena fast paths) — `_arena`
    helpers for list/map size/in/eq/concat.
  - **M5.D step 2** (kHost trampolines + kDynamic dispatchers +
    `cel_message_eq`) — Layer-2 Impls + Layer-3 wasmtime
    registrations + the polymorphic dispatcher.
  - **M5.E** (`OverloadTable::kBuiltinSeeds` populated + coverage
    tripwire) — every cel-cpp `StandardOverloadIds::k*` is either
    seeded or `kExplicitlyUnimplementedIds`.
  - **M5.F** (general kCall arm + `EmitGeneralCall`) — `string_view
    overload_id` from cel-cpp's reference_map drives a single
    dispatch site.  Receiver-form flattening for `s.contains(sub)`.
  - **M5.G** (control flow + 3VL — Slice 2 of conformance unlock
    plan) — `cel_and` / `cel_or` / `cel_not` 3VL helpers + ternary
    BinaryenIf-based lowering.

Conformance bumps over the milestone (post-M2 baseline 203 →
post-M5.G 509 → post-Slice-1.5/1.55/1.6 cumulative 664):
+461 PASS across all M5 + conformance-unlock-plan slices.

Plan-vs-execution deltas:

  - **Comprehension lowering** was scoped out of M5 mid-flight
    (2026-04-25).  Original plan §1.2 carved out the
    ResolvePass scope handler + `kComprehension` codegen arm +
    dynamic-list primitive (~3.5 sessions of work) into a
    follow-on milestone (`m5-comprehensions-followon.md` — not
    yet drafted).  M4's `ComprehensionDetector` early-reject in
    `ResolvePass` stays in place; conformance tests containing
    `kComprehensionExpr` continue to classify as SKIP until the
    follow-on lands.
  - **Cross-origin list concat / map equality** materialisation
    bodies POISON with `TYPE_MISMATCH` today.  Strategy is
    documented in §"Cross-origin materialisation" below;
    bodies land as M6 follow-up.

Three follow-on slices shipped on top of M5 to widen the
conformance unlock that M5.B step 2b made reachable:

  - **Slice 1.5** — `dyn(scalar)` static-subset admission
    (`dyn-passthrough-plan.md`); 509 → 562 (+53 PASS).
  - **Slice 1.55** — NaN-not-equal-self IEEE fix
    (`cel_numeric_ne_at_vv` returns true on `kCmpNanInequal`).
  - **Slice 1.6** — cross-numeric ordering / membership ladder
    (`cross-numeric-ordering-plan.md`); 562 → 664 (+102 PASS).

> M5.F as-shipped (2026-04-25): `EmitGeneralCall` lookup-by-overload-id
> + slot-out call emit landed in `compiler_v2/codegen/expr_lower.cc`;
> `LowerToEvalFunction` signature gained `const OverloadTable&`;
> `compile.cc` builds the table via `OverloadTableBuilder().Build()`
> and installs eager imports for every kCelRuntime helper that ships
> today.  M5.D step 2 (2026-04-25) wired the seven aggregate-op
> dispatchers (`cel_list_size` / `cel_list_in` / `cel_list_eq` /
> `cel_list_concat` / `cel_map_size` / `cel_map_in` / `cel_map_eq`)
> + standalone `cel_message_eq`: runtime dispatchers + extern
> decls + 8 Layer-2 Impls in `cel_host.cc` + 8 Layer-3 trampolines
> in `cel_host_wasmtime.cc` + linker `RegisterCelHostImports`.
> `kPendingRuntimeExports` was deleted; `InstallOverloadImports`
> special-cases the seven dispatcher names for arity inference;
> `BindAllRuntimeExports` adds the seven export names.  Cross-origin
> list concat / map equality currently POISON with TYPE_MISMATCH —
> materialisation strategy documented in §"Cross-origin
> materialisation" below; bodies land as M6 follow-up.
> `LayoutPass::AggregateStorageVisitor::PostVisitCall` now allocates
> a workspace slot for general-arm calls; control-flow ops
> (`_&&_` / `_||_` / `_?_:_` / `!_`) explicitly bypass slot
> allocation pending M5.G.  Engine (`compiler_v2/api/engine.cc`)
> binds 50+ helper exports off the runtime instance via a new
> `BindAllRuntimeExports` helper.  Receiver-form `s.contains(sub)`
> flattens `target` to `args[0]` so the wasm helper's
> `(out, s, sub)` signature matches the uniform `_at_vv` shape.
> Conformance: 207 → 391 PASS.  Test deltas: +9 expr_lower kCall
> lowering tests, +32 m5_test e2e tests, two updated layout_pass
> tests, one updated compile_test.
Comprehension lowering scoped out 2026-04-25; see
`doc/implementation-plan/rewrite/m5-comprehensions-followon.md`
(to be drafted).

Parent: `design.md` (this milestone covers Slice 5 + the deferred
half of Slice 6).
Predecessor: `m4-list-literals.md` (shipped 2026-04-25, the lists
half of the three-path dispatch contract).  Authoritative design
references:

  - `design.md §4.2` — uniform call ABI (`(out_slot, args…) -> void`).
  - `design.md §4.3` — `OverloadTable` builder + frozen table +
    `kBuiltinSeeds`.
  - `design.md §4.4` — codegen dispatch (general `kCall` arm).
  - `map-list-dispatch.md §6` — `size` / `in` / `==` / `+` reuse the
    three-path origin dispatch shipped at M3 / M4.
  - `third_party/cel-cpp/common/standard_definitions.h` — canonical
    overload-id list (~212 `constexpr absl::string_view k*` constants);
    every entry must be either mapped to a helper or marked
    `kExplicitlyUnimplemented` (per `design.md §4.5`).

**Scope rename note.** `m1-scalar-pipeline.md §10` originally
slotted "M5 — custom functions" here.  `design.md §11.4` re-ordered
custom functions to S7 (post-M5) so they ride the now-populated
`OverloadTable` rather than landing alongside the seed work.  M5
now ships the overload set + control flow + message equality —
**not** comprehensions, **not** customs, **not** proto literals.
Customs stay M6; proto literals stay M7; comprehension lowering
moves to a follow-on milestone (`m5-comprehensions-followon.md`)
that depends on M5's general kCall arm but is otherwise
independent.  Error provenance / structured `Error` matcher
splits out into a small standalone slice or M6, since the
work is orthogonal to the kCall arm.

## 0. Why M5 now, in one milestone

  - **Three-path dispatch is locked.**  M3 (maps) and M4 (lists)
    proved the kArena / kHost / kDynamic contract end-to-end.
    Every M5 aggregate overload (`size(list)`, `k in m`,
    `m1 == m2`, `[1]+[2]`) is one new row in the `OverloadTable`
    pointing at a helper that internally dispatches on operand
    `repr` + `*_origin` exactly like the existing `_[_]` arm.
  - **The conformance ceiling is gated on this milestone.**  M4
    landed +9 PASSes against an estimated +60–80 because most of
    `lists.textproto` is gated on `size(list)` / `==` / `in` /
    `+`; `comparisons.textproto` (406 tests, 0 PASS) needs the
    full equality + ordering matrix; `string.textproto` /
    `fp_math.textproto` / `integer_math.textproto` /
    `logic.textproto` are entirely M5 unlocks.  Rough M5 ceiling:
    ~+400–500 PASSes (212 → ~700).

**Why split this off from customs / proto literals /
comprehensions.**  Custom functions (M6) only need the
`OverloadTableBuilder::RegisterCustom` path that already exists;
once `kBuiltinSeeds` is populated, customs are *more* rows in
the same table — no design or codegen change.  Proto literals
(M7) need a separate host surface (`cel_make_message` /
`cel_set_field`) that doesn't share machinery with M5.
Comprehensions need a scope-aware resolver + a `kComprehension`
codegen arm + a dynamic-list primitive (3.5 sessions of work in
the original M5 plan) that all sit on top of the general kCall
arm but are otherwise self-contained — separating them lets M5
unblock the bulk of conformance immediately while the
comprehensions follow-on lands independently.

## 1. Scope

### 1.1 What works end-to-end after M5

```cpp
auto compiler = *cel::Compiler::NewBuilder()
    .DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"))
    .DeclareVariable("xs", CelType::List(CelType::Int()))
    .Build();
```

Compiles + evaluates:

  - **Arithmetic.**  `1 + 2 * 3` → `7`; `(1 << 62) + (1 << 62)`
    → `CEL_ERROR / CEL_ERR_OVERFLOW`; `1 / 0` →
    `CEL_ERR_DIVIDE_BY_ZERO`; `5 % 0` → `CEL_ERR_MODULUS_BY_ZERO`.
  - **Comparison.**  `1 == 1u` → `true` (cross-type numeric ladder
    per langdef); `1 < 2` → `true`; `"a" < "b"` → `true`;
    `[1,2] == [1,2]` → `true`; `{"a":1} == {"a":1}` → `true`;
    `c == c` → `true` (message equality).
  - **Logical (short-circuit).**  `false && (1/0 == 0)` → `false`
    (right operand never evaluated); `true || (1/0 == 0)` →
    `true`; `!true` → `false`.  3VL absorption per langdef:
    `unknown && false` → `false` (commutatively); `unknown ||
    true` → `true`.
  - **Ternary.**  `cond ? expr_a : expr_b` — only the chosen
    branch evaluates.
  - **String / bytes ops.**  `"abc" + "def"` → `"abcdef"`;
    `size("hello")` → `5`; `"abc".contains("b")` → `true`;
    `"abc".startsWith("a")` / `"abc".endsWith("c")` → `true`.
  - **Aggregate ops (kArena + kHost).**  `size([1,2,3])` → `3`;
    `2 in [1,2,3]` → `true`; `"k" in {"k": 1}` → `true`;
    `size(c.tags)` → proto repeated field size; `[1,2] + [3,4]`
    → `[1,2,3,4]`.
  - **`has()` already shipped at M2** — no new work.

### 1.2 Out of scope (deferred)

| Capability | Deferred to | Why |
|---|---|---|
| Comprehensions (`exists`, `all`, `exists_one`, `map`, `filter`) | comprehensions follow-on milestone | Scope-aware resolver + comprehension codegen + dynamic-list primitive (3.5 sessions of work in the original M5 plan).  Decoupled from kCall arm + control flow once Option B aggregate routing fixed at M5.E. |
| Custom functions (`Compiler::Builder::RegisterFunction` + `RuntimeBindings::AddFunction`) | M6 | Sits on top of the populated `OverloadTable`; no design change once seeds are in. |
| Proto literals (`Customer{name: "a"}`) | M7 | Separate host surface (`cel_make_message`, `cel_set_field`); orthogonal to kCall. |
| Structured `Value::Error` matcher / `eval_error` conformance matchers | small standalone slice (post-M5) | Conformance harness work; no codegen change needed. |
| Error provenance (`CelErrorPayload.expr_id`) | S10 | Bundled with Sethi–Ullman per `design.md`. |
| Sethi–Ullman slot allocation + debug-layout mode | S10 | Optimisation; correctness-first in M5. |
| `RejectDyn` tightening (catch implicit dyn from `[]` and `[1, "two"]`) | M5.A standalone slice (before main M5) | Surfaced as a follow-up in `m4-list-literals.md`; small enough to ride before the kCall work. |
| `cel_string_matches` (regex) | post-M5 small slice if conformance volume justifies | Need to pick a regex engine; deferring keeps M5 dependency-stable. |
| `timestamp` / `duration` arithmetic + constructors | post-M5 small slice | Specialised; not on the conformance critical path the same way scalar arith is. |
| `Activation::Bind` of `string` / `bytes` / `list<string>` | host-arena slice | Inherits the M2 host-arena gap (`IdentE2ETest::String` SKIPped). |

### 1.3 Envelope boundary probes

  - **Arithmetic overflow.**  `(1 << 62) + (1 << 62)` →
    `CEL_ERR_OVERFLOW`; `INT64_MIN * -1` likewise.
  - **Div / mod by zero.**  `1 / 0` → `CEL_ERR_DIVIDE_BY_ZERO`;
    `5 % 0` → `CEL_ERR_MODULUS_BY_ZERO`.  Per langdef
    §"Numeric values": double `0.0 / 0.0` → `NaN` (NOT an error
    — IEEE 754); cross-checked against cel-cpp.
  - **Cross-type numeric equality ladder.**  `1 == 1u` → `true`;
    `1 == 1.0` → `true`; `1u == 1.0` → `true`; `INT64_MAX
    == INT64_MAX_AS_DOUBLE` → spec edge — match cel-cpp.

## 2. Surfaces introduced in M5

### 2.1 `runtime/cel_runtime.{h,c}` — full helper set

Uniform slot-out ABI per `design.md §4.2`: every helper has wasm
signature `(i32 out_slot, i32 arg0, ..., i32 argN-1) -> void`.
Each helper carries a cel-cpp parity comment citing the source-of-
truth in `third_party/cel-cpp/runtime/standard/`.

**Arithmetic** (per scalar kind × per op):

```c
// int64
void cel_int_add_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_int_sub_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_int_mul_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_int_div_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_int_mod_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_int_neg_at_v(uint32_t out, uint32_t v);
// uint64 — unsigned overflow rules
void cel_uint_add_at_vv(uint32_t out, uint32_t a, uint32_t b);
... // sub/mul/div/mod
// double — IEEE 754; div-by-zero semantics defer to cel-cpp
void cel_double_add_at_vv(uint32_t out, uint32_t a, uint32_t b);
... // sub/mul/div/mod/neg
```

**Comparison** (full matrix per scalar kind, plus cross-type
numeric ladder):

```c
void cel_int_eq_at_vv(uint32_t out, uint32_t a, uint32_t b);  // → CEL_BOOL
void cel_int_lt_at_vv(uint32_t out, uint32_t a, uint32_t b);
... // ne / le / gt / ge per kind
// Cross-type numeric — checker dispatches `1 == 1u` to this.
void cel_numeric_eq_at_vv (uint32_t out, uint32_t a, uint32_t b);
void cel_numeric_lt_at_vv (uint32_t out, uint32_t a, uint32_t b);
... // full ladder
// Bool / null / type
void cel_bool_eq_at_vv(...);
void cel_null_eq_at_vv(...);  // null == null → true
```

**Logical (3VL)** — already shipped from v1 M4 Slice A.  M5
transcribes / verifies parity:

```c
void cel_and_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_or_at_vv (uint32_t out, uint32_t a, uint32_t b);
void cel_not_at_v (uint32_t out, uint32_t v);
void cel_unknown_merge(uint32_t out, uint32_t a, uint32_t b);
```

**String / bytes ops:**

```c
void cel_string_concat_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_string_size_at_v   (uint32_t out, uint32_t s);
void cel_string_eq_at_vv    (uint32_t out, uint32_t a, uint32_t b);
void cel_string_lt_at_vv    (uint32_t out, uint32_t a, uint32_t b);
void cel_string_contains_at_vv  (uint32_t out, uint32_t s, uint32_t sub);
void cel_string_starts_with_at_vv(uint32_t out, uint32_t s, uint32_t pfx);
void cel_string_ends_with_at_vv  (uint32_t out, uint32_t s, uint32_t sfx);
// cel_string_matches (regex) — deferred per §1.2.
// Bytes: same shape, kind-tagged variants.
void cel_bytes_concat_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_bytes_size_at_v   (uint32_t out, uint32_t b);
... // eq / lt
```

String concat `cel_alloc`s the joined payload in the arena;
caller's `out_slot` gets the resulting `{CEL_STRING, payload.s}`.
Same arena that `cel_reset` rewinds at the top of `$eval` —
strings produced during one Eval are alive for the rest of that
Eval but reset on the next.

**Aggregate ops** — three-path dispatch per `map-list-dispatch.md
§6`.  Each has a kArena fast path, a kHost trampoline, and a
kDynamic dispatcher; codegen routes via operand `*_origin`
exactly like the existing `_[_]` arm:

```c
// Arena fast paths.
void cel_list_size_arena(uint32_t out, uint32_t l);
void cel_list_in_arena  (uint32_t out, uint32_t v, uint32_t l);
void cel_list_eq_arena  (uint32_t out, uint32_t a, uint32_t b);
void cel_list_concat_arena(uint32_t out, uint32_t a, uint32_t b);
void cel_map_size_arena(uint32_t out, uint32_t m);
void cel_map_in_arena  (uint32_t out, uint32_t k, uint32_t m);
void cel_map_eq_arena  (uint32_t out, uint32_t a, uint32_t b);

// kDynamic dispatchers (with __attribute__((musttail)) arms).
void cel_list_size(uint32_t out, uint32_t l);
void cel_list_in  (uint32_t out, uint32_t v, uint32_t l);
... // and map equivalents

// kHost arms — declared in cel_runtime.c, defined host-side.
extern void cel_host_cel_list_size(...) __attribute__((import_module("cel_host"), ...));
extern void cel_host_cel_list_in  (...);
extern void cel_host_cel_list_eq  (...);
extern void cel_host_cel_map_size (...);
extern void cel_host_cel_map_in   (...);
extern void cel_host_cel_map_eq   (...);
```

> The dynamic list primitive (`cel_list_create_dynamic` /
> `cel_list_append` / `cel_list_grow`) and `cel_map_keys` —
> needed for comprehension iteration / `filter` accumulation —
> move to the comprehensions follow-on milestone alongside the
> `kComprehension` codegen arm.

### 2.2 `codegen/overload_table.{h,cc}` — `kBuiltinSeeds` populated

As-shipped (M5.E + M5.B step 2, 2026-04-25): `kBuiltinSeeds` has
80 entries; `kExplicitlyUnimplementedIds` has 86; the coverage
tripwire classifies every cel-cpp `StandardOverloadIds::k*`.
Originally planned: every overload id from `StandardOverloadIds`
mapped to a helper or marked `kExplicitlyUnimplemented`.  Rough
partition:

| family | helpers | rows |
|---|---|---|
| arithmetic | `cel_{int,uint,double}_{add,sub,mul,div,mod,neg}_at_v[v]` | ~25 |
| string concat | `cel_string_concat_at_vv`, `cel_bytes_concat_at_vv`, `cel_list_concat_*`, `cel_duration_add_*`, `cel_timestamp_add_dur_*` | ~10 |
| comparison | full eq / ne / lt / le / gt / ge × scalar kinds + cross-type | ~50 |
| logical | `cel_and` / `cel_or` / `cel_not` (special-cased — see §2.4) | 3 |
| size | `cel_string_size`, `cel_bytes_size`, `cel_list_size_*`, `cel_map_size_*` | ~6 |
| in | `cel_list_in_*`, `cel_map_in_*` | ~6 |
| string ops | `cel_string_{contains,starts_with,ends_with}_at_vv` | 3 |
| equality (aggregate) | `cel_list_eq_*`, `cel_map_eq_*`, `cel_host.cel_message_eq` | ~7 |
| explicitly unimplemented | regex `matches`, timestamp/duration arithmetic, type conversions | ~variable |

Coverage tripwire test in `overload_table_test.cc` per
`design.md §4.5`: iterate every `k*` in cel-cpp's
`StandardOverloadIds`, assert each is either resolvable via
`InternOverloadId` or in the explicit-unimplemented set.

### 2.3 `codegen/expr_lower.{h,cc}` — general `kCall` arm

As-shipped (M5.F, 2026-04-25): the general arm is wired; the
narrow `_[_]` arm coexists as a `function() == "_[_]"` special
case; the 7 pending dispatchers + control-flow ops still return
Unimplemented.  Originally planned shape:

```cpp
case cel::ExprKindCase::kCallExpr: {
  const cel::CallExpr& call = expr.call_expr();
  // Special control flow — short-circuit / ternary lower as wasm
  // branches, NOT via the slot-out helper ABI (§2.4).
  if (call.function() == "_&&_")  return EmitLogicalAnd(ctx, expr, ann, ...);
  if (call.function() == "_||_")  return EmitLogicalOr (ctx, expr, ann, ...);
  if (call.function() == "_?_:_") return EmitConditional(ctx, expr, ann, ...);

  // The narrow M3/M4 arm — kept as a special case; routes via
  // operand repr + *_origin per map-list-dispatch.md.
  if (call.function() == "_[_]")  return EmitKIndexCall(ctx, expr, call, ann);

  // General arm — every other CEL operator + receiver-style
  // method call (size, in, contains, …).  ResolvePass populated
  // ann.overload_id from cel-cpp's reference_map; the table maps
  // it to (module, helper_name) and codegen emits one wasm call.
  return EmitGeneralCall(ctx, expr, call, ann);
}
```

`EmitGeneralCall` is short:

```cpp
absl::StatusOr<BinaryenExpressionRef> EmitGeneralCall(
    EmitCtx& ctx, const cel::Expr& expr, const cel::CallExpr& call,
    const NodeAnnotation& ann) {
  const OverloadImpl* impl = ctx.overloads.Lookup(ann.overload_id);
  if (impl == nullptr) {
    return absl::UnimplementedError(
        absl::StrCat("expr_lower: overload_id=", ann.overload_id,
                     " (", call.function(),
                     ") not in OverloadTable — M5+ pending"));
  }
  // out_slot from layout; arg refs from recursive Emit on each child.
  std::vector<BinaryenExpressionRef> args;
  args.reserve(1 + call.args().size());
  args.push_back(I32Const(ctx.mod, ann.storage.payload));
  for (const cel::Expr& a : call.args()) {
    auto r = Emit(ctx, a);
    if (!r.ok()) return r.status();
    args.push_back(*r);
  }
  // Receiver-style: `s.size()` has target=s, args={}; emit target as args[0].
  if (call.has_target()) {
    auto t = Emit(ctx, call.target());
    if (!t.ok()) return t.status();
    args.insert(args.begin() + 1, *t);
  }
  return BinaryenCall(ctx.mod.raw(),
                      std::string(impl->name).c_str(), args.data(),
                      args.size(), BinaryenTypeNone());
}
```

### 2.4 Special control flow — `&&` / `||` / `?:`

Per langdef, `&&` / `||` short-circuit + absorb 3VL; `?:` only
evaluates the chosen branch.  These cannot be lowered as ordinary
slot-out calls because the helper ABI would force eager
evaluation of both operands.

**Lowering (sketch, real form lives in
`expr_lower.cc::EmitLogicalAnd`).**

```wat
;; a && b
<emit a> -> slot_a
;; If a is unknown / error, absorb without evaluating b.
;; If a is false, result is false; skip b.
;; Otherwise (a is true), evaluate b and call cel_and(out, a, b)
;; for the strict combination semantics.
(block $done
  (br_if $done (i32.eq <slot_a.kind> CEL_BOOL_FALSE))
  ;; … full 3VL absorption + b-eval + cel_and call
)
```

`EmitConditional` is the same shape — emit the predicate, branch
on its bool / unknown / error tag, lower only the chosen arm
into the result slot.  3VL invariant: `unknown ?: a : b` →
`unknown` (no branch evaluated); `error ?: a : b` → `error`.

**Why not just call `cel_and` always.**  cel-cpp's `cel_and`
helper does the right thing on values it sees, but it sees both
arguments — which means we'd evaluate them both eagerly and
violate short-circuit on side-effecting expressions.  Strict
calls work for pure expressions but lose the
`false && (1/0 == 0)` short-circuit guarantee that langdef
mandates.  Explicit branching is the only correct lowering.

### 2.5 Comprehension-related surfaces — deferred to follow-on milestone

> ResolvePass scope handler, `kComprehension` codegen arm, and
> LayoutPass comprehension-scope `PushScope` / `PopScope`
> semantics all move to the comprehensions follow-on milestone
> (`m5-comprehensions-followon.md`).  M4's
> `ComprehensionDetector` early-reject in `ResolvePass` stays in
> place through M5; conformance tests containing
> `kComprehensionExpr` continue to classify as SKIP until the
> follow-on lands.  See `design.md §10.2` for the
> scope-absorption design that the follow-on inherits.

### 2.6 `api/internal/cel_host.{h,cc}` — message equality

One new Layer-2 trampoline:

```cpp
// Message equality (langdef §"Equality" — MessageDifferencer
// semantics).  Lowers `_==_` between two CEL_MESSAGE operands.
absl::Status CelMessageEqImpl(uint32_t out_slot,
                              uint32_t lhs_slot, uint32_t rhs_slot,
                              const TrampolineContext& ctx);
```

Layer-3 wasmtime registration extends `RegisterCelHostImports`:
`cel_host.cel_message_eq` (3-arg), plus the six new aggregate ops
named in §2.1 (`cel_host.cel_list_size`, `cel_host.cel_list_in`,
`cel_host.cel_list_eq`, `cel_host.cel_map_size`,
`cel_host.cel_map_in`, `cel_host.cel_map_eq`).  `wasm_imports.txt`
grows by six lines.

> `CelMapKeysImpl` (map-key enumeration as `CEL_LIST_HOST`) was
> previously slated here for comprehension iteration; it moves to
> the comprehensions follow-on milestone.

### 2.7 Conformance harness

Envelope is unchanged: `IsInM4Envelope` already admits
`scalar_value:` / `map_value:` / `list_value:` matchers.  M5's
work is unblocking the *expressions* that produce those values —
arithmetic, comparison, control flow, etc. — so test rows
graduate from compile/eval failure to PASS.

`structured Error` matchers (`eval_error: { errors: { message:
"divide by zero" } }`) stay outside the envelope until the
follow-up slice that adds them.  M5 doesn't widen here.

## 3. Source layout (M5 deliverables)

```
compiler_v2/
├── runtime/
│   ├── cel_runtime.h                   + every helper from §2.1
│   ├── cel_runtime.c                   + bodies (cel-cpp parity)
│   ├── cel_arith.h                     NEW — arithmetic helpers split
│   ├── cel_compare.h                   NEW — comparison split
│   ├── cel_string_ops.h                NEW — string/bytes ops split
│   ├── cel_arith_test.cc               NEW — per kind × per op + parity
│   ├── cel_compare_test.cc             NEW
│   ├── cel_string_ops_test.cc          NEW
│   └── BUILD.bazel
├── codegen/
│   ├── overload_table.{cc,_test.cc}    + kBuiltinSeeds populated +
│   │                                     coverage tripwire
│   └── expr_lower.{cc,_test.cc}        + general kCall arm +
│                                         &&/||/?: control flow
├── api/
│   └── internal/
│       ├── cel_host.{h,cc,_test.cc}    + CelMessageEqImpl +
│       │                                 aggregate op impls
│       │                                 (size/in/eq for list &
│       │                                 map host paths)
│       └── cel_host_wasmtime.{h,cc}    + new trampoline registrations
└── e2e/
    └── m5_test.cc                      NEW — kCall e2e (mirror of
                                         m4_test.cc shape)
```

WAT traces 16-22 (per CLAUDE.md WAT-first rule); 19/20/25
land with M5.G / M5.D step 2; 23-24 reserved for the
comprehensions follow-on milestone:

  - `16_arith_int_add.wat` — `1 + 2` (kArena fast path).
    **Shipped (M5.B step 1).**
  - `17_compare_int_eq.wat` — `1 == 2` (slot-out comparison).
    **Shipped (M5.B step 1).**
  - `18_string_concat.wat` — `"a" + "b"` (arena allocation).
    **Shipped (M5.C).**
  - `19_logical_and.wat` — `false && expensive` (short-circuit).
    *Pending M5.G.*
  - `20_conditional.wat` — `cond ? a : b` (branch lowering).
    *Pending M5.G.*
  - `21_size_list.wat` — `size([1,2,3])` (kArena).
    **Shipped (M5.D step 1).**
  - `22_in_list.wat` — `2 in [1,2,3]`. **Shipped (M5.D step 1).**
  - `23_…` — reserved (comprehensions follow-on).
  - `24_…` — reserved (comprehensions follow-on).
  - `25_message_eq.wat` — `c == c` (host trampoline).
    *Pending M5.D step 2.*

Each shipped WAT has a walkthrough in `wat-traces.md`.

## 4. What gets ported from v1 / v2

  - **3VL helpers** — `cel_and` / `cel_or` / `cel_not` /
    `cel_unknown_merge` already shipped in v1 M4 Slice A.
    Verify the bodies are at v2 parity and add the cel-cpp
    parity pointer if missing.
  - **Existing arena primitives** — cel_alloc / cel_reset / span
    payload patterns from M1 stay; string concat builds on top.
  - **Three-path dispatch pattern** — copy-paste from M3/M4 for
    every aggregate overload (size / in / eq / concat).  Layer
    1/2/3 split + `__attribute__((musttail))` toolchain flags
    already in place.

## 5. Work breakdown (order of authoring)

8 slices.  Each: WAT → assemble + `wat_runner` → unit tests →
e2e through `Instance::Eval` → milestone doc updated.

1. **M5.A — `RejectDyn` tightening (small standalone, before main
   M5).**  ~~Plan.~~ **Shipped 2026-04-25.**
   `UnacceptableLabel` in `frontend/parse_and_check.cc` now
   recurses through `list_type().elem_type()`,
   `map_type().{key,value}_type()`, and
   `abstract_type().parameter_types()`.  Two TODO tests in
   `m4_test.cc::ListRejectionE2ETest` flipped to expect rejection
   (`BareEmptyListLiteralRejected`, `HeterogeneousListRejected`).
   Four `{}`-bearing tests in `compile_test`, `expr_lower_test`,
   `layout_pass_test`, `instance_test` either flipped (compile_test
   `EmptyMapLiteralRejected`, `RuntimeImportsAlsoPresentForLiteralOnlyMap`
   switched to `{"a": 1}`) or were retired with a TODO pointing to
   the comprehensions follow-on milestone (the N=0 codegen path
   stays reachable through comprehension `accu_init = {}` lowering).
   Conformance: 212 → 207 (5 rows that previously slipped through
   now correctly fail-compile; recovered by M5.B-G + the follow-on).

2. **M5.B — Runtime helpers: arithmetic + comparison.**
   ~~Plan.~~ **Step 1 shipped 2026-04-25** (same-kind matrix +
   ABI lock):

     - `cel_arith.h` (int / uint / double × add/sub/mul/div/mod
       + neg for int/double) and `cel_compare.h` (per-kind
       eq/ne/lt/le/gt/ge + bool eq/ne + null eq).  Bodies in
       `cel_runtime.c`; cel-cpp parity citations on the helper
       block.
     - WAT 16 (`arith_int_add`) + WAT 17 (`compare_int_eq`)
       lock the slot-out helper ABI shape `(out_slot, args…) → ()`.
       Both run end-to-end through `wat_runner` against the real
       runtime export — `1+2=3`, `1==2=false`.
     - 39 unit tests across `cel_arith_test` + `cel_compare_test`
       (happy path × overflow × div-by-zero × IEEE NaN/Inf
       × type-mismatch × 3VL absorption).
     - `uint64_mul_overflows` / `int64_mul_overflows` use a
       split 32×32→64 partial-product shape.  cel-cpp's
       `__builtin_mul_overflow` lowers through `__multi3` (a
       compiler-rt 128-bit multiply) which the wasm32 freestanding
       build doesn't link.  The split form keeps every multiply
       at i64.mul granularity that the wasm32 backend lowers
       natively.
     - 47 helper exports added to `cel_runtime.wasm`'s `--export`
       list and the `wat_runner` `kRuntimeExports` array.

   **Step 2 shipped 2026-04-25** — cross-type numeric ladder +
   bool / string / bytes ordering tail:

     - 6 cross-type numeric helpers (`cel_numeric_{eq,ne,lt,le,
       gt,ge}_at_vv`) implementing the int↔uint↔double ladder
       per langdef §"Equality" / §"Comparison".  cel-cpp parity
       comments cite the source-of-truth.
     - 4 bool ordering helpers (`cel_bool_{lt,le,gt,ge}_at_vv`)
       via the existing `DEFINE_CMP_VV` macro.
     - 6 string / bytes ordering tail helpers
       (`cel_string_{le,gt,ge}_at_vv` +
       `cel_bytes_{le,gt,ge}_at_vv`) via a new `DEFINE_SPAN_CMP_VV`
       macro.
     - 34 ids moved from `kExplicitlyUnimplementedIds` to
       `kBuiltinSeeds`; seed count 46 → 80; unimplemented
       count 120 → 86.
     - 64 new test cases across `cel_compare_test.cc`
       (cross-type numeric + bool ordering) and
       `cel_string_ops_test.cc` (le/gt/ge tail).
     - One cel-cpp parity edge case mirrored verbatim:
       `kGreaterEqualsUintDouble = "greater_equals_uint_double"`
       (typo missing `64`) at
       `third_party/cel-cpp/common/standard_definitions.h:212`.

   **Step 2b (open):** polymorphic `equals` / `not_equals`
   overloads — depend on M5.D step 2 (aggregate eq +
   `cel_message_eq`); registers the cross-kind eq dispatcher
   used by `dyn == dyn` and the message-vs-message equality
   path.  Randomised-fixture cel-cpp parity sweep + structured
   `Error` payload matchers also tracked here.

3. **M5.C — Runtime helpers: string + bytes ops.**
   ~~Plan.~~ **Shipped 2026-04-25.**

     - `cel_string_ops.h` — string concat / size / eq / lt /
       contains / startsWith / endsWith + bytes concat / size /
       eq / lt (no contains/startsWith/endsWith on bytes per
       langdef).
     - Bodies in `cel_runtime.c` share span helpers
       (`span_eq` / `span_lt` / `span_contains` /
       `span_match_at`) so each per-helper body is two lines.
     - WAT 18 (`string_concat`) locks the arena-alloc slot-out
       shape — concat is the only M5.C helper that calls
       `cel_alloc`; the rest read operand spans without
       allocating.
     - 18 unit tests in `cel_string_ops_test.cc` covering
       happy path, empty / multi-byte UTF-8 / embedded-NUL
       boundaries, byte-vs-string lt unsigned ordering, and
       3VL / type-mismatch envelope.
     - 11 helper exports added to `cel_runtime.wasm` and
       `kRuntimeExports`.
     - Regex `matches` deferred per §1.2.

4. **M5.D — Aggregate op helpers + cel_host_cel_*_size /
   /_in / _eq + cel_message_eq.**  Three-path dispatch (kArena
   fast path + kHost trampoline + kDynamic dispatcher with
   `__attribute__((musttail))`).  Per `map-list-dispatch.md §6`.

   **Step 1 shipped 2026-04-25** — kArena fast paths only:

     - 7 helpers added (`cel_list_size_arena`, `cel_list_in_arena`,
       `cel_list_eq_arena`, `cel_list_concat_arena`,
       `cel_map_size_arena`, `cel_map_in_arena`,
       `cel_map_eq_arena`); decls split between `cel_list.h`
       and `cel_map.h`, bodies in `cel_runtime.c`.
     - Shared scalar matcher `cel_value_eq` (forward-declared
       across the file boundary): builds on the existing
       `map_keys_equal` ladder (int↔uint cross-type,
       bool/string), adds same-kind double / bytes / null
       branches.  Cross-type numeric involving double defers
       to M5.B step 2's `cel_numeric_*` ladder.
     - WAT 21 (`size_list`) + WAT 22 (`in_list`) lock the
       1-operand and 2-operand slot-out shapes; both run
       end-to-end through `wat_runner` against the real
       runtime export.
     - 22 unit tests in `cel_aggregate_arena_test.cc`
       (per-helper happy path × empty boundary × cross-type
       numeric `in` × map order-irrelevance × 3VL absorption
       × type-mismatch).
     - 7 helper exports added; `kRuntimeExports` grew to 65.

   **Step 2 (open):** kDynamic dispatchers (with
   `__attribute__((musttail))` arms mirroring M3/M4) +
   7 kHost trampolines (cel_list_size, cel_list_in,
   cel_list_eq, cel_map_size, cel_map_in, cel_map_eq,
   cel_message_eq) + Layer-2 Impls in `cel_host.cc` +
   Layer-3 wasmtime registrations extending
   `RegisterCelHostImports`.  `cel_map_keys` (formerly
   bundled here for comprehension iteration) moves to the
   comprehensions follow-on milestone.

5. **M5.E — `OverloadTable::kBuiltinSeeds` populated + coverage
   tripwire green.**  ~~Plan.~~ **Shipped 2026-04-25.**

     - `kBuiltinSeeds` populated with 46 entries: arithmetic
       same-kind (16 — int / uint / double × add/sub/mul/div/mod
       plus int/double neg) + concat (3 — string / bytes / list)
       + same-kind ordering (14 — int/uint/double × lt/le/gt/ge
       plus less_string / less_bytes) + container size (8 —
       function-form `size(_)` and member-call `_.size()` ids
       across string / bytes / list / map) + container in (2 —
       list / map) + string ops (3 — contains / startsWith /
       endsWith).  Each row points at a `cel_runtime` helper
       shipped in M5.B step 1 / M5.C / M5.D step 1.
     - `kExplicitlyUnimplementedIds` populated with 120 entries
       covering every `StandardOverloadIds::k*` not in
       `kBuiltinSeeds`: special-cased in `expr_lower` (10 —
       short-circuit `&&` / `||`, ternary `?:`, indexing `_[_]`,
       polymorphic `equals` / `not_equals`, comprehension-
       internal `not_strictly_false`); cross-type numeric ladder
       (deferred to M5.B step 2); bool / string / bytes ordering
       tail (`le` / `gt` / `ge` for bytes / string and bool
       ordering — also M5.B step 2); timestamp / duration
       arithmetic + ordering; regex `matches`; timestamp /
       duration accessors; type conversions.
     - New header symbol `OverloadTableIsExplicitlyUnimplemented`
       exposes the unimplemented set so the tripwire can
       classify every cel-cpp id.
     - Coverage tripwire test
       `CoverageTripwireClassifiesEveryStandardId` iterates
       every `cel::StandardOverloadIds::k*` constant and asserts
       each is either resolvable via `InternOverloadId` or in
       `kExplicitlyUnimplementedIds` — forcing-function for the
       next vendoring of cel-cpp; silent additions no longer go
       unnoticed.
     - `BUILD.bazel` adds a dep on
       `@cel-cpp//common:standard_definitions` for the tripwire.
     - 13 tests pass (12 existing + the new tripwire); the
       existing custom-registration tests now use a test-local
       `kBuiltinSeedCount = 46` constant instead of asserting
       interning starts at 1.
     - Pure data change; conformance unchanged at 207 PASSes
       (M5.F's general kCall arm is what wires these into the
       pipeline).

   > Post-M5.B-step-2 delta (2026-04-25): seed count 46 → 80
   > and unimplemented count 120 → 86 after step 2 moved the
   > cross-type numeric ladder + bool / string / bytes
   > ordering tail rows from unimplemented to seeded.  The
   > tripwire still classifies every `StandardOverloadIds::k*`.

6. **M5.F — General `kCall` arm in `expr_lower.cc`.**
   ~~Plan.~~ **Shipped 2026-04-25.**

     - `NodeAnnotation::overload_id` changed from `uint32_t`
       to `absl::string_view` (`compiler_v2/ir/annotations.h`)
       so codegen can name helpers directly out of the resolved
       overload id without a separate intern step.
     - `OverloadIdResolver` visitor in `resolve_pass.cc` walks
       every kCallExpr and stamps cel-cpp's resolved overload
       string from `Ast::reference_map`.
     - `EmitGeneralCall` in `expr_lower.cc` looks up
       `ann.overload_id` in `OverloadTable`, emits
       `(call $<helper> (i32.const out_slot) <args...>)` per
       §2.3.  Receiver-form `s.contains(sub)` flattens
       `target` to `args[0]` so the wasm helper's
       `(out, s, sub)` signature matches the uniform `_at_vv`
       shape.
     - `kPendingRuntimeExports` set guards 7 dispatcher names
       not yet shipped (`cel_list_size`, `cel_list_in`,
       `cel_list_eq`, `cel_list_concat`, `cel_map_size`,
       `cel_map_in`, `cel_map_eq`) — `EmitGeneralCall` returns
       Unimplemented for those rather than emitting a
       link-failing import.  The set shrinks to empty when M5.D
       step 2 ships.
     - `LayoutPass::AggregateStorageVisitor::PostVisitCall`
       extended to allocate a workspace slot for general-arm
       kCalls; control-flow ops (`_&&_` / `_||_` / `_?_:_` /
       `!_`) bypass slot allocation per the M5.G plan.
     - `compile.cc::InstallOverloadImports` installs eager
       imports for every shipped helper (skipping the 7
       pending dispatchers).
     - `engine.cc::BindAllRuntimeExports` binds 50+ runtime
       exports onto the wasmtime linker.
     - 9 new unit tests in `expr_lower_test.cc`; 32 new e2e
       tests in new `compiler_v2/e2e/m5_test.cc` covering
       arithmetic / same-kind comparison / string ops / bytes
       ops / bound-var arithmetic / proto-field arithmetic /
       pending-dispatcher guards / control-flow guards.
     - Conformance: 207 → 391 PASS (+184).

7. **M5.G — Special control flow: `&&` / `||` / `?:`.**  Branch-
   style emission per §2.4.  3VL absorption locked via
   `PartialEval` tests covering `unknown && false → false`
   commutatively, `error ?: a : b → error`, etc.  Conformance:
   `logic.textproto` graduates entirely (~30 PASSes).

8. **M5.H — conformance reconcile + doc reconcile + milestone
   close.**  M5.F already landed `m5_test.cc` (32 tests across
   the kCall arm's reachable surface); M5.H grows it as M5.G +
   M5.D step 2 ship.  Re-run `run_conformance`; expect 391 →
   ~700-800 PASSes (per §6.3) once M5.D step 2 + M5.B step 2b +
   M5.G land.  Update `conformance/README.md` inventory.  Tick
   design.md S5 + S6 (msg-eq half).  Mark this doc
   `Status: shipped`.

Each slice leaves the test suite green (default + manual);
each can be reverted independently.  The ResolvePass scope
handler + `kComprehension` codegen arm + dynamic-list primitive
(formerly slices M5.H–M5.I) move to the comprehensions follow-on
milestone (`m5-comprehensions-followon.md`).

## 6. Test plan

### 6.1 Unit (per file — see `per-component-test-coverage.md §3`)

  - `cel_arith_test` — per kind (`int`/`uint`/`double`) × per op
    (`add`/`sub`/`mul`/`div`/`mod`/`neg`).  Boundary values
    (`INT64_MIN`, `INT64_MAX`, `UINT64_MAX`, `0`, `-1`, NaN, Inf,
    `-0.0`).  Overflow / div-by-zero → `CEL_ERR_*` per langdef.
    Cel-cpp parity: ~50 fixture pairs cross-checked.
  - `cel_compare_test` — same matrix for eq / ne / lt / le / gt /
    ge.  Cross-type numeric ladder explicit (`1 == 1u`,
    `1 < 1.0`, `INT64_MAX == INT64_MAX_AS_DOUBLE`).
  - `cel_string_ops_test` — concat round-trip per length (0, 1,
    7, 8, 9 bytes — alignment edges); contains / startsWith /
    endsWith with empty / overlap / no-match cases; size in
    bytes (langdef pins UTF-8 byte-count).
  - `cel_runtime_test` (aggregate ops) — `cel_list_size_arena` /
    `cel_list_in_arena` / `cel_list_eq_arena` per element kind +
    boundary indices.  kDynamic dispatcher tail-call test (loop
    N times, observe stack-pointer doesn't grow — same shape as
    M3's map dispatcher test).
  - `overload_table_test` — coverage tripwire: every
    `StandardOverloadIds::k*` is mapped or in
    `kExplicitlyUnimplemented`.  `RegisterCustom` collision
    rules unchanged.  `LookupById` returns the seeded impl per
    overload id.
  - `expr_lower_test` — `kCall` general arm × per overload
    family; `kCall(_&&_)` short-circuit lowering; `kCall(_?_:_)`
    branch lowering.  Each verified against the matching WAT
    trace byte-for-byte (modulo Binaryen-assigned names).
  - `cel_host_test` — `CelMessageEqImpl` against two
    `Customer` fixtures; aggregate ops on each backing.

### 6.2 E2E (`compiler_v2/e2e/m5_test.cc` — new)

Mirror of `m4_test.cc` shape; ~50 tests across:

  - **`ArithmeticE2ETest`** — per scalar kind × per op × boundary
    (8 ops × 3 kinds × 5 boundaries ≈ 120 cases consolidated to
    ~25 parameterised TESTs).
  - **`ComparisonE2ETest`** — same matrix for eq / ordering;
    explicit cross-type numeric tests.
  - **`LogicalE2ETest`** — short-circuit invariants (right
    operand never evaluates when left short-circuits, observable
    via a side-effecting `1/0` predicate); 3VL absorption under
    `Instance::Eval` and `Instance::PartialEval`.
  - **`ConditionalE2ETest`** — `?:` × scalar / list / map /
    message branch types; only chosen branch evaluates; 3VL.
  - **`StringOpsE2ETest`** — concat / size / contains /
    startsWith / endsWith over UTF-8 inputs (multi-byte
    boundaries).
  - **`AggregateOpsE2ETest`** — `size` / `in` / `==` over
    kArena and kHost lists/maps; `+` (concat) on lists.
  - **`MessageEqE2ETest`** — proto message equality via
    `cel_host.cel_message_eq`; nested message; field-order
    irrelevance per `MessageDifferencer`.

Per `per-component-test-coverage.md` SKIP rule: **no
fixture-level GTEST_SKIPs** — every test runs green or has a
single-test deferral with a tracked follow-up.

### 6.3 Conformance unlock

Original projection: 212 → ~700 PASSes.  As shipped through
M5.F: **207 → 391 PASS (+184)** — most of the M5.F-reachable
unlock landed; the remaining gap to ~700-800 sits behind
M5.D step 2 (aggregate dispatchers + msg-eq), M5.B step 2b
(polymorphic equals / not_equals), and M5.G (short-circuit /
ternary).  Comprehension-driven fixtures (`macros.textproto`,
`macros2.textproto`, comprehension rows in `parse.textproto`)
graduate in the comprehensions follow-on milestone, not M5.

| fixture | pre-M5 | post-M5.F (actual) | post-M5 estimate | remaining unlock driver |
|---|---|---|---|---|
| `lists.textproto` | 4 / 39 | partial | ~35 / 39 | M5.D step 2 (`size` / `in` / `==` / `+` for non-arena lists) |
| `comparisons.textproto` | 0 / 406 | major (cross-type ladder live) | ~380 / 406 | M5.B step 2b polymorphic eq for aggregates |
| `string.textproto` | 0 / 51 | most | ~45 / 51 | regex `matches` deferred |
| `fp_math.textproto` | 0 / 30 | most | ~28 / 30 | conversion edges |
| `integer_math.textproto` | 0 / 64 | most | ~60 / 64 | conversion edges |
| `logic.textproto` | 0 / 30 | 0 (still) | 30 / 30 | M5.G short-circuit + 3VL |
| `parse.textproto` | 150 / 219 | partial | ~165 / 219 | non-comprehension rows graduate |
| `basic.textproto` | 38 / 43 | ~41 / 43 | 41 / 43 | a few size/list rows graduate |

Actual M5.F unlock total: **207 → 391 PASS (+184)**.
Remaining projection: 391 → ~700-800 across M5.D step 2 +
M5.B step 2b + M5.G.

### 6.4 Closeout gate

Per `per-component-test-coverage.md §5`:

```
M5 closeout

[ ] All per-component _test.cc files written (per §3)
[ ] No fixture-level GTEST_SKIPs added to e2e/m5_test.cc
[ ] M4-era ListRejectionE2ETest TODO tests flipped (M5.A)
[ ] bazel test //compiler_v2/... passes
[ ] scripts/run_full_suite.sh passes (default + manual targets
    + m5_test.cc registered)
[ ] m5_test runs green (no fixture skips)
[ ] bazel run //compiler_v2/conformance:run_conformance —
    README inventory refreshed; per-fixture moves documented;
    +400 PASSes minimum vs M4
[ ] testing-checklist.md "Rewrite M5" rows ticked
[ ] m5-kcall-comprehensions.md status header reflects shipping
[ ] design.md S5 + S6 (msg-eq half) marked SHIPPED
[ ] WAT traces 16-22 + 25 exist and are exercised by
    wat_runner_test (16/17/18/21/22 shipped; 19/20 land with
    M5.G; 25 lands with M5.D step 2; 23-24 reserved for
    comprehensions follow-on)
[ ] OverloadTable coverage tripwire green
[ ] Cross-type numeric equality ladder cited per langdef in
    cel_compare.h
[ ] Per-helper cel-cpp parity comment present in cel_runtime.h
```

## 7. Rough size estimate

  - **M5.A** (RejectDyn tighten): ~0.5 sessions.  Mechanical.
  - **M5.B** (arith + compare runtime): ~2 sessions.  Volume.
  - **M5.C** (string ops): ~1 session.
  - **M5.D** (aggregate ops × three-path): ~2 sessions.  Mirrors
    M3+M4 work but for 4 new helpers each.
  - **M5.E** (kBuiltinSeeds population): ~0.5 sessions.  Data.
  - **M5.F** (general kCall arm): ~1 session.  Codegen wiring.
  - **M5.G** (control flow): ~1 session.  Tricky 3VL invariants.
  - **M5.H** (e2e + conformance + doc): ~1 session.

**Shipped 2026-04-25:** M5.A + M5.B step 1 + M5.B step 2 +
M5.C + M5.D step 1 + M5.E + M5.F (~5 sessions spent).

**Remaining: ~3.8 sessions:**

  - **M5.D step 2** (kDynamic dispatchers + 7 kHost
    trampolines + Layer-2 Impls + Layer-3 wasmtime
    registrations + `cel_message_eq`): ~1.5 sessions.
  - **M5.B step 2b** (polymorphic `equals` / `not_equals`,
    depends on D step 2): ~0.3 sessions.
  - **M5.G** (control flow `&&` / `||` / `?:` + 3VL
    invariants): ~1 session.
  - **M5.H** (e2e + conformance + doc reconcile +
    milestone close): ~1 session.

Comprehension lowering (originally M5.H + M5.I + the
comprehension-half of M5.J) split out to the follow-on
milestone — was ~4.5 of the original 12 sessions.

## 8. Risks + open questions

1. **Overload coverage scope creep.**  cel-cpp's
   `StandardOverloadIds` has ~212 entries; not every one has a
   stable cel-cpp implementation today (some are checker-only
   placeholders).  Mitigation: explicit
   `kExplicitlyUnimplemented` set covering anything not in the
   §1.2 in-scope list (`cel_string_matches`, timestamp /
   duration arithmetic, type conversions); coverage tripwire
   asserts each id is classified.

2. **Short-circuit + 3VL interaction.**  langdef's truth table
   for `&&` / `||` is non-trivial: `unknown && false → false`
   commutatively (so `false && unknown → false`).  Plain wasm
   short-circuit gives only the strict path; the 3VL path
   needs to evaluate both arms and call `cel_and` /
   `cel_or` to get the absorption right.  Mitigation: the
   §2.4 lowering picks short-circuit when the left operand
   is concrete (bool); falls through to evaluate-both +
   `cel_and` when left is unknown / error.  Locked under
   `PartialEval` tests that exercise both shapes.

3. **cel-cpp parity for arithmetic edge cases.**  Int overflow
   semantics (langdef pins overflow → ERROR for int / uint;
   double is IEEE 754); modulus sign rules; double NaN
   propagation through comparisons.  Mitigation: every helper
   carries a parity comment; M5.B's slice runs a randomised-
   fixture cross-check against cel-cpp's interpreter.

## 9. Dependencies + sequencing

  - **Hard prereqs (already shipped):**
      - M2 (idents / kSelect / has / PartialEval) — 2026-04-25.
      - M3 (maps + tail-call dispatcher + cel_host scaffolding)
        — 2026-04-24.
      - M4 (lists + activation marshal kList + envelope flip)
        — 2026-04-25.
  - **M5 unblocks:**
      - **The comprehensions follow-on milestone**
        (`m5-comprehensions-followon.md`) — sits on M5's
        general kCall arm + populated OverloadTable, then
        adds the ResolvePass scope handler, `kComprehension`
        codegen arm, dynamic-list primitive, and
        `cel_map_keys`.  Now ships independently of M5.
      - **M6 (custom functions)** — `RegisterCustom` already
        works; M6 is plumbing to expose it to embedders + the
        per-function wasm import emit.  Sits on M5's populated
        OverloadTable.
      - **Sethi–Ullman optimisation (S10)** — runs against the
        feature-complete v2 once M5 + the comprehensions
        follow-on close.
      - **Structured `Error` matcher slice** — small, can run
        in parallel with M6 / M7.
  - **M7 still independent of M5** — proto literals
    (`cel_make_message` / `cel_set_field`) don't reuse the
    OverloadTable; can ship out of order.
  - **Single-test SKIPs from M2 still inherited:** `IdentE2ETest::String`
    / `Bytes` (host-arena allocator).  M5 doesn't fix these;
    `Activation::Bind("xs", Value::List({Value::String(...)}))`
    inherits the same gap and stays a single-test skip until
    the host-arena work lands.
  - **M5.A (RejectDyn) precedes the main M5 work** since the
    static-subset gate change is small + standalone, and the
    M4 follow-up calls for it.  Could also ship after M5
    if scheduling pressure favours; flagged as M5.A for
    natural ordering.

---

## Cross-origin materialisation (design)

Mixed-origin aggregate operations (`a + b` for one arena + one
host list, `a == b` for one arena + one host map, etc.) are
NOT walked in place across heterogeneous storage.  Instead, the
host operand is **materialised into the arena** and the existing
arena+arena fast path runs over the unified result.  Concretely,
for list concat:

  1. Allocate a fresh `ArenaListHeader` + elements run via
     `cel_alloc`, sized `a_size + b_size`.  `ArenaAllocator::Alloc`
     reenters wasm for `cel_alloc`, so this works from inside a
     host trampoline.
  2. For each operand:
       - If `CEL_LIST_ARENA`: memcpy its elements run into the
         destination at the appropriate offset.
       - If `CEL_LIST_HOST`: walk via `backing->ForEach` (or
         `At(i)` over `[0..Size())`), encode each `cel::Value` to
         a `CelValue` (`EncodeBackingScalar` extended for
         aggregates), and write into the destination.
  3. Write `{kind:CEL_LIST_ARENA, arena_list.header_ptr=hdr_off}`
     into `out_slot`.  The result is observably an arena list,
     keeping downstream codegen on the arena fast path.

The same lift-then-walk pattern applies to mixed-origin map
equality (`CelMapEqImpl`) and any future operator that needs to
walk both operands as one origin: lift host into arena, then run
the arena fast path.  Element/value encoding gracefully degrades
to scalars in M5; aggregate-element materialisation (lists of
lists, maps of messages) lands at M6 alongside the broader
nested-aggregate equality story.

**M5.D step 2 ship state.**  The `_arena` fast paths handle the
common same-origin cases.  The kHost trampolines implement
host-only operations via the externref backings.  Cross-origin
concat and cross-origin map equality currently POISON with
`TYPE_MISMATCH`; the materialisation body lands as a follow-up
in M6 (or earlier) without changing the dispatcher contract.
Codegen does not need to know — every dispatcher call site sees
the same `out_slot` slot-out shape.

## Future work (will be appended at close)

Filled in as M5 ships.  Anything surfaced during execution
that wasn't in the as-written plan goes here per CLAUDE.md
"Closing out a planning doc" rule.

  - **Mixed-origin list concat / map equality materialisation.**
    Strategy is documented above; bodies POISON with
    `TYPE_MISMATCH` in M5.D step 2.  Lift-and-walk lands as a
    M6 (or earlier) follow-up; same lift helper unblocks
    nested-aggregate element equality in `CelListInImpl` /
    `CelListEqImpl` / `CelMapEqImpl`.
