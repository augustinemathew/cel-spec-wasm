;; CEL source:  [10, 20, 30][1]
;; Decl:        — (no free variables)
;;
;; m31 — compile-time materialization of a constant list literal.
;; Unlike 12_list_index_arena.wat (which builds [1,2,3] at eval time via
;; cel_list_create + cel_list_append_at), here the WHOLE list value —
;; ArenaListHeader + the 3-element run + the outer CEL_LIST_ARENA frame —
;; is written into the module's data segment at compile time.  $eval does
;; NOT construct anything: it reads the materialized list directly through
;; cel_list_at_arena, exactly as if an arena had built it.  This freezes
;; the byte layout StaticMemoryBuilder::MaterializeList must reproduce and
;; proves the read kernel cannot tell a materialized list from an
;; arena-built one (m31 §2, §5).
;;
;; Layout (runtime/cel_data.h): CelValue is 24 B { kind:u32@0, _pad@4,
;; payload@8 (16 B) }; for CEL_INT the i64 sits at @8.  ArenaListHeader is
;; 16 B { count:u32, capacity:u32, elements_offset:u32, _pad:u32 }; the
;; element run is `capacity × 24 B`.  CEL_INT = 2, CEL_LIST_ARENA = 7.
;;
;; Memory map (all offsets absolute, header-first per m31 §5):
;;   [  0,  16)  reserved (null sentinel)
;;   [ 16,  32)  ArenaListHeader { count=3, cap=3, elements_offset=32 }
;;   [ 32,  56)  element 0   {CEL_INT, i=10}
;;   [ 56,  80)  element 1   {CEL_INT, i=20}
;;   [ 80, 104)  element 2   {CEL_INT, i=30}
;;   [104, 128)  outer frame {CEL_LIST_ARENA, header_ptr=16}  ← the value
;;                            the kListExpr would lower to an i32.const of
;;   [128, 152)  lookup index {CEL_INT, i=1}
;;   [152, 176)  workspace: cel_list_at_arena result slot (out=152)
;;
;; No arena import: cel_list_at_arena only reads the list + copies
;; element[i] into out_slot; it allocates nothing.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "cel_list_at_arena"
          (func $cel_list_at_arena (param i32 i32 i32)))

  ;; ArenaListHeader @16: count=3, capacity=3, elements_offset=32, _pad=0
  (data (i32.const 16)
        "\03\00\00\00" "\03\00\00\00" "\20\00\00\00" "\00\00\00\00")

  ;; Element run @32: three {CEL_INT} frames, i = 10 / 20 / 30
  (data (i32.const 32)
        "\02\00\00\00" "\00\00\00\00"
        "\0a\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 56)
        "\02\00\00\00" "\00\00\00\00"
        "\14\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 80)
        "\02\00\00\00" "\00\00\00\00"
        "\1e\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  ;; Outer list frame @104: CEL_LIST_ARENA(7), arena_list.header_ptr = 16
  (data (i32.const 104)
        "\07\00\00\00" "\00\00\00\00"
        "\10\00\00\00" "\00\00\00\00\00\00\00\00\00\00\00\00")

  ;; Lookup index @128: {CEL_INT, i=1}
  (data (i32.const 128)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    ;; Read materialized list[1] — fast path, no construction, no host trip.
    (call $cel_list_at_arena
          (i32.const 152) (i32.const 104) (i32.const 128))
    (i32.const 152))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
