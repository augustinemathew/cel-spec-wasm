# M8 wrapper probe — empirical cel-cpp behaviour

Throwaway probe at `compiler_v2/throwaway/m8_wrapper_probe.cc` runs an
exhaustive matrix through the vendored cel-cpp `Compiler` +
`StandardRuntimeBuilder` against `cel.expr.conformance.proto3.TestAllTypes`
(and proto2 where relevant).  Full output:
`compiler_v2/throwaway/m8_wrapper_probe.output.txt`.

The KEY QUESTIONS (rows in the task spec marked `???`) resolved as
follows.

---

## KEY QUESTIONS — answers

| Row | Expression | Result | Interpretation |
|-----|------------|--------|----------------|
| A4 | `Int32Value{} == null` | `false` | A wrapper LITERAL with no `value` field set is still a present-message, not null.  `==` peels it to the default scalar (`0`), and `0 == null` is `false`. |
| A5 | `Int32Value{value: 0} == null` | `false` | Same as A4 — explicitly set-to-zero is a present message; peels to `0`; `0 == null` is `false`. |
| A6 | `Int32Value{value: 1} != null` | `true` | Symmetric — wrapper literal is never null. |
| A8 | `Int32Value{value:1} == "1"` | **compile-time TYPE ERROR** (`no matching overload for '_==_' applied to '(wrapper(int), string)'`) | The checker carries `wrapper(int)`, and `==` requires the operand types to match after peeling.  M8 must reject this at check time, not runtime. |
| B1 | `TestAllTypes{...}.single_int32_wrapper` (set) | `1` | Field read of a SET wrapper field auto-peels to the scalar payload. |
| B3 | `TestAllTypes{}.single_int32_wrapper == null` | **`false` (!)** | **SURPRISE — contradicts the task's expectation.**  cel-cpp's standard runtime returns the WRAPPER ZERO (i.e. `0`) for an UNSET wrapper field, not `null`.  Compare `null` and `0` → `false`. |
| B4 | `TestAllTypes{}.single_int32_wrapper == 0` | `true` | Confirms B3 — unset wrapper field reads as default `0`, not `null`. |
| B5 | `TestAllTypes{single_int32_wrapper: 0}.single_int32_wrapper == null` | `false` | Set-to-zero peels to `0`; `0 == null` is `false`.  Same behaviour as unset (B3). |
| B6 (proto2) | same on `proto2.TestAllTypes` | `false` | proto2 and proto3 wrapper-field semantics agree — both peel unset to scalar default. |
| C4 | `TestAllTypes{single_int32_wrapper: "5"}` | **compile-time TYPE ERROR** (`expected 'wrapper(int)' but provided 'string'`) | Construction-side type mismatch fails the checker.  M8's frontend must enforce. |
| C5 | `TestAllTypes{single_uint32_wrapper: -1}` | **compile-time TYPE ERROR** (`expected 'wrapper(uint)' but provided 'int'`) | int-into-uint-wrapper is rejected at check time; no implicit numeric coercion across signedness. |
| E-d-nan | `DoubleValue{value: NaN} == NaN` | `false` | IEEE-754 NaN inequality propagates through wrapper-peel; M8 runtime equality on wrappers must use scalar `==`, not bitwise. |

---

## Additional findings worth folding into the M8 plan

1. **Wrapper field read peels to scalar even when unset.**  The CEL spec
   text "unset wrapper field reads as `null`" appears NOT to be honoured
   by cel-cpp's `StandardRuntimeBuilder`.  Both `B3` and `B4` confirm an
   unset `single_int32_wrapper` reads as scalar `0`, not `null`.  This
   matters for our M8 frontend: if we model wrapper fields as
   `optional<int>` in the type system, we'll mis-typecheck programs
   that cel-cpp accepts.  The type carried for the read is plain `int`
   (per `B-type-set` and `B-type-unset` → both report `int`).

   > Implication: M8 should follow cel-cpp's lead — wrapper field reads
   > are typed as the scalar (int / uint / double / string / bytes /
   > bool), not as `null|scalar`.  Presence is observable ONLY through
   > `has()`, never through `== null`.

2. **`has(msg.wrapper_field)` works as expected** (B7=true for set,
   B8=false for unset) — presence is independent of the value.  Use
   `has()` for the "was it set?" question; `==` always peels.

3. **Wrapper literals are never `== null`.**  Even `Int32Value{}` (no
   `value` set) compares not-equal to `null`.  Only an `optional`
   field set to `null` or a typed `null` literal yields null.

4. **Type-checker rejects all malformed wrapper uses.**  Wrong scalar
   kind into wrapper-typed field (`"5"` → `wrapper(int)`), wrong
   signedness (`-1` → `wrapper(uint)`), wrapper-vs-non-matching-scalar
   equality (`wrapper(int) == string`) all fail at check time.  M8's
   frontend should reuse this — it's the cel-cpp checker doing the
   work, not our codegen.

5. **Wrapper auto-wrap on construction is admitted for matching
   scalar kind.**  `TestAllTypes{single_int32_wrapper: 5}` is fine
   (C1 = true); `null` into a wrapper-typed field also admitted
   syntactically (C2 — compiles, evaluates to `null` in field-read,
   then `null == null` → `false` because cel-cpp returns scalar `0`
   for the read after construction with `null`... actually C2 reports
   `false` which means construction with `null` materialised as
   "unset", and unset reads as `0`, and `0 == null` → `false`.  Same
   shape for `C-null-bool`, `C-null-str`, `C-null-bytes`).

6. **Any-contained wrapper peels through Any.**  All D-rows return
   `true` and `D-type-i32` reports `int`.  M8 should ensure the
   existing M7a Any-unwrap path chains into the wrapper-peel path
   (the field-read carve-out from M7a.B presumably already does
   this — confirm in code review).

7. **Cross-form symmetry holds.**  F1, F1-rev, F-unset-vs-litzero,
   F-setzero-vs-litzero all return `true`.  Literal-vs-field-read
   equality is fully symmetric; an unset field equals an empty
   wrapper literal equals a zero literal — all three peel to the
   scalar default and compare equal.

8. **Boundary values pass cleanly.**  INT32_MIN/MAX, INT64_MIN/MAX,
   UINT32/64_MAX, FLT/DBL infinities all round-trip via wrapper
   literal-construct + scalar-peel + `==`.  NaN propagates IEEE
   inequality as expected.

---

## What changes in the M8 plan because of this

- **Drop "unset wrapper field reads as `null`" from the plan if it
  was there.**  cel-cpp's StandardRuntime peels to scalar default,
  not to null.  Modify our M8 codegen accordingly.
- **`== null` on wrapper field reads is always `false`.**  This is
  trivially derivable from the above and should be a test row.
- **`has()` is the only way to observe wrapper-field presence.**
  Test row: `has(msg.wrapper_field)` is `true` when set (even to
  default zero), `false` when unset.
- **Construction-side type-checking is delegated to cel-cpp's
  checker.**  We don't need to add bespoke wrapper-kind validation
  in our frontend — `RejectDyn` + the checker's wrapper-type-aware
  field-assignment check already rejects every malformed shape.
- **NaN equality is IEEE-style false.**  Our M8 wrapper-`==`
  trampoline (or inline lowering) must use scalar `==`, not bitwise.
- **Any-contained wrapper chains.**  Confirmed working at the
  cel-cpp level; ensure our wat traces for M8 include an
  Any-contained wrapper case so the chaining is e2e-verified.
