// Comparison helpers — same-kind eq/ne/lt/le/gt/ge for int/uint/
// double/bool, cross-type numeric ladder, null equality, and the
// polymorphic equality dispatcher.
//
// `numeric_compare_kernel` + `is_numeric_kind` are internal-extern in
// cel_internal.h so `cel_value_eq` (which lives in cel_runtime.c
// alongside the entangled list/map operations) can reach them.
//
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/equality_functions.cc
//   third_party/cel-cpp/runtime/standard/comparison_functions.cc

#include "runtime/cel_compare.h"

#include <stdint.h>

#include "runtime/cel_arena.h"
#include "runtime/cel_internal.h"
#include "runtime/cel_list.h"  // cel_list_eq dispatcher
#include "runtime/cel_log.h"
#include "runtime/cel_map.h"  // cel_map_eq dispatcher

// ─────────────────────────────────────────────────────────────
// Same-kind comparison helpers via DEFINE_CMP_VV macro expansion.
// ─────────────────────────────────────────────────────────────

#define DEFINE_CMP_VV(name, kind, field, op)                       \
  void name(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) { \
    CelValue* out = cel_value_at(out_slot);                        \
    const CelValue* a = cel_value_at(a_slot);                      \
    const CelValue* b = cel_value_at(b_slot);                      \
    if (absorb_3vl_binary(out, a, b)) return;                      \
    if (require_kinds(out, a, b, kind)) return;                    \
    write_bool(out, a->payload.field op b->payload.field);         \
  }

DEFINE_CMP_VV(cel_int_lt_at_vv, CEL_INT, i, <)
DEFINE_CMP_VV(cel_int_le_at_vv, CEL_INT, i, <=)
DEFINE_CMP_VV(cel_int_gt_at_vv, CEL_INT, i, >)
DEFINE_CMP_VV(cel_int_ge_at_vv, CEL_INT, i, >=)

DEFINE_CMP_VV(cel_uint_lt_at_vv, CEL_UINT, u, <)
DEFINE_CMP_VV(cel_uint_le_at_vv, CEL_UINT, u, <=)
DEFINE_CMP_VV(cel_uint_gt_at_vv, CEL_UINT, u, >)
DEFINE_CMP_VV(cel_uint_ge_at_vv, CEL_UINT, u, >=)

// Double: `==` / `!=` follow IEEE 754 (NaN != NaN, NaN == NaN
// false).  C's `==` and `!=` operators implement this directly.
// Ordering operators (<, <=, >, >=) likewise return false for any
// NaN-bearing comparison per IEEE.
DEFINE_CMP_VV(cel_double_lt_at_vv, CEL_DOUBLE, d, <)
DEFINE_CMP_VV(cel_double_le_at_vv, CEL_DOUBLE, d, <=)
DEFINE_CMP_VV(cel_double_gt_at_vv, CEL_DOUBLE, d, >)
DEFINE_CMP_VV(cel_double_ge_at_vv, CEL_DOUBLE, d, >=)

DEFINE_CMP_VV(cel_bool_eq_at_vv, CEL_BOOL, b, ==)
// Bool ordering — `false < true` per langdef §"Booleans".  Since
// `payload.b` is normalised to 0/1 by `write_bool` and
// `cel_make_bool`, the integer relational operators give the
// langdef order directly.
DEFINE_CMP_VV(cel_bool_lt_at_vv, CEL_BOOL, b, <)
DEFINE_CMP_VV(cel_bool_le_at_vv, CEL_BOOL, b, <=)
DEFINE_CMP_VV(cel_bool_gt_at_vv, CEL_BOOL, b, >)
DEFINE_CMP_VV(cel_bool_ge_at_vv, CEL_BOOL, b, >=)

#undef DEFINE_CMP_VV

// ─────────────────────────────────────────────────────────────
// Cross-type numeric comparison ladder.  See
// `rewrite/cross-numeric-ordering-plan.md`.
//
// Each helper accepts any combination of {CEL_INT, CEL_UINT,
// CEL_DOUBLE} on either operand.  The shared `numeric_compare_kernel`
// returns a tri-state result {kLess, kEqual, kGreater, kNanInequal}
// mirroring cel-cpp's `internal/number.h::ComparisonResult`.  Each
// op then collapses that tri-state to a CEL_BOOL.
//
// Boundary handling (cel-cpp parity, `internal/number.h:25-44 /
// 95-165`):
//   - int vs uint: negative int is always < any uint; otherwise
//     compare as uint64.
//   - int vs double: if double > kInt64Max → double > int; if
//     double < kInt64Min → double < int; otherwise IEEE-compare
//     (double)int vs double.
//   - uint vs double: if double > kUint64Max → double > uint; if
//     double < 0 → double < uint; otherwise IEEE-compare
//     (double)uint vs double.
//   - NaN: any comparison involving NaN returns kNanInequal so all
//     six op wrappers answer false (matches IEEE / langdef "NaN
//     compares unequal in every direction").
//
// Wasm32 freestanding constraint: this code MUST avoid `__multi3` /
// other compiler-rt 128-bit intrinsics (mirrors the
// `int64_mul_overflows` precedent in same-kind arithmetic).  Only operations used here are
// 64-bit comparisons + a single int↔uint cast that the wasm32 backend
// lowers natively as `i64.lt_s` / `i64.lt_u` / `f64.lt`.  The
// `noinline` attribute on the kernel keeps clang from re-deriving a
// 128-bit fold across the leaf wrappers.
// ─────────────────────────────────────────────────────────────

