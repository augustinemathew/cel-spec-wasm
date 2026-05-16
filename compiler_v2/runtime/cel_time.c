// Timestamp / Duration kernels — see cel_time.h for the public ABI.

#include "compiler_v2/runtime/cel_time.h"

#include <stdint.h>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_internal.h"

// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/time_functions.cc::
//     {add,subtract}_duration_duration / _time_duration /
//     _time_time + less / greater / etc.
// Pure-int kernels here; the parse / format / int conversions live
// behind host trampolines (M7B.D) and the TZ-aware accessor lives
// behind a single dispatch trampoline (M7B.E).

#define NANOS_PER_SEC 1000000000

// langdef §"Timestamps and Durations": timestamp seconds must
// represent a UTC instant in [0001-01-01T00:00:00Z, 9999-12-31T23:59:59Z].
// cel-cpp surfaces out-of-range timestamps as CEL_ERR_OVERFLOW (the
// same error code as int64 overflow on the seconds field), so we
// match that here.  Bounds: year 1 epoch = -62135596800; year 9999
// last second = 253402300799 (per the langdef and cel-cpp's
// internal/time.cc range checks).
#define TIMESTAMP_MIN_SECONDS (-62135596800LL)
#define TIMESTAMP_MAX_SECONDS (253402300799LL)

static inline int timestamp_in_range(int64_t seconds) {
  return seconds >= TIMESTAMP_MIN_SECONDS && seconds <= TIMESTAMP_MAX_SECONDS;
}

// ----- shared helpers (file-local) ----------------------------------------

// require_kinds_2 — same-kind guard for non-uniform operand pairs.
// `require_kinds` in cel_internal.h takes a single `want` for both
// operands; here we need (CEL_TIMESTAMP, CEL_DURATION) and friends.
static inline int require_kinds_2(CelValue* out, const CelValue* a,
                                  const CelValue* b, uint32_t want_a,
                                  uint32_t want_b) {
  if (a->kind != want_a || b->kind != want_b) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  return 0;
}

// Combine two CelDurTs payloads as (s1 ± s2, n1 ± n2) with nanos-
// carry and sign-correlated normalisation.  Returns non-zero on
// int64-seconds overflow (caller poisons out as CEL_ERR_OVERFLOW).
//
// Algorithm (m7b §3.2):
//   1. raw_s = a.s ± b.s            (overflow-checked).
//   2. raw_n = a.n ± b.n            (sum in (-2e9, 2e9) — fits in
//                                    int32 because |a.n|, |b.n| < 1e9).
//   3. Carry the |nanos| ≥ 1e9 case into seconds with ±1
//      (overflow-checked at the seconds add).
//   4. Sign-correlate.  If seconds > 0 and nanos < 0, subtract 1
//      from seconds and add 1e9 to nanos (and symmetric).  These
//      adjustments always move seconds toward zero; no overflow.
static int dur_combine(const CelDurTs* a, const CelDurTs* b, int subtract,
                       CelDurTs* result) {
  int64_t raw_s;
  if (subtract) {
    if (__builtin_sub_overflow(a->seconds, b->seconds, &raw_s)) return 1;
  } else {
    if (__builtin_add_overflow(a->seconds, b->seconds, &raw_s)) return 1;
  }
  int32_t raw_n = subtract ? (a->nanos - b->nanos) : (a->nanos + b->nanos);
  int64_t carry = 0;
  if (raw_n >= NANOS_PER_SEC) {
    raw_n -= NANOS_PER_SEC;
    carry = 1;
  } else if (raw_n <= -NANOS_PER_SEC) {
    raw_n += NANOS_PER_SEC;
    carry = -1;
  }
  if (carry != 0 && __builtin_add_overflow(raw_s, carry, &raw_s)) return 1;
  if (raw_s > 0 && raw_n < 0) {
    raw_s -= 1;
    raw_n += NANOS_PER_SEC;
  } else if (raw_s < 0 && raw_n > 0) {
    raw_s += 1;
    raw_n -= NANOS_PER_SEC;
  }
  result->seconds = raw_s;
  result->nanos = raw_n;
  result->_pad = 0;
  return 0;
}

// Lexicographic (seconds, nanos) compare.  Returns -1 / 0 / +1.
static int dur_compare_lex(const CelDurTs* a, const CelDurTs* b) {
  if (a->seconds != b->seconds) return a->seconds < b->seconds ? -1 : 1;
  if (a->nanos != b->nanos) return a->nanos < b->nanos ? -1 : 1;
  return 0;
}

