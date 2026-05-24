;; CEL source:  1 == 2
;; Decl:        — (no free variables)
;;
;; M5.B slice — companion to 16, locks the slot-out comparison
;; helper ABI.  Same `(out_slot, a_slot, b_slot) → ()` shape as the
;; arithmetic helpers; only the result kind differs (CEL_BOOL
;; instead of CEL_INT).
;;
;; Memory layout (identical to 16 — comparison and arithmetic
;; helpers share the slot-out shape exactly):
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  rodata: kConst 1     {CEL_INT, i=1}
;;   [40, 64)  rodata: kConst 2     {CEL_INT, i=2}
;;   [64, 88)  workspace: kCall(`_==_`) result slot (out=64)
;;   [88+]  bump arena (malloc'd in heap)
;;
;; New import this milestone:
;;   cel.cel_int_eq_at_vv(out_slot, a_slot, b_slot) — i32×3 → ()
;;
;; cel_int_eq_at_vv contract (cel-cpp parity:
;;   third_party/cel-cpp/runtime/standard/equality_functions.cc::Equal):
;;     - reads a / b as CEL_INT.  Any other kind on either operand →
;;       out_slot = {CEL_ERROR, err=CEL_ERR_TYPE_MISMATCH}.  Cross-
;;       type numeric equality (1 == 1u, 1 == 1.0) is dispatched to
;;       the cross-type ladder helpers (`cel_numeric_eq_at_vv`); this
;;       helper assumes same-kind operands.
;;     - 3VL absorption — UNKNOWN/ERROR propagation matches the
;;       arith helpers.
;;     - happy path: out_slot = {CEL_BOOL, _pad=0, b=(a==b ? 1 : 0)}.
;;
;; The kCall arm in `expr_lower.cc::EmitGeneralCall` looks up
;; `equals_int64` in the OverloadTable and emits exactly this call.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_int_eq_at_vv"
          (func $cel_int_eq_at_vv (param i32 i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; out_slot=64, a=16, b=40.  Helper reads two CEL_INT frames out
    ;; of memory, writes a CEL_BOOL CelValue (false here, since
    ;; 1 != 2) into slot 64.
    (call $cel_int_eq_at_vv
          (i32.const 64) (i32.const 16) (i32.const 40))

    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
