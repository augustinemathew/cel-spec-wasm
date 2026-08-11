// Timestamp / Duration kernels — see cel_time.h for the public ABI.
//
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/time_functions.cc::
//     {add,subtract}_duration_duration / _time_duration /
//     _time_time + less / greater / etc.
// Pure-integer kernels for arithmetic / ordering / conversions;
// calendar projections go through absl civil time against the UTC
// zone (the hand-rolled Hinnant civil_from_days C implementation
// predated absl linking into cel_runtime.wasm and was retired when
// absl did).  String parse / format live in `cel_time_parse.{h,cc}`;
// the TZ-aware accessor family routes through a single dispatch
// trampoline on the host (see `rewrite/m7b-duration-timestamp.md`).

#include "runtime/cel_time.h"

#include <cstdint>

#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_internal.h"

namespace {

constexpr int32_t kNanosPerSec = 1000000000;

// langdef §"Timestamps and Durations": timestamp seconds must
// represent a UTC instant in [0001-01-01T00:00:00Z, 9999-12-31T23:59:59Z].
// cel-cpp surfaces out-of-range timestamps as CEL_ERR_OVERFLOW (the
// same error code as int64 overflow on the seconds field), so we
// match that here.  Bounds: year 1 epoch = -62135596800; year 9999
// last second = 253402300799 (per the langdef and cel-cpp's
// internal/time.cc range checks).
constexpr int64_t kTimestampMinSeconds = -62135596800LL;
constexpr int64_t kTimestampMaxSeconds = 253402300799LL;

// proto Duration text format range — 10,000 years.  Per cel-cpp /
// langdef, conversion results outside this range produce an error
// (mirrors `google.protobuf.Duration`'s documented seconds bound).
constexpr int64_t kDurationMinSeconds = -315576000000LL;
constexpr int64_t kDurationMaxSeconds = 315576000000LL;

// Sign-correlated (seconds, nanos) range check.  The "raw" form
// (just seconds) misses corners where seconds is exactly at MIN /
// MAX and nanos has the same sign (which pushes the real value
// past the bound).
bool PayloadInRange(int64_t seconds, int32_t nanos, int64_t lo, int64_t hi) {
  if (seconds > hi || seconds < lo) return false;
  // Boundary refinement: at MAX a positive nanos overflows; at MIN
  // a negative nanos overflows.
  if (seconds == hi && nanos > 0) return false;
  if (seconds == lo && nanos < 0) return false;
  return true;
}

bool TimestampInRange(int64_t seconds, int32_t nanos) {
  // Langdef inclusive range is `[0001-01-01T00:00:00Z,
  // 9999-12-31T23:59:59.999999999Z]` — at MAX_SECONDS, nanos in
  // [0, 999999999] are still in range (one full second of sub-second
  // precision lives ON the boundary).  The shared PayloadInRange
  // helper's "MAX seconds + positive nanos always overflows" rule
  // is right for durations (where MAX_SECONDS is the proto Duration
  // bound, exclusive of any further fraction) but wrong here.  CEL
  // conformance row `timestamps/conversions/toString_timestamp_nanos`
  // (spec/tests/simple/testdata/timestamps.textproto) pins this.
  if (seconds > kTimestampMaxSeconds || seconds < kTimestampMinSeconds) {
    return false;
  }
  // Accept any valid nanos at MAX_SECONDS; at MIN_SECONDS a
  // (sign-correlated) negative nanos is past the lower bound.
  return seconds != kTimestampMinSeconds || nanos >= 0;
}

bool DurationInRange(int64_t seconds, int32_t nanos) {
  return PayloadInRange(seconds, nanos, kDurationMinSeconds,
                        kDurationMaxSeconds);
}

// Tighter bound used for arithmetic results.  cel-cpp's
// `CheckedSub(Time, Time)` (overflow.cc:295) represents the result
// in int64 nanoseconds (`s * 1e9 + ns`); the implicit bound is
// therefore `|s * 1e9 + ns| <= INT64_MAX` ≈ ±292 years, much
// tighter than the proto-Duration ±10000-year parse-side bound.
// Empirically verified against cel-cpp: with
// `enable_timestamp_duration_overflow_errors=true`,
// `ts(9999) - ts(0001)` returns `OUT_OF_RANGE: integer overflow`.
bool ArithDurationInRange(int64_t seconds, int32_t nanos) {
  // INT64_MAX = 9223372036854775807 = 9223372036 * 1e9 + 854775807.
  constexpr int64_t kMaxAbsSec = 9223372036LL;
  constexpr int32_t kMaxBoundaryNs = 854775807;
  if (seconds > kMaxAbsSec || seconds < -kMaxAbsSec) return false;
  if (seconds == kMaxAbsSec && nanos > kMaxBoundaryNs) return false;
  if (seconds == -kMaxAbsSec && nanos < -kMaxBoundaryNs) return false;
  return true;
}

// ----- shared helpers (file-local) ----------------------------------------

// Same-kind guard for non-uniform operand pairs.  `require_kinds`
// in cel_internal.h takes a single `want` for both operands; here
// we need (CEL_TIMESTAMP, CEL_DURATION) and friends.
bool RequireKinds2(CelValue* out, const CelValue* a, const CelValue* b,
                   uint32_t want_a, uint32_t want_b) {
  if (a->kind != want_a || b->kind != want_b) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return true;
  }
  return false;
}

