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
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "compiler_v2/api/attribute.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "compiler_v2/runtime/cel_data.h"

namespace google::protobuf {
class Descriptor;
class FieldDescriptor;
class Message;
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

  // Optional access to the underlying proto message — proto backings
  // (`ProtoBacking`, `OwnedProtoBacking`) return a non-null pointer;
  // non-proto custom subclasses (JSON, struct-of-structs) inherit
  // the nullptr default.  `CelMessageEqImpl` (M5.B) uses this to
  // reach `MessageDifferencer` without dynamic_casting per concrete
  // backing type — both M2.C-bound (`ProtoBacking`) and M7-built
  // (`OwnedProtoBacking`) messages compare uniformly.
  virtual const google::protobuf::Message* absl_nullable message() const {
    return nullptr;
  }
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

  const google::protobuf::Message* absl_nullable message() const override {
    return msg_;
  }

 private:
  const google::protobuf::Message* absl_nonnull msg_;
};

// M7.A: owning counterpart to `ProtoBacking` — wraps a
// `unique_ptr<Message>` allocated by `MessageFactory::GetPrototype()
// ->New()` inside `CelMakeMessageImpl`.  The runtime needs an owning
// backing because the message has no host-side anchor (the literal
// is constructed during eval, not bound by the caller); the
// ExternrefTable holds a `shared_ptr<HostMessageBacking>` that frees
// the message at `Reset()` between Evals.
//
// Read-side (ReadField / HasField) delegates to a composed
// `ProtoBacking` over the owned message — no duplicated reflection
// code; M7-constructed messages flow through the same M2.C kSelect
// read path as host-bound proto messages.  M7.B's `cel_set_field`
// will mutate the message in place via `mutable_message()`.
class OwnedProtoBacking final : public HostMessageBacking {
 public:
  explicit OwnedProtoBacking(std::unique_ptr<google::protobuf::Message> msg);

  absl::StatusOr<cel::Value> ReadField(
      int field_number, absl::string_view field_name,
      const cel::CelType& expected_type) const override;

  bool HasField(int field_number, absl::string_view field_name) const override;

  // M7.B uses this for `Reflection::Set...`; M7.A doesn't call it
  // (every empty `Foo{}` reads back through the read-side path).
  google::protobuf::Message* absl_nonnull mutable_message() {
    return msg_.get();
  }

  const google::protobuf::Message* absl_nullable message() const override {
    return msg_.get();
  }

 private:
  std::unique_ptr<google::protobuf::Message> msg_;
  // Composed view over `msg_.get()` so the read-side ReadField /
  // HasField overrides delegate without duplicating ProtoBacking's
  // reflection logic.  Initialised after `msg_` (declaration order
  // matters — `inner_` constructor reads `msg_.get()`).
  ProtoBacking inner_;
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
      size_t index, const cel::CelType& expected_element_type) const override;
  void ForEach(absl::FunctionRef<void(const cel::Value&)> visit) const override;

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
      size_t index, const cel::CelType& expected_element_type) const override;
  void ForEach(absl::FunctionRef<void(const cel::Value&)> visit) const override;

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

// M7.A: type_id → (FQN, Descriptor*).  Populated by `Engine::Plan`
// from `cel.abi.types[]` by resolving each FQN against the
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
  absl::Span<const cel::AttributePattern> unknown_patterns;
  // M7.A: type_id → resolved descriptor lookup.  Index 0 is the
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
ABSL_MUST_USE_RESULT absl::Status CelListAtImpl(uint32_t out_slot,
                                                uint32_t list_slot,
                                                uint32_t index_slot,
                                                const TrampolineContext& ctx);

// M5.D step 2 — Layer-2 entry points for the kHost arms of the
// aggregate-op runtime dispatchers (`cel_list_size` / `cel_list_in`
// / `cel_list_eq` / `cel_list_concat` / `cel_map_size` / `cel_map_in`
// / `cel_map_eq`).  Each absorbs UNKNOWN / ERROR on its operands,
// dereferences the kHost backing via `ctx.refs.LookupList` /
// `LookupMap`, runs the spec-level operation, and writes the result
// CelValue into `out_slot`.  Spec-level errors (no_such_key,
// kind-mismatched element, …) travel inside the CelValue;
// non-OK Status only on infrastructure failure.  See
// `m5-kcall-comprehensions.md §2.1` for routing.
//
// Each Impl uses the 3-arg trampoline shape (out + 2 operands) so
// the existing `HostThreeArgTrampoline<Impl>` template fits.
// `CelListSizeImpl` and `CelMapSizeImpl` ignore the third arg; the
// dispatcher omits the third arg entirely (2-arg call), and the
// linker registers them as 2-arg host functions.
ABSL_MUST_USE_RESULT absl::Status CelListSizeImpl(uint32_t out_slot,
                                                  uint32_t list_slot,
                                                  const TrampolineContext& ctx);

ABSL_MUST_USE_RESULT absl::Status CelListInImpl(uint32_t out_slot,
                                                uint32_t value_slot,
                                                uint32_t list_slot,
                                                const TrampolineContext& ctx);

ABSL_MUST_USE_RESULT absl::Status CelListEqImpl(uint32_t out_slot,
                                                uint32_t a_slot,
                                                uint32_t b_slot,
                                                const TrampolineContext& ctx);

