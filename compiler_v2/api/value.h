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

#ifndef CELWASM_COMPILER_V2_API_VALUE_H_
#define CELWASM_COMPILER_V2_API_VALUE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "compiler_v2/api/attribute.h"
#include "compiler_v2/api/error.h"

// Forward-decl to avoid pulling protobuf into api/value.h's transitive
// set.  Users that build a `Value::Message` already include protobuf.
namespace google::protobuf {
class Message;
}

namespace cel {

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
  static Value Unknown(AttributeId attr);
  static Value Error(ErrorPayload payload);

  // ——— Aggregate / message builders (signature-final stubs) ———
  // Bodies `ABSL_CHECK` until their milestone (lists + maps: M6;
  // messages: M2 for read, M7 for construction).  Declared here so
  // the surface shape doesn't change later.
  static Value List(std::vector<Value> elements);
  static Value Map(std::vector<std::pair<Value, Value>> entries);
  static Value Message(const google::protobuf::Message& m);
  static Value OwnedMessage(std::unique_ptr<google::protobuf::Message> m);

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
  absl::StatusOr<AttributeId> UnknownAttribute() const;
  absl::StatusOr<const ErrorPayload*> ErrorInfo() const;

  // Structural equality — scalar-only at M1.  Aggregates / messages
  // delegate to the M6 / M2 bodies respectively.  Returns false if
  // kinds differ.  For 3VL-aware equality (`CelEquals`) that absorbs
  // Unknown / Error per langdef, see the runtime — not this surface.
  bool StructurallyEquals(const Value& other) const;

 private:
  // Discriminated by kind_.  kBytes shares the std::string alternative
  // with kString — the kind tag disambiguates.
  struct Empty {};
  using Payload = std::variant<Empty,                           // Null
                               bool,                            // Bool
                               int64_t,                         // Int
                               uint64_t,                        // Uint
                               double,                          // Double
                               std::string,                     // String/Bytes
                               absl::Duration,                  // Duration
                               absl::Time,                      // Timestamp
                               AttributeId,                     // Unknown
                               std::shared_ptr<ErrorPayload>>;  // Error

  Kind kind_;
  Payload payload_;

  // Tag type used in the constructor to discriminate string-vs-bytes
  // at the private ctor site without multiple public factories
  // colliding.
  struct StringTag {};
  struct BytesTag {};
  Value(StringTag, std::string s);
  Value(BytesTag, std::string s);
};

absl::string_view ValueKindName(Value::Kind k);

}  // namespace cel

#endif  // CELWASM_COMPILER_V2_API_VALUE_H_
