// `cel_host.cel_set_field` — proto-literal field writes.  Own TU
// (same cc_library as cel_host_message.cc): the per-cpp_type set /
// append / map-insert walkers and the write-side WKT pack helpers
// are one logical unit, and large enough to review in isolation.

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/functional/function_ref.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "eval/internal/cel_host_backing.h"
#include "eval/internal/cel_host_common.h"
#include "eval/internal/cel_host_error.h"
#include "eval/internal/cel_host_message.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "runtime/cel_data.h"

namespace celwasm {

// ══════════════════════════════════════════════════════════════════
// `cel_host.cel_set_field(msg_slot, field_ref_id, value_slot)`.
//
// Per-cpp_type dispatch on the resolved FieldDescriptor.  The
// OwnedProtoBacking wrapping the constructed message exposes a
// non-const `Message*` via `mutable_message()`; reflection's
// `Set...` family writes through that pointer.
//
// Repeated + map fields walk the source list/map (arena or host)
// and per element call `Reflection::Add...` for repeated, or build
// a fresh MapEntry submessage via `Reflection::AddMessage` for map.
// Per-cpp_type dispatch shares structure with the scalar path
// (singular `Set...` ↔ repeated `Add...`); element-of-message and
// map-value-of-message route through `CopyFrom` on a fresh
// reflection-allocated submessage.
// ══════════════════════════════════════════════════════════════════

namespace {

// Read a CelValue's scalar payload as int64 — used for INT32/INT64/ENUM
// dispatch.  Caller has verified `cv.kind == CEL_INT`.
int64_t ReadInt64(const CelValue& cv) {
  return cv.payload.i;
}
uint64_t ReadUInt64(const CelValue& cv) {
  return cv.payload.u;
}
double ReadDouble(const CelValue& cv) {
  return cv.payload.d;
}

// Range-check an int64 CEL value before narrowing to an int32 proto
// field.  cel-cpp surfaces an out-of-range field assignment as an
// eval error (struct_value_builder.cc "int64 to int32 overflow");
// we return OutOfRange so the caller propagates it rather than
// silently truncating.  Shared by every int32-narrowing write arm:
// the bare singular field, the Int32Value wrapper, repeated appends
// (arena + host list), and host-map entry keys/values.
absl::Status CheckInt32Range(int64_t v, absl::string_view field_name) {
  if (v < std::numeric_limits<int32_t>::min() ||
      v > std::numeric_limits<int32_t>::max()) {
    return absl::OutOfRangeError(absl::StrCat(
        "CelSetFieldImpl: field `", field_name, "` int32 range error: ", v));
  }
  return absl::OkStatus();
}

// Range-check a uint64 CEL value before narrowing to a uint32 proto
// field.  Same contract as `CheckInt32Range`.
absl::Status CheckUint32Range(uint64_t v, absl::string_view field_name) {
  if (v > std::numeric_limits<uint32_t>::max()) {
    return absl::OutOfRangeError(absl::StrCat(
        "CelSetFieldImpl: field `", field_name, "` uint32 range error: ", v));
  }
  return absl::OkStatus();
}

// Read a string/bytes payload via the MemoryView's Span reader.
// Caller has verified the value kind is CEL_STRING or CEL_BYTES.
std::string ReadSpanString(const CelValue& cv, const MemoryView& mem) {
  absl::string_view sv = mem.ReadSpan(cv.payload.s.ptr, cv.payload.s.len);
  return std::string(sv);
}

// Write `src` into `dst` (a CPPTYPE_MESSAGE slot the caller already
// resolved via `MutableMessage` or `AddMessage`).  Three shapes:
//   (1) dst descriptor == src descriptor    → CopyFrom.
//   (2) dst is google.protobuf.Any          → reflection-pack.
//   (3) other descriptor mismatch           → InvalidArgument (see
//                                             tail below).
// The reflection path (vs the typed `Any::PackFrom`) is required for
// portability across generated and dynamic descriptor pools.  Shared
// across every cpp_type-MESSAGE caller (singular set, repeated
// append, map-entry value).
absl::Status WriteMessageOrPack(google::protobuf::Message* dst,
                                const google::protobuf::Message& src) {
  using FD = google::protobuf::FieldDescriptor;
  const google::protobuf::Descriptor* dst_desc = dst->GetDescriptor();
  const google::protobuf::Descriptor* src_desc = src.GetDescriptor();
  if (src_desc == dst_desc) {
    dst->CopyFrom(src);
    return absl::OkStatus();
  }
  if (dst_desc->full_name() == "google.protobuf.Any") {
    const google::protobuf::Reflection* refl = dst->GetReflection();
    const google::protobuf::FieldDescriptor* type_url_fd =
        dst_desc->FindFieldByName("type_url");
    const google::protobuf::FieldDescriptor* value_fd =
        dst_desc->FindFieldByName("value");
    if (refl == nullptr || type_url_fd == nullptr ||
        type_url_fd->cpp_type() != FD::CPPTYPE_STRING || value_fd == nullptr ||
        value_fd->cpp_type() != FD::CPPTYPE_STRING) {
      return absl::InternalError(
          "WriteMessageOrPack: Any descriptor missing type_url/value fields");
    }
    refl->SetString(
        dst, type_url_fd,
        absl::StrCat("type.googleapis.com/", src_desc->full_name()));
    refl->SetString(dst, value_fd, src.SerializeAsString());
    return absl::OkStatus();
  }
  // The remaining descriptor-mismatch path (dst is some non-Any
  // non-same-descriptor target) is reachable only if codegen /
  // Activation handed us a CEL_MESSAGE with the wrong descriptor.
  // Wrapper tail-unwrap and `SetWrapperFieldFromScalar` both route
  // scalar values through their own paths before reaching here;
  // only proper-message-vs-mismatched-message lands here.  Surface
  // as Invalid rather than CHECK — embedder error, not codegen bug.
  return absl::InvalidArgumentError(
      absl::StrCat("WriteMessageOrPack: dst `", dst_desc->full_name(),
                   "` ≠ src `", src_desc->full_name(),
                   "` — descriptor mismatch on singular-message "
                   "field write"));
}

// Build the `SetWrapperInnerValue` kind-mismatch error for a wrapper
// whose inner `value` field rejects the supplied CelValue kind.
absl::Status WrapperInnerMismatch(
    const google::protobuf::Descriptor& wrapper_desc,
    absl::string_view expected, const CelValue& value) {
  return absl::InvalidArgumentError(absl::StrCat(
      "SetWrapperInnerValue: `", wrapper_desc.full_name(), "` expects ",
      expected, " but value kind is ", static_cast<int>(value.kind)));
}

// Integer / bool arms of `SetWrapperInnerValue` (CPPTYPE_BOOL ..
// CPPTYPE_UINT64).  Returns the set Status for a handled cpp_type, or
// `std::nullopt` for FLOAT / DOUBLE / STRING (the caller handles
// those).  INT32 / UINT32 range-check before narrowing.  Single call
// site → inlines back.
std::optional<absl::Status> SetWrapperIntegerValue(
    const google::protobuf::Reflection& wr, google::protobuf::Message& wrapper,
    const google::protobuf::FieldDescriptor& vf,
    const google::protobuf::Descriptor& wrapper_desc, const CelValue& value) {
  using FD = google::protobuf::FieldDescriptor;
  switch (vf.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      if (value.kind != CEL_BOOL) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_BOOL", value);
      }
      wr.SetBool(&wrapper, &vf, value.payload.b != 0);
      return absl::OkStatus();
    case FD::CPPTYPE_INT32:
      if (value.kind != CEL_INT) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_INT", value);
      }
      if (auto s = CheckInt32Range(ReadInt64(value), wrapper_desc.full_name());
          !s.ok()) {
        return s;
      }
      wr.SetInt32(&wrapper, &vf, static_cast<int32_t>(ReadInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_INT64:
      if (value.kind != CEL_INT) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_INT", value);
      }
      wr.SetInt64(&wrapper, &vf, ReadInt64(value));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT32:
      if (value.kind != CEL_UINT) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_UINT", value);
      }
      if (auto s =
              CheckUint32Range(ReadUInt64(value), wrapper_desc.full_name());
          !s.ok()) {
        return s;
      }
      wr.SetUInt32(&wrapper, &vf, static_cast<uint32_t>(ReadUInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT64:
      if (value.kind != CEL_UINT) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_UINT", value);
      }
      wr.SetUInt64(&wrapper, &vf, ReadUInt64(value));
      return absl::OkStatus();
    default:
      return std::nullopt;
  }
}

// Write the inner `value` field of a freshly-allocated wrapper
// message from a matching scalar CelValue.  9-way cpp_type dispatch,
// mirror of the read-side `UnpackWrapperMessage`.  Extracted from
// `SetWrapperFieldFromScalar` to keep the parent under the
// readability-function-size gate.
absl::Status SetWrapperInnerValue(
    const google::protobuf::Reflection& wr, google::protobuf::Message& wrapper,
    const google::protobuf::FieldDescriptor& vf,
    const google::protobuf::Descriptor& wrapper_desc, const CelValue& value,
    const MemoryView& mem) {
  using FD = google::protobuf::FieldDescriptor;
  if (auto s = SetWrapperIntegerValue(wr, wrapper, vf, wrapper_desc, value);
      s.has_value()) {
    return *std::move(s);
  }
  switch (vf.cpp_type()) {
    case FD::CPPTYPE_FLOAT:
      if (value.kind != CEL_DOUBLE) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_DOUBLE", value);
      }
      wr.SetFloat(&wrapper, &vf, static_cast<float>(ReadDouble(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_DOUBLE:
      if (value.kind != CEL_DOUBLE) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_DOUBLE", value);
      }
      wr.SetDouble(&wrapper, &vf, ReadDouble(value));
      return absl::OkStatus();
    case FD::CPPTYPE_STRING:
      if (vf.type() == FD::TYPE_BYTES) {
        if (value.kind != CEL_BYTES) {
          return WrapperInnerMismatch(wrapper_desc, "CEL_BYTES", value);
        }
      } else if (value.kind != CEL_STRING) {
        return WrapperInnerMismatch(wrapper_desc, "CEL_STRING", value);
      }
      wr.SetString(&wrapper, &vf, ReadSpanString(value, mem));
      return absl::OkStatus();
    default:
      ABSL_CHECK(false)
          << "SetWrapperInnerValue: WKT-wrapper FQN claims unexpected inner "
             "cpp_type "
          << static_cast<int>(vf.cpp_type());
      return absl::InternalError("unreachable");
  }
}

// Synthesise a wrapper-message proto from a matching scalar
// CelValue and assign it to a wrapper-typed singular-message field
// on the outer message.  Called by `SetScalarField`'s CPPTYPE_MESSAGE
// arm when the field's `message_type()` FQN is one of the 9 wrapper
// FQNs.  Mirror of the read-side `UnpackWrapperMessage` shape.
absl::Status SetWrapperFieldFromScalar(
    const google::protobuf::Reflection& outer_refl,
    google::protobuf::Message& outer,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Descriptor& wrapper_desc, const CelValue& value,
    const MemoryView& mem) {
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(
          &wrapper_desc);
  if (prototype == nullptr) {
    return absl::InternalError(absl::StrCat(
        "SetWrapperFieldFromScalar: no generated_factory prototype for `",
        wrapper_desc.full_name(), "`"));
  }
  std::unique_ptr<google::protobuf::Message> wrapper(prototype->New());
  const google::protobuf::Reflection* wr = wrapper->GetReflection();
  const google::protobuf::FieldDescriptor* vf =
      wrapper_desc.FindFieldByNumber(1);
  if (wr == nullptr || vf == nullptr) {
    return absl::InternalError(
        absl::StrCat("SetWrapperFieldFromScalar: `", wrapper_desc.full_name(),
                     "` missing reflection or value-field descriptor"));
  }
  if (auto s =
          SetWrapperInnerValue(*wr, *wrapper, *vf, wrapper_desc, value, mem);
      !s.ok()) {
    return s;
  }
  outer_refl.MutableMessage(&outer, &field)->CopyFrom(*wrapper);
  return absl::OkStatus();
}

// Forward declarations for the WKT pack dispatch invoked from
// `SetScalarField`'s CPPTYPE_MESSAGE arm and the arena-walker
// helpers the JSON-Value packer recurses through (both defined
// below, after the repeated/map walkers).
std::optional<absl::Status> MaybeSetWktMessageField(
    google::protobuf::Message& outer,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs);
std::optional<absl::Status> MaybePackWktMessage(
    google::protobuf::Message& target, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs);
// Forward-declared so the Struct / ListValue packers can recurse into
// it before its definition; the body lives below those helpers.
// NOLINTNEXTLINE(readability-redundant-declaration)
absl::Status PackCelValueIntoJsonValue(
    const CelValue& value, google::protobuf::Message& out,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs);

// Resolve a CEL_MESSAGE source through `refs` and copy/pack it into a
// freshly-mutable submessage of `field` on `msg`.  Tail of
// `SetSingularMessageField` (reached once the null / wrapper / WKT
// gates ahead of it miss); split out so the parent stays under the
// function-size gate.  Single call site → inlines back.
absl::Status SetNestedSingularMessage(
    const google::protobuf::Reflection& refl, google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const ExternrefTable* absl_nullable refs) {
  if (value.kind != CEL_MESSAGE) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CelSetFieldImpl: field `", field.name(),
        "` is MESSAGE but value kind is ", static_cast<int>(value.kind)));
  }
  if (refs == nullptr) {
    return absl::InternalError(
        absl::StrCat("CelSetFieldImpl: field `", field.name(),
                     "` is MESSAGE but no ExternrefTable supplied "
                     "(nested-message call-site bug)"));
  }
  const HostMessageBacking* src = refs->Lookup(value.payload.msg_slot);
  if (src == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: field `", field.name(),
                     "` source has no externref entry"));
  }
  const google::protobuf::Message* src_msg = src->message();
  if (src_msg == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: field `", field.name(),
                     "` source backing has no proto message"));
  }
  // Descriptor-aware dst-write: descriptors match → CopyFrom;
  // dst is google.protobuf.Any → reflection-pack; other mismatch
  // → InvalidArgument (wrapper auto-wrap is gated above).
  google::protobuf::Message* dst = refl.MutableMessage(&msg, &field);
  return WriteMessageOrPack(dst, *src_msg);
}

// CPPTYPE_MESSAGE arm of `SetScalarField` — singular message-field
// assignment.  Handles, in spec order: `null` clears the field (or
// packs an explicit `null_value` for a `google.protobuf.Value`
// target); a scalar source onto a wrapper-typed field auto-wraps; a
// scalar/aggregate onto a WKT message field (Duration / Timestamp /
// Value / Struct / ListValue / Any) packs through the shared WKT
// dispatch; otherwise the source must be a CEL_MESSAGE resolved
// through `refs` and copied/packed via `WriteMessageOrPack`.  Split
// from `SetScalarField` so the cpp_type switch stays under the
// function-size gate; single call site → inlines back.
absl::Status SetSingularMessageField(
    const google::protobuf::Reflection& refl, google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs) {
  // langdef §"Field Selection" + cel-cpp behaviour: assigning
  // `null` to a singular message field clears it (equivalent
  // to leaving it unset).  `Foo{m: null} == Foo{}` per the
  // conformance corpus's `set_null/*` rows.  For wrapper-typed
  // fields, langdef line 484-486's unset-reads-as-null rule
  // makes this round-trip with the read-side wrapper peel.
  // A `null` assigned to a `google.protobuf.Value` field packs as
  // an explicit `null_value` (NOT a cleared field) — langdef JSON
  // semantics, `value_null/field_assign_*` corpus rows.  Every
  // other message type clears on null.
  if (value.kind == CEL_NULL) {
    const google::protobuf::Descriptor* nmt = field.message_type();
    if (nmt != nullptr && nmt->full_name() == "google.protobuf.Value") {
      return PackCelValueIntoJsonValue(
          value, *refl.MutableMessage(&msg, &field), mem, refs);
    }
    refl.ClearField(&msg, &field);
    return absl::OkStatus();
  }
  // Wrapper-typed field with scalar source — synthesise the
  // wrapper proto and assign.  `Foo{single_int32_wrapper: 5}`
  // sees scalar CEL_INT here because typed_ast.cc:56 stamps the
  // value as `Int32` Repr; the auto-wrap below is the boundary
  // where the scalar becomes an `Int32Value{value: 5}` proto.
  const google::protobuf::Descriptor* mt = field.message_type();
  if (mt != nullptr && IsWrapperFqn(mt->full_name()) &&
      value.kind != CEL_MESSAGE) {
    return SetWrapperFieldFromScalar(refl, msg, field, *mt, value, mem);
  }
  // Duration / Timestamp / Value / Struct / ListValue / Any from
  // a non-message CelValue — pack the scalar/aggregate into the
  // matching well-known message.  Mirror of the read-side
  // `MaybeUnpackWktMessage` / `UnpackJsonValueMessage` peelers.
  if (mt != nullptr && value.kind != CEL_MESSAGE) {
    if (auto s = MaybeSetWktMessageField(msg, field, value, mem, refs);
        s.has_value()) {
      return *std::move(s);
    }
  }
  // Nested singular message — `Foo{nested: Bar{...}}`.  The
  // outer kStructExpr lowering recursively built `Bar{...}` into
  // a fresh OwnedProtoBacking and wrote a CEL_MESSAGE CelValue
  // at value_slot.  Resolve that backing and CopyFrom into a
  // freshly-mutable submessage of the outer field — same pattern
  // as repeated-of-message in `AppendRepeatedFromCelValue`.
  return SetNestedSingularMessage(refl, msg, field, value, refs);
}

// Build the singular-scalar kind-mismatch error (a checker regression
// surfaced as InvalidArgument).  Shared across the scalar set arms.
absl::Status ScalarKindMismatch(const google::protobuf::FieldDescriptor& field,
                                absl::string_view ty, const CelValue& value) {
  return absl::InvalidArgumentError(
      absl::StrCat("CelSetFieldImpl: field `", field.name(), "` is ", ty,
                   " but value kind is ", static_cast<int>(value.kind)));
}

// Integer / bool scalar arms of `SetScalarField` (CPPTYPE_BOOL ..
// CPPTYPE_UINT64).  Returns the set Status for a handled cpp_type, or
// `std::nullopt` for FLOAT / DOUBLE / STRING / ENUM / MESSAGE.  INT32 /
// UINT32 range-check before narrowing.  Single call site → inlines
// back.
std::optional<absl::Status> SetScalarIntegerField(
    const google::protobuf::Reflection& refl, google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      if (value.kind != CEL_BOOL) {
        return ScalarKindMismatch(field, "BOOL", value);
      }
      refl.SetBool(&msg, &field, value.payload.b != 0);
      return absl::OkStatus();
    case FD::CPPTYPE_INT32:
      if (value.kind != CEL_INT) {
        return ScalarKindMismatch(field, "INT32", value);
      }
      if (auto s = CheckInt32Range(ReadInt64(value), field.name()); !s.ok()) {
        return s;
      }
      refl.SetInt32(&msg, &field, static_cast<int32_t>(ReadInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_INT64:
      if (value.kind != CEL_INT) {
        return ScalarKindMismatch(field, "INT64", value);
      }
      refl.SetInt64(&msg, &field, ReadInt64(value));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT32:
      if (value.kind != CEL_UINT) {
        return ScalarKindMismatch(field, "UINT32", value);
      }
      if (auto s = CheckUint32Range(ReadUInt64(value), field.name()); !s.ok()) {
        return s;
      }
      refl.SetUInt32(&msg, &field, static_cast<uint32_t>(ReadUInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT64:
      if (value.kind != CEL_UINT) {
        return ScalarKindMismatch(field, "UINT64", value);
      }
      refl.SetUInt64(&msg, &field, ReadUInt64(value));
      return absl::OkStatus();
    default:
      return std::nullopt;
  }
}

// FLOAT / DOUBLE scalar arms of `SetScalarField`.  Both accept
// CEL_DOUBLE (CEL has one floating type); FLOAT narrows.  Single call
// site → inlines back.
std::optional<absl::Status> SetScalarFloatField(
    const google::protobuf::Reflection& refl, google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_FLOAT:
      if (value.kind != CEL_DOUBLE) {
        return ScalarKindMismatch(field, "FLOAT", value);
      }
      refl.SetFloat(&msg, &field, static_cast<float>(ReadDouble(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_DOUBLE:
      if (value.kind != CEL_DOUBLE) {
        return ScalarKindMismatch(field, "DOUBLE", value);
      }
      refl.SetDouble(&msg, &field, ReadDouble(value));
      return absl::OkStatus();
    default:
      return std::nullopt;
  }
}

// STRING / BYTES and ENUM scalar arms of `SetScalarField`.  String
// fields accept CEL_STRING, bytes fields (same CPPTYPE_STRING slot,
// distinguished by `field.type()`) accept CEL_BYTES; enum values flow
// as CEL_INT (langdef §"Enumerated Types") and range-check into int32
// before assignment.  Single call site → inlines back.
absl::Status SetStringOrEnumField(
    const google::protobuf::Reflection& refl, google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const MemoryView& mem) {
  using FD = google::protobuf::FieldDescriptor;
  if (field.cpp_type() == FD::CPPTYPE_STRING) {
    if (field.type() == FD::TYPE_BYTES) {
      if (value.kind != CEL_BYTES) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: field `", field.name(),
            "` is BYTES but value kind is ", static_cast<int>(value.kind)));
      }
    } else if (value.kind != CEL_STRING) {
      return absl::InvalidArgumentError(absl::StrCat(
          "CelSetFieldImpl: field `", field.name(),
          "` is STRING but value kind is ", static_cast<int>(value.kind)));
    }
    refl.SetString(&msg, &field, ReadSpanString(value, mem));
    return absl::OkStatus();
  }
  // CPPTYPE_ENUM: cel-cpp's checker resolves `Foo.SOME_VALUE` to a
  // Constant int64; codegen flows that as CEL_INT here.
  if (value.kind != CEL_INT) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CelSetFieldImpl: field `", field.name(),
        "` is ENUM but value kind is ", static_cast<int>(value.kind)));
  }
  // Enum values narrow to int32 on the wire; an out-of-range
  // assignment is a CEL error in cel-cpp (struct_value_builder.cc
  // `CPPTYPE_ENUM` returns TypeConversionError), surfaced here via
  // the poison contract rather than a silent truncation.
  if (auto s = CheckInt32Range(ReadInt64(value), field.name()); !s.ok()) {
    return s;
  }
  refl.SetEnumValue(&msg, &field, static_cast<int>(ReadInt64(value)));
  return absl::OkStatus();
}

// Set a scalar singular field on `msg` per `field`'s cpp_type.  Returns
// non-OK Status on cpp_type / value-kind mismatches that the cel-cpp
// checker should have rejected pre-codegen — surfaces as a wasm trap so
// a checker regression fails the row loudly.  Repeated and map fields
// are NOT routed here; the caller checks `is_map() || is_repeated()`
// and returns Unimplemented before reaching this dispatch.
absl::Status SetScalarField(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs = nullptr) {
  using FD = google::protobuf::FieldDescriptor;
  const google::protobuf::Reflection* refl = msg.GetReflection();
  if (refl == nullptr) {
    return absl::InternalError("CelSetFieldImpl: message has no reflection");
  }
  if (auto s = SetScalarIntegerField(*refl, msg, field, value); s.has_value()) {
    return *std::move(s);
  }
  if (auto s = SetScalarFloatField(*refl, msg, field, value); s.has_value()) {
    return *std::move(s);
  }
  switch (field.cpp_type()) {
    case FD::CPPTYPE_STRING:
    case FD::CPPTYPE_ENUM:
      return SetStringOrEnumField(*refl, msg, field, value, mem);
    case FD::CPPTYPE_MESSAGE:
      return SetSingularMessageField(*refl, msg, field, value, mem, refs);
    default:
      break;
  }
  ABSL_CHECK(false) << "CelSetFieldImpl: unknown cpp_type "
                    << static_cast<int>(field.cpp_type());
  return absl::InternalError("unreachable");
}

// ──── Repeated + map field set helpers ──────────────────────────

// Walk an arena-list CelValue's elements, calling `visit(elem, i)`
// per element (read via the MemoryView).  Caller has verified
// `cv.kind == CEL_LIST_ARENA`.
void ForEachArenaListElement(
    const CelValue& cv, const MemoryView& mem,
    absl::FunctionRef<void(const CelValue&, uint32_t)> visit) {
  ArenaListHeader hdr{};
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_list.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  for (uint32_t i = 0; i < hdr.count; ++i) {
    CelValue elem = mem.ReadCelValue(
        hdr.elements_offset + (i * static_cast<uint32_t>(kCelListEntryStride)));
    visit(elem, i);
  }
}

// Walk an arena-map CelValue's entries, calling `visit(key, val, i)`
// per entry.  Caller has verified `cv.kind == CEL_MAP_ARENA`.
// Each entry is a 48-byte (key, val) pair at
// `entries_offset + i * kCelMapEntryStride`.
void ForEachArenaMapEntry(
    const CelValue& cv, const MemoryView& mem,
    absl::FunctionRef<void(const CelValue&, const CelValue&, uint32_t)> visit) {
  ArenaMapHeader hdr{};
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_map.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  for (uint32_t i = 0; i < hdr.count; ++i) {
    const uint32_t entry_off =
        hdr.entries_offset + (i * static_cast<uint32_t>(kCelMapEntryStride));
    CelValue k = mem.ReadCelValue(entry_off);
    CelValue v = mem.ReadCelValue(entry_off +
                                  static_cast<uint32_t>(kCelListEntryStride));
    visit(k, v, i);
  }
}

// ──── Well-known-type pack helpers (write side) ─────────────────
//
// Inverse of the read-side `MaybeUnpackWktMessage` /
// `UnpackJsonValueMessage` peelers: synthesise a Duration /
// Timestamp / Value / Struct / ListValue / Any message from a
// CelValue and assign it to a singular-message field.

// Allocate a fresh prototype instance of the WKT message `desc`
// describes.  Returns null on a missing generated_factory prototype
// (an InternalError the caller surfaces).
std::unique_ptr<google::protobuf::Message> NewWktMessage(
    const google::protobuf::Descriptor& desc) {
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(
          &desc);
  if (prototype == nullptr) return nullptr;
  return std::unique_ptr<google::protobuf::Message>(prototype->New());
}

// Set the (seconds, nanos) pair on a freshly-built Duration /
// Timestamp message from a CEL_DURATION / CEL_TIMESTAMP payload.
// `dur_ts` is `value.payload.dur` or `.ts` (both `CelDurTs`).
absl::Status PackDurationOrTimestamp(google::protobuf::Message& wkt,
                                     const CelDurTs& dur_ts) {
  const google::protobuf::Descriptor* d = wkt.GetDescriptor();
  const google::protobuf::Reflection* refl = wkt.GetReflection();
  const google::protobuf::FieldDescriptor* sf =
      d != nullptr ? d->FindFieldByNumber(1) : nullptr;
  const google::protobuf::FieldDescriptor* nf =
      d != nullptr ? d->FindFieldByNumber(2) : nullptr;
  if (refl == nullptr || sf == nullptr || nf == nullptr) {
    return absl::InternalError(
        "PackDurationOrTimestamp: WKT time message missing seconds/nanos");
  }
  refl->SetInt64(&wkt, sf, dur_ts.seconds);
  refl->SetInt32(&wkt, nf, dur_ts.nanos);
  return absl::OkStatus();
}

// `PackCelValueIntoJsonValue` (the recursive CelValue →
// google.protobuf.Value packer) is forward-declared with the other
// WKT-pack helpers above; its definition follows below.

// Populate a `google.protobuf.Struct`'s `fields` map from a CEL map
// CelValue.  Keys must be CEL_STRING (JSON object keys); each value
// is recursively packed into a `google.protobuf.Value`.
absl::Status PackStruct(const CelValue& map_cv,
                        google::protobuf::Message& strct, const MemoryView& mem,
                        const ExternrefTable* absl_nullable refs) {
  const google::protobuf::Descriptor* d = strct.GetDescriptor();
  const google::protobuf::Reflection* refl = strct.GetReflection();
  const google::protobuf::FieldDescriptor* fields_fd =
      d != nullptr ? d->FindFieldByNumber(1) : nullptr;
  if (refl == nullptr || fields_fd == nullptr || !fields_fd->is_map()) {
    return absl::InternalError("PackStruct: Struct missing `fields` map");
  }
  const google::protobuf::FieldDescriptor* key_fd =
      fields_fd->message_type()->FindFieldByNumber(1);
  const google::protobuf::FieldDescriptor* val_fd =
      fields_fd->message_type()->FindFieldByNumber(2);
  absl::Status status = absl::OkStatus();
  auto add_entry = [&](const CelValue& k, const CelValue& v) {
    if (!status.ok()) return;
    if (k.kind != CEL_STRING) {
      status = absl::InvalidArgumentError(
          "PackStruct: google.protobuf.Struct key is not a string");
      return;
    }
    google::protobuf::Message* entry = refl->AddMessage(&strct, fields_fd);
    const google::protobuf::Reflection* er = entry->GetReflection();
    er->SetString(entry, key_fd, ReadSpanString(k, mem));
    status = PackCelValueIntoJsonValue(v, *er->MutableMessage(entry, val_fd),
                                       mem, refs);
  };
  if (map_cv.kind == CEL_MAP_ARENA) {
    ForEachArenaMapEntry(
        map_cv, mem, [&](const CelValue& k, const CelValue& v, uint32_t /*i*/) {
          add_entry(k, v);
        });
    return status;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("PackStruct: source kind=", static_cast<int>(map_cv.kind),
                   " (expected CEL_MAP_ARENA)"));
}

// Populate a `google.protobuf.ListValue`'s `values` from a CEL list
// CelValue.  Each element is recursively packed into a
// `google.protobuf.Value`.
absl::Status PackListValue(const CelValue& list_cv,
                           google::protobuf::Message& lv, const MemoryView& mem,
                           const ExternrefTable* absl_nullable refs) {
  const google::protobuf::Descriptor* d = lv.GetDescriptor();
  const google::protobuf::Reflection* refl = lv.GetReflection();
  const google::protobuf::FieldDescriptor* values_fd =
      d != nullptr ? d->FindFieldByNumber(1) : nullptr;
  if (refl == nullptr || values_fd == nullptr || !values_fd->is_repeated()) {
    return absl::InternalError("PackListValue: ListValue missing `values`");
  }
  absl::Status status = absl::OkStatus();
  if (list_cv.kind == CEL_LIST_ARENA) {
    ForEachArenaListElement(
        list_cv, mem, [&](const CelValue& elem, uint32_t /*i*/) {
          if (!status.ok()) return;
          status = PackCelValueIntoJsonValue(
              elem, *refl->AddMessage(&lv, values_fd), mem, refs);
        });
    return status;
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "PackListValue: source kind=", static_cast<int>(list_cv.kind),
      " (expected CEL_LIST_ARENA)"));
}

absl::Status PackCelValueIntoJsonValue(
    const CelValue& value, google::protobuf::Message& out,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs) {
  const google::protobuf::Descriptor* d = out.GetDescriptor();
  const google::protobuf::Reflection* refl = out.GetReflection();
  // kind_case field numbers in google.protobuf.Value: 1 null_value,
  // 2 number_value, 3 string_value, 4 bool_value, 5 struct_value,
  // 6 list_value.
  auto field = [&](int n) {
    return d->FindFieldByNumber(n);
  };
  switch (value.kind) {
    case CEL_NULL:
      refl->SetEnumValue(&out, field(1), 0);  // NULL_VALUE
      return absl::OkStatus();
    case CEL_BOOL:
      refl->SetBool(&out, field(4), value.payload.b != 0);
      return absl::OkStatus();
    case CEL_INT:
      refl->SetDouble(&out, field(2), static_cast<double>(value.payload.i));
      return absl::OkStatus();
    case CEL_UINT:
      refl->SetDouble(&out, field(2), static_cast<double>(value.payload.u));
      return absl::OkStatus();
    case CEL_DOUBLE:
      refl->SetDouble(&out, field(2), value.payload.d);
      return absl::OkStatus();
    case CEL_STRING:
      refl->SetString(&out, field(3), ReadSpanString(value, mem));
      return absl::OkStatus();
    case CEL_LIST_ARENA:
      return PackListValue(value, *refl->MutableMessage(&out, field(6)), mem,
                           refs);
    case CEL_MAP_ARENA:
      return PackStruct(value, *refl->MutableMessage(&out, field(5)), mem,
                        refs);
    default:
      return absl::InvalidArgumentError(absl::StrCat(
          "PackCelValueIntoJsonValue: unsupported value kind ",
          static_cast<int>(value.kind), " for google.protobuf.Value"));
  }
}

// Pick the well-known inner FQN a CelValue is packed into before
// being wrapped in an Any.  Mirrors cel-cpp's to-Any conversion:
//
//   - int / uint / bytes → the matching wrapper message, so the value
//     round-trips as its CEL kind through the read-side wrapper peel
//     (a bare JSON Value would lose int/uint to double, and JSON has
//     no bytes type).
//   - list → google.protobuf.ListValue, map → google.protobuf.Struct
//     packed DIRECTLY (not nested inside a Value) — the corpus's
//     `complex/any_list_map` row pins `type_url=.../ListValue`.
//   - everything else (bool / double / string / null) → a bare
//     google.protobuf.Value.
absl::string_view AnyInnerFqn(uint32_t kind) {
  switch (kind) {
    case CEL_INT:
      return "google.protobuf.Int64Value";
    case CEL_UINT:
      return "google.protobuf.UInt64Value";
    case CEL_BYTES:
      return "google.protobuf.BytesValue";
    case CEL_LIST_ARENA:
    case CEL_LIST_HOST:
      return "google.protobuf.ListValue";
    case CEL_MAP_ARENA:
    case CEL_MAP_HOST:
      return "google.protobuf.Struct";
    default:
      return "google.protobuf.Value";
  }
}

// True for the inner FQNs that are filled by their dedicated packer
// rather than `SetWrapperInnerValue`'s value-field set.
bool IsAggregateInnerFqn(absl::string_view fqn) {
  return fqn == "google.protobuf.ListValue" ||
         fqn == "google.protobuf.Struct" || fqn == "google.protobuf.Value";
}

// Pack a non-message CelValue into the already-allocated `Any`
// message `any`.  int / uint / bytes wrap into the matching wrapper
// type (so the value round-trips as its CEL kind through the
// read-side wrapper peel); everything else wraps into a
// `google.protobuf.Value`.  Matches cel-cpp's to-Any conversion.
absl::Status PackAnyFromScalar(google::protobuf::Message& any,
                               const CelValue& value, const MemoryView& mem,
                               const ExternrefTable* absl_nullable refs) {
  const google::protobuf::DescriptorPool* pool =
      any.GetDescriptor()->file()->pool();
  const absl::string_view inner_fqn = AnyInnerFqn(value.kind);
  const google::protobuf::Descriptor* inner_desc =
      pool->FindMessageTypeByName(std::string(inner_fqn));
  if (inner_desc == nullptr) {
    return absl::InternalError(absl::StrCat("PackAnyFromScalar: ", inner_fqn,
                                            " not in descriptor pool"));
  }
  std::unique_ptr<google::protobuf::Message> inner = NewWktMessage(*inner_desc);
  if (inner == nullptr) {
    return absl::InternalError(
        absl::StrCat("PackAnyFromScalar: no prototype for ", inner_fqn));
  }
  if (IsAggregateInnerFqn(inner_fqn)) {
    // List → ListValue, map → Struct, scalar JSON kind → Value: route
    // through the shared WKT packer for the chosen inner type.
    if (auto s = MaybePackWktMessage(*inner, value, mem, refs);
        s.has_value() && !s->ok()) {
      return *std::move(s);
    }
  } else {
    // Wrapper: set the inner `value` field (number 1) from the scalar.
    const google::protobuf::Reflection* wr = inner->GetReflection();
    const google::protobuf::FieldDescriptor* vf =
        inner_desc->FindFieldByNumber(1);
    if (wr == nullptr || vf == nullptr) {
      return absl::InternalError(
          "PackAnyFromScalar: wrapper missing value field");
    }
    if (auto s =
            SetWrapperInnerValue(*wr, *inner, *vf, *inner_desc, value, mem);
        !s.ok()) {
      return s;
    }
  }
  return WriteMessageOrPack(&any, *inner);
}

// Dispatch on `target`'s descriptor FQN to pack a non-message
// CelValue into an already-allocated WKT message `target`.  Returns
// `nullopt` if `target` is not a packable WKT — the caller then
// falls through to its mismatch error.  Shared by the singular,
// repeated, and map message-set paths; each resolves `target` from
// the appropriate reflection mutator (MutableMessage / AddMessage).
std::optional<absl::Status> MaybePackWktMessage(
    google::protobuf::Message& target, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs) {
  const absl::string_view fqn = target.GetDescriptor()->full_name();
  const bool is_duration = fqn == "google.protobuf.Duration";
  const bool is_timestamp = fqn == "google.protobuf.Timestamp";
  if (is_duration || is_timestamp) {
    if ((is_duration && value.kind != CEL_DURATION) ||
        (is_timestamp && value.kind != CEL_TIMESTAMP)) {
      return std::nullopt;
    }
    return PackDurationOrTimestamp(
        target, is_duration ? value.payload.dur : value.payload.ts);
  }
  if (fqn == "google.protobuf.Value") {
    return PackCelValueIntoJsonValue(value, target, mem, refs);
  }
  if (fqn == "google.protobuf.Struct") {
    return PackStruct(value, target, mem, refs);
  }
  if (fqn == "google.protobuf.ListValue") {
    return PackListValue(value, target, mem, refs);
  }
  if (fqn == "google.protobuf.Any") {
    return PackAnyFromScalar(target, value, mem, refs);
  }
  return std::nullopt;
}

// Singular-field WKT pack: resolve a mutable submessage on the outer
// field and delegate to `MaybePackWktMessage`.  `nullopt` propagates
// (the field's message type isn't a packable WKT) — but only after
// confirming via FQN so we don't mutate the field for a non-match.
std::optional<absl::Status> MaybeSetWktMessageField(
    google::protobuf::Message& outer,
    const google::protobuf::FieldDescriptor& field, const CelValue& value,
    const MemoryView& mem, const ExternrefTable* absl_nullable refs) {
  const google::protobuf::Descriptor* mt = field.message_type();
  const absl::string_view fqn = mt != nullptr ? mt->full_name() : "";
  if (fqn != "google.protobuf.Duration" && fqn != "google.protobuf.Timestamp" &&
      fqn != "google.protobuf.Value" && fqn != "google.protobuf.Struct" &&
      fqn != "google.protobuf.ListValue" && fqn != "google.protobuf.Any") {
    return std::nullopt;
  }
  google::protobuf::Message* sub =
      outer.GetReflection()->MutableMessage(&outer, &field);
  return MaybePackWktMessage(*sub, value, mem, refs);
}

// Repeated-field append mismatch error, shared by the numeric/bool/
// string/enum arms below so they emit an identical message.
absl::Status RepeatedAppendMismatch(
    const google::protobuf::FieldDescriptor& field, absl::string_view ty,
    const CelValue& cv) {
  return absl::InvalidArgumentError(
      absl::StrCat("CelSetFieldImpl: repeated `", field.name(), "` ", ty,
                   " element kind=", static_cast<int>(cv.kind)));
}

// INT32/INT64/UINT32/UINT64 arms of AppendRepeatedNumeric.  The
// 32-bit widths range-check before narrowing (same contract as the
// singular SetScalarIntegerField).  Returns nullopt for a
// non-integer cpp_type.
std::optional<absl::Status> AppendRepeatedInteger(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const CelValue& cv) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_INT32:
      if (cv.kind != CEL_INT) return RepeatedAppendMismatch(field, "INT32", cv);
      if (auto s = CheckInt32Range(cv.payload.i, field.name()); !s.ok()) {
        return s;
      }
      refl.AddInt32(&msg, &field, static_cast<int32_t>(cv.payload.i));
      return absl::OkStatus();
    case FD::CPPTYPE_INT64:
      if (cv.kind != CEL_INT) return RepeatedAppendMismatch(field, "INT64", cv);
      refl.AddInt64(&msg, &field, cv.payload.i);
      return absl::OkStatus();
    case FD::CPPTYPE_UINT32:
      if (cv.kind != CEL_UINT) {
        return RepeatedAppendMismatch(field, "UINT32", cv);
      }
      if (auto s = CheckUint32Range(cv.payload.u, field.name()); !s.ok()) {
        return s;
      }
      refl.AddUInt32(&msg, &field, static_cast<uint32_t>(cv.payload.u));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT64:
      if (cv.kind != CEL_UINT) {
        return RepeatedAppendMismatch(field, "UINT64", cv);
      }
      refl.AddUInt64(&msg, &field, cv.payload.u);
      return absl::OkStatus();
    default:
      return std::nullopt;
  }
}

// INT32/INT64/UINT32/UINT64/FLOAT/DOUBLE arms of AppendRepeatedScalar.
// Returns nullopt for a non-numeric cpp_type (caller handles
// BOOL/STRING/ENUM/MESSAGE).  Uses the `Add...` reflection family.
std::optional<absl::Status> AppendRepeatedNumeric(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const CelValue& cv) {
  using FD = google::protobuf::FieldDescriptor;
  if (auto s = AppendRepeatedInteger(msg, field, refl, cv); s.has_value()) {
    return s;
  }
  switch (field.cpp_type()) {
    case FD::CPPTYPE_FLOAT:
      if (cv.kind != CEL_DOUBLE) {
        return RepeatedAppendMismatch(field, "FLOAT", cv);
      }
      refl.AddFloat(&msg, &field, static_cast<float>(cv.payload.d));
      return absl::OkStatus();
    case FD::CPPTYPE_DOUBLE:
      if (cv.kind != CEL_DOUBLE) {
        return RepeatedAppendMismatch(field, "DOUBLE", cv);
      }
      refl.AddDouble(&msg, &field, cv.payload.d);
      return absl::OkStatus();
    default:
      return std::nullopt;
  }
}

// Numeric / bool / string / enum arms of `AppendRepeatedFromCelValue`
// (every non-MESSAGE cpp_type).  Returns the append Status for a
// handled cpp_type, or `std::nullopt` for CPPTYPE_MESSAGE (caller
// handles).  Mirror of the singular scalar setters but uses the
// `Add...` reflection family.
std::optional<absl::Status> AppendRepeatedScalar(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const CelValue& cv,
    const MemoryView& mem) {
  using FD = google::protobuf::FieldDescriptor;
  if (auto s = AppendRepeatedNumeric(msg, field, refl, cv); s.has_value()) {
    return s;
  }
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      if (cv.kind != CEL_BOOL) return RepeatedAppendMismatch(field, "BOOL", cv);
      refl.AddBool(&msg, &field, cv.payload.b != 0);
      return absl::OkStatus();
    case FD::CPPTYPE_STRING: {
      const bool want_bytes = field.type() == FD::TYPE_BYTES;
      if (want_bytes ? cv.kind != CEL_BYTES : cv.kind != CEL_STRING) {
        return RepeatedAppendMismatch(field, "STRING/BYTES", cv);
      }
      refl.AddString(&msg, &field, ReadSpanString(cv, mem));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_ENUM:
      if (cv.kind != CEL_INT) return RepeatedAppendMismatch(field, "ENUM", cv);
      if (auto s = CheckInt32Range(cv.payload.i, field.name()); !s.ok()) {
        return s;
      }
      refl.AddEnumValue(&msg, &field, static_cast<int>(cv.payload.i));
      return absl::OkStatus();
    default:
      return std::nullopt;
  }
}

// CPPTYPE_MESSAGE arm of `AppendRepeatedFromCelValue`.  A `null`
// element is PRUNED (skipped) per the `set_null/repeated_*` corpus
// rows; a non-message element targeting a WKT field packs via the
// shared dispatch; otherwise the source must be a CEL_MESSAGE resolved
// through `refs` and copied/packed into a fresh AddMessage element.
// Single call site → inlines back.
absl::Status AppendRepeatedMessage(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const CelValue& cv,
    const MemoryView& mem, const ExternrefTable& refs) {
  // A `null` element of a message-typed repeated field is PRUNED
  // (skipped), not appended — `[timestamp(1), null]` round-trips
  // as `[timestamp(1)]` per the `set_null/repeated_*` corpus rows.
  if (cv.kind == CEL_NULL) {
    return absl::OkStatus();
  }
  // Non-message element targeting a WKT message field (Duration /
  // Timestamp / Value / Struct / ListValue / Any) — pack into a
  // fresh AddMessage element via the shared WKT dispatch.
  if (cv.kind != CEL_MESSAGE) {
    if (auto s =
            MaybePackWktMessage(*refl.AddMessage(&msg, &field), cv, mem, &refs);
        s.has_value()) {
      return *std::move(s);
    }
  }
  // Repeated-of-message: source element is CEL_MESSAGE pointing
  // at a HostMessageBacking that exposes its underlying Message*.
  // We CopyFrom into a fresh `AddMessage` submessage so the
  // outer field carries an independent copy (the source backing
  // may be freed at ExternrefTable::Reset between Evals).
  if (cv.kind != CEL_MESSAGE) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: repeated message `", field.name(),
                     "` element kind=", static_cast<int>(cv.kind)));
  }
  const HostMessageBacking* src = refs.Lookup(cv.payload.msg_slot);
  if (src == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: repeated message `", field.name(),
                     "` element has no externref entry"));
  }
  const google::protobuf::Message* src_msg = src->message();
  if (src_msg == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: repeated message `", field.name(),
                     "` element backing has no proto message"));
  }
  google::protobuf::Message* dst = refl.AddMessage(&msg, &field);
  return WriteMessageOrPack(dst, *src_msg);
}

