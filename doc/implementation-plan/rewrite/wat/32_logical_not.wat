;; CEL source:  !true
;; Decl:        — (no free variables)
;;
;; M5.G (Slice 2) — unary slot-out helper.  ABI mirrors the unary
;; arithmetic helpers (`cel_int_neg_at_v`, etc.):
;; `(out_slot, v_slot) → ()`.  ERROR / UNKNOWN propagate verbatim;
;; non-bool operand → CEL_ERR_TYPE_MISMATCH.
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  rodata: kConst true   {CEL_BOOL,  b=1}
;;   [40, 64)  workspace: kCall(`!_`) result slot (out=40)
;;   [64, mem_size)  bump arena
;;
;; New import this milestone:
;;   cel.cel_not(out_slot, v_slot) — i32×2 → ()
;;
;; cel_not contract (langdef §"Logical operators" / parity with
;;   cel-cpp `runtime/standard/logical_functions.cc::LogicalNot`):
;;     - bool true  → bool false
;;     - bool false → bool true
;;     - ERROR / UNKNOWN → propagate verbatim (24-byte copy).
;;     - Any other kind → CEL_ERROR with CEL_ERR_TYPE_MISMATCH.
;;
;; Expected: out_slot = {CEL_BOOL, b=0}.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_not" (func $cel_not (param i32 i32)))

  (data (i32.const 16)
        "\01\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; out_slot=40, v=16 (true).  cel_not writes CEL_BOOL false.
    (call $cel_not (i32.const 40) (i32.const 16))

    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
