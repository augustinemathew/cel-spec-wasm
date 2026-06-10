;; CEL source:  optional.ofNonZeroValue(<CEL_MESSAGE>).hasValue()  → bool
;; Decl:        — (the message CelValue is pre-staged in rodata; in a
;;              real pipeline it would be a `TestAllTypes{...}` literal
;;              whose `cel_make_message` call interned msg_slot=1)
;;
;; Locks the **host-backed arm of the zero-value predicate** for
;; `cel_optional_of_non_zero_at_v` (cleanup-backlog #10): the runtime's
;; `is_zero_value` (runtime/cel_optional.c) must consult the host for
;; the three host-backed kinds instead of `__builtin_trap()`-ing:
;;
;;   | v.kind        | host surface consulted                          |
;;   |---------------|-------------------------------------------------|
;;   | CEL_LIST_HOST | cel_host.cel_list_size(tmp, v_slot) → INT == 0  |
;;   | CEL_MAP_HOST  | cel_host.cel_map_size(tmp, v_slot)  → INT == 0  |
;;   | CEL_MESSAGE   | cel_host.cel_message_is_zero(tmp, v_slot) → BOOL|
;;
;; ── New host import this trace locks ───────────────────────
;;
;;   cel_host.cel_message_is_zero(out_slot, msg_slot) → ()
;;
;; Contract (cel-cpp parity — `ParsedMessageValue::IsZeroValue()`,
;; third_party/cel-cpp/common/values/parsed_message_value.cc:78):
;;   1. Read the CelValue at `msg_slot`; must be CEL_MESSAGE
;;      (UNKNOWN/ERROR propagate; other kinds poison kTypeMismatch).
;;   2. Dereference `payload.msg_slot` via the ExternrefTable to the
;;      HostMessageBacking; unmapped slot / non-proto backing poisons
;;      kHostAdapterError.
;;   3. Write CEL_BOOL at `out_slot`: true iff the proto's
;;      unknown-field set is empty AND `Reflection::ListFields`
;;      returns no set fields.
;;
;; The wasm caller (`is_zero_value`) arena-allocs a 24-byte scratch
;; CelValue, passes its offset as `out_slot`, and reads the result:
;; CEL_BOOL true → zero (→ None); CEL_BOOL false → non-zero (→ Some);
;; any non-BOOL result (poison) → treated as non-zero so the operand
;; propagates instead of vanishing (same posture as the predicate's
;; default arm).
;;
;; NOTE: the import is declared by `cel_runtime.wasm` (the kernel
;; calls it), NOT by this expr module — exactly like
;; `cel_host.cel_set_field` in m14_proto_set_field_if_present.wat.
;; The harness binds the stub on the shared linker, so the runtime's
;; import resolves to it.
;;
;; Memory layout:
;;   [ 0, 16)   reserved null + arena scaffolding
;;   [16, 40)   rodata: CelValue{kind=CEL_MESSAGE(10), payload.msg_slot=1}
;;   [40, 64)   workspace: kCall(`optional.ofNonZeroValue`) out_slot
;;   [64, 88)   workspace: kCall(`hasValue`) out_slot (the returned bool)
;;   [88, mem_size)  bump arena: is_zero_value's 24-byte scratch slot,
;;                   then the 32-byte OptionalCell.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_optional_of_non_zero_at_v"
          (func $cel_optional_of_non_zero_at_v (param i32 i32)))
  (import "cel" "cel_optional_has_value_at_v"
          (func $cel_optional_has_value_at_v (param i32 i32)))

  ;; rodata: CelValue{CEL_MESSAGE(=10), _pad=0, payload.msg_slot=1}
  (data (i32.const 16)
        "\0a\00\00\00"
        "\00\00\00\00"
        "\01\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)
    ;; `optional.ofNonZeroValue(<message>)`:
    ;;   out_slot = 40, v_slot = 16  (CEL_MESSAGE, msg_slot=1)
    ;; The kernel's CEL_MESSAGE arm calls
    ;; cel_host.cel_message_is_zero(scratch, 16); the stubbed host
    ;; answer decides None (stub writes true) vs Some (stub writes
    ;; false).
    (call $cel_optional_of_non_zero_at_v (i32.const 40) (i32.const 16))
    ;; `.hasValue()`: out_slot = 64, opt_slot = 40.
    (call $cel_optional_has_value_at_v (i32.const 64) (i32.const 40))
    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
