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
Value::Value(TypeTag, std::string s)
    : kind_(Kind::kType), payload_(std::move(s)) {}

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
Value Value::Type(std::string name) {
  return Value(TypeTag{}, std::move(name));
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

// ——— Aggregate / message builders ———
// Signature is locked by cel-host-surface.md §2.5 (pass-by-value sink
// pattern); stub bodies ABSL_CHECK until the milestone that lights
// them up.
//
// Forwarded to api/internal/cel_host.cc — needs `HostList` /
// `HostMap`'s complete definition.  Same one-way dep rule as
// `Value::Message`.
//
//   Value Value::List(std::vector<Value>)            cel_host.cc
//   Value Value::HostList(...)                       cel_host.cc
//   Value Value::Map(std::vector<std::pair<...>>)    cel_host.cc
//   Value Value::HostMap(...)                        cel_host.cc
// Defined in `api/internal/cel_host.cc` — needs `OwnedProtoBacking`'s
// complete definition.  Same one-way dep rule as `Value::Message`.

// HostMessage is the general-purpose message constructor — it takes
// any `HostMessageBacking` subclass (`ProtoBacking` for proto,
// embedder subclasses for JSON / XML / …) and stashes it on the
// Value.  `Value::Message(proto)` is the proto-specific convenience,
// defined in `api/internal/cel_host.cc` (needs ProtoBacking's full
// type) to avoid a circular library dep: value.cc doesn't know
// about ProtoBacking; cel_host.cc depends on value.h.
Value Value::HostMessage(std::shared_ptr<celwasm::HostMessageBacking> backing) {
  ABSL_CHECK(backing != nullptr)
      << "Value::HostMessage: backing must not be null";
  Value r;
  r.kind_ = Kind::kMessage;
  r.payload_ = std::move(backing);
  return r;
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
absl::StatusOr<absl::string_view> Value::AsType() const {
  if (kind_ != Kind::kType) return KindMismatch("type", kind_);
  return absl::string_view(std::get<std::string>(payload_));
}
absl::StatusOr<AttributeId> Value::UnknownAttribute() const {
  if (kind_ != Kind::kUnknown) return KindMismatch("unknown", kind_);
  return std::get<AttributeId>(payload_);
}
absl::StatusOr<const ErrorPayload*> Value::ErrorInfo() const {
  if (kind_ != Kind::kError) return KindMismatch("error", kind_);
  return std::get<std::shared_ptr<ErrorPayload>>(payload_).get();
}
absl::StatusOr<const celwasm::HostMessageBacking*> Value::MessageBacking()
    const {
  if (kind_ != Kind::kMessage) return KindMismatch("message", kind_);
  return std::get<std::shared_ptr<celwasm::HostMessageBacking>>(payload_).get();
}
absl::StatusOr<std::shared_ptr<const celwasm::HostMessageBacking>>
Value::SharedMessageBacking() const {
  if (kind_ != Kind::kMessage) return KindMismatch("message", kind_);
  return std::get<std::shared_ptr<celwasm::HostMessageBacking>>(payload_);
}
absl::StatusOr<const celwasm::HostMapBacking*> Value::MapBacking() const {
  if (kind_ != Kind::kMap) return KindMismatch("map", kind_);
  return std::get<std::shared_ptr<celwasm::HostMapBacking>>(payload_).get();
}
absl::StatusOr<std::shared_ptr<const celwasm::HostMapBacking>>
Value::SharedMapBacking() const {
  if (kind_ != Kind::kMap) return KindMismatch("map", kind_);
  return std::get<std::shared_ptr<celwasm::HostMapBacking>>(payload_);
}
absl::StatusOr<const celwasm::HostListBacking*> Value::ListBacking() const {
  if (kind_ != Kind::kList) return KindMismatch("list", kind_);
  return std::get<std::shared_ptr<celwasm::HostListBacking>>(payload_).get();
}
absl::StatusOr<std::shared_ptr<const celwasm::HostListBacking>>
Value::SharedListBacking() const {
  if (kind_ != Kind::kList) return KindMismatch("list", kind_);
  return std::get<std::shared_ptr<celwasm::HostListBacking>>(payload_);
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
    case Kind::kType:
      // kType shares the std::string Payload alternative; the kind
      // tag (already checked above) disambiguates.  Byte-equality per
      // langdef §"Equality" + m9-type-subsystem.md §3.4.
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
    case Kind::kMessage:
      // Pointer-identity on the backing — element-wise equality lands
      // in the runtime / M5 `==` overloads.
      return std::get<std::shared_ptr<celwasm::HostMessageBacking>>(payload_) ==
             std::get<std::shared_ptr<celwasm::HostMessageBacking>>(
                 other.payload_);
    case Kind::kMap:
      return std::get<std::shared_ptr<celwasm::HostMapBacking>>(payload_) ==
             std::get<std::shared_ptr<celwasm::HostMapBacking>>(other.payload_);
    case Kind::kList:
      return std::get<std::shared_ptr<celwasm::HostListBacking>>(payload_) ==
             std::get<std::shared_ptr<celwasm::HostListBacking>>(
                 other.payload_);
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
    case Value::Kind::kType:
      return "type";
    case Value::Kind::kUnknown:
      return "unknown";
    case Value::Kind::kError:
      return "error";
  }
  ABSL_CHECK(false) << "unhandled Value::Kind = " << static_cast<int>(k);
}

}  // namespace cel
