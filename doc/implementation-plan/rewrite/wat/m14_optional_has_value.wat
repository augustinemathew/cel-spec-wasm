;; CEL source:  optional.of(1).hasValue()                       → true
;; Decl:        — (no free variables)
;;
;; M14 Slice 0 — locks two ABI surfaces:
;;
;;   1. The present-flag read on an OptionalCell:
;;      cell[0..4] is the present u32 (0=None, 1=Some).  The
;;      `cel_optional_has_value_at_v` kernel reads this u32 and
;;      materialises a CEL_BOOL CelValue.
;;
;;   2. The receiver-form kCall ABI for member overloads on
;;      optional<T>.  Per probe Q12 (ast_shape_probe_test.cc), the
;;      checker produces:
;;          kCallExpr {
;;            function: "hasValue"
;;            target:   <the optional operand>   // NOT args[0]
;;            args:     []
;;          }
;;      Receiver-flattening — target → args[0] — happens at codegen
;;      time, mirroring M5.F's `EmitGeneralCall` arm for
;;      `s.contains(sub)`.  The kernel signature is the post-flatten
;;      shape: (out_slot, opt_slot) — a regular 1-arg out-slot kernel.
;;
;; ── New runtime kernel this slice locks ────────────────────
;;
;;   cel.cel_optional_has_value_at_v(out_slot, opt_slot) → ()
;;
;; Contract:
;;   1. Read opt = *opt_slot.
;;   2. If opt.kind != CEL_OPTIONAL ⇒
;;        out_slot = {CEL_ERROR, err=CEL_ERR_TYPE_MISMATCH}.
;;      (3VL absorption: CEL_UNKNOWN/CEL_ERROR operands propagate.)
;;   3. cell = cel_value_at(opt.payload.opt)  // see cel_arena.h
;;   4. present = cell.present (u32 at byte 0 of the cell)
;;   5. out_slot.kind        = CEL_BOOL (=1)
;;   6. out_slot._pad        = 0
;;   7. out_slot.payload.b   = (present != 0) ? 1 : 0
;;   8. out_slot remaining union bytes = 0
;;
;; Memory layout:
;;   [ 0, 16)   reserved null + arena scaffolding
;;   [16, 40)   rodata: kConst `1`  {kind=CEL_INT, i=1}
;;   [40, 64)   workspace: kCall(`optional.of`) out_slot — optional<int>
;;   [64, 88)   workspace: kCall(`hasValue`)    out_slot — bool
;;   [88, mem_size)  bump arena — 32-byte OptionalCell allocated by
;;                   `cel_optional_of_at_v` lives here at [88, 120).
;;                     [ 88,  92)  cell.present = 1
;;                     [ 92,  96)  cell._pad    = 0
;;                     [ 96, 120)  cell.inner   = {CEL_INT, i=1}
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_optional_of_at_v"
          (func $cel_optional_of_at_v (param i32 i32)))
  (import "cel" "cel_optional_has_value_at_v"
          (func $cel_optional_has_value_at_v (param i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\01\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)
    ;; step 1: `optional.of(1)` → workspace at 40
    (call $cel_optional_of_at_v (i32.const 40) (i32.const 16))
    ;; step 2: `.hasValue()` — receiver-form kCall.
    ;;   Pre-receiver-flatten AST: target = <optional at 40>, args = []
    ;;   Post-receiver-flatten ABI: (out_slot, opt_slot) = (64, 40)
    (call $cel_optional_has_value_at_v (i32.const 64) (i32.const 40))
    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
