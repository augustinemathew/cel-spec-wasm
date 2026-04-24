// cel_host — host-side helpers the wasm expr module calls through
// its `cel_host.*` imports.  Three-layer split per
// `m2-ident-select-unknowns.md §2.4`:
//
//   Layer 1 — HostMessageBacking + per-backing impls.  Pure CEL
//             semantics.  Given a message (proto, JSON, …), read
//             or test a named field and return a `cel::Value`.
//             No wasm, no memory views, no externref tables.
//
//   Layer 2 — Trampoline semantics.  Adapts Layer 1 to the wasm
//             ABI: unpacks CelValues from linear memory, absorbs
//             UNKNOWN / ERROR, checks unknown-pattern matches,
//             marshals the Layer-1 `Value` back into a 24-byte
//             CelValue at `out_slot`.  Runtime-agnostic — driven
//             by abstractions (MemoryView, ExternrefTable,
//             ArenaAllocator).  Lands in M2.C.0b.
//
//   Layer 3 — wasmtime glue.  Wires Layer 2 onto a
//             `wasmtime_linker_t` under the `cel_host` module
//             namespace.  Lands in M2.C.5.
//
// This header declares Layer 1 + the interfaces Layer 2 will
// depend on (so M2.C.0b is an additive slice).  M6 adds concrete
// HostMap/HostListBacking implementations; the interfaces are
// declared here now as signature-final stubs per CLAUDE.md.

#ifndef CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_H_
#define CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "absl/base/attributes.h"
#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"

// Forward-declare proto types so callers that don't need them
// (unit tests on JsonBacking, for instance) don't pull them in.
namespace google::protobuf {
class Message;
}

namespace celwasm {

// ═══════════════════════════════════════════════════════════════
// Layer 1 — per-backing field-read / field-test semantics.
// ═══════════════════════════════════════════════════════════════

// Polymorphism hook for "how do I plug in <backing type> as a CEL
// message?"  One shipping impl at M2 (ProtoBacking); embedders
// subclass to add their own (JSON, struct-of-structs, XML).
// `cel::Activation::Bind(name, Value::HostMessage(backing))`
// carries the backing through to this interface at `cel_host`
// dispatch time.
class HostMessageBacking {
 public:
  virtual ~HostMessageBacking() = default;

  HostMessageBacking() = default;
  HostMessageBacking(const HostMessageBacking&) = delete;
  HostMessageBacking& operator=(const HostMessageBacking&) = delete;

  // Read one field.
  //   `field_number` is 0 for non-proto-backed backings (name-only
  //     resolution); always non-zero for ProtoBacking.
  //   `field_name` is always populated.
  //   `expected_type` is what the checker says this field's result
  //     type should be — backings consult it to pick the return
  //     shape (e.g. Value::HostMessage(...) for a MESSAGE-typed
  //     field, Value::String(...) for a STRING-typed field).
  // Returns:
  //   - The `Value` on success.
  //   - `Value::Error(kFieldNotFound)` when the backing can't
  //     resolve the field.
  //   - `Value::Error(kTypeUnsupported)` for MAP / REPEATED
  //     (M2 envelope boundary — M6 flips to host-backed aggregates).
  //   - Non-OK Status on an infrastructure failure; CEL_ERROR
  //     semantics (wrong-kind, missing-field) surface as
  //     `Value::Error`, not Status.
  virtual absl::StatusOr<cel::Value> ReadField(
      int field_number, absl::string_view field_name,
      const cel::CelType& expected_type) = 0;

  // Returns `has(msg.field)` per langdef.md's proto2/proto3 rules
  // for the native backing.  Embedders decide what "has" means for
  // non-proto backings — the typical rule is "the key exists in
  // the underlying tree and its value isn't null-equivalent".
  virtual bool HasField(int field_number, absl::string_view field_name) = 0;
};

// Built-in adapter over `google::protobuf::Message` + its
// descriptor-pool-resolved reflection.  Transcribed from v1's
// `compiler/host/cel_host.cc::ReadField` / `HasField` bodies with
// the signature shape changed from `(CelValue* out, ArenaAllocator,
// InternMessage)` to `StatusOr<Value>` — Layer 2 handles the
// CelValue marshalling + arena + externref concerns.
//
// Non-owning: the message must outlive the ProtoBacking.  In
// practice the host's `Activation::Bind("c", Value::Message(msg))`
// keeps the message alive for the duration of the Eval call; the
// backing stays valid across back-to-back Evals as long as the
// caller's `msg` local does.
class ProtoBacking final : public HostMessageBacking {
 public:
  explicit ProtoBacking(const google::protobuf::Message* absl_nonnull msg)
      : msg_(msg) {}

  absl::StatusOr<cel::Value> ReadField(
      int field_number, absl::string_view field_name,
      const cel::CelType& expected_type) override;

  bool HasField(int field_number, absl::string_view field_name) override;

  // Accessor for tests + nested-field helpers that need the raw
  // pointer (e.g. the two-hop `c.billing_address.city` case has to
  // wrap a sub-message in a fresh ProtoBacking).
  const google::protobuf::Message* absl_nonnull message() const {
    return msg_;
  }

 private:
  const google::protobuf::Message* absl_nonnull msg_;
};

// ───── HostMapBacking / HostListBacking — M6 stubs ─────
//
// Shipped at M2 as signature-final interfaces so M6 is an additive
// slice: Value::HostMap / Value::HostList (api/value.h §2.1.1),
// the Layer-2 trampoline signatures (§2.4.2), and the
// NodeAnnotation::map_origin / list_origin fields
// (ir/annotations.h §2.6) are all already in place.
//
// The interfaces are definitional — no concrete implementations
// until M6 lands `ProtoMapBacking` + `ProtoRepeatedBacking`.  Per
// CLAUDE.md "unimplemented features": virtual methods on a class
// the runtime never instantiates don't need ABSL_CHECK bodies; the
// runtime-edge stub is on `Value::HostMap(backing)` / `Value::Host
// List(backing)` (api/value.cc — crashes today).

class HostMapBacking {
 public:
  virtual ~HostMapBacking() = default;

  HostMapBacking() = default;
  HostMapBacking(const HostMapBacking&) = delete;
  HostMapBacking& operator=(const HostMapBacking&) = delete;

  virtual size_t Size() const = 0;
  virtual absl::StatusOr<cel::Value> Get(
      const cel::Value& key, const cel::CelType& expected_value_type) = 0;
  virtual bool ContainsKey(const cel::Value& key) const = 0;
  virtual void ForEach(
      absl::FunctionRef<void(const cel::Value&, const cel::Value&)> visit)
      const = 0;
};

class HostListBacking {
 public:
  virtual ~HostListBacking() = default;

  HostListBacking() = default;
  HostListBacking(const HostListBacking&) = delete;
  HostListBacking& operator=(const HostListBacking&) = delete;

  virtual size_t Size() const = 0;
  virtual absl::StatusOr<cel::Value> At(
      size_t index, const cel::CelType& expected_element_type) = 0;
  virtual void ForEach(
      absl::FunctionRef<void(const cel::Value&)> visit) const = 0;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_API_INTERNAL_CEL_HOST_H_
