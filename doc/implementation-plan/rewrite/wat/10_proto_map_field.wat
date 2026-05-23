;; CEL source:  c.metadata["k"]
;; Decl:        c : celwasm.testdata.Customer
;;
;; M3.G slice — proto MAP-field reads.  ProtoBacking::ReadField on
;; a MAP field returns `Value::HostMap(ProtoMap{owner, field})`,
;; which the cel_host trampoline interns into the externref table
;; via InternMap; the kSelect emits a `cel_get_field` call that
;; writes `{kind=CEL_MAP_HOST, ref_slot=<map_slot>}` into the
;; intermediate select-result slot.  The kCallExpr `_[_]` arm sees
;; an operand whose `map_origin == kHost` (assigned by
;; MapOriginVisitor on map-typed kSelect nodes) and routes through
;; cel_host.cel_map_lookup directly.
;;
;; Two host-trampoline calls in the body:
;;   1. cel_host.cel_get_field → fills c.metadata's HostMap into a slot
;;   2. cel_host.cel_map_lookup → Looks up "k" in that HostMap
;;
;; Memory layout:
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  workspace: c's slot                 (variable)
;;   [40, 64)  rodata: lookup-key kConst           {CEL_STRING, "k"}
;;   [64, 65)  rodata: span payload "k"
;;   [72, 96)  workspace: kSelect result slot
;;             (the select on c.metadata returns a CEL_MAP_HOST CelValue
;;              into this slot; out_slot=72)
;;   [96,120)  workspace: kCallExpr result slot    (out=96)
;;   [120+]  bump arena (malloc'd in heap)
;;
;; cel.abi tables (populated at compile time, decoded at Plan time):
;;   fields[1]     = (field_number=10, name="metadata",
;;                    owner_fqn="celwasm.testdata.Customer")
;;   attributes[1] = ("c", ["metadata"])
;;
;; The trampoline reads field_ref_id=1 → looks up FieldDescriptor →
;; calls ProtoBacking::ReadField → the MAP-field arm constructs a
;; ProtoMap and interns it.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel_host" "cel_get_field"
          (func $cel_get_field (param i32 i32 i32 i32)))
  (import "cel_host" "cel_has_field"
          (func $cel_has_field (param i32 i32 i32 i32)))
  (import "cel_host" "cel_map_lookup"
          (func $cel_map_lookup (param i32 i32 i32)))

  ;; rodata: "k" string CelValue + payload.
  (data (i32.const 40)
        "\05\00\00\00" "\00\00\00\00"
        "\40\00\00\00" "\01\00\00\00"
        "\00\00\00\00\00\00\00\00")
  (data (i32.const 64) "k")

  (func $eval (result i32)
    (local $c_off i32)

    (local.set $c_off (i32.const 16))
    (call $arena_reset)

    ;; ── First hop ─ kSelect on c.metadata.
    ;; Trampoline reads c's msg_slot, resolves field_ref_id=1
    ;; ("metadata"), invokes ProtoBacking::ReadField → MAP arm
    ;; → InternMap → writes {CEL_MAP_HOST, ref_slot=<n>} into
    ;; out_slot=72.  attribute_id=1 names the path "c.metadata"
    ;; for unknown-pattern matching under PartialEval.
    (call $cel_get_field
          (i32.const 72)         ;; out_slot
          (local.get $c_off)     ;; msg_slot
          (i32.const 1)          ;; field_ref_id ("metadata")
          (i32.const 1))         ;; attribute_id ("c.metadata")

    ;; ── Second hop ─ kCallExpr `_[_]` on the kHost map.
    (call $cel_map_lookup
          (i32.const 96)         ;; out_slot
          (i32.const 72)         ;; map_slot — the kSelect's output
          (i32.const 40))        ;; key_slot ("k")

    (i32.const 96))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
