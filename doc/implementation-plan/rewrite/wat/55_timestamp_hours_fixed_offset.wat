;; CEL source:  timestamp("2009-02-13T23:31:30Z").getHours("+02:00")
;; Decl:        — (no free variables)
;;
;; M7B.E slice — fixed-offset TZ flavour of the with-TZ accessor.
;; Same host trampoline as `54_timestamp_year_with_tz.wat`
;; (`cel_host.cel_timestamp_tz_accessor`); only the `accessor_kind`
;; constant and the tz-string body differ.  Pins the fixed-offset
;; path through the same dispatch trampoline as IANA — absl's
;; `TimeZone::Load("+02:00")` parses the offset directly without
;; touching the tzdata database, but the trampoline reads it
;; through the same `absl::TimeZone` handle, so a single Layer-2
;; impl covers both flavours.
;;
;; accessor_kind=6 (kHours).  See `54_timestamp_year_with_tz.wat`
;; for the full enum table.
;;
;; Memory layout:
;;   [ 0,  16)  reserved + arena cursor/limit
;;   [16,  40)  rodata: kConst timestamp("2009-02-13T23:31:30Z") →
;;                  CelValue{kind=CEL_TIMESTAMP(13), _pad,
;;                  payload.ts={seconds=1234567890, nanos=0, _pad=0}}
;;   [40,  64)  rodata: kConst CEL_STRING tz wrapper →
;;                  CelValue{kind=CEL_STRING(5), _pad,
;;                  span={ptr=64, len=6}, pad8}
;;   [64,  70)  rodata: string body "+02:00" (6 bytes)
;;   [72,  96)  workspace: out_slot for the kCallExpr result
;;                          (8-byte aligned past rodata which ends at 70)
;;                          expected: {CEL_INT(2), i = 1}
;;                          (23:31:30Z + 02:00 = 01:31:30 next day →
;;                          getHours = 1.  This is the dateline-cross
;;                          edge that motivates having the with-TZ form
;;                          at all; the no-TZ UTC accessor at the same
;;                          ts gives getHours = 23.)
;;   [96, mem_size)  bump arena
;;
;; Codegen shape: identical to `54_timestamp_year_with_tz.wat` with
;; `(i32.const 6)` for the accessor_kind arg (kHours) instead of
;; `(i32.const 0)` (kYear), and a 6-byte tz string instead of 19.
;; The lowering arm in `expr_lower.cc::EmitGeneralCall` is the same
;; for all 10 with-TZ accessors — the OverloadTable carries the
;; per-overload `accessor_kind` constant.
;;
;; This file is the second of two WATs that together exercise both
;; absl::TimeZone code paths on the host:
;;   - 54: IANA name → tzdata lookup ("America/Los_Angeles")
;;   - 55: fixed offset → no tzdata, just signed-minute arithmetic
;;         ("+02:00")
;; Both flow through the same trampoline; Layer-2 is agnostic to
;; which flavour the tz string is.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel_host" "cel_timestamp_tz_accessor"
          (func $cel_timestamp_tz_accessor (param i32 i32 i32 i32)))

  ;; rodata @ 16: CEL_TIMESTAMP(13) {seconds=1234567890, nanos=0}.
  ;; 1234567890 = 0x499602D2.
  (data (i32.const 16)
        "\0d\00\00\00" "\00\00\00\00"
        "\d2\02\96\49\00\00\00\00"
        "\00\00\00\00" "\00\00\00\00")

  ;; rodata @ 40: CEL_STRING(5) wrapper, span.ptr=64, span.len=6, pad8.
  (data (i32.const 40)
        "\05\00\00\00" "\00\00\00\00"
        "\40\00\00\00" "\06\00\00\00"
        "\00\00\00\00\00\00\00\00")

  ;; rodata @ 64: tz body "+02:00" (6 bytes).
  (data (i32.const 64) "+02:00")

  (func $eval (result i32)
    ;; Arena begins at 96 (16-byte aligned past out_slot @ 72).
    (call $arena_reset)

    ;; Host trampoline: dispatch on accessor_kind=6 (kHours).
    (call $cel_timestamp_tz_accessor
          (i32.const 72)        ;; out_slot
          (i32.const 16)        ;; ts_slot
          (i32.const 40)        ;; tz_slot (CEL_STRING wrapper, "+02:00")
          (i32.const 6))        ;; accessor_kind = kHours

    (i32.const 72))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
