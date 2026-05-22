;; CEL source:  [1, 2, 3][1]
;; Decl:        — (no free variables)
;;
;; M4.F slice — kCreateList + kCallExpr(`_[_]`) on the kArena
;; origin.  Codegen routes the lookup through cel_list_at_arena
;; (the pure-wasm fast path), bypassing the kDynamic dispatcher
;; and the kHost trampoline because ResolvePass proved the
;; operand is arena-built.
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  rodata: element 0       {CEL_INT, i=1}
;;   [40, 64)  rodata: element 1       {CEL_INT, i=2}
;;   [64, 88)  rodata: element 2       {CEL_INT, i=3}
;;   [88,112)  rodata: lookup-index    {CEL_INT, i=1}
;;   [112,136) workspace: kListExpr result slot (out=112)
;;   [136,160) workspace: kCallExpr result slot (out=136)
;;   [160, mem_size)  bump arena
;;
;; New import this milestone (vs. 11):
;;   cel.cel_list_at_arena(out_slot, list_slot, index_slot) — i32×3 → ()
;;
;; cel_list_at_arena:
;;   - reads the index CelValue, requires CEL_INT (any other kind
;;     poisons out_slot with CEL_ERR_TYPE_MISMATCH);
;;   - 3VL absorption — UNKNOWN/ERROR on either operand propagates;
;;   - bounds-checks 0 <= i < count (else CEL_ERR_INDEX_OUT_OF_BOUNDS,
;;     which also catches negative i32-cast);
;;   - copies element[i] CelValue into out_slot.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_set" (func $cel_list_set (param i32 i32 i32)))
  (import "cel" "cel_list_at_arena"
          (func $cel_list_at_arena (param i32 i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\03\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 88)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; Build the list at slot 112.
    (call $cel_list_create (i32.const 112) (i32.const 3))
    (call $cel_list_set (i32.const 112) (i32.const 0) (i32.const 16))
    (call $cel_list_set (i32.const 112) (i32.const 1) (i32.const 40))
    (call $cel_list_set (i32.const 112) (i32.const 2) (i32.const 64))

    ;; Index the list — fast path, no host trip.
    (call $cel_list_at_arena
          (i32.const 136) (i32.const 112) (i32.const 88))

    (i32.const 136))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