// Append one element to a repeated field from an arena-source
// CelValue.  Per-cpp_type dispatch mirrors `SetScalarField` but
// uses the `Add...` reflection family instead of `Set...`.
// CPPTYPE_MESSAGE elements expect the source to be a CEL_MESSAGE
// pointing at a HostMessageBacking — we CopyFrom into a fresh
// `AddMessage` submessage.
absl::Status AppendRepeatedFromCelValue(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const CelValue& cv,
    const MemoryView& mem, const ExternrefTable& refs) {
  using FD = google::protobuf::FieldDescriptor;
  if (auto s = AppendRepeatedScalar(msg, field, refl, cv, mem); s.has_value()) {
    return *std::move(s);
  }
  if (field.cpp_type() == FD::CPPTYPE_MESSAGE) {
    return AppendRepeatedMessage(msg, field, refl, cv, mem, refs);
  }
  ABSL_CHECK(false) << "AppendRepeatedFromCelValue: unknown cpp_type "
                    << static_cast<int>(field.cpp_type());
  return absl::InternalError("unreachable");
}

// Non-MESSAGE arms of `AppendRepeatedFromHostListValue` — reads each
// INT32/INT64/UINT32/UINT64 arms of AppendRepeatedHostScalar, reading
// via celwasm::Value's typed accessors (a failed accessor propagates
// its Status).  Returns nullopt for a non-integer cpp_type.
std::optional<absl::Status> AppendRepeatedHostInteger(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const celwasm::Value& v) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_INT32: {
      auto i = v.AsInt();
      if (!i.ok()) return i.status();
      if (auto s = CheckInt32Range(*i, field.name()); !s.ok()) return s;
      refl.AddInt32(&msg, &field, static_cast<int32_t>(*i));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_INT64: {
      auto i = v.AsInt();
      if (!i.ok()) return i.status();
      refl.AddInt64(&msg, &field, *i);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT32: {
      auto u = v.AsUint();
      if (!u.ok()) return u.status();
      if (auto s = CheckUint32Range(*u, field.name()); !s.ok()) return s;
      refl.AddUInt32(&msg, &field, static_cast<uint32_t>(*u));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT64: {
      auto u = v.AsUint();
      if (!u.ok()) return u.status();
      refl.AddUInt64(&msg, &field, *u);
      return absl::OkStatus();
    }
    default:
      return std::nullopt;
  }
}

// FLOAT / DOUBLE arms of AppendRepeatedHostScalar (both read AsDouble;
// FLOAT narrows).  Returns nullopt for a non-float cpp_type.
std::optional<absl::Status> AppendRepeatedHostFloat(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const celwasm::Value& v) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_FLOAT: {
      auto d = v.AsDouble();
      if (!d.ok()) return d.status();
      refl.AddFloat(&msg, &field, static_cast<float>(*d));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_DOUBLE: {
      auto d = v.AsDouble();
      if (!d.ok()) return d.status();
      refl.AddDouble(&msg, &field, *d);
      return absl::OkStatus();
    }
    default:
      return std::nullopt;
  }
}

