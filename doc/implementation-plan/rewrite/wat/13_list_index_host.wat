;; CEL source:  xs[0]
;; Decl:        xs : list<int>
;;
;; M4.F slice — kCallExpr(`_[_]`) on a kHost list operand.  The
;; bound `HostListBacking` (vector-backed via Activation::Bind, or
;; ProtoList if from a proto repeated field) is interned in the
;; per-Instance ExternrefTable; the list CelValue carries
;; `{kind=CEL_LIST_HOST, payload.ref_slot=<slot>}`.
;; ResolvePass's ListOriginVisitor stamped `list_origin = kHost`
;; on the kIdent (because its declared type is list<int>), so
;; codegen routes the index call directly through the cel_host
;; trampoline — no runtime dispatcher trip, no arena helpers in
;; the body.
;;
;; Memory layout:
;;   [ 0, 16)  null sentinel (arena state lives in runtime BSS)
;;   [16, 40)  workspace slot for `xs`     (variable bound by host)
;;   [40, 64)  rodata: lookup-index kConst {CEL_INT, i=0}
;;   [64, 88)  workspace: kCallExpr result slot (out=64)
;;   [88+]  bump arena (malloc'd in heap)
;;
;; The runtime body is one extern call into the host trampoline.
;; No cel_list_create / cel_list_append_at (the operand was constructed
;; host-side, not in the arena).
;;
;; New import this milestone (vs. M3):
;;   cel_host.cel_list_at(out_slot, list_slot, index_slot) — i32×3 → ()
;;
;; The Layer-3 wasmtime trampoline (api/internal/cel_host_wasmtime.cc
;; via HostThreeArgTrampoline<CelListAtImpl>) reads the ref_slot
;; from the list operand, looks up the HostListBacking via
;; ExternrefTable::LookupList, decodes the index CelValue
;; (must be CEL_INT; non-int → CEL_ERR_TYPE_MISMATCH), bounds-
;; checks against backing->Size(), calls HostListBacking::At,
;; encodes the result back into out_slot via EncodeFieldResult.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel_host" "cel_list_at"
          (func $cel_list_at (param i32 i32 i32)))

  ;; rodata: kind=CEL_INT, payload.i=0.
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $xs_off i32)

    (local.set $xs_off (i32.const 16))
    (call $arena_reset)

    ;; kHost arm of `_[_]`: directly to the cel_host trampoline.
    (call $cel_list_at
          (i32.const 64)         ;; out_slot
          (local.get $xs_off)    ;; list_slot — kind=CEL_LIST_HOST
          (i32.const 40))        ;; index_slot

    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
