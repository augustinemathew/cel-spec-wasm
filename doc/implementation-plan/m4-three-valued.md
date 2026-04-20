# M4 — Three-valued logic (OK / UNKNOWN / ERROR)

Status: **slices A+B+C+D+E1 shipped (2026-04-20); slice E2 (host
UNKNOWN) and slices F-G in progress.**
Unblocked — the per-type and per-`ExprKindCase` codegen surface M3
closed is exactly what the 3VL plumbing threads through, so nothing
upstream blocks this.

**Ordering note (2026-04-19): M4 and M5 were swapped.**  Originally
M4 was collections + comprehensions and M5 was three-valued logic,
with the rationale that the comprehension lowering was the first
place error propagation had multiple branch points in one
expression.  That was an M4-internal observation; the stronger
ordering constraint turned out to be that the §8.2 host ABI is
leaking semantics the compiler cannot rely on at compile time —
every `get_field` can return UNKNOWN / ERROR, and today codegen has
no story for threading that status.  The right fix is to finish
three-valued logic first so the ABI has something well-defined to
hand back, then build collections on top.  Collections moved to
M5 (`m5-collections-and-comprehensions.md`).

## Scope

Make the compiler produce CEL's normative three-valued semantics
end-to-end.  Up to this point, every lowered expression trusts its
inputs and emits straight-line WASM; M4 inserts the
check/propagate/short-circuit plumbing.

Post-M4 these expressions evaluate to **ERROR**, not panic / not
trap:

  - `1 / 0` → ERROR (divide-by-zero).
  - `2 ^ 63 - 1 + 1` → ERROR (signed overflow per §langdef).
  - `"a" < 1.0` → checker-error, not runtime (this is the negative
    case that confirms the checker still catches what it should).
  - `NaN < 1.0` → ERROR (NaN-unordered).

Post-M4 these evaluate to **UNKNOWN**:

  - When `request.user` is marked unknown by the host,
    `request.user.name == "alice"` returns `UnknownSet{request.user}`.
  - `unknown && false` → `false` (short-circuit beats unknown).
  - `unknown || true` → `true`.
  - `unknown && unknown` → `unknown` (merged set).

## Deliverables

### Runtime

- [x] `CelKind::CEL_UNKNOWN` + `CEL_ERROR` constructors + payload
      helpers — pre-existing from M2, reused unchanged.
- [x] `UnknownSet` — a sorted `i32[]` of attribute ids.  Attribute
      ids come from the `cel.abi.attributes` table interned at
      compile time.  Runtime layout: `{ids_ptr:u32, ids_len:u32}`
      at `CelValue.payload.unk`.
- [x] `cel_and(uint32_t a, uint32_t b)` — short-circuits OK(false)
      past ERROR/UNKNOWN; ERROR dominates UNKNOWN otherwise; two
      UNKNOWNs fold via `cel_unknown_merge`.  (M4 slice A.)
- [x] `cel_or` — symmetric (short-circuits OK(true)).
- [x] `cel_not` — `OK(b) → OK(!b)`, `ERR → ERR`, `UNK → UNK`.
- [x] `cel_status_either(a, b)` — ERROR > UNKNOWN > OK dominance
      with left-wins tie-breaking.  Returns 0 when both operands
      are OK, signalling "proceed with arithmetic".  (M4 slice A.)
- [x] `cel_unknown_merge(a, b)` — sorted-dedup'd union of two
      UnknownSets, deterministic (order-independent).  (M4 slice A.)

**M4 slice A (2026-04-19): 3VL runtime helpers.** All six helpers
live in `compiler/runtime/cel_runtime.{h,c}` and return the arena
offset of the result `CelValue` (or 0 on type error / OOM, matching
the rest of the ABI).  The merge walk is factored into a static
`merge_sorted_ids` helper so `cel_unknown_merge` stays under the
function-size lint gate.  Coverage: `cel_runtime_test.cc` runs full
5×5 parametric truth tables for `cel_and` / `cel_or` over
{TRUE, FALSE, ERROR, UnknownA, UnknownB}, plus the usual
positive/negative cases for merge (determinism + dedup),
`cel_not` (bool flip + status passthrough), and `cel_status_either`
(error dominance, left-wins ordering).  No codegen wiring yet; that
lands in slices B (checked arithmetic) and onward.

