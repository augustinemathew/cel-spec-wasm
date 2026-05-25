// Timestamp / Duration kernels — pure-wasm slot-out helpers per
// `rewrite/design.md` §4.2 and `rewrite/m7b-duration-timestamp.md`
// §4.3.  All bodies follow the standard
// `(out_slot, arg_slot...) -> void` shape; reads operands from the
// workspace, writes the result CelValue at out_slot.
//
// Scope:
//   - Arithmetic: 6 helpers (add / sub variants over dur / ts).
//   - Ordering: 8 helpers ({lt,le,gt,ge} × {dur, ts}).
//   - UTC accessors: 10 timestamp + 4 duration accessor helpers
//     over the Hinnant civil-calendar helper.
//   - Conversions: int <-> ts/dur (pure-wasm half).  String parse
//     / format live in `cel_time_parse.{h,cc}`.
//   - With-TZ accessor shims: 10 helpers, all routing through the
//     single host trampoline `cel_host.cel_timestamp_tz_accessor`
//     with a fixed accessor_kind constant per shim.
//
// Semantics:
//   - 3VL absorption: CEL_UNKNOWN / CEL_ERROR on either operand
//     propagates verbatim (mirrors cel_arith.c).  See cel_internal.h
//     `absorb_3vl_binary`.
//   - Operand kinds checked exactly: wrong kind on either operand →
//     `CEL_ERR_TYPE_MISMATCH` poison.  Cross-kind arithmetic (e.g.
//     `ts + ts`) is checker-rejected upstream; the guards here are
//     defence-in-depth.
//   - Overflow on signed `int64 seconds` → `CEL_ERR_OVERFLOW`.
//     Detected via `__builtin_*_overflow` at every signed add /
//     sub; nanos overflow always carries cleanly because the
//     unbiased nanos sum sits in (-2e9, 2e9) and fits in int32.
//   - Result normalisation is sign-correlated (proto Duration
//     text format / `absl::IDivDuration` form per Probe D) — seconds
//     and nanos share sign; |nanos| < 1e9 post-normalise.

#ifndef CELWASM_RUNTIME_CEL_TIME_H_
#define CELWASM_RUNTIME_CEL_TIME_H_

#include <stdint.h>

#include "runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----- Arithmetic (6 helpers) ---------------------------------------------
// Operand kinds pinned in helper name suffix.  Result kind:
//   (dur, dur) -> dur          : add, sub
//   (ts, dur) | (dur, ts) -> ts: add  (commutative; checker emits
//                                 either helper depending on the
//                                 source-order operands)
//   (ts, dur) -> ts            : sub  (subtract a duration from a
//                                 timestamp; non-commutative)
//   (ts, ts) -> dur            : sub  (delta between two timestamps)

void cel_dur_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_dur_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_ts_dur_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_dur_ts_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_ts_dur_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_ts_ts_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

// ----- Ordering (8 helpers) -----------------------------------------------
// Lexicographic (seconds, nanos) compare — sign-correlated
// representation makes this equivalent to comparing the absl::Duration
// / absl::Time values themselves.  Result is `CEL_BOOL`.
// Equality / inequality route through the existing
// `cel_equals_at_vv` / `cel_not_equals_at_vv` dispatch.

void cel_dur_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_dur_le_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_dur_gt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_dur_ge_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_ts_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_ts_le_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_ts_gt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_ts_ge_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

// ----- UTC timestamp accessors (10 helpers) --------------------------------
// All take a single Timestamp operand and write the matching int64
// projection.  No TZ argument — UTC by definition; the with-TZ variants
// (below) route through a single dispatch trampoline on the host.
//
// Result kinds + ranges per langdef + cel-cpp's
// `runtime/standard/time_functions.cc`:
//   year       : int  (Gregorian year, e.g. 2009)
//   month      : int  (0-based per cel-cpp; Jan = 0, Dec = 11)
//   day_of_month_1 : int  (1-based, 1..31; matches `getDate`)
//   day_of_month   : int  (0-based, 0..30; matches `getDayOfMonth`)
//   day_of_year    : int  (0-based, Jan 1 = 0)
//   day_of_week    : int  (0-based, Sunday = 0)
//   hours / minutes / seconds : int (0..23 / 0..59 / 0..59)
//   milliseconds   : int  (nanos / 1_000_000, truncating)

void cel_ts_year_utc_at_v(uint32_t out_slot, uint32_t ts_slot);
void cel_ts_month_utc_at_v(uint32_t out_slot, uint32_t ts_slot);
void cel_ts_day_of_month_1_utc_at_v(uint32_t out_slot, uint32_t ts_slot);
void cel_ts_day_of_month_utc_at_v(uint32_t out_slot, uint32_t ts_slot);
void cel_ts_day_of_year_utc_at_v(uint32_t out_slot, uint32_t ts_slot);
void cel_ts_day_of_week_utc_at_v(uint32_t out_slot, uint32_t ts_slot);
void cel_ts_hours_utc_at_v(uint32_t out_slot, uint32_t ts_slot);
void cel_ts_minutes_utc_at_v(uint32_t out_slot, uint32_t ts_slot);
void cel_ts_seconds_utc_at_v(uint32_t out_slot, uint32_t ts_slot);
void cel_ts_milliseconds_utc_at_v(uint32_t out_slot, uint32_t ts_slot);

