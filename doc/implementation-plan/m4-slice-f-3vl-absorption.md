# M4 Slice F — 3VL absorption in comparisons, arithmetic, and other
# non-absorbing intermediate ops

Status: **planning doc; not started.**  Blocks: none (self-contained
ABI change).  Unblocks: nothing load-bearing — M5 comprehensions do
need UNKNOWN / ERROR absorption, but only through `&&` / `||`, which
already works (slice A + 3b2).  Slice F closes the *semantic* gap
between what CEL's spec requires and what our codegen delivers; M5
can ship without it if we're willing to eat the documented breaks
below.

## The gap

Today every "non-absorbing" intermediate op — numeric / string /
bytes comparisons, arithmetic, string operations, `size(…)`,
`starts_with` / `ends_with` / `contains`, `matches`, message
equality — is written in a scalar-or-i32 ABI that has no room for
UNKNOWN / ERROR.  When one of those ops is fed an UNKNOWN or ERROR,
codegen either early-returns from `$eval` (arithmetic, NaN-compare,
ternary-cond-after-Slice-E1, select-of-scalar-field-after-Slice-E2a)
or silently forges a well-typed garbage result (string compare on
unknown CelValue, bool compare with `cel_bool_from_value` on
UNKNOWN).

Either path breaks the spec's 3VL absorption rules whenever the
non-absorbing op is wrapped by `&&` / `||` / `?:` / comprehension
aggregator.  The pattern is:

```
PRODUCER → NON-ABSORBING-OP → 3VL-ABSORBER
  (unknown / error)    (comparison, arithmetic, ...)   (&&, ||, ?:)
```

The absorber is supposed to see the UNKNOWN / ERROR *as a value* and
short-circuit appropriately (`UNKNOWN || OK(true) → OK(true)`,
`ERROR && OK(false) → OK(false)`).  Our pipeline never lets the
UNKNOWN / ERROR reach the absorber — the intermediate op consumes
it first, and we either early-exit or mangle it into a bogus OK.

## Scope of Slice F

Make every non-absorbing op 3VL-aware.  The minimum-viable shape:

  - **Operand ABI.**  Comparisons and string / bytes ops take
    CelValue offsets (arena-relative i32) for both operands, not
    scalars.  Arithmetic keeps its scalar fast path when both
    operands are definite, but grows a CelValue-offset slow path
    for any operand whose producer can yield UNKNOWN / ERROR.
  - **Output ABI.**  Every non-absorbing op writes a CelValue into
    a caller-provided scratch slot (sret) — same shape arithmetic
    already uses.  On ERROR / UNKNOWN operand, the op *copies the
    dominant status into its own sret slot* (per
    `cel_status_either` from slice A) instead of early-returning
    from `$eval`.
  - **Absorber consumption.**  `cel_and` / `cel_or` / `cel_not`
    already take CelValue offsets and do the right thing with
    UNKNOWN / ERROR — no change needed there.  `?:` gains a new
    dispatch: when cond is UNKNOWN / ERROR, still propagate (as
    Slice E1 today) *but only at eval root*; when a wrapping
    absorber is present, let it see the UNKNOWN / ERROR.
  - **Checker-driven fast path.**  For expressions where the
    checker has proved both operands are definite (no UNKNOWN /
    ERROR producer in either subtree), codegen stays on the scalar
    path — zero overhead regression against M4 Slice B / C shapes.

This is big enough to warrant its own slice.  Internal breakdown
(agreed 2026-04-20):

  - **F1 — comparison sret ABI.**  `==`/`!=`/`<`/`<=`/`>`/`>=` stop
    returning scalar `i32` bool; they accept CelValue-offset inputs
    when either operand's subtree can produce UNKNOWN / ERROR, write
    a CelValue into a caller-provided scratch slot, and on
    UNKNOWN / ERROR operand copy the dominant status in per
    `cel_status_either`.  A checker-driven fast path keeps the
    scalar shape for expressions whose operands are both proved
    definite — zero overhead regression against Slice B/C.
    Unblocks rows 1, 2, 3, 5, 9–14, 20.
  - **F2 — arithmetic slow path.**  When either operand of
    `+`/`-`/`*`/`/`/`%` comes from a subtree that can yield
    UNKNOWN / ERROR, lower through the CelValue-offset shape F1
    introduces instead of the scalar overflow-check pair.  The
    definite-both-sides case stays on the scalar Slice B checked
    arithmetic.  Unblocks rows 4, 18, 22.
  - **F3 — string / bytes ops + `size`.**  `==` / `startsWith` /
    `endsWith` / `contains` / `matches` and `size(…)` go sret
    CelValue end-to-end (they already travel as arena offsets, so
    this is a wrapper change, not an ABI overhaul).  Unblocks rows
    15, 16, 17, 21.
  - **F4 — ternary dispatch.**  `?:` gains a dispatch: when the
    cond slot holds UNKNOWN / ERROR and the `?:` is *not* at eval
    root (i.e. has a wrapping 3VL absorber), copy the dominant
    status through to the sret slot instead of early-returning
    from `$eval` (Slice E1's current behaviour at root stays
    intact).  Unblocks rows 7, 8, 19.

