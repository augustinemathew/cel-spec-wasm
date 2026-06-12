;; CEL source:  m.inner.inner.i64
;; Decl:        m : celwasm.testdata.HostMsg3
;;
;; Batched proto select chain — ONE host crossing for the whole
;; contiguous message-typed chain instead of one cel_get_field call
;; per hop.  The unbatched lowering of this expression is three
;; cel_get_field calls (≈104 ns of boundary cost per hop); the
;; batched lowering is a single cel_get_field_path call whose
;; interned path row carries the per-hop (field_ref_id,
;; attribute_id) pairs, and the host walks the hops entirely in
;; C++ (no intermediate CelValue encode / externref intern / wasm
;; re-entry between hops).
;;
;; Memory layout:
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  workspace slot for `m`            — local_index 0
;;   [40, 64)  workspace slot for the OUTERMOST select — the batched
;;             call's out_slot.  (LayoutPass still allocates one slot
;;             per intermediate select node; the batched lowering
;;             simply never writes the intermediate slots — they are
;;             consumed by nothing else, since the AST is a tree and
;;             each intermediate select node is referenced only as
;;             the next hop's operand.)
;;   [64+]     bump arena (malloc'd in heap)
;;
;; New import this surface:
;;   cel_host.cel_get_field_path(out_slot, msg_slot, path_ref_id)
;;       — 3 × i32 → ()  (i32-only per the unchecked-ABI invariant,
;;         abi/runtime_catalogue.h "LOAD-BEARING")
;;
;;   out_slot     : offset of the 24B cell to fill with the FINAL
;;                  hop's CelValue (scalar inline / span via arena /
;;                  aggregate via externref intern — byte-identical
;;                  to what the last unbatched cel_get_field call
;;                  would have written).
;;   msg_slot     : offset of the 24B cell holding the root operand's
;;                  message CelValue (kind=CEL_MESSAGE,
;;                  payload.msg_slot=<externref idx>).
;;   path_ref_id  : dense index into cel.abi.paths[] — resolves to an
;;                  ordered hop list, innermost (root-adjacent) hop
;;                  first.  Each hop is a (field_ref_id, attribute_id)
;;                  pair:
;;                    field_ref_id → cel.abi.fields[] row (the SAME
;;                      per-hop rows the unbatched calls would
;;                      reference; the eval side reuses each row's
;;                      per-site ResolvedFieldCache untouched);
;;                    attribute_id → cel.abi.attributes[] row of THAT
;;                      hop's OPERAND — exactly the id the unbatched
;;                      per-hop cel_get_field call would have passed,
;;                      so partial-eval unknown matching (and the id
;;                      carried inside a minted UnknownSet) is
;;                      byte-identical per hop.
;;
;; No attribute_id call argument: the per-hop ids live in the path
;; row (they differ per hop — passing only the root's id would lose
;; the per-prefix pattern-match parity).
;;
;; Batching eligibility (codegen-side, static):
;;   - every hop is a non-test_only kSelect taking the proto branch;
;;   - every INTERMEDIATE node (each hop's operand past the root) has
;;     Repr::kMessage and a message FQN outside the WKT set
;;     {9 wrappers, Any, Timestamp, Duration, Struct, Value,
;;      ListValue} — i.e. its read class is kMessagePlain, so the
;;     host-side walk's "stay in proto reflection" step is exactly
;;     what the per-hop trampolines would have done.  Optional-,
;;     map-, and WKT-typed intermediates fall back to the existing
;;     per-hop lowering (semantics outrank the optimization);
;;   - chains shorter than 2 hops keep the single cel_get_field call.
;;   The FINAL hop's field may be anything (scalar / map / repeated /
;;   wrapper / Any / message): its encode path is shared verbatim
;;   with cel_get_field's.
;;
;; ABI tables (cel.abi.fields[], cel.abi.attributes[], cel.abi.paths[])
;; are decoded once at Engine::Plan time and threaded through as
;; callback data to the trampoline.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel_host" "cel_get_field_path"
          (func $cel_get_field_path (param i32 i32 i32)))

  (func $eval (result i32)
    (local $m_off i32)

    ;; ── PRELUDE ──────────────────────────────────────────────
    (local.set $m_off (i32.const 16))

    ;; ── RESET ────────────────────────────────────────────────
    (call $arena_reset)

    ;; ── BODY ─────────────────────────────────────────────────
    ;; Batched kSelect chain: one call covers all three hops.
    ;; path_ref_id=1 resolves to hops
    ;;   [(field "inner" on HostMsg3, attr "m"),
    ;;    (field "inner" on HostMsg3, attr "m.inner"),
    ;;    (field "i64"   on HostMsg3, attr "m.inner.inner")]
    ;; The host walks hop-by-hop in C++ and writes ONLY the final
    ;; hop's CelValue at out_slot.
    (call $cel_get_field_path
          (i32.const 40)     ;; out_slot   (outermost select's slot)
          (local.get $m_off) ;; msg_slot   (root operand `m`)
          (i32.const 1))     ;; path_ref_id

    ;; Return the outermost select's output offset.
    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