**M4 slice C commit 1 (2026-04-19): sret arithmetic ABI (runtime
only).**  Runtime grows a scratch-slot / sret counterpart to every
Slice B checked-arithmetic helper: `cel_int_add_at(out, a, b)` and
friends write the 24-byte result CelValue into a caller-provided
slot at offset `out` instead of bump-allocating.  `cel_box_{bool,
int,uint,double}` are the scalar→slot counterparts used at
scalar→boxed boundaries (e.g. a checker-proven-bool operand
entering 3VL `cel_and`).  Status propagation is via
`cel_status_either` + 24-byte memcpy into the slot.  Type mismatch
surfaces as `CEL_ERROR{CEL_ERR_TYPE_MISMATCH = 13}` (new enum
value) so a checker miss degrades gracefully instead of forging a
phantom OK.  Coverage: 30 new tests in `cel_runtime_test.cc`
covering box helpers, happy / overflow / div0 / mod0 / INT64_MIN
edge cases, ERROR/UNKNOWN propagation (both sides, both-unknown
merge, error-dominates-unknown), type mismatch, and zero-offset
operand.  No codegen wiring yet; commit 2 adds the scratch-slot
pool at `$eval` entry and flips arithmetic codegen over.

**M4 slice C commit 2 (2026-04-19): codegen switches to sret
arithmetic.**  Runtime adds 11 scalar-arg sret helpers
(`cel_int_add_at_ii(out, i64, i64)` and friends for int/uint add/
sub/mul/div/mod, plus `cel_int_neg_at_i`) so codegen can call the
sret ABI from straight-line i64 wasm without re-boxing scalars.
Codegen grows a single scratch slot per `$eval` invocation: the
new `LoweringContext::GetScratchSlotLocal()` lazily allocates an
i32 local, and `WithScratchSlotPrologue` wraps the body with
`local.set $slot (call $cel_alloc 24)` when the slot was used.
`EmitCheckedArithmetic` now emits `Call(helper, LocalGet(slot),
lhs, rhs); If(kind==CEL_ERROR, unreachable); i64.load offset=8 —
no arena bump per op.  One slot suffices today because every
checked-arithmetic shape loads the payload into a wasm local
before the next helper call reuses the slot (straight-line tree
evaluation).  Coverage: 15 new runtime tests parametrise the new
scalar helpers over happy / overflow / div-0 / mod-0 / INT64_MIN
edges + zero-offset no-op; three codegen assertions now verify
the new body shape (outer Block = [prologue, inner checked-op
Block]).  All 15 `//compiler/...` test targets pass.  Trap-on-
ERROR is still the stopgap; commit 3 wires 3VL `&&` / `||` and
flips the arithmetic-ERROR path from trap to observable CelValue.

**M4 slice C commit 3a (2026-04-20): `$eval` flips to sret ABI
`(slot, args) → void`.**  The `$eval` signature grows a leading
i32 sret slot argument and now returns void; every codegen path
that used to produce an i32 result now writes it through the sret
slot via a small set of `cel_copy_*` helpers (`cel_copy_i64_at` /
`cel_copy_f64_at` / `cel_copy_celvalue_at`) or through the sret
variant of an existing runtime helper.  The host test harness and
wasmtime loader (`cel_host_wasmtime.cc`) allocate the caller slot
via `cel_alloc(24)` before calling `$eval` and read the CelValue
back through the slot.  No behavioural change yet — arithmetic
ERROR still traps — but the ABI is now shaped so commit 3b1 can
drop the trap in favour of an observable CelValue copy.

**M4 slice C commit 3b1 (2026-04-20): arithmetic + NaN-unordered
compares stop trapping.**  `EmitCheckedArithmetic` and
`LowerDoubleOrderedCompare` swap their `BinaryenUnreachable`
branches for `Block([cel_copy_celvalue_at(sret, scratch),
return])` — on ERROR the scratch slot's CelValue is copied into
the caller's sret slot and the function returns, so downstream
consumers (comprehensions, 3VL `&&` / `||`) can observe and
absorb the ERROR instead of the host seeing a wasm trap.
NaN-unordered compares use the same path but construct their
ERROR via `cel_set_error_at(scratch, CEL_ERR_NAN_COMPARE)`.
Coverage: updated `expr_lower_test` shape assertions and the e2e
arithmetic / NaN tests now expect `CelValue{ERROR, code=…}`
instead of an `absl::InternalError` trap surface.