No sub-slice blocks another; F2 and F3 both consume the F1 ABI, F4
is independent.  F1 ships first because it is the largest
structural change and has the longest test row.

## F1 — shipped 2026-04-20

F1 landed as a single commit introducing the boxed comparison ABI
and flipping the DISABLED_ prefix on the rows it unblocks.  Live
regression coverage in `eval_test.cc`:

  - ERROR-source rows 1, 2, 3, 5 (`ThreeValuedAbsorptionError{EqAbsorbedBy{Or,And},OrderedCompareAbsorbed,OverflowAbsorbed}`).
  - UNKNOWN-source row 9 (`UnknownThroughOrderedCompareAbsorbedByOr`).
  - UNKNOWN-source row 10 (`UnknownThroughEqualityAbsorbedByAnd`).
  - UNKNOWN-source row 11 (`UnknownEqualityBothOperandsUnknownAbsorbedByOr`).
  - UNKNOWN-source row 12 (`UnknownUintOrderedCompareAbsorbedByOr`).
  - UNKNOWN-source row 13 (`UnknownDoubleOrderedCompareAbsorbedByOr`).
  - UNKNOWN-source row 20 (`UnknownBoolEqualityPropagatesThroughOrFalse`).

Rows 4 (F2), 6 / 8 (F4), 14 (F3 — message equality), 15 / 16 / 17 /
21 (F3), 18 / 22 (F2), 19 (F4) remain DISABLED pending the matching
slice.  The `cel_unknown_merge` empty-set handling was also fixed
in the same commit — the host `get_field` trampoline mints UNKNOWNs
with `payload.unk == 0`, which the old merge rejected as invalid;
it now treats an empty set as "no ids to contribute" and returns
the other side verbatim (left-biased when both empty).

## F1 implementation plan (as shipped)

### Root cause we're unwinding

Two codegen sites early-return from `$eval` when their sret scratch
holds a non-OK CelValue:

  - `LowerSelectField` for scalar Reprs (int / uint / double /
    message) — emits `EmitSretEarlyReturnIfNonOk(ctx, scratch)`
    before `LoadSelectPayload`.  The reason the early-return was
    added: `LoadSelectPayload` reads `[scratch+8]` as a raw scalar,
    so an UNKNOWN CelValue's `unk` pointer would be reinterpreted
    as an int64 — silent garbage.
  - `EmitCheckedArithmetic` — after the helper writes to scratch,
    inspects `[scratch+0] == CEL_ERROR` and, if so, copies the ERROR
    into `$sret` and returns.  Same motivation: the surrounding
    codegen expects an i64 off the block, but scratch holds a full
    ERROR box.

Both early-returns bypass any wrapping `&&` / `||` absorber.  F1
keeps them on the scalar fast path for definite-both-sides
expressions and introduces a parallel **boxed** path taken whenever
a comparison's operand subtree can produce UNKNOWN / ERROR.

### Runtime helpers (cel_runtime.{h,c})

Four-operator group per scalar kind, plus bool eq/ne.  Every helper
takes two CelValue offsets and returns a CelValue offset pointing
at a `Repr::kBool` result — or at a propagated ERROR / UNKNOWN when
`cel_status_either` reports a dominant non-OK.  Return 0 on type
error so codegen keeps the familiar zero-check shape.