// Combine two CelDurTs payloads as (s1 ± s2, n1 ± n2) with nanos-
// carry and sign-correlated normalisation.  Returns true on
// int64-seconds overflow (caller poisons out as CEL_ERR_OVERFLOW).
//
// Algorithm:
//   1. raw_s = a.s ± b.s            (overflow-checked).
//   2. raw_n = a.n ± b.n            (sum in (-2e9, 2e9) — fits in
//                                    int32 because |a.n|, |b.n| < 1e9).
//   3. Carry the |nanos| ≥ 1e9 case into seconds with ±1
//      (overflow-checked at the seconds add).
//   4. Sign-correlate.  If seconds > 0 and nanos < 0, subtract 1
//      from seconds and add 1e9 to nanos (and symmetric).  These
//      adjustments always move seconds toward zero; no overflow.
bool DurCombine(const CelDurTs* a, const CelDurTs* b, bool subtract,
                CelDurTs* result) {
  int64_t raw_s = 0;
  if (subtract) {
    if (__builtin_sub_overflow(a->seconds, b->seconds, &raw_s)) return true;
  } else {
    if (__builtin_add_overflow(a->seconds, b->seconds, &raw_s)) return true;
  }
  int32_t raw_n = subtract ? (a->nanos - b->nanos) : (a->nanos + b->nanos);
  int64_t carry = 0;
  if (raw_n >= kNanosPerSec) {
    raw_n -= kNanosPerSec;
    carry = 1;
  } else if (raw_n <= -kNanosPerSec) {
    raw_n += kNanosPerSec;
    carry = -1;
  }
  if (carry != 0 && __builtin_add_overflow(raw_s, carry, &raw_s)) return true;
  if (raw_s > 0 && raw_n < 0) {
    raw_s -= 1;
    raw_n += kNanosPerSec;
  } else if (raw_s < 0 && raw_n > 0) {
    raw_s += 1;
    raw_n -= kNanosPerSec;
  }
  result->seconds = raw_s;
  result->nanos = raw_n;
  result->_pad = 0;
  return false;
}

// Lexicographic (seconds, nanos) compare.  Returns -1 / 0 / +1.
int DurCompareLex(const CelDurTs* a, const CelDurTs* b) {
  if (a->seconds != b->seconds) return a->seconds < b->seconds ? -1 : 1;
  if (a->nanos != b->nanos) return a->nanos < b->nanos ? -1 : 1;
  return 0;
}

// One-shot arithmetic dispatch.  Each exported helper is a thin
// wrapper that supplies the (operand-kind, operand-kind, sub-flag,
// result-kind) 4-tuple and delegates to `RunArith`.  Packed in a
// struct so the param count stays under the lint gate.
struct ArithSpec {
  uint32_t want_a;
  uint32_t want_b;
  bool subtract;
  uint32_t result_kind;
};

