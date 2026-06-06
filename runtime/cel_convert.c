// Conversion kernels — numeric inter-conversion, string parsing,
// number/bool → string formatting, bytes ↔ string.
//
// Carved out of cel_runtime.c per
// `rewrite/cel-runtime-c-split-plan.md`.  See cel_convert.h for the
// public ABI and `rewrite/m10-conversions.md` for the design
// rationale.

#include "runtime/cel_convert.h"

#include <stdint.h>

#include "runtime/cel_arena.h"
#include "runtime/cel_internal.h"
#include "runtime/cel_log.h"

// ─────────────────────────────────────────────────────────────
// Numeric inter-conversion kernels.
//
// Six unary helpers for the cel-cpp overload ids:
//   uint64_to_int64    int(uint)
//   double_to_int64    int(double)
//   int64_to_uint64    uint(int)
//   double_to_uint64   uint(double)
//   int64_to_double    double(int)
//   uint64_to_double   double(uint)
//
// Overflow / NaN / negative-source rejections poison out_slot with
// `CEL_ERR_OVERFLOW` per langdef §"int" / §"uint" / §"double" and
// cel-cpp's `Checked*ToInt64` / `Checked*ToUint64` helpers
// (`third_party/cel-cpp/internal/overflow.cc`).
//
// Double bounds for int / uint use the exact-representable
// boundaries 2^63 and 2^64.  Per the conformance corpus
// (conversions.textproto: int(-9223372036854775808.0) -> "range" error),
// the double `-2^63` is REJECTED — and since no double lies strictly
// between `-2^63` and the next representable value, `INT64_MIN` is simply
// unreachable via int(double).  `INT64_MAX + 1 == 2^63` is likewise
// rejected; the largest admissible double is `2^63 - 1024`.
// NaN is rejected via the `is_nan` helper (the `v != v` idiom — works
// without <math.h>, which the freestanding wasm32 build does not link).
// ─────────────────────────────────────────────────────────────

// Exact-representable double bounds.  `kDoubleInt64Min == -2^63`
// exactly; `kDoubleInt64MaxPlus1 == 2^63` exactly.  Compare with
// `<` / `>=` to admit the inclusive int64 range and reject the
// out-of-range edge cleanly.
static const double kDoubleInt64Min = -9223372036854775808.0;
static const double kDoubleInt64MaxPlus1 = 9223372036854775808.0;
static const double kDoubleUint64MaxPlus1 = 18446744073709551616.0;

// True iff `v` is NaN.  IEEE-754 makes NaN the only value that compares
// unequal to itself, so `v != v` is the canonical isnan() — used here
// because the freestanding wasm32 runtime build links no <math.h> (and
// thus has no isnan()).  Named so call sites read as intent, not a typo.
static inline int is_nan(double v) {
  return v != v;
}

void cel_uint_to_int_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_UINT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const uint64_t v = a->payload.u;
  if (v > (uint64_t)INT64_MAX) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, (int64_t)v);
}

void cel_double_to_int_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const double v = a->payload.d;
  // NaN check (is_nan) + range gate using exact-representable
  // boundaries.  Rejects [-inf, -2^63], [2^63, +inf], and NaN.  Per the
  // conformance corpus (conversions.textproto: int(-9223372036854775808.0)
  // -> "range" error), the double -2^63 itself is REJECTED — there is no
  // double strictly between -2^63 and the next representable value, so
  // INT64_MIN is simply unreachable via int(double).
  if (is_nan(v) || v <= kDoubleInt64Min || v >= kDoubleInt64MaxPlus1) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  // C99 cast truncates toward zero — matches langdef §"int"
  // "rounds toward zero".
  write_int(out, (int64_t)v);
}

void cel_int_to_uint_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const int64_t v = a->payload.i;
  if (v < 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, (uint64_t)v);
}

void cel_double_to_uint_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const double v = a->payload.d;
  if (is_nan(v) || v < 0.0 || v >= kDoubleUint64MaxPlus1) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, (uint64_t)v);
}

void cel_int_to_double_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Never errors per langdef — lossy for |v| >= 2^53 is allowed.
  write_double(out, (double)a->payload.i);
}

void cel_uint_to_double_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_UINT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Never errors per langdef — lossy for v >= 2^53 is allowed.
  write_double(out, (double)a->payload.u);
}