```
// Returns cel_status_either(a, b) if either side is non-OK;
// otherwise reads .payload.i from both and returns cel_make_bool.
uint32_t cel_cmp_int_eq (uint32_t a, uint32_t b);
uint32_t cel_cmp_int_ne (uint32_t a, uint32_t b);
uint32_t cel_cmp_int_lt (uint32_t a, uint32_t b);
uint32_t cel_cmp_int_le (uint32_t a, uint32_t b);
uint32_t cel_cmp_int_gt (uint32_t a, uint32_t b);
uint32_t cel_cmp_int_ge (uint32_t a, uint32_t b);

// Same shape on CEL_UINT / payload.u.
uint32_t cel_cmp_uint_eq / ne / lt / le / gt / ge(uint32_t, uint32_t);

// CEL_DOUBLE / payload.d.  Ordered ops additionally return
// cel_make_error(CEL_ERR_TYPE_MISMATCH) when either OK operand is
// NaN — same spec rule as LowerDoubleOrderedCompare.
uint32_t cel_cmp_double_eq / ne / lt / le / gt / ge(uint32_t, uint32_t);

// CEL_BOOL / payload.b.  No ordered variant (CEL has no <,<=,>,>=
// overload on bool).
uint32_t cel_cmp_bool_eq (uint32_t a, uint32_t b);
uint32_t cel_cmp_bool_ne (uint32_t a, uint32_t b);
```

Each helper factors through a tiny inline dispatcher:

```
uint32_t st = cel_status_either(a, b);
if (st != 0) return st;
// Both a, b are OK of the expected kind (codegen guarantees via
// Repr); read the payload and return cel_make_bool(raw_cmp).
```

Rows that need this helper family on the boxed path: 1, 2, 3, 5,
9–13, 20.  Row 14 (`msg.sub_msg == other || true`) and the string
equality rows (15, 17) stay on the existing span / message path;
moving them to the boxed shape is F3's scope.

### Codegen (`compiler/codegen/expr_lower.cc`)

1. **`HasNonOkProducer(const TypedAst&, const cel::Expr&) -> bool`**

   Pure AST predicate.  Returns true iff the subtree contains a
   node that can leave a non-OK CelValue in a scratch slot:

     - `SelectExpr` (field reads can return UNKNOWN / ERROR).
     - `CallExpr` for `/` / `%` / `+` / `-` / `*` on int or uint
       (checked-arith can overflow → CEL_ERROR).
     - `CallExpr` for ordered compare on double (NaN → CEL_ERROR).

   Constants, idents, bool ops, and string / bytes ops are pure OK
   today; they return false.  The predicate is conservative — false
   positives just force the boxed path (no correctness loss, only
   fast-path coverage loss).  Recurses into call args and into
   select operands.

2. **`LowerExprBoxed(LoweringContext&, const TypedAst&, const cel::Expr&)
    -> absl::StatusOr<BinaryenExpressionRef>`**

   Returns a wasm i32 expression producing a CelValue offset for
   `expr`, regardless of its static Repr.  Mirrors `LowerExpr` but
   keeps everything in the boxed ABI:

     - Constant of int / uint / double → `cel_make_int / uint /
       double` on the raw scalar.  Bool / string / bytes / null
       already return CelValue offsets — reuse `LowerConstant`.
     - Ident of int / uint / double → alloc 24-byte scratch,
       `cel_box_int / uint / double(scratch, raw)`, return scratch.
       Bool / string / bytes / message idents are already CelValue
       offsets post-3b2 — reuse `LowerIdent`.
     - `SelectExpr` (non-test-only) → same machinery as
       `LowerSelectField` up to the `get_field` call, then return
       the scratch offset.  Skip both `LoadSelectPayload` and
       `EmitSretEarlyReturnIfNonOk`.  `test_only` (`has(…)`) stays
       on the scalar path (the host never returns UNKNOWN for
       `has`; it returns a definite 0/1).
     - `CallExpr` for checked arithmetic (`/`, `%`, `+`, `-`, `*`
       on int / uint) → alloc scratch, call
       `cel_int_add_at_ii(scratch, lhs, rhs)` (or the op variant),
       return scratch.  Skip the CEL_ERROR kind-check + early-return
       that `EmitCheckedArithmetic` emits today.  (F2 will grow this
       out to the nested case where either operand is itself
       boxed — F1 keeps lhs / rhs as raw scalars because the
       comparison dispatch only boxes one level up.)
     - `CallExpr` for double arithmetic → compute via
       `LowerDoubleArithmetic`, then `cel_box_double(scratch, raw)`,
       return scratch.
     - `CallExpr` for `_&&_` / `_||_` / `!_` / `_?_:_` → fall
       through to `LowerExpr`; those already return CelValue
       offsets.
     - Any other node → `LowerExpr` + box if Repr is scalar.

