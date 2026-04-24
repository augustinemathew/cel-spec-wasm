#include "compiler_v2/api/internal/cel_host.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/error.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"

namespace celwasm {

namespace {

// Error payloads Layer 1 surfaces via `Value::Error`.  CEL_ERROR
// semantics (missing field, wrong type) travel inside `Value`; only
// infrastructure failures (null deref, checker inconsistency) travel
// as `absl::Status`.
cel::Value FieldNotFound(absl::string_view name) {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/cel::ErrorCode::kFieldNotFound,
      /*message=*/std::string(name),
      /*expr_id=*/0,
  });
}

cel::Value TypeUnsupported(absl::string_view name) {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/cel::ErrorCode::kTypeUnsupported,
      /*message=*/std::string(name),
      /*expr_id=*/0,
  });
}

// Port of v1 `ReadNumericField` — dispatches on the field's
// `cpp_type` to build a `cel::Value` of the matching scalar kind.
// Returns `std::nullopt` on non-numeric fields; the caller handles
// string / bytes / message branches.
std::optional<cel::Value> ReadNumericField(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      return cel::Value::Bool(refl.GetBool(msg, &field));
    case FD::CPPTYPE_INT32:
      return cel::Value::Int(refl.GetInt32(msg, &field));
    case FD::CPPTYPE_INT64:
      return cel::Value::Int(refl.GetInt64(msg, &field));
    case FD::CPPTYPE_UINT32:
      return cel::Value::Uint(refl.GetUInt32(msg, &field));
    case FD::CPPTYPE_UINT64:
      return cel::Value::Uint(refl.GetUInt64(msg, &field));
    case FD::CPPTYPE_FLOAT:
      return cel::Value::Double(refl.GetFloat(msg, &field));
    case FD::CPPTYPE_DOUBLE:
      return cel::Value::Double(refl.GetDouble(msg, &field));
    case FD::CPPTYPE_ENUM:
      // CEL treats proto enum values as ints (langdef §2.4.7).
      return cel::Value::Int(refl.GetEnumValue(msg, &field));
    default:
      return std::nullopt;
  }
}

// Read one singular proto field, returning the matching cel::Value.
// Non-OK Status is reserved for infrastructure failures (reflection
// missing, descriptor null) — spec-level errors (field not found,
// repeated read at M2) surface as `Value::Error`.
absl::StatusOr<cel::Value> ReadScalarField(
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field) {
  const google::protobuf::Reflection* refl = msg.GetReflection();
  if (refl == nullptr) {
    return absl::InternalError(
        "ProtoBacking::ReadField: message has no reflection");
  }
  using FD = google::protobuf::FieldDescriptor;
  if (auto numeric = ReadNumericField(*refl, msg, field); numeric.has_value()) {
    return *std::move(numeric);
  }
  if (field.cpp_type() == FD::CPPTYPE_STRING) {
    std::string scratch;
    const std::string& s = refl->GetStringReference(msg, &field, &scratch);
    if (field.type() == FD::TYPE_BYTES) {
      return cel::Value::Bytes(std::string(s));
    }
    return cel::Value::String(std::string(s));
  }
  if (field.cpp_type() == FD::CPPTYPE_MESSAGE) {
    const google::protobuf::Message& sub = refl->GetMessage(msg, &field);
    // Wrap the sub-message in a fresh ProtoBacking.  Non-owning
    // pointer — the root message's lifetime covers every nested
    // field per protobuf contract.
    return cel::Value::HostMessage(std::make_shared<ProtoBacking>(&sub));
  }
  return absl::InternalError(absl::StrCat(
      "ProtoBacking::ReadField: unhandled cpp_type ",
      static_cast<int>(field.cpp_type()), " on field `", field.name(), "`"));
}

// Resolve the FieldDescriptor on a message preferring the wire field
// number when non-zero; falling back to a by-name lookup.  The
// fallback is the forward-compat path for non-proto backings (JSON /
// map) that emit `field_number = 0`.
const google::protobuf::FieldDescriptor* absl_nullable ResolveFieldDescriptor(
    const google::protobuf::Message& msg, int field_number,
    absl::string_view field_name) {
  const google::protobuf::Descriptor* d = msg.GetDescriptor();
  if (d == nullptr) return nullptr;
  if (field_number != 0) return d->FindFieldByNumber(field_number);
  return d->FindFieldByName(std::string(field_name));
}

}  // namespace

// ══════════════════════════════════════════════════════════════════
// ProtoBacking — Layer 1 over google::protobuf::Message.
// ══════════════════════════════════════════════════════════════════

absl::StatusOr<cel::Value> ProtoBacking::ReadField(
    int field_number, absl::string_view field_name,
    const cel::CelType& /*expected_type*/) {
  ABSL_CHECK(msg_ != nullptr) << "ProtoBacking::ReadField: null message";
  const google::protobuf::FieldDescriptor* field =
      ResolveFieldDescriptor(*msg_, field_number, field_name);
  if (field == nullptr) return FieldNotFound(field_name);

  // M2 envelope boundary: MAP / REPEATED field reads are not yet
  // lowered (M6 adds `ProtoRepeatedBacking` / `ProtoMapBacking`).
  // Surface CEL_ERR_TYPE_UNSUPPORTED instead of silently reading
  // the 0th element or returning an incomplete value.
  if (field->is_repeated() || field->is_map()) {
    return TypeUnsupported(field_name);
  }
  return ReadScalarField(*msg_, *field);
}

bool ProtoBacking::HasField(int field_number, absl::string_view field_name) {
  ABSL_CHECK(msg_ != nullptr) << "ProtoBacking::HasField: null message";
  const google::protobuf::FieldDescriptor* field =
      ResolveFieldDescriptor(*msg_, field_number, field_name);
  if (field == nullptr) return false;
  const google::protobuf::Reflection* refl = msg_->GetReflection();
  if (refl == nullptr) return false;
  if (field->is_repeated()) {
    return refl->FieldSize(*msg_, field) > 0;
  }
  // Singular field: proto2 uses explicit presence (HasField
  // returns true iff the bit is set); proto3 implicit-presence
  // scalars report HasField based on the default-value comparison.
  // Reflection's HasField handles both cases correctly.
  return refl->HasField(*msg_, field);
}

}  // namespace celwasm

// ══════════════════════════════════════════════════════════════════
// cel::Value::Message(const google::protobuf::Message&)
//
// Defined in this TU — not in value.cc — so value.cc doesn't need
// to know about ProtoBacking.  The dependency is one-way: cel_host
// depends on value; value never depends on cel_host (only forward-
// declares HostMessageBacking for the shared_ptr slot in its
// variant).
// ══════════════════════════════════════════════════════════════════

namespace cel {

Value Value::Message(const google::protobuf::Message& m) {
  return Value::HostMessage(std::make_shared<celwasm::ProtoBacking>(&m));
}

}  // namespace cel