// ─────────────────────────────────────────────────────────────
// M10.C: string parsing helpers.
//
// Four unary helpers `(out_slot, in_slot) -> void`:
//   string_to_int64    int(string)
//   string_to_uint64   uint(string)
//   string_to_double   double(string)
//   string_to_bool     bool(string)
//
// Hand-rolled byte-loop parsers mirroring `absl::SimpleAtoi` /
// `SimpleAtod` admit-sets (the cel-cpp reference impl).  All parser
// subroutines return 1 on success and 0 on any malformed input —
// kernels translate 0 to `CEL_ERR_OVERFLOW`.
// ─────────────────────────────────────────────────────────────

static const uint8_t* span_bytes(const CelValue* cv) {
  return cel_memory_base_() + cv->payload.s.ptr;
}

// Accumulate decimal digits in [start, len) into *acc with overflow
// detection.  Returns 1 on success, 0 on malformed digit or overflow.
// Manual overflow check — `__builtin_mul_overflow` on 64-bit needs
// `__multi3` which the freestanding wasm32 build does not link
// (same precedent as cel_runtime.c's int64_mul_overflows).
static int accumulate_u64_decimal(const uint8_t* p, uint32_t start,
                                  uint32_t len, uint64_t* acc) {
  for (uint32_t i = start; i < len; ++i) {
    if (p[i] < '0' || p[i] > '9') return 0;
    uint32_t d = (uint32_t)(p[i] - '0');
    if (*acc > UINT64_MAX / 10ULL) return 0;
    *acc *= 10ULL;
    if (*acc > UINT64_MAX - (uint64_t)d) return 0;
    *acc += (uint64_t)d;
  }
  return 1;
}

// Sign-apply a non-negative u64 accumulator to an int64.  Returns 1
// on success, 0 on out-of-range.  INT64_MIN is the canonical
// `|INT64_MIN| == INT64_MAX + 1` edge.
static int apply_int64_sign(uint64_t acc, int neg, int64_t* out) {
  if (neg) {
    if (acc > (uint64_t)INT64_MAX + 1ULL) return 0;
    if (acc == (uint64_t)INT64_MAX + 1ULL) {
      *out = INT64_MIN;
    } else {
      *out = -(int64_t)acc;
    }
  } else {
    if (acc > (uint64_t)INT64_MAX) return 0;
    *out = (int64_t)acc;
  }
  return 1;
}

static int parse_int64_str(const uint8_t* p, uint32_t len, int64_t* out) {
  if (len == 0) return 0;
  int neg = 0;
  uint32_t i = 0;
  if (p[i] == '-') {
    neg = 1;
    ++i;
  } else if (p[i] == '+') {
    // cel-cpp's absl::SimpleAtoi accepts a leading '+'.
    ++i;
  }
  if (i == len) return 0;
  uint64_t acc = 0;
  if (!accumulate_u64_decimal(p, i, len, &acc)) return 0;
  return apply_int64_sign(acc, neg, out);
}

static int parse_uint64_str(const uint8_t* p, uint32_t len, uint64_t* out) {
  if (len == 0) return 0;
  uint32_t i = 0;
  if (p[i] == '+') {
    // cel-cpp's absl::SimpleAtoi accepts a leading '+' on unsigned too.
    ++i;
  }
  if (i == len) return 0;
  uint64_t acc = 0;
  if (!accumulate_u64_decimal(p, i, len, &acc)) return 0;
  *out = acc;
  return 1;
}

// Case-insensitive ASCII prefix match.  Returns 1 iff `p[0..plen)`
// matches `pattern[0..plen)` byte-for-byte after folding A-Z to a-z.
static int eq_ci(const uint8_t* p, uint32_t plen, const char* pattern) {
  for (uint32_t i = 0; i < plen; ++i) {
    uint8_t a = p[i];
    uint8_t b = (uint8_t)pattern[i];
    if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (uint8_t)(b + 32);
    if (a != b) return 0;
  }
  return 1;
}

// Read optional sign + check for inf/infinity/nan special literals.
// Returns 1 if a special was consumed (sets *out); 0 otherwise (does
// not advance state).
static int try_parse_double_special(const uint8_t* p, uint32_t len, int neg,
                                    double* out) {
  if (len == 8 && eq_ci(p, 8, "infinity")) {
    *out = neg ? -__builtin_inf() : __builtin_inf();
    return 1;
  }
  if (len == 3 && eq_ci(p, 3, "inf")) {
    *out = neg ? -__builtin_inf() : __builtin_inf();
    return 1;
  }
  if (len == 3 && eq_ci(p, 3, "nan")) {
    *out = __builtin_nan("");
    return 1;
  }
  return 0;
}

// Read decimal digits starting at *i; updates *i past the run and
// accumulates into *mantissa.  Returns 1 if at least one digit was
// consumed, 0 otherwise.
static int scan_decimal_digits(const uint8_t* p, uint32_t len, uint32_t* i,
                               double* mantissa) {
  int saw = 0;
  while (*i < len && p[*i] >= '0' && p[*i] <= '9') {
    *mantissa = (*mantissa * 10.0) + (double)(p[*i] - '0');
    saw = 1;
    ++*i;
  }
  return saw;
}

