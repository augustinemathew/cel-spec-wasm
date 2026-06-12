// Arithmetic helpers — int / uint / double add / sub / mul / div /
// mod / neg.
//
// Carved out of cel_runtime.c per
// `rewrite/cel-runtime-c-split-plan.md`.  See cel_arith.h for the
// public ABI; `rewrite/wat-traces.md` traces 16-17 lock the wire
// shape.

#include "runtime/cel_arith.h"

#include <stdint.h>

#include "runtime/cel_arena.h"
#include "runtime/cel_internal.h"

// Manual u64 multiply-overflow detection via split 32×32→64
// partial products.  We avoid `__builtin_mul_overflow` for 64-bit
// operands because clang lowers it through `__multi3` (a 128-bit
// multiply from compiler-rt) — and the wasm32 freestanding build
// doesn't link compiler-rt by design.  We avoid the divide-by-b
// bounds-check shape too: clang's optimiser recognises it and
// re-folds it back into a `__multi3` call.  Splitting into
// 32-bit halves means every multiply here is a 32×32→64 op the
// wasm32 backend lowers natively as `i64.mul`, with no high-half
// reasoning the optimiser can lift to 128-bit math.
//
// Logic:
//   a*b = (ah*2^32 + al) * (bh*2^32 + bl)
//       = al*bl + (ah*bl + al*bh)*2^32 + ah*bh*2^64
//   Overflow iff (ah * bh) != 0
//                OR (ah*bl + al*bh) overflows 32 bits
//                OR adding the shifted middle to al*bl overflows.
static int uint64_mul_overflows(uint64_t a, uint64_t b, uint64_t* r) {
  uint64_t ah = a >> 32;
  uint64_t al = a & 0xFFFFFFFFULL;
  uint64_t bh = b >> 32;
  uint64_t bl = b & 0xFFFFFFFFULL;
  if (ah != 0 && bh != 0) return 1;
  uint64_t mid = (ah * bl) + (al * bh);  // operands ≤32 bits, sum may carry
  if ((mid >> 32) != 0) return 1;
  uint64_t lo = al * bl;
  uint64_t result = lo + (mid << 32);
  if (result < lo) return 1;  // unsigned add overflow → product > UINT64_MAX
  *r = result;
  return 0;
}

// Signed int64 mul overflow on top of the unsigned check: take
// magnitudes, run the unsigned check, then validate the signed
// range against INT64_MIN / INT64_MAX based on operand signs.
static int int64_mul_overflows(int64_t a, int64_t b, int64_t* r) {
  if (a == 0 || b == 0) {
    *r = 0;
    return 0;
  }
  // INT64_MIN handled specially: |INT64_MIN| > INT64_MAX, so the
  // standard magnitude trick can't represent it.  Only INT64_MIN*1
  // and INT64_MIN*0 don't overflow; we already handled 0 above.
  if (a == INT64_MIN) return b != 1 ? 1 : (*r = INT64_MIN, 0);
  if (b == INT64_MIN) return a != 1 ? 1 : (*r = INT64_MIN, 0);
  uint64_t ua = (uint64_t)(a < 0 ? -a : a);
  uint64_t ub = (uint64_t)(b < 0 ? -b : b);
  uint64_t up;
  if (uint64_mul_overflows(ua, ub, &up)) return 1;
  // Signs determine whether result is +/-.  Range:
  //   positive: [0, INT64_MAX]
  //   negative: [INT64_MIN, 0]   (so |result| ≤ -(INT64_MIN+1)+1 = INT64_MAX+1)
  int negative = (a < 0) ^ (b < 0);
  if (negative) {
    if (up > (uint64_t)INT64_MAX + 1ULL) return 1;
    *r = -(int64_t)up;
  } else {
    if (up > (uint64_t)INT64_MAX) return 1;
    *r = (int64_t)up;
  }
  return 0;
}

// ---- int64 arithmetic ----------------------------------------------------
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/arithmetic_functions.cc::
//     add_int64 / sub_int64 / mul_int64 / div_int64 / mod_int64 /
//     negate_int64

void cel_int_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  int64_t r;
  if (__builtin_add_overflow(a->payload.i, b->payload.i, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, r);
}

void cel_int_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  int64_t r;
  if (__builtin_sub_overflow(a->payload.i, b->payload.i, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, r);
}

void cel_int_mul_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  int64_t r;
  if (int64_mul_overflows(a->payload.i, b->payload.i, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, r);
}

void cel_int_div_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  if (b->payload.i == 0) {
    poison(out, CEL_ERR_DIVIDE_BY_ZERO);
    return;
  }
  // INT64_MIN / -1 overflows in two's complement.
  if (a->payload.i == INT64_MIN && b->payload.i == -1) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, a->payload.i / b->payload.i);
}

void cel_int_mod_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  if (b->payload.i == 0) {
    poison(out, CEL_ERR_MODULUS_BY_ZERO);
    return;
  }
  // INT64_MIN % -1 is undefined behaviour in C (the implied division
  // overflows).  cel-cpp treats it as an integer-overflow ERROR, not 0:
  // CheckedMod errors on `x == INT64_MIN && y == -1` before computing
  // (third_party/cel-cpp/internal/overflow.cc CheckedMod).  Mirror the
  // divide case above.
  if (a->payload.i == INT64_MIN && b->payload.i == -1) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, a->payload.i % b->payload.i);
}

void cel_int_neg_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (v->payload.i == INT64_MIN) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, -v->payload.i);
}

// ---- uint64 arithmetic ---------------------------------------------------

void cel_uint_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  uint64_t r;
  if (__builtin_add_overflow(a->payload.u, b->payload.u, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, r);
}

void cel_uint_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  uint64_t r;
  if (__builtin_sub_overflow(a->payload.u, b->payload.u, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, r);
}

void cel_uint_mul_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  uint64_t r;
  if (uint64_mul_overflows(a->payload.u, b->payload.u, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, r);
}

void cel_uint_div_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  if (b->payload.u == 0) {
    poison(out, CEL_ERR_DIVIDE_BY_ZERO);
    return;
  }
  write_uint(out, a->payload.u / b->payload.u);
}

void cel_uint_mod_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  if (b->payload.u == 0) {
    poison(out, CEL_ERR_MODULUS_BY_ZERO);
    return;
  }
  write_uint(out, a->payload.u % b->payload.u);
}

// ---- double arithmetic ---------------------------------------------------
// Per langdef §"Numeric values": double follows IEEE 754.  No
// overflow / div-by-zero errors — inf/nan results are valid.

void cel_double_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d + b->payload.d);
}

void cel_double_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d - b->payload.d);
}

void cel_double_mul_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d * b->payload.d);
}

void cel_double_div_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d / b->payload.d);
}

void cel_double_neg_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_double(out, -v->payload.d);
}