static CmpResult cmp_i64(int64_t a, int64_t b) {
  if (a < b) return kCmpLess;
  if (a > b) return kCmpGreater;
  return kCmpEqual;
}

static CmpResult cmp_u64(uint64_t a, uint64_t b) {
  if (a < b) return kCmpLess;
  if (a > b) return kCmpGreater;
  return kCmpEqual;
}

static CmpResult cmp_double(double a, double b) {
  if (a != a || b != b) return kCmpNanInequal;
  if (a < b) return kCmpLess;
  if (a > b) return kCmpGreater;
  return kCmpEqual;
}

static CmpResult cmp_int_vs_uint(int64_t a, uint64_t b) {
  if (a < 0) return kCmpLess;
  return cmp_u64((uint64_t)a, b);
}

static CmpResult cmp_int_vs_double(int64_t a, double b) {
  if (b != b) return kCmpNanInequal;
  if (b > (double)INT64_MAX) return kCmpLess;
  if (b < (double)INT64_MIN) return kCmpGreater;
  return cmp_double((double)a, b);
}

static CmpResult cmp_uint_vs_double(uint64_t a, double b) {
  if (b != b) return kCmpNanInequal;
  if (b > (double)UINT64_MAX) return kCmpLess;
  if (b < 0.0) return kCmpGreater;
  return cmp_double((double)a, b);
}

static CmpResult cmp_flip(CmpResult r) {
  if (r == kCmpLess) return kCmpGreater;
  if (r == kCmpGreater) return kCmpLess;
  return r;
}

static uint32_t numeric_kind_pair(uint32_t a_kind, uint32_t b_kind) {
  return (a_kind << 8) | b_kind;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
__attribute__((noinline)) CmpResult numeric_compare_kernel(const CelValue* a,
                                                           const CelValue* b) {
  switch (numeric_kind_pair(a->kind, b->kind)) {
    case (CEL_INT << 8) | CEL_INT:
      return cmp_i64(a->payload.i, b->payload.i);
    case (CEL_UINT << 8) | CEL_UINT:
      return cmp_u64(a->payload.u, b->payload.u);
    case (CEL_DOUBLE << 8) | CEL_DOUBLE:
      return cmp_double(a->payload.d, b->payload.d);
    case (CEL_INT << 8) | CEL_UINT:
      return cmp_int_vs_uint(a->payload.i, b->payload.u);
    case (CEL_UINT << 8) | CEL_INT:
      return cmp_flip(cmp_int_vs_uint(b->payload.i, a->payload.u));
    case (CEL_INT << 8) | CEL_DOUBLE:
      return cmp_int_vs_double(a->payload.i, b->payload.d);
    case (CEL_DOUBLE << 8) | CEL_INT:
      return cmp_flip(cmp_int_vs_double(b->payload.i, a->payload.d));
    case (CEL_UINT << 8) | CEL_DOUBLE:
      return cmp_uint_vs_double(a->payload.u, b->payload.d);
    case (CEL_DOUBLE << 8) | CEL_UINT:
      return cmp_flip(cmp_uint_vs_double(b->payload.u, a->payload.d));
    default:
      // Caller (`numeric_prelude`) already filters non-numeric kinds;
      // a non-numeric pair reaching the kernel is an invariant
      // violation.  Return kNanInequal so all six op wrappers answer
      // false rather than miscompiling silently.
      return kCmpNanInequal;
  }
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
int is_numeric_kind(uint32_t kind) {
  return kind == CEL_INT || kind == CEL_UINT || kind == CEL_DOUBLE;
}

// Shared prelude for the cross-type numeric helpers: 3VL absorption
// then a numeric-kind check on each operand (any non-numeric → type
// mismatch).  Returns 1 when out_slot has been written and the
// caller should skip the kernel.
static int numeric_prelude(CelValue* out, const CelValue* a,
                           const CelValue* b) {
  if (absorb_3vl_binary(out, a, b)) return 1;
  if (!is_numeric_kind(a->kind) || !is_numeric_kind(b->kind)) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  return 0;
}

void cel_numeric_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpEqual);
}

void cel_numeric_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpLess);
}

void cel_numeric_le_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpLess || r == kCmpEqual);
}

void cel_numeric_gt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpGreater);
}

void cel_numeric_ge_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpGreater || r == kCmpEqual);
}

