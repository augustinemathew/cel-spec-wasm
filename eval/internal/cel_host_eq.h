// Origin-agnostic deep value equality over wire CelValues — the
// single entry point (`WireValueEq`) every host-side element / value
// compare funnels through, plus the list / map equality walks and
// the proto message-equality core it recurses into.  Shared by the
// list, map, and message trampoline TUs.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_EQ_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_EQ_H_

#include <cstddef>
#include <cstdint>

#include "absl/base/nullability.h"
#include "absl/status/statusor.h"
#include "eval/internal/cel_host_common.h"
#include "eval/internal/cel_host_memory.h"
#include "eval/value.h"
#include "runtime/cel_data.h"

namespace google::protobuf {
class Message;
}  // namespace google::protobuf

namespace celwasm {

// ══════════════════════════════════════════════════════════════════
// Aggregate-op kHost trampolines.
//
// The seven dispatchers in `cel_runtime.c` (`cel_list_size` /
// `cel_list_in` / `cel_list_eq` / `cel_list_concat` / `cel_map_size`
// / `cel_map_in` / `cel_map_eq`) tail-call here when the operand
// origin is `CEL_LIST_HOST` or `CEL_MAP_HOST`.  Each Impl reads its
// operand backing(s) via `ctx.refs.LookupList` / `LookupMap`, runs
// the corresponding spec-level operation, and writes the result
// CelValue into `out_slot`.
//
// Element / value equality is ORIGIN-AGNOSTIC and DEEP: `WireValueEq`
// dispatches on wire kind, recursing into `ListsEqual` /
// `NormalizedMapEq` for nested aggregates and `CompareProtoMessages`
// for messages, so `xss == xss` and `[1, 2] in xss` answer the same
// as their arena-literal twins.  The observable CEL semantics must
// not depend on whether an operand was built by codegen or bound
// through `Activation::Bind`.
// ══════════════════════════════════════════════════════════════════

// Origin-agnostic DEEP equality of two wire CelValues per langdef
// §"Equality": scalars by value across the numeric ladder, lists
// element-wise, maps as key/value sets, messages via
// MessageDifferencer — independent of whether either side is
// arena-built or host-backed.  Defined below the list- and
// map-equality walks it recurses through; declared here because
// those walks call back into it for their element / value compares.
// Non-OK Status only on infrastructure failure (bad ref_slot,
// backing read error).
absl::StatusOr<bool> WireValueEq(const CelValue& a, const CelValue& b,
                                 const TrampolineContext& ctx);

// Tri-state outcome of proto-message equality (Any peel + descriptor
// check + MessageDifferencer).  kNotComparable covers operands
// without an underlying proto (custom non-proto backings) and Any
// payloads that fail to unpack.  Defined with the message-equality
// trampoline below; shared with the list-equality walk so message
// elements compare with full proto semantics.
enum class ProtoMessageEqOutcome : uint8_t { kEqual, kUnequal, kNotComparable };

ProtoMessageEqOutcome CompareProtoMessages(
    const google::protobuf::Message* absl_nullable a,
    const google::protobuf::Message* absl_nullable b);

// Direct equality of a backing-side `celwasm::Value` against the
// already-decoded wire query `query_cv`.  For SCALARS this skips the
// per-element encode + arena allocation that an At()-loop would pay —
// for a 1000-element 50-byte-string scan that allocation alone was
// ~50us / scan (measured 2026-06-03; see
// `BM_Eval_In_IamPermissions_Bound_Last/1000` vs the cel-cpp sibling).
// Aggregate elements can't be compared without materialising a wire
// handle, so those arms encode first and then recurse through
// `WireValueEq`; the scalar fast paths are unchanged, so the hot
// `<string> in <bound list>` scan pays nothing for it.
//
// Cross-numeric (int / uint / double) routes through
// `HostNumericCrossEq` against a synthesised CelValue prototype so
// the langdef §"Equality" mathematical-value rule holds for
// `1 in [1u, 2u]`.
// Compare a numeric backing scalar (int / uint / double) against a
// wire query CelValue.  `same_kind` is the wire kind matching
absl::StatusOr<bool> BackingValueEqualsQuery(const celwasm::Value& bv,
                                             const CelValue& query_cv,
                                             const TrampolineContext& ctx);

// Returns the count of `cv` whether arena or host.  Caller has
// already verified kind ∈ {CEL_LIST_ARENA, CEL_LIST_HOST}.
absl::StatusOr<size_t> ListLength(const CelValue& cv,
                                  const TrampolineContext& ctx);

// Read the i-th element of a CEL_LIST_ARENA via the MemoryView.
// `cv` must be CEL_LIST_ARENA; caller has verified.
CelValue ReadArenaListElement(const CelValue& cv, uint32_t i,
                              const MemoryView& mem);

// Length-then-element-wise equality of two list operands of any
// origin pair.  Caller has verified both kinds are list-shaped.
// The entry point `WireValueEq` recurses through for nested lists.
absl::StatusOr<bool> ListsEqual(const CelValue& a_cv, const CelValue& b_cv,
                                const TrampolineContext& ctx);

// Set-equality of two map operands of any origin pair, via the
// normalized snapshots above (langdef §"Equality" — map order is
// irrelevant).  Caller has verified both kinds are map-shaped.
// Returns the boolean answer; non-OK Status only on infrastructure
// failure (bad ref_slot, backing error).
absl::StatusOr<bool> NormalizedMapEq(const CelValue& a_cv, const CelValue& b_cv,
                                     const TrampolineContext& ctx);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_EQ_H_
