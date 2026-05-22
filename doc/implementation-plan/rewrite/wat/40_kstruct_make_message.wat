;; CEL source:  celwasm.testdata.HostMsg3{}
;;
;; M7.A — empty proto literal construction.  No field-set yet
;; (M7.B): `Foo{a: 1}` lowers as `cel_make_message` followed by
;; one `cel_set_field` per entry.  This trace is the empty-Foo
;; baseline — codegen for non-empty literals layers per-entry
;; `cel_set_field` calls between the make-message call and the
;; final i32.const that returns the slot offset.
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  workspace slot for kStructExpr — out_slot of
;;             cel_host.cel_make_message.  After the call this
;;             cell contains:
;;               kind        = CEL_MESSAGE (10)
;;               payload.msg_slot = <ExternrefTable index>
;;             (the externref points at an OwnedProtoBacking
;;              owning a default-constructed proto.)
;;   [40, mem_size)  bump arena (unused for empty construction)
;;
;; New import this milestone:
;;   cel_host.cel_make_message(type_id, out_slot)  — 2 × i32 → ()
;;
;;   type_id   : dense index into cel.abi.types[] — resolves at
;;               Plan time to a google::protobuf::Descriptor* via
;;               DescriptorPool::FindMessageTypeByName(fqn).  Index
;;               0 is the reserved sentinel "no type".
;;   out_slot  : offset of 24B cell to fill with the constructed
;;               message's CelValue.
;;
;; ABI table addition: cel.abi.types[] (parallel to fields[]).
;; Each row is { id: u32, fully_qualified_name: string }.  The
;; descriptor handle is NOT on the wire — DescriptorPool lookup
;; at Plan time is the single source of truth (matches FieldEntry
;; .owner_fqn discipline; m7-proto-literals.md §4.3).
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel_host" "cel_make_message"
          (func $cel_make_message (param i32 i32)))

  (func $eval (result i32)
    ;; ── PRELUDE ──────────────────────────────────────────────
    ;; (no free variables)

    ;; ── RESET ────────────────────────────────────────────────
    (call $arena_reset)

    ;; ── BODY ─────────────────────────────────────────────────
    ;; kStructExpr lowering for `HostMsg3{}`.  type_id=1 is the
    ;; compile-time-assigned handle for "celwasm.testdata.HostMsg3"
    ;; (id 0 is the cel.abi.types[] sentinel); out_slot=16 is the
    ;; workspace slot LayoutPass allocated.  No field-set calls
    ;; (M7.B): the message is left at proto's default-construction.
    (call $cel_make_message
          (i32.const 1)    ;; type_id
          (i32.const 16))  ;; out_slot

    ;; Return the kStructExpr's output offset.
    (i32.const 16))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
