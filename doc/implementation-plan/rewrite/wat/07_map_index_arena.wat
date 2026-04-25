;; CEL source:  {1: 10}[1]
;; Decl:        — (no free variables)
;;
;; M3.F slice — kCreateMap + kCallExpr(`_[_]`) on the kArena
;; origin.  Codegen routes the lookup through cel_map_lookup_arena
;; (the pure-wasm fast path), bypassing the kDynamic dispatcher
;; and the kHost trampoline because ResolvePass proved the
;; operand is arena-built.
;;
;; Three kConst nodes — the insert key, the value, and the lookup
;; key.  No dedup at M3 (StaticMemoryBuilder allocates a fresh
;; frame per kConst), so the literal `1` appears twice in rodata.
;;
;; Memory layout:
;;   [ 0, 16)  null sentinel + arena cursor/limit
;;   [16, 40)  rodata: insert-key kConst {CEL_INT, i=1}
;;   [40, 64)  rodata: value kConst       {CEL_INT, i=10}
;;   [64, 88)  rodata: lookup-key kConst  {CEL_INT, i=1}
;;   [88,112)  workspace: kMapExpr result slot (out=88)
;;   [112,136) workspace: kCallExpr lookup result slot (out=112)
;;   [136, mem_size)  bump arena
;;
;; New import this milestone (in addition to those in 06):
;;   cel.cel_map_lookup_arena(out_slot, map_slot, key_slot) — i32×3 → ()
;;
;; cel_map_lookup_arena reads the ArenaMapHeader at map_slot,
;; linear-scans the entries-run for a key that StructurallyEquals
;; the lookup key, copies the matching value CelValue into
;; out_slot.  Missing key → {CEL_ERROR, payload.err=CEL_ERR_NO_SUCH_KEY}.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel" "cel_map_create" (func $cel_map_create (param i32 i32)))
  (import "cel" "cel_map_insert" (func $cel_map_insert (param i32 i32 i32)))
  (import "cel" "cel_map_lookup_arena"
          (func $cel_map_lookup_arena (param i32 i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\0a\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $cel_reset (i32.const 136) (i32.const 131072))

    ;; Build the map at slot 88.
    (call $cel_map_create (i32.const 88) (i32.const 1))
    (call $cel_map_insert (i32.const 88) (i32.const 16) (i32.const 40))

    ;; Index it with key at 64, store the result at slot 112.
    (call $cel_map_lookup_arena
          (i32.const 112) (i32.const 88) (i32.const 64))

    (i32.const 112))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