// One-shot arithmetic dispatch.  Each helper is a thin wrapper that
// supplies the (operand-kind, operand-kind, sub-flag, result-kind)
// 4-tuple and delegates to `run_arith`.  Packed in a struct so the
// param count stays under the lint gate.
typedef struct {
  uint32_t want_a;
  uint32_t want_b;
  int subtract;
  uint32_t result_kind;
} ArithSpec;

static void run_arith(CelValue* out, const CelValue* a, const CelValue* b,
                      ArithSpec spec) {
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds_2(out, a, b, spec.want_a, spec.want_b)) return;
  CelDurTs r;
  if (dur_combine(&a->payload.dur, &b->payload.dur, spec.subtract, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  if (spec.result_kind == CEL_TIMESTAMP && !timestamp_in_range(r.seconds)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  out->kind = spec.result_kind;
  out->payload.dur = r;
}

// One-shot ordering body shared by the eight helpers.  `cmp_op`
// encodes the comparison direction: -1 = "want lt result", +1 =
// "want gt result", 0 = "either lt or gt is true" (for !=).  Here
// the four ops we ship are <, <=, >, >= which fold to:
//   lt:  cmp == -1
//   le:  cmp <= 0
//   gt:  cmp == +1
//   ge:  cmp >= 0
static void run_compare(CelValue* out, const CelValue* a, const CelValue* b,
                        uint32_t want, int (*pred)(int cmp)) {
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, want)) return;
  const int cmp = dur_compare_lex(&a->payload.dur, &b->payload.dur);
  write_bool(out, pred(cmp));
}

static int pred_lt(int cmp) { return cmp < 0; }
static int pred_le(int cmp) { return cmp <= 0; }
static int pred_gt(int cmp) { return cmp > 0; }
static int pred_ge(int cmp) { return cmp >= 0; }

// ----- arithmetic -----

void cel_dur_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_arith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
            (ArithSpec){CEL_DURATION, CEL_DURATION, 0, CEL_DURATION});
}

void cel_dur_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_arith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
            (ArithSpec){CEL_DURATION, CEL_DURATION, 1, CEL_DURATION});
}

void cel_ts_dur_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_arith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
            (ArithSpec){CEL_TIMESTAMP, CEL_DURATION, 0, CEL_TIMESTAMP});
}

void cel_dur_ts_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_arith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
            (ArithSpec){CEL_DURATION, CEL_TIMESTAMP, 0, CEL_TIMESTAMP});
}

void cel_ts_dur_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_arith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
            (ArithSpec){CEL_TIMESTAMP, CEL_DURATION, 1, CEL_TIMESTAMP});
}

void cel_ts_ts_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_arith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
            (ArithSpec){CEL_TIMESTAMP, CEL_TIMESTAMP, 1, CEL_DURATION});
}

// ----- ordering -----

void cel_dur_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_compare(cel_value_at(out_slot), cel_value_at(a_slot),
              cel_value_at(b_slot), CEL_DURATION, pred_lt);
}

void cel_dur_le_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_compare(cel_value_at(out_slot), cel_value_at(a_slot),
              cel_value_at(b_slot), CEL_DURATION, pred_le);
}

void cel_dur_gt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_compare(cel_value_at(out_slot), cel_value_at(a_slot),
              cel_value_at(b_slot), CEL_DURATION, pred_gt);
}

void cel_dur_ge_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_compare(cel_value_at(out_slot), cel_value_at(a_slot),
              cel_value_at(b_slot), CEL_DURATION, pred_ge);
}

void cel_ts_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_compare(cel_value_at(out_slot), cel_value_at(a_slot),
              cel_value_at(b_slot), CEL_TIMESTAMP, pred_lt);
}

void cel_ts_le_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_compare(cel_value_at(out_slot), cel_value_at(a_slot),
              cel_value_at(b_slot), CEL_TIMESTAMP, pred_le);
}

void cel_ts_gt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_compare(cel_value_at(out_slot), cel_value_at(a_slot),
              cel_value_at(b_slot), CEL_TIMESTAMP, pred_gt);
}

void cel_ts_ge_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  run_compare(cel_value_at(out_slot), cel_value_at(a_slot),
              cel_value_at(b_slot), CEL_TIMESTAMP, pred_ge);
}