// Parse the optional `eXX` exponent suffix.  On entry *i points at
// `e` / `E` (or past everything).  Returns 1 on success (or no
// exponent present), 0 on malformed exponent.  Writes the signed
// exponent into *exp_out (0 if absent).
static int scan_exponent(const uint8_t* p, uint32_t len, uint32_t* i,
                         int* exp_out) {
  *exp_out = 0;
  if (*i >= len || (p[*i] != 'e' && p[*i] != 'E')) return 1;
  ++*i;
  if (*i == len) return 0;
  int exp_neg = 0;
  if (p[*i] == '-') {
    exp_neg = 1;
    ++*i;
  } else if (p[*i] == '+') {
    ++*i;
  }
  if (*i == len) return 0;
  int exp_val = 0;
  int saw = 0;
  while (*i < len && p[*i] >= '0' && p[*i] <= '9') {
    // Cap to avoid wraparound — magnitudes past +/-308 saturate to
    // inf / 0 anyway under IEEE 754 doubles.
    if (exp_val < 10000) exp_val = (exp_val * 10) + (p[*i] - '0');
    saw = 1;
    ++*i;
  }
  if (!saw) return 0;
  *exp_out = exp_neg ? -exp_val : exp_val;
  return 1;
}

// Apply the integer power-of-10 scale to mantissa.  Iterative —
// precision-lossy on edge cases but matches cel-cpp's SimpleAtod
// for the common admit-set; a tighter implementation (Grisu / Ryu)
// can land later.
//
// Precision note: the naive `mantissa /= 10` (or `mantissa *= 10`)
// chain rounds at each step but compounds favourably for inputs like
// "5.43e-21" where strtod's correctly-rounded answer happens to fall
// on the chain's path.  Building the divisor in a single pow10 pass
// and dividing once is mathematically cleaner but breaks the chain
// for |total_exp| > 22 because 10^N is no longer exactly
// representable past N=22.  For |total_exp| up to 22, single-pass
// dividing gives the strtod-canonical answer ("123.456" round-trips
// exactly); past 22 the original step-by-step chain wins because the
// intermediate rounds cancel rather than accumulate.  Routing on the
// magnitude picks the better algorithm for each range.
static double apply_decimal_scale(double mantissa, int total_exp) {
  if (total_exp == 0) return mantissa;
  int k = total_exp > 0 ? total_exp : -total_exp;
  if (k <= 22) {
    double pow10 = 1.0;
    for (int j = 0; j < k; ++j) {
      pow10 *= 10.0;
    }
    return total_exp > 0 ? mantissa * pow10 : mantissa / pow10;
  }
  // Step-by-step chain for |total_exp| > 22.  Bypassing the
  // single-pass form here matches the cel-cpp / strtod result on the
  // exp_neg_neg corpus (`-5.43e-21` → total_exp=-23, which is the
  // canonical boundary case).
  for (int j = 0; j < k; ++j) {
    mantissa = total_exp > 0 ? mantissa * 10.0 : mantissa / 10.0;
  }
  return mantissa;
}

// Consume the optional leading sign byte; updates *i and sets *neg.
static void scan_leading_sign(const uint8_t* p, uint32_t len, uint32_t* i,
                              int* neg) {
  *neg = 0;
  if (*i >= len) return;
  if (p[*i] == '-') {
    *neg = 1;
    ++*i;
  } else if (p[*i] == '+') {
    ++*i;
  }
}

// Consume the integer + optional fractional digits.  Returns 1 if at
// least one digit was seen; writes the accumulated mantissa and the
// number of fractional digits.
static int scan_mantissa(const uint8_t* p, uint32_t len, uint32_t* i,
                         double* mantissa, int* frac_digits) {
  int saw = scan_decimal_digits(p, len, i, mantissa);
  *frac_digits = 0;
  if (*i < len && p[*i] == '.') {
    ++*i;
    uint32_t before = *i;
    if (scan_decimal_digits(p, len, i, mantissa)) saw = 1;
    *frac_digits = (int)(*i - before);
  }
  return saw;
}

