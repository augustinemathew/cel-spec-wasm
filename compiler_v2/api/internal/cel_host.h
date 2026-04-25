// cel_host — host-side helpers the wasm expr module calls via its
// `cel_host.*` imports.  Three-layer split (m2-ident-select-unknowns.md §2.4):
//
//   Layer 1: HostMessageBacking — pure CEL field-read semantics.
//   Layer 2: trampoline — adapts Layer 1 to the wasm ABI (lives in this file,
//            runtime-agnostic, driven by MemoryView / ExternrefTable /
//            ArenaAllocator).  M2.C.0b.
//   Layer 3: wasmtime glue.  M2.C.5.

#ifndef CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_H_
#define CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/api/attribute.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "compiler_v2/runtime/cel_data.h"

namespace google::protobuf {
class Message;
class FieldDescriptor;
}  // namespace google::protobuf

namespace celwasm {

// ═══════════ Layer 1 — per-backing field-read semantics ═══════════

// Plug-in point for a CEL message.  ProtoBacking is the built-in;
// embedders subclass for JSON / XML / …  `Activation::Bind(name,
// Value::HostMessage(backing))` carries the backing into dispatch.
class HostMessageBacking {
 public:
  virtual ~HostMessageBacking() = default;

  HostMessageBacking() = default;
  HostMessageBacking(const HostMessageBacking&) = delete;
  HostMessageBacking& operator=(const HostMessageBacking&) = delete;

  // `field_number == 0` means "resolve by name only" (non-proto backings).
  // Spec-level errors (missing field, repeated at M2) return
  // `Value::Error(...)`; infrastructure failures return non-OK Status.
  virtual absl::StatusOr<cel::Value> ReadField(
      int field_number, absl::string_view field_name,
      const cel::CelType& expected_type) const = 0;

  // has(msg.field) per langdef proto2/proto3 presence rules.
  virtual bool HasField(int field_number,
                        absl::string_view field_name) const = 0;
};

// Non-owning adapter over `google::protobuf::Message` — caller keeps the
// message alive for the Eval that observes this backing.
class ProtoBacking final : public HostMessageBacking {
 public:
  explicit ProtoBacking(const google::protobuf::Message* absl_nonnull msg)
      : msg_(msg) {}

  absl::StatusOr<cel::Value> ReadField(
      int field_number, absl::string_view field_name,
      const cel::CelType& expected_type) const override;

  bool HasField(int field_number, absl::string_view field_name) const override;

  const google::protobuf::Message* absl_nonnull message() const {
    return msg_;
  }

 private:
  const google::protobuf::Message* absl_nonnull msg_;
};

// Plug-in point for a CEL map.  Two built-in concretes (below):
// `HostMap` for vector-backed user bindings, `ProtoMap` for proto
// reflection-backed map fields.  `Activation::Bind(name,
// Value::Map(...))` (or `Value::HostMap(...)`) carries the backing
// into dispatch.
class HostMapBacking {
 public:
  virtual ~HostMapBacking() = default;

  HostMapBacking() = default;
  HostMapBacking(const HostMapBacking&) = delete;
  HostMapBacking& operator=(const HostMapBacking&) = delete;

  virtual size_t Size() const = 0;

  // Returns the value associated with `key` per langdef map-key
  // equality (cross-type numeric for int/uint; structural for
  // bool/string).  Missing key returns `Value::Error(no_such_key)`,
  // not non-OK Status — those are reserved for infrastructure
  // failures (backing unavailable, reflection error, etc.).
  // `expected_value_type` is informational; M3 ignores it (no
  // implicit coercion between value-side numeric kinds).
  virtual absl::StatusOr<cel::Value> Get(
      const cel::Value& key, const cel::CelType& expected_value_type) const = 0;

  virtual bool ContainsKey(const cel::Value& key) const = 0;
  virtual void ForEach(
      absl::FunctionRef<void(const cel::Value&, const cel::Value&)> visit)
      const = 0;
};

// Vector-backed concrete.  Used for `Activation::Bind(Value::Map(...))`
// — the host caller hands us a key/value list and we keep them in
// insertion order.  Linear scan on lookup; matches the runtime's
// `cel_map_lookup_arena` semantics so host- and arena-built maps
// behave identically under CEL's equality rules.
class HostMap final : public HostMapBacking {
 public:
  explicit HostMap(std::vector<std::pair<cel::Value, cel::Value>> entries);
  ~HostMap() override = default;

