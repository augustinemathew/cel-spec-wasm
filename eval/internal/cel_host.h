// cel_host — host-side helpers the wasm expr module calls via its
// `cel_host.*` imports.  Three-layer split (see
// `doc/implementation-plan/rewrite/cel-host-surface.md` §4):
//
//   Layer 1: HostMessageBacking — pure CEL field-read semantics.
//   Layer 2: trampoline — adapts Layer 1 to the wasm ABI (lives in this file,
//            runtime-agnostic, driven by MemoryView / ExternrefTable /
//            ArenaAllocator).
//   Layer 3: wasmtime glue.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_H_

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
#include "eval/attribute.h"
#include "eval/value.h"
#include "runtime/cel_data.h"
#include "shared/type.h"

namespace google::protobuf {
class Descriptor;
class DescriptorPool;
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
  virtual absl::StatusOr<celwasm::Value> ReadField(
      int field_number, absl::string_view field_name,
      const celwasm::CelType& expected_type) const = 0;

  // has(msg.field) per langdef proto2/proto3 presence rules.
  virtual bool HasField(int field_number,
                        absl::string_view field_name) const = 0;

  // Optional access to the underlying proto message — proto backings
  // (`ProtoBacking`, `OwnedProtoBacking`) return a non-null pointer;
  // non-proto custom subclasses (JSON, struct-of-structs) inherit
  // the nullptr default.  `CelMessageEqImpl` uses this to reach
  // `MessageDifferencer` without dynamic_casting per concrete
  // backing type — both Activation-bound (`ProtoBacking`) and
  // proto-literal-built (`OwnedProtoBacking`) messages compare
  // uniformly.
  virtual const google::protobuf::Message* absl_nullable message() const {
    return nullptr;
  }
};

// Non-owning adapter over `google::protobuf::Message` — caller keeps the
// message alive for the Eval that observes this backing.
// `ReadField` returns a `celwasm::Value` whose string/bytes payload
// is heap-backed by the proto's internal storage at the C++ layer
// (via `GetStringReference`).  However — and this is the load-
// bearing fact for DoS-budgeting an Eval — when the result crosses
// back into the expr module through the `cel_get_field`
// trampoline, `cel_host.cc::EncodeSpan` (line ~737) eagerly
// memcpy's the proto-owned string into the per-Eval bump arena
// before stamping `(ptr, len)` in the result CelValue.  So at the
// wire transition the proto field IS arena-resident.
//
// Consequences for embedders:
//
//   - A proto field whose serialised size exceeds the per-Eval
//     arena cap (`runtime/cel_layout.h`, 64 KiB by default)
//     poisons the Eval with `kArenaOverflow` even for
//     ostensibly-cheap probes like `size(c.huge_field)`,
//     `has(c.huge_field)`, or `c.huge_field == ""` — every
//     field read currently goes through the eager arena copy.
//   - The failure mode is clean (well-typed `kError`, not
//     truncation or memory corruption), but it is a DoS vector:
//     an attacker who controls the proto binding can guarantee
//     Eval failure for any expression referencing the huge
//     field.  Embedders that accept untrusted protos MUST size
//     the arena to bound the largest acceptable field, OR pre-
//     validate field sizes before binding.
//
// Filed as **cleanup-backlog #35**.  The right fix is the same
// surface as #17 / #21 / #34 — a runtime grow-on-demand arena.
// Until that lands, this contract is the documentation of the
// current eager-copy reality (originally documented as "lazy
// copy" — that was wrong; `e2e/proto_arena_lazy_copy_test.cc`
// is the empirical pin that corrected the contract).
class ProtoBacking final : public HostMessageBacking {
 public:
  explicit ProtoBacking(const google::protobuf::Message* absl_nonnull msg)
      : msg_(msg) {}

  absl::StatusOr<celwasm::Value> ReadField(
      int field_number, absl::string_view field_name,
      const celwasm::CelType& expected_type) const override;

  bool HasField(int field_number, absl::string_view field_name) const override;

  const google::protobuf::Message* absl_nullable message() const override {
    return msg_;
  }

 private:
  const google::protobuf::Message* absl_nonnull msg_;
};

// Owning counterpart to `ProtoBacking` — wraps a
// `unique_ptr<Message>` allocated by `MessageFactory::GetPrototype()
// ->New()` inside `CelMakeMessageImpl`.  The runtime needs an owning
// backing because the message has no host-side anchor (the literal
// is constructed during eval, not bound by the caller); the
// ExternrefTable holds a `shared_ptr<HostMessageBacking>` that frees
// the message at `Reset()` between Evals.
//
// Read-side (ReadField / HasField) delegates to a composed
// `ProtoBacking` over the owned message — no duplicated reflection
// code; constructed messages flow through the same kSelect read path
// as host-bound proto messages.  `cel_set_field` mutates the message
// in place via `mutable_message()`.
class OwnedProtoBacking final : public HostMessageBacking {
 public:
  explicit OwnedProtoBacking(std::unique_ptr<google::protobuf::Message> msg);

