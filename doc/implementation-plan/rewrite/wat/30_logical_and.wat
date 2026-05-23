;; CEL source:  true && false
;; Decl:        — (no free variables)
;;
;; M5.G (Slice 2) — locks the slot-out shape for `_&&_`.  Both
;; operands are eagerly evaluated into their own slots; the helper
;; then runs the 3VL truth table and writes the result into
;; `out_slot`.  No short-circuit branching at the wasm level —
;; non-strict semantics force full eval (`false && (1/0)` must
;; succeed at `false`, not propagate the divide-by-zero), and
;; the truth table itself runs entirely inside `cel_and`.
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  rodata: kConst true   {CEL_BOOL,  b=1}
;;   [40, 64)  rodata: kConst false  {CEL_BOOL,  b=0}
;;   [64, 88)  workspace: kCall(`_&&_`) result slot (out=64)
;;   [88, mem_size)  bump arena
;;
;; New import this milestone:
;;   cel.cel_and(out_slot, a_slot, b_slot) — i32×3 → ()
;;
;; cel_and contract (langdef §"Logical operators" / parity with
;;   cel-cpp `runtime/standard/logical_functions.cc::LogicalAnd`):
;;     - OK(false) on EITHER side absorbs everything (including
;;       a non-3VL other operand) — `false && X = false`.
;;     - Past the absorber: any non-3VL operand → CEL_ERROR
;;       with code CEL_ERR_TYPE_MISMATCH.
;;     - OK(true) && X = X (with X ∈ {bool, error, unknown}).
;;     - ERROR > UNKNOWN dominance.
;;     - Both UNKNOWN → sorted-deduplicated union of attribute-id
;;       sets via `cel_unknown_merge`.
;;
;; Expected: out_slot = {CEL_BOOL, b=0}.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_and" (func $cel_and (param i32 i32 i32)))

  ;; bool(true) at 16: kind=CEL_BOOL=1, _pad=0, b=1, rest zero.
  (data (i32.const 16)
        "\01\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; bool(false) at 40: kind=CEL_BOOL=1, _pad=0, b=0, rest zero.
  (data (i32.const 40)
        "\01\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; out_slot=64, a=16 (true), b=40 (false).  cel_and writes
    ;; CEL_BOOL false into slot 64 via the OK(false) absorber.
    (call $cel_and (i32.const 64) (i32.const 16) (i32.const 40))

    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
