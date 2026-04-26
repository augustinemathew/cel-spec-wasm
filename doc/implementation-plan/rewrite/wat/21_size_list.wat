;; CEL source:  size([1, 2, 3])
;; Decl:        — (no free variables)
;;
;; M5.D step 1 — locks the kArena fast path for aggregate ops.
;; size on a kArena list is a pure-wasm `(out_slot, list_slot)` call
;; that reads the ArenaListHeader's `count` field and writes a
;; CEL_INT into `out_slot`.  No host trip; no dispatcher.
;;
;; The full three-path dispatch design (`map-list-dispatch.md §6`)
;; calls for kHost / kDynamic siblings, but those land in M5.D
;; step 2 (the kHost trampolines + the kDynamic dispatcher with
;; `__attribute__((musttail))` arms).  Every M5 codegen call site
;; targets one of the three concrete entry points based on the
;; operand's `*_origin` annotation; this WAT exercises the kArena
;; one.
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  rodata: kConst 1     {CEL_INT, i=1}
;;   [40, 64)  rodata: kConst 2     {CEL_INT, i=2}
;;   [64, 88)  rodata: kConst 3     {CEL_INT, i=3}
;;   [88,112)  workspace: kListExpr result slot (out=88)
;;   [112,136) workspace: kCall(size) result slot (out=112)
;;   [136, mem_size)  bump arena (ArenaListHeader + elements run)
;;
;; New import this milestone:
;;   cel.cel_list_size_arena(out_slot, list_slot) — i32×2 → ()
;;
;; cel_list_size_arena contract (cel-cpp parity:
;;   third_party/cel-cpp/runtime/standard/container_functions.cc::Size):
;;     - reads l as CEL_LIST_ARENA.  Other kind → out_slot =
;;       {CEL_ERROR, err=CEL_ERR_TYPE_MISMATCH}.
;;     - 3VL absorption — UNKNOWN/ERROR propagates verbatim.
;;     - happy path: out_slot = {CEL_INT, i=ArenaListHeader.count}.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_set" (func $cel_list_set (param i32 i32 i32)))
  (import "cel" "cel_list_size_arena"
          (func $cel_list_size_arena (param i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\03\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $cel_reset (i32.const 136) (i32.const 131072))

    ;; Build the list at slot 88.
    (call $cel_list_create (i32.const 88) (i32.const 3))
    (call $cel_list_set (i32.const 88) (i32.const 0) (i32.const 16))
    (call $cel_list_set (i32.const 88) (i32.const 1) (i32.const 40))
    (call $cel_list_set (i32.const 88) (i32.const 2) (i32.const 64))

    ;; Size of the list — kArena fast path.
    (call $cel_list_size_arena (i32.const 112) (i32.const 88))

    (i32.const 112))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