  size_t Size() const override;
  absl::StatusOr<cel::Value> Get(
      const cel::Value& key,
      const cel::CelType& expected_value_type) const override;
  bool ContainsKey(const cel::Value& key) const override;
  void ForEach(absl::FunctionRef<void(const cel::Value&, const cel::Value&)>
                   visit) const override;

 private:
  std::vector<std::pair<cel::Value, cel::Value>> entries_;
};

// Proto reflection-backed concrete.  Wraps a single
// `google::protobuf::Message*` + the FieldDescriptor for one of its
// map fields.  M3.G fills in the bodies; M3.D ships the class so the
// header surface is stable.  Non-owning — the caller (typically
// `ProtoBacking::ReadField`) keeps the message alive for the
// lifetime of any Eval that observes the wrapping Value.
class ProtoMap final : public HostMapBacking {
 public:
  ProtoMap(const google::protobuf::Message* absl_nonnull owner,
           const google::protobuf::FieldDescriptor* absl_nonnull field);
  ~ProtoMap() override = default;

  size_t Size() const override;
  absl::StatusOr<cel::Value> Get(
      const cel::Value& key,
      const cel::CelType& expected_value_type) const override;
  bool ContainsKey(const cel::Value& key) const override;
  void ForEach(absl::FunctionRef<void(const cel::Value&, const cel::Value&)>
                   visit) const override;

 private:
  // Non-owning views.  Caller (typically `ProtoBacking::ReadField`)
  // keeps the message alive for the lifetime of any Eval that
  // observes the wrapping Value.
  const google::protobuf::Message* absl_nonnull owner_;
  const google::protobuf::FieldDescriptor* absl_nonnull field_;
};

// Plug-in point for a CEL list.  Two built-in concretes (below):
// `HostList` for vector-backed user bindings, `ProtoList` for proto
// reflection-backed REPEATED fields.  `Activation::Bind(name,
// Value::List(...))` (or `Value::HostList(...)`) carries the backing
// into dispatch.  Out-of-bounds At() returns
// `Value::Error(kIndexOutOfBounds)`, never non-OK Status.
class HostListBacking {
 public:
  virtual ~HostListBacking() = default;

  HostListBacking() = default;
  HostListBacking(const HostListBacking&) = delete;
  HostListBacking& operator=(const HostListBacking&) = delete;

  virtual size_t Size() const = 0;

  // `expected_element_type` is informational; M4 ignores it (no
  // implicit coercion between numeric kinds).  Mirrors
  // `HostMapBacking::Get`'s expected_value_type contract.
  virtual absl::StatusOr<cel::Value> At(
      size_t index, const cel::CelType& expected_element_type) const = 0;

  virtual void ForEach(
      absl::FunctionRef<void(const cel::Value&)> visit) const = 0;
};

// Vector-backed concrete.  Used for `Activation::Bind(Value::List(...))`
// — the host caller hands us an ordered element list and we keep it
// in insertion order.  Mirrors `HostMap`'s shape.
class HostList final : public HostListBacking {
 public:
  explicit HostList(std::vector<cel::Value> elements);
  ~HostList() override = default;

  size_t Size() const override;
  absl::StatusOr<cel::Value> At(
      size_t index,
      const cel::CelType& expected_element_type) const override;
  void ForEach(
      absl::FunctionRef<void(const cel::Value&)> visit) const override;

 private:
  std::vector<cel::Value> elements_;
};

// Proto reflection-backed concrete.  Wraps a single
// `google::protobuf::Message*` + the FieldDescriptor for one of its
// REPEATED (non-map) fields.  Non-owning — caller (typically
// `ProtoBacking::ReadField`) keeps the message alive for the
// lifetime of any Eval that observes the wrapping Value.
class ProtoList final : public HostListBacking {
 public:
  ProtoList(const google::protobuf::Message* absl_nonnull owner,
            const google::protobuf::FieldDescriptor* absl_nonnull field);
  ~ProtoList() override = default;

  size_t Size() const override;
  absl::StatusOr<cel::Value> At(
      size_t index,
      const cel::CelType& expected_element_type) const override;
  void ForEach(
      absl::FunctionRef<void(const cel::Value&)> visit) const override;

 private:
  const google::protobuf::Message* absl_nonnull owner_;
  const google::protobuf::FieldDescriptor* absl_nonnull field_;
};

// ═══════════ Layer 2 — trampoline abstractions + entry points ═══════════

// Abstract 24-byte CelValue + span read/write over the expr module's
// linear memory.  Production impl wraps `wasmtime_memory_t`; tests use
// a vector-backed fake.
class MemoryView {
 public:
  virtual ~MemoryView() = default;

