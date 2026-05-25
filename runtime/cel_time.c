// Timestamp / Duration kernels — see cel_time.h for the public ABI.

#include "compiler_v2/runtime/cel_time.h"

#include <stdint.h>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_internal.h"

// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/time_functions.cc::
//     {add,subtract}_duration_duration / _time_duration /
//     _time_time + less / greater / etc.
// Pure-int kernels here; string parse / format live in
// `cel_time_parse.{h,cc}` (vendored absl); the TZ-aware accessor
// lives behind a single dispatch trampoline on the host (see
// `rewrite/m7b-duration-timestamp.md`).

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

// proto Duration text format range — 10,000 years.  Per cel-cpp /
// langdef, arithmetic results outside this range produce an error
// (mirrors `google.protobuf.Duration`'s documented seconds bound).
#define DURATION_MIN_SECONDS (-315576000000LL)
#define DURATION_MAX_SECONDS (315576000000LL)

// Sign-correlated (seconds, nanos) range check.  The "raw" form
// (just seconds) misses corners where seconds is exactly at MIN /
// MAX and nanos has the same sign (which pushes the real value
// past the bound).  Used for both timestamp and duration kinds.
static inline int payload_in_range(int64_t seconds, int32_t nanos, int64_t lo,
                                   int64_t hi) {
  if (seconds > hi || seconds < lo) return 0;
  // Boundary refinement: at MAX a positive nanos overflows; at MIN
  // a negative nanos overflows.
  if (seconds == hi && nanos > 0) return 0;
  if (seconds == lo && nanos < 0) return 0;
  return 1;
}

static inline int timestamp_in_range(int64_t seconds, int32_t nanos) {
  return payload_in_range(seconds, nanos, TIMESTAMP_MIN_SECONDS,
                          TIMESTAMP_MAX_SECONDS);
}

static inline int duration_in_range(int64_t seconds, int32_t nanos) {
  return payload_in_range(seconds, nanos, DURATION_MIN_SECONDS,
                          DURATION_MAX_SECONDS);
}