static int parse_double_str(const uint8_t* p, uint32_t len, double* out) {
  if (len == 0) return 0;
  uint32_t i = 0;
  int neg = 0;
  scan_leading_sign(p, len, &i, &neg);
  if (i == len) return 0;
  if (try_parse_double_special(p + i, len - i, neg, out)) return 1;
  double mantissa = 0.0;
  int frac_digits = 0;
  if (!scan_mantissa(p, len, &i, &mantissa, &frac_digits)) return 0;
  int signed_exp = 0;
  if (!scan_exponent(p, len, &i, &signed_exp)) return 0;
  if (i != len) return 0;  // trailing garbage rejected.
  mantissa = apply_decimal_scale(mantissa, signed_exp - frac_digits);
  *out = neg ? -mantissa : mantissa;
  return 1;
}

// cel-cpp's `StringToBoolFunction` truth table (10 rows).
//   true:  "1" / "t" / "true" / "TRUE" / "True"
//   false: "0" / "f" / "false" / "FALSE" / "False"
// Exact byte-match (NOT case-insensitive — the spec admits only
// the 5 spellings per polarity, mixed case beyond `True` / `TRUE`
// rejects).
static int parse_bool_str(const uint8_t* p, uint32_t len, int* out) {
  struct Row {
    const char* s;
    uint32_t len;
    int v;
  };
  static const struct Row kRows[10] = {
      {"1", 1, 1},     {"t", 1, 1},     {"true", 4, 1}, {"TRUE", 4, 1},
      {"True", 4, 1},  {"0", 1, 0},     {"f", 1, 0},    {"false", 5, 0},
      {"FALSE", 5, 0}, {"False", 5, 0},
  };
  for (uint32_t r = 0; r < 10; ++r) {
    if (len != kRows[r].len) continue;
    int match = 1;
    for (uint32_t i = 0; i < len; ++i) {
      if (p[i] != (uint8_t)kRows[r].s[i]) {
        match = 0;
        break;
      }
    }
    if (match) {
      *out = kRows[r].v;
      return 1;
    }
  }
  return 0;
}

void cel_string_to_int_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  int64_t v;
  if (!parse_int64_str(span_bytes(a), a->payload.s.len, &v)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, v);
}

void cel_string_to_uint_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  uint64_t v;
  if (!parse_uint64_str(span_bytes(a), a->payload.s.len, &v)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, v);
}

void cel_string_to_double_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  double v;
  if (!parse_double_str(span_bytes(a), a->payload.s.len, &v)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_double(out, v);
}

void cel_string_to_bool_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  int v;
  if (!parse_bool_str(span_bytes(a), a->payload.s.len, &v)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_bool(out, v);
}

// ─────────────────────────────────────────────────────────────
// M10.D: number / bool → string formatting helpers.
//
// Four unary kernels.  Output strings allocated in the per-Eval arena
// via `arena_alloc(n)` and stamped as `{CEL_STRING, payload.s}` — same
// lifetime model as the `cel_type_of_at_v` helper.
// ─────────────────────────────────────────────────────────────

// Write decimal digits of a uint64 into `dst`, returning the count.
// No leading zeros (except for value 0 itself).
static uint32_t write_uint_decimal(uint8_t* dst, uint64_t v) {
  if (v == 0) {
    dst[0] = '0';
    return 1;
  }
  uint8_t buf[20];  // ceil(log10(UINT64_MAX)) = 20
  uint32_t n = 0;
  while (v > 0) {
    buf[n++] = (uint8_t)('0' + (v % 10ULL));
    v /= 10ULL;
  }
  for (uint32_t i = 0; i < n; ++i) {
    dst[i] = buf[n - 1 - i];
  }
  return n;
}

// Write decimal digits of an int64 into `dst`, with leading `-` for
// negatives.  Handles INT64_MIN by promoting through the |v| route.
static uint32_t write_int_decimal(uint8_t* dst, int64_t v) {
  if (v >= 0) {
    return write_uint_decimal(dst, (uint64_t)v);
  }
  dst[0] = '-';
  uint64_t abs_v =
      (v == INT64_MIN) ? ((uint64_t)INT64_MAX + 1ULL) : (uint64_t)(-v);
  return 1u + write_uint_decimal(dst + 1, abs_v);
}

// Common allocate-and-stamp for the small string outputs.  Returns
// 1 on success; on arena OOM poisons out_slot and returns 0.
static int stamp_string(CelValue* out, const uint8_t* src, uint32_t len) {
  uint32_t off = arena_alloc(len);
  if (off == 0 && len > 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return 0;
  }
  uint8_t* dst = cel_memory_base_() + off;
  for (uint32_t i = 0; i < len; ++i) {
    dst[i] = src[i];
  }
  out->kind = CEL_STRING;
  out->_pad = 0;
  out->payload.s.ptr = off;
  out->payload.s.len = len;
  return 1;
}

