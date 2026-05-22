;; CEL source:  duration("3600s") + duration("60s")
;;              timestamp("2009-02-13T23:31:30Z") -
;;                  timestamp("2009-02-13T23:31:29Z")
;; Decl:        — (no free variables)
;;
;; M7B.B slice — pure-wasm duration/timestamp arithmetic kernels.
;; Per `m7b-duration-timestamp.md` §4.3 (Option C), the 6 arithmetic
;; helpers (`cel_dur_add_at_vv`, `cel_dur_sub_at_vv`,
;; `cel_ts_dur_add_at_vv`, `cel_dur_ts_add_at_vv`,
;; `cel_ts_dur_sub_at_vv`, `cel_ts_ts_sub_at_vv`) plus the 8 ordering
;; helpers (`cel_dur_lt/le/gt/ge_at_vv` + same for ts) stay in pure
;; wasm because all they need is `__builtin_add_overflow` on the
;; seconds + nanos-carry — no library dependency.  Equality routes
;; through the existing `cel_equals_at_vv` once M7B.A wires the
;; CEL_DURATION / CEL_TIMESTAMP arms.
;;
;; This WAT pins the slot-out ABI byte-for-byte against the existing
;; `cel_int_add_at_vv` shape (see `16_arith_int_add.wat`).  Two kernels
;; are traced together to lock both the (dur, dur) → dur and the
;; (ts, ts) → dur signatures in one place — they are the two
;; "interesting" arithmetic shapes (the other 4 helpers are
;; structurally identical with the kind tag swapped between
;; CEL_DURATION(12) and CEL_TIMESTAMP(13)).
;;
;; Memory layout:
;;   [ 0,  16)  reserved + arena cursor/limit
;;   [16,  40)  rodata: kConst duration("3600s") → CelValue{
;;                  kind=CEL_DURATION(12), _pad,
;;                  payload.dur={seconds=3600, nanos=0, _pad=0}}
;;   [40,  64)  rodata: kConst duration("60s") → CelValue{
;;                  kind=CEL_DURATION(12), payload.dur={seconds=60, ...}}
;;   [64,  88)  workspace: kCall(`_+_`) result slot for dur add (out=64)
;;                          expected: {CEL_DURATION, dur={seconds=3660}}
;;   [88, 112)  rodata: kConst timestamp("2009-02-13T23:31:30Z") →
;;                  CelValue{kind=CEL_TIMESTAMP(13),
;;                  payload.ts={seconds=1234567890, nanos=0, _pad=0}}
;;   [112, 136) rodata: kConst timestamp("2009-02-13T23:31:29Z") →
;;                  CelValue{kind=CEL_TIMESTAMP(13),
;;                  payload.ts={seconds=1234567889, nanos=0, _pad=0}}
;;   [136, 160) workspace: kCall(`_-_`) result slot for ts sub (out=136)
;;                          expected: {CEL_DURATION, dur={seconds=1}}
;;   [160, mem_size)  bump arena
;;
;; New imports this slice:
;;   cel.cel_dur_add_at_vv(out_slot, a_slot, b_slot) — i32×3 → ()
;;   cel.cel_ts_ts_sub_at_vv(out_slot, a_slot, b_slot) — i32×3 → ()
;;
;; cel_dur_add_at_vv contract (helper body in
;; `compiler_v2/runtime/cel_time.c`):
;;   - reads a / b as CEL_DURATION.  Any other kind on either operand
;;     → out_slot = {CEL_ERROR, CEL_ERR_TYPE_MISMATCH}.
;;   - 3VL absorption — CEL_UNKNOWN/CEL_ERROR pass through (mirrors
;;     `cel_int_add_at_vv` per §16's contract).
;;   - signed overflow on seconds OR nanos carry → out_slot =
;;     {CEL_ERROR, CEL_ERR_OVERFLOW}.  Detected via
;;     `__builtin_add_overflow` on the seconds i64 lane after the
;;     nanos carry is normalised into [0, 1_000_000_000).  Per
;;     langdef §"Timestamps and Durations": overflow is an ERROR,
;;     NOT wrap.
;;   - happy path: out_slot = {CEL_DURATION,
;;     payload.dur={a.seconds + b.seconds + nanos_carry,
;;                  (a.nanos + b.nanos) mod 1e9, _pad=0}}.
;;
;; cel_ts_ts_sub_at_vv contract: same shape but reads CEL_TIMESTAMP
;; operands and writes a CEL_DURATION result (langdef:
;; `timestamp - timestamp → duration`).  Overflow ladder runs on the
;; subtracted seconds + nanos-borrow.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_dur_add_at_vv"
          (func $cel_dur_add_at_vv (param i32 i32 i32)))
  (import "cel" "cel_ts_ts_sub_at_vv"
          (func $cel_ts_ts_sub_at_vv (param i32 i32 i32)))

  ;; rodata @ 16: CEL_DURATION(12) duration("3600s") = {seconds=3600, nanos=0}.
  ;; 3600 = 0x0E10  → little-endian int64 = 10 0E 00 00 00 00 00 00
  (data (i32.const 16)
        "\0c\00\00\00" "\00\00\00\00"
        "\10\0e\00\00\00\00\00\00"
        "\00\00\00\00" "\00\00\00\00")

  ;; rodata @ 40: CEL_DURATION duration("60s") = {seconds=60, nanos=0}.
  ;; 60 = 0x3C  → little-endian int64 = 3C 00 00 00 00 00 00 00
  (data (i32.const 40)
        "\0c\00\00\00" "\00\00\00\00"
        "\3c\00\00\00\00\00\00\00"
        "\00\00\00\00" "\00\00\00\00")

  ;; rodata @ 88: CEL_TIMESTAMP(13) "2009-02-13T23:31:30Z" =
  ;;     {seconds=1234567890, nanos=0}.
  ;; 1234567890 = 0x499602D2  → little-endian int64 =
  ;;     D2 02 96 49 00 00 00 00
  (data (i32.const 88)
        "\0d\00\00\00" "\00\00\00\00"
        "\d2\02\96\49\00\00\00\00"
        "\00\00\00\00" "\00\00\00\00")

  ;; rodata @ 112: CEL_TIMESTAMP "2009-02-13T23:31:29Z" =
  ;;     {seconds=1234567889, nanos=0}.
  ;; 1234567889 = 0x499602D1
  (data (i32.const 112)
        "\0d\00\00\00" "\00\00\00\00"
        "\d1\02\96\49\00\00\00\00"
        "\00\00\00\00" "\00\00\00\00")

  (func $eval (result i32)
    ;; Arena starts at 160 (past both result slots, 16-byte aligned).
    (call $arena_reset)

    ;; dur add: out=64, a=16, b=40 → {CEL_DURATION, seconds=3660}.
    (call $cel_dur_add_at_vv
          (i32.const 64) (i32.const 16) (i32.const 40))

    ;; ts sub: out=136, a=88, b=112 → {CEL_DURATION, seconds=1}.
    (call $cel_ts_ts_sub_at_vv
          (i32.const 136) (i32.const 88) (i32.const 112))

    ;; Return the dur-add slot.  The ts-sub result is observable at
    ;; slot 136 in the post-eval memory snapshot.
    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
