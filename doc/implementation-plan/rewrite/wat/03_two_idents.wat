;; CEL source:  x + y
;; Decls:       x : int, y : int
;;
;; At M2 the kCall (_+_) arm isn't implemented yet — expr_lower
;; returns Unimplemented.  This WAT stubs the kCall body with
;; `unreachable` so we can still validate the ident plumbing end
;; to end.  The prelude + local declarations are exactly what M3
;; will build on when the arithmetic arm lands.
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  workspace slot for x — local_index 0
;;   [40, 64)  workspace slot for y — local_index 1
;;   [64, mem_size)  bump arena
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))

  (func $eval (result i32)
    (local $x_off i32)  ;; local 0
    (local $y_off i32)  ;; local 1

    ;; ── PRELUDE ──────────────────────────────────────────────
    ;; One local.set per ResolvedVariable, in local_index order.
    (local.set $x_off (i32.const 16))
    (local.set $y_off (i32.const 40))

    ;; ── RESET ────────────────────────────────────────────────
    (call $arena_reset)

    ;; ── BODY ─────────────────────────────────────────────────
    ;; M3 will emit something like:
    ;;   (call $cel_int_add_at_vv
    ;;         (i32.const <out_slot>)
    ;;         (local.get $x_off)
    ;;         (local.get $y_off))
    ;;   (i32.const <out_slot>)
    ;; At M2 the kCall arm fails Unimplemented — stub with
    ;; unreachable so the module still validates.
    unreachable)

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
