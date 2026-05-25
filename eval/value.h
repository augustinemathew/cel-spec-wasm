// Value — the user-facing counterpart to the 24-byte wire CelValue
// (runtime/cel_data.h::CelValue).  Owns its payload by value (no
// wire-memory references); safe to copy, move, pass by value.
//
// Construction is by named factory (`Value::Int(42)`,
// `Value::String("hi")`, `Value::Unknown(attr)`).  Inspection is by
// `StatusOr<T> AsX()`; a mismatch is a user error, not a crash.
//
// M1 scope: scalar kinds + Unknown + Error land fully.  Aggregate
// builders (List/Map/Message/OwnedMessage) and equality (`CelEquals`)
// are declared with signature-final stubs whose bodies `ABSL_CHECK`
// (per CLAUDE.md "unimplemented features" convention), surfacing
// pre-milestone callers loudly instead of silently miscompiling.
// The stubs keep the header stable across M1-Mn — populating an arm
// later does not change the user surface.

#ifndef CELWASM_EVAL_VALUE_H_
#define CELWASM_EVAL_VALUE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "eval/attribute.h"
#include "eval/error.h"

// Forward-decl to avoid pulling protobuf into api/value.h's transitive
// set.  Users that build a `Value::Message` already include protobuf.
namespace google::protobuf {
class Message;
}

// Forward-decl so Value can carry a shared_ptr<HostMessageBacking> in
// its payload without pulling cel_host.h into every includer.  Full
// definition lives in `eval/internal/cel_host.h`.
namespace celwasm {
class HostMessageBacking;
class HostMapBacking;
class HostListBacking;
}  // namespace celwasm

namespace celwasm {

// AttributeId lives in `namespace celwasm` (alongside the other
// attribute types — it must not collide with cel-cpp's `cel::Attribute*`
// when both libraries link into one binary).  ErrorPayload still lives
// in `namespace cel`.  These usings let the Value class body refer to
// them unqualified.
using ::celwasm::AttributeId;
using ::celwasm::ErrorPayload;

class Value {
 public:
  // Numeric values are stable on the wire — kept in sync with
  // `runtime/cel_data.h::CelKind`.
  enum class Kind : uint8_t {
    kNull = 0,
    kBool = 1,
    kInt = 2,
    kUint = 3,
    kDouble = 4,
    kString = 5,
    kBytes = 6,
    kList = 7,
    kMap = 8,
    kMessage = 9,
    kDuration = 11,
    kTimestamp = 12,
    // Type values.  Wire `CelKind::CEL_TYPE = 11`; user-facing
    // numbering is independent (kDuration already occupies 11 here).
    // Slot 13 was free.  See `rewrite/m9-type-subsystem.md`.
    kType = 13,
    kUnknown = 14,
    kError = 15,
  };

  // Default construction is Null — mirrors CEL's "value of the null
  // type" which is the only well-formed empty value.
  Value();

  // ————————— Builders —————————
  static Value Null();
  static Value Bool(bool v);
  static Value Int(int64_t v);
  static Value Uint(uint64_t v);
  static Value Double(double v);
  static Value String(std::string v);
  static Value Bytes(std::string v);
  static Value Duration(absl::Duration v);
  static Value Timestamp(absl::Time v);
  // type-of-types.  `name` is the spec type-name (`"int"`,
  // `"bool"`, `"<message-FQN>"`, `"null_type"`, `"list"`, `"map"`,
  // `"type"`, `"google.protobuf.Timestamp"`, ...).  Per langdef
  // §"Type Values" these are the values of expressions like `int`
  // standalone or `type(x)`.  No name validation — arbitrary bytes
  // are accepted (consistent with `Value::Message`).
  static Value Type(std::string name);
  static Value Unknown(AttributeId attr);
  static Value Error(ErrorPayload payload);

  // ——— Aggregate / message builders ———
  // OwnedMessage stays stubbed until M7 (proto literal construction).
  // Lists land in M4 (vector-backed `HostList` wrapper, mirroring
  // `Map`'s shape).
  static Value List(std::vector<Value> elements);
  // Wraps `entries` in a `celwasm::HostMap` (vector-backed) and
  // forwards to `Value::HostMap`.  Duplicate keys are flattened —
  // the runtime catches them via `cel_map_insert` for arena
  // literals; this host-side variant accepts the entries as given.
  static Value Map(std::vector<std::pair<Value, Value>> entries);
  static Value OwnedMessage(std::unique_ptr<google::protobuf::Message> m);

  // Carry a host-supplied message backing through Activation::Bind
  // into the cel_host dispatch.  ProtoBacking is the built-in for
  // `google::protobuf::Message`; embedders with JSON / XML /
  // struct-of-structs data shapes provide their own subclass of
  // `celwasm::HostMessageBacking` (see
  // `eval/internal/cel_host.h`).
  static Value HostMessage(
      std::shared_ptr<celwasm::HostMessageBacking> backing);

  // Convenience: wrap a `google::protobuf::Message` in a fresh
  // `ProtoBacking` and forward to `HostMessage`.  Non-owning — the
  // caller must keep `m` alive for the duration of any Eval that
  // observes this Value (typically the Activation-Bind-to-Eval
  // window).
  //
  // Implemented in `eval/internal/cel_host.cc` rather
  // than `value.cc` — the implementation needs `ProtoBacking`'s
  // complete definition, and keeping the dependency one-way
  // (cel_host -> value, never the reverse) avoids a library cycle.
  static Value Message(const google::protobuf::Message& m);

