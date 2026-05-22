;; CEL source:  [1, 2, 3].filter(v, v != 2)    → [1, 3]
;; Decl:        — (no free variables)
;;
;; M5 slice (Slice D — `filter` / `map` + dynamic-list primitive).
;; Companion to `62_comprehension_map_list.wat`; differs in
;; `loop_step`'s shape.  `filter` keeps the element when the
;; predicate is true, so its per-iter step is a *conditional*
;; append.
;;
;; cel-cpp macro → kComprehensionExpr (parser/macro.cc::MakeFilter):
;;   iter_range  = [1, 2, 3]                    (kCreateList)
;;   iter_var    = v
;;   accu_var    = @result
;;   accu_init   = []                           (empty list literal)
;;   loop_cond   = true
;;   loop_step   = (v != 2) ? @result + [v] : @result
;;   result      = @result
;;
;; ── Key design claims ───────────────────────────────────────
;;
;;   1. **Conditional append → wasm `if` block around the
;;      `cel_list_append_at` call.**  No allocation when the
;;      predicate is false; no copy of `v` into a workspace.
;;      Codegen pattern-matches the ternary IR shape (kCall(`_?_:_`,
;;      pred, accu + [v], accu)) and emits the wasm `if` directly,
;;      bypassing the general ternary lowering that would copy via
;;      cel_copy_slot.
;;
;;   2. **Append `v` itself, not a transformed value.**  Unlike
;;      `map(v, t)` where the appended element is `t = f(v)`, in
;;      `filter` we append the iter element verbatim — the iter
;;      pointer goes straight into `cel_list_append_at`.  No
;;      step_out workspace slot needed for the appended value.
;;
;;   3. **Predicate uses cel_int_ne (CEL_BOOL out).**  We read
;;      payload.b at offset 8 of pred_slot to drive the `if`.
;;      `i32.load offset=8` is the canonical peephole — same as
;;      the `exists`/`all` loop_cond pattern.
;;
;; ── DEPENDS ON Slice D ──────────────────────────────────────
;; Same as WAT 62: `cel_list_append_at` is not yet exported.
;; Tagged `manual` until Slice D ships.
;;
;; Memory layout:
;;   [ 0, 16)   reserved + arena cursor/limit
;;   [16, 40)   rodata: list elem [0] = {CEL_INT, i=1}
;;   [40, 64)   rodata: list elem [1] = {CEL_INT, i=2}
;;   [64, 88)   rodata: list elem [2] = {CEL_INT, i=3}
;;   [88,112)   rodata: predicate rhs `2` for `v != 2`
;;   [112,136)  workspace: iter_range list slot
;;   [136,160)  workspace: accu_slot (the growing filter result)
;;   [160,184)  workspace: pred_slot (per-iter `v != 2` result)
;;   [184, mem_size)  bump arena
;;
;; ── Runtime helpers ─────────────────────────────────────────
;;   cel.cel_int_ne_at_vv     (M5.B)          — `v != 2`
;;   cel.cel_list_append_at   (Slice D NEW)
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_set" (func $cel_list_set (param i32 i32 i32)))
  (import "cel" "cel_int_ne_at_vv"
          (func $cel_int_ne_at_vv (param i32 i32 i32)))
  ;; DEPENDS ON Slice D.
  (import "cel" "cel_list_append_at"
          (func $cel_list_append_at (param i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\03\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; Predicate rhs `2`.
  (data (i32.const 88)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $list_hdr i32)
    (local $iter_off i32)
    (local $end_off  i32)

    (call $arena_reset)

    ;; iter_range = [1, 2, 3] at slot 112.
    (call $cel_list_create (i32.const 112) (i32.const 3))
    (call $cel_list_set (i32.const 112) (i32.const 0) (i32.const 16))
    (call $cel_list_set (i32.const 112) (i32.const 1) (i32.const 40))
    (call $cel_list_set (i32.const 112) (i32.const 2) (i32.const 64))

    ;; accu_init = [] at slot 136.
    (call $cel_list_create (i32.const 136) (i32.const 0))

    (local.set $list_hdr (i32.load offset=8 (i32.const 112)))
    (local.set $iter_off (i32.load offset=8 (local.get $list_hdr)))
    (local.set $end_off  (i32.add (local.get $iter_off) (i32.const 72)))

    (block $exit
      (loop $continue
        (br_if $exit
               (i32.ge_u (local.get $iter_off) (local.get $end_off)))

        ;; loop_step: predicate then conditional append.
        ;;   pred_slot = (v != 2)
        (call $cel_int_ne_at_vv
              (i32.const 160)
              (local.get $iter_off)
              (i32.const 88))

        ;; if (pred_slot.payload.b) { accu = accu + [v] }
        (if (i32.load offset=8 (i32.const 160))
          (then
            (call $cel_list_append_at
                  (i32.const 136)         ;; accu_slot
                  (local.get $iter_off)))) ;; v IS iter_off

        (local.set $iter_off
                   (i32.add (local.get $iter_off) (i32.const 24)))
        (br $continue)))

    (i32.const 136))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
