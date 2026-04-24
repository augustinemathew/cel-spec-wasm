;; CEL source:  c.name
;; Decl:        c : celwasm.testdata.Customer
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  workspace slot for `c`      — local_index 0
;;   [40, 64)  workspace slot for select   — out_slot of cel_get_field
;;             (LayoutPass assigns a fresh workspace slot per
;;             internal-node output; slot reuse across unrelated
;;             subtrees is a future optimisation, not required here.)
;;   [64, mem_size)  bump arena
;;
;; New import this milestone:
;;   cel_host.cel_get_field(out_slot, msg_slot, field_ref_id,
;;                          attribute_id)  — 4 × i32 → ()
;;
;;   out_slot     : offset of 24B cell to fill with the field's CelValue
;;   msg_slot     : offset of 24B cell holding the message CelValue
;;                  (kind=CEL_MESSAGE, payload.msg_slot=<externref idx>)
;;   field_ref_id : dense index into cel.abi.fields[] — resolves to
;;                  (field_number, field_name)
;;   attribute_id : dense index into cel.abi.attributes[] — consulted
;;                  at runtime by PartialEval to absorb Unknowns
;;
;; ABI tables (cel.abi.fields[], cel.abi.attributes[]) are decoded
;; once at Engine::Plan time and threaded through as callback data
;; to the trampoline.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel_host" "cel_get_field"
          (func $cel_get_field (param i32 i32 i32 i32)))
  (import "cel_host" "cel_has_field"
          (func $cel_has_field (param i32 i32 i32 i32)))

  (func $eval (result i32)
    (local $c_off i32)

    ;; ── PRELUDE ──────────────────────────────────────────────
    (local.set $c_off (i32.const 16))

    ;; ── RESET ────────────────────────────────────────────────
    (call $cel_reset (i32.const 64) (i32.const 131072))

    ;; ── BODY ─────────────────────────────────────────────────
    ;; kSelect lowering: ask cel_host to fill the select's output
    ;; workspace slot from c's message slot.  field_ref_id=1 is the
    ;; compile-time-assigned handle for "Customer.name";
    ;; attribute_id=1 identifies the attribute path "c.name" for
    ;; unknown-pattern matching (no-op under Eval, consulted under
    ;; PartialEval).
    (call $cel_get_field
          (i32.const 40)    ;; out_slot
          (local.get $c_off) ;; msg_slot
          (i32.const 1)      ;; field_ref_id ("Customer.name")
          (i32.const 1))     ;; attribute_id ("c.name")

    ;; Return the select's output offset.
    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
