;; CEL source:  google.protobuf.Int32Value{value: 5}
;; Decl:        no free variables
;;
;; M8.C — kStructExpr wrapper tail-unwrap.  Direct clone of m7b's
;; `cel_wkt_unwrap_time` shape (see `wat-traces.md` §51 walkthrough
;; and `compiler_v2/codegen/expr_lower.cc::MaybeEmitWktUnwrapTailCall`
;; for the m7b codegen seam).  Differs from m7b in two places:
;;
;;   1.  Trampoline is `cel_host.cel_wkt_unwrap_wrapper` (NOT
;;       `cel_wkt_unwrap_time`).  Three i32 args — the third is a
;;       `wrapper_kind` enum (see below) so the Layer-2 impl avoids
;;       a per-call descriptor walk on the hot path; codegen knows
;;       the kind statically from the kStructExpr FQN.
;;   2.  Result `CelKind` is one of {CEL_BOOL, CEL_INT, CEL_UINT,
;;       CEL_DOUBLE, CEL_STRING, CEL_BYTES} depending on
;;       `wrapper_kind`; m7b's analog always writes CEL_TIMESTAMP /
;;       CEL_DURATION.
;;
;; Why this is the kStructExpr tail call, not a separate AST node:
;; the typed_ast Repr mapping at `compiler_v2/ir/typed_ast.cc:56`
;; already maps `wrapper(IntXX)` → `Repr::kInt` (and the
;; `wrapper(YY)` → `Repr::kYY` siblings).  Every consumer downstream
;; of the kStructExpr (equality, arithmetic-check, list-element-
;; assignment) expects a scalar slot at the wrapper-typed expression's
;; storage.  Without the tail-unwrap, kStructExpr would leave a
;; `CEL_MESSAGE` slot at `out_slot`, and the next pass would either
;; CHECK-fail or silently miscompile.  See m8-wrapper-types.md §4.2
;; for the full trace of `Int32Value{value: 1} == 1`.
;;
;; `wrapper_kind` enum.  Numerically equal to the CelKind the
;; trampoline writes — keeps the dispatch table at the Layer-2 impl
;; trivial (one switch on `wrapper_kind` selecting both the
;; descriptor FQN to cross-check AND the matching CelValue.kind to
;; emit).  Values match `compiler_v2/runtime/cel_data.h::CelKind`:
;;
;;     CEL_BOOL   = 1   ←→  google.protobuf.BoolValue
;;     CEL_INT    = 2   ←→  google.protobuf.Int32Value  / Int64Value
;;     CEL_UINT   = 3   ←→  google.protobuf.UInt32Value / UInt64Value
;;     CEL_DOUBLE = 4   ←→  google.protobuf.FloatValue  / DoubleValue
;;     CEL_STRING = 5   ←→  google.protobuf.StringValue
;;     CEL_BYTES  = 6   ←→  google.protobuf.BytesValue
;;
;; (Int32/Int64 collapse onto kInt and UInt32/UInt64 onto kUInt
;; because CEL's value algebra has no 32-vs-64 distinction; the
;; checker sign-extends / range-checks at the proto boundary.
;; Float/Double similarly collapse onto kDouble.)
;;
;; Memory layout:
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  workspace slot for kStructExpr — out_slot of
;;             cel_make_message; mutated in place by cel_set_field;
;;             OVERWRITTEN in place by cel_wkt_unwrap_wrapper with
;;             the peeled scalar CelValue.  This is the SAME slot
;;             across all three operations — the kStructExpr's
;;             storage doesn't grow, the message exists only as a
;;             transient externref between cel_make_message and
;;             cel_wkt_unwrap_wrapper.  After unwrap the slot
;;             contains:
;;               kind        = CEL_INT (2)
;;               payload.i   = 5
;;             (and the ExternrefTable entry is released — see the
;;              m7b walkthrough on `wat-traces.md` §51 for the
;;              identical lifetime story.)
;;   [40, 64)  rodata: literal CelValue for the entry `5` (kInt) —
;;             read-only operand passed to cel_set_field.  See
;;             `41_kstruct_set_scalar.wat` for the same convention.
;;   [64+]  bump arena (malloc'd in heap) (untouched for this construction —
;;             the wrapper message lives in the ExternrefTable, not
;;             in linear memory; the peeled scalar is by-value in
;;             the CelValue payload, so no arena allocation needed).
;;
;; New import this slice:
;;   cel_host.cel_wkt_unwrap_wrapper(out_slot, msg_slot, wrapper_kind)
;;     — 3 × i32 → ()
;;
;;     out_slot     : offset of the 24B CelValue cell to overwrite
;;                    with the peeled scalar.  In the kStructExpr
;;                    tail-call shape, this is the SAME offset as
;;                    msg_slot (the message slot is consumed and
;;                    replaced in place).
;;     msg_slot     : offset of the 24B CelValue carrying
;;                    {CEL_MESSAGE, payload.msg_slot=<externref>}
;;                    for the just-constructed wrapper proto.
;;     wrapper_kind : i32 enum (CEL_BOOL=1 .. CEL_BYTES=6) tagging
;;                    which of the 9 wrapper FQNs.  Layer-2 impl
;;                    cross-checks `descriptor()->full_name()`
;;                    against `wrapper_kind` — mismatch is a
;;                    codegen regression, surfaces as
;;                    {CEL_ERROR, CEL_ERR_TYPE_MISMATCH} written to
;;                    out_slot (defence in depth; codegen only
;;                    emits the call when the FQN matches).
;;
;; 3VL absorption: if msg_slot already holds CEL_UNKNOWN or
;; CEL_ERROR (rare — kStructExpr operands' 3VL is absorbed at the
;; cel_set_field level), the trampoline propagates it verbatim to
;; out_slot.  Mirrors `CelWktUnwrapTimeImpl`'s shape in
;; `compiler_v2/api/internal/cel_host.cc:3119-3122`.
;;
;; Codegen shape (emitted by
;; `compiler_v2/codegen/expr_lower.cc::EmitKStructExpr` once
;; `MaybeEmitWktUnwrapTailCall` is extended to recognise the 9
;; wrapper FQNs):
;;
;;   1. Emit cel_make_message(type_id=Int32Value, out=msg_slot).
;;   2. Emit cel_set_field(msg_slot, value_field, scalar_rodata).
;;      Field-name "value" resolves at host time against the
;;      wrapper descriptor.
;;   3. Emit cel_wkt_unwrap_wrapper(out=msg_slot, in=msg_slot,
;;      kind=2 [CEL_INT]).  Tail-call form — overwrites slot in
;;      place (matches m7b's `(out_slot, out_slot)` argument
;;      pattern in MaybeEmitWktUnwrapTailCall).
;;   4. Return msg_slot as the i32 result of $eval.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel_host" "cel_make_message"
          (func $cel_make_message (param i32 i32)))
  (import "cel_host" "cel_set_field"
          (func $cel_set_field (param i32 i32 i32)))
  (import "cel_host" "cel_wkt_unwrap_wrapper"
          (func $cel_wkt_unwrap_wrapper (param i32 i32 i32)))

  ;; Rodata for the literal `5`.  CelValue layout (matches the
  ;; convention in 41_kstruct_set_scalar.wat):
  ;;   [ 0,  4) kind = CEL_INT (2)
  ;;   [ 4,  8) _pad
  ;;   [ 8, 16) payload.i = 5 (i64, little-endian)
  ;;   [16, 24) padding to 24-byte stride
  (data (i32.const 40)
        "\02\00\00\00"
        "\00\00\00\00"
        "\05\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    ;; ── PRELUDE ──────────────────────────────────────────────
    ;; (no free variables)

    ;; ── RESET ────────────────────────────────────────────────
    ;; Arena base = 64 (past workspace slot at 16 + rodata at 40,
    ;; rounded up to 24-byte CelValue alignment).
    (call $arena_reset)

    ;; ── BODY ─────────────────────────────────────────────────
    ;; M7.A — construct the default Int32Value proto into slot 16.
    ;; type_id=1 here is a stand-in for the Plan-assigned dense
    ;; index of "google.protobuf.Int32Value" in cel.abi.types[];
    ;; the actual id is unobservable from the WAT (just a u32 the
    ;; host resolves).
    (call $cel_make_message
          (i32.const 1)    ;; type_id = "google.protobuf.Int32Value"
          (i32.const 16))  ;; out_slot

    ;; M7.B — set field `value` to 5.  field_ref_id=1 in
    ;; cel.abi.fields[] resolves to (name="value", owner_fqn=
    ;; "google.protobuf.Int32Value", field_number=0); the host's
    ;; SetScalarField path dispatches on the resolved
    ;; FieldDescriptor's cpp_type (CPPTYPE_INT32) and calls
    ;; Reflection::SetInt32.
    (call $cel_set_field
          (i32.const 16)   ;; msg_slot
          (i32.const 1)    ;; field_ref_id = "Int32Value.value"
          (i32.const 40))  ;; value_slot (rodata literal `5`)

    ;; M8.C — wrapper tail-unwrap.  Overwrites slot 16 IN PLACE
    ;; (msg_slot == out_slot) with the peeled scalar.  Kind=2 is
    ;; CEL_INT (matches the wrapper's inner field type — see the
    ;; header comment's wrapper_kind enum).
    (call $cel_wkt_unwrap_wrapper
          (i32.const 16)   ;; out_slot (in-place overwrite)
          (i32.const 16)   ;; msg_slot
          (i32.const 2))   ;; wrapper_kind = CEL_INT

    ;; Return the kStructExpr's output offset.  After unwrap,
    ;; slot 16 holds {CEL_INT(2), payload.i=5}.
    (i32.const 16))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