**M4 slice C commit 3b2 (2026-04-20): bool travels as a CelValue
offset; 3VL plumbing in `&&` / `||` / `!` / `?:`.**
`Repr::kBool`'s ABI is still i32, but every bool value is now a
CelValue arena offset — literals go through `cel_make_bool`
(replacing the deleted `cel_box_bool`), and every bool-producing
site (`has(msg.f)`, `starts_with` / `ends_with` / `contains`,
ordered / equality comparisons) wraps its i32 result in
`cel_make_bool` before handing it upward.  `$eval` params of
`Repr::kBool` are auto-boxed into CelValue locals at prologue via
a new `LoweringContext::prologue_setups` list.  `&&` / `||` / `!`
dispatch to `cel_and` / `cel_or` / `cel_not` (the slice A 3VL
helpers) on boxed operands.  `?:` unboxes its condition through
`cel_bool_from_value` — a two-valued stopgap documented in the
design-doc §10.2.1 until we decide on 3VL ternary semantics.
Sret writes for bool / string / bytes / wrapped-message roots
now all share `cel_copy_celvalue_at`, and pre-existing dead
runtime exports (`cel_int_*` / `cel_uint_*` non-sret, `_ii` /
`_uu` scalar variants, boxed `_at` variants, `cel_int_neg*`,
`cel_box_bool`, and the `propagate_status_at` / `*_binop_prelude`
C helpers) were removed in the same commit — codegen only
reaches the `_at_ii` / `_at_uu` / `_neg_at_i` sret shape.

### Codegen

- [x] Arithmetic ops grow an **overflow check** on int.  Slice B
      (2026-04-19) shipped the trap-on-ERROR stopgap via
      `cel_int_add_ii` etc.; slice C commit 2 (2026-04-19) flipped
      codegen onto the sret helpers (`cel_int_add_at_ii` /
      `_at_uu` / `cel_int_neg_at_i`) with one per-`$eval` scratch
      slot; slice C commit 3b1 (2026-04-20) then dropped the trap
      in favour of an observable ERROR: the emitted shape is
      `Block(Call(sret_helper, slot, a, b), If(kind==CEL_ERROR,
      [cel_copy_celvalue_at(sret, slot), return]),
      i64.load offset=8)`.  "INT_MAX + 1" now surfaces as a
      `CelValue{ERROR, code=CEL_ERR_OVERFLOW}` the host can
      inspect, not a wasmtime trap.
- [x] `/` and `%` grow a **zero-divisor check**.  (Slice B: division
      and modulo go through the same checked helpers; `_uint_ / 0`,
      `_int_ / 0`, `INT64_MIN / -1`, and `_int_ % 0` all produce a
      CEL_ERROR that trips the trap-on-ERROR path.  `INT64_MIN % -1`
      is defined as 0 per the helper, matching cel-go.)
- [x] Double comparisons convert "unordered" results (NaN inputs)
      into an `ERROR` return instead of the plain `0` / `1` i32 that
      M2 emits.  **M4 slice D (2026-04-19):** ordered double compares
      route through `LowerDoubleOrderedCompare`, which binds both
      operands to f64 locals, checks `a != a | b != b` (true iff
      either is NaN), traps via `BinaryenUnreachable` on NaN, and
      otherwise returns `f64.<op>`.  Equality (`==` / `!=`) stays as
      a bare `f64.eq` / `f64.ne` because IEEE 754 already defines
      those to return false / true for any NaN input — no trap
      needed, no CEL-spec gap.  Like Slice B, this is the trap
      stopgap; the observable-ERROR value path lands with the 3VL
      retrofit.  Covered by 8 e2e tests in `eval_test.cc`
      (`DoubleLessNaN*Traps`, `DoubleLessEqNaNTraps`,
      `DoubleGreaterNaNTraps`, `DoubleGreaterEqNaNTraps`,
      `DoubleEqualityWithNaNReturnsFalseNotTrap`,
      `DoubleOrderedCompareNonNaNStillWorks`,
      `DoubleDivZeroProducesInfNotTrap`), plus updated
      `expr_lower_test::DoubleComparisons` asserting the new
      Block[set_a, set_b, trap_if, Binary] body shape.
