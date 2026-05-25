;; CEL source:  TestAllTypes{standalone_enum: 5000000000, single_int32: 7}
;; Decl:        — (no free variables)
;;
;; Locks the poison-on-error contract for `cel_host.cel_set_field`
;; (M20 / cleanup-backlog #11).  The ABI signature is UNCHANGED —
;; still `(msg_slot, field_ref_id, value_slot)` → () — so codegen for
;; kStructExpr is byte-identical to today (make_message + one
;; cel_set_field per entry + trailing `i32.const out_slot`).  What
;; this trace freezes is the *semantic* contract on the existing
;; msg_slot cell:
;;
;;   cel_host.cel_set_field(msg_slot, field_ref_id, value_slot):
;;     1. m = cel_value_at(msg_slot)
;;        If m.kind == CEL_ERROR: return  ;; EARLY-OUT — an earlier
;;        entry already poisoned the message; leave the poison so it
;;        rides the slot through the remaining sets untouched.
;;     2. Resolve FieldDescriptor; read value_slot's CelValue.
;;     3. VALUE-ERROR (e.g. an int outside [INT32_MIN, INT32_MAX]
;;        assigned to an int32 / enum / wrapper field): write
;;        CEL_ERROR{CEL_ERR_OVERFLOW} into msg_slot (overwriting the
;;        partially-built message) and return.  NOT a wasm trap — the
;;        expression result becomes a catchable CEL error, matching
;;        cel-cpp's TypeConversionError (struct_value_builder.cc:1081).
;;     4. INTERNAL violation (wrong value kind, bad descriptor,
;;        non-mutable backing): still a non-OK Status → wasm trap.
;;        These are codegen/checker invariant breaks, not CEL
;;        semantics.
;;
;; Because the poison rides the existing out_slot, the trailing
;; `i32.const out_slot` naturally returns the error value with ZERO
;; codegen change.
;;
;; ── Memory layout ─────────────────────────────────────────────
;;
;;   [ 0, 16)   reserved null + arena scaffolding
;;   [16, 40)   rodata: out-of-range value CelValue {CEL_INT,
;;              i=5000000000} (0x12A05F200 — > INT32_MAX)
;;   [40, 64)   workspace: the message CelValue {CEL_MESSAGE(10),
;;              payload.msg_slot=1}.  Statically initialised here in
;;              lieu of a cel_make_message call (the host stub reads
;;              only msg_slot.kind; the proto contents are irrelevant
;;              to the poison contract).  In production a preceding
;;              cel_make_message writes this cell.
;;   [64, 88)   rodata: in-range value CelValue {CEL_INT, i=7}
;;   [88, mem)  bump arena (unused)
;;
;; ── Host stub contract (wat_runner test) ──────────────────────
;;
;; The test installs a `cel_host_cel_set_field_stub` modelling the
;; contract above:
;;   - read msg_slot.kind; if CEL_ERROR → return (early-out);
;;   - read value_slot; if it's a CEL_INT outside int32 range →
;;     write CEL_ERROR{CEL_ERR_OVERFLOW=10} to msg_slot;
;;   - else record a normal set.
;; Expectations after $eval:
;;   - the FIRST set (overflow) poisons slot 40 → CEL_ERROR{10};
;;   - the SECOND set (in-range 7) hits the early-out (msg already
;;     CEL_ERROR) and is a no-op — proving the poison propagates and
;;     a later valid field can't "un-poison" the result;
;;   - $eval returns slot 40, decoding to CEL_ERROR{CEL_ERR_OVERFLOW}.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel_host" "cel_set_field"
          (func $cel_set_field (param i32 i32 i32)))

  ;; rodata: out-of-range CelValue{CEL_INT(2), i=5000000000} at 16.
  ;; 5000000000 = 0x1_2A05_F200 -> LE 8 bytes: 00 F2 05 2A 01 00 00 00.
  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\00\f2\05\2a\01\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; workspace: CelMessage{kind=CEL_MESSAGE(10), payload.msg_slot=1} at 40.
  (data (i32.const 40)
        "\0a\00\00\00"
        "\00\00\00\00"
        "\01\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata: in-range CelValue{CEL_INT(2), i=7} at 64.
  (data (i32.const 64)
        "\02\00\00\00"
        "\00\00\00\00"
        "\07\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)
    ;; entry 1: standalone_enum = 5000000000 (out of int32 range).
    ;; Stub poisons msg_slot 40 -> CEL_ERROR{CEL_ERR_OVERFLOW}.
    (call $cel_set_field
          (i32.const 40)    ;; msg_slot
          (i32.const 100)   ;; field_ref_id (enum field sentinel)
          (i32.const 16))   ;; value_slot — out-of-range int
    ;; entry 2: single_int32 = 7 (in range).  Stub sees msg_slot is
    ;; already CEL_ERROR and EARLY-OUTs — no-op.  Proves a later valid
    ;; field cannot overwrite the poison.
    (call $cel_set_field
          (i32.const 40)    ;; msg_slot
          (i32.const 101)   ;; field_ref_id (int32 field sentinel)
          (i32.const 64))   ;; value_slot — in-range int
    ;; Return the message slot — now carrying the poison.
    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