std::optional<absl::Status> AppendRepeatedHostScalar(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const celwasm::Value& v) {
  using FD = google::protobuf::FieldDescriptor;
  if (auto s = AppendRepeatedHostInteger(msg, field, refl, v); s.has_value()) {
    return s;
  }
  if (auto s = AppendRepeatedHostFloat(msg, field, refl, v); s.has_value()) {
    return s;
  }
  switch (field.cpp_type()) {
    case FD::CPPTYPE_ENUM: {  // enum is int-encoded on the wire
      auto i = v.AsInt();
      if (!i.ok()) return i.status();
      if (auto s = CheckInt32Range(*i, field.name()); !s.ok()) {
        return s;
      }
      refl.AddEnumValue(&msg, &field, static_cast<int>(*i));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_BOOL: {
      auto b = v.AsBool();
      if (!b.ok()) return b.status();
      refl.AddBool(&msg, &field, *b);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_STRING: {
      auto s = (field.type() == FD::TYPE_BYTES) ? v.AsBytes() : v.AsString();
      if (!s.ok()) return s.status();
      refl.AddString(&msg, &field, std::string(*s));
      return absl::OkStatus();
    }
    default:
      return std::nullopt;
  }
}

// Same as `AppendRepeatedFromCelValue` but the source element comes
// from a host-list backing as a `celwasm::Value` (Activation::Bind
// path).  Per-cpp_type dispatch reads via celwasm::Value's typed
// accessors instead of CelValue payloads + MemoryView.
// Defined below with the host map-entry helpers; forward-declared so
// the repeated host arm can pack temporal WKT elements too.
// NOLINTNEXTLINE(readability-redundant-declaration)
std::optional<absl::Status> MaybePackWktFromHostValue(
    google::protobuf::Message& dst, const celwasm::Value& value);

absl::Status AppendRepeatedFromHostListValue(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const celwasm::Value& v) {
  using FD = google::protobuf::FieldDescriptor;
  using K = celwasm::Value::Kind;
  if (auto s = AppendRepeatedHostScalar(msg, field, refl, v); s.has_value()) {
    return *std::move(s);
  }
  if (field.cpp_type() == FD::CPPTYPE_MESSAGE) {
    // Null element of a message-typed repeated field is PRUNED, not
    // appended — parity with the arena path's `AppendRepeatedMessage`.
    if (v.kind() == K::kNull) {
      return absl::OkStatus();
    }
    google::protobuf::Message* dst = refl.AddMessage(&msg, &field);
    if (auto s = MaybePackWktFromHostValue(*dst, v); s.has_value()) {
      return *std::move(s);
    }
    if (v.kind() != K::kMessage) {
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: host-list repeated message `",
                       field.name(), "` element kind != kMessage"));
    }
    auto backing_or = v.MessageBacking();
    if (!backing_or.ok()) return backing_or.status();
    const google::protobuf::Message* src_msg = (*backing_or)->message();
    if (src_msg == nullptr) {
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: host-list repeated message `",
                       field.name(), "` backing has no proto message"));
    }
    return WriteMessageOrPack(dst, *src_msg);
  }
  ABSL_CHECK(false) << "AppendRepeatedFromHostListValue: unknown cpp_type "
                    << static_cast<int>(field.cpp_type());
  return absl::InternalError("unreachable");
}

absl::Status SetRepeatedField(google::protobuf::Message& msg,
                              const google::protobuf::FieldDescriptor& field,
                              const CelValue& source, const MemoryView& mem,
                              const ExternrefTable& refs) {
  const google::protobuf::Reflection* refl = msg.GetReflection();
  if (refl == nullptr) {
    return absl::InternalError(
        "CelSetFieldImpl: repeated set: message has no reflection");
  }
  if (source.kind == CEL_LIST_ARENA) {
    absl::Status status = absl::OkStatus();
    ForEachArenaListElement(
        source, mem, [&](const CelValue& elem, uint32_t /*i*/) {
          if (!status.ok()) return;
          status =
              AppendRepeatedFromCelValue(msg, field, *refl, elem, mem, refs);
        });
    return status;
  }
  if (source.kind == CEL_LIST_HOST) {
    const HostListBacking* backing = refs.LookupList(source.payload.ref_slot);
    if (backing == nullptr) {
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                       "` host source has no externref entry"));
    }
    absl::Status status = absl::OkStatus();
    backing->ForEach([&](const celwasm::Value& v) {
      if (!status.ok()) return;
      status = AppendRepeatedFromHostListValue(msg, field, *refl, v);
    });
    return status;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                   "` source kind=", static_cast<int>(source.kind),
                   " (expected CEL_LIST_ARENA or CEL_LIST_HOST)"));
}

