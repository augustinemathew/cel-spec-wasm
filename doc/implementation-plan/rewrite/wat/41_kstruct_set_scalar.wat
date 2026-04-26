;; CEL source:  celwasm.testdata.HostMsg3{i32: 7}
;;
;; M7.B — proto literal construction with one scalar field set.
;; Layered on top of M7.A's `cel_make_message`: each literal entry
;; lowers to (1) the value sub-expression's lower, then (2) a
;; `cel_host.cel_set_field(msg_slot, field_ref_id, value_slot)`
;; call.  The result slot is reused across set calls; only the
;; final `i32.const out_slot` returns the constructed message.
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  workspace slot for kStructExpr — out_slot of
;;             cel_make_message; mutated in place by every
;;             cel_set_field.
;;   [40, 64)  workspace slot for entry value `7` (kInt) — packed
;;             into rodata for kConst, but the .rodata bytes are
;;             the operand-slot CelValue, so the slot offset is
;;             still the rodata offset.  The lowering returns the
;;             rodata offset as the i32 operand to cel_set_field.
;;   [64, mem_size)  bump arena
;;
;; New import this milestone:
;;   cel_host.cel_set_field(msg_slot, field_ref_id, value_slot)
;;     — 3 × i32 → ()
;;
;;   msg_slot     : offset of the 24B CelValue (kind=CEL_MESSAGE,
;;                  payload.msg_slot=<ExternrefTable index>) — the
;;                  message being mutated.
;;   field_ref_id : dense index into cel.abi.fields[] — same
;;                  intern table M2.C uses for kSelect reads;
;;                  M7.B appends one row per kStructExpr entry.
;;                  Set rows carry name+owner_fqn (field_number=0)
;;                  so the host resolves the FieldDescriptor by
;;                  name on the bound message.
;;   value_slot   : offset of the 24B CelValue carrying the new
;;                  field value.  cpp_type dispatch on the
;;                  resolved FieldDescriptor picks the matching
;;                  Reflection::Set… (SetInt32/SetString/etc.).
;;
;; ABI tables: cel.abi.fields[] is shared with M2.C's read-side
;; intern (the (name, owner_fqn) row carries everything both reads
;; and writes need; cpp_type is read at the host from the
;; resolved FieldDescriptor, not from the wire).
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel_host" "cel_make_message"
          (func $cel_make_message (param i32 i32)))
  (import "cel_host" "cel_set_field"
          (func $cel_set_field (param i32 i32 i32)))

  ;; Rodata for the literal `7`.  CelValue layout:
  ;;   [ 0,  4) kind = CEL_INT (2)
  ;;   [ 4,  8) _pad
  ;;   [ 8, 16) payload.i = 7 (i64)
  ;;   [16, 24) padding to 24-byte stride
  (data (i32.const 64)
        "\02\00\00\00"
        "\00\00\00\00"
        "\07\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    ;; ── PRELUDE ──────────────────────────────────────────────
    ;; (no free variables)

    ;; ── RESET ────────────────────────────────────────────────
    (call $cel_reset (i32.const 88) (i32.const 131072))

    ;; ── BODY ─────────────────────────────────────────────────
    ;; M7.A: construct the default proto into out_slot=16.
    (call $cel_make_message
          (i32.const 1)    ;; type_id=1 ("celwasm.testdata.HostMsg3")
          (i32.const 16))  ;; out_slot

    ;; M7.B: set field i32=7.  field_ref_id=1 ("HostMsg3.i32"),
    ;; value at rodata offset 64.
    (call $cel_set_field
          (i32.const 16)   ;; msg_slot
          (i32.const 1)    ;; field_ref_id
          (i32.const 64))  ;; value_slot (rodata)

    ;; Return the kStructExpr's output offset.
    (i32.const 16))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