3. **Modified `LowerComparison`.**  After the existing
   `Repr::kString` / `kBytes` / `kMessage` / `kBool` dispatches, the
   default scalar case (int / uint / double) gains a pre-check:

     ```
     if (HasNonOkProducer(lhs_expr) || HasNonOkProducer(rhs_expr)) {
       return LowerBoxedComparison(ctx, ast, call, arg_r);
     }
     ```

   `LowerBoxedComparison` calls `LowerExprBoxed` on both operands,
   picks the right helper name by `(arg_r, op)`, and emits:

     ```
     cel_cmp_<kind>_<op>(lhs_boxed, rhs_boxed)   // returns i32 offset
     ```

   The result is a CelValue offset — `Repr::kBool` — matching what
   every bool-consuming caller (cel_and / cel_or / BinaryenIf of a
   `?:` guard that has been unboxed) expects.

   The bool equality case gains the same pre-check: if either
   operand's subtree is non-OK-producing, emit
   `cel_cmp_bool_{eq,ne}` instead of `UnboxBool` + `BinaryenEqInt32`.
   This is row 20's fix.

4. **Signature change at the `LowerBinaryCall` seam.**
   `LowerComparison` needs access to the raw operand expressions (to
   run `HasNonOkProducer`), not just their lowered forms.  Two
   options: (a) pass `call.args()` through alongside the lowered
   values, or (b) run `HasNonOkProducer` before calling
   `LowerExpr` on the operands, then pick the path.  Option (b) is
   cleaner — `LowerBinaryCall` picks the path, calls either the
   scalar `LowerExpr` pair or the boxed `LowerExprBoxed` pair, then
   dispatches.  F1 takes option (b).

### Tests

  - **Runtime unit tests** in `cel_runtime_test.cc`: one `TEST`
    block per new helper covering (i) OK + OK → OK bool, (ii)
    ERROR + OK → ERROR propagation, (iii) UNKNOWN + OK → UNKNOWN
    propagation, (iv) ERROR + UNKNOWN → ERROR (dominance), (v)
    NaN-in-ordered-double → ERROR.  Type-mismatch → 0.
  - **Codegen tests**: extend `expr_lower_test.cc` with a fixture
    that runs a comparison over a Select-of-scalar operand and
    validates the emitted IR calls `cel_cmp_int_lt` (not
    `BinaryenLtSInt64`) when the Select is present.
  - **eval_test.cc**: flip `DISABLED_` off the absorption rows F1
    unblocks (1, 2, 3, 5, 9–13, 20) once the codegen + runtime
    pieces are in place.

### Out of F1 scope (deferred to F2 / F3)

  - Row 14 (message equality UNKNOWN): needs a boxed `message_eq`
    — F3.
  - Rows 15 / 16 / 17 / 21 (string / bytes / size): F3.
  - Row 4, 18, 22 (UNKNOWN through arithmetic into another op): F2
    grows `LowerExprBoxed` to handle operands that are themselves
    boxed checked-arith results.
  - Rows 7 / 8 / 19 (ternary at root vs. under an absorber): F4.

## Spec-breaking test cases (the Slice F checklist)

Each row below is an expression whose result today does NOT match
the CEL spec.  Every test either sits DISABLED in `eval_test.cc`
(see `DISABLED_ThreeValuedAbsorption_*`) or is listed here waiting
for a companion producer (UNKNOWN producer lands with Slice E2a.1).
When Slice F ships, the corresponding DISABLED prefix comes off and
the test should pass; for rows without a test today, the DISABLED
test is added in the same commit.

### ERROR-source cases (testable today)

All assume the spec: `ERROR` absorbed by `&&` past OK(false) or
`||` past OK(true) yields the OK side.  Currently every one of
these surfaces as `absl::InternalError("CallEval: result is
ERROR")` via the arithmetic or NaN-compare sret early-return.

| # | Expression                             | Spec result | Notes                                                                                       |
|---|----------------------------------------|-------------|---------------------------------------------------------------------------------------------|
| 1 | `(1 / 0 == 0) \|\| true`                 | `true`      | Int div-by-zero → ERROR; `==` must absorb, `\|\|` short-circuits.                             |
| 2 | `(1 / 0 == 0) && false`                | `false`     | Mirror of #1 through `&&`.                                                                  |
| 3 | `(1 / 0 > 5) \|\| true`                  | `true`      | Ordered compare instead of equality.                                                         |
| 4 | `((1 / 0) + 1) == 0 \|\| true`           | `true`      | ERROR through a second arithmetic hop before the compare.                                   |
| 5 | `(9223372036854775807 + 1 == 0) \|\| true` | `true`    | Overflow ERROR absorbed via `==` + `\|\|`.                                                    |
| 6 | `(x < 1.0) \|\| true` (x = NaN)          | `true`      | NaN-compare ERROR absorbed.  Needs `EvaluateWithVars` + NaN param.                          |
| 7 | `(1 / 0 == 0) ? "a" : "b"`             | ERROR-bubble | Ternary cond is ERROR; Slice E1 already early-returns here, which is spec-correct for a *root* `?:` but not if the ternary itself is wrapped by `\|\|`.  See #8. |
| 8 | `((1 / 0 == 0) ? true : false) \|\| true` | `true`    | Ternary result is ERROR; `\|\|` absorbs.  This is E1's semantic gap when wrapped.            |

