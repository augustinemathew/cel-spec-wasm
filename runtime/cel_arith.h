// Arithmetic helpers — slot-out helper ABI per
// `rewrite/design.md` §4.2.
//
// Every helper has the uniform wasm signature
// `(i32 out_slot, i32 arg0, ..., i32 argN-1) -> void`.  Bodies live
// in `cel_runtime.c`; this header keeps the topic separate so call
// sites can include only what they use (per `rewrite/design.md`
// §3 source layout).
//
// Semantics are cel-cpp parity (per-helper inline pointer in the
// .c body, citing
// `third_party/cel-cpp/runtime/standard/arithmetic_functions.cc`).
// In summary:
//
//   - Operands are read out of `arg*_slot`s as 24-byte CelValues.
//     Wrong kind on either operand → `out_slot = {CEL_ERROR,
//     err = CEL_ERR_TYPE_MISMATCH}`.  Cross-type numeric arith
//     (`1 + 1u`) routes through the kCall arm's overload-id
//     resolution to the correct same-kind helper after coercion;
//     these helpers only handle uniform-kind operand pairs.
//
//   - 3VL absorption: `CEL_UNKNOWN`/`CEL_ERROR` on either operand
//     propagates verbatim into `out_slot` — UNKNOWN+UNKNOWN merges
//     via `cel_unknown_merge`; ERROR is left-bias.
//
//   - Int / uint overflow → `out_slot = {CEL_ERROR,
//     err = CEL_ERR_OVERFLOW}` per langdef §"Numeric values"
//     (NOT wrap).  Detected via `__builtin_*_overflow`; cel-cpp
//     uses the same intrinsics.
//
//   - Int / uint div-by-zero → `CEL_ERR_DIVIDE_BY_ZERO`.  Mod-by-
//     zero → `CEL_ERR_MODULUS_BY_ZERO`.  Double divides by zero
//     produce IEEE 754 inf/nan (NOT errors) per langdef.

#ifndef CELWASM_RUNTIME_CEL_ARITH_H_
#define CELWASM_RUNTIME_CEL_ARITH_H_

#include <stdint.h>

#include "runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// int64 — overflow detected via __builtin_*_overflow.  `cel_int_neg`
// is unary (one arg).  Modulus follows C99 truncation-toward-zero
// rules (langdef §"Numeric values" pins the same).
// cel:codegen-export
void cel_int_add_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_int_sub_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_int_mul_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_int_div_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_int_mod_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_int_neg_at_v(uint32_t out, uint32_t v);

// uint64 — overflow detected via __builtin_*_overflow on the
// unsigned types.  No unary negation (CEL spec disallows
// `-uint`).  Mod follows the same C99 rules; uint operands make
// the sign question moot.
// cel:codegen-export
void cel_uint_add_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_uint_sub_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_uint_mul_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_uint_div_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_uint_mod_at_vv(uint32_t out, uint32_t a, uint32_t b);

// double — IEEE 754; overflow / underflow / div-by-zero produce
// inf/nan, NOT errors (langdef §"Numeric values": "Double values
// follow IEEE 754").  No modulus operator on doubles in CEL.
// cel:codegen-export
void cel_double_add_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_double_sub_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_double_mul_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_double_div_at_vv(uint32_t out, uint32_t a, uint32_t b);
// cel:codegen-export
void cel_double_neg_at_v(uint32_t out, uint32_t v);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_ARITH_H_
