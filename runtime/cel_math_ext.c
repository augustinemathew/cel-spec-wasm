// CEL `math` extension kernels — self-hosted in cel_runtime.wasm.
//
// Single TU for the whole math extension (public ABI in
// cel_math_ext.h).  Sections: scalar, bitwise, min/max.  Semantics
// mirror cel-cpp `extensions/math_ext.cc` verbatim; the conformance
// fixture `tests/simple/testdata/math_ext.textproto` is the
// assertion source of truth.  See
// `doc/implementation-plan/rewrite/m16-math-ext.md`.

#include "runtime/cel_math_ext.h"

#include <math.h>
#include <stdint.h>

#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_internal.h"
#include "runtime/cel_list.h"

// ════════════════════════════════════════════════════════════════
// Scalar: rounding (double → double).  std::ceil / floor / round /
// trunc; clang lowers ceil/floor/trunc to native wasm f64 ops.
// ════════════════════════════════════════════════════════════════

void cel_math_ceil_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_double(out, ceil(v->payload.d));
}

void cel_math_floor_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_double(out, floor(v->payload.d));
}

void cel_math_round_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // C round(): half away from zero (langdef / cel-cpp parity); NOT
  // wasm f64.nearest (half-to-even).
  write_double(out, round(v->payload.d));
}

void cel_math_trunc_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_double(out, trunc(v->payload.d));
}

// ════════════════════════════════════════════════════════════════
// Scalar: float predicates (double → bool).
// ════════════════════════════════════════════════════════════════

void cel_math_is_inf_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_bool(out, isinf(v->payload.d));
}

void cel_math_is_nan_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_bool(out, isnan(v->payload.d));
}

void cel_math_is_finite_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_bool(out, isfinite(v->payload.d));
}

// ════════════════════════════════════════════════════════════════
// Scalar: magnitude / sign (int / uint / double, kind-dispatch).
// A non-numeric operand (e.g. a dyn-erased bool reaching at runtime)
// poisons with CEL_ERR_TYPE_MISMATCH — the legitimate runtime-error
// path, not an invariant violation, so `default:` is a real arm.
// ════════════════════════════════════════════════════════════════

void cel_math_abs_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  switch (v->kind) {
    case CEL_INT:
      if (v->payload.i == INT64_MIN) {
        poison(out, CEL_ERR_OVERFLOW);  // |INT64_MIN| is unrepresentable.
        return;
      }
      write_int(out, v->payload.i < 0 ? -v->payload.i : v->payload.i);
      return;
    case CEL_UINT:
      write_uint(out, v->payload.u);  // abs(uint) is identity.
      return;
    case CEL_DOUBLE:
      write_double(out, fabs(v->payload.d));
      return;
    default:
      poison(out, CEL_ERR_TYPE_MISMATCH);
      return;
  }
}

void cel_math_sign_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  switch (v->kind) {
    case CEL_INT: {
      int64_t s = 0;
      if (v->payload.i < 0) {
        s = -1;
      } else if (v->payload.i > 0) {
        s = 1;
      }
      write_int(out, s);
      return;
    }
    case CEL_UINT:
      write_uint(out, v->payload.u == 0 ? 0 : 1);
      return;
    case CEL_DOUBLE: {
      const double d = v->payload.d;
      if (isnan(d)) {
        write_double(out, d);  // sign(NaN) = NaN (cel-cpp parity).
      } else if (d == 0.0) {
        write_double(out, 0.0);
      } else {
        write_double(out, signbit(d) ? -1.0 : 1.0);
      }
      return;
    }
    default:
      poison(out, CEL_ERR_TYPE_MISMATCH);
      return;
  }
}

// ════════════════════════════════════════════════════════════════
// Scalar: sqrt (int / uint / double → double).  Always double; sqrt
// of a negative operand yields NaN (no error), per std::sqrt.
// ════════════════════════════════════════════════════════════════

void cel_math_sqrt_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  switch (v->kind) {
    case CEL_INT:
      write_double(out, sqrt((double)v->payload.i));
      return;
    case CEL_UINT:
      write_double(out, sqrt((double)v->payload.u));
      return;
    case CEL_DOUBLE:
      write_double(out, sqrt(v->payload.d));
      return;
    default:
      poison(out, CEL_ERR_TYPE_MISMATCH);
      return;
  }
}