void RunArith(CelValue* out, const CelValue* a, const CelValue* b,
              ArithSpec spec) {
  if (absorb_3vl_binary(out, a, b)) return;
  if (RequireKinds2(out, a, b, spec.want_a, spec.want_b)) return;
  CelDurTs r;
  if (DurCombine(&a->payload.dur, &b->payload.dur, spec.subtract, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  if (spec.result_kind == CEL_TIMESTAMP &&
      !TimestampInRange(r.seconds, r.nanos)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  if (spec.result_kind == CEL_DURATION &&
      !ArithDurationInRange(r.seconds, r.nanos)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  out->kind = spec.result_kind;
  out->payload.dur = r;
}

// One-shot ordering body shared by the eight helpers.  The four ops
// we ship are <, <=, >, >= which fold over the tri-state compare to:
//   lt:  cmp == -1
//   le:  cmp <= 0
//   gt:  cmp == +1
//   ge:  cmp >= 0
void RunCompare(CelValue* out, const CelValue* a, const CelValue* b,
                uint32_t want, bool (*pred)(int cmp)) {
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, want)) return;
  const int cmp = DurCompareLex(&a->payload.dur, &b->payload.dur);
  write_bool(out, pred(cmp) ? 1 : 0);
}

bool PredLt(int cmp) {
  return cmp < 0;
}
bool PredLe(int cmp) {
  return cmp <= 0;
}
bool PredGt(int cmp) {
  return cmp > 0;
}
bool PredGe(int cmp) {
  return cmp >= 0;
}

}  // namespace

// ----- arithmetic -----

extern "C" void cel_dur_add_at_vv(uint32_t out_slot, uint32_t a_slot,
                                  uint32_t b_slot) {
  RunArith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
           ArithSpec{CEL_DURATION, CEL_DURATION, false, CEL_DURATION});
}

extern "C" void cel_dur_sub_at_vv(uint32_t out_slot, uint32_t a_slot,
                                  uint32_t b_slot) {
  RunArith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
           ArithSpec{CEL_DURATION, CEL_DURATION, true, CEL_DURATION});
}

extern "C" void cel_ts_dur_add_at_vv(uint32_t out_slot, uint32_t a_slot,
                                     uint32_t b_slot) {
  RunArith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
           ArithSpec{CEL_TIMESTAMP, CEL_DURATION, false, CEL_TIMESTAMP});
}

extern "C" void cel_dur_ts_add_at_vv(uint32_t out_slot, uint32_t a_slot,
                                     uint32_t b_slot) {
  RunArith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
           ArithSpec{CEL_DURATION, CEL_TIMESTAMP, false, CEL_TIMESTAMP});
}

extern "C" void cel_ts_dur_sub_at_vv(uint32_t out_slot, uint32_t a_slot,
                                     uint32_t b_slot) {
  RunArith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
           ArithSpec{CEL_TIMESTAMP, CEL_DURATION, true, CEL_TIMESTAMP});
}

extern "C" void cel_ts_ts_sub_at_vv(uint32_t out_slot, uint32_t a_slot,
                                    uint32_t b_slot) {
  RunArith(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
           ArithSpec{CEL_TIMESTAMP, CEL_TIMESTAMP, true, CEL_DURATION});
}

// ----- ordering -----

extern "C" void cel_dur_lt_at_vv(uint32_t out_slot, uint32_t a_slot,
                                 uint32_t b_slot) {
  RunCompare(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
             CEL_DURATION, PredLt);
}

extern "C" void cel_dur_le_at_vv(uint32_t out_slot, uint32_t a_slot,
                                 uint32_t b_slot) {
  RunCompare(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
             CEL_DURATION, PredLe);
}

extern "C" void cel_dur_gt_at_vv(uint32_t out_slot, uint32_t a_slot,
                                 uint32_t b_slot) {
  RunCompare(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
             CEL_DURATION, PredGt);
}

extern "C" void cel_dur_ge_at_vv(uint32_t out_slot, uint32_t a_slot,
                                 uint32_t b_slot) {
  RunCompare(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
             CEL_DURATION, PredGe);
}

extern "C" void cel_ts_lt_at_vv(uint32_t out_slot, uint32_t a_slot,
                                uint32_t b_slot) {
  RunCompare(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
             CEL_TIMESTAMP, PredLt);
}

extern "C" void cel_ts_le_at_vv(uint32_t out_slot, uint32_t a_slot,
                                uint32_t b_slot) {
  RunCompare(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
             CEL_TIMESTAMP, PredLe);
}

extern "C" void cel_ts_gt_at_vv(uint32_t out_slot, uint32_t a_slot,
                                uint32_t b_slot) {
  RunCompare(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
             CEL_TIMESTAMP, PredGt);
}

extern "C" void cel_ts_ge_at_vv(uint32_t out_slot, uint32_t a_slot,
                                uint32_t b_slot) {
  RunCompare(cel_value_at(out_slot), cel_value_at(a_slot), cel_value_at(b_slot),
             CEL_TIMESTAMP, PredGe);
}

