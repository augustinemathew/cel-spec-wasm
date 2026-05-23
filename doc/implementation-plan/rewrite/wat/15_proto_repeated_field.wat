;; CEL source:  c.tags[2]
;; Decl:        c : celwasm.testdata.Customer  (with `repeated string tags`)
;;
;; M4.G slice — proto REPEATED field reads.  ProtoBacking::ReadField
;; on a REPEATED (non-map) field returns
;; `Value::HostList(ProtoList{owner, field})`, which the cel_host
;; trampoline interns into the externref table via InternList; the
;; kSelect emits a `cel_get_field` call that writes
;; `{kind=CEL_LIST_HOST, ref_slot=<list_slot>}` into the
;; intermediate select-result slot.  The kCallExpr `_[_]` arm sees
;; an operand whose `list_origin == kHost` (assigned by
;; ListOriginVisitor on list-typed kSelect nodes) and routes
;; through cel_host.cel_list_at directly.
;;
;; Two host-trampoline calls in the body:
;;   1. cel_host.cel_get_field  → fills c.tags's HostList into a slot
;;   2. cel_host.cel_list_at    → reads element 2 from that HostList
;;
;; Memory layout:
;;   [ 0, 16)  reserved + arena cursor/limit
;;   [16, 40)  workspace: c's slot                  (variable)
;;   [40, 64)  rodata: lookup-index kConst          {CEL_INT, i=2}
;;   [64, 88)  workspace: kSelect result slot       (out=64)
;;             (the select on c.tags returns CEL_LIST_HOST into here)
;;   [88,112)  workspace: kCallExpr result slot     (out=88)
;;   [112, mem_size)  bump arena
;;
;; cel.abi tables:
;;   fields[1]     = (field_number=12, name="tags",
;;                    owner_fqn="celwasm.testdata.Customer")
;;   attributes[1] = ("c", ["tags"])
;;
;; The trampoline reads field_ref_id=1 → looks up FieldDescriptor →
;; calls ProtoBacking::ReadField → the REPEATED arm constructs a
;; ProtoList over the field and interns it.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel_host" "cel_get_field"
          (func $cel_get_field (param i32 i32 i32 i32)))
  (import "cel_host" "cel_has_field"
          (func $cel_has_field (param i32 i32 i32 i32)))
  (import "cel_host" "cel_list_at"
          (func $cel_list_at (param i32 i32 i32)))

  ;; rodata: kind=CEL_INT, payload.i=2.
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $c_off i32)

    (local.set $c_off (i32.const 16))
    (call $arena_reset)

    ;; ── First hop ─ kSelect on c.tags.
    ;; Trampoline reads c's msg_slot, resolves field_ref_id=1
    ;; ("tags"), invokes ProtoBacking::ReadField → REPEATED arm
    ;; → InternList → writes {CEL_LIST_HOST, ref_slot=<n>} into
    ;; out_slot=64.  attribute_id=1 names "c.tags".
    (call $cel_get_field
          (i32.const 64)         ;; out_slot
          (local.get $c_off)     ;; msg_slot
          (i32.const 1)          ;; field_ref_id ("tags")
          (i32.const 1))         ;; attribute_id ("c.tags")

    ;; ── Second hop ─ kCallExpr `_[_]` on the kHost list.
    (call $cel_list_at
          (i32.const 88)         ;; out_slot
          (i32.const 64)         ;; list_slot — the kSelect's output
          (i32.const 40))        ;; index_slot (=2)

    (i32.const 88))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
