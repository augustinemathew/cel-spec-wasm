# M4 — Three-valued logic (OK / UNKNOWN / ERROR)

Status: **slices A+B shipped (2026-04-19); slices C-G in progress.**
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

### Codegen

- [x] Arithmetic ops grow an **overflow check** on int.  Slice B
      (2026-04-19): codegen dispatches `_+_` / `_-_` / `_*_` / `_/_` /
      `_%_` on int/uint through the B1 runtime helpers
      (`cel_int_add_ii` etc.).  The emitted shape is
      `Block(LocalSet(Call(helper)), If(kind==CEL_ERROR, unreachable),
      i64.load offset=8)` — on ERROR the trap surfaces through
      wasmtime as an `absl::InternalError("... trapped: ...")`.  The
      CEL-correct "observable ERROR value" path lands with the 3VL
      &&/|| retrofit in a later slice; the trap is the stopgap so the
      "INT_MAX + 1" testing-checklist row is closed today.
- [x] `/` and `%` grow a **zero-divisor check**.  (Slice B: division
      and modulo go through the same checked helpers; `_uint_ / 0`,
      `_int_ / 0`, `INT64_MIN / -1`, and `_int_ % 0` all produce a
      CEL_ERROR that trips the trap-on-ERROR path.  `INT64_MIN % -1`
      is defined as 0 per the helper, matching cel-go.)
- [ ] Double comparisons convert "unordered" results (NaN inputs)
      into an `ERROR` return instead of the plain `0` / `1` i32 that
      M2 emits.
- [ ] `&&` / `||` switch from M2's scalar short-circuit to the
      three-valued `cel_and` / `cel_or` helpers.  The codegen
      inspects both operand Reprs and picks the scalar-only path
      when the checker has guaranteed both are definite booleans —
      this is an important optimisation because without it every
      boolean expression pays the three-valued overhead.
- [ ] `?:` grows the same treatment: when the condition is
      UNKNOWN / ERROR, the ternary returns the same (per spec).
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