void cel_int_to_string_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  uint8_t buf[21];  // 20 digits + sign
  uint32_t n = write_int_decimal(buf, a->payload.i);
  (void)stamp_string(out, buf, n);
}

void cel_uint_to_string_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_UINT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  uint8_t buf[20];
  uint32_t n = write_uint_decimal(buf, a->payload.u);
  (void)stamp_string(out, buf, n);
}

void cel_bool_to_string_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_BOOL) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  static const uint8_t kTrue[4] = {'t', 'r', 'u', 'e'};
  static const uint8_t kFalse[5] = {'f', 'a', 'l', 's', 'e'};
  if (a->payload.b) {
    (void)stamp_string(out, kTrue, 4);
  } else {
    (void)stamp_string(out, kFalse, 5);
  }
}

// `cel_double_to_string_at_v` lives in a sibling C++ TU
// (`cel_convert_double_format.cc`) so it can use `std::to_chars` for
// shortest-round-trip formatting.  The hand-rolled C path that used
// to live here accumulated rounding error past ~6 fractional digits
// (e.g. `string(123.456)` → `"123.45600000000000306"`); the rewrite
// mirrors cel-cpp's `FormatDouble`
// (`runtime/standard/type_conversion_functions.cc:56`).

// ─────────────────────────────────────────────────────────────
// M10.E: bytes ↔ string interconversion.
//
// Both helpers share the source's `payload.s` (no arena copy) — safe
// because the underlying bytes are immutable for the lifetime of the
// CelValue.  Aliased slots (`out_slot == in_slot`) are handled by
// reading the span into a local before writing.
// ─────────────────────────────────────────────────────────────

// RFC3629 UTF-8 byte-wise validator.  See
// `rewrite/m10-conversions.md` §4.5 for
// the reject matrix.  Helpers below cover one byte-length class each
// so the top-level loop stays within the function-size gate.

static int utf8_two_byte_valid(const uint8_t* p, uint32_t* i, uint32_t len) {
  if (*i + 1 >= len) return 0;
  if ((p[*i + 1] & 0xC0) != 0x80) return 0;
  *i += 2;
  return 1;
}

static int utf8_three_byte_valid(const uint8_t* p, uint32_t* i, uint32_t len) {
  if (*i + 2 >= len) return 0;
  uint8_t b = p[*i];
  uint8_t b1 = p[*i + 1];
  uint8_t b2 = p[*i + 2];
  if ((b1 & 0xC0) != 0x80) return 0;
  if ((b2 & 0xC0) != 0x80) return 0;
  if (b == 0xE0 && b1 < 0xA0) return 0;   // overlong
  if (b == 0xED && b1 >= 0xA0) return 0;  // surrogate
  *i += 3;
  return 1;
}

static int utf8_four_byte_valid(const uint8_t* p, uint32_t* i, uint32_t len) {
  if (*i + 3 >= len) return 0;
  uint8_t b = p[*i];
  uint8_t b1 = p[*i + 1];
  uint8_t b2 = p[*i + 2];
  uint8_t b3 = p[*i + 3];
  if ((b1 & 0xC0) != 0x80) return 0;
  if ((b2 & 0xC0) != 0x80) return 0;
  if ((b3 & 0xC0) != 0x80) return 0;
  if (b == 0xF0 && b1 < 0x90) return 0;   // overlong
  if (b == 0xF4 && b1 >= 0x90) return 0;  // > U+10FFFF
  *i += 4;
  return 1;
}

static int utf8_valid(const uint8_t* p, uint32_t len) {
  uint32_t i = 0;
  while (i < len) {
    uint8_t b = p[i];
    if (b < 0x80) {
      ++i;
      continue;
    }
    if (b < 0xC2) return 0;  // orphan continuation OR overlong 2-byte
    if (b < 0xE0) {
      if (!utf8_two_byte_valid(p, &i, len)) return 0;
      continue;
    }
    if (b < 0xF0) {
      if (!utf8_three_byte_valid(p, &i, len)) return 0;
      continue;
    }
    if (b < 0xF5) {
      if (!utf8_four_byte_valid(p, &i, len)) return 0;
      continue;
    }
    return 0;  // 0xF5-0xFF: invalid leading byte
  }
  return 1;
}

void cel_string_to_bytes_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const CelSpan span = a->payload.s;  // read first (alias-safe).
  out->kind = CEL_BYTES;
  out->_pad = 0;
  out->payload.s = span;
}

void cel_bytes_to_string_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_BYTES) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const CelSpan span = a->payload.s;
  if (!utf8_valid(cel_memory_base_() + span.ptr, span.len)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  out->kind = CEL_STRING;
  out->_pad = 0;
  out->payload.s = span;
}