// ─── Civil-calendar helper + UTC accessor family ────────────────────────

namespace {

// The Gregorian projections every UTC accessor reads, in the shapes
// CEL wants (cel-cpp's `runtime/standard/time_functions.cc`):
// month 0-based, day-of-year 0-based (Jan 1 = 0), day-of-week
// 0-based (Sunday = 0).  `absl::CivilSecond` carries year / month
// (1-based) / day (1-based) / hour / minute / second directly.
struct CivilParts {
  absl::CivilSecond cs;
  int64_t day_of_year_0;
  int64_t day_of_week_0;  // Sunday = 0
};

// Project epoch seconds to the UTC civil tuple.  The UTC accessor
// family reads ONLY the seconds field of the timestamp payload —
// a pre-epoch instant stored sign-correlated as (0s, -1ns) projects
// as second 0 of 1970-01-01, not the preceding civil second.  (The
// milliseconds accessor below is the one that reads nanos, with its
// own floor shift.)
CivilParts UtcCivilFromSeconds(int64_t epoch_seconds) {
  const absl::TimeZone::CivilInfo info =
      absl::UTCTimeZone().At(absl::FromUnixSeconds(epoch_seconds));
  const absl::CivilDay day(info.cs);
  // absl::GetYearDay is 1-based; absl::Weekday is monday=0..sunday=6,
  // CEL wants sunday=0..saturday=6.
  return CivilParts{info.cs, absl::GetYearDay(day) - 1,
                    (static_cast<int64_t>(absl::GetWeekday(day)) + 1) % 7};
}

// Common preamble for every UTC accessor: 3VL absorb + kind guard.
// Returns true if the result has already been written (3VL absorbed
// or kind mismatch); false to continue.
bool TsAccessorPrelude(CelValue* out, const CelValue* a) {
  if (absorb_3vl_unary(out, a)) return true;
  if (a->kind != CEL_TIMESTAMP) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return true;
  }
  return false;
}

}  // namespace

#define DEFINE_TS_ACCESSOR(name, projection)                         \
  extern "C" void name(uint32_t out_slot, uint32_t ts_slot) {        \
    CelValue* out = cel_value_at(out_slot);                          \
    const CelValue* a = cel_value_at(ts_slot);                       \
    if (TsAccessorPrelude(out, a)) return;                           \
    const CivilParts c = UtcCivilFromSeconds(a->payload.ts.seconds); \
    write_int(out, (projection));                                    \
  }

DEFINE_TS_ACCESSOR(cel_ts_year_utc_at_v, c.cs.year())
DEFINE_TS_ACCESSOR(cel_ts_month_utc_at_v, c.cs.month() - 1)
DEFINE_TS_ACCESSOR(cel_ts_day_of_month_1_utc_at_v, c.cs.day())
DEFINE_TS_ACCESSOR(cel_ts_day_of_month_utc_at_v, c.cs.day() - 1)
DEFINE_TS_ACCESSOR(cel_ts_day_of_year_utc_at_v, c.day_of_year_0)
DEFINE_TS_ACCESSOR(cel_ts_day_of_week_utc_at_v, c.day_of_week_0)
DEFINE_TS_ACCESSOR(cel_ts_hours_utc_at_v, c.cs.hour())
DEFINE_TS_ACCESSOR(cel_ts_minutes_utc_at_v, c.cs.minute())
DEFINE_TS_ACCESSOR(cel_ts_seconds_utc_at_v, c.cs.second())

#undef DEFINE_TS_ACCESSOR

// Milliseconds is the one ts accessor that reads the nanos field
// instead of projecting the civil tuple — kept standalone rather
// than folded into the macro.
extern "C" void cel_ts_milliseconds_utc_at_v(uint32_t out_slot,
                                             uint32_t ts_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(ts_slot);
  if (TsAccessorPrelude(out, a)) return;
  // cel-cpp / langdef: getMilliseconds returns the ms component
  // within the current civil second (always [0, 999]), not the
  // absolute milliseconds since epoch.  For pre-epoch timestamps
  // our sign-correlated CelDurTs stores nanos negative; convert
  // to the unix-floor form (positive nanos in [0, 1e9)) before
  // dividing.  This matches cel-cpp's
  // `ToInt64Milliseconds(t - FloorToSecond(t))`.
  int32_t n = a->payload.ts.nanos;
  if (n < 0) n += kNanosPerSec;
  write_int(out, n / 1000000);
}

