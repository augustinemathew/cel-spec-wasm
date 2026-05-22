;; CEL source:  1 + 2
;; Decl:        — (no free variables)
;;
;; M5.B slice — first WAT for the slot-out helper ABI added by
;; the general kCall arm (`design.md §4.2`, `m5-kcall-comprehensions.md
;; §2.1`).  Every M5 helper has the same wasm signature:
;;
;;     (i32 out_slot, i32 arg0, ..., i32 argN-1) -> void
;;
;; The helper reads operand CelValues out of `arg*_slot`s, computes
;; the result, writes a fresh CelValue into `out_slot`.  No return
;; values; the caller already knows the result lives at `out_slot`.
;; Mirrors the M3/M4 `cel_*_arena` shape so M5's generated wasm
;; looks identical to M3/M4 except for the helper name.
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  rodata: kConst 1     {CEL_INT, i=1}
;;   [40, 64)  rodata: kConst 2     {CEL_INT, i=2}
;;   [64, 88)  workspace: kCall(`_+_`) result slot (out=64)
;;   [88, mem_size)  bump arena
;;
;; New import this milestone:
;;   cel.cel_int_add_at_vv(out_slot, a_slot, b_slot) — i32×3 → ()
;;
;; cel_int_add_at_vv contract (cel-cpp parity:
;;   third_party/cel-cpp/runtime/standard/arithmetic_functions.cc::Add):
;;     - reads a / b as CEL_INT.  Any other kind on either operand →
;;       out_slot = {CEL_ERROR, err=CEL_ERR_TYPE_MISMATCH}.
;;     - 3VL absorption — if a.kind or b.kind is CEL_UNKNOWN/CEL_ERROR,
;;       out_slot inherits that kind verbatim (cel_unknown_merge for
;;       UNKNOWN+UNKNOWN; left-bias for ERROR otherwise).  Mirrors
;;       the v1 M4 Slice A 3VL helpers.
;;     - signed overflow → out_slot = {CEL_ERROR, err=CEL_ERR_OVERFLOW}.
;;       Per langdef §"Numeric values": int overflow is an ERROR
;;       (NOT wrap).  Detected via __builtin_add_overflow on
;;       i64 lanes; cel-cpp's helper does the same.
;;     - happy path: out_slot = {CEL_INT, _pad=0, i=a+b, slack=0}.
;;
;; The kCall arm in `expr_lower.cc::EmitGeneralCall` looks up
;; `add_int64` in the OverloadTable, finds `cel.cel_int_add_at_vv`
;; (module=cel, name=cel_int_add_at_vv), and emits exactly the
;; (i32.const 64) (i32.const 16) (i32.const 40) call below.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_int_add_at_vv"
          (func $cel_int_add_at_vv (param i32 i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; out_slot=64, a=16, b=40.  Helper reads two CEL_INT frames out
    ;; of memory, writes the sum CelValue into slot 64.
    (call $cel_int_add_at_vv
          (i32.const 64) (i32.const 16) (i32.const 40))

    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
