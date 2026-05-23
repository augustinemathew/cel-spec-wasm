;; CEL source:  "ab" + "cd"
;; Decl:        — (no free variables)
;;
;; M5.C slice — locks the slot-out helper ABI for string ops where
;; the helper must allocate output bytes in the arena.  Every
;; helper this milestone follows the same `(out_slot, args...) -> ()`
;; shape as M5.B's arith helpers (WAT 16); the difference is that
;; concat extends a CelSpan to point at fresh arena bytes rather
;; than rewriting a fixed-size scalar payload.
;;
;; Memory layout:
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  rodata: kConst "ab"  {CEL_STRING, payload.s={ptr=64, len=2}}
;;   [40, 64)  rodata: kConst "cd"  {CEL_STRING, payload.s={ptr=66, len=2}}
;;   [64, 66)  rodata: payload "ab"
;;   [66, 68)  rodata: payload "cd"
;;   [68, 72)  padding to 8-align workspace
;;   [72, 96)  workspace: kCall(`_+_`) result slot (out=72)
;;   [96+]  bump arena (malloc'd in heap) (concat target lives here)
;;
;; New import this milestone:
;;   cel.cel_string_concat_at_vv(out_slot, a_slot, b_slot) — i32×3 → ()
;;
;; cel_string_concat_at_vv contract (cel-cpp parity:
;;   third_party/cel-cpp/runtime/standard/string_functions.cc::ConcatString):
;;     - reads a / b as CEL_STRING.  Wrong kind on either operand →
;;       out_slot = {CEL_ERROR, err=CEL_ERR_TYPE_MISMATCH}.
;;     - 3VL absorption — UNKNOWN/ERROR propagates verbatim.
;;     - allocates `a.len + b.len` bytes in the arena via
;;       arena_alloc.  OOM → out_slot = {CEL_ERROR, err=CEL_ERR_OVERFLOW}.
;;     - copies a.bytes then b.bytes into the new buffer.
;;     - writes out_slot = {CEL_STRING, payload.s={ptr=<new>, len=a.len+b.len}}.
;;
;; The new payload lives in the bump arena, so it survives until
;; arena_reset is next called (top of next $eval).  This matches the
;; existing string-handling pattern from M1: every dynamically-
;; produced string payload is owned by the arena that the next
;; arena_reset rewinds.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_string_concat_at_vv"
          (func $cel_string_concat_at_vv (param i32 i32 i32)))

  ;; CelValue{kind=CEL_STRING(5), payload.s={ptr=64, len=2}}
  (data (i32.const 16)
        "\05\00\00\00" "\00\00\00\00"
        "\40\00\00\00" "\02\00\00\00"
        "\00\00\00\00" "\00\00\00\00")
  ;; CelValue{kind=CEL_STRING(5), payload.s={ptr=66, len=2}}
  (data (i32.const 40)
        "\05\00\00\00" "\00\00\00\00"
        "\42\00\00\00" "\02\00\00\00"
        "\00\00\00\00" "\00\00\00\00")
  ;; Payload bytes for "ab" + "cd" packed back-to-back.
  (data (i32.const 64) "abcd")

  (func $eval (result i32)
    (call $arena_reset)

    ;; out_slot=72, a=16, b=40.  Helper allocates 4 bytes in the
    ;; arena, copies "abcd" into them, writes
    ;; {CEL_STRING, payload.s={ptr=<new>, len=4}} to slot 72.
    (call $cel_string_concat_at_vv
          (i32.const 72) (i32.const 16) (i32.const 40))

    (i32.const 72))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
