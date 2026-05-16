;; CEL source:  ts.getDate()
;; Decl:        ts : google.protobuf.Timestamp
;;
;; M7B.C slice — pure-wasm UTC accessor.  Per `m7b-duration-timestamp.md`
;; §4.3 (Option C), the 10 UTC accessor overloads are pure-wasm
;; helpers backed by the shared `cel_civil_from_seconds` algorithm
;; (probe A confirmed Hinnant `civil_from_days` is bit-identical to
;; `absl::ToCivilSecond(UTCTimeZone())` across the §6.4 quirk grid).
;; No host trampoline — the runtime kernel stays descriptor-free
;; per `design.md` §4.7.6.
;;
;; The companion two-arg form `ts.getDate('America/Los_Angeles')`
;; (M7B.E) DOES need a host trampoline because the IANA tzdata
;; database lives on the host — that lowers to
;; `cel_host.cel_timestamp_tz_accessor(out, ts, tz, kind=2)`.  This
;; file is the no-TZ shape only.
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  workspace slot for `ts` (variable bound by host)
;;             {kind=CEL_TIMESTAMP(13), payload.ts={seconds, nanos}}
;;   [40, 64)  workspace: kCallExpr result slot (out=40) for getDate
;;   [64, mem_size)  bump arena
;;
;; New import this slice:
;;   cel.cel_ts_day_of_month_1_utc(out_slot, ts_slot) — i32×2 → ()
;;
;; cel_ts_day_of_month_1_utc contract (helper body in
;; `compiler_v2/runtime/cel_time.c`):
;;   - reads ts_slot as CEL_TIMESTAMP.  Any other kind → out_slot =
;;     {CEL_ERROR, CEL_ERR_TYPE_MISMATCH}.
;;   - 3VL absorption — CEL_UNKNOWN/CEL_ERROR pass through.
;;   - happy path: runs `cel_civil_from_seconds(seconds, &civil)`,
;;     reads `civil.day_1`, writes out_slot = {CEL_INT(2),
;;     payload.i = day_1}.  Nanos are ignored — getDate() only
;;     resolves to day-level precision.
;;
;; The cel_civil_from_seconds helper is the shared core for all 10
;; UTC accessors; per-accessor wrappers each project one field of
;; the CelCivil struct.  See `m7b-duration-timestamp.md` §4.8 for
;; the algorithm citation (Hinnant date_algorithms.html — public,
;; portable, no float, no overflow on the [Y0001, Y9999] range).
;;
;; Codegen shape (emitted by `expr_lower.cc::EmitGeneralCall` once
;; `timestamp_to_day_of_month_1_based` graduates from
;; `kExplicitlyUnimplementedIds`):
;;
;;   1. Emit ResolvePass-stamped read of `ts` workspace slot (16).
;;   2. Emit the call to the pure-wasm helper with (out_slot=40,
;;      ts_slot=16) — same shape as `16_arith_int_add.wat`.
;;   3. Result lives at slot 40.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel" "cel_ts_day_of_month_1_utc"
          (func $cel_ts_day_of_month_1_utc (param i32 i32)))

  (func $eval (result i32)
    (local $ts_off i32)

    ;; Variable prelude — `ts` lives at workspace slot 16 (host
    ;; writes the CEL_TIMESTAMP CelValue there before Eval).
    (local.set $ts_off (i32.const 16))

    ;; Arena begins at 64 (past workspace + out_slot, 16-byte aligned).
    (call $cel_reset (i32.const 64) (i32.const 131072))

    ;; Pure-wasm accessor — no host trampoline.
    (call $cel_ts_day_of_month_1_utc
          (i32.const 40)         ;; out_slot
          (local.get $ts_off))   ;; ts_slot

    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