// ════════════════════════════════════════════════════════════════
// Bitwise (int / uint; shift amount always int).  cel-cpp parity:
// bitAnd/Or/Xor/Not are the plain C operators; bitShift* reject a
// negative offset (CEL_ERR_INVALID_ARGUMENT), saturate a count > 63
// to 0, and bitShiftRight is a LOGICAL shift on int (no sign
// extension) — see math_ext.cc BitShiftRightInt.  Operands of
// mismatched / non-integer kind poison with CEL_ERR_TYPE_MISMATCH.
// ════════════════════════════════════════════════════════════════

void cel_math_bit_and_at_vv(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (a->kind == CEL_INT && b->kind == CEL_INT) {
    write_int(out, a->payload.i & b->payload.i);
  } else if (a->kind == CEL_UINT && b->kind == CEL_UINT) {
    write_uint(out, a->payload.u & b->payload.u);
  } else {
    poison(out, CEL_ERR_TYPE_MISMATCH);
  }
}

void cel_math_bit_or_at_vv(uint32_t out_slot, uint32_t a_slot,
                           uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (a->kind == CEL_INT && b->kind == CEL_INT) {
    write_int(out, a->payload.i | b->payload.i);
  } else if (a->kind == CEL_UINT && b->kind == CEL_UINT) {
    write_uint(out, a->payload.u | b->payload.u);
  } else {
    poison(out, CEL_ERR_TYPE_MISMATCH);
  }
}

void cel_math_bit_xor_at_vv(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (a->kind == CEL_INT && b->kind == CEL_INT) {
    write_int(out, a->payload.i ^ b->payload.i);
  } else if (a->kind == CEL_UINT && b->kind == CEL_UINT) {
    write_uint(out, a->payload.u ^ b->payload.u);
  } else {
    poison(out, CEL_ERR_TYPE_MISMATCH);
  }
}

void cel_math_bit_not_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind == CEL_INT) {
    write_int(out, ~v->payload.i);
  } else if (v->kind == CEL_UINT) {
    write_uint(out, ~v->payload.u);
  } else {
    poison(out, CEL_ERR_TYPE_MISMATCH);
  }
}

// Shared shift-offset validation: the count operand must be CEL_INT
// and non-negative.  Returns the count (>= 0) via *shift and 1 on OK;
// poisons `out` and returns 0 otherwise.
static int math_shift_offset(CelValue* out, const CelValue* n, int64_t* shift) {
  if (n->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 0;
  }
  if (n->payload.i < 0) {
    poison(out, CEL_ERR_INVALID_ARGUMENT);  // "negative offset"
    return 0;
  }
  *shift = n->payload.i;
  return 1;
}

void cel_math_bit_shift_left_at_vv(uint32_t out_slot, uint32_t x_slot,
                                   uint32_t n_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* x = cel_value_at(x_slot);
  const CelValue* n = cel_value_at(n_slot);
  if (absorb_3vl_binary(out, x, n)) return;
  int64_t shift;
  if (!math_shift_offset(out, n, &shift)) return;
  // Shift in unsigned space to avoid signed-overflow UB; the bit
  // pattern is identical to cel-cpp's `lhs << rhs`.
  if (x->kind == CEL_INT) {
    write_int(out, shift > 63 ? 0 : (int64_t)((uint64_t)x->payload.i << shift));
  } else if (x->kind == CEL_UINT) {
    write_uint(out, shift > 63 ? 0u : x->payload.u << shift);
  } else {
    poison(out, CEL_ERR_TYPE_MISMATCH);
  }
}

void cel_math_bit_shift_right_at_vv(uint32_t out_slot, uint32_t x_slot,
                                    uint32_t n_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* x = cel_value_at(x_slot);
  const CelValue* n = cel_value_at(n_slot);
  if (absorb_3vl_binary(out, x, n)) return;
  int64_t shift;
  if (!math_shift_offset(out, n, &shift)) return;
  // LOGICAL shift on int too (no sign extension) — cel-cpp parity.
  if (x->kind == CEL_INT) {
    write_int(out, shift > 63 ? 0 : (int64_t)((uint64_t)x->payload.i >> shift));
  } else if (x->kind == CEL_UINT) {
    write_uint(out, shift > 63 ? 0u : x->payload.u >> shift);
  } else {
    poison(out, CEL_ERR_TYPE_MISMATCH);
  }
}