// Build one map entry submessage on `msg`'s map field.  The entry's
// key + value sub-fields are populated by recursive `SetScalarField`
// calls — each map field's entry message is a synthetic 2-field
// proto (descriptor.proto §"map_entry"); both sub-fields are
// singular scalars (or singular message for value).  Reusing
// `SetScalarField` for the dispatch shares the cpp_type table with
// the singular path; the key path naturally rejects map keys typed
// as message (proto disallows map<message,_>) at the descriptor
// level.
// Set the value sub-field of an arena-map entry whose value type is a
// message.  A non-message source either packs into a WKT value
// submessage (Duration / Timestamp / Value / Struct / ListValue / Any)
// or errors; a CEL_MESSAGE source is resolved through `refs` and
// copied/packed via `WriteMessageOrPack`.  Split from
// `InsertArenaMapEntry` so the parent stays under the function-size
// gate; single call site → inlines back.
absl::Status SetArenaMapEntryMessageValue(
    google::protobuf::Message& entry,
    const google::protobuf::FieldDescriptor& key_fd,
    const google::protobuf::FieldDescriptor& val_fd, const CelValue& val_cv,
    const MemoryView& mem, const ExternrefTable& refs) {
  // Non-message value targeting a WKT map value (Duration /
  // Timestamp / Value / Struct / ListValue / Any) — pack via the
  // shared WKT dispatch onto the entry's mutable value submessage.
  google::protobuf::Message* dst =
      entry.GetReflection()->MutableMessage(&entry, &val_fd);
  if (val_cv.kind != CEL_MESSAGE) {
    if (auto s = MaybePackWktMessage(*dst, val_cv, mem, &refs); s.has_value()) {
      return *std::move(s);
    }
    return absl::InvalidArgumentError(absl::StrCat(
        "CelSetFieldImpl: map<", key_fd.name(), ", message `", val_fd.name(),
        "`> value kind=", static_cast<int>(val_cv.kind)));
  }
  const HostMessageBacking* src = refs.Lookup(val_cv.payload.msg_slot);
  const google::protobuf::Message* src_msg =
      src != nullptr ? src->message() : nullptr;
  if (src_msg == nullptr) {
    return absl::InvalidArgumentError(
        "CelSetFieldImpl: map message-value source has no backing");
  }
  return WriteMessageOrPack(dst, *src_msg);
}