  absl::StatusOr<celwasm::Value> ReadField(
      int field_number, absl::string_view field_name,
      const celwasm::CelType& expected_type) const override;

  bool HasField(int field_number, absl::string_view field_name) const override;

  // `cel_set_field` uses this for `Reflection::Set...`; the
  // empty-message construction path doesn't call it (every empty
  // `Foo{}` reads back through the read-side path).
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
  virtual absl::StatusOr<celwasm::Value> Get(
      const celwasm::Value& key,
      const celwasm::CelType& expected_value_type) const = 0;

  virtual bool ContainsKey(const celwasm::Value& key) const = 0;
  virtual void ForEach(
      absl::FunctionRef<void(const celwasm::Value&, const celwasm::Value&)>
          visit) const = 0;
};

// Vector-backed concrete.  Used for `Activation::Bind(Value::Map(...))`
// — the host caller hands us a key/value list and we keep them in
// insertion order.  Linear scan on lookup; matches the runtime's
// `cel_map_lookup_arena` semantics so host- and arena-built maps
// behave identically under CEL's equality rules.
class HostMap final : public HostMapBacking {
 public:
  explicit HostMap(
      std::vector<std::pair<celwasm::Value, celwasm::Value>> entries);
  ~HostMap() override = default;

  size_t Size() const override;
  absl::StatusOr<celwasm::Value> Get(
      const celwasm::Value& key,
      const celwasm::CelType& expected_value_type) const override;
  bool ContainsKey(const celwasm::Value& key) const override;
  void ForEach(
      absl::FunctionRef<void(const celwasm::Value&, const celwasm::Value&)>
          visit) const override;

 private:
  std::vector<std::pair<celwasm::Value, celwasm::Value>> entries_;
};

// Proto reflection-backed concrete.  Wraps a single
// `google::protobuf::Message*` + the FieldDescriptor for one of its
// map fields.  Non-owning — the caller (typically
// `ProtoBacking::ReadField`) keeps the message alive for the
// lifetime of any Eval that observes the wrapping Value.
class ProtoMap final : public HostMapBacking {
 public:
  ProtoMap(const google::protobuf::Message* absl_nonnull owner,
           const google::protobuf::FieldDescriptor* absl_nonnull field);
  ~ProtoMap() override = default;

  size_t Size() const override;
  absl::StatusOr<celwasm::Value> Get(
      const celwasm::Value& key,
      const celwasm::CelType& expected_value_type) const override;
  bool ContainsKey(const celwasm::Value& key) const override;
  void ForEach(
      absl::FunctionRef<void(const celwasm::Value&, const celwasm::Value&)>
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
  virtual absl::StatusOr<celwasm::Value> At(
      size_t index, const celwasm::CelType& expected_element_type) const = 0;

  virtual void ForEach(
      absl::FunctionRef<void(const celwasm::Value&)> visit) const = 0;
};

// Vector-backed concrete.  Used for `Activation::Bind(Value::List(...))`
// — the host caller hands us an ordered element list and we keep it
// in insertion order.  Mirrors `HostMap`'s shape.
class HostList final : public HostListBacking {
 public:
  explicit HostList(std::vector<celwasm::Value> elements);
  ~HostList() override = default;

  size_t Size() const override;
  absl::StatusOr<celwasm::Value> At(
      size_t index,
      const celwasm::CelType& expected_element_type) const override;
  void ForEach(
      absl::FunctionRef<void(const celwasm::Value&)> visit) const override;

 private:
  std::vector<celwasm::Value> elements_;
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
  absl::StatusOr<celwasm::Value> At(
      size_t index,
      const celwasm::CelType& expected_element_type) const override;
  void ForEach(
      absl::FunctionRef<void(const celwasm::Value&)> visit) const override;

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

  // Total bytes of the underlying linear memory.  Used by the
  // default `IsInBounds` helper below and by callers that need to
  // bounds-check `ptr + len` against memory size before iterating
  // (e.g. when walking an attacker-controlled length field from a
  // CelValue payload).
  virtual uint32_t Size() const = 0;

  // True iff `[ptr, ptr+len)` lies entirely inside `[0, Size())`,
  // i.e. the read/write of `len` bytes at `ptr` is safe.  Empty
  // ranges (`len == 0`) are always in-bounds — they perform no
  // memory access.  Subtraction is rearranged
  // (`len <= Size() - ptr`) to avoid overflow when computing
  // `ptr + len` on the u32s.
  bool IsInBounds(uint32_t ptr, uint32_t len) const {
    if (len == 0) return true;
    const uint32_t size = Size();
    return ptr <= size && len <= size - ptr;
  }

