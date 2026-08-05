// cel_host Layer 1 — per-backing field/element-read semantics: the
// HostMessageBacking / HostMapBacking / HostListBacking plug-in
// points, their built-in proto/vector concretes, and the shared
// proto-read helpers (WKT peel chain, field resolution + read
// classification) the Layer-2 trampolines dispatch through.  See the
// aggregator `cel_host.h` for the three-layer split.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_BACKING_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_BACKING_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "eval/value.h"
#include "shared/type.h"

namespace google::protobuf {
class Descriptor;
class DescriptorPool;
class FieldDescriptor;
class Message;
class Reflection;
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
// trampoline, `cel_host_common.cc::EncodeSpan` eagerly
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

// Precomputed read-dispatch classification for a resolved proto
// field.  Derived once from the FieldDescriptor (whose shape is
// immutable) so the per-read path dispatches on this enum instead of
// re-deriving `is_map()` / `is_repeated()` / WKT `full_name()`
// string compares on every field read.
enum class ProtoFieldReadClass : uint8_t {
  kScalar = 0,        // numeric / bool / enum / string / bytes
  kMap,               // map<K, V> field
  kRepeated,          // repeated (non-map) field
  kMessageWrapper,    // google.protobuf.{Bool,Int32,...,Bytes}Value
  kMessageAny,        // google.protobuf.Any
  kMessageTimestamp,  // google.protobuf.Timestamp
  kMessageDuration,   // google.protobuf.Duration
  kMessageJson,       // google.protobuf.{Value,Struct,ListValue}
  kMessagePlain,      // any other singular message
};

// Single-entry resolved-field cache, keyed by the owning message's
// `Descriptor*`.  Holds everything `ProtoBacking::ReadField` used to
// re-derive per read: the resolved `FieldDescriptor*`, the read
// classification above, and (for WKT wrapper / time fields) the
// inner sub-field descriptors the peelers dereference.
//
// `owner` is the validity key: a hit requires pointer-identity with
// the incoming message's `GetDescriptor()`, so messages bound from
// any descriptor pool (generated or dynamic) stay correct — a
// different pool's same-FQN type is a different `Descriptor*` and
// re-resolves.  Lifetime: the cached pointers are dereferenced only
// after a key match, i.e. only while a live message of that exact
// descriptor is being read; embedders binding messages from
// short-lived dynamic pools must keep the pool alive for the
// Instance's lifetime (the same contract `MessageTypeEntry`'s
// Plan-time `descriptor` already imposes).
struct ResolvedFieldCache {
  const google::protobuf::Descriptor* absl_nullable owner = nullptr;
  const google::protobuf::FieldDescriptor* absl_nullable field = nullptr;
  ProtoFieldReadClass read_class = ProtoFieldReadClass::kScalar;
  // kMessageWrapper: the wrapper's inner `value` field (number 1).
  // kMessageTimestamp / kMessageDuration: `seconds` (number 1).
  const google::protobuf::FieldDescriptor* absl_nullable sub_field1 = nullptr;
  // kMessageTimestamp / kMessageDuration: `nanos` (number 2).
  const google::protobuf::FieldDescriptor* absl_nullable sub_field2 = nullptr;
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

// ── Shared proto-read helpers ────────────────────────────────────
//
// Defined in cel_host_backing.cc; used by the Layer-2 trampoline TUs
// (cel_host_message.cc / cel_host_set_field.cc) as well as by the
// backing classes above.  Full contracts live on the definitions.

// True iff `fqn` names one of the 9 google.protobuf wrapper types.
bool IsWrapperFqn(absl::string_view fqn);

// Descriptor-gated peel of a wrapper message to its inner scalar;
// nullopt when `sub` is not a wrapper.
std::optional<celwasm::Value> UnpackWrapperMessage(
    const google::protobuf::Message& sub);

// Peel google.protobuf.Timestamp / Duration to the matching temporal
// Value; nullopt for any other message type.
std::optional<celwasm::Value> UnpackWellKnownTimeMessage(
    const google::protobuf::Message& sub);

// Precompute the read-dispatch classification for a resolved field
// into `out` (which must already carry `out->field == &field`).
void ClassifyResolvedField(const google::protobuf::FieldDescriptor& field,
                           ResolvedFieldCache* absl_nonnull out);

// Read a singular CPPTYPE_MESSAGE field per the precomputed
// classification `c` (one of the kMessage* arms).
absl::StatusOr<celwasm::Value> ReadClassifiedMessageField(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const ResolvedFieldCache& c);

// Resolve the FieldDescriptor on a message preferring the wire field
// number when non-zero; falling back to by-name (incl. proto2
// extensions addressed by fully-qualified name).
const google::protobuf::FieldDescriptor* absl_nullable ResolveFieldDescriptor(
    const google::protobuf::Message& msg, int field_number,
    absl::string_view field_name);

// has(msg.field) on an already-resolved descriptor.
bool ProtoHasFieldResolved(const google::protobuf::Message& msg,
                           const google::protobuf::FieldDescriptor& field);

// FieldDescriptor for the synthetic key/value sub-field (1 = key,
// 2 = value) on a proto map field's entry message type.
const google::protobuf::FieldDescriptor* absl_nonnull MapEntryField(
    const google::protobuf::FieldDescriptor& field, int number);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_BACKING_H_
