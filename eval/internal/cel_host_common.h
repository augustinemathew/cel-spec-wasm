// cel_host Layer-2 shared vocabulary: the ExternrefTable
// abstraction, the per-Plan / per-eval binding structs
// (CelHostBindings / TrampolineContext), and the shared CelValue
// encode helpers every trampoline marshals results through.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_COMMON_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_COMMON_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "eval/attribute.h"
#include "eval/internal/cel_host_backing.h"
#include "eval/internal/cel_host_memory.h"
#include "eval/value.h"
#include "runtime/cel_data.h"

namespace celwasm {

// Opaque u32 slots back `CelValue.payload.msg_slot`.  Intern is
// monotonic per Eval; Reset() clears between Evals.
class ExternrefTable {
 public:
  virtual ~ExternrefTable() = default;

  ExternrefTable() = default;
  ExternrefTable(const ExternrefTable&) = delete;
  ExternrefTable& operator=(const ExternrefTable&) = delete;

  virtual uint32_t Intern(
      std::shared_ptr<const HostMessageBacking> backing) = 0;
  virtual const HostMessageBacking* absl_nullable Lookup(
      uint32_t slot) const = 0;

  // Same intern/lookup contract for map backings.  Slot
  // namespaces are independent — `LookupMap(slot)` will not find a
  // message interned under `Intern(...)` and vice-versa.  An
  // implementation may share the slot space if it tags entries by
  // type, but callers should treat the namespaces as disjoint.
  virtual uint32_t InternMap(std::shared_ptr<const HostMapBacking> backing) = 0;
  virtual const HostMapBacking* absl_nullable LookupMap(
      uint32_t slot) const = 0;

  // Same intern/lookup contract for list backings.  Independent
  // namespace from messages and maps — `LookupList(slot)` won't find
  // a map or message interned under the other API.
  virtual uint32_t InternList(
      std::shared_ptr<const HostListBacking> backing) = 0;
  virtual const HostListBacking* absl_nullable LookupList(
      uint32_t slot) const = 0;

  // Proto-identity intern entry points.  Semantically equivalent to
  // `Intern(std::make_shared<ProtoBacking>(msg))` (resp. `InternMap`
  // over a `ProtoMap`, `InternList` over a `ProtoList`) — the
  // defaults below ARE exactly that.  An implementation may override
  // to dedup by the underlying proto identity: the pointer(s) fully
  // determine the backing's observable behaviour (the wrappers are
  // stateless beyond them), so re-interning the same message — or
  // the same (owner, field) map/list — may return the
  // previously-issued slot with no allocation.  Lifetime contract a
  // deduping implementation relies on: every pointer handed in stays
  // valid until the next `Reset()`.  Trampoline callers satisfy it —
  // their messages are anchored by an Activation binding (outlives
  // the Eval by contract) or by an already-interned owning backing
  // (held by this table until `Reset()`).
  virtual uint32_t InternProtoMessage(
      const google::protobuf::Message* absl_nonnull msg) {
    return Intern(std::make_shared<ProtoBacking>(msg));
  }
  virtual uint32_t InternProtoMapField(
      const google::protobuf::Message* absl_nonnull owner,
      const google::protobuf::FieldDescriptor* absl_nonnull field) {
    return InternMap(std::make_shared<ProtoMap>(owner, field));
  }
  virtual uint32_t InternProtoListField(
      const google::protobuf::Message* absl_nonnull owner,
      const google::protobuf::FieldDescriptor* absl_nonnull field) {
    return InternList(std::make_shared<ProtoList>(owner, field));
  }

  virtual void Reset() = 0;
};

// Mint an UnknownSet descriptor (the wire contract of
// doc/design/03-abi-and-memory.md §8.2): allocates the 2-word
// `{ids_off, len}` descriptor plus the id array in the guest arena —
// ids stored sorted ascending, deduplicated — and writes
// `(CEL_UNKNOWN, payload.unk = descriptor offset)` into `*out`.
// Empty `ids` writes the legal empty UnknownSet (`payload.unk == 0`)
// without allocating.  ResourceExhausted on arena OOM.
//
// Every host-side unknown writer routes through this helper so the
// wire always carries the descriptor shape the runtime kernel
// (`cel_unknown_merge`) and the `%v` formatter dereference — a raw
// attribute id in `payload.unk` would be misread as a descriptor
// offset.
ABSL_MUST_USE_RESULT absl::Status EncodeUnknownSet(
    absl::Span<const uint32_t> ids, ArenaAllocator& alloc,
    CelValue* absl_nonnull out);

// field_ref_id → (field_number, field_name).  `field_number == 0`
// means "resolve by name only".
struct FieldRefEntry {
  uint32_t field_number = 0;
  std::string field_name;

