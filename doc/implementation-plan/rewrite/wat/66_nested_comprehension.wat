;; CEL source:  [1].exists(y, [0].exists(y, y == 0))     → true
;; Decl:        — (no free variables)
;;
;; M5 slice (Slices A–C closeout; nested-scope validation).
;; Locks the wasm shape codegen emits for **nested** comprehensions
;; with name shadowing: the outer `y` (binding the elements of
;; `[1]`) and the inner `y` (binding the elements of `[0]`) live
;; in *different* wasm locals.  When the inner body's kIdent arm
;; resolves `y`, ScopeResolver walks the scope stack
;; inner-to-outer and finds the inner binding first; the outer
;; `y` is shadowed for the duration of the inner comprehension's
;; body, then becomes visible again after the inner exits.
;;
;; cel-cpp expansion (two stacked kComprehensionExprs):
;;
;;   outer = kComprehensionExpr {
;;     iter_range  = [1]
;;     iter_var    = y         ;; outer-y
;;     accu_var    = @result
;;     accu_init   = false
;;     loop_step   = @result || INNER
;;     ...
;;   }
;;   INNER = kComprehensionExpr {
;;     iter_range  = [0]
;;     iter_var    = y         ;; inner-y SHADOWS outer-y
;;     accu_var    = @result  ;; SHADOWS outer @result too
;;     accu_init   = false
;;     loop_step   = @result || (y == 0)
;;     ...
;;   }
;;
;; ── Key design claims locked here ───────────────────────────
;;
;;   1. **Independent wasm locals per nesting level.**  Outer's
;;      `$iter_off_o` and inner's `$iter_off_i` are different
;;      locals.  LayoutPass allocates them disjointly per
;;      ComprehensionFrame; the inner frame's free-cursor-at-entry
;;      snapshot ensures the outer's slots aren't trampled
;;      (m5-comprehensions-followon.md §3.3).
;;
;;   2. **kIdent for `y` inside the inner body resolves to
;;      `local.get $iter_off_i`.**  ScopeResolver pushes the inner
;;      frame before lowering the inner body's subtrees, pops on
;;      exit.  The shadowing is automatic — no explicit name
;;      collision check needed (m5-comprehensions-design.md §3.5
;;      and §3.7).
;;
;;   3. **Independent accu slots per nesting level.**  Outer's
;;      `@result` lives at one workspace slot; inner's
;;      `@result` lives at another.  After the inner
;;      comprehension's `result` returns, the outer body
;;      continues, and references to outer `@result` resolve
;;      to the OUTER slot — the inner frame is already popped.
;;
;;   4. **Inner result feeds outer loop_step.**  The inner
;;      comprehension's `result` is just `@result` (its accu
;;      slot, here at 232).  The outer's loop_step reads that slot
;;      into the outer accu via cel_or.  The slot id is wired by
;;      LayoutPass; codegen lowers the inner comprehension
;;      expression to "the slot offset of inner-accu" exactly as
;;      it does for any other sub-expression's result.
;;
;; ── No new runtime helpers needed ───────────────────────────
;; This is a structural test of LayoutPass + ScopeResolver, not
;; a runtime extension.  Every helper called is already exported.
;; **Runnable today** (modulo Slices A–C ResolvePass/LayoutPass
;; being shipped first; nothing else blocks this WAT).
;;
;; Memory layout:
;;   [ 0, 16)   reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)   rodata: outer-list elem {CEL_INT, i=1}
;;   [40, 64)   rodata: inner-list elem {CEL_INT, i=0}
;;   [64, 88)   rodata: accu_init false (shared — both inits are
;;                                       structurally identical;
;;                                       StaticMemoryBuilder may
;;                                       or may not dedupe, but the
;;                                       shape is the same either way)
;;   [88,112)   rodata: rhs `0` for `y == 0` inside inner
;;   [112,136)  workspace: outer iter_range list slot
;;   [136,160)  workspace: outer accu_slot
;;   [160,184)  workspace: outer step_out scratch
;;   [184,208)  workspace: inner iter_range list slot
;;   [208,232)  workspace: inner accu_slot
;;   [232,256)  workspace: inner step_out scratch (`y == 0` result)
;;   [256+]  bump arena (malloc'd in heap)
;;
;; ── Runtime helpers — all exported today ────────────────────
;;   cel.cel_list_create / cel_list_append_at         (M4.F)
;;   cel.cel_equals_at_vv                       (M5.B)
;;   cel.cel_or                                 (M5.G)
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_append_at" (func $cel_list_append_at (param i32 i32)))
  (import "cel" "cel_equals_at_vv"
          (func $cel_equals_at_vv (param i32 i32 i32)))
  (import "cel" "cel_or" (func $cel_or (param i32 i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\01\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 88)
        "\02\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    ;; Outer-comprehension locals.
    (local $iter_off_o i32)
    (local $end_off_o  i32)
    (local $hdr_o      i32)
    ;; Inner-comprehension locals — disjoint from outer's.
    (local $iter_off_i i32)
    (local $end_off_i  i32)
    (local $hdr_i      i32)

    (call $arena_reset)

    ;; ── OUTER iter_range = [1] at slot 112 ──────────────────
    (call $cel_list_create (i32.const 112) (i32.const 1))
    (call $cel_list_append_at (i32.const 112) (i32.const 16))

    ;; Outer accu_slot ← false (24B copy from rodata at 64).
    (i32.store offset=0  (i32.const 136) (i32.load offset=0  (i32.const 64)))
    (i32.store offset=4  (i32.const 136) (i32.load offset=4  (i32.const 64)))
    (i32.store offset=8  (i32.const 136) (i32.load offset=8  (i32.const 64)))
    (i32.store offset=12 (i32.const 136) (i32.load offset=12 (i32.const 64)))
    (i32.store offset=16 (i32.const 136) (i32.load offset=16 (i32.const 64)))
    (i32.store offset=20 (i32.const 136) (i32.load offset=20 (i32.const 64)))

    (local.set $hdr_o      (i32.load offset=8 (i32.const 112)))
    (local.set $iter_off_o (i32.load offset=8 (local.get $hdr_o)))
    (local.set $end_off_o  (i32.add (local.get $iter_off_o) (i32.const 24)))

    (block $exit_o
      (loop $continue_o
        (br_if $exit_o
               (i32.ge_u (local.get $iter_off_o) (local.get $end_off_o)))
        (br_if $exit_o (i32.load offset=8 (i32.const 136)))

        ;; outer loop_step: @result || INNER
        ;; First evaluate INNER → produces a value in slot 208
        ;; (the inner accu slot, which is the inner comprehension's
        ;; result).  Then `cel_or` merges it into the outer accu.

        ;; ── INNER iter_range = [0] at slot 184 ──────────────
        (call $cel_list_create (i32.const 184) (i32.const 1))
        (call $cel_list_append_at (i32.const 184) (i32.const 40))

        ;; Inner accu_slot ← false.
        (i32.store offset=0  (i32.const 208) (i32.load offset=0  (i32.const 64)))
        (i32.store offset=4  (i32.const 208) (i32.load offset=4  (i32.const 64)))
        (i32.store offset=8  (i32.const 208) (i32.load offset=8  (i32.const 64)))
        (i32.store offset=12 (i32.const 208) (i32.load offset=12 (i32.const 64)))
        (i32.store offset=16 (i32.const 208) (i32.load offset=16 (i32.const 64)))
        (i32.store offset=20 (i32.const 208) (i32.load offset=20 (i32.const 64)))

        (local.set $hdr_i      (i32.load offset=8 (i32.const 184)))
        (local.set $iter_off_i (i32.load offset=8 (local.get $hdr_i)))
        (local.set $end_off_i  (i32.add (local.get $iter_off_i) (i32.const 24)))

        (block $exit_i
          (loop $continue_i
            (br_if $exit_i
                   (i32.ge_u (local.get $iter_off_i) (local.get $end_off_i)))
            (br_if $exit_i (i32.load offset=8 (i32.const 208)))

            ;; inner loop_step: @result || (y == 0)
            ;; The `y` here is INNER-y == local.get $iter_off_i.
            ;; ScopeResolver pushed the inner frame before this
            ;; lowering ran, so the inner binding wins.
            (call $cel_equals_at_vv
                  (i32.const 232)
                  (local.get $iter_off_i)
                  (i32.const 88))
            (call $cel_or
                  (i32.const 208) (i32.const 208) (i32.const 232))

            (local.set $iter_off_i
                       (i32.add (local.get $iter_off_i) (i32.const 24)))
            (br $continue_i)))

        ;; INNER result is now in slot 208.  Use it as the rhs of
        ;; the outer || via step_out scratch (160).
        ;; (One copy step: step_out_o ← inner_accu.)
        (i32.store offset=0  (i32.const 160) (i32.load offset=0  (i32.const 208)))
        (i32.store offset=4  (i32.const 160) (i32.load offset=4  (i32.const 208)))
        (i32.store offset=8  (i32.const 160) (i32.load offset=8  (i32.const 208)))
        (i32.store offset=12 (i32.const 160) (i32.load offset=12 (i32.const 208)))
        (i32.store offset=16 (i32.const 160) (i32.load offset=16 (i32.const 208)))
        (i32.store offset=20 (i32.const 160) (i32.load offset=20 (i32.const 208)))

        (call $cel_or
              (i32.const 136) (i32.const 136) (i32.const 160))

        (local.set $iter_off_o
                   (i32.add (local.get $iter_off_o) (i32.const 24)))
        (br $continue_o)))

    (i32.const 136))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
