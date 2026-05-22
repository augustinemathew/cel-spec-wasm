;; CEL source:  42
;;
;; Memory layout:
;;   [ 0,  8)  reserved null sentinel
;;   [ 8, 12)  arena cursor (u32)  — written by arena_reset
;;   [12, 16)  arena limit  (u32)  — written by arena_reset
;;   [16, 40)  rodata: one 24-byte CelValue {kind=CEL_INT, payload.i=42}
;;   [40, mem_size)  bump arena
;;
;; No variables, no workspace.
(module
  ;; ABI: cel.memory is host-allocated and bound by Engine::Plan.
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))

  ;; rodata: CelValue{kind=CEL_INT(=2), _pad=0, payload.i=42}.
  ;; Wire layout (LE):
  ;;   u32 kind          = 0x02 0x00 0x00 0x00
  ;;   u32 _pad          = 0x00 0x00 0x00 0x00
  ;;   i64 payload.i     = 0x2a 0x00 0x00 0x00 0x00 0x00 0x00 0x00
  ;;   (union padding)    0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00
  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\2a\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    ;; Every $eval starts by resetting the arena: cursor=arena_base,
    ;; limit=mem_size.  No-op for this expr (no string/bytes allocations).
    (call $arena_reset)
    ;; Return the rodata offset of the 42-constant CelValue.
    (i32.const 16))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