  MemoryView() = default;
  MemoryView(const MemoryView&) = delete;
  MemoryView& operator=(const MemoryView&) = delete;

  virtual CelValue ReadCelValue(uint32_t offset) const = 0;
  virtual void WriteCelValue(uint32_t offset, const CelValue& v) = 0;
  virtual absl::string_view ReadSpan(uint32_t ptr, uint32_t len) const = 0;
};

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

  // M3.E: same intern/lookup contract for map backings.  Slot
  // namespaces are independent — `LookupMap(slot)` will not find a
  // message interned under `Intern(...)` and vice-versa.  An
  // implementation may share the slot space if it tags entries by
  // type, but callers should treat the namespaces as disjoint.
  virtual uint32_t InternMap(std::shared_ptr<const HostMapBacking> backing) = 0;
  virtual const HostMapBacking* absl_nullable LookupMap(
      uint32_t slot) const = 0;

  // M4.E: same intern/lookup contract for list backings.  Independent
  // namespace from messages and maps — `LookupList(slot)` won't find
  // a map or message interned under the other API.
  virtual uint32_t InternList(
      std::shared_ptr<const HostListBacking> backing) = 0;
  virtual const HostListBacking* absl_nullable LookupList(
      uint32_t slot) const = 0;

  virtual void Reset() = 0;
};

// Bump allocator for string/bytes payloads.  `out_offset` receives
// the wasm-side offset the CelSpan carries.  Zero-byte alloc returns
// a valid pointer; OOM returns nullptr.
class ArenaAllocator {
 public:
  virtual ~ArenaAllocator() = default;

  ArenaAllocator() = default;
  ArenaAllocator(const ArenaAllocator&) = delete;
  ArenaAllocator& operator=(const ArenaAllocator&) = delete;

  virtual uint8_t* absl_nullable Alloc(size_t len,
                                       uint32_t* absl_nonnull out_offset) = 0;
};

// field_ref_id → (field_number, field_name).  `field_number == 0`
// means "resolve by name only".
struct FieldRefEntry {
  uint32_t field_number = 0;
  std::string field_name;
};

// attribute_id → (variable, qualifiers) path for unknown-pattern match.
// Populated by M2.E; empty at M2.B/C.
struct AttributeEntry {
  std::string root_variable;
  std::vector<std::string> qualifiers;
};

// Per-Plan runtime state.  Owned by InstanceImpl; borrowed by Layer 3.
// `unknown_patterns` is non-empty only under PartialEval.
struct CelHostBindings {
  absl::Span<const FieldRefEntry> field_refs;
  absl::Span<const AttributeEntry> attributes;
  absl::Span<const cel::AttributePattern> unknown_patterns;
};

// Per-eval context; bundled to stay under the 6-param lint gate.
// `alloc` is unused by has() but shared for signature uniformity.
struct TrampolineContext {
  const CelHostBindings& bindings;
  MemoryView& mem;
  ExternrefTable& refs;
  ArenaAllocator& alloc;
};

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

// M3.E: Layer-2 entry point for the kHost arm of map indexing.
// Reads the map slot's `ref_slot`, dereferences via
// `ExternrefTable::LookupMap` to a `HostMapBacking`, decodes the
// key CelValue, calls `backing->Get(key, ...)`, and marshals the
// returned `cel::Value` into `out_slot`.  Absorbs UNKNOWN / ERROR
// on either operand without dereferencing the backing — same 3VL
// contract as the runtime dispatcher.  Non-OK Status only on
// infrastructure failure (bad slot, missing reflection); spec-level
// errors (no_such_key, type-mismatch on key) travel inside the
// returned CelValue.
ABSL_MUST_USE_RESULT absl::Status CelMapLookupImpl(
    uint32_t out_slot, uint32_t map_slot, uint32_t key_slot,
    const TrampolineContext& ctx);

// M4.E: Layer-2 entry point for the kHost arm of list indexing.
// Reads list_slot's `ref_slot`, dereferences via
// `ExternrefTable::LookupList` to a `HostListBacking`, decodes the
// index CelValue (must be CEL_INT; non-int → kTypeMismatch;
// negative or `>= Size()` → kIndexOutOfBounds), calls
// `backing->At(index)`, and marshals the returned `cel::Value` into
// `out_slot`.  Absorbs UNKNOWN / ERROR on either operand.  Same
// non-OK Status contract as CelMapLookupImpl.
ABSL_MUST_USE_RESULT absl::Status CelListAtImpl(
    uint32_t out_slot, uint32_t list_slot, uint32_t index_slot,
    const TrampolineContext& ctx);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_H_