// Tighter bound used for arithmetic results.  cel-cpp's
// `CheckedSub(Time, Time)` (overflow.cc:295) represents the result
// in int64 nanoseconds (`s * 1e9 + ns`); the implicit bound is
// therefore `|s * 1e9 + ns| <= INT64_MAX` ≈ ±292 years, much
// tighter than the proto-Duration ±10000-year parse-side bound.
// Empirically verified against cel-cpp (see
// `compiler_v2/throwaway/cel_cpp_corner_probe.cc`): with
// `enable_timestamp_duration_overflow_errors=true`,
// `ts(9999) - ts(0001)` returns `OUT_OF_RANGE: integer overflow`.
//
// Avoid `__builtin_mul_overflow(int64, int64)` here — it lowers to
// `__multi3` on wasm32 which the freestanding cross-compile
// doesn't link (same workaround pattern as cel_arith.c).
static inline int arith_duration_in_range(int64_t seconds, int32_t nanos) {
  // INT64_MAX = 9223372036854775807 = 9223372036 * 1e9 + 854775807.
  const int64_t kMaxAbsSec = 9223372036LL;
  const int32_t kMaxBoundaryNs = 854775807;
  if (seconds > kMaxAbsSec || seconds < -kMaxAbsSec) return 0;
  if (seconds == kMaxAbsSec && nanos > kMaxBoundaryNs) return 0;
  if (seconds == -kMaxAbsSec && nanos < -kMaxBoundaryNs) return 0;
  return 1;
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
  if (spec.result_kind == CEL_TIMESTAMP &&
      !timestamp_in_range(r.seconds, r.nanos)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  if (spec.result_kind == CEL_DURATION &&
      !arith_duration_in_range(r.seconds, r.nanos)) {
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

static int pred_lt(int cmp) {
  return cmp < 0;
}
static int pred_le(int cmp) {
  return cmp <= 0;
}
static int pred_gt(int cmp) {
  return cmp > 0;
}
static int pred_ge(int cmp) {
  return cmp >= 0;
}

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

// ─── Civil-calendar helper + UTC accessor family ────────────────────────
//
// `cel_civil_from_seconds` projects an int64 epoch seconds value to
// the Gregorian (year, month, day, hour, minute, second, day_of_year,
// day_of_week) tuple via the documented Howard Hinnant
// `civil_from_days` algorithm
// (http://howardhinnant.github.io/date_algorithms.html#civil_from_days).
// Pure integer arithmetic; correct across the full langdef
// timestamp range and beyond.  Validated against
// `absl::ToCivilSecond(UTCTimeZone())` for the §6.4 quirk grid in
// Probe A of the m7b plan.
//
// Day-of-week + day-of-year are computed alongside the y/m/d so a
// single helper call serves every UTC-accessor projection.

#define SECONDS_PER_DAY 86400LL
#define DAYS_FROM_EPOCH_TO_HINNANT_EPOCH 719468LL
#define DAYS_PER_ERA 146097LL  // 400 Gregorian years
#define ERA_BIAS 146096LL      // for negative-floor div

typedef struct {
  int32_t year;     // Gregorian year (negative possible if input below year 0)
  int32_t month_0;  // 0-based, 0=Jan ... 11=Dec  (cel-cpp's getMonth)
  int32_t day_1;    // 1-based, 1..31             (cel-cpp's getDate)
  int32_t day_0;    // 0-based, 0..30             (cel-cpp's getDayOfMonth)
  int32_t hour;     // 0..23
  int32_t minute;   // 0..59
  int32_t second;   // 0..59
  int32_t day_of_year;  // 0-based, Jan 1 = 0
  int32_t day_of_week;  // 0-based, Sunday = 0
} CelCivil;

// 0-indexed cumulative days at the start of each month.  Used to
// convert the Hinnant-form day-of-year (March 1 = 0) to the
// CEL-spec day-of-year (Jan 1 = 0).
static const int32_t kCumulativeDaysBeforeMonthNonLeap[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
static const int32_t kCumulativeDaysBeforeMonthLeap[12] = {
    0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};

static inline int is_leap_year(int32_t year) {
  if (year % 4 != 0) return 0;
  if (year % 100 != 0) return 1;
  return (year % 400 == 0) ? 1 : 0;
}

// Floor-divide epoch seconds into (days, day_secs) such that
// `epoch_seconds = days * 86400 + day_secs` and `0 <= day_secs < 86400`.
// C's `/` truncates toward zero; for negative epochs we adjust to
// make `days` a true floor.
static void split_days(int64_t epoch_seconds, int64_t* days,
                       int64_t* day_secs) {
  int64_t d = epoch_seconds / SECONDS_PER_DAY;
  int64_t r = epoch_seconds - d * SECONDS_PER_DAY;
  if (r < 0) {
    d -= 1;
    r += SECONDS_PER_DAY;
  }
  *days = d;
  *day_secs = r;
}

static void cel_civil_from_seconds(int64_t epoch_seconds, CelCivil* out) {
  int64_t days;
  int64_t day_secs;
  split_days(epoch_seconds, &days, &day_secs);

  out->hour = (int32_t)(day_secs / 3600);
  out->minute = (int32_t)((day_secs % 3600) / 60);
  out->second = (int32_t)(day_secs % 60);

  // Day of week: Jan 1, 1970 = Thursday = 4.  Adding 7 inside the
  // modulus normalises the C-defined truncated mod into [0, 6].
  int32_t dow = (int32_t)(((days % 7) + 4 + 7) % 7);
  out->day_of_week = dow;

  // Hinnant civil_from_days.  Shifts epoch so that day 0 is March 1
  // of year 0, which puts the Feb-29 leap-day on the LAST day of
  // each year-cycle and lets the era / yoe / mp formulas avoid
  // branching.
  int64_t z = days + DAYS_FROM_EPOCH_TO_HINNANT_EPOCH;
  int64_t era = (z >= 0 ? z : z - ERA_BIAS) / DAYS_PER_ERA;
  uint32_t doe = (uint32_t)(z - (era * DAYS_PER_ERA));
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t y = (int32_t)((int64_t)yoe + (era * 400));
  uint32_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
  uint32_t mp = ((5 * doy) + 2) / 153;
  uint32_t d = doy - (((153 * mp) + 2) / 5) + 1;
  uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
  out->year = y;
  out->month_0 = (int32_t)m - 1;
  out->day_1 = (int32_t)d;
  out->day_0 = (int32_t)d - 1;

  // Day of year (Jan 1 = 0) — convert from the y/m/d we just
  // computed using the cumulative-days table.  Cheaper than
  // recovering the doe form because we already have y/m/d.
  const int32_t* table = is_leap_year(y) ? kCumulativeDaysBeforeMonthLeap
                                         : kCumulativeDaysBeforeMonthNonLeap;
  out->day_of_year = table[out->month_0] + (int32_t)d - 1;
}

// Common preamble for every UTC accessor: 3VL absorb + kind guard,
// then compute the civil tuple.  Returns 1 if the result has already
// been written (3VL absorbed or kind mismatch); 0 to continue.
static int ts_accessor_prelude(CelValue* out, const CelValue* a,
                               CelCivil* civil) {
  if (absorb_3vl_unary(out, a)) return 1;
  if (a->kind != CEL_TIMESTAMP) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  cel_civil_from_seconds(a->payload.ts.seconds, civil);
  return 0;
}

#define DEFINE_TS_ACCESSOR(name, projection)       \
  void name(uint32_t out_slot, uint32_t ts_slot) { \
    CelValue* out = cel_value_at(out_slot);        \
    const CelValue* a = cel_value_at(ts_slot);     \
    CelCivil c;                                    \
    if (ts_accessor_prelude(out, a, &c)) return;   \
    write_int(out, projection);                    \
  }

DEFINE_TS_ACCESSOR(cel_ts_year_utc_at_v, c.year)
DEFINE_TS_ACCESSOR(cel_ts_month_utc_at_v, c.month_0)
DEFINE_TS_ACCESSOR(cel_ts_day_of_month_1_utc_at_v, c.day_1)
DEFINE_TS_ACCESSOR(cel_ts_day_of_month_utc_at_v, c.day_0)
DEFINE_TS_ACCESSOR(cel_ts_day_of_year_utc_at_v, c.day_of_year)
DEFINE_TS_ACCESSOR(cel_ts_day_of_week_utc_at_v, c.day_of_week)
DEFINE_TS_ACCESSOR(cel_ts_hours_utc_at_v, c.hour)
DEFINE_TS_ACCESSOR(cel_ts_minutes_utc_at_v, c.minute)
DEFINE_TS_ACCESSOR(cel_ts_seconds_utc_at_v, c.second)

#undef DEFINE_TS_ACCESSOR

// Milliseconds is the one ts accessor that reads the nanos field
// instead of projecting the civil tuple — kept inline rather than
// folded into the macro.
void cel_ts_milliseconds_utc_at_v(uint32_t out_slot, uint32_t ts_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(ts_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_TIMESTAMP) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // cel-cpp / langdef: getMilliseconds returns the ms component
  // within the current civil second (always [0, 999]), not the
  // absolute milliseconds since epoch.  For pre-epoch timestamps
  // our sign-correlated CelDurTs stores nanos negative; convert
  // to the unix-floor form (positive nanos in [0, 1e9)) before
  // dividing.  This matches cel-cpp's
  // `ToInt64Milliseconds(t - FloorToSecond(t))`.
  int32_t n = a->payload.ts.nanos;
  if (n < 0) n += 1000000000;
  write_int(out, n / 1000000);
}

// ─── Duration accessors (4 helpers) ─────────────────────────────────────

static int duration_accessor_prelude(CelValue* out, const CelValue* a) {
  if (absorb_3vl_unary(out, a)) return 1;
  if (a->kind != CEL_DURATION) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  return 0;
}

void cel_dur_hours_at_v(uint32_t out_slot, uint32_t d_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(d_slot);
  if (duration_accessor_prelude(out, a)) return;
  // C99 integer division truncates toward zero — matches cel-cpp's
  // `duration_to_hours` which uses `IDivDuration(d, absl::Hours(1))`
  // for the sign-preserving truncated form.
  write_int(out, a->payload.dur.seconds / 3600);
}

void cel_dur_minutes_at_v(uint32_t out_slot, uint32_t d_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(d_slot);
  if (duration_accessor_prelude(out, a)) return;
  write_int(out, a->payload.dur.seconds / 60);
}

void cel_dur_seconds_at_v(uint32_t out_slot, uint32_t d_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(d_slot);
  if (duration_accessor_prelude(out, a)) return;
  write_int(out, a->payload.dur.seconds);
}

// ─── Pure-wasm half: int <-> ts/dur conversions ────────────────────────

void cel_ts_to_int_at_v(uint32_t out_slot, uint32_t ts_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(ts_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_TIMESTAMP) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // langdef + cel-cpp: int(timestamp) returns the epoch-seconds
  // field; nanos are truncated.
  write_int(out, a->payload.ts.seconds);
}

void cel_dur_to_int_at_v(uint32_t out_slot, uint32_t dur_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(dur_slot);
  if (duration_accessor_prelude(out, a)) return;
  // langdef + cel-cpp: int(duration) returns whole seconds,
  // truncating toward zero — the sign-correlated form (Probe D)
  // makes this just the seconds field as-is.
  write_int(out, a->payload.dur.seconds);
}

void cel_int_to_ts_at_v(uint32_t out_slot, uint32_t int_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(int_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // langdef-pinned timestamp range — same gate run_arith uses for
  // arithmetic results, applied here so that
  // `timestamp(INT64_MAX)` poisons cleanly rather than producing
  // a CelValue that downstream accessors would interpret out-of-
  // range.
  const int64_t s = a->payload.i;
  if (!timestamp_in_range(s, 0)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  out->kind = CEL_TIMESTAMP;
  out->payload.ts = (CelDurTs){.seconds = s, .nanos = 0, ._pad = 0};
}

// ─── With-TZ accessor shims ───────────────────────────────────────────
//
// Each shim is a thin wrapper over the host trampoline.  The
// `cel_host.cel_timestamp_tz_accessor` import is declared with
// `import_module/import_name` on wasm32 and resolved at instantiate
// time; on the host build (unit tests that link cel_runtime as a
// cc_library), a weak no-op stub poisons out_slot so the build
// links cleanly.  Identical pattern to
// `cel_host_resolve_message_type_name` in cel_type.c.

#ifdef __wasm__
extern void cel_host_cel_timestamp_tz_accessor(uint32_t out_slot,
                                               uint32_t ts_slot,
                                               uint32_t tz_slot,
                                               uint32_t accessor_kind)
    __attribute__((import_module("cel_host"),
                   import_name("cel_timestamp_tz_accessor")));
#else
__attribute__((weak)) void cel_host_cel_timestamp_tz_accessor(
    uint32_t out_slot, uint32_t ts_slot, uint32_t tz_slot,
    uint32_t accessor_kind) {
  (void)ts_slot;
  (void)tz_slot;
  (void)accessor_kind;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
#endif

#define DEFINE_TZ_ACCESSOR_SHIM(name, kind)                                 \
  void name(uint32_t out_slot, uint32_t ts_slot, uint32_t tz_slot) {        \
    cel_host_cel_timestamp_tz_accessor(out_slot, ts_slot, tz_slot, (kind)); \
  }

DEFINE_TZ_ACCESSOR_SHIM(cel_ts_year_with_tz_at_vv, CEL_TZ_ACC_YEAR)
DEFINE_TZ_ACCESSOR_SHIM(cel_ts_month_with_tz_at_vv, CEL_TZ_ACC_MONTH)
DEFINE_TZ_ACCESSOR_SHIM(cel_ts_day_of_month_1_with_tz_at_vv,
                        CEL_TZ_ACC_DAY_OF_MONTH_1)
DEFINE_TZ_ACCESSOR_SHIM(cel_ts_day_of_month_with_tz_at_vv,
                        CEL_TZ_ACC_DAY_OF_MONTH)
DEFINE_TZ_ACCESSOR_SHIM(cel_ts_day_of_year_with_tz_at_vv,
                        CEL_TZ_ACC_DAY_OF_YEAR)
DEFINE_TZ_ACCESSOR_SHIM(cel_ts_day_of_week_with_tz_at_vv,
                        CEL_TZ_ACC_DAY_OF_WEEK)
DEFINE_TZ_ACCESSOR_SHIM(cel_ts_hours_with_tz_at_vv, CEL_TZ_ACC_HOURS)
DEFINE_TZ_ACCESSOR_SHIM(cel_ts_minutes_with_tz_at_vv, CEL_TZ_ACC_MINUTES)
DEFINE_TZ_ACCESSOR_SHIM(cel_ts_seconds_with_tz_at_vv, CEL_TZ_ACC_SECONDS)
DEFINE_TZ_ACCESSOR_SHIM(cel_ts_milliseconds_with_tz_at_vv,
                        CEL_TZ_ACC_MILLISECONDS)

#undef DEFINE_TZ_ACCESSOR_SHIM

void cel_int_to_dur_at_v(uint32_t out_slot, uint32_t int_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(int_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (!duration_in_range(a->payload.i, 0)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  out->kind = CEL_DURATION;
  out->payload.dur = (CelDurTs){.seconds = a->payload.i, .nanos = 0, ._pad = 0};
}

void cel_dur_milliseconds_at_v(uint32_t out_slot, uint32_t d_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(d_slot);
  if (duration_accessor_prelude(out, a)) return;
  // cel-cpp / spec: Duration `getMilliseconds` returns the
  // *millisecond component* (sub-second ms in [-999, 999]),
  // NOT the total duration in milliseconds.  Sign-preserved.
  // The conformance test's description pins this: "this is not
  // the same as converting the duration to milliseconds".
  write_int(out, a->payload.dur.nanos / 1000000);
}
