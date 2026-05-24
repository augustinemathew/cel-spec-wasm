;; CEL source:  m["k"]
;; Decl:        m : map<string, int>
;;
;; M3.F slice — kCallExpr(`_[_]`) on a kHost operand.  The bound
;; map's `HostMapBacking` (vector-backed via Activation::Bind, or
;; ProtoMap if from a proto field) is interned in the per-Instance
;; ExternrefTable; the map CelValue carries `{kind=CEL_MAP_HOST,
;; payload.ref_slot=<slot>}` rather than an arena header pointer.
;; ResolvePass's MapOriginVisitor stamped `map_origin = kHost` on
;; the kIdent, so codegen routes the index call directly through
;; the cel_host trampoline — no runtime dispatcher trip, no
;; arena helpers in the body.
;;
;; Memory layout:
;;   [ 0, 16)  null sentinel (arena state lives in runtime BSS)
;;   [16, 40)  workspace slot for `m`           (variable bound by host)
;;   [40, 64)  rodata: lookup-key kConst        {CEL_STRING, "k"}
;;   [64, 65)  rodata: span payload "k"         (1 byte)
;;   [72, 96)  workspace: kCallExpr result slot (out=72; aligned past
;;             rodata which ends at 65 — the codegen aligns up to 8
;;             via RoundUp8 in layout_pass.cc)
;;   [96+]  bump arena (malloc'd in heap)
;;
;; The runtime body is one extern call into the host trampoline.
;; No cel_map_create / cel_map_insert (the operand was constructed
;; host-side, not in the arena).
;;
;; New import this milestone (vs. M2):
;;   cel_host.cel_map_lookup(out_slot, map_slot, key_slot) — i32×3 → ()
;;
;; The Layer-3 wasmtime trampoline (api/internal/cel_host_wasmtime.cc)
;; reads the ref_slot from the map operand, looks up the
;; HostMapBacking via ExternrefTable::LookupMap, decodes the key
;; CelValue, calls HostMapBacking::Get, encodes the result back
;; into out_slot through EncodeFieldResult.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel_host" "cel_map_lookup"
          (func $cel_map_lookup (param i32 i32 i32)))

  ;; rodata: kind=CEL_STRING(5), _pad, span.ptr=64, span.len=1, pad8.
  ;; Span payload "k" lives at [64, 65) so ptr=64, len=1.
  (data (i32.const 40)
        "\05\00\00\00" "\00\00\00\00"
        "\40\00\00\00" "\01\00\00\00"
        "\00\00\00\00\00\00\00\00")
  (data (i32.const 64) "k")

  (func $eval (result i32)
    (local $m_off i32)

    ;; Variable prelude — `m` lives at workspace slot 16.
    (local.set $m_off (i32.const 16))

    ;; Reset — arena begins at 96.
    (call $arena_reset)

    ;; kHost arm of `_[_]`: route directly to the cel_host trampoline.
    (call $cel_map_lookup
          (i32.const 72)        ;; out_slot
          (local.get $m_off)    ;; map_slot
          (i32.const 40))       ;; key_slot (rodata "k")

    (i32.const 72))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
