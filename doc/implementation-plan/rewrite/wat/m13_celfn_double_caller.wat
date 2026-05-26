;; CEL source (the caller expression):  double(21)
;;   with the CEL-defined library `foo` (see m13_celfn_double_lib.wat)
;;
;; This is the expr module — identical import surface to any compiled
;; expression, PLUS one import of the custom fn from the `foo` module.
;; It owns the [0, 8192) reserved low region for its rodata + workspace
;; (via the runtime's --global-base=8192); the library's cells live in
;; the heap at __memory_base.  The two never overlap.
;;
;; ── Memory layout (expr's region, [0, 8192)) ────────────────────────
;;   [ 0, 16)  reserved (null sentinel; arena state in runtime BSS)
;;   [16, 40)  rodata: CelValue{INT, 21}        (the argument literal)
;;   [40, 64)  workspace: result slot for double(21)
;;   foo's cells (const 2, scratch) live at __memory_base in the heap —
;;   NOT here.  Proof of disjointness: this module writes only [16,40)
;;   (data segment) and passes 40 as out_slot; foo writes only
;;   __memory_base+{0,24} and the 40 we handed it.
(module
  ;; Same shared memory the runtime owns and foo.wasm imports.
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))

  ;; The custom fn, resolved at Plan time to foo.wasm's export.  The
  ;; import MODULE name is the .celfn `Module foo;` directive; the
  ;; FIELD name is the overload-id.
  (import "foo" "double_int" (func $double_int (param i32 i32)))

  ;; rodata: CelValue{kind=CEL_INT(=2), _pad=0, payload.i=21}.
  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\15\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)
    ;; double(21): out = workspace slot 40, arg = rodata slot 16.
    (call $double_int (i32.const 40) (i32.const 16))
    ;; result CelValue lives at 40.
    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
