// Layer-2 trampoline entry points for the message-family
// `cel_host.*` imports: field get/has, message equality / zero-value
// probe, proto-literal construction (`cel_make_message` /
// `cel_set_field`), type-name resolution, and the WKT
// literal-unwrap bridges.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_MESSAGE_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_MESSAGE_H_

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "eval/internal/cel_host_common.h"

namespace celwasm {

// Trampoline entry points.  Both write their result CelValue to
// `out_slot` in `ctx.mem`.  Absorbs UNKNOWN / ERROR on the input;
// consults `unknown_patterns` before calling Layer 1; marshals the
// returned Value (scalars inline, spans via arena, messages via
// Intern).  Non-OK Status only on infrastructure failure.
ABSL_MUST_USE_RESULT absl::Status CelGetFieldImpl(uint32_t out_slot,
                                                  uint32_t msg_slot,
                                                  uint32_t field_ref_id,
                                                  uint32_t attribute_id,
                                                  const TrampolineContext& ctx);

ABSL_MUST_USE_RESULT absl::Status CelHasFieldImpl(uint32_t out_slot,
                                                  uint32_t msg_slot,
                                                  uint32_t field_ref_id,
                                                  uint32_t attribute_id,
                                                  const TrampolineContext& ctx);

// Polymorphic message equality.  Both operands must be
// `CEL_MESSAGE` with valid `payload.msg_slot` ref-slots; either
// operand UNKNOWN / ERROR propagates 3VL.  Uses
// `google::protobuf::util::MessageDifferencer::Equals` over the
// underlying `HostMessageBacking::Message()` per langdef §"Equality".
// `cel_message_eq` is a standalone helper for the polymorphic
// `cel_equals_at_vv` ladder; not one of the seven dispatchers above.
ABSL_MUST_USE_RESULT absl::Status CelMessageEqImpl(
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot,
    const TrampolineContext& ctx);

// cel_host.cel_message_is_zero — proto-message zero-value probe for
// `optional.ofNonZeroValue`.  Reads the CEL_MESSAGE CelValue at
// `msg_slot`, dereferences `payload.msg_slot` via `ctx.refs`, and
// writes a CEL_BOOL at `out_slot`: true iff the backing proto has no
// set fields (`Reflection::ListFields` is empty) AND an empty
// unknown-field set — cel-cpp parity with
// `ParsedMessageValue::IsZeroValue()`
// (third_party/cel-cpp/common/values/parsed_message_value.cc:78).
// UNKNOWN / ERROR operands propagate 3VL; a non-CEL_MESSAGE operand
// poisons kTypeMismatch; an unmapped msg_slot or a non-proto custom
// backing (`message() == nullptr` — no reflection to walk, and
// `HostMessageBacking` has no zero-value hook) poisons
// kHostAdapterError.  The wasm caller treats any non-BOOL result as
// "non-zero" so the operand propagates as Some instead of vanishing.
ABSL_MUST_USE_RESULT absl::Status CelMessageIsZeroImpl(
    uint32_t out_slot, uint32_t msg_slot, const TrampolineContext& ctx);

// cel_host.cel_make_message — proto literal construction.
//   1. Resolve `type_id` against `bindings.message_types` →
//      `Descriptor*` (kTypeMismatch CEL_ERROR if id is the sentinel
//      or out-of-range, or the descriptor was not in the pool).
//   2. `MessageFactory::generated_factory()->GetPrototype(desc)
//      ->New()` — allocates a default-constructed proto.
//   3. Wrap in `OwnedProtoBacking(unique_ptr<Message>)` for owning
//      lifetime semantics — the ExternrefTable's per-Eval `Reset()`
//      drops the shared_ptr, freeing the message.
//   4. `ctx.refs.Intern(shared_ptr<OwnedProtoBacking>)` → `slot`.
//   5. Write `{ kind: CEL_MESSAGE, payload.msg_slot = slot }` to
//      the `out_slot` cell in `ctx.mem`.
// Non-OK Status only on infrastructure failure (descriptor pool
// lookup mismatched against the FQN at Plan time but the
// trampoline can't reach the pool now); spec-level errors travel
// inside out_slot as CEL_ERROR.
ABSL_MUST_USE_RESULT absl::Status CelMakeMessageImpl(
    uint32_t type_id, uint32_t out_slot, const TrampolineContext& ctx);

// cel_host.resolve_message_type_name — descriptor-FQN resolution
// for `type(<message>)`.  Reads the CEL_MESSAGE CelValue at
// `in_slot`, dereferences `payload.msg_slot` against `ctx.refs` to
// recover the `HostMessageBacking`, walks to the proto's
// `GetDescriptor()->full_name()`, copies the FQN bytes into the
// per-Eval arena via `ctx.alloc`, stamps
// `{kind: CEL_TYPE, payload.s: {arena_off, len}}` into out_slot.
ABSL_MUST_USE_RESULT absl::Status CelResolveMessageTypeNameImpl(
    uint32_t out_slot, uint32_t in_slot, const TrampolineContext& ctx);

// cel_host.cel_set_field — proto literal field set.
//   1. Read `msg_cv` from `msg_slot` — must be CEL_MESSAGE; the
//      msg_slot externref must point at an `OwnedProtoBacking`
//      (cast via `dynamic_cast` so externally-bound, non-mutable
//      ProtoBackings can't be mutated through this path —
//      kTypeMismatch trap).
//   2. Resolve `field_ref_id` against `bindings.field_refs` →
//      `(field_number, field_name)`; the host then resolves the
//      `FieldDescriptor*` by name on the message's descriptor
//      (mirrors `ProtoBacking::ResolveFieldDescriptor`).
//   3. Read `value_cv` from `value_slot`.  Dispatch on the
//      field's `cpp_type`:
//        BOOL  → SetBool   (value: CEL_BOOL)
//        INT32 → SetInt32  (value: CEL_INT)
//        INT64 → SetInt64  (value: CEL_INT)
//        UINT32 → SetUInt32 (value: CEL_UINT)
//        UINT64 → SetUInt64 (value: CEL_UINT)
//        FLOAT  → SetFloat  (value: CEL_DOUBLE)
//        DOUBLE → SetDouble (value: CEL_DOUBLE)
//        STRING → SetString (value: CEL_STRING / CEL_BYTES depending
//                            on field type — span bytes via mem)
//        ENUM   → SetEnumValue (value: CEL_INT — langdef
//                               §"Enumerated Types")
//   4. Repeated, map, and singular-message field shapes route
//      through dedicated walkers in `cel_host_set_field.cc`; see
//      `cel-host-surface.md` for the per-shape dispatch.
// Non-OK Status surfaces as a wasm trap; the conformance harness
// records the row as failure without aborting the run.
ABSL_MUST_USE_RESULT absl::Status CelSetFieldImpl(uint32_t msg_slot,
                                                  uint32_t field_ref_id,
                                                  uint32_t value_slot,
                                                  const TrampolineContext& ctx);

// Bridge for `Timestamp{...}` / `Duration{...}` proto-literal
// construction.  Reads `msg_slot` (expected CEL_MESSAGE
// of WKT time-type descriptor), peels `(seconds, nanos)` via
// reflection, writes a `CEL_TIMESTAMP` / `CEL_DURATION` CelValue at
// `out_slot`.  Non-WKT or non-message operands → CEL_ERROR
// (kTypeMismatch); codegen emits this call only for WKT struct
// literals, so the only way to reach the error path is a codegen
// regression.
ABSL_MUST_USE_RESULT absl::Status CelWktUnwrapTimeImpl(
    uint32_t out_slot, uint32_t msg_slot, const TrampolineContext& ctx);

// Bridge for the 9 wrapper proto-literal types
// (`google.protobuf.{Bool,Int32,Int64,UInt32,UInt64,Float,Double,
// String,Bytes}Value`).  Three-arg `(out_slot, msg_slot,
// wrapper_kind)` — reads the CEL_MESSAGE at `msg_slot`, peels the
// inner `value` field via reflection, writes the matching scalar
// CelValue (CEL_BOOL / CEL_INT / CEL_UINT / CEL_DOUBLE / CEL_STRING
// / CEL_BYTES) at `out_slot`.  `wrapper_kind` is the expected inner
// CelKind (1..6 per `cel_data.h::CelKind`) — Layer-2 cross-checks
// against the descriptor's actual cpp_type and writes a
// `CEL_ERR_TYPE_MISMATCH` poison on regression.  Direct clone of
// `CelWktUnwrapTimeImpl` for the 9 wrapper FQNs.
ABSL_MUST_USE_RESULT absl::Status CelWktUnwrapWrapperImpl(
    uint32_t out_slot, uint32_t msg_slot, uint32_t wrapper_kind,
    const TrampolineContext& ctx);

// Resolve `field_ref_id` against the `bindings.field_refs` table.
// Returns nullptr on OOR / sentinel; caller writes
// CEL_ERR_FIELD_NOT_FOUND.
// Shared between the field trampolines (cel_host_message.cc) and
// `CelSetFieldImpl` (cel_host_set_field.cc).
const FieldRefEntry* absl_nullable ResolveFieldRef(
    const CelHostBindings& bindings, uint32_t field_ref_id);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_MESSAGE_H_
