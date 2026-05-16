// Timestamp / Duration kernels — pure-wasm slot-out helpers per
// `design.md §4.2` and `m7b-duration-timestamp.md` §4.3.  All
// bodies follow the standard `(out_slot, arg_slot...) -> void`
// shape; reads operands from the workspace, writes the result
// CelValue at out_slot.
//
// Scope (m7b §5):
//   - M7B.B: 6 arithmetic helpers + 8 ordering helpers (this file).
//   - M7B.C: 14 UTC accessor helpers + civil-calendar helper (this
//     file, expanded inline when M7B.C ships).
//   - M7B.D pure-wasm half: int<->ts/dur conversions + identities
//     (this file, expanded inline when M7B.D ships).
//   - M7B.E per-kind shim helpers around the host TZ accessor
//     trampoline (this file, expanded inline when M7B.E ships).
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

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_TIME_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_TIME_H_

#include <stdint.h>

#include "compiler_v2/runtime/cel_data.h"

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
// (M7B.E) route through a single dispatch trampoline on the host.
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

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_TIME_H_
