;; CEL source:     [1, 2, 3].exists(x, x > 0)
;; cel-cpp macro → kComprehensionExpr:
;;   iter_range  = [1, 2, 3]            (kCreateList)
;;   iter_var    = x
;;   accu_var    = @result
;;   accu_init   = false
;;   loop_cond   = @not_strictly_false(!@result)  ;; stop when true
;;   loop_step   = @result || (x > 0)
;;   result      = @result
;;
;; ── Key design claim ────────────────────────────────────────
;; The SAME kIdent lowering that serves top-level free variables
;; (local.get) serves iter_var and accu_var inside a comprehension.
;; Only the set-site changes:
;;   - free var       : prelude   sets local = <fixed workspace slot>
;;   - iter var (x)   : loop hdr  sets local = <base + N * 24>
;;                                  (in-place pointer into list payload)
;;   - accu var       : init/step set local = <accu slot or fresh CelValue>
;;
;; Memory layout sketch (rough; actual offsets computed by LayoutPass):
;;   [ 0, 16)   reserved + arena cursor/limit
;;   [16, 88)   rodata: three int consts {1, 2, 3}  — 3×24 = 72 bytes
;;   [88, 112)  rodata: bool const {false}          — accu_init
;;   [112, 136) rodata: int const {0}               — rhs of `x > 0`
;;   [136, 160) workspace slot for the list CelValue (header only)
;;   [160, 184) workspace slot for the accu CelValue
;;   [184, 208) workspace slot for the loop_step output
;;   [208, mem_size)  bump arena
;;
;; Imports we'd need at M3 (arith) + M6 (list build) once those land:
;;   cel.cel_int_gt_at_vv(out, a, b)   — `x > 0`
;;   cel.cel_or_at_vv(out, a, b)       — `||`
;;   cel.cel_list_build_i(out, …)      — or rodata-packed list literal
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  ;; Stubs — declared so the WAT validates; real impls ship in M3/M6.
  (import "cel" "cel_int_gt_at_vv"
          (func $cel_int_gt_at_vv (param i32 i32 i32)))
  (import "cel" "cel_or_at_vv"
          (func $cel_or_at_vv (param i32 i32 i32)))

  ;; Three int constants at [16, 88).  Each CelValue is 24 bytes:
  ;;   kind=CEL_INT(=2), _pad=0, payload.i=<value>, pad8.
  (data (i32.const 16)
        ;; {CEL_INT, 1}
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00"
        ;; {CEL_INT, 2}
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00"
        ;; {CEL_INT, 3}
        "\02\00\00\00" "\00\00\00\00"
        "\03\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; accu_init = false at offset 88.
  (data (i32.const 88)
        "\01\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; rhs of `x > 0` = 0 at offset 112.
  (data (i32.const 112)
        "\02\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    ;; LOCAL CATALOGUE ────────────────────────────────────────
    (local $iter_off  i32)  ;; = $x_off under M2's uniform rule
    (local $accu_off  i32)
    (local $end_off   i32)  ;; one-past-end of iter_range payload
    (local $step_out  i32)  ;; loop_step output slot

    ;; ── RESET ────────────────────────────────────────────────
    (call $arena_reset)

    ;; ── COMPREHENSION SETUP ─────────────────────────────────
    ;; iter starts at first element's CelValue address (offset 16).
    (local.set $iter_off (i32.const 16))
    ;; end = iter_off + (list_len * 24) = 16 + 72.
    (local.set $end_off  (i32.const 88))
    ;; accu <- accu_init (points at the `false` const in rodata).
    (local.set $accu_off (i32.const 88))
    (local.set $step_out (i32.const 184))  ;; per-iter scratch slot

    ;; ── LOOP ────────────────────────────────────────────────
    (block $exit
      (loop $continue
        ;; Iteration exit #1: iter >= end.
        (br_if $exit
               (i32.ge_u (local.get $iter_off) (local.get $end_off)))

        ;; Iteration exit #2: loop_cond says we're done.  For
        ;; `exists`, cel-cpp lowers this to
        ;; `!@not_strictly_false(@result)` — exit once accu is
        ;; already true.  Read the bool byte at accu_off + 8
        ;; (payload.b lives at the i32 offset 8 inside a CelValue).
        (br_if $exit (i32.load offset=8 (local.get $accu_off)))

        ;; loop_step: accu = accu || (x > 0)
        ;;
        ;;   (x > 0)  -> cel_int_gt_at_vv(step_out, iter, rhs=112)
        (call $cel_int_gt_at_vv
              (local.get $step_out)
              (local.get $iter_off)
              (i32.const 112))
        ;;   accu = accu || step_out
        (call $cel_or_at_vv
              (i32.const 160)            ;; accu slot
              (local.get $accu_off)
              (local.get $step_out))
        (local.set $accu_off (i32.const 160))

        ;; Advance iter by sizeof(CelValue) = 24.
        (local.set $iter_off
                   (i32.add (local.get $iter_off) (i32.const 24)))
        (br $continue)))

    ;; result = accu.
    (local.get $accu_off))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