  // Read/write methods MUST bounds-check `offset + sizeof(...)` (or
  // `ptr + len`) against `Size()` before touching memory.  On OOB:
  //
  //   - `ReadCelValue` returns a zeroed `CelValue` (kind == 0,
  //     payload == 0) — observable as a kNull on the host side, so
  //     the trampoline propagates a defined-but-empty value rather
  //     than reading host memory adjacent to the wasm reservation.
  //   - `ReadSpan` returns an empty `absl::string_view` — the
  //     caller sees a zero-length string/bytes, never a span
  //     pointing past the wasm sandbox.
  //   - `WriteCelValue` / `WriteU32` are no-ops on OOB — the
  //     write is silently dropped; subsequent reads observe the
  //     prior memory state.  No partial writes.
  //
  // This is the security-relevant contract: a malicious or buggy
  // wasm module that passes out-of-bounds `ptr`/`offset` values
  // through a host trampoline CANNOT leak host memory or corrupt
  // host state via these methods.  The eval that produced the
  // bad value still proceeds — the value it ultimately yields is
  // observably wrong (typically kNull or an empty container), so
  // a downstream assertion in well-written CEL catches it.  See
  // `cleanup-backlog #36` for the audit that closed this gap.
  virtual CelValue ReadCelValue(uint32_t offset) const = 0;
  virtual void WriteCelValue(uint32_t offset, const CelValue& v) = 0;
  virtual absl::string_view ReadSpan(uint32_t ptr, uint32_t len) const = 0;