### UNKNOWN-source cases (testable after Slice E2a.1)

All assume a host that marks one proto field on a variable as
unknown via `CelHost::SetUnknownAttrs`.  Expressions assume `msg`
is a variable of a message type with the obvious fields.

| #  | Expression                                         | Spec result | Notes                                                                                   |
|----|----------------------------------------------------|-------------|-----------------------------------------------------------------------------------------|
| 9  | `msg.int_field > 10 \|\| true`                       | `true`      | Scalar UNKNOWN through ordered compare into `\|\|`.                                      |
| 10 | `msg.int_field == 0 && false`                      | `false`     | Scalar UNKNOWN through equality into `&&`.                                              |
| 11 | `msg.int_field == msg.other_int \|\| true`           | `true`      | Both operands unknown; absorber wins.                                                   |
| 12 | `msg.uint_field >= 0u \|\| true`                     | `true`      | Uint variant.                                                                           |
| 13 | `msg.double_field < 1.0 \|\| true`                   | `true`      | Double variant.                                                                         |
| 14 | `msg.sub_msg == other \|\| true`                     | `true`      | Message equality UNKNOWN; absorber wins.                                                |
| 15 | `msg.str_field == "foo" \|\| true`                   | `true`      | String equality UNKNOWN.  `cel_string_eq` must absorb.                                  |
| 16 | `msg.str_field.startsWith("x") && false`           | `false`     | String op UNKNOWN.                                                                      |
| 17 | `msg.bytes_field == b"foo" \|\| true`                | `true`      | Bytes equality UNKNOWN.                                                                 |
| 18 | `(msg.int_field + 1) == 0 \|\| true`                 | `true`      | UNKNOWN through arithmetic, then compare, then absorber.                                |
| 19 | `(msg.int_field > 0 ? 1 : 2) == 1 \|\| true`         | `true`      | UNKNOWN in ternary cond; ternary propagates UNKNOWN (E1); outer `\|\|` absorbs.         |
| 20 | `(msg.bool_field == true) \|\| false`                | UNKNOWN     | Bool-equality on UNKNOWN: spec says UNKNOWN, not OK-coerced-via-`cel_bool_from_value`. |
| 21 | `size(msg.str_field) == 0 \|\| true`                 | `true`      | `size` on UNKNOWN string; must absorb.                                                  |

### Combined chain case

| #  | Expression                                              | Spec result | Notes                                                                           |
|----|---------------------------------------------------------|-------------|---------------------------------------------------------------------------------|
| 22 | `(msg.int_field + (1 / 0)) == 0 \|\| true`                | `true`      | UNKNOWN and ERROR in the same arithmetic subtree; `cel_status_either` picks ERROR; absorbed by `\|\|`. |

## Why we're shipping the gap for now

Every one of the expressions above is either (a) already broken for
ERROR today (shipped with 3b1 on 2026-04-20) or (b) broken for
UNKNOWN the moment E2a.1 lands.  The failure mode is consistent and
loud — an `InternalError` on the host side, not silent corruption —
and the patterns that break are the ones where a CEL author is
deliberately mixing a partially-known / potentially-failing subtree
with a literal fallback.  In the partial-eval use case driving M4,
the dominant expression shape is `msg.some_field` or
`has(msg.field)` gated on a known bool, neither of which hits any
of the breaks above.  We explicitly accept the gap so M4 can ship
and M5 (collections + comprehensions) can start, with Slice F as
the follow-up that makes CEL 3VL end-to-end correct.

## CLI flag for unknown attrs — shipped 2026-04-20

M4 Slice E2a.2 landed the `celwasmc-eval --unknown_attrs=var.q[,…]`
flag (commit 9a1b672).  The flag only exposes the E2a.1 host API —
it does not unblock any of the spec-breaking rows above; those still
require Slice F.  Rows 9–22 become testable from the CLI once F1/F2
land, but eval_test.cc (not the CLI) is the canonical coverage
surface.
