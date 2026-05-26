;; CEL source (the .celfn library):
;;   Module foo;
;;   fn double(x int): int { x * 2 }
;;
;; This is the CEL-DEFINED-fn library module `foo.wasm` — the side
;; module the caller expression links against.  WE generate it via
;; Binaryen (same pipeline as the expr module), NOT via a C toolchain:
;; it has NO libc, NO wasi imports, NO own memory.  It imports the
;; runtime's SHARED memory + kernels, plus one `__memory_base` global
;; (the wasm dynamic-linking PIC primitive) that the engine assigns to
;; a malloc'd heap slice at Plan time.
;;
;; ── Slot model (the point of this trace) ────────────────────────────
;; Cross-call slots are ABSOLUTE offsets in the shared memory, chosen
;; by the CALLER and passed as args:
;;   out_slot   — where the caller wants the result CelValue written
;;   arg0_slot  — where the caller put the argument CelValue
;; The body's OWN cells are __memory_base-RELATIVE (its private region):
;;   __memory_base + 0   : CelValue{INT, 2}   (the literal `2`, rodata)
;;   __memory_base + 24  : scratch slot for `x * 2`
;; The runtime kernel never knows the difference: `__memory_base + 24`
;; IS an absolute address once the add runs, so cel_int_mul_at_vv
;; reads/writes it like any other slot.
;;
;; Disjointness invariant proven here: this body only ever writes
;;   (a) the caller-supplied out_slot, and
;;   (b) cells inside [__memory_base, __memory_base + region_size).
;; It NEVER writes a bare low offset, so it cannot clobber the expr's
;; [0, 8192) slots — regardless of what offsets the expr's LayoutPass
;; picked.
(module
  ;; The one shared memory, owned + exported by cel_runtime.wasm.
  (import "cel" "memory" (memory 2 1024 shared))

  ;; PIC base for THIS library's static region.  Engine mallocs a
  ;; slice from the runtime heap and binds this global to its offset
  ;; before instantiating the expr module.  Immutable i32.
  (import "env" "__memory_base" (global $__memory_base i32))

  ;; Runtime kernels the body reaches — plain function imports, bound
  ;; on the linker as cel.* (identical surface to the expr module).
  (import "cel" "cel_int_mul_at_vv"
          (func $cel_int_mul_at_vv (param i32 i32 i32)))
  (import "cel" "cel_copy_slot"
          (func $cel_copy_slot (param i32 i32)))

  ;; double(x) = x * 2.  Export name = the synthesised overload-id
  ;; the .celfn parser produces ("double_int").  ABI: (out_slot,
  ;; arg0_slot) -> ().
  (func $double_int (param $out_slot i32) (param $arg0_slot i32)
    ;; scratch = arg0 * 2
    (call $cel_int_mul_at_vv
          ;; dst: foo's scratch cell, __memory_base-relative
          (i32.add (global.get $__memory_base) (i32.const 24))
          ;; lhs: the caller's argument slot, ABSOLUTE
          (local.get $arg0_slot)
          ;; rhs: foo's literal `2`, __memory_base-relative
          (i32.add (global.get $__memory_base) (i32.const 0)))
    ;; out_slot = scratch  (write the result into the caller's slot)
    (call $cel_copy_slot
          (local.get $out_slot)
          (i32.add (global.get $__memory_base) (i32.const 24))))

  (export "double_int" (func $double_int)))
