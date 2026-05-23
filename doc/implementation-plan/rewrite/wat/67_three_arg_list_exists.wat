;; CEL source:  [10, 20, 30].exists(i, v, v == 20 && i == 1)   → true
;; Decl:        — (no free variables)
;;
;; M5 slice (Slice F — two-iter-var support).  Locks the wasm
;; shape for the **three-arg** comprehension forms (`exists(i, v,
;; p)`, `all(i, v, p)`, `transformList(i, v, t)`, etc.) over a
;; list source.  cel-cpp's `kComprehensionExpr` AST natively
;; carries two iter-var fields (`iter_var` and `iter_var2`); the
;; evaluator dispatches Evaluate1 vs Evaluate2 based on whether
;; `iter_var2` is set.
;;
;; Binding semantics for **list source, two iter_vars** (per
;; m5-comprehensions-followon.md §3.8 and cel-cpp's
;; comprehension_step.cc::Evaluate2):
;;
;;   iter_var  = i  (an int counter — current index, 0-based)
;;   iter_var2 = v  (the per-iter list element, same pointer as
;;                   the single-iter-var case)
;;
;; (For **map source, two iter_vars** the binding is iter_var=key,
;; iter_var2=value — see Slice E + Slice F overlap.  That shape
;; gets its own WAT in a follow-up; this WAT is the list case.)
;;
;; cel-cpp macro → kComprehensionExpr (macros2 cohort,
;; ComprehensionsV2CompilerLibrary):
;;   iter_range  = [10, 20, 30]
;;   iter_var    = i                            ;; index counter
;;   iter_var2   = v                            ;; element pointer
;;   accu_var    = @result
;;   accu_init   = false
;;   loop_cond   = @not_strictly_false(!@result)
;;   loop_step   = @result || (v == 20 && i == 1)
;;   result      = @result
;;
;; ── Key design claims locked here ───────────────────────────
;;
;;   1. **Index counter is a wasm local + a workspace CelValue
;;      slot.**  The local (`$index`) is a fast i32 that's bumped
;;      by 1 per iter.  Once per iter, before lowering loop_cond
;;      and loop_step, we WRITE that int into a CelValue at the
;;      index-workspace slot (`$index_slot`):
;;
;;          {kind=CEL_INT(=2), _pad=0, payload.i=<index>, pad8=0}
;;
;;      The kIdent arm for `i` lowers to `(i32.const <index_slot>)`
;;      exactly as for any other workspace-resolved name.
;;      See m5-comprehensions-followon.md §4.7.
;;
;;   2. **`v` is iter_off (same uniform load as single-var).**
;;      The second iter-var binds to the moving pointer; this is
;;      the same kIdent arm we use for the single-var case
;;      (WAT 60).  Adding the index counter is the only new
;;      surface.
;;
;;   3. **No new runtime helper.**  The index-as-CelValue write is
;;      inline wasm (`i32.store` for the kind tag at offset 0
;;      then `i64.store` for the payload at offset 8) — six
;;      instructions, no call.
;;
;;   4. **Same accu / loop-cond shape as single-var exists.**
;;      Two-iter-var is purely additive to the binding setup;
;;      the rest of the loop body (cel_or, the bool peephole)
;;      is bit-identical to WAT 60.
;;
;; ── No new runtime helpers needed ───────────────────────────
;; This is a frontend / codegen extension, not a runtime one.
;; Every helper called is already exported.  **Runnable today**
;; (once Slice F's `iter_var2` plumbing in ResolvePass /
;; LayoutPass / expr_lower lands).
;;
;; Memory layout:
;;   [ 0, 16)   reserved + arena cursor/limit
;;   [16, 40)   rodata: list elem [0] = {CEL_INT, i=10}
;;   [40, 64)   rodata: list elem [1] = {CEL_INT, i=20}
;;   [64, 88)   rodata: list elem [2] = {CEL_INT, i=30}
;;   [88,112)   rodata: accu_init = {CEL_BOOL, false}
;;   [112,136)  rodata: rhs `20` for `v == 20`
;;   [136,160)  rodata: rhs `1`  for `i == 1`
;;   [160,184)  workspace: iter_range list slot
;;   [184,208)  workspace: accu_slot
;;   [208,232)  workspace: index_slot (i's CelValue — written each iter)
;;   [232,256)  workspace: scratch A — (v == 20) result
;;   [256,280)  workspace: scratch B — (i == 1)  result
;;   [280,304)  workspace: scratch C — (scratchA && scratchB) result
;;   [304, mem_size)  bump arena
;;
;; ── Runtime helpers — all exported today ────────────────────
;;   cel.cel_list_create / cel_list_set         (M4.F)
;;   cel.cel_int_eq_at_vv                       (M5.B)
;;   cel.cel_and                                (M5.G)
;;   cel.cel_or                                 (M5.G)
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_append_at" (func $cel_list_append_at (param i32 i32)))
  (import "cel" "cel_int_eq_at_vv"
          (func $cel_int_eq_at_vv (param i32 i32 i32)))
  (import "cel" "cel_and" (func $cel_and (param i32 i32 i32)))
  (import "cel" "cel_or" (func $cel_or (param i32 i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\0a\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\14\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\1e\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 88)
        "\01\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 112)
        "\02\00\00\00" "\00\00\00\00"
        "\14\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 136)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $list_hdr i32)
    (local $iter_off i32)
    (local $end_off  i32)
    (local $index    i32)    ;; the int counter for iter_var = i

    (call $arena_reset)

    ;; iter_range = [10, 20, 30].
    (call $cel_list_create (i32.const 160) (i32.const 3))
    (call $cel_list_append_at (i32.const 160) (i32.const 16))
    (call $cel_list_append_at (i32.const 160) (i32.const 40))
    (call $cel_list_append_at (i32.const 160) (i32.const 64))

    ;; accu_slot ← false.
    (i32.store offset=0  (i32.const 184) (i32.load offset=0  (i32.const 88)))
    (i32.store offset=4  (i32.const 184) (i32.load offset=4  (i32.const 88)))
    (i32.store offset=8  (i32.const 184) (i32.load offset=8  (i32.const 88)))
    (i32.store offset=12 (i32.const 184) (i32.load offset=12 (i32.const 88)))
    (i32.store offset=16 (i32.const 184) (i32.load offset=16 (i32.const 88)))
    (i32.store offset=20 (i32.const 184) (i32.load offset=20 (i32.const 88)))

    ;; Init the index counter at 0.  Initial index_slot kind tag.
    (local.set $index (i32.const 0))
    ;; Pre-write the index_slot kind = CEL_INT(2) once; the
    ;; payload portion gets updated per iter.  Tail bytes stay 0
    ;; (we zero the i64 payload at the same time each iter for
    ;; consistency).
    (i32.store offset=0 (i32.const 208) (i32.const 2))
    (i32.store offset=4 (i32.const 208) (i32.const 0))

    (local.set $list_hdr (i32.load offset=8 (i32.const 160)))
    (local.set $iter_off (i32.load offset=8 (local.get $list_hdr)))
    (local.set $end_off  (i32.add (local.get $iter_off) (i32.const 72)))

    (block $exit
      (loop $continue
        (br_if $exit
               (i32.ge_u (local.get $iter_off) (local.get $end_off)))
        (br_if $exit (i32.load offset=8 (i32.const 184)))

        ;; Write the current index into the index_slot's payload.
        ;; payload.i is an i64 at offset 8; the upper 4 bytes stay 0.
        (i64.store offset=8 (i32.const 208)
                   (i64.extend_i32_u (local.get $index)))

        ;; loop_step: @result || ((v == 20) && (i == 1))
        ;;   scratchA = v == 20
        (call $cel_int_eq_at_vv
              (i32.const 232)
              (local.get $iter_off)             ;; v IS iter_off
              (i32.const 112))
        ;;   scratchB = i == 1
        (call $cel_int_eq_at_vv
              (i32.const 256)
              (i32.const 208)                   ;; i IS index_slot
              (i32.const 136))
        ;;   scratchC = scratchA && scratchB
        (call $cel_and
              (i32.const 280) (i32.const 232) (i32.const 256))
        ;;   accu = accu || scratchC
        (call $cel_or
              (i32.const 184) (i32.const 184) (i32.const 280))

        ;; Bump both: iter_off += 24; index += 1.
        (local.set $iter_off
                   (i32.add (local.get $iter_off) (i32.const 24)))
        (local.set $index
                   (i32.add (local.get $index) (i32.const 1)))
        (br $continue)))

    (i32.const 184))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