  // Per-access-site resolved-field cache.  This is the natural seam
  // for caching proto-read resolution: codegen interns ONE
  // FieldRefEntry per kSelect site (see `FieldRefRow` in
  // compiler/codegen/expr_lower.h — rows are appended per select,
  // never deduplicated), and the entries live on the per-Instance
  // `CelHostCallbackEnv::field_refs_storage`, so each cache slot is
  // private to one (Instance, access site) pair.  The site's static
  // type means the observed `Descriptor*` is stable in practice; a
  // single entry suffices and a re-bound message from a different
  // pool simply re-resolves (see `ResolvedFieldCache::owner`).
  //
  // The descriptor itself can't be resolved at Plan/bind time —
  // the concrete `Descriptor*` is only known once a message arrives
  // through the Activation at Eval time — so the cache fills lazily
  // on first read and persists across Evals.
  //
  // `mutable` because bindings travel as `Span<const FieldRefEntry>`;
  // safe because an Instance is single-threaded per Eval (the same
  // assumption `HostExternrefTable` and the activation buffer
  // already rely on — no locks anywhere on this path).
  mutable ResolvedFieldCache resolved;
};

// attribute_id → (variable, qualifiers) path for unknown-pattern match.
// Populated when unknown-pattern matching is in scope; empty otherwise.
struct AttributeEntry {
  std::string root_variable;
  std::vector<std::string> qualifiers;
};

// type_id → (FQN, Descriptor*).  Populated by `Engine::Plan` from
// `cel.abi.types[]` by resolving each FQN against the
// embedder-supplied descriptor pool.  `descriptor` is nullable —
// nullable means "FQN was not in the pool"; the trampoline returns
// CEL_ERROR (kFieldNotFound or a future kTypeNotFound) rather than
// trapping so a corpus row referencing an unknown descriptor lands
// as a clean per-row failure instead of bringing down Eval.
struct MessageTypeEntry {
  std::string fully_qualified_name;
  const google::protobuf::Descriptor* absl_nullable descriptor = nullptr;
};

// Per-Plan runtime state.  Owned by InstanceImpl; borrowed by Layer 3.
// `unknown_patterns` is non-empty only under PartialEval.
struct CelHostBindings {
  absl::Span<const FieldRefEntry> field_refs;
  absl::Span<const AttributeEntry> attributes;
  absl::Span<const celwasm::AttributePattern> unknown_patterns;
  // type_id → resolved descriptor lookup.  Index 0 is the
  // sentinel; rows [1..N] are the ids `cel_make_message` calls
  // reference.
  absl::Span<const MessageTypeEntry> message_types;
};

// Per-eval context; bundled to stay under the 6-param lint gate.
// `alloc` is unused by has() but shared for signature uniformity.
struct TrampolineContext {
  const CelHostBindings& bindings;
  MemoryView& mem;
  ExternrefTable& refs;
  ArenaAllocator& alloc;
};

// Marshal a `celwasm::Value` produced host-side (e.g. a `@host`
// callback return) into the 24-byte CelValue at `out_slot`: scalars +
// null + duration + timestamp inline, string / bytes arena-allocated,
// message / list / map interned into the externref table as a
// CEL_*_HOST handle.  Shares the exact encoder the built-in trampolines
// use, so a host fn's wire output is byte-identical to theirs.
//
// `kUnknown` / `kError`: `kError` encodes to CEL_ERROR; `kUnknown` is
// rejected here (CHECK) — the host-call path writes CEL_UNKNOWN
// directly so the shared backing-side "backings don't return unknowns"
// invariant stays intact.  Callers route unknown returns around this.
ABSL_MUST_USE_RESULT absl::Status EncodeValueToSlot(const celwasm::Value& v,
                                                    uint32_t out_slot,
                                                    MemoryView& mem,
                                                    ExternrefTable& refs,
                                                    ArenaAllocator& alloc);

// m7b §3.1 / Probe D — sign-correlated (seconds, nanos)
// decomposition for an absl::Duration.  Shared between the
// host-side encoders (`EncodeDurationValue` / `EncodeTimestampValue`
// in cel_host_common.cc and the matching `EncodeDuration` /
// `EncodeTimestamp` in instance.cc).  `absl::IDivDuration` writes
// the remainder back to its argument; two calls yield the
// integer-second + sub-second-nanos pair matching the proto
// Duration / Timestamp text format convention.
inline void DecomposeAbslDuration(absl::Duration d, CelDurTs* out) {
  out->seconds = absl::IDivDuration(d, absl::Seconds(1), &d);
  out->nanos =
      static_cast<int32_t>(absl::IDivDuration(d, absl::Nanoseconds(1), &d));
  out->_pad = 0;
}

// ── Shared encode helpers ────────────────────────────────────────
//
// Defined in cel_host_common.cc; the single marshalling path every
// Layer-2 trampoline writes results through.  Full contracts live on
// the definitions.

// Encode a scalar / null / temporal / error `celwasm::Value` into a
// CelValue, allocating string/bytes payloads through `alloc`.
// Aggregate kinds must route through the aggregate encoders below.
ABSL_MUST_USE_RESULT absl::Status EncodeValue(const celwasm::Value& v,
                                              CelValue* absl_nonnull out,
                                              ArenaAllocator& alloc);

// Encode a Layer-1 aggregate (message / map / list) into a wire
// CelValue by interning the backing.  Returns true iff `v` was an
// aggregate kind and `*out` has been written.
absl::StatusOr<bool> EncodeAggregateToCelValue(const celwasm::Value& v,
                                               const TrampolineContext& ctx,
                                               CelValue* absl_nonnull out);

// Marshal any Layer-1 `celwasm::Value` into the CelValue at
// `out_slot` (scalars inline / via arena, aggregates interned).
ABSL_MUST_USE_RESULT absl::Status EncodeFieldResult(
    const celwasm::Value& v, uint32_t out_slot, const TrampolineContext& ctx);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_COMMON_H_