// ─── Duration accessors (4 helpers) ─────────────────────────────────────

namespace {

bool DurationAccessorPrelude(CelValue* out, const CelValue* a) {
  if (absorb_3vl_unary(out, a)) return true;
  if (a->kind != CEL_DURATION) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return true;
  }
  return false;
}

}  // namespace

extern "C" void cel_dur_hours_at_v(uint32_t out_slot, uint32_t d_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(d_slot);
  if (DurationAccessorPrelude(out, a)) return;
  // Integer division truncates toward zero — matches cel-cpp's
  // `duration_to_hours` which uses `IDivDuration(d, absl::Hours(1))`
  // for the sign-preserving truncated form.
  write_int(out, a->payload.dur.seconds / 3600);
}

extern "C" void cel_dur_minutes_at_v(uint32_t out_slot, uint32_t d_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(d_slot);
  if (DurationAccessorPrelude(out, a)) return;
  write_int(out, a->payload.dur.seconds / 60);
}

extern "C" void cel_dur_seconds_at_v(uint32_t out_slot, uint32_t d_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(d_slot);
  if (DurationAccessorPrelude(out, a)) return;
  write_int(out, a->payload.dur.seconds);
}

extern "C" void cel_dur_milliseconds_at_v(uint32_t out_slot, uint32_t d_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(d_slot);
  if (DurationAccessorPrelude(out, a)) return;
  // cel-cpp / spec: Duration `getMilliseconds` returns the
  // *millisecond component* (sub-second ms in [-999, 999]),
  // NOT the total duration in milliseconds.  Sign-preserved.
  // The conformance test's description pins this: "this is not
  // the same as converting the duration to milliseconds".
  write_int(out, a->payload.dur.nanos / 1000000);
}

// ─── Pure-wasm half: int <-> ts/dur conversions ────────────────────────

extern "C" void cel_ts_to_int_at_v(uint32_t out_slot, uint32_t ts_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(ts_slot);
  if (TsAccessorPrelude(out, a)) return;
  // langdef + cel-cpp: int(timestamp) returns the epoch-seconds
  // field; nanos are truncated.
  write_int(out, a->payload.ts.seconds);
}

extern "C" void cel_dur_to_int_at_v(uint32_t out_slot, uint32_t dur_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(dur_slot);
  if (DurationAccessorPrelude(out, a)) return;
  // langdef + cel-cpp: int(duration) returns whole seconds,
  // truncating toward zero — the sign-correlated form makes this
  // just the seconds field as-is.
  write_int(out, a->payload.dur.seconds);
}

extern "C" void cel_int_to_ts_at_v(uint32_t out_slot, uint32_t int_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(int_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // langdef-pinned timestamp range — same gate RunArith uses for
  // arithmetic results, applied here so that
  // `timestamp(INT64_MAX)` poisons cleanly rather than producing
  // a CelValue that downstream accessors would interpret out-of-
  // range.
  const int64_t s = a->payload.i;
  if (!TimestampInRange(s, 0)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  out->kind = CEL_TIMESTAMP;
  out->payload.ts = CelDurTs{.seconds = s, .nanos = 0, ._pad = 0};
}

extern "C" void cel_int_to_dur_at_v(uint32_t out_slot, uint32_t int_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(int_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (!DurationInRange(a->payload.i, 0)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  out->kind = CEL_DURATION;
  out->payload.dur = CelDurTs{.seconds = a->payload.i, .nanos = 0, ._pad = 0};
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
extern "C" void cel_host_cel_timestamp_tz_accessor(uint32_t out_slot,
                                                   uint32_t ts_slot,
                                                   uint32_t tz_slot,
                                                   uint32_t accessor_kind)
    __attribute__((import_module("cel_host"),
                   import_name("cel_timestamp_tz_accessor")));
#else
extern "C" __attribute__((weak)) void cel_host_cel_timestamp_tz_accessor(
    uint32_t out_slot, uint32_t ts_slot, uint32_t tz_slot,
    uint32_t accessor_kind) {
  (void)ts_slot;
  (void)tz_slot;
  (void)accessor_kind;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
#endif

#define DEFINE_TZ_ACCESSOR_SHIM(name, kind)                                 \
  extern "C" void name(uint32_t out_slot, uint32_t ts_slot,                 \
                       uint32_t tz_slot) {                                  \
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