// ----- Duration accessors (4 helpers) --------------------------------------
// Truncating int division on the duration value; sign preserved.
//
//   getHours        : seconds / 3600
//   getMinutes      : seconds / 60
//   getSeconds      : seconds                  (whole seconds)
//   getMilliseconds : (seconds * 1000) + (nanos / 1_000_000)
//                     (NB: this is *within the current second* per
//                     cel-cpp's `duration_to_milliseconds`; whole
//                     duration in ms is `Duration / 1ms` and shipped
//                     as `getMilliseconds` only on the in-second part).

void cel_dur_hours_at_v(uint32_t out_slot, uint32_t d_slot);
void cel_dur_minutes_at_v(uint32_t out_slot, uint32_t d_slot);
void cel_dur_seconds_at_v(uint32_t out_slot, uint32_t d_slot);
void cel_dur_milliseconds_at_v(uint32_t out_slot, uint32_t d_slot);

// ----- Conversions (4 helpers — pure-wasm half) ----------------------------
// String <-> timestamp/duration parse + format are host trampolines
// (RFC3339 / proto-Duration text format need a real parser); the
// int-direction conversions are pure-wasm.
//
//   timestamp_to_int64 : seconds field, ignoring nanos
//   duration_to_int64  : seconds field, truncating nanos toward zero
//   int64_to_timestamp : (seconds, 0) with langdef-range check
//   int64_to_duration  : (seconds, 0); no range check (any int64 fits)

void cel_ts_to_int_at_v(uint32_t out_slot, uint32_t ts_slot);
void cel_dur_to_int_at_v(uint32_t out_slot, uint32_t dur_slot);
void cel_int_to_ts_at_v(uint32_t out_slot, uint32_t int_slot);
void cel_int_to_dur_at_v(uint32_t out_slot, uint32_t int_slot);

// ----- With-TZ accessor shims (10 helpers) ---------------------------------
// Each shim takes `(out_slot, ts_slot, tz_slot)` and delegates to
// `cel_host.cel_timestamp_tz_accessor(out, ts, tz, accessor_kind)`
// with a fixed accessor_kind constant per shim.  Single host
// trampoline absorbs all 10 surfaces — see
// `rewrite/m7b-duration-timestamp.md` §4.3 "single dispatch trampoline".

void cel_ts_year_with_tz_at_vv(uint32_t out_slot, uint32_t ts_slot,
                               uint32_t tz_slot);
void cel_ts_month_with_tz_at_vv(uint32_t out_slot, uint32_t ts_slot,
                                uint32_t tz_slot);
void cel_ts_day_of_month_1_with_tz_at_vv(uint32_t out_slot, uint32_t ts_slot,
                                         uint32_t tz_slot);
void cel_ts_day_of_month_with_tz_at_vv(uint32_t out_slot, uint32_t ts_slot,
                                       uint32_t tz_slot);
void cel_ts_day_of_year_with_tz_at_vv(uint32_t out_slot, uint32_t ts_slot,
                                      uint32_t tz_slot);
void cel_ts_day_of_week_with_tz_at_vv(uint32_t out_slot, uint32_t ts_slot,
                                      uint32_t tz_slot);
void cel_ts_hours_with_tz_at_vv(uint32_t out_slot, uint32_t ts_slot,
                                uint32_t tz_slot);
void cel_ts_minutes_with_tz_at_vv(uint32_t out_slot, uint32_t ts_slot,
                                  uint32_t tz_slot);
void cel_ts_seconds_with_tz_at_vv(uint32_t out_slot, uint32_t ts_slot,
                                  uint32_t tz_slot);
void cel_ts_milliseconds_with_tz_at_vv(uint32_t out_slot, uint32_t ts_slot,
                                       uint32_t tz_slot);

// `accessor_kind` enum — wire contract for the single dispatch
// trampoline.  Closed, append-only.  Used by both the cel_time.c
// shims AND the Layer-2 `CelTimestampTzAccessorImpl` switch; keep
// these in lockstep.
typedef enum {
  CEL_TZ_ACC_YEAR = 0,
  CEL_TZ_ACC_MONTH = 1,
  CEL_TZ_ACC_DAY_OF_MONTH_1 = 2,
  CEL_TZ_ACC_DAY_OF_MONTH = 3,
  CEL_TZ_ACC_DAY_OF_YEAR = 4,
  CEL_TZ_ACC_DAY_OF_WEEK = 5,
  CEL_TZ_ACC_HOURS = 6,
  CEL_TZ_ACC_MINUTES = 7,
  CEL_TZ_ACC_SECONDS = 8,
  CEL_TZ_ACC_MILLISECONDS = 9,
} CelTzAccessorKind;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // CELWASM_RUNTIME_CEL_TIME_H_
