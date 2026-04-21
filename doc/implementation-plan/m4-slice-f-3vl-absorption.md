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

This is big enough to warrant its own slice.  Original breakdown
(agreed 2026-04-20, F1 shipped under this scheme):

  - **F1 — comparison sret ABI.**  `==`/`!=`/`<`/`<=`/`>`/`>=` stop
    returning scalar `i32` bool; they accept CelValue-offset inputs
    when either operand's subtree can produce UNKNOWN / ERROR, write
    a CelValue into a caller-provided scratch slot, and on
    UNKNOWN / ERROR operand copy the dominant status in per
    `cel_status_either`.  A checker-driven fast path keeps the
    scalar shape for expressions whose operands are both proved
    definite — zero overhead regression against Slice B/C.
    Unblocks rows 1, 2, 3, 5, 9–14, 20.  **Shipped 2026-04-20.**
  - **F2 / F3 / F4 — superseded 2026-04-20** by the uniform-boxed
    ABI plan below.  Leaving their original descriptions here as
    historical context:
    - F2 was scoped as an arithmetic slow path gated by
      `HasNonOkProducer`, reusing F1's dual-path machinery.
    - F3 was scoped as a wrapper change over the existing span ABI
      for string / bytes ops + `size`.
    - F4 was scoped as a ternary dispatch that conditioned on
      "wrapping 3VL absorber present".
    All three shared F1's fast-vs-boxed dispatch complexity.  The
    revised plan collapses them into a single slice that drops the
    dispatch in favour of one uniform code path.

## Revised plan — uniform boxed ABI (agreed 2026-04-20)

**Goal: conformance first, most of the performance.**  Keeping the
scalar fast path alongside a boxed slow path means every op has two
shapes, a predicate (`HasNonOkProducer`) picking between them, and
parallel runtime helpers.  F1 shipped with this dual-path, but F2 /
F3 / F4 each add another round of the same complexity.  The revised
plan collapses F2–F4 into one slice that drops the scalar fast path
for intermediate non-absorbing ops and keeps everything on the
CelValue-offset ABI end-to-end.  The resulting codegen is a single
code path; conformance falls out; perf is "most of" the scalar
path's because (a) the runtime helpers still do raw `i64.add` /
`i64.lt_s` / etc. *inside* the boxed wrapper, and (b) the envelope
allocation is a bump-pointer into a per-eval arena, not a heap
alloc.

### Scope — "Slice F-uniform" (supersedes F2 / F3 / F4)

All intermediate non-absorbing ops travel as CelValue offsets.  The
Reprs `kInt`, `kUint`, `kDouble` change from "raw i64 / f64 wasm
value" to "arena-relative CelValue offset" — matching what
`kBool`, `kString`, `kBytes`, `kMessage` already do today (Slice C
3b2).  Concrete changes:

1. **Scalar param boxing** (mirroring the bool case from Slice C
   3b2).  Int / uint / double params on `$eval` still arrive as raw
   i64 / f64 per the host ABI contract, but at function entry
   codegen emits
   `local.set $boxed (call cel_make_{int,uint,double} (local.get
   $raw_param))` for each scalar param and rebinds the ident name
   to `$boxed`.  Subsequent ident reads land on the CelValue
   offset.  Zero host ABI change.
