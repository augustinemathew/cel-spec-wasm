;; CEL source:  x
;; Decl:        x : int
;;
;; Memory layout:
;;   [ 0,  8)  reserved null sentinel
;;   [ 8, 12)  arena cursor (u32)  — written by cel_reset
;;   [12, 16)  arena limit  (u32)  — written by cel_reset
;;   [16, 40)  workspace slot for `x` — 24-byte CelValue cell.
;;             Host writes `x`'s bound Value into this cell via
;;             Instance::Eval(activation) BEFORE calling $eval.
;;   [40, mem_size)  bump arena
;;
;; rodata is empty (no literals in this expression).
;;
;; Wasm locals on $eval:
;;   local 0 (i32) — the current offset of x's CelValue.  Held in
;;                   a local (not hardcoded as a constant) so the
;;                   same codegen scheme works for comprehension
;;                   iter/accu vars, whose offset changes per
;;                   iteration (M5 extends exactly this path).
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))

  ;; No `(data ...)` — rodata is empty for this expression.

  (func $eval (result i32)
    (local $x_off i32)  ;; one wasm local per referenced variable

    ;; ── PRELUDE ──────────────────────────────────────────────
    ;; For each referenced variable, write its fixed workspace
    ;; slot offset into its local.  For free variables this is a
    ;; compile-time constant; for comprehension iter vars (M5)
    ;; the loop header will overwrite it per iteration.
    (local.set $x_off (i32.const 16))

    ;; ── RESET ────────────────────────────────────────────────
    (call $cel_reset (i32.const 40) (i32.const 131072))

    ;; ── BODY ─────────────────────────────────────────────────
    ;; kIdent lowering: return the offset of x's CelValue.
    (local.get $x_off))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
