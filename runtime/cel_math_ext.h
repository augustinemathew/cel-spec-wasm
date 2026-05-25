// CEL `math` extension kernels — self-hosted in cel_runtime.wasm.
//
// Public ABI for the cel-cpp `math` extension functions.  All 20
// kernels follow the runtime's out-slot convention: every parameter
// is a `uint32_t` byte offset into the shared linear memory naming a
// 24-byte CelValue; results land in `out_slot` (the kernel returns
// void).  Suffix encodes arity: `_at_v` = out + 1 value (2 params),
// `_at_vv` = out + 2 values (3 params).
//
// `math.greatest` / `math.least` never reach codegen as such — the
// cel-cpp parser macros expand them at parse time into global
// `math.@max` / `math.@min` calls (scalar unary, list unary, or
// binary), so the runtime surface is the four @min/@max kernels here
// plus `cel_copy_slot` for the unary-scalar identity case.  See
// `doc/implementation-plan/rewrite/m16-ast-probe-findings.md`.
//
// 3VL / error handling mirrors the arith kernels: ERROR / UNKNOWN
// operands are absorbed verbatim into `out_slot`; a wrong-kind operand
// poisons `out_slot` with CEL_ERR_TYPE_MISMATCH.  Cross-type min/max
// and the list folds dispatch on each operand's runtime CelKind via
// the shared numeric compare ladder.

#ifndef CELWASM_RUNTIME_CEL_MATH_EXT_H_
#define CELWASM_RUNTIME_CEL_MATH_EXT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Scalar: rounding (double → double) ────────────────────────────
// std::ceil / floor / round / trunc.  Wrong kind → CEL_ERR_TYPE_MISMATCH.
void cel_math_ceil_at_v(uint32_t out_slot, uint32_t v_slot);
void cel_math_floor_at_v(uint32_t out_slot, uint32_t v_slot);
void cel_math_round_at_v(uint32_t out_slot, uint32_t v_slot);
void cel_math_trunc_at_v(uint32_t out_slot, uint32_t v_slot);

// ── Scalar: float predicates (double → bool) ──────────────────────
void cel_math_is_inf_at_v(uint32_t out_slot, uint32_t v_slot);
void cel_math_is_nan_at_v(uint32_t out_slot, uint32_t v_slot);
void cel_math_is_finite_at_v(uint32_t out_slot, uint32_t v_slot);

// ── Scalar: magnitude / sign (int / uint / double, kind-dispatch) ─
// abs(int) overflows on INT64_MIN (→ CEL_ERR_OVERFLOW); abs(uint) is
// identity.  sign returns the operand kind: int/uint → {-1,0,1} (uint
// never -1); double → {-1.0, 0.0, 1.0} with NaN passed through.
void cel_math_abs_at_v(uint32_t out_slot, uint32_t v_slot);
void cel_math_sign_at_v(uint32_t out_slot, uint32_t v_slot);

// ── Scalar: sqrt (int / uint / double → double) ───────────────────
// Always double; sqrt of a negative operand yields NaN (no error).
void cel_math_sqrt_at_v(uint32_t out_slot, uint32_t v_slot);

// ── Bitwise (int / uint; shift amount always int) ─────────────────
void cel_math_bit_and_at_vv(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot);
void cel_math_bit_or_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_math_bit_xor_at_vv(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot);
void cel_math_bit_not_at_v(uint32_t out_slot, uint32_t v_slot);
void cel_math_bit_shift_left_at_vv(uint32_t out_slot, uint32_t x_slot,
                                   uint32_t n_slot);
void cel_math_bit_shift_right_at_vv(uint32_t out_slot, uint32_t x_slot,
                                    uint32_t n_slot);

// ── Variadic min / max (post-macro math.@min / math.@max) ─────────
// Binary forms dispatch on the two operands' runtime kinds (cross-type
// numeric); list forms fold a CEL_LIST_ARENA of numerics.  Result kind
// is the winning operand's kind (dyn at the checker level for
// cross-type / mixed-list).
void cel_math_min_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_math_max_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_math_min_list_at_v(uint32_t out_slot, uint32_t list_slot);
void cel_math_max_list_at_v(uint32_t out_slot, uint32_t list_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_MATH_EXT_H_
