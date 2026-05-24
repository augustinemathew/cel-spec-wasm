;; CEL source:  2 in [1, 2, 3]
;; Decl:        — (no free variables)
;;
;; M5.D step 1 — kArena fast path for the `in` operator.  The
;; helper iterates the ArenaListHeader's elements run, applying
;; the element-equality matcher (same shape as `cel_map_lookup_arena`'s
;; `map_keys_equal`).  Returns CEL_BOOL.
;;
;; Memory layout:
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  rodata: kConst 1     {CEL_INT, i=1}
;;   [40, 64)  rodata: kConst 2     {CEL_INT, i=2}
;;   [64, 88)  rodata: kConst 3     {CEL_INT, i=3}
;;   [88,112)  rodata: needle 2     {CEL_INT, i=2}
;;   [112,136) workspace: kListExpr result slot (out=112)
;;   [136,160) workspace: kCall(in) result slot (out=136)
;;   [160+]  bump arena (malloc'd in heap)
;;
;; New import this milestone:
;;   cel.cel_list_in_arena(out_slot, value_slot, list_slot) — i32×3 → ()
;;
;; cel_list_in_arena contract (cel-cpp parity:
;;   third_party/cel-cpp/runtime/standard/container_functions.cc::In):
;;     - reads l as CEL_LIST_ARENA.  Other kind → CEL_ERR_TYPE_MISMATCH.
;;     - 3VL absorption on either operand.
;;     - element-equality uses the same kind-aware matcher as
;;       map_keys_equal: bool/int/uint/double via numeric semantics
;;       per langdef §"Equality"; string/bytes via byte-eq.
;;     - happy path: out_slot = {CEL_BOOL, b=(needle in list ? 1 : 0)}.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_append_at" (func $cel_list_append_at (param i32 i32)))
  (import "cel" "cel_list_in_arena"
          (func $cel_list_in_arena (param i32 i32 i32)))

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
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; Build the list at slot 112.
    (call $cel_list_create (i32.const 112) (i32.const 3))
    (call $cel_list_append_at (i32.const 112) (i32.const 16))
    (call $cel_list_append_at (i32.const 112) (i32.const 40))
    (call $cel_list_append_at (i32.const 112) (i32.const 64))

    ;; `2 in list` — fast path.
    (call $cel_list_in_arena
          (i32.const 136) (i32.const 88) (i32.const 112))

    (i32.const 136))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
