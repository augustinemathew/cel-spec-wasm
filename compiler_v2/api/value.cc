#include "compiler_v2/api/value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "compiler_v2/api/attribute.h"
#include "compiler_v2/api/error.h"
#include "google/protobuf/message.h"  // needed for ~unique_ptr<Message>

namespace cel {

namespace {

absl::Status KindMismatch(absl::string_view wanted, Value::Kind got) {
  return absl::InvalidArgumentError(
      absl::StrCat("Value: expected ", wanted, ", got ", ValueKindName(got)));
}

}  // namespace

Value::Value() : kind_(Kind::kNull), payload_(Empty{}) {}

Value::Value(StringTag, std::string s)
    : kind_(Kind::kString), payload_(std::move(s)) {}
Value::Value(BytesTag, std::string s)
    : kind_(Kind::kBytes), payload_(std::move(s)) {}

Value Value::Null() {
  return {};
}
Value Value::Bool(bool v) {
  Value r;
  r.kind_ = Kind::kBool;
  r.payload_ = v;
  return r;
}
Value Value::Int(int64_t v) {
  Value r;
  r.kind_ = Kind::kInt;
  r.payload_ = v;
  return r;
}
Value Value::Uint(uint64_t v) {
  Value r;
  r.kind_ = Kind::kUint;
  r.payload_ = v;
  return r;
}
Value Value::Double(double v) {
  Value r;
  r.kind_ = Kind::kDouble;
  r.payload_ = v;
  return r;
}
Value Value::String(std::string v) {
  return Value(StringTag{}, std::move(v));
}
Value Value::Bytes(std::string v) {
  return Value(BytesTag{}, std::move(v));
}
Value Value::Duration(absl::Duration v) {
  Value r;
  r.kind_ = Kind::kDuration;
  r.payload_ = v;
  return r;
}
Value Value::Timestamp(absl::Time v) {
  Value r;
  r.kind_ = Kind::kTimestamp;
  r.payload_ = v;
  return r;
}
Value Value::Unknown(AttributeId attr) {
  Value r;
  r.kind_ = Kind::kUnknown;
  r.payload_ = attr;
  return r;
}
Value Value::Error(ErrorPayload payload) {
  Value r;
  r.kind_ = Kind::kError;
  r.payload_ = std::make_shared<ErrorPayload>(std::move(payload));
  return r;
}

// ——— Aggregate / message builders: signature-final stubs ———
// Signature is locked by cel-host-surface.md §2.5 (pass-by-value sink
// pattern); stub bodies ABSL_CHECK until the milestone that lights
// them up.  The `auto taken = std::move(...)` lines both demonstrate
// the sink semantics and satisfy `performance-unnecessary-value-param`
// (moving binds an rvalue ref, which counts as a non-const use).
Value Value::List(std::vector<Value> elements) {
  [[maybe_unused]] auto taken = std::move(elements);
  ABSL_CHECK(false) << "Value::List is a stub until M6 (lists + maps)";
}
Value Value::Map(std::vector<std::pair<Value, Value>> entries) {
  [[maybe_unused]] auto taken = std::move(entries);
  ABSL_CHECK(false) << "Value::Map is a stub until M6 (lists + maps)";
}
Value Value::Message(const google::protobuf::Message& /*m*/) {
  ABSL_CHECK(false) << "Value::Message is a stub until M2 (proto field reads)";
}
Value Value::OwnedMessage(std::unique_ptr<google::protobuf::Message> m) {
  [[maybe_unused]] auto taken = std::move(m);
  ABSL_CHECK(false)
      << "Value::OwnedMessage is a stub until M7 (proto literal construction)";
}

// ————————— Accessors —————————
absl::StatusOr<bool> Value::AsBool() const {
  if (kind_ != Kind::kBool) return KindMismatch("bool", kind_);
  return std::get<bool>(payload_);
}
absl::StatusOr<int64_t> Value::AsInt() const {
  if (kind_ != Kind::kInt) return KindMismatch("int", kind_);
  return std::get<int64_t>(payload_);
}
absl::StatusOr<uint64_t> Value::AsUint() const {
  if (kind_ != Kind::kUint) return KindMismatch("uint", kind_);
  return std::get<uint64_t>(payload_);
}
absl::StatusOr<double> Value::AsDouble() const {
  if (kind_ != Kind::kDouble) return KindMismatch("double", kind_);
  return std::get<double>(payload_);
}
absl::StatusOr<absl::string_view> Value::AsString() const {
  if (kind_ != Kind::kString) return KindMismatch("string", kind_);
  return absl::string_view(std::get<std::string>(payload_));
}
absl::StatusOr<absl::string_view> Value::AsBytes() const {
  if (kind_ != Kind::kBytes) return KindMismatch("bytes", kind_);
  return absl::string_view(std::get<std::string>(payload_));
}
absl::StatusOr<absl::Duration> Value::AsDuration() const {
  if (kind_ != Kind::kDuration) return KindMismatch("duration", kind_);
  return std::get<absl::Duration>(payload_);
}
absl::StatusOr<absl::Time> Value::AsTimestamp() const {
  if (kind_ != Kind::kTimestamp) return KindMismatch("timestamp", kind_);
  return std::get<absl::Time>(payload_);
}
absl::StatusOr<AttributeId> Value::UnknownAttribute() const {
  if (kind_ != Kind::kUnknown) return KindMismatch("unknown", kind_);
  return std::get<AttributeId>(payload_);
}
absl::StatusOr<const ErrorPayload*> Value::ErrorInfo() const {
  if (kind_ != Kind::kError) return KindMismatch("error", kind_);
  return std::get<std::shared_ptr<ErrorPayload>>(payload_).get();
}

bool Value::StructurallyEquals(const Value& other) const {
  if (kind_ != other.kind_) return false;
  switch (kind_) {
    case Kind::kNull:
      return true;
    case Kind::kBool:
      return std::get<bool>(payload_) == std::get<bool>(other.payload_);
    case Kind::kInt:
      return std::get<int64_t>(payload_) == std::get<int64_t>(other.payload_);
    case Kind::kUint:
      return std::get<uint64_t>(payload_) == std::get<uint64_t>(other.payload_);
    case Kind::kDouble:
      // Structural: bitwise compare via memcmp would be finer but
      // simple == is fine for host-side test code (the documented use).
      return std::get<double>(payload_) == std::get<double>(other.payload_);
    case Kind::kString:
    case Kind::kBytes:
      return std::get<std::string>(payload_) ==
             std::get<std::string>(other.payload_);
    case Kind::kDuration:
      return std::get<absl::Duration>(payload_) ==
             std::get<absl::Duration>(other.payload_);
    case Kind::kTimestamp:
      return std::get<absl::Time>(payload_) ==
             std::get<absl::Time>(other.payload_);
    case Kind::kUnknown:
      return std::get<AttributeId>(payload_) ==
             std::get<AttributeId>(other.payload_);
    case Kind::kError: {
      const auto& a = *std::get<std::shared_ptr<ErrorPayload>>(payload_);
      const auto& b = *std::get<std::shared_ptr<ErrorPayload>>(other.payload_);
      return a.code == b.code && a.message == b.message &&
             a.expr_id == b.expr_id;
    }
    case Kind::kList:
    case Kind::kMap:
    case Kind::kMessage:
      ABSL_CHECK(false)
          << "StructurallyEquals on aggregates is a stub until M6/M7";
  }
  ABSL_CHECK(false) << "unhandled Value::Kind = " << static_cast<int>(kind_);
}

absl::string_view ValueKindName(Value::Kind k) {
  switch (k) {
    case Value::Kind::kNull:
      return "null";
    case Value::Kind::kBool:
      return "bool";
    case Value::Kind::kInt:
      return "int";
    case Value::Kind::kUint:
      return "uint";
    case Value::Kind::kDouble:
      return "double";
    case Value::Kind::kString:
      return "string";
    case Value::Kind::kBytes:
      return "bytes";
    case Value::Kind::kList:
      return "list";
    case Value::Kind::kMap:
      return "map";
    case Value::Kind::kMessage:
      return "message";
    case Value::Kind::kDuration:
      return "duration";
    case Value::Kind::kTimestamp:
      return "timestamp";
    case Value::Kind::kUnknown:
      return "unknown";
    case Value::Kind::kError:
      return "error";
  }
  ABSL_CHECK(false) << "unhandled Value::Kind = " << static_cast<int>(k);
}

}  // namespace cel
