;; CEL source:  true ? 1 : 2
;; Decl:        — (no free variables)
;;
;; M5.G (Slice 2) — locks the BinaryenIf-based shape for `_?_:_`.
;; Unlike the eager-eval helpers for `_&&_` / `_||_`, ternary
;; lowers to **inline branching wasm**: only the selected arm is
;; evaluated, side effects on the dropped arm are skipped, and the
;; result is materialised into `out_slot` via `cel_copy_slot`.
;; This is correct under langdef §"Conditional expression":
;;   "If c is an error or unknown, the result is c.  Otherwise,
;;    only the chosen branch is evaluated."
;;
;; Branch shape (codegen target):
;;   if (cond.kind == CEL_BOOL) {
;;     if (cond.payload.b != 0) {
;;       <eval then-branch into then_slot>
;;       cel_copy_slot(out, then_slot);
;;     } else {
;;       <eval else-branch into else_slot>
;;       cel_copy_slot(out, else_slot);
;;     }
;;   } else {
;;     // ERROR / UNKNOWN propagate verbatim.
;;     cel_copy_slot(out, cond_slot);
;;   }
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  rodata: cond  bool(true)   {CEL_BOOL, b=1}
;;   [40, 64)  rodata: then  int(1)       {CEL_INT, i=1}
;;   [64, 88)  rodata: else  int(2)       {CEL_INT, i=2}
;;   [88,112)  workspace: out slot for `_?_:_` (out=88)
;;   [112, mem_size)  bump arena
;;
;; New import this milestone:
;;   cel.cel_copy_slot(dst_slot, src_slot) — i32×2 → ()
;;
;; CelValue layout (cel_data.h):
;;   off 0..3   kind (uint32)
;;   off 4..7   _pad
;;   off 8..23  payload (16 bytes; b at off 8 for CEL_BOOL).
;;
;; CEL_BOOL = 1, CEL_UNKNOWN = 15, CEL_ERROR = 16.
;;
;; Expected: out_slot = {CEL_INT, i=1} (cond is true → then arm).
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "cel_copy_slot" (func $cel_copy_slot (param i32 i32)))

  (data (i32.const 16)
        "\01\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; cond.kind == CEL_BOOL ?
    (if (i32.eq (i32.load (i32.const 16))
                (i32.const 1))
      (then
        ;; cond.payload.b != 0 ?
        (if (i32.ne (i32.load offset=8 (i32.const 16))
                    (i32.const 0))
          (then
            ;; then arm: copy rodata int(1) into out.
            (call $cel_copy_slot (i32.const 88) (i32.const 40)))
          (else
            ;; else arm: copy rodata int(2) into out.
            (call $cel_copy_slot (i32.const 88) (i32.const 64)))))
      (else
        ;; cond is ERROR / UNKNOWN → propagate verbatim.
        (call $cel_copy_slot (i32.const 88) (i32.const 16))))

    (i32.const 88))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
