;; CEL source:  false || true
;; Decl:        — (no free variables)
;;
;; M5.G (Slice 2) — symmetric companion to 30.  `_||_` mirrors
;; `_&&_` with the OK(false) / OK(true) absorbers swapped:
;; `true || X = true (any X)`; `false || X = X` (with the same
;; downstream type-check + ERROR/UNKNOWN dominance).  Eager
;; evaluation matches `_&&_` — non-strict semantics for the
;; non-bool sides come from langdef.
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  rodata: kConst false  {CEL_BOOL,  b=0}
;;   [40, 64)  rodata: kConst true   {CEL_BOOL,  b=1}
;;   [64, 88)  workspace: kCall(`_||_`) result slot (out=64)
;;   [88, mem_size)  bump arena
;;
;; New import this milestone:
;;   cel.cel_or(out_slot, a_slot, b_slot) — i32×3 → ()
;;
;; cel_or contract (langdef §"Logical operators" / parity with
;;   cel-cpp `runtime/standard/logical_functions.cc::LogicalOr`):
;;     - OK(true) on EITHER side absorbs everything → CEL_BOOL true.
;;     - Past the absorber: any non-3VL operand → CEL_ERROR
;;       with code CEL_ERR_TYPE_MISMATCH.
;;     - OK(false) || X = X.
;;     - ERROR > UNKNOWN dominance; both UNKNOWN merge.
;;
;; Expected: out_slot = {CEL_BOOL, b=1}.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_or" (func $cel_or (param i32 i32 i32)))

  (data (i32.const 16)
        "\01\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\01\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; out_slot=64, a=16 (false), b=40 (true).  OK(true) absorber
    ;; writes CEL_BOOL true into slot 64.
    (call $cel_or (i32.const 64) (i32.const 16) (i32.const 40))

    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
