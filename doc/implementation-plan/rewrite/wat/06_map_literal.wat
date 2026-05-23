;; CEL source:  {1: 10}
;; Decl:        — (no free variables)
;;
;; M3.F slice — kCreateMap lowering.  The literal constructs in
;; the wasm bump arena via cel_map_create + cel_map_insert; the
;; result CelValue lives in a workspace slot.  Codegen pattern
;; mirrors expr_lower.cc::EmitKMapExpr.
;;
;; Memory layout:
;;   [ 0,  8)  null sentinel
;;   [ 8, 12)  arena cursor  — written by cel_reset
;;   [12, 16)  arena limit   — written by cel_reset
;;   [16, 40)  rodata: key kConst   {kind=CEL_INT(2), payload.i=1}
;;   [40, 64)  rodata: value kConst {kind=CEL_INT(2), payload.i=10}
;;   [64, 88)  workspace: kMapExpr result slot (out_slot=64)
;;   [88, mem_size)  bump arena — cel_map_create allocates the
;;                   ArenaMapHeader (16 B) here, then cel_map_insert
;;                   bumps to allocate the entries run.
;;
;; New imports this milestone (vs. M2):
;;   cel.cel_map_create(out_slot, capacity)         — i32, i32 → ()
;;   cel.cel_map_insert(map_slot, key_slot, val_slot) — i32×3  → ()
;;
;; cel_map_create allocates 16 B for the header + capacity*48 B
;; for the entries run (per ArenaMapHeader contract in
;; runtime/cel_data.h).  cel_map_insert appends one (key, value)
;; pair; capacity overflow poisons the map with CEL_ERR_OVERFLOW.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_map_create" (func $cel_map_create (param i32 i32)))
  (import "cel" "cel_map_insert" (func $cel_map_insert (param i32 i32 i32)))

  ;; rodata: kind=CEL_INT(2), _pad, payload.i=1, pad8.
  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\01\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata: kind=CEL_INT(2), _pad, payload.i=10, pad8.
  (data (i32.const 40)
        "\02\00\00\00"
        "\00\00\00\00"
        "\0a\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    ;; ── RESET ─ arena begins past workspace (= 88).
    (call $arena_reset)

    ;; ── BODY ─ kCreateMap arm.
    ;; Allocate header + entries-run for capacity=1.
    (call $cel_map_create (i32.const 64) (i32.const 1))
    ;; Append (key=16, value=40) to the map at slot 64.
    (call $cel_map_insert (i32.const 64) (i32.const 16) (i32.const 40))

    ;; Return the map's workspace slot offset.
    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