  // Raw u32 write at `offset`.  Used by aggregate-iter trampolines
  // that populate runtime-side state structs (e.g. `MapIterState`)
  // whose 4-byte fields don't fit the 24-byte CelValue shape.
  virtual void WriteU32(uint32_t offset, uint32_t value) = 0;
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

// Auto-unpack a `google.protobuf.Any` proto message into the CEL
// Value its payload encodes.  Chains Any-of-Any layers and applies
// the WKT/wrapper unwrap to the innermost terminal.  Mirrors
// cel-cpp's eval-time Any unpacking behaviour — the Any contract is
// "unpack on read" (langdef §"Message Field Selection" admits Any
// at any message-typed slot).  Returns `Value::OwnedMessage(...)`
// wrapping a fresh clone of the innermost message when no WKT
// peel applies; an Error value when the type_url is empty / unknown.
celwasm::Value UnpackAnyToValue(const google::protobuf::Message& any,
                                const google::protobuf::DescriptorPool* pool);

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

// Layer-2 entry point for the kHost arm of map indexing.
// Reads the map slot's `ref_slot`, dereferences via
// `ExternrefTable::LookupMap` to a `HostMapBacking`, decodes the
// key CelValue, calls `backing->Get(key, ...)`, and marshals the
// returned `celwasm::Value` into `out_slot`.  Absorbs UNKNOWN / ERROR
// on either operand without dereferencing the backing — same 3VL
// contract as the runtime dispatcher.  Non-OK Status only on
// infrastructure failure (bad slot, missing reflection); spec-level
// errors (no_such_key, type-mismatch on key) travel inside the
// returned CelValue.
ABSL_MUST_USE_RESULT absl::Status CelMapLookupImpl(
    uint32_t out_slot, uint32_t map_slot, uint32_t key_slot,
    const TrampolineContext& ctx);

// Layer-2 entry point for the kHost arm of list indexing.
// Reads list_slot's `ref_slot`, dereferences via
// `ExternrefTable::LookupList` to a `HostListBacking`, decodes the
// index CelValue (must be CEL_INT; non-int → kTypeMismatch;
// negative or `>= Size()` → kIndexOutOfBounds), calls
// `backing->At(index)`, and marshals the returned `celwasm::Value` into
// `out_slot`.  Absorbs UNKNOWN / ERROR on either operand.  Same
// non-OK Status contract as CelMapLookupImpl.
ABSL_MUST_USE_RESULT absl::Status CelListAtImpl(uint32_t out_slot,
                                                uint32_t list_slot,
                                                uint32_t index_slot,
                                                const TrampolineContext& ctx);

// Comprehension-iter snapshot for a `CEL_LIST_HOST` source.  Walks
// the HostListBacking via `At(i)`, encodes each element into an
// arena-allocated `N×24-byte` elements run, allocates a 16-byte
// ArenaListHeader pointing at that run, and writes a synthetic
// `{kind:CEL_LIST_ARENA, payload.arena_list.header_ptr=...}`
// CelValue at `out_slot`.  Lets the inline arena prologue in
// `expr_lower_comprehension.cc` walk host lists unchanged.
//
// On empty source, OOM, or non-CEL_LIST_HOST input: writes an
// empty arena list (header_ptr=0) so the comprehension loop body
// never runs.  Mirrors `CelMapIterOpenImpl` for maps; see m5b
// §CCF-8 for the design.
ABSL_MUST_USE_RESULT absl::Status CelListIterOpenImpl(
    uint32_t out_slot, uint32_t list_slot, const TrampolineContext& ctx);

// Layer-2 entry points for the kHost arms of the aggregate-op
// runtime dispatchers (`cel_list_size` / `cel_list_in` /
// `cel_list_eq` / `cel_list_concat` / `cel_map_size` / `cel_map_in`
// / `cel_map_eq`).  Each absorbs UNKNOWN / ERROR on its operands,
// dereferences the kHost backing via `ctx.refs.LookupList` /
// `LookupMap`, runs the spec-level operation, and writes the result
// CelValue into `out_slot`.  Spec-level errors (no_such_key,
// kind-mismatched element, …) travel inside the CelValue;
// non-OK Status only on infrastructure failure.  Routing rationale
// in `doc/implementation-plan/rewrite/map-list-dispatch.md`.
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
// the runtime's `arena_alloc`.  Current ship state: mixed origins
// POISON with TYPE_MISMATCH; full materialisation is follow-up work
// tracked in the M5 doc.
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

// Map equality accepts ANY origin pair (host+host, host+arena,
// arena+host) — both operands are normalized into host-side
// (key, value) CelValue snapshots before the set-equality walk, so a
// proto map field compares structurally against a map literal.  The
// runtime dispatcher short-circuits arena+arena in its own fast path
// before reaching here.
ABSL_MUST_USE_RESULT absl::Status CelMapEqImpl(uint32_t out_slot,
                                               uint32_t a_slot, uint32_t b_slot,
                                               const TrampolineContext& ctx);

// Comprehension-iter open for a `CEL_MAP_HOST` source.  Walks the
// HostMapBacking via ForEach, encodes each (key, value) pair into
// a flat 48-byte snapshot in the arena (key at +0, value at +24),
// and writes the runtime-side `MapIterState` at `state_offset`:
//
//     [ 0..4]  kind   = 1 (MAP_ITER_KIND_HOST)
//     [ 4..8]  cursor = 0 (pre-first)
//     [ 8..12] payload = snapshot start offset
//     [12..16] count   = snapshot entry count
//
// On empty source or arena OOM, sets `count = 0` so the runtime's
// `cel_map_iter_init` collapses the handle to 0 (empty iter).
//
// Pure scalar-key snapshots are 48B/entry (e.g. `{string→int}` →
// 96B for 2 entries).  Nested map/list/message values are encoded
// as `CEL_MAP_HOST` / `CEL_LIST_HOST` / `CEL_MESSAGE` referencing
// externref slots — same wire shape as inline indexed access.
ABSL_MUST_USE_RESULT absl::Status CelMapIterOpenImpl(
    uint32_t state_offset, uint32_t map_slot, const TrampolineContext& ctx);

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
//      through dedicated walkers in `cel_host.cc`; see
//      `cel-host-surface.md` for the per-shape dispatch.
// Non-OK Status surfaces as a wasm trap; the conformance harness
// records the row as failure without aborting the run.
ABSL_MUST_USE_RESULT absl::Status CelSetFieldImpl(uint32_t msg_slot,
                                                  uint32_t field_ref_id,
                                                  uint32_t value_slot,
                                                  const TrampolineContext& ctx);

// Timestamp / duration parse + format trampolines (formerly four
// host-side Impls) are now self-hosted in
// `runtime/cel_time_parse.cc`; codegen routes
// `string_to_timestamp` etc. to `cel_runtime.cel_*_at_v` directly.
// See `doc/implementation-plan/rewrite/phase-c-plan.md` §4.

// Single dispatch trampoline for the 10 with-TZ accessor overloads.
// Reads the timestamp + TZ-name string operands; loads the IANA /
// fixed-offset zone via `absl::TimeZone::Load`; projects the
// requested civil-time field per `accessor_kind` (matches
// `CelTzAccessorKind` enum in cel_time.h).  Invalid TZ name →
// CEL_ERROR(kInvalidArgument).  Bad accessor_kind →
// CEL_ERROR(kTypeMismatch) (defence in depth; codegen wouldn't
// emit an unknown kind).
ABSL_MUST_USE_RESULT absl::Status CelTimestampTzAccessorImpl(
    uint32_t out_slot, uint32_t ts_slot, uint32_t tz_slot,
    uint32_t accessor_kind, const TrampolineContext& ctx);

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

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_H_