absl::Status InsertArenaMapEntry(google::protobuf::Message& msg,
                                 const google::protobuf::FieldDescriptor& field,
                                 const CelValue& key_cv, const CelValue& val_cv,
                                 const MemoryView& mem,
                                 const ExternrefTable& refs) {
  const google::protobuf::Reflection& refl = *msg.GetReflection();
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(field, 1);
  const google::protobuf::FieldDescriptor* val_fd = MapEntryField(field, 2);
  const bool val_is_message =
      val_fd->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE;
  // A `null` value of a message-typed map field is PRUNED — the whole
  // entry is skipped, not inserted with a default value.  Matches the
  // `set_null/map_*_null_pruned` corpus rows.  Check before AddMessage
  // so no stray empty entry is left behind.
  if (val_is_message && val_cv.kind == CEL_NULL) {
    return absl::OkStatus();
  }
  google::protobuf::Message* entry = refl.AddMessage(&msg, &field);
  // Recursive scalar-set on the entry submessage.  For a value
  // typed as message we route through a `CopyFrom` via the same
  // CEL_MESSAGE-source path repeated-of-message uses.
  if (auto s = SetScalarField(*entry, *key_fd, key_cv, mem, &refs); !s.ok()) {
    return s;
  }
  if (val_is_message) {
    return SetArenaMapEntryMessageValue(*entry, *key_fd, *val_fd, val_cv, mem,
                                        refs);
  }
  return SetScalarField(*entry, *val_fd, val_cv, mem, &refs);
}

