;; CEL source:  [1, 2, 3]
;; Decl:        — (no free variables)
;;
;; M4.F slice — kCreateList lowering.  The literal constructs in
;; the wasm bump arena via cel_list_create + per-element
;; cel_list_set; the result CelValue lives in a workspace slot.
;; Codegen pattern mirrors expr_lower.cc::EmitKListExpr.
;;
;; Plan-vs-execution delta from the M4 plan: the runtime API is
;; `create(out, count)` + `set(list, index, elem)`, NOT the
;; planned `create / append / grow` triple.  Codegen always knows
;; the element count at lowering time, so a fixed-length API is
;; sufficient and simpler.  See `m4-list-literals.md` header
;; callout.
;;
;; Memory layout:
;;   [ 0,  8)  null sentinel
;;   [ 8, 12)  arena cursor  — cel_reset
;;   [12, 16)  arena limit   — cel_reset
;;   [16, 40)  rodata: element 0   {CEL_INT, i=1}
;;   [40, 64)  rodata: element 1   {CEL_INT, i=2}
;;   [64, 88)  rodata: element 2   {CEL_INT, i=3}
;;   [88,112)  workspace: kListExpr result slot (out=88)
;;   [112, mem_size)  bump arena — cel_list_create reserves
;;                    the ArenaListHeader (16 B) followed by
;;                    count*24 B for the elements run.
;;
;; New imports this milestone:
;;   cel.cel_list_create(out_slot, count)         — i32, i32 → ()
;;   cel.cel_list_set(list_slot, index, elem_slot) — i32×3   → ()
;;
;; cel_list_create allocates 16 + count*24 B and zero-fills the
;; elements run with CEL_NULL CelValues.  cel_list_set writes the
;; CelValue at elem_slot into element[index]; out-of-bounds index
;; or set on a poisoned list poisons with CEL_ERR_OVERFLOW.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_set" (func $cel_list_set (param i32 i32 i32)))

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
    (call $cel_reset (i32.const 112) (i32.const 131072))

    ;; Reserve header + 3 element slots.
    (call $cel_list_create (i32.const 88) (i32.const 3))
    ;; Per-element write — index increments 0, 1, 2.
    (call $cel_list_set (i32.const 88) (i32.const 0) (i32.const 16))
    (call $cel_list_set (i32.const 88) (i32.const 1) (i32.const 40))
    (call $cel_list_set (i32.const 88) (i32.const 2) (i32.const 64))

    (i32.const 88))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
