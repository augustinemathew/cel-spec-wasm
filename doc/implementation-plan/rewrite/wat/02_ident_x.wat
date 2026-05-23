;; CEL source:  x
;; Decl:        x : int
;;
;; Memory layout:
;;   [ 0,  8)  reserved null sentinel
;;   [ 8, 12)  legacy cursor slot (arena cursor now in runtime BSS)
;;   [12, 16)  legacy limit slot (arena limit now in runtime BSS)
;;   [16, 40)  workspace slot for `x` — 24-byte CelValue cell.
;;             Host writes `x`'s bound Value into this cell via
;;             Instance::Eval(activation) BEFORE calling $eval.
;;   [40+]  bump arena (malloc'd in heap)
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
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))

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
    (call $arena_reset)

    ;; ── BODY ─────────────────────────────────────────────────
    ;; kIdent lowering: return the offset of x's CelValue.
    (local.get $x_off))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