// INT32/INT64/UINT32/UINT64 arms of SetHostMapEntryKey.  Returns
// nullopt for a non-integer key cpp_type (caller handles bool/string).
// Integer-typed map-entry set, shared by the KEY and VALUE call sites:
// the two differed only in parameter names.  `fd` is whichever of the
// entry's key/value FieldDescriptors the caller is filling.
std::optional<absl::Status> SetHostMapEntryInteger(
    google::protobuf::Message& entry,
    const google::protobuf::Reflection& entry_refl,
    const google::protobuf::FieldDescriptor& fd, const celwasm::Value& v) {
  using FD = google::protobuf::FieldDescriptor;
  switch (fd.cpp_type()) {
    case FD::CPPTYPE_INT32: {
      auto i = v.AsInt();
      if (!i.ok()) return i.status();
      if (auto s = CheckInt32Range(*i, fd.name()); !s.ok()) return s;
      entry_refl.SetInt32(&entry, &fd, static_cast<int32_t>(*i));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_INT64: {
      auto i = v.AsInt();
      if (!i.ok()) return i.status();
      entry_refl.SetInt64(&entry, &fd, *i);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT32: {
      auto u = v.AsUint();
      if (!u.ok()) return u.status();
      if (auto s = CheckUint32Range(*u, fd.name()); !s.ok()) return s;
      entry_refl.SetUInt32(&entry, &fd, static_cast<uint32_t>(*u));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT64: {
      auto u = v.AsUint();
      if (!u.ok()) return u.status();
      entry_refl.SetUInt64(&entry, &fd, *u);
      return absl::OkStatus();
    }
    default:
      return std::nullopt;
  }
}

// Map keys are a closed set (bool / int / uint / string) per
// descriptor.proto; any other cpp_type is rejected (defence — the
// descriptor wouldn't have legalised the field).  A failed typed
// accessor propagates its Status.
absl::Status SetHostMapEntryKey(google::protobuf::Message& entry,
                                const google::protobuf::Reflection& entry_refl,
                                const google::protobuf::FieldDescriptor& key_fd,
                                const celwasm::Value& key) {
  using FD = google::protobuf::FieldDescriptor;
  if (auto s = SetHostMapEntryInteger(entry, entry_refl, key_fd, key);
      s.has_value()) {
    return *s;
  }
  switch (key_fd.cpp_type()) {
    case FD::CPPTYPE_BOOL: {
      auto b = key.AsBool();
      if (!b.ok()) return b.status();
      entry_refl.SetBool(&entry, &key_fd, *b);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_STRING: {
      auto s = key.AsString();
      if (!s.ok()) return s.status();
      entry_refl.SetString(&entry, &key_fd, std::string(*s));
      return absl::OkStatus();
    }
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: map key cpp_type ",
                       static_cast<int>(key_fd.cpp_type()), " not allowed"));
  }
}

// INT32/INT64/UINT32/UINT64 arms of SetHostMapEntryValue.  Returns
// nullopt for a non-integer cpp_type (caller handles the rest).

// FLOAT / DOUBLE arms of SetHostMapEntryValue (both read AsDouble;
// FLOAT narrows).  Returns nullopt for a non-float cpp_type.
std::optional<absl::Status> SetHostMapEntryFloatValue(
    google::protobuf::Message& entry,
    const google::protobuf::Reflection& entry_refl,
    const google::protobuf::FieldDescriptor& val_fd,
    const celwasm::Value& value) {
  using FD = google::protobuf::FieldDescriptor;
  switch (val_fd.cpp_type()) {
    case FD::CPPTYPE_FLOAT: {
      auto d = value.AsDouble();
      if (!d.ok()) return d.status();
      entry_refl.SetFloat(&entry, &val_fd, static_cast<float>(*d));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_DOUBLE: {
      auto d = value.AsDouble();
      if (!d.ok()) return d.status();
      entry_refl.SetDouble(&entry, &val_fd, *d);
      return absl::OkStatus();
    }
    default:
      return std::nullopt;
  }
}

// CPPTYPE_MESSAGE arm of SetHostMapEntryValue: resolve the source
// backing and copy/pack it into the entry's value submessage.
// Packs a host-origin Duration / Timestamp value into a WKT-typed
// `dst`, mirroring the arena path's `MaybePackWktMessage` temporal
// arms.  Only the temporal arms exist here: the JSON WKTs (Value /
// Struct / ListValue) and Any need a dyn-typed source, which
// RejectDyn keeps out of the static subset for declared variables.
// nullopt → `dst` is not a temporal WKT or `value` is not the
// matching temporal kind; the caller falls through to its
// message-backing path (and its diagnostic).
std::optional<absl::Status> MaybePackWktFromHostValue(
    google::protobuf::Message& dst, const celwasm::Value& value) {
  const google::protobuf::Descriptor* d = dst.GetDescriptor();
  const absl::string_view fqn = d != nullptr ? d->full_name() : "";
  CelDurTs dur_ts{};
  if (fqn == "google.protobuf.Duration") {
    auto dv = value.AsDuration();
    if (!dv.ok()) return std::nullopt;
    // Sign-correlated (seconds, nanos) with `_pad` zeroed — the shared
    // decomposer, not a second copy of the same arithmetic.
    DecomposeAbslDuration(*dv, &dur_ts);
  } else if (fqn == "google.protobuf.Timestamp") {
    auto tv = value.AsTimestamp();
    if (!tv.ok()) return std::nullopt;
    // ToUnixSeconds floors, so the sub-second remainder is in
    // [0, 1e9) nanos — the google.protobuf.Timestamp invariant.
    dur_ts.seconds = absl::ToUnixSeconds(*tv);
    dur_ts.nanos = static_cast<int32_t>(
        absl::ToInt64Nanoseconds(*tv - absl::FromUnixSeconds(dur_ts.seconds)));
    dur_ts._pad = 0;
  } else {
    return std::nullopt;
  }
  return PackDurationOrTimestamp(dst, dur_ts);
}

absl::Status SetHostMapEntryMessageValue(
    google::protobuf::Message& entry,
    const google::protobuf::Reflection& entry_refl,
    const google::protobuf::FieldDescriptor& val_fd,
    const celwasm::Value& value) {
  google::protobuf::Message* dst = entry_refl.MutableMessage(&entry, &val_fd);
  if (auto s = MaybePackWktFromHostValue(*dst, value); s.has_value()) {
    return *std::move(s);
  }
  auto backing_or = value.MessageBacking();
  if (!backing_or.ok()) return backing_or.status();
  const google::protobuf::Message* src_msg = (*backing_or)->message();
  if (src_msg == nullptr) {
    return absl::InvalidArgumentError(
        "CelSetFieldImpl: map message-value backing has no proto");
  }
  return WriteMessageOrPack(dst, *src_msg);
}

// STRING/BYTES and ENUM arms of SetHostMapEntryValue (enum is
// int-encoded).  Returns nullopt for a non-string/enum cpp_type.
std::optional<absl::Status> SetHostMapEntryStringOrEnumValue(
    google::protobuf::Message& entry,
    const google::protobuf::Reflection& entry_refl,
    const google::protobuf::FieldDescriptor& val_fd,
    const celwasm::Value& value) {
  using FD = google::protobuf::FieldDescriptor;
  switch (val_fd.cpp_type()) {
    case FD::CPPTYPE_STRING: {
      auto s = (val_fd.type() == FD::TYPE_BYTES) ? value.AsBytes()
                                                 : value.AsString();
      if (!s.ok()) return s.status();
      entry_refl.SetString(&entry, &val_fd, std::string(*s));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_ENUM: {
      auto i = value.AsInt();
      if (!i.ok()) return i.status();
      if (auto s = CheckInt32Range(*i, val_fd.name()); !s.ok()) {
        return s;
      }
      entry_refl.SetEnumValue(&entry, &val_fd, static_cast<int>(*i));
      return absl::OkStatus();
    }
    default:
      return std::nullopt;
  }
}

// Set the value sub-field of a host-map entry from a `celwasm::Value`.
// Every cpp_type is allowed (per descriptor.proto); message values
// resolve their backing and copy/pack via `WriteMessageOrPack`.  A
// failed typed accessor propagates its Status.
absl::Status SetHostMapEntryValue(
    google::protobuf::Message& entry,
    const google::protobuf::Reflection& entry_refl,
    const google::protobuf::FieldDescriptor& val_fd,
    const celwasm::Value& value) {
  using FD = google::protobuf::FieldDescriptor;
  if (auto s = SetHostMapEntryInteger(entry, entry_refl, val_fd, value);
      s.has_value()) {
    return *s;
  }
  if (auto s = SetHostMapEntryFloatValue(entry, entry_refl, val_fd, value);
      s.has_value()) {
    return *s;
  }
  if (auto s =
          SetHostMapEntryStringOrEnumValue(entry, entry_refl, val_fd, value);
      s.has_value()) {
    return *s;
  }
  switch (val_fd.cpp_type()) {
    case FD::CPPTYPE_BOOL: {
      auto b = value.AsBool();
      if (!b.ok()) return b.status();
      entry_refl.SetBool(&entry, &val_fd, *b);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_MESSAGE:
      return SetHostMapEntryMessageValue(entry, entry_refl, val_fd, value);
    default:
      break;  // handled by the delegations above; fall to the CHECK below.
  }
  ABSL_CHECK(false) << "InsertHostMapEntry: unknown value cpp_type "
                    << static_cast<int>(val_fd.cpp_type());
  return absl::InternalError("unreachable");
}

absl::Status InsertHostMapEntry(google::protobuf::Message& msg,
                                const google::protobuf::FieldDescriptor& field,
                                const google::protobuf::Reflection& refl,
                                const celwasm::Value& key,
                                const celwasm::Value& value) {
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(field, 1);
  const google::protobuf::FieldDescriptor* val_fd = MapEntryField(field, 2);
  // A `null` value of a message-typed map field is PRUNED — the whole
  // entry is skipped, not inserted with a default value.  Parity with
  // the arena path's `InsertArenaMapEntry`; check before AddMessage so
  // no stray empty entry is left behind.
  if (val_fd->cpp_type() ==
          google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
      value.kind() == celwasm::Value::Kind::kNull) {
    return absl::OkStatus();
  }
  google::protobuf::Message* entry = refl.AddMessage(&msg, &field);
  // The entry submessage's key/value sub-fields are *Set* (singular),
  // not *Add* (repeated) — so we dispatch through the two host-value
  // helpers rather than reusing `AppendRepeatedFromHostListValue`.
  // (Converting celwasm::Value → CelValue + reusing SetScalarField
  // would need arena bytes for strings, which we don't have here.)
  const google::protobuf::Reflection* entry_refl = entry->GetReflection();
  if (auto s = SetHostMapEntryKey(*entry, *entry_refl, *key_fd, key); !s.ok()) {
    return s;
  }
  return SetHostMapEntryValue(*entry, *entry_refl, *val_fd, value);
}

absl::Status SetMapField(google::protobuf::Message& msg,
                         const google::protobuf::FieldDescriptor& field,
                         const CelValue& source, const MemoryView& mem,
                         const ExternrefTable& refs) {
  const google::protobuf::Reflection* refl = msg.GetReflection();
  if (refl == nullptr) {
    return absl::InternalError(
        "CelSetFieldImpl: map set: message has no reflection");
  }
  if (source.kind == CEL_MAP_ARENA) {
    absl::Status status = absl::OkStatus();
    ForEachArenaMapEntry(
        source, mem, [&](const CelValue& k, const CelValue& v, uint32_t /*i*/) {
          if (!status.ok()) return;
          status = InsertArenaMapEntry(msg, field, k, v, mem, refs);
        });
    return status;
  }
  if (source.kind == CEL_MAP_HOST) {
    const HostMapBacking* backing = refs.LookupMap(source.payload.ref_slot);
    if (backing == nullptr) {
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: map `", field.name(),
                       "` host source has no externref entry"));
    }
    absl::Status status = absl::OkStatus();
    backing->ForEach([&](const celwasm::Value& k, const celwasm::Value& v) {
      if (!status.ok()) return;
      status = InsertHostMapEntry(msg, field, *refl, k, v);
    });
    return status;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("CelSetFieldImpl: map `", field.name(),
                   "` source kind=", static_cast<int>(source.kind),
                   " (expected CEL_MAP_ARENA or CEL_MAP_HOST)"));
}

}  // namespace

// Resolve the mutable `Message*` a `cel_set_field` call targets from
// its `msg_slot` CelValue.  On success `*out_msg` is the mutable proto
// and the returned optional is empty; a non-empty optional carries the
// terminal Status the trampoline returns directly (OkStatus for the
// poison short-circuit, InvalidArgument for a kind / backing / mutability
// mismatch).  Split from `CelSetFieldImpl` so the resolve preamble and
// the per-kind set dispatch live in separately-reviewable functions;
// single call site → inlines back.
static std::optional<absl::Status> ResolveOwnedSetTarget(
    const CelValue& msg_cv, const ExternrefTable& refs,
    google::protobuf::Message** absl_nonnull out_msg) {
  // Poison propagation: a prior field-set on this same message slot
  // overflowed and wrote a CEL_ERROR there.  Leave it untouched and
  // no-op so the error rides the construction's result slot out to the
  // expression value — cel-cpp likewise short-circuits a struct
  // constructor once one argument errors.  This is the read side of the
  // poison contract; see the out-of-range tail below.
  if (msg_cv.kind == CEL_ERROR) {
    return absl::OkStatus();
  }
  if (msg_cv.kind != CEL_MESSAGE) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: msg_slot kind is ",
                     static_cast<int>(msg_cv.kind), " (expected CEL_MESSAGE)"));
  }
  const HostMessageBacking* backing = refs.Lookup(msg_cv.payload.msg_slot);
  if (backing == nullptr) {
    return absl::InvalidArgumentError(
        "CelSetFieldImpl: msg_slot has no externref entry");
  }
  // Only OwnedProtoBacking is mutable through this path — the
  // host-bound `ProtoBacking` (Activation-bound messages) wraps a
  // non-const `const Message*` and must not be mutated.  A
  // dynamic_cast distinguishes; mismatch is a checker regression
  // (a `Foo{...}` literal must construct a fresh OwnedProtoBacking,
  // never feed an Activation::Bind binding through here).
  //
  // The const-cast is required because `ExternrefTable::Lookup`
  // returns a `const HostMessageBacking*` for read-side use, but
  // `cel_set_field` owns mutating writes to OwnedProtoBacking's
  // wrapped proto.  Lookup is the only API surface that hands out
  // the backing; widening the table return type to non-const would
  // leak mutability into every read site.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  auto* owned_backing = const_cast<OwnedProtoBacking*>(
      dynamic_cast<const OwnedProtoBacking*>(backing));
  if (owned_backing == nullptr) {
    return absl::InvalidArgumentError(
        "CelSetFieldImpl: msg_slot points at a non-owned message backing "
        "(can't mutate host-bound proto messages through cel_set_field)");
  }
  google::protobuf::Message* msg = owned_backing->mutable_message();
  ABSL_CHECK(msg != nullptr)
      << "CelSetFieldImpl: OwnedProtoBacking has null msg";
  *out_msg = msg;
  return std::nullopt;
}

