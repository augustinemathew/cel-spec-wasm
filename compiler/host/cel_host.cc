#include "compiler/host/cel_host.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "google/protobuf/util/message_differencer.h"

namespace celwasm {

namespace {

void WriteError(CelValue* absl_nonnull out) {
  out->kind = CEL_ERROR;
  out->payload.err = 0;
}

// Copies `src` (len bytes) into a fresh arena allocation and fills the
// span portion of `*out` with `{ptr = offset, len = src_len}`.  Returns
// false on allocator failure so the caller can surface CEL_ERROR instead
// of a half-populated value.
bool WriteSpanPayload(const char* src, size_t len, uint32_t kind,
                      CelValue* absl_nonnull out, ArenaAllocator& alloc) {
  uint32_t offset = 0;
  uint8_t* dst = alloc(len, &offset);
  if (dst == nullptr && len != 0) return false;
  if (len != 0) std::memcpy(dst, src, len);
  out->kind = kind;
  out->payload.s.ptr = offset;
  out->payload.s.len = static_cast<uint32_t>(len);
  return true;
}

// Writes numeric / bool / enum fields into `*out`.  Returns false if the
// field's cpp_type isn't one of the scalar-numeric kinds, leaving `*out`
// untouched so the caller can dispatch to string / message handling.
bool ReadNumericField(const google::protobuf::Reflection& refl,
                      const google::protobuf::Message& msg,
                      const google::protobuf::FieldDescriptor& field,
                      CelValue* absl_nonnull out) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      out->kind = CEL_BOOL;
      out->payload.b = refl.GetBool(msg, &field) ? 1 : 0;
      return true;
    case FD::CPPTYPE_INT32:
      out->kind = CEL_INT;
      out->payload.i = refl.GetInt32(msg, &field);
      return true;
    case FD::CPPTYPE_INT64:
      out->kind = CEL_INT;
      out->payload.i = refl.GetInt64(msg, &field);
      return true;
    case FD::CPPTYPE_UINT32:
      out->kind = CEL_UINT;
      out->payload.u = refl.GetUInt32(msg, &field);
      return true;
    case FD::CPPTYPE_UINT64:
      out->kind = CEL_UINT;
      out->payload.u = refl.GetUInt64(msg, &field);
      return true;
    case FD::CPPTYPE_FLOAT:
      out->kind = CEL_DOUBLE;
      out->payload.d = refl.GetFloat(msg, &field);
      return true;
    case FD::CPPTYPE_DOUBLE:
      out->kind = CEL_DOUBLE;
      out->payload.d = refl.GetDouble(msg, &field);
      return true;
    case FD::CPPTYPE_ENUM:
      // CEL treats proto enum values as ints, matching the spec's
      // "enum values are implicitly convertible to int" rule.
      out->kind = CEL_INT;
      out->payload.i = refl.GetEnumValue(msg, &field);
      return true;
    default:
      return false;
  }
}

void ReadScalarField(const google::protobuf::Reflection& refl,
                     const google::protobuf::Message& msg,
                     const google::protobuf::FieldDescriptor& field,
                     CelValue* absl_nonnull out, ArenaAllocator& alloc,
                     InternMessage& intern) {
  using FD = google::protobuf::FieldDescriptor;
  if (ReadNumericField(refl, msg, field, out)) return;
  if (field.cpp_type() == FD::CPPTYPE_STRING) {
    std::string scratch;
    const std::string& s = refl.GetStringReference(msg, &field, &scratch);
    const uint32_t kind =
        field.type() == FD::TYPE_BYTES ? CEL_BYTES : CEL_STRING;
    if (!WriteSpanPayload(s.data(), s.size(), kind, out, alloc)) {
      WriteError(out);
    }
    return;
  }
  if (field.cpp_type() == FD::CPPTYPE_MESSAGE) {
    const google::protobuf::Message& sub = refl.GetMessage(msg, &field);
    out->kind = CEL_MESSAGE;
    out->payload.msg_slot = intern(sub);
    return;
  }
  WriteError(out);
}

}  // namespace

void ReadField(const google::protobuf::Message& msg, int field_number,
               CelValue* absl_nonnull out, ArenaAllocator& alloc,
               InternMessage& intern) {
  const google::protobuf::Descriptor* descriptor = msg.GetDescriptor();
  const google::protobuf::FieldDescriptor* field =
      descriptor->FindFieldByNumber(field_number);
  if (field == nullptr) {
    WriteError(out);
    return;
  }
  if (field->is_repeated()) {
    // M3 doesn't lower list-valued selects — the checker should reject
    // the surrounding expression earlier.  Surface CEL_ERROR defensively
    // so a codegen bug can't silently observe an uninitialised CelValue.
    WriteError(out);
    return;
  }
  const google::protobuf::Reflection* refl = msg.GetReflection();
  ReadScalarField(*refl, msg, *field, out, alloc, intern);
}

bool HasField(const google::protobuf::Message& msg, int field_number) {
  const google::protobuf::Descriptor* descriptor = msg.GetDescriptor();
  const google::protobuf::FieldDescriptor* field =
      descriptor->FindFieldByNumber(field_number);
  if (field == nullptr) return false;
  if (field->is_repeated()) {
    return msg.GetReflection()->FieldSize(msg, field) > 0;
  }
  return msg.GetReflection()->HasField(msg, field);
}

bool MessageEq(const google::protobuf::Message& a,
               const google::protobuf::Message& b) {
  return google::protobuf::util::MessageDifferencer::Equals(a, b);
}

}  // namespace celwasm