// Cross-origin (one CEL_LIST_ARENA + one CEL_LIST_HOST) and
// both-host concat materialise into a fresh arena list under
// the runtime's `cel_alloc`.  For M5.D step 2 ship state, mixed
// origins POISON with TYPE_MISMATCH; full materialisation is
// follow-up work tracked in the M5 doc.
ABSL_MUST_USE_RESULT absl::Status CelListConcatImpl(
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot,
    const TrampolineContext& ctx);

ABSL_MUST_USE_RESULT absl::Status CelMapSizeImpl(uint32_t out_slot,
                                                 uint32_t map_slot,
                                                 const TrampolineContext& ctx);

ABSL_MUST_USE_RESULT absl::Status CelMapInImpl(uint32_t out_slot,
                                               uint32_t key_slot,
                                               uint32_t map_slot,
                                               const TrampolineContext& ctx);

ABSL_MUST_USE_RESULT absl::Status CelMapEqImpl(uint32_t out_slot,
                                               uint32_t a_slot, uint32_t b_slot,
                                               const TrampolineContext& ctx);

// Polymorphic message equality.  Both operands must be
// `CEL_MESSAGE` with valid `payload.msg_slot` ref-slots; either
// operand UNKNOWN / ERROR propagates 3VL.  Uses
// `google::protobuf::util::MessageDifferencer::Equals` over the
// underlying `HostMessageBacking::Message()` per langdef §"Equality".
// `cel_message_eq` is a standalone helper for M5.B step 2b's
// polymorphic `cel_equals_at_vv` ladder; not one of the seven
// dispatchers above.
ABSL_MUST_USE_RESULT absl::Status CelMessageEqImpl(
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot,
    const TrampolineContext& ctx);

// M7.A: cel_host.cel_make_message — proto literal construction.
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

// M9.B: cel_host.resolve_message_type_name — descriptor-FQN
// resolution for `type(<message>)`.  Reads the CEL_MESSAGE
// CelValue at `in_slot`, dereferences `payload.msg_slot` against
// `ctx.refs` to recover the `HostMessageBacking`, walks to the
// proto's `GetDescriptor()->full_name()`, copies the FQN bytes
// into the per-Eval arena via `ctx.alloc`, stamps
// `{kind: CEL_TYPE, payload.s: {arena_off, len}}` into out_slot.
//
// M9.B ships the function as a stub that poisons out_slot with
// kTypeMismatch — registration of the trampoline is what makes
// the runtime instantiate cleanly.  M9.C replaces the body with
// the real descriptor walk.
ABSL_MUST_USE_RESULT absl::Status CelResolveMessageTypeNameImpl(
    uint32_t out_slot, uint32_t in_slot, const TrampolineContext& ctx);

// M7.B: cel_host.cel_set_field — proto literal field set.
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
//   4. Repeated, map, and singular-message field shapes are M7.C
//      (lists/maps) and M7.E (nested messages); they return non-OK
//      Status from this trampoline (wasm trap) until those slices
//      ship.
// Non-OK Status surfaces as a wasm trap; the conformance harness
// records the row as failure without aborting the run.
ABSL_MUST_USE_RESULT absl::Status CelSetFieldImpl(uint32_t msg_slot,
                                                  uint32_t field_ref_id,
                                                  uint32_t value_slot,
                                                  const TrampolineContext& ctx);

// M7B.D: cel_host parse + format trampolines.  All four are
// `(out_slot, in_slot)` shape — read the input CelValue at
// `in_slot` (string for parse, ts/dur for format), run the
// appropriate absl::ParseTime / ParseDuration / FormatTime / proto-
// Duration text-format kernel, write the result CelValue at
// `out_slot`.  Spec-level failures (parse error, lowercase z,
// out-of-range, unordered compound units) write a `CEL_ERROR`
// with `CEL_ERR_INVALID_ARGUMENT`; non-OK Status reserved for
// infrastructure failures.  See m7b §4.3 for the split rationale.
ABSL_MUST_USE_RESULT absl::Status CelTimestampParseImpl(
    uint32_t out_slot, uint32_t str_slot, const TrampolineContext& ctx);
ABSL_MUST_USE_RESULT absl::Status CelDurationParseImpl(
    uint32_t out_slot, uint32_t str_slot, const TrampolineContext& ctx);
ABSL_MUST_USE_RESULT absl::Status CelTimestampFormatImpl(
    uint32_t out_slot, uint32_t ts_slot, const TrampolineContext& ctx);
ABSL_MUST_USE_RESULT absl::Status CelDurationFormatImpl(
    uint32_t out_slot, uint32_t dur_slot, const TrampolineContext& ctx);

// M7B.E: single dispatch trampoline for the 10 with-TZ accessor
// overloads.  Reads the timestamp + TZ-name string operands; loads
// the IANA / fixed-offset zone via `absl::TimeZone::Load`; projects
// the requested civil-time field per `accessor_kind` (matches
// `CelTzAccessorKind` enum in cel_time.h).  Invalid TZ name →
// CEL_ERROR(kInvalidArgument).  Bad accessor_kind →
// CEL_ERROR(kTypeMismatch) (defence in depth; codegen wouldn't
// emit an unknown kind).
ABSL_MUST_USE_RESULT absl::Status CelTimestampTzAccessorImpl(
    uint32_t out_slot, uint32_t ts_slot, uint32_t tz_slot,
    uint32_t accessor_kind, const TrampolineContext& ctx);

// m7b §3.1 / Probe D — sign-correlated (seconds, nanos)
// decomposition for an absl::Duration.  Shared between the
// host-side encoders (`EncodeDurationValue` / `EncodeTimestampValue`
// in cel_host.cc and the matching `EncodeDuration` /
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

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_H_