// ════════════════════════════════════════════════════════════════
// Variadic min / max — the post-macro math.@min / math.@max surface.
// cel-cpp parity (MinNumber / MaxNumber): the result is the WINNING
// operand copied verbatim (its kind preserved), compared via the
// cross-type numeric ladder.  A comparison involving NaN is false in
// cel-cpp, so NaN keeps the current/first operand — which maps to
// `kCmpNanInequal` never triggering a replace below.  Cross-type and
// mixed-list overloads are dyn at the checker level; the kernel keys
// off each operand's runtime kind, not a pinned overload id.  See
// `m16-ast-probe-findings.md` + `wat/m16_math_min_list.wat`.
// ════════════════════════════════════════════════════════════════

// Should `cand` replace the running `best`?  For min: cand < best
// (compare(best,cand)==kCmpGreater).  For max: cand > best
// (compare(best,cand)==kCmpLess).  NaN (kCmpNanInequal) → no replace.
static int math_minmax_replaces(const CelValue* best, const CelValue* cand,
                                int want_greater) {
  CmpResult c = numeric_compare_kernel(best, cand);
  if (want_greater) return c == kCmpLess;  // max: replace if cand > best
  return c == kCmpGreater;                 // min: replace if cand < best
}

static void math_minmax_binary(uint32_t out_slot, uint32_t a_slot,
                               uint32_t b_slot, int want_greater) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (!is_numeric_kind(a->kind) || !is_numeric_kind(b->kind)) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  *out = math_minmax_replaces(a, b, want_greater) ? *b : *a;
}

void cel_math_min_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  math_minmax_binary(out_slot, a_slot, b_slot, /*want_greater=*/0);
}

void cel_math_max_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  math_minmax_binary(out_slot, a_slot, b_slot, /*want_greater=*/1);
}

static CelValue* math_arena_list_element(uint32_t header_ptr, uint32_t i) {
  ArenaListHeader* hdr = (ArenaListHeader*)(cel_memory_base_() + header_ptr);
  return (CelValue*)(cel_memory_base_() + hdr->elements_offset +
                     ((size_t)i * sizeof(CelValue)));
}

// Element count of an arena list header, tolerating the header-less
// shape (`header_ptr == 0`) that `cel_list_arena_view` vends when a
// host snapshot had nothing to snapshot — offset 0 is never a real
// allocation, so it must not be dereferenced.
static uint32_t math_arena_list_count(uint32_t header_ptr) {
  if (header_ptr == 0) return 0;
  return ((ArenaListHeader*)(cel_memory_base_() + header_ptr))->count;
}

static void math_minmax_list(uint32_t out_slot, uint32_t list_slot,
                             int want_greater) {
  const CelValue* list = cel_value_at(list_slot);
  if (absorb_3vl_unary(cel_value_at(out_slot), list)) return;
  if (list->kind != CEL_LIST_ARENA && list->kind != CEL_LIST_HOST) {
    poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Normalise origin: an arena operand passes through as itself, a
  // host-backed one is snapshotted into the arena via
  // `cel_host.cel_list_iter_open` — the same lift `list.join()` and
  // the comprehension prologue take.  Every pointer must be
  // re-derived afterwards: the snapshot allocates, and `arena_alloc`
  // may `memory.grow` and relocate the linear-memory base.
  const uint32_t view_slot = cel_list_arena_view(list_slot);
  const uint32_t header_ptr =
      cel_value_at(view_slot)->payload.arena_list.header_ptr;
  const uint32_t count = math_arena_list_count(header_ptr);
  if (count == 0) {
    // cel-cpp `extensions/math_ext.cc:106` / `:152`:
    // "math.@min argument must not be empty".
    poison(cel_value_at(out_slot), CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  CelValue best = *math_arena_list_element(header_ptr, 0);
  for (uint32_t i = 0; i < count; ++i) {
    const CelValue* e = math_arena_list_element(header_ptr, i);
    if (e->kind == CEL_ERROR || e->kind == CEL_UNKNOWN) {
      *cel_value_at(out_slot) = *e;  // 3VL propagation.
      return;
    }
    if (!is_numeric_kind(e->kind)) {
      poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
      return;
    }
    if (i != 0 && math_minmax_replaces(&best, e, want_greater)) best = *e;
  }
  *cel_value_at(out_slot) = best;
}

void cel_math_min_list_at_v(uint32_t out_slot, uint32_t list_slot) {
  math_minmax_list(out_slot, list_slot, /*want_greater=*/0);
}

void cel_math_max_list_at_v(uint32_t out_slot, uint32_t list_slot) {
  math_minmax_list(out_slot, list_slot, /*want_greater=*/1);
}