// ─────────────────────────────────────────────────────────────
// Map-KEY equality — LOSSLESS, not rounding.
//
// The ladder above puts int / uint / double on one continuous number
// line and tolerates the precision loss of the int→double cast, which
// is what the `==` operator, list membership, and element compares
// want (cel-cpp `internal/number.h`, `Number::operator==`).
//
// Map-key lookup wants the opposite.  cel-cpp converts the query key
// to the stored key's type only when the conversion ROUND-TRIPS, then
// compares exactly — `internal/number.h`
// `LosslessConvertibleToIntVisitor` / `LosslessConvertibleToUintVisitor`,
// driven from `eval/eval/container_access_step.cc::LookupInMap`,
// `runtime/standard/container_membership_functions.cc`'s
// `doubleKeyInSet` / `intKeyInSet` / `uintKeyInSet`, and
// `runtime/standard/equality_functions.cc::CheckAlternativeNumericType`.
//
// The rules diverge above 2^53, where one double is the rounded image
// of a RANGE of int64s.  Oracle-pinned in
// `testdata/cel_cpp_oracle_test.cc` (`MapKeyNumericCrossType`):
//   `dyn(9007199254740993) == 9007199254740992.0`        -> true
//   `dyn(9007199254740992.0) in {9007199254740993: 'a'}` -> false
//
// The SwissTable index (`cel_map_hash.h`) canonicalizes a double key
// to the integer it EXACTLY represents, so it already agrees with this
// predicate: keys this function calls equal always hash identically,
// and no lookup needs to bypass the index.
// ─────────────────────────────────────────────────────────────

// Round-trip windows, verbatim from cel-cpp `internal/number.h:35-45`:
// `RoundingError<T>() == 1 << (digits<T> - digits<double> - 1)` — 512
// for int64 (63-53-1), 1024 for uint64 (64-53-1) — and the bound is the
// double image of `max - RoundingError`.  Concretely these evaluate to
// 2^63-1024 and 2^64-2048, the largest doubles whose integer cast is
// still in range: `(double)INT64_MAX` is 2^63 and `(double)UINT64_MAX`
// is 2^64, both one past the end, which is exactly why the naive
// rounding compare called `UINT64_MAX` equal to `2^64.0`.
static const double kDoubleToIntMin = (double)INT64_MIN;
static const double kMaxDoubleAsInt = (double)(INT64_MAX - 512);
static const double kMaxDoubleAsUint = (double)(UINT64_MAX - 1024);

// `d` → int64 iff exact.  The bounds are written as `!(d >= lo)` /
// `!(d <= hi)` so NaN (false on every comparison) is rejected by both,
// and ±Inf by one; the cast round-trip then rejects any non-integral
// value.  Mirrors `LosslessConvertibleToIntVisitor`.
static int double_as_int(double d, int64_t* out) {
  if (!(d >= kDoubleToIntMin) || !(d <= kMaxDoubleAsInt)) return 0;
  const int64_t i = (int64_t)d;
  if ((double)i != d) return 0;
  *out = i;
  return 1;
}

// `d` → uint64 iff exact.  Mirrors `LosslessConvertibleToUintVisitor`.
static int double_as_uint(double d, uint64_t* out) {
  if (!(d >= 0.0) || !(d <= kMaxDoubleAsUint)) return 0;
  const uint64_t u = (uint64_t)d;
  if ((double)u != d) return 0;
  *out = u;
  return 1;
}

// `d` equals the integer-kinded key `i` (CEL_INT or CEL_UINT) iff `d`
// converts losslessly to that kind AND the converted value matches.
static int double_matches_integer(double d, const CelValue* i) {
  if (i->kind == CEL_INT) {
    int64_t v = 0;
    return double_as_int(d, &v) && v == i->payload.i;
  }
  uint64_t v = 0;
  return double_as_uint(d, &v) && v == i->payload.u;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
int cel_numeric_key_eq(const CelValue* a, const CelValue* b) {
  if (!is_numeric_kind(a->kind) || !is_numeric_kind(b->kind)) return 0;
  if (a->kind == CEL_DOUBLE && b->kind == CEL_DOUBLE) {
    return a->payload.d == b->payload.d;  // IEEE: NaN never equal.
  }
  if (a->kind == CEL_DOUBLE) return double_matches_integer(a->payload.d, b);
  if (b->kind == CEL_DOUBLE) return double_matches_integer(b->payload.d, a);
  // int / uint: no double is involved, and `numeric_compare_kernel`
  // already compares that pair exactly (`cmp_int_vs_uint` widens the
  // non-negative int to uint64), so its verdict IS the lossless one.
  return numeric_compare_kernel(a, b) == kCmpEqual;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
int cel_map_key_eq(const CelValue* a, const CelValue* b) {
  if (is_numeric_kind(a->kind) && is_numeric_kind(b->kind)) {
    return cel_numeric_key_eq(a, b);
  }
  // bool / string / bytes / null / duration / timestamp compare
  // structurally in `cel_value_eq`, which is exact for all of them.
  return cel_value_eq(a, b);
}

void cel_null_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_NULL)) return;
  write_bool(out, 1);  // null == null is always true.
}
