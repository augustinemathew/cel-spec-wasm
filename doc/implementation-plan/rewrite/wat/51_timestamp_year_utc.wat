;; CEL source:  ts.getFullYear()
;; Decl:        ts : google.protobuf.Timestamp
;;
;; M7B.C slice — pure-wasm UTC accessor.  Per `m7b-duration-timestamp.md`
;; §4.3 (Option C), the 10 UTC accessor overloads are pure-wasm
;; helpers backed by the shared `cel_civil_from_seconds` algorithm
;; (probe A confirmed Hinnant `civil_from_days` is bit-identical to
;; `absl::ToCivilSecond(UTCTimeZone())` across the §6.4 quirk grid:
;; Y2K leap-divisible-400, century-not-leap, langdef Y0001 lower
;; bound, Y9999 upper bound).  No host trampoline — the runtime
;; kernel stays descriptor-free per `design.md` §4.7.6.
;;
;; This WAT is the load-bearing pin for ALL 14 accessor helpers — the
;; 10 timestamp UTC accessors (`cel_ts_year_utc`, `cel_ts_month_utc`,
;; `cel_ts_day_of_month_utc`, `cel_ts_day_of_month_1_utc`,
;; `cel_ts_day_of_year_utc`, `cel_ts_day_of_week_utc`,
;; `cel_ts_hours_utc`, `cel_ts_minutes_utc`, `cel_ts_seconds_utc`,
;; `cel_ts_milliseconds_utc`) AND the 4 duration accessors
;; (`cel_dur_hours`, `cel_dur_minutes`, `cel_dur_seconds`,
;; `cel_dur_milliseconds`) all use the identical 2-arg `(out_slot,
;; v_slot)` shape; the only difference is the `CelCivil` field
;; projection (for ts) or the truncating-int division ladder
;; (for dur).  We trace `cel_ts_year_utc` because it's the canonical
;; example used in the plan §6.9 + cel-cpp parity matrix.
;;
;; The companion two-arg form `ts.getFullYear('America/Los_Angeles')`
;; (M7B.E) DOES need a host trampoline because the IANA tzdata
;; database lives on the host — that lowers to
;; `cel_host.cel_timestamp_tz_accessor(out, ts, tz, kind=0)`.  See
;; `54_timestamp_year_with_tz.wat`.  This file is the no-TZ shape only.
;;
;; Memory layout:
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  workspace slot for `ts` (variable bound by host)
;;             {kind=CEL_TIMESTAMP(13), payload.ts={seconds=1234567890,
;;              nanos=0, _pad=0}}
;;   [40, 64)  workspace: kCallExpr result slot (out=40) for getFullYear
;;   [64+]  bump arena (malloc'd in heap)
;;
;; New import this slice:
;;   cel.cel_ts_year_utc(out_slot, ts_slot) — i32×2 → ()
;;
;; cel_ts_year_utc contract (helper body in
;; `runtime/cel_time.c`):
;;   - reads ts_slot as CEL_TIMESTAMP.  Any other kind → out_slot =
;;     {CEL_ERROR, CEL_ERR_TYPE_MISMATCH}.
;;   - 3VL absorption — CEL_UNKNOWN/CEL_ERROR pass through.
;;   - happy path: runs `cel_civil_from_seconds(seconds, &civil)`,
;;     reads `civil.year`, writes out_slot = {CEL_INT(2),
;;     payload.i = year}.  Nanos are ignored — getFullYear() only
;;     resolves to year-level precision.  For the bound-value
;;     seconds=1234567890 (2009-02-13T23:31:30Z) the result is
;;     {CEL_INT, i=2009}.
;;
;; The `cel_civil_from_seconds` helper is the shared core for all
;; 10 UTC accessors; per-accessor wrappers each project one field of
;; the `CelCivil` struct.  See `m7b-duration-timestamp.md` §4.9 for
;; the algorithm citation (Hinnant date_algorithms.html — public,
;; portable, no float, no overflow on the [Y0001, Y9999] range).
;;
;; Codegen shape (emitted by `expr_lower.cc::EmitGeneralCall` once
;; `timestamp_to_year` graduates from `kExplicitlyUnimplementedIds`):
;;
;;   1. Emit ResolvePass-stamped read of `ts` workspace slot (16).
;;   2. Emit the call to the pure-wasm helper with (out_slot=40,
;;      ts_slot=16) — same shape as `16_arith_int_add.wat`.
;;   3. Result lives at slot 40.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_ts_year_utc"
          (func $cel_ts_year_utc (param i32 i32)))

  (func $eval (result i32)
    (local $ts_off i32)

    ;; Variable prelude — `ts` lives at workspace slot 16 (host
    ;; writes the CEL_TIMESTAMP CelValue there before Eval).
    (local.set $ts_off (i32.const 16))

    ;; Arena begins at 64 (past workspace + out_slot, 16-byte aligned).
    (call $arena_reset)

    ;; Pure-wasm accessor — no host trampoline.
    (call $cel_ts_year_utc
          (i32.const 40)         ;; out_slot
          (local.get $ts_off))   ;; ts_slot

    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