2. **Always-boxed comparison.**  Drop the
   `HasNonOkProducer`-gated dispatch in `LowerBinaryCall`.  Every
   `==`/`!=`/`<`/`<=`/`>`/`>=` lowers through `LowerBoxedComparison`
   (F1's helpers).  The `BinaryenEqInt64` / `BinaryenLtSInt64` /
   etc. emitters go away.  Bool equality on raw i32 goes away too
   (use `cel_cmp_bool_eq`/`_ne`).
3. **Always-boxed arithmetic.**  `+`/`-`/`*`/`/`/`%`/unary-`-`
   lower to a CelValue-offset-in, CelValue-offset-out helper.  Add
   `cel_int_add_at_vv(out, a_off, b_off)` and siblings.  Each helper
   (a) short-circuits on `cel_status_either(a, b)` — writing the
   dominant non-OK into the sret slot — and (b) falls through to
   the existing scalar overflow check on raw payload loads.  Result
   Repr becomes CelValue offset, so
   `EmitCheckedArithmetic`'s kind-check-and-early-return disappears
   — the scratch offset flows upward like every other boxed value.
4. **Select-of-scalar becomes non-early-returning.**  Drop
   `EmitSretEarlyReturnIfNonOk` in `LowerSelectField`.  Scalar
   payload load (`LoadSelectPayload`) is removed: the select's
   result is already a CelValue offset.  The existing select path
   for bool / string / bytes / message already returns the scratch
   offset, so this collapses to one branch.
5. **Simplified ternary.**  `LowerConditional` just reads cond's
   kind byte: if `kind >= CEL_UNKNOWN`,
   `cel_copy_celvalue_at($sret, $cond); return;` (same as today);
   if OK, `cel_bool_from_value → BinaryenIf` to pick an arm.  No
   "am I at eval root?" contextual logic — the wrapping absorber
   either sees the CelValue or doesn't, and F1's machinery already
   passes it through comparison / `cel_and` / `cel_or`.  Rows 7,
   8, 19 resolve.
6. **String / bytes / size absorb non-OK.**  `cel_string_eq`,
   `cel_bytes_eq`, `cel_string_concat`, `cel_bytes_concat`,
   `cel_string_starts_with` / `_ends_with` / `_contains`,
   `cel_string_size`, `cel_bytes_size` — each prefixed with a
   `cel_status_either` check that short-circuits to the dominant
   non-OK.  Return types shift from raw `i32` / `i64` bool-or-length
   to CelValue offsets.  Rows 14 / 15 / 16 / 17 / 21 resolve.
7. **Message equality absorption.**  `cel_host.message_eq` stays
   2-arg externref (pure host-side compare), but the caller-side
   wrapper `cel_wrap_message_eq_result` checks the two `msg_slot`
   offsets for UNKNOWN / ERROR kind before invoking the host.  Row
   14 resolves.

### Runtime dead-code sweep (mandatory in the same slice)

Once the scalar fast path is gone from codegen, a pile of runtime
helpers lose their last caller.  **The slice is not done until they
are deleted and `cel_runtime.{c,h}` and `cel_runtime_test.cc` are
scrubbed of their references.**  Candidates to enumerate during
implementation (audit all exported `cel_*` symbols against
`expr_lower.cc` emission sites):

  - `cel_bool_from_value` — was used by the scalar bool path.  If
    the only surviving caller is the ternary cond unbox, keep it;
    otherwise delete.
  - `cel_int_add_at_ii` / `_sub_at_ii` / `_mul_at_ii` / `_div_at_ii`
    / `_mod_at_ii` / `_neg_at_i` and the `_at_uu` uint siblings —
    superseded by `_at_vv` variants.  Delete the scalar-arg forms
    if no caller remains.
  - `cel_set_error_at` / `cel_copy_celvalue_at` — may still be used
    by the sret root copy; audit.
  - `cel_box_int` / `cel_box_uint` / `cel_box_double` — only used
    by F1's stopgap literal-box path; probably deletable once
    step 1 (scalar param boxing) lands and literals go through
    `cel_make_int` / `_uint` / `_double` directly.
  - Any helper introduced for the F1 dual-path that the uniform
    path subsumes (`LowerCheckedArithBoxed` in codegen — delete;
    `LowerExprBoxed` — merges with `LowerExpr` once scalar-repr
    callers are gone).
  - Corresponding unit tests in `cel_runtime_test.cc`: remove the
    tests for deleted helpers; keep (and extend) the tests for
    helpers whose contract grew a 3VL absorption arm.

Record the deletions in the commit message so the diff reviewer
sees "dead code after dispatch collapse" and not a drive-by.

### Implementation order (incremental, each step testable)

1. **[shipped 2026-04-20]** Auto-box int / uint / double `$eval`
   params at function entry.  `BuildParamList` now boxes every
   scalar-bearing Repr (bool / int / uint / double) through a
   `cel_make_<kind>` prologue that stores the CelValue offset in a
   scratch local and rebinds the ident name to it (generalised from
   the bool-only `PendingBoolBox` into `PendingScalarBox`).  Added
   runtime helpers `cel_int_from_value` / `cel_uint_from_value` /
   `cel_double_from_value` (kind-checked payload accessors) and
   codegen helpers `UnboxInt` / `UnboxUint` / `UnboxDouble`;
   `LowerIdent` for int / uint / double now emits
   `UnboxK(LocalGet(boxed))` so downstream arithmetic / compare
   opcodes still see raw scalars.  This is a pure runtime no-op
   (box-then-unbox round-trip) — every existing test stays green
   without flipping any DISABLED_ rows.  Unit tests updated in
   `cel_runtime_test.cc` (round-trip + kind-mismatch for the three
   new unbox helpers) and `expr_lower_test.cc`
   (`IntIdentLowersToLocalGetWithI64Param`,
   `IdentsOfAllScalarReprs`) to match the new ident shape.
2. **[shipped 2026-04-20]** Boxed-in / boxed-out arithmetic
   helpers.  Added `cel_int_{add,sub,mul,div,mod}_at_vv` and the
   `cel_uint_*_at_vv` siblings, plus unary `cel_int_neg_at_v`.  Each
   helper wraps `arith_boxed_prologue` which (a) short-circuits on
   `cel_status_either` — copying the dominant non-OK into the sret
   slot via `cel_copy_celvalue_at` — and (b) raises
   `CEL_ERR_TYPE_MISMATCH` when either operand has the wrong kind
   or is the null offset.  On success delegates to the scalar
   `_at_ii` / `_at_uu` sibling for the arithmetic + overflow /
   div0 check.  Codegen: `LowerCheckedArithBoxed` now routes every
   `+`/`-`/`*`/`/`/`%` through `_at_vv` and recurses into operands
   via `LowerExprBoxed` (not `LowerExpr`), so nested arithmetic
   inside a boxed compare stays on the boxed path end-to-end.
   Flipped DISABLED_ rows 4 (`ThreeValuedAbsorptionErrorArithThenCompareAbsorbed`)
   and 18 (`UnknownThroughArithThenCompareAbsorbed`); added row 22
   (`UnknownAndErrorInArithSubtreeErrorDominates`, ERROR > UNKNOWN
   in `cel_status_either` absorbed by `|| true`).  Unit coverage:
   16 new tests in `cel_runtime_test.cc` covering happy path,
   overflow, div-by-zero, left / right / both absorption,
   ERROR-dominates-UNKNOWN, kind mismatch, and zero-offset no-op
   across int / uint add / sub / mul / div / mod / neg.  The
   scalar `_at_ii` / `_at_uu` helpers remain reachable via the
   wrappers; their direct call sites from codegen go away when
   step 7 lands.
3. **[shipped 2026-04-20]** Always-boxed comparison.  Dropped the
   `HasNonOkProducer` gate in `LowerBinaryCall`; every scalar / bool
   `==`/`!=`/`<`/`<=`/`>`/`>=` now lowers through
   `LowerBoxedComparison` → `cel_cmp_<kind>_<op>`.  Deleted
   now-unreachable scalar-compare machinery: `HasNonOkProducer`,
   `ScalarEqualityOp`, `OrderedIntOp` / `OrderedUintOp` /
   `OrderedDoubleOp` / `OrderedCompareOp`, `LowerDoubleOrderedCompare`
   (the codegen NaN-guard block — runtime `cel_cmp_double_{lt,le,
   gt,ge}` already produces `CEL_ERROR{TYPE_MISMATCH}` on NaN per
   spec), `UnboxBool` (no remaining callers — `LowerConditional` uses
   `cel_bool_from_value` directly), and the `kCelErrTypeMismatch`
   codegen constant.  `LowerComparison` is now string / bytes /
   message eq/ne only (steps 4 / 5 will move those too).  Flipped
   DISABLED_ row 6 (`ThreeValuedAbsorptionNaNCompareAbsorbed`).
   Unit-test updates: `ExprLowerTest.IntComparisonsAreSigned` /
   `UintComparisonsAreUnsigned` / `DoubleComparisons` replaced with
   `*DispatchesToBoxedHelper` variants that assert the runtime-helper
   call shape instead of the removed wasm opcodes.  Scalar arith
   (`LowerArithmetic`, `LowerCheckedIntArith`, double arith) still
   lives on the scalar path at the root — step 7 will take a second
   pass once the whole pipeline is on the uniform ABI.
4. **[shipped 2026-04-20]** Upgrade string / bytes / size helpers
   to absorb non-OK and return CelValue offsets.  Added 9 new
   `_v`-suffixed runtime helpers alongside the existing scalar-return
   ones: `cel_string_eq_v`, `cel_bytes_eq_v`, `cel_string_concat_v`,
   `cel_bytes_concat_v`, `cel_string_starts_with_v` / `_ends_with_v` /
   `_contains_v`, `cel_string_size_v`, `cel_bytes_size_v`.  Each
   (a) routes through `cel_status_either` to pick the dominant non-OK
   (ERROR > UNKNOWN) for two-operand helpers, or passes UNKNOWN / ERROR
   through unchanged for `size_v`; (b) kind-checks both sides for
   `CEL_ERR_TYPE_MISMATCH` on null offset or wrong kind; (c) on OK
   inputs delegates to the existing raw helper (`span_eq`, `span_concat`,
   `span_has_sub`, `cel_string_size`, `cel_bytes_size`) and boxes the
   result via `cel_make_bool` / `cel_make_int` / directly.  Codegen
   swaps in `expr_lower.cc`: `LowerSpanConcat`, `LowerSpanEquality`
   (now also routes `!=` through `cel_not(...eq_v(...))` instead of
   `i32.eqz` so UNKNOWN / ERROR ride through), `LowerStringMemberCall`
   (drops the outer `cel_make_bool` wrap — the `_v` helper returns a
   CelValue offset directly).  For `size()` the scalar-root path still
   uses the raw `cel_string_size` / `cel_bytes_size` (Repr::kInt wants
   raw i64 for the sret box); the boxed path (reached via nested
   compare) routes through the new `LowerExprBoxed` branch that calls
   the `_v` variants.  Flipped DISABLED_ row 15
   (`UnknownThroughStringEqAbsorbedByOr`); added new rows 16
   (`UnknownThroughStartsWithAbsorbedByAndFalse`,
   `UNKNOWN && false → false`), 17
   (`UnknownThroughBytesEqAbsorbedByOr`), and 21
   (`UnknownThroughSizeThenCompareAbsorbed`, `size(UNKNOWN)` rides
   UNKNOWN through `== 0 || true`).  Runtime unit coverage: 37 new
   tests in `cel_runtime_test.cc` exercising happy path,
   left-UNKNOWN-absorbs, right-ERROR-dominates-UNKNOWN, both-UNKNOWN-
   merges, kind mismatch, and zero-offset across all 9 helpers
   (size_v also covers UNKNOWN and ERROR pass-through).  Codegen
   shape tests: 5 `*LowersToRuntimeCall` tests now expect the `_v`
   target names; `StringInequalityInvertsEqualityCall` asserts the
   outer body is `cel_not(cel_string_eq_v(...))`.  The scalar helpers
   (`cel_string_eq`, `cel_string_concat`, etc.) remain reachable via
   the `_v` wrappers; their direct call sites from codegen go away
   when step 7 lands.
5. Upgrade message equality wrapper (caller-side absorption).  Flip
   row 14.
6. Simplify ternary: drop any "at eval root?" contextual logic.
   Flip rows 7, 8, 19.
7. Dead-code sweep: delete unreferenced runtime helpers + scalar-
   path codegen emitters.  Update `cel_runtime_test.cc`.  No
   `// NOLINT` around now-unused declarations — delete them.
8. Doc sweep: update §10.2 / §10.2.2 in the design doc to describe
   the uniform ABI as the single path, not "fast + slow".  Update
   the testing-checklist row for Slice F to tick all 22 rows (or
   the subset that stays in scope for conformance).

No sub-step blocks another in principle, but step 1 is a
prerequisite for 2 / 3 to have anywhere to lower idents from.  Each
step ships tests; no step ships DISABLED_ shells for the next.

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
