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

This is big enough to warrant its own slice.  Slicing internally
(F1: scalar → CelValue ABI for comparisons; F2: arithmetic slow
path; F3: string / bytes ops; F4: cross-wiring with comprehension
aggregators) is likely but not mapped out yet.

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

## Also-deferred companion: CLI flag for unknown attrs

`celwasmc --eval --unknown-attrs=var.field,…` (was M4 Slice E2a.2)
is deferred with Slice F.  Rationale: until Slice F lands, the CLI
flag would let users write expressions whose runtime behavior
disagrees with the spec, and the quietly-wrong answers are worse
than "no CLI support yet".  E2a.1 will expose the
`CelHost::SetUnknownAttrs` API for C++ test harnesses; the CLI
surface waits.
