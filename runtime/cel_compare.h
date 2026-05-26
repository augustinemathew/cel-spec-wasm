// Comparison helpers — slot-out helper ABI per
// `rewrite/design.md` §4.2.
//
// Every helper has the uniform wasm signature
// `(i32 out_slot, i32 a_slot, i32 b_slot) -> void`.  Result is
// always `{CEL_BOOL, payload.b = 0|1}` on the happy path, with the
// same 3VL / type-mismatch error envelope as the arithmetic
// helpers (see `cel_arith.h`).
//
// **Same-kind only.**  These helpers assume `a.kind == b.kind`;
// any kind drift → `CEL_ERR_TYPE_MISMATCH`.  Cross-type numeric
// equality (`1 == 1u`, `1 == 1.0`, `1u == 1.0`) routes through a
// dedicated `cel_numeric_*` ladder (defines the lossless promotion
// order langdef §"Equality" pins) so this header stays focused on
// the per-kind path.
//
// Bool / null operands have a tiny matrix of their own
// (eq/ne for bool; eq for null since `null == null → true` is
// the only operation langdef defines on the null type).
//
// String / bytes comparisons live in `cel_string_ops.h` because
// they share span-walking machinery with concat / contains.

#ifndef CELWASM_RUNTIME_CEL_COMPARE_H_
#define CELWASM_RUNTIME_CEL_COMPARE_H_

#include <stdint.h>

#include "runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// int64 — full eq/ne/lt/le/gt/ge matrix.
void cel_int_eq_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_int_ne_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_int_lt_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_int_le_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_int_gt_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_int_ge_at_vv(uint32_t out, uint32_t a, uint32_t b);

// uint64 — same matrix.
void cel_uint_eq_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_uint_ne_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_uint_lt_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_uint_le_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_uint_gt_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_uint_ge_at_vv(uint32_t out, uint32_t a, uint32_t b);

// double — IEEE 754 ordering.  NaN comparisons follow IEEE: any
// comparison involving NaN returns false (including NaN == NaN).
// Mirrors cel-cpp `equality_functions.cc::Equal` for double.
void cel_double_eq_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_double_ne_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_double_lt_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_double_le_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_double_gt_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_double_ge_at_vv(uint32_t out, uint32_t a, uint32_t b);

// bool — eq/ne plus ordering (false < true).  Per langdef
// §"Booleans" / cel-cpp `comparison_functions.cc::LessThanBool`,
// CEL defines a total order on bool with `false < true`.
void cel_bool_eq_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_bool_ne_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_bool_lt_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_bool_le_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_bool_gt_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_bool_ge_at_vv(uint32_t out, uint32_t a, uint32_t b);

// null — eq only.  `null == null → true`; type-mismatch (other
// kind on either operand) → CEL_ERR_TYPE_MISMATCH like everywhere
// else.
void cel_null_eq_at_vv(uint32_t out, uint32_t a, uint32_t b);

// Cross-type numeric ladder.  Each helper accepts
// any combination of {CEL_INT, CEL_UINT, CEL_DOUBLE} on either
// operand and produces a CEL_BOOL.  Same-kind pairs delegate to
// the per-kind helpers above; cross-kind pairs follow the
// lossless-promotion ladder cel-cpp pins in
// `internal/number.h::Number::Compare` — negative int never
// equals/exceeds any uint, NaN compares unequal in every direction,
// and double operands outside the int64/uint64 range answer the
// boundary case before any narrowing cast.
//
// Type-mismatch on a non-numeric operand → CEL_ERR_TYPE_MISMATCH;
// 3VL absorption is identical to the same-kind helpers.
void cel_numeric_eq_at_vv(uint32_t out, uint32_t a, uint32_t b);
void cel_numeric_ne_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_numeric_lt_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_numeric_le_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_numeric_gt_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_numeric_ge_at_vv(uint32_t out, uint32_t a, uint32_t b);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_COMPARE_H_