  // Carry a host-supplied map backing through Activation::Bind into
  // the cel_host dispatch.  Mirrors `HostMessage` for maps —
  // `celwasm::HostMap` is the vector-backed default; `celwasm::ProtoMap`
  // wraps a proto reflection map field.  Embedders with non-proto
  // map shapes provide their own subclass of `celwasm::HostMapBacking`.
  static Value HostMap(std::shared_ptr<celwasm::HostMapBacking> backing);

  // Carry a host-supplied list backing through Activation::Bind into
  // the cel_host dispatch.  Mirrors `HostMap` for lists —
  // `celwasm::HostList` is the vector-backed default;
  // `celwasm::ProtoList` wraps a proto repeated field.  Embedders
  // with non-proto list shapes provide their own subclass of
  // `celwasm::HostListBacking`.
  static Value HostList(std::shared_ptr<celwasm::HostListBacking> backing);

  // ————————— Inspection —————————
  Kind kind() const {
    return kind_;
  }
  bool IsNull() const {
    return kind_ == Kind::kNull;
  }
  bool IsUnknown() const {
    return kind_ == Kind::kUnknown;
  }
  bool IsError() const {
    return kind_ == Kind::kError;
  }

  // Typed accessors.  Mismatch (wrong kind) → InvalidArgument.  Bytes
  // and string share the underlying std::string; `AsString` /
  // `AsBytes` check the kind tag.
  absl::StatusOr<bool> AsBool() const;
  absl::StatusOr<int64_t> AsInt() const;
  absl::StatusOr<uint64_t> AsUint() const;
  absl::StatusOr<double> AsDouble() const;
  absl::StatusOr<absl::string_view> AsString() const;
  absl::StatusOr<absl::string_view> AsBytes() const;
  absl::StatusOr<absl::Duration> AsDuration() const;
  absl::StatusOr<absl::Time> AsTimestamp() const;
  // Returns the type-name string for a kType Value.
  // InvalidArgument on any other kind.
  absl::StatusOr<absl::string_view> AsType() const;
  absl::StatusOr<AttributeId> UnknownAttribute() const;
  absl::StatusOr<const ErrorPayload*> ErrorInfo() const;

  // Retrieve the host-side backing for a kMessage-kind Value.
  // Returns `InvalidArgument` on any other kind.
  absl::StatusOr<const celwasm::HostMessageBacking*> MessageBacking() const;

  // Shared-ownership handle to the message backing.  Use this
  // (instead of MessageBacking()) when the caller needs to store
  // the backing past the lifetime of this Value — e.g. the
  // cel_host trampoline's ExternrefTable::Intern, which holds the
  // backing for a full Eval.
  absl::StatusOr<std::shared_ptr<const celwasm::HostMessageBacking>>
  SharedMessageBacking() const;

  // Retrieve the host-side backing for a kMap-kind Value.  Returns
  // `InvalidArgument` on any other kind.  Mirrors `MessageBacking`.
  absl::StatusOr<const celwasm::HostMapBacking*> MapBacking() const;
  absl::StatusOr<std::shared_ptr<const celwasm::HostMapBacking>>
  SharedMapBacking() const;

  // Retrieve the host-side backing for a kList-kind Value.  Returns
  // `InvalidArgument` on any other kind.  Mirrors `MapBacking`.
  absl::StatusOr<const celwasm::HostListBacking*> ListBacking() const;
  absl::StatusOr<std::shared_ptr<const celwasm::HostListBacking>>
  SharedListBacking() const;

  // Structural equality.  Scalar kinds compare by value; aggregate
  // kinds (kMessage / kMap / kList) compare by backing-pointer
  // identity — spec-compliant element-wise equality lives in the
  // runtime / M5 `==` overloads.  Returns false if kinds differ.
  // For 3VL-aware equality (`CelEquals`) that absorbs Unknown /
  // Error per langdef, see the runtime — not this surface.
  bool StructurallyEquals(const Value& other) const;

 private:
  // Discriminated by kind_.  kBytes shares the std::string alternative
  // with kString — the kind tag disambiguates.
  struct Empty {};
  using Payload =
      std::variant<Empty,                          // Null
                   bool,                           // Bool
                   int64_t,                        // Int
                   uint64_t,                       // Uint
                   double,                         // Double
                   std::string,                    // String/Bytes
                   absl::Duration,                 // Duration
                   absl::Time,                     // Timestamp
                   AttributeId,                    // Unknown
                   std::shared_ptr<ErrorPayload>,  // Error
                   std::shared_ptr<celwasm::HostMessageBacking>,  // Message
                   std::shared_ptr<celwasm::HostMapBacking>,      // Map
                   std::shared_ptr<celwasm::HostListBacking>>;    // List

  Kind kind_;
  Payload payload_;

  // Tag type used in the constructor to discriminate string-vs-bytes
  // at the private ctor site without multiple public factories
  // colliding.
  struct StringTag {};
  struct BytesTag {};
  // kType values share the std::string Payload alternative
  // with kString / kBytes; the kind_ tag disambiguates.
  struct TypeTag {};
  Value(StringTag, std::string s);
  Value(BytesTag, std::string s);
  Value(TypeTag, std::string s);
};

absl::string_view ValueKindName(Value::Kind k);

}  // namespace celwasm

#endif  // CELWASM_EVAL_VALUE_H_