namespace {

// Routes a field-set to the matching writer.  Map precedes repeated
// because every proto map field is also `is_repeated()` per
// descriptor.proto.
absl::Status DispatchFieldSet(google::protobuf::Message& msg,
                              const google::protobuf::FieldDescriptor& field,
                              const CelValue& value_cv,
                              const TrampolineContext& ctx) {
  if (field.is_map()) {
    return SetMapField(msg, field, value_cv, ctx.mem, ctx.refs);
  }
  if (field.is_repeated()) {
    return SetRepeatedField(msg, field, value_cv, ctx.mem, ctx.refs);
  }
  // 3VL operands never reach here: `CelSetFieldImpl` absorbs them into
  // the message slot before dispatching, so one arriving is an
  // invariant break rather than user input.
  ABSL_CHECK(value_cv.kind != CEL_UNKNOWN && value_cv.kind != CEL_ERROR)
      << "DispatchFieldSet: 3VL value kind=" << static_cast<int>(value_cv.kind)
      << " should have been absorbed by CelSetFieldImpl";
  return SetScalarField(msg, field, value_cv, ctx.mem, &ctx.refs);
}

}  // namespace

absl::Status CelSetFieldImpl(uint32_t msg_slot, uint32_t field_ref_id,
                             uint32_t value_slot,
                             const TrampolineContext& ctx) {
  const CelValue msg_cv = ctx.mem.ReadCelValue(msg_slot);
  google::protobuf::Message* msg = nullptr;
  if (auto terminal = ResolveOwnedSetTarget(msg_cv, ctx.refs, &msg);
      terminal.has_value()) {
    return *std::move(terminal);
  }

  const FieldRefEntry* field_ref = ResolveFieldRef(ctx.bindings, field_ref_id);
  if (field_ref == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CelSetFieldImpl: field_ref_id=", field_ref_id, " out of range"));
  }
  const google::protobuf::FieldDescriptor* field = ResolveFieldDescriptor(
      *msg, field_ref->field_number, field_ref->field_name);
  if (field == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: field `", field_ref->field_name,
                     "` not found on descriptor"));
  }

  const CelValue value_cv = ctx.mem.ReadCelValue(value_slot);
  // 3VL absorption — the same guard every other trampoline opens
  // with.  `Foo{a: 1/0}` type-checks (the field expression is
  // int-typed) and evaluates to an error, and under PartialEval any
  // field expression can be Unknown.  Per langdef the construction
  // then IS that error / unknown, so stamp it into the message slot,
  // overwriting the partially-built message; the construction's
  // trailing `(i32.const out_slot)` carries it onward.  Absorbing
  // here rather than in DispatchFieldSet keeps the map / repeated /
  // scalar arms free of 3VL handling.
  if (value_cv.kind == CEL_UNKNOWN || value_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(msg_slot, value_cv);
    return absl::OkStatus();
  }
  absl::Status set_status = DispatchFieldSet(*msg, *field, value_cv, ctx);

  // Poison-on-range-error (the write side of the poison contract).  An
  // out-of-range scalar/enum assignment is a CEL value-level error —
  // cel-cpp returns an ErrorValue, so the expression result is a CEL
  // error, not a host trap.  The field-write helpers signal this with
  // `kOutOfRange`; convert it to a CEL_ERROR poison stamped into
  // msg_slot, overwriting the partially-built message, and report
  // success.  The construction's trailing `(i32.const out_slot)` then
  // naturally carries the error.  Every other non-OK status is an
  // internal invariant violation (kind mismatch, missing reflection)
  // and stays non-OK → trap, per the "a release build
  // that miscompiles silently is worse than one that crashes" rule.
  if (set_status.code() == absl::StatusCode::kOutOfRange) {
    WriteWireError(CEL_ERR_OVERFLOW, msg_slot, ctx.mem);
    return absl::OkStatus();
  }
  return set_status;
}

}  // namespace celwasm