- [x] `&&` / `||` / `!` switch from M2's scalar short-circuit to
      the three-valued `cel_and` / `cel_or` / `cel_not` helpers.
      **Slice C commit 3b2 (2026-04-20):** bool values now travel
      as CelValue offsets (Repr::kBool is an arena-offset i32);
      `&&` / `||` / `!` call the slice A 3VL helpers on boxed
      operands.  The checker-driven scalar fast-path is deferred —
      every boolean expression pays the boxed overhead today, but
      the `cel_make_bool` + 3VL-helper shape is uniform, which
      keeps codegen small.  Fast-path revisit is tracked in
      `testing-checklist.md`.
- [x] `?:` grows the same treatment: when the condition is
      UNKNOWN / ERROR, the ternary returns the same (per spec).
      **M4 slice E1 (2026-04-20):** `LowerConditional` emits a
      3-child Block `[cond_set, err_if, inner_if]`.  The err_if
      tests `kind >= CEL_UNKNOWN` on the cond's kind byte and, on
      match, copies the cond CelValue into the eval sret slot and
      early-returns — same shape as `EmitCheckedArithmetic` /
      `LowerDoubleOrderedCompare`.  The inner_if unboxes the cond
      via `cel_bool_from_value` and dispatches as before.
      Codegen-shape coverage via `expr_lower_test::Conditional`;
      e2e coverage of the 3VL-cond path is deferred until Slice E2
      introduces the first non-early-returning UNKNOWN producer.
- [ ] Identifier lookup against a host-provided `unknown_attributes`
      set — the host ABI grows
      `cel_host.is_unknown(externref, i32 attr_id) → i32`; if true,
      the ident lowering returns an `UnknownSet{attr_id}` instead
      of the scalar.
- [ ] Select lowering chains the unknown: if the receiver is
      UNKNOWN, propagate; else read field.
- [ ] Comprehension aggregation: the accumulator's kind dominates.
      `all` over a range that has an unknown element returns UNKNOWN,
      not false (unless an earlier element already forced false, in
      which case short-circuit wins — per spec).

### CLI / host-ABI tooling

- [ ] `celwasmc --unknown-attrs=var.field,…` — a CLI flag to mark
      specific attributes unknown for testing.  Avoids wiring a
      full host config for every repro.
- [ ] `cel.abi.attributes` table grows to hold the reverse map
      (attr_id → source path) so error + unknown messages can
      pretty-print.

## Testing obligations

`testing-checklist.md` e2e rows that flip:

- [x] Arithmetic overflow (int + int overflows to ERROR).
- [x] Division by zero (int / 0 and double / 0 — note: double
      division produces +/-Inf, which is NOT an error per IEEE 754;
      only modulo is).
- [x] String coercion errors where the spec forbids them.
- [x] `unknown` propagation through `&&` / `||` (M4).
- [x] Partial-eval: `unknown && false → false` commutatively (M4).

New e2e cases:

- [ ] Every cell of the `cel_and` truth table (5 values × 5 = 25 cases,
      plus commutativity = 50) — parametrise with gtest's
      `INSTANTIATE_TEST_SUITE_P`.  Same for `cel_or`.
- [ ] NaN-unordered compare returns ERROR for every operator
      (`<`, `<=`, `>`, `>=`) but OK(false) for `==` / `!=`.
- [ ] Unknown attribute is noted in every ERROR message (round-trip
      the attr path for human diagnostics).
- [ ] UnknownSet merge is deterministic (two unknowns in different
      source order produce the same merged set).

Negative tests:

- [ ] Double modulo — `1.0 % 2.0` — checker rejects (no overload);
      verify the diagnostic is the checker's, not codegen's.
- [ ] Malformed `--unknown-attrs` flag value — CLI rejects with a
      readable message.

## Open design questions

1. **Checked arithmetic codegen shape.** Inline branch per op, or
   one `cel_add_checked` runtime call?  Call is simpler; inline is
   faster.  Benchmark M4-1 to decide.
2. **UnknownSet representation.** Sorted dedup'd `i32[]` is the
   current lean.  An open question is whether the spec allows the
   compiler to dedup eagerly, or whether the host sees the
   pre-dedup set.  Read §langdef §partial-evaluation before
   committing.
3. **Error source propagation.** The spec doesn't mandate that a
   runtime ERROR carries its source-code span, but the design doc
   §12 implies we record it.  M4 is where it lands or gets
   deferred.
