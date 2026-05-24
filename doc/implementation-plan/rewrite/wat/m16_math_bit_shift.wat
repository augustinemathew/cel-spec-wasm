;; CEL source:  math.bitShiftLeft(1, 2)
;; Decl:        — (no free variables)
;;
;; Slice 0 WAT-first trace for the bit-shift kernels.  `bitShiftLeft`
;; / `bitShiftRight` are plain global `math.<name>` calls (no macro);
;; the checker resolves overload ids `math_bitShiftLeft_{int,uint}_int`
;; (value kind int or uint; shift amount always int).  Runtime
;; surface:
;;
;;   cel.cel_math_bit_shift_left_at_vv(out_slot, x_slot, n_slot)
;;       — i32, i32, i32 → ()
;;
;; (`_at_vv` suffix = 3 args: out + two values).  bitShiftRight,
;; bitAnd, bitOr, bitXor share this exact 3-arg shape; bitNot is the
;; unary `_at_v` sibling.
;;
;; ABI this trace FREEZES:
;;   - `x_slot` holds the value to shift (CEL_INT=2 or CEL_UINT=3;
;;     result keeps x's kind).  `n_slot` holds the shift count, always
;;     CEL_INT.
;;   - Output CelValue at `out_slot`, same kind as `x_slot`.
;;   - Shift semantics follow the CEL spec (math_ext.textproto), NOT
;;     C undefined-behaviour: a negative shift count and a count >= 64
;;     are spec-defined error/zero cases the kernel handles explicitly
;;     (covered by the kernel unit-test matrix; this trace exercises
;;     the nominal in-range case to lock the slot/kind shape).
;;
;; Expected decoded result: {CEL_INT, i = 4}   (1 << 2).
;;
;; NOTE: cel_math_bit_shift_left_at_vv does not yet exist in
;; cel_runtime.wasm.  This trace ASSEMBLES (wasm-as) to freeze the
;; import signature + slot shape; it runs end-to-end through
;; wat_runner only once the kernel lands (M16 Slice B).
;;
;; Memory layout:
;;   [ 0,  8)  null sentinel
;;   [ 8, 16)  legacy arena cursor/limit slots (arena now in runtime BSS)
;;   [16, 40)  rodata: x = {CEL_INT, i=1}
;;   [40, 64)  rodata: n = {CEL_INT, i=2}
;;   [64, 88)  workspace: result slot (out=64)
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "cel_math_bit_shift_left_at_vv"
          (func $cel_math_bit_shift_left_at_vv (param i32 i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; 1 << 2 → slot 64.
    (call $cel_math_bit_shift_left_at_vv
          (i32.const 64) (i32.const 16) (i32.const 40))

    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
