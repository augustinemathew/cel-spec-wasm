#include "compiler_v2/api/internal/cel_host.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "compiler_v2/api/error.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_legacy.h"
#include "google/protobuf/message.h"
#include "google/protobuf/util/message_differencer.h"

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

// Helper: `Value::Error` with a chosen code, formatted message.
cel::Value MakeError(cel::ErrorCode code, std::string message) {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/code,
      /*message=*/std::move(message),
      /*expr_id=*/0,
  });
}

// Forward declarations for the WKT-peel helpers used by both
// `UnpackAnyToValue` (Any-of-WKT chain) and `ReadScalarField`
// (singular CPPTYPE_MESSAGE arm).  Bodies are defined below in
// this anonymous namespace alongside the other peelers.
std::optional<cel::Value> UnpackWrapperMessage(
    const google::protobuf::Message& sub);
std::optional<cel::Value> UnpackWellKnownTimeMessage(
    const google::protobuf::Message& sub);

// Chain the two well-known-type peelers in a single call site:
// returns the inner-scalar / Timestamp / Duration value if `sub` is
// one of the recognised WKT message types, otherwise `std::nullopt`.
// Lets both Any-unwrap and proto-field-read share one entry point
// without duplicating the if-cascade.
inline std::optional<cel::Value> MaybeUnpackWktMessage(
    const google::protobuf::Message& sub) {
  if (auto wrap = UnpackWrapperMessage(sub); wrap.has_value()) return wrap;
  if (auto wkt = UnpackWellKnownTimeMessage(sub); wkt.has_value()) return wkt;
  return std::nullopt;
}

// Parse `any.type_url`, look up the wrapped FQN in `pool`, parse
// `any.value` against that descriptor, return an owning backing.
// Returns a `cel::Value`:
//   - `null` when type_url is empty (matches the proto-literal
//     null-on-unset rule and serves as the "Any not populated" signal).
//   - `Error(kFieldNotFound)` when the FQN isn't in the pool.
//   - `Error(kTypeMismatch)` when value bytes don't parse.
//   - `HostMessage(OwnedProtoBacking(unwrapped))` on success.
cel::Value UnpackAnyToValue(const google::protobuf::Message& any,
                            const google::protobuf::DescriptorPool* pool) {
  const google::protobuf::Descriptor* any_desc = any.GetDescriptor();
  const google::protobuf::Reflection* any_refl = any.GetReflection();
  const google::protobuf::FieldDescriptor* type_url_fd =
      any_desc->FindFieldByName("type_url");
  const google::protobuf::FieldDescriptor* value_fd =
      any_desc->FindFieldByName("value");
  ABSL_CHECK(any_refl != nullptr && type_url_fd != nullptr &&
             value_fd != nullptr)
      << "UnpackAnyToValue: Any descriptor missing type_url/value/reflection";
  std::string url_scratch;
  std::string val_scratch;
  const std::string& type_url =
      any_refl->GetStringReference(any, type_url_fd, &url_scratch);
  if (type_url.empty()) return cel::Value::Null();
  // FQN = substring after the last '/'.  No slash → FQN is the whole
  // string; subsequent pool lookup either resolves or fails.
  const size_t slash = type_url.rfind('/');
  const absl::string_view fqn =
      (slash == absl::string_view::npos)
          ? absl::string_view(type_url)
          : absl::string_view(type_url).substr(slash + 1);
  const google::protobuf::Descriptor* sub_desc =
      pool != nullptr ? pool->FindMessageTypeByName(std::string(fqn)) : nullptr;
  if (sub_desc == nullptr) {
    return MakeError(cel::ErrorCode::kFieldNotFound,
                     absl::StrCat("Any type_url FQN `", fqn,
                                  "` not registered in descriptor pool"));
  }
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(
          sub_desc);
  if (prototype == nullptr) {
    return MakeError(cel::ErrorCode::kFieldNotFound,
                     absl::StrCat("Any type `", fqn,
                                  "` has no generated_factory prototype"));
  }
  std::unique_ptr<google::protobuf::Message> sub(prototype->New());
  const std::string& bytes =
      any_refl->GetStringReference(any, value_fd, &val_scratch);
  if (!sub->ParseFromString(bytes)) {
    return MakeError(
        cel::ErrorCode::kTypeMismatch,
        absl::StrCat("Any payload bytes don't parse against `", fqn, "`"));
  }
  // Chain wrapper-peel + WKT-time-peel after Any-unwrap so
  // `Any{Int32Value{value:1}}` surfaces as `int 1` and
  // `Any{Timestamp{...}}` as `CEL_TIMESTAMP` (instead of CEL_MESSAGE).
  // Mirrors the same chain at `ReadScalarField` so Any-erased and
  // field-erased WKT operands behave identically.
  if (auto v = MaybeUnpackWktMessage(*sub); v.has_value()) return *std::move(v);
  return cel::Value::OwnedMessage(std::move(sub));
}

// Well-known time-type normaliser for proto field reads.  When a
// singular CPPTYPE_MESSAGE field resolves to
// `google.protobuf.Timestamp` / `google.protobuf.Duration`, peel the
// (seconds, nanos) pair via reflection (field numbers 1 and 2 are
// pinned by the well-known type definitions) and return the matching
// cel::Value::Timestamp / Duration.  Returns nullopt for any other
// message type — caller falls back to `HostMessage(ProtoBacking)`.
//
// Reflection-based on purpose: works for both generated-class
// messages (via DynamicCastToGenerated downcast) and dynamic
// messages loaded from a runtime descriptor pool.
std::optional<cel::Value> UnpackWellKnownTimeMessage(
    const google::protobuf::Message& sub) {
  const google::protobuf::Descriptor* d = sub.GetDescriptor();
  if (d == nullptr) return std::nullopt;
  const absl::string_view fqn = d->full_name();
  const bool is_timestamp = (fqn == "google.protobuf.Timestamp");
  const bool is_duration = (fqn == "google.protobuf.Duration");
  if (!is_timestamp && !is_duration) return std::nullopt;
  const google::protobuf::Reflection* refl = sub.GetReflection();
  if (refl == nullptr) return std::nullopt;
  const google::protobuf::FieldDescriptor* sf = d->FindFieldByNumber(1);
  const google::protobuf::FieldDescriptor* nf = d->FindFieldByNumber(2);
  if (sf == nullptr || nf == nullptr) return std::nullopt;
  const int64_t s = refl->GetInt64(sub, sf);
  const int32_t ns = refl->GetInt32(sub, nf);
  if (is_timestamp) {
    return cel::Value::Timestamp(absl::UnixEpoch() + absl::Seconds(s) +
                                 absl::Nanoseconds(ns));
  }
  return cel::Value::Duration(absl::Seconds(s) + absl::Nanoseconds(ns));
}

// Closed set of 9 google.protobuf wrapper FQNs.  Shared between the
// unset-field-null gate in `ReadScalarField` and the peel-the-inner-
// scalar `UnpackWrapperMessage` below (and chained from
// `UnpackAnyToValue` for Any-of-wrapper).  Per langdef line 484-486
// the unset-wrapper-field-evaluates-to-null exception applies
// regardless of proto syntax (proto2 and proto3 agree).
bool IsWrapperFqn(absl::string_view fqn) {
  return fqn == "google.protobuf.BoolValue" ||
         fqn == "google.protobuf.Int32Value" ||
         fqn == "google.protobuf.Int64Value" ||
         fqn == "google.protobuf.UInt32Value" ||
         fqn == "google.protobuf.UInt64Value" ||
         fqn == "google.protobuf.FloatValue" ||
         fqn == "google.protobuf.DoubleValue" ||
         fqn == "google.protobuf.StringValue" ||
         fqn == "google.protobuf.BytesValue";
}

// Well-known WRAPPER-type normaliser for proto field reads.  Mirror
// of `UnpackWellKnownTimeMessage`: when a singular CPPTYPE_MESSAGE
// field resolves to one of the 9
// google.protobuf.{Bool,Int32,Int64,UInt32,UInt64,Float,Double,
// String,Bytes}Value types, peel the inner `value` field (number 1)
// via reflection and return the matching cel::Value scalar.
// Returns std::nullopt for any other message type — caller falls
// back to HostMessage(ProtoBacking).
//
// Reflection-based: works for both generated-class messages
// (DynamicCastToGenerated downcast) and dynamic messages loaded
// from a runtime descriptor pool.
//
// Per langdef §"Dynamic Values" line 479 ("wrapper types |
// converted as eponymous field type"): this helper handles the
// SET case — reading the inner scalar.  The UNSET case
// (field-evaluates-to-null per line 484-486) is gated at the
// caller before this helper is reached.
// String / bytes peel for the WKT wrapper inner `value` field.
// CPPTYPE_STRING covers both string and bytes wire types — the
// distinction comes from `field.type()`, not `cpp_type()`.  Pulled
// out of `UnpackWrapperMessage` so the parent stays under the
// readability-function-size gate (9-branch dispatch + this branch
// would otherwise overflow).
cel::Value UnpackWrapperStringOrBytes(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& sub,
    const google::protobuf::FieldDescriptor& vf) {
  std::string scratch;
  std::string s(refl.GetStringReference(sub, &vf, &scratch));
  if (vf.type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
    return cel::Value::Bytes(std::move(s));
  }
  return cel::Value::String(std::move(s));
}

std::optional<cel::Value> UnpackWrapperMessage(
    const google::protobuf::Message& sub) {
  const google::protobuf::Descriptor* d = sub.GetDescriptor();
  if (d == nullptr) return std::nullopt;
  if (!IsWrapperFqn(d->full_name())) return std::nullopt;
  const google::protobuf::Reflection* refl = sub.GetReflection();
  if (refl == nullptr) return std::nullopt;
  const google::protobuf::FieldDescriptor* vf = d->FindFieldByNumber(1);
  if (vf == nullptr) return std::nullopt;
  // Dispatch on the inner `value` field's cpp_type — closed set per
  // the 9 wrapper definitions.  IsWrapperFqn above gates entry, so
  // any other cpp_type here is an invariant violation (corrupted
  // descriptor pool); CHECK at the default arm.
  using FD = google::protobuf::FieldDescriptor;
  switch (vf->cpp_type()) {
    case FD::CPPTYPE_BOOL:
      return cel::Value::Bool(refl->GetBool(sub, vf));
    case FD::CPPTYPE_INT32:
      return cel::Value::Int(refl->GetInt32(sub, vf));
    case FD::CPPTYPE_INT64:
      return cel::Value::Int(refl->GetInt64(sub, vf));
    case FD::CPPTYPE_UINT32:
      return cel::Value::Uint(refl->GetUInt32(sub, vf));
    case FD::CPPTYPE_UINT64:
      return cel::Value::Uint(refl->GetUInt64(sub, vf));
    case FD::CPPTYPE_FLOAT:
      return cel::Value::Double(refl->GetFloat(sub, vf));
    case FD::CPPTYPE_DOUBLE:
      return cel::Value::Double(refl->GetDouble(sub, vf));
    case FD::CPPTYPE_STRING:
      return UnpackWrapperStringOrBytes(*refl, sub, *vf);
    default:
      ABSL_CHECK(false)
          << "UnpackWrapperMessage: WKT-wrapper FQN claims an unexpected "
             "inner cpp_type "
          << static_cast<int>(vf->cpp_type());
      return std::nullopt;
  }
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

// Read a singular CPPTYPE_MESSAGE field, applying the langdef
// §"Field Selection" presence rules and the WKT auto-peel chain
// (Any-unwrap, Timestamp / Duration peel, wrapper peel — see
// `doc/implementation-plan/rewrite/cel-host-surface.md` for the
// peel chain spec).  Extracted from `ReadScalarField` so the dispatch
// ladder there stays under the readability-function-size gate.
//
// Presence rules:
//   - Wrapper-typed unset field           → Null (langdef line 484-486;
//                                          both proto2 and proto3,
//                                          exception to default-msg).
//   - Proto3 generic-message unset field  → Null (langdef §"Field
//                                          Selection").
//   - Proto2 generic-message unset field  → default-instance message.
//
// Peel chain (after presence resolves the field is set OR proto2
// default-instance):
//   - Any → UnpackAnyToValue (chains wrapper / WKT-time peels).
//   - Wrapper → inner scalar (CEL_BOOL/INT/UINT/DOUBLE/STRING/BYTES).
//   - Timestamp / Duration → CEL_TIMESTAMP / CEL_DURATION.
//   - Otherwise → HostMessage(ProtoBacking).
absl::StatusOr<cel::Value> ReadSingularMessageField(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field) {
  const google::protobuf::Descriptor* mt = field.message_type();
  if (mt != nullptr && IsWrapperFqn(mt->full_name()) &&
      !refl.HasField(msg, &field)) {
    return cel::Value::Null();
  }
  const google::protobuf::FileDescriptor* file =
      field.containing_type()->file();
  const bool is_proto3 =
      file != nullptr &&
      google::protobuf::FileDescriptorLegacy(file).edition() ==
          google::protobuf::EDITION_PROTO3;
  if (is_proto3 && !refl.HasField(msg, &field)) {
    return cel::Value::Null();
  }
  const google::protobuf::Message& sub = refl.GetMessage(msg, &field);
  if (mt != nullptr && mt->full_name() == "google.protobuf.Any") {
    return UnpackAnyToValue(sub, mt->file()->pool());
  }
  if (auto v = MaybeUnpackWktMessage(sub); v.has_value()) {
    return *std::move(v);
  }
  return cel::Value::HostMessage(std::make_shared<ProtoBacking>(&sub));
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
    return ReadSingularMessageField(*refl, msg, field);
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
// OwnedProtoBacking — Layer 1 over an owned `unique_ptr<Message>`.
// ══════════════════════════════════════════════════════════════════
//
// Constructed by `CelMakeMessageImpl` for proto literals built inside
// the wasm module; owns the heap-allocated default-proto so the
// `ExternrefTable::Reset()` between Evals frees it.  Reads delegate
// to a composed `ProtoBacking` over the owned message — same
// reflection path used for host-bound messages, no duplicated logic.

OwnedProtoBacking::OwnedProtoBacking(
    std::unique_ptr<google::protobuf::Message> msg)
    : msg_(std::move(msg)), inner_(msg_.get()) {
  ABSL_CHECK(msg_ != nullptr) << "OwnedProtoBacking: null message";
}

absl::StatusOr<cel::Value> OwnedProtoBacking::ReadField(
    int field_number, absl::string_view field_name,
    const cel::CelType& expected_type) const {
  return inner_.ReadField(field_number, field_name, expected_type);
}

bool OwnedProtoBacking::HasField(int field_number,
                                 absl::string_view field_name) const {
  return inner_.HasField(field_number, field_name);
}

// ══════════════════════════════════════════════════════════════════
// ProtoBacking — Layer 1 over google::protobuf::Message.
// ══════════════════════════════════════════════════════════════════

absl::StatusOr<cel::Value> ProtoBacking::ReadField(
    int field_number, absl::string_view field_name,
    const cel::CelType& /*expected_type*/) const {
  ABSL_CHECK(msg_ != nullptr) << "ProtoBacking::ReadField: null message";
  const google::protobuf::FieldDescriptor* field =
      ResolveFieldDescriptor(*msg_, field_number, field_name);
  if (field == nullptr) return FieldNotFound(field_name);

  // Map fields land here as `Value::HostMap(ProtoMap{…})` — the
  // trampoline interns the backing into the ExternrefTable and hands
  // a `CEL_MAP_HOST` slot back to wasm.
  // REPEATED (non-map) fields land as `Value::HostList(ProtoList{…})`
  // — same intern path, separate ExternrefTable namespace.
  // `is_map()` is checked first because every map field is also
  // `is_repeated()` per descriptor.proto.
  if (field->is_map()) {
    return cel::Value::HostMap(std::make_shared<ProtoMap>(msg_, field));
  }
  if (field->is_repeated()) {
    return cel::Value::HostList(std::make_shared<ProtoList>(msg_, field));
  }
  return ReadScalarField(*msg_, *field);
}

bool ProtoBacking::HasField(int field_number,
                            absl::string_view field_name) const {
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

// ══════════════════════════════════════════════════════════════════
// HostMap — vector-backed `HostMapBacking` for user bindings.
// ══════════════════════════════════════════════════════════════════

// File-scope helpers shared by HostMap (Layer-1, vector-backed) and
// ProtoMap (reflection-backed).  TU-internal via `static` so the
// symbols don't escape this translation unit.

// langdef §"Equality" / §"Map keys": cross-type numeric equality
// (int ≡ uint by mathematical value; negative int never equals any
// uint), structural same-kind for bool / string.  Mirrors
// `map_keys_equal` in `runtime/cel_runtime.c` so host- and arena-
// built maps lookup identically.  Returns OkStatus on legal compares;
// returns false for any non-key-kind operand (caller should already
// have rejected; this is defence-in-depth).
static bool MapKeysEqual(const cel::Value& a, const cel::Value& b) {
  using K = cel::Value::Kind;
  const K ka = a.kind();
  const K kb = b.kind();
  if (ka == K::kInt && kb == K::kInt) {
    return *a.AsInt() == *b.AsInt();
  }
  if (ka == K::kUint && kb == K::kUint) {
    return *a.AsUint() == *b.AsUint();
  }
  if (ka == K::kInt && kb == K::kUint) {
    int64_t ai = *a.AsInt();
    return ai >= 0 && static_cast<uint64_t>(ai) == *b.AsUint();
  }
  if (ka == K::kUint && kb == K::kInt) {
    int64_t bi = *b.AsInt();
    return bi >= 0 && *a.AsUint() == static_cast<uint64_t>(bi);
  }
  if (ka == K::kBool && kb == K::kBool) {
    return *a.AsBool() == *b.AsBool();
  }
  if (ka == K::kString && kb == K::kString) {
    return *a.AsString() == *b.AsString();
  }
  return false;
}

// Convenience for Get/ContainsKey: caller didn't pre-validate the
// key's kind; emit a kTypeMismatch error value if it's not a legal
// map key.  Mirrors the runtime's `is_valid_map_key_kind` gate.
static bool IsValidMapKeyKind(cel::Value::Kind k) {
  return k == cel::Value::Kind::kBool || k == cel::Value::Kind::kInt ||
         k == cel::Value::Kind::kUint || k == cel::Value::Kind::kString;
}

static cel::Value KeyTypeMismatch() {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/cel::ErrorCode::kTypeMismatch,
      /*message=*/"map key kind is not bool/int/uint/string",
      /*expr_id=*/0,
  });
}

static cel::Value NoSuchKey() {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/cel::ErrorCode::kKeyNotFound,
      /*message=*/"no such key",
      /*expr_id=*/0,
  });
}

HostMap::HostMap(std::vector<std::pair<cel::Value, cel::Value>> entries)
    : entries_(std::move(entries)) {}

size_t HostMap::Size() const {
  return entries_.size();
}

absl::StatusOr<cel::Value> HostMap::Get(
    const cel::Value& key, const cel::CelType& /*expected_value_type*/) const {
  if (!IsValidMapKeyKind(key.kind())) {
    return KeyTypeMismatch();
  }
  for (const auto& [k, v] : entries_) {
    if (MapKeysEqual(k, key)) return v;
  }
  return NoSuchKey();
}

bool HostMap::ContainsKey(const cel::Value& key) const {
  if (!IsValidMapKeyKind(key.kind())) return false;
  return std::any_of(entries_.begin(), entries_.end(), [&](const auto& kv) {
    return MapKeysEqual(kv.first, key);
  });
}

void HostMap::ForEach(
    absl::FunctionRef<void(const cel::Value&, const cel::Value&)> visit) const {
  for (const auto& [k, v] : entries_) {
    visit(k, v);
  }
}

// ══════════════════════════════════════════════════════════════════
// CelMapLookupImpl — Layer 2 entry for `cel_host.cel_map_lookup`.
// Reads map_slot's ref_slot, dereferences to a HostMapBacking,
// decodes key, calls Get(), marshals result back.
// ══════════════════════════════════════════════════════════════════

namespace {

// Decode a scalar CelValue into a cel::Value.  Map-key kinds only
// (bool/int/uint/string) — every other kind returns nullopt and the
// caller surfaces a TYPE_MISMATCH error to the wasm side.  Strings
// dereference through the MemoryView so the cel::Value owns a copy.
std::optional<cel::Value> DecodeKey(const CelValue& cv, const MemoryView& mem) {
  switch (cv.kind) {
    case CEL_BOOL:
      return cel::Value::Bool(cv.payload.b != 0);
    case CEL_INT:
      return cel::Value::Int(cv.payload.i);
    case CEL_UINT:
      return cel::Value::Uint(cv.payload.u);
    case CEL_STRING: {
      absl::string_view bytes =
          mem.ReadSpan(cv.payload.s.ptr, cv.payload.s.len);
      return cel::Value::String(std::string(bytes));
    }
    default:
      return std::nullopt;
  }
}

// Map the host-side ErrorCode catalogue → the wire `CEL_ERR_*`
// numeric code carried in `CelValue.payload.err`.  Both sides
// extend independently; surface unrecognized codes as TYPE_MISMATCH
// rather than dropping silently.
uint32_t WireErrorCode(cel::ErrorCode c) {
  switch (c) {
    case cel::ErrorCode::kOverflow:
      return CEL_ERR_OVERFLOW;
    case cel::ErrorCode::kDivideByZero:
      return CEL_ERR_DIVIDE_BY_ZERO;
    case cel::ErrorCode::kModulusByZero:
      return CEL_ERR_MODULUS_BY_ZERO;
    case cel::ErrorCode::kTypeMismatch:
      return CEL_ERR_TYPE_MISMATCH;
    case cel::ErrorCode::kTypeUnsupported:
      return CEL_ERR_TYPE_UNSUPPORTED;
    case cel::ErrorCode::kKeyNotFound:
      return CEL_ERR_NO_SUCH_KEY;
    case cel::ErrorCode::kFieldNotFound:
      return CEL_ERR_FIELD_NOT_FOUND;
    case cel::ErrorCode::kIndexOutOfBounds:
      return CEL_ERR_INDEX_OUT_OF_BOUNDS;
    case cel::ErrorCode::kInvalidArgument:
      return CEL_ERR_INVALID_ARGUMENT;
    case cel::ErrorCode::kHostAdapterError:
      return CEL_ERR_HOST_ADAPTER_ERROR;
    default:
      return CEL_ERR_TYPE_MISMATCH;
  }
}

// Build a CEL_ERROR CelValue with the given wire code and write it
// to `out_slot`.  Used by every Layer-2 arm that surfaces a
// spec-level error in-wire (rather than returning non-OK Status).
void WriteWireError(uint32_t wire_code, uint32_t out_slot, MemoryView& mem) {
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = wire_code;
  mem.WriteCelValue(out_slot, err);
}

// Encode the (string|bytes) span via the per-eval ArenaAllocator.
absl::Status EncodeSpan(const cel::Value& v, CelValue* out,
                        ArenaAllocator& alloc) {
  using K = cel::Value::Kind;
  absl::string_view s = v.kind() == K::kString ? *v.AsString() : *v.AsBytes();
  uint32_t off = 0;
  uint8_t* p = alloc.Alloc(s.size(), &off);
  if (p == nullptr && !s.empty()) {
    return absl::ResourceExhaustedError("arena OOM in CelMapLookupImpl");
  }
  if (!s.empty()) std::memcpy(p, s.data(), s.size());
  out->kind = v.kind() == K::kString ? CEL_STRING : CEL_BYTES;
  out->payload.s.ptr = off;
  out->payload.s.len = static_cast<uint32_t>(s.size());
  return absl::OkStatus();
}

// m7b §3.1 / Probe D — `DecomposeAbslDuration` lives in
// `cel_host.h` (header-inline) so instance.cc shares it.

absl::Status EncodeDurationValue(const cel::Value& v, CelValue* out) {
  auto d_or = v.AsDuration();
  if (!d_or.ok()) return d_or.status();
  out->kind = CEL_DURATION;
  DecomposeAbslDuration(*d_or, &out->payload.dur);
  return absl::OkStatus();
}

absl::Status EncodeTimestampValue(const cel::Value& v, CelValue* out) {
  auto t_or = v.AsTimestamp();
  if (!t_or.ok()) return t_or.status();
  out->kind = CEL_TIMESTAMP;
  DecomposeAbslDuration(*t_or - absl::UnixEpoch(), &out->payload.ts);
  return absl::OkStatus();
}

// Encode a cel::Value into a CelValue, allocating string/bytes
// payloads through the per-eval ArenaAllocator.  Returns non-OK
// Status on infrastructure failure (arena OOM); spec-level errors
// inside the input Value already encode as `{kind:CEL_ERROR, err:…}`.
absl::Status EncodeValue(const cel::Value& v, CelValue* out,
                         ArenaAllocator& alloc) {
  using K = cel::Value::Kind;
  switch (v.kind()) {
    case K::kNull:
      out->kind = CEL_NULL;
      return absl::OkStatus();
    case K::kBool:
      out->kind = CEL_BOOL;
      out->payload.b = *v.AsBool() ? 1 : 0;
      return absl::OkStatus();
    case K::kInt:
      out->kind = CEL_INT;
      out->payload.i = *v.AsInt();
      return absl::OkStatus();
    case K::kUint:
      out->kind = CEL_UINT;
      out->payload.u = *v.AsUint();
      return absl::OkStatus();
    case K::kDouble:
      out->kind = CEL_DOUBLE;
      out->payload.d = *v.AsDouble();
      return absl::OkStatus();
    case K::kString:
    case K::kBytes:
      return EncodeSpan(v, out, alloc);
    case K::kError: {
      const cel::ErrorPayload* e = *v.ErrorInfo();
      out->kind = CEL_ERROR;
      out->payload.err = WireErrorCode(e->code);
      return absl::OkStatus();
    }
    case K::kUnknown:
      // Layer 2 contract: backings don't return unknowns — operand-
      // pair propagation happens before this encoder.  M4
      // PartialEval surfaces unknowns via a different path
      // (MatchesAnyUnknownPattern).
      ABSL_CHECK(false) << "EncodeValue: kUnknown is unreachable from "
                           "Layer 1 returns";
    case K::kMessage:
    case K::kMap:
    case K::kList:
      // Aggregate kinds are handled by `EncodeFieldResult` /
      // `EncodeAggregateIfAny`, never via the inline path.  Reaching
      // here is a contract violation by the caller.
      ABSL_CHECK(false) << "EncodeValue: aggregate kind "
                        << static_cast<int>(v.kind())
                        << " must route through EncodeFieldResult";
    case K::kDuration:
      return EncodeDurationValue(v, out);
    case K::kTimestamp:
      return EncodeTimestampValue(v, out);
    case K::kType:
      // M9: Layer-1 backings don't return type-values — `type(x)` is
      // always lowered to the standard `cel_type_of_at_v` runtime
      // helper, never reached through a Layer-1 backing call.
      // Activation-side encoding lives in `instance.cc::EncodeType`.
      ABSL_CHECK(false) << "EncodeValue: kType is unreachable from "
                           "Layer-1 backings (type(x) is a runtime helper)";
  }
  ABSL_CHECK(false) << "EncodeValue: unhandled kind "
                    << static_cast<int>(v.kind());
}

// Encode a Layer-1 aggregate (message / map / list) by interning
// the backing into the matching externref namespace and writing the
// resulting ref_slot.  Returns true if `v` is an aggregate kind and
// has been encoded; false if `v` is a scalar (caller falls through
// to the inline EncodeValue path).
absl::StatusOr<bool> EncodeAggregateIfAny(const cel::Value& v,
                                          uint32_t out_slot,
                                          const TrampolineContext& ctx) {
  using K = cel::Value::Kind;
  CelValue cv{};
  if (v.kind() == K::kMessage) {
    auto sub_or = v.SharedMessageBacking();
    if (!sub_or.ok()) return sub_or.status();
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = ctx.refs.Intern(*std::move(sub_or));
  } else if (v.kind() == K::kMap) {
    auto sub_or = v.SharedMapBacking();
    if (!sub_or.ok()) return sub_or.status();
    cv.kind = CEL_MAP_HOST;
    cv.payload.ref_slot = ctx.refs.InternMap(*std::move(sub_or));
  } else if (v.kind() == K::kList) {
    auto sub_or = v.SharedListBacking();
    if (!sub_or.ok()) return sub_or.status();
    cv.kind = CEL_LIST_HOST;
    cv.payload.ref_slot = ctx.refs.InternList(*std::move(sub_or));
  } else {
    return false;
  }
  ctx.mem.WriteCelValue(out_slot, cv);
  return true;
}

// Marshal a `cel::Value` returned by Layer 1 (ReadField / At / Get)
// into the 24-byte CelValue at `out_slot`.  Scalars + null + error
// encode inline / via arena (`EncodeValue`); aggregate kinds intern
// via `EncodeAggregateIfAny`.  Used by every Layer-2 trampoline so
// the wire shape is consistent across surfaces.
absl::Status EncodeFieldResult(const cel::Value& v, uint32_t out_slot,
                               const TrampolineContext& ctx) {
  auto encoded_or = EncodeAggregateIfAny(v, out_slot, ctx);
  if (!encoded_or.ok()) return encoded_or.status();
  if (*encoded_or) return absl::OkStatus();
  CelValue cv{};
  if (auto s = EncodeValue(v, &cv, ctx.alloc); !s.ok()) return s;
  ctx.mem.WriteCelValue(out_slot, cv);
  return absl::OkStatus();
}

}  // namespace

absl::Status CelListAtImpl(uint32_t out_slot, uint32_t list_slot,
                           uint32_t index_slot, const TrampolineContext& ctx) {
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);
  CelValue idx_cv = ctx.mem.ReadCelValue(index_slot);

  // 3VL on operands — same path the runtime dispatcher uses.  Index
  // first so an unknown index propagates even if the list is also
  // poisoned (matches the runtime fast-path order in
  // cel_list_at_arena).
  if (idx_cv.kind == CEL_UNKNOWN || idx_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, idx_cv);
    return absl::OkStatus();
  }
  if (list_cv.kind == CEL_UNKNOWN || list_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, list_cv);
    return absl::OkStatus();
  }

  // Codegen calls into us only on the kHost arm; defence-in-depth.
  if (list_cv.kind != CEL_LIST_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  // langdef §"Indexing": list indices are int only; checker rejects
  // uint upstream.  Defend in depth.
  if (idx_cv.kind != CEL_INT) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }

  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListAtImpl: list ref_slot ", list_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }

  const int64_t i = idx_cv.payload.i;
  if (i < 0) {
    WriteWireError(CEL_ERR_INDEX_OUT_OF_BOUNDS, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  // backing->At returns a Value::Error(kIndexOutOfBounds) when i >=
  // Size; the encoder maps that to CEL_ERR_INDEX_OUT_OF_BOUNDS via
  // WireErrorCode.  Single round-trip, no host-side double-check.
  auto got = backing->At(static_cast<size_t>(i), cel::CelType::Int());
  if (!got.ok()) return got.status();
  return EncodeFieldResult(*got, out_slot, ctx);
}

absl::Status CelMapLookupImpl(uint32_t out_slot, uint32_t map_slot,
                              uint32_t key_slot, const TrampolineContext& ctx) {
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  CelValue key_cv = ctx.mem.ReadCelValue(key_slot);

  // 3VL on operands — same path the runtime dispatcher uses.
  if (key_cv.kind == CEL_UNKNOWN || key_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, key_cv);
    return absl::OkStatus();
  }
  if (map_cv.kind == CEL_UNKNOWN || map_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, map_cv);
    return absl::OkStatus();
  }

  // Codegen calls into us only on the kHost arm; defence-in-depth
  // for codegen drift.
  if (map_cv.kind != CEL_MAP_HOST) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }

  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapLookupImpl: map ref_slot ", map_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }

  std::optional<cel::Value> key = DecodeKey(key_cv, ctx.mem);
  if (!key.has_value()) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }

  // expected_value_type is informational at M3 (no implicit
  // coercion); HostMap ignores it.  Pass an arbitrary scalar — the
  // type catalogue exposes no `Dyn` factory yet, and any choice
  // here is observed only by future backings that opt into typed
  // narrowing.
  auto got = backing->Get(*key, cel::CelType::Int());
  if (!got.ok()) return got.status();
  // EncodeFieldResult handles scalar + every aggregate kind
  // uniformly — nested map/list/message values from Get land
  // through the matching externref namespace.
  return EncodeFieldResult(*got, out_slot, ctx);
}

// ══════════════════════════════════════════════════════════════════
// ProtoMap — proto-reflection over a single map field.
//
// Proto map fields serialise on the wire as `repeated MapEntry`,
// where MapEntry is a synthesized message with fields
// `key=1, value=2` matching the user-declared key/value types.
// `Reflection::FieldSize` + `GetRepeatedMessage(*owner_, field_, i)`
// walk the entries; each entry's reflection lets us read the key
// and value sub-fields with the existing `ReadScalarField` helper.
//
// Linear scan on lookup mirrors the wasm-side `cel_map_lookup_arena`
// semantics — host- and arena-built maps must agree under langdef
// map-key equality so user-visible behaviour is identical.
// ══════════════════════════════════════════════════════════════════

namespace {

// FieldDescriptor for the synthetic key/value sub-field on a proto
// map's entry message type.  Pinned via field number (1=key, 2=value)
// per descriptor.proto.
const google::protobuf::FieldDescriptor* absl_nonnull MapEntryField(
    const google::protobuf::FieldDescriptor& field, int number) {
  const google::protobuf::Descriptor* entry = field.message_type();
  ABSL_CHECK(entry != nullptr)
      << "ProtoMap: map field `" << field.name()
      << "` has no entry message_type — invariant violation";
  const google::protobuf::FieldDescriptor* sub =
      entry->FindFieldByNumber(number);
  ABSL_CHECK(sub != nullptr)
      << "ProtoMap: entry of `" << field.name() << "` missing field " << number;
  return sub;
}

}  // namespace

ProtoMap::ProtoMap(const google::protobuf::Message* absl_nonnull owner,
                   const google::protobuf::FieldDescriptor* absl_nonnull field)
    : owner_(owner), field_(field) {
  ABSL_CHECK(field->is_map())
      << "ProtoMap: field `" << field->name() << "` is not a map";
}

size_t ProtoMap::Size() const {
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  ABSL_CHECK(refl != nullptr) << "ProtoMap::Size: no reflection";
  return static_cast<size_t>(refl->FieldSize(*owner_, field_));
}

absl::StatusOr<cel::Value> ProtoMap::Get(
    const cel::Value& key, const cel::CelType& /*expected_value_type*/) const {
  if (!IsValidMapKeyKind(key.kind())) {
    return KeyTypeMismatch();
  }
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  if (refl == nullptr) {
    return absl::InternalError("ProtoMap::Get: no reflection");
  }
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(*field_, 1);
  const google::protobuf::FieldDescriptor* val_fd = MapEntryField(*field_, 2);
  const int n = refl->FieldSize(*owner_, field_);
  for (int i = 0; i < n; ++i) {
    const google::protobuf::Message& entry =
        refl->GetRepeatedMessage(*owner_, field_, i);
    auto k_or = ReadScalarField(entry, *key_fd);
    if (!k_or.ok()) return k_or.status();
    if (MapKeysEqual(*k_or, key)) {
      return ReadScalarField(entry, *val_fd);
    }
  }
  return NoSuchKey();
}

bool ProtoMap::ContainsKey(const cel::Value& key) const {
  if (!IsValidMapKeyKind(key.kind())) return false;
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  if (refl == nullptr) return false;
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(*field_, 1);
  const int n = refl->FieldSize(*owner_, field_);
  for (int i = 0; i < n; ++i) {
    const google::protobuf::Message& entry =
        refl->GetRepeatedMessage(*owner_, field_, i);
    auto k_or = ReadScalarField(entry, *key_fd);
    if (!k_or.ok()) return false;
    if (MapKeysEqual(*k_or, key)) return true;
  }
  return false;
}

void ProtoMap::ForEach(
    absl::FunctionRef<void(const cel::Value&, const cel::Value&)> visit) const {
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  ABSL_CHECK(refl != nullptr) << "ProtoMap::ForEach: no reflection";
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(*field_, 1);
  const google::protobuf::FieldDescriptor* val_fd = MapEntryField(*field_, 2);
  const int n = refl->FieldSize(*owner_, field_);
  for (int i = 0; i < n; ++i) {
    const google::protobuf::Message& entry =
        refl->GetRepeatedMessage(*owner_, field_, i);
    auto k_or = ReadScalarField(entry, *key_fd);
    auto v_or = ReadScalarField(entry, *val_fd);
    if (!k_or.ok() || !v_or.ok()) continue;
    visit(*k_or, *v_or);
  }
}

// ══════════════════════════════════════════════════════════════════
// HostList — vector-backed `HostListBacking` for user bindings.
// ══════════════════════════════════════════════════════════════════

namespace {

cel::Value IndexOutOfBounds(size_t index, size_t count) {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/cel::ErrorCode::kIndexOutOfBounds,
      /*message=*/
      absl::StrCat("index ", index, " out of range [0, ", count, ")"),
      /*expr_id=*/0,
  });
}

}  // namespace

HostList::HostList(std::vector<cel::Value> elements)
    : elements_(std::move(elements)) {}

size_t HostList::Size() const {
  return elements_.size();
}

absl::StatusOr<cel::Value> HostList::At(
    size_t index, const cel::CelType& /*expected_element_type*/) const {
  if (index >= elements_.size()) {
    return IndexOutOfBounds(index, elements_.size());
  }
  return elements_[index];
}

void HostList::ForEach(absl::FunctionRef<void(const cel::Value&)> visit) const {
  for (const cel::Value& v : elements_) {
    visit(v);
  }
}

// ══════════════════════════════════════════════════════════════════
// ProtoList — proto reflection over a single REPEATED (non-map)
// field.  Element reads delegate to `ReadScalarField` against a
// synthesized FieldDescriptor view of the i-th element — proto's
// `GetRepeated{Bool,Int32,…}()` family does the type dispatch.
// ══════════════════════════════════════════════════════════════════

namespace {

// Read the i-th element of a REPEATED field of the given cpp_type
// into a cel::Value.  Mirrors `ReadNumericField` + the
// string/bytes/message branches of `ReadScalarField`, but reads the
// repeated-element accessors instead of the singular ones.  Returns
// non-OK Status on infrastructure failure (no reflection); the
// caller surfaces spec-level errors as Value::Error.
absl::StatusOr<cel::Value> ReadRepeatedElement(
    const google::protobuf::Reflection& refl,
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field, int i) {
  using FD = google::protobuf::FieldDescriptor;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      return cel::Value::Bool(refl.GetRepeatedBool(msg, &field, i));
    case FD::CPPTYPE_INT32:
      return cel::Value::Int(refl.GetRepeatedInt32(msg, &field, i));
    case FD::CPPTYPE_INT64:
      return cel::Value::Int(refl.GetRepeatedInt64(msg, &field, i));
    case FD::CPPTYPE_UINT32:
      return cel::Value::Uint(refl.GetRepeatedUInt32(msg, &field, i));
    case FD::CPPTYPE_UINT64:
      return cel::Value::Uint(refl.GetRepeatedUInt64(msg, &field, i));
    case FD::CPPTYPE_FLOAT:
      return cel::Value::Double(refl.GetRepeatedFloat(msg, &field, i));
    case FD::CPPTYPE_DOUBLE:
      return cel::Value::Double(refl.GetRepeatedDouble(msg, &field, i));
    case FD::CPPTYPE_ENUM:
      return cel::Value::Int(refl.GetRepeatedEnumValue(msg, &field, i));
    case FD::CPPTYPE_STRING: {
      std::string scratch;
      const std::string& s =
          refl.GetRepeatedStringReference(msg, &field, i, &scratch);
      if (field.type() == FD::TYPE_BYTES) {
        return cel::Value::Bytes(std::string(s));
      }
      return cel::Value::String(std::string(s));
    }
    case FD::CPPTYPE_MESSAGE: {
      const google::protobuf::Message& sub =
          refl.GetRepeatedMessage(msg, &field, i);
      return cel::Value::HostMessage(std::make_shared<ProtoBacking>(&sub));
    }
  }
  return absl::InternalError(absl::StrCat("ProtoList::At: unhandled cpp_type ",
                                          static_cast<int>(field.cpp_type()),
                                          " on field `", field.name(), "`"));
}

}  // namespace

ProtoList::ProtoList(
    const google::protobuf::Message* absl_nonnull owner,
    const google::protobuf::FieldDescriptor* absl_nonnull field)
    : owner_(owner), field_(field) {
  ABSL_CHECK(field->is_repeated())
      << "ProtoList: field `" << field->name() << "` is not repeated";
  ABSL_CHECK(!field->is_map()) << "ProtoList: field `" << field->name()
                               << "` is a map; use ProtoMap instead";
}

size_t ProtoList::Size() const {
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  ABSL_CHECK(refl != nullptr) << "ProtoList::Size: no reflection";
  return static_cast<size_t>(refl->FieldSize(*owner_, field_));
}

absl::StatusOr<cel::Value> ProtoList::At(
    size_t index, const cel::CelType& /*expected_element_type*/) const {
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  if (refl == nullptr) {
    return absl::InternalError("ProtoList::At: no reflection");
  }
  const auto count = static_cast<size_t>(refl->FieldSize(*owner_, field_));
  if (index >= count) {
    return IndexOutOfBounds(index, count);
  }
  return ReadRepeatedElement(*refl, *owner_, *field_, static_cast<int>(index));
}

void ProtoList::ForEach(
    absl::FunctionRef<void(const cel::Value&)> visit) const {
  const google::protobuf::Reflection* refl = owner_->GetReflection();
  ABSL_CHECK(refl != nullptr) << "ProtoList::ForEach: no reflection";
  const int n = refl->FieldSize(*owner_, field_);
  for (int i = 0; i < n; ++i) {
    auto v_or = ReadRepeatedElement(*refl, *owner_, *field_, i);
    if (v_or.ok()) visit(*v_or);
  }
}

// ══════════════════════════════════════════════════════════════════
// Layer-2 trampoline bodies — `CelGetFieldImpl` / `CelHasFieldImpl`.
//
// Both share the same prelude:
//   1. read msg_cv from `mem` (must precede any out_slot writes so
//      msg_slot == out_slot aliasing works);
//   2. propagate UNKNOWN / ERROR on the input;
//   3. defence: msg_cv.kind != CEL_MESSAGE → kTypeMismatch;
//   4. resolve field_ref_id → (number, name) via
//      `bindings.field_refs`; OOR / sentinel → kFieldNotFound;
//   5. attribute_id != 0 → consult `bindings.unknown_patterns`;
//      a FULL match → CEL_UNKNOWN(attribute_id);
//   6. dereference externref slot → backing pointer;
//      missing → kHostAdapterError.
// They diverge after that: Get calls `ReadField` and marshals the
// returned `cel::Value` (scalar inline, span via arena, message
// via Intern); Has calls `HasField` and writes a CEL_BOOL.
//
// Non-OK Status only on infrastructure failure that the wasm side
// can't recover from (memory-out-of-range — handled deeper).  All
// spec-level errors travel inside `out_slot` as CEL_ERROR.
// ══════════════════════════════════════════════════════════════════

namespace {

// Resolve `field_ref_id` against the `bindings.field_refs` table.
// Returns nullptr on OOR / sentinel; caller writes
// CEL_ERR_FIELD_NOT_FOUND.
const FieldRefEntry* absl_nullable ResolveFieldRef(
    const CelHostBindings& bindings, uint32_t field_ref_id) {
  if (field_ref_id == 0) return nullptr;  // sentinel
  if (field_ref_id >= bindings.field_refs.size()) return nullptr;
  return &bindings.field_refs[field_ref_id];
}

// Build an `Attribute` from the AttributeEntry at `attribute_id`.
// AttributeEntry.qualifiers are interned as strings (the CEL paths
// are dotted-only at M2; no array indexing).  Returns nullopt if
// `attribute_id` is the sentinel (0) or OOR.
std::optional<cel::Attribute> ResolveAttribute(const CelHostBindings& bindings,
                                               uint32_t attribute_id) {
  if (attribute_id == 0) return std::nullopt;
  if (attribute_id >= bindings.attributes.size()) return std::nullopt;
  const AttributeEntry& a = bindings.attributes[attribute_id];
  std::vector<cel::AttributeQualifier> path;
  path.reserve(a.qualifiers.size());
  for (const std::string& q : a.qualifiers) {
    path.push_back(cel::AttributeQualifier::OfString(q));
  }
  return cel::Attribute(a.root_variable, std::move(path));
}

// Build the effective attribute for the kSelect being evaluated:
// the operand's attribute (resolved from `attribute_id`) extended
// by one qualifier — the field name being selected.  This is the
// path the kSelect would write into `cel.abi.attributes[]` if every
// node interned its own; M2 only interns the operand and lets the
// trampoline append the leaf qualifier here so the pattern matcher
// sees the full path the user wrote (`c.name`, not just `c`).
cel::Attribute EffectiveSelectAttribute(const cel::Attribute& operand_attr,
                                        absl::string_view field_name) {
  std::vector<cel::AttributeQualifier> path(
      operand_attr.qualifier_path().begin(),
      operand_attr.qualifier_path().end());
  path.push_back(cel::AttributeQualifier::OfString(std::string(field_name)));
  return {std::string(operand_attr.variable_name()), std::move(path)};
}

// Returns true iff any pattern in `unknown_patterns` `kFull`-matches
// the *effective* attribute (operand ⊕ field name).  `kPartial`
// means a sub-attribute is unknown but THIS one isn't; we fall
// through and read.  `kFull` means the pattern covers this
// attribute, so it's opaque to PartialEval and we surface UNKNOWN.
bool MatchesAnyUnknownPattern(const CelHostBindings& bindings,
                              uint32_t attribute_id,
                              absl::string_view field_name) {
  if (attribute_id == 0) return false;
  if (bindings.unknown_patterns.empty()) return false;
  auto attr = ResolveAttribute(bindings, attribute_id);
  if (!attr.has_value()) return false;
  const cel::Attribute eff = EffectiveSelectAttribute(*attr, field_name);
  return std::any_of(
      bindings.unknown_patterns.begin(), bindings.unknown_patterns.end(),
      [&eff](const cel::AttributePattern& pat) {
        return pat.IsMatch(eff) == cel::AttributePattern::MatchType::kFull;
      });
}

// Shared prelude for Get / Has.  Returns:
//   - non-OK Status only on infrastructure failure (none today —
//     reads are bounds-checked deeper).
//   - kSentinelHandled = true  → `out_slot` already populated with
//     UNKNOWN / ERROR / wire-error CelValue.  Caller returns OK.
//   - kSentinelHandled = false → resolved a backing + field;
//     populates `*out_backing` + `*out_field` and returns.
struct FieldDispatchPrelude {
  bool sentinel_handled = false;
  const HostMessageBacking* absl_nullable backing = nullptr;
  const FieldRefEntry* absl_nullable field = nullptr;
};

absl::StatusOr<FieldDispatchPrelude> RunFieldPrelude(
    uint32_t out_slot, uint32_t msg_slot, uint32_t field_ref_id,
    uint32_t attribute_id, const TrampolineContext& ctx) {
  CelValue msg_cv = ctx.mem.ReadCelValue(msg_slot);

  // 3VL absorption — same path Get and Has share.
  if (msg_cv.kind == CEL_UNKNOWN || msg_cv.kind == CEL_ERROR) {
    ctx.mem.WriteCelValue(out_slot, msg_cv);
    return FieldDispatchPrelude{/*sentinel_handled=*/true};
  }
  if (msg_cv.kind != CEL_MESSAGE) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return FieldDispatchPrelude{/*sentinel_handled=*/true};
  }

  const FieldRefEntry* field = ResolveFieldRef(ctx.bindings, field_ref_id);
  if (field == nullptr) {
    WriteWireError(CEL_ERR_FIELD_NOT_FOUND, out_slot, ctx.mem);
    return FieldDispatchPrelude{/*sentinel_handled=*/true};
  }

  if (MatchesAnyUnknownPattern(ctx.bindings, attribute_id, field->field_name)) {
    CelValue unk{};
    unk.kind = CEL_UNKNOWN;
    unk.payload.unk = attribute_id;
    ctx.mem.WriteCelValue(out_slot, unk);
    return FieldDispatchPrelude{/*sentinel_handled=*/true};
  }

  const HostMessageBacking* backing = ctx.refs.Lookup(msg_cv.payload.msg_slot);
  if (backing == nullptr) {
    WriteWireError(CEL_ERR_HOST_ADAPTER_ERROR, out_slot, ctx.mem);
    return FieldDispatchPrelude{/*sentinel_handled=*/true};
  }

  return FieldDispatchPrelude{/*sentinel_handled=*/false, backing, field};
}

}  // namespace

absl::Status CelGetFieldImpl(uint32_t out_slot, uint32_t msg_slot,
                             uint32_t field_ref_id, uint32_t attribute_id,
                             const TrampolineContext& ctx) {
  auto prelude_or =
      RunFieldPrelude(out_slot, msg_slot, field_ref_id, attribute_id, ctx);
  if (!prelude_or.ok()) return prelude_or.status();
  if (prelude_or->sentinel_handled) return absl::OkStatus();

  // expected_type informational at M2 — ProtoBacking dispatches
  // on descriptor cpp_type, not on this hint.  Pass an arbitrary
  // scalar; real plumb-through arrives with the typed-narrowing
  // milestone.
  auto v_or = prelude_or->backing->ReadField(prelude_or->field->field_number,
                                             prelude_or->field->field_name,
                                             cel::CelType::Int());
  if (!v_or.ok()) return v_or.status();
  return EncodeFieldResult(*v_or, out_slot, ctx);
}

absl::Status CelHasFieldImpl(uint32_t out_slot, uint32_t msg_slot,
                             uint32_t field_ref_id, uint32_t attribute_id,
                             const TrampolineContext& ctx) {
  auto prelude_or =
      RunFieldPrelude(out_slot, msg_slot, field_ref_id, attribute_id, ctx);
  if (!prelude_or.ok()) return prelude_or.status();
  if (prelude_or->sentinel_handled) return absl::OkStatus();

  const bool present = prelude_or->backing->HasField(
      prelude_or->field->field_number, prelude_or->field->field_name);
  CelValue out{};
  out.kind = CEL_BOOL;
  out.payload.b = present ? 1 : 0;
  ctx.mem.WriteCelValue(out_slot, out);
  return absl::OkStatus();
}

// ══════════════════════════════════════════════════════════════════
// Aggregate-op kHost trampolines.
//
// The seven dispatchers in `cel_runtime.c` (`cel_list_size` /
// `cel_list_in` / `cel_list_eq` / `cel_list_concat` / `cel_map_size`
// / `cel_map_in` / `cel_map_eq`) tail-call here when the operand
// origin is `CEL_LIST_HOST` or `CEL_MAP_HOST`.  Each Impl reads its
// operand backing(s) via `ctx.refs.LookupList` / `LookupMap`, runs
// the corresponding spec-level operation, and writes the result
// CelValue into `out_slot`.  Element/value equality reuses a
// scalar-only matcher (`HostScalarValueEq`) consistent with the
// arena fast paths in `cel_runtime.c::cel_value_eq` —
// nested-aggregate equality (lists of lists, maps of messages, …)
// returns false here for now; see
// `doc/implementation-plan/rewrite/cel-host-surface.md` for the
// scope boundary.
// ══════════════════════════════════════════════════════════════════

namespace {

// Scalar-equality matcher mirroring `cel_runtime.c::cel_value_eq`
// + `map_keys_equal`: cross-type numeric per langdef §"Equality"
// for int/uint/double; structural for bool / string / bytes / null.
// Aggregate kinds (CEL_LIST_*, CEL_MAP_*, CEL_MESSAGE) return false —
// the host arms only need scalar equality for `in` / `eq` element
// matching.  Span operands ReadSpan via the MemoryView since both
// arena spans (literal-built) and encoded backing-element spans
// (allocated via the trampoline's ArenaAllocator) live in linear
// memory.
bool HostScalarSpanEq(const CelValue& a, const CelValue& b,
                      const MemoryView& mem) {
  if (a.payload.s.len != b.payload.s.len) return false;
  absl::string_view sa = mem.ReadSpan(a.payload.s.ptr, a.payload.s.len);
  absl::string_view sb = mem.ReadSpan(b.payload.s.ptr, b.payload.s.len);
  return sa == sb;
}

bool HostScalarSameKindEq(const CelValue& a, const CelValue& b,
                          const MemoryView& mem) {
  switch (a.kind) {
    case CEL_NULL:
      return true;
    case CEL_BOOL:
      return a.payload.b == b.payload.b;
    case CEL_INT:
      return a.payload.i == b.payload.i;
    case CEL_UINT:
      return a.payload.u == b.payload.u;
    case CEL_DOUBLE:
      return a.payload.d == b.payload.d;
    case CEL_STRING:
    case CEL_BYTES:
      return HostScalarSpanEq(a, b, mem);
    default:
      return false;
  }
}

bool HostNumericCrossEq(const CelValue& a, const CelValue& b) {
  // langdef §"Equality": int/uint/double compare by mathematical
  // value across the type ladder.  Unrepresentable cross-type
  // (e.g. int<0 vs uint) → false.
  auto get_d = [](const CelValue& v, double* out) {
    switch (v.kind) {
      case CEL_INT:
        *out = static_cast<double>(v.payload.i);
        return true;
      case CEL_UINT:
        *out = static_cast<double>(v.payload.u);
        return true;
      case CEL_DOUBLE:
        *out = v.payload.d;
        return true;
      default:
        return false;
    }
  };
  double da = 0;
  double db = 0;
  if (!get_d(a, &da) || !get_d(b, &db)) return false;
  return da == db;
}

bool HostScalarValueEq(const CelValue& a, const CelValue& b,
                       const MemoryView& mem) {
  if (a.kind == b.kind) return HostScalarSameKindEq(a, b, mem);
  return HostNumericCrossEq(a, b);
}

// Read the i-th element of a CEL_LIST_ARENA via the MemoryView.
// `cv` must be CEL_LIST_ARENA; caller has verified.
CelValue ReadArenaListElement(const CelValue& cv, uint32_t i,
                              const MemoryView& mem) {
  ArenaListHeader hdr{};
  // ReadCelValue is also what reads ArenaListHeader-shaped runs —
  // it's just memcpy(24) which truncates to the 16-byte header.
  // Use a typed memcpy through ReadSpan for clarity.
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_list.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  return mem.ReadCelValue(hdr.elements_offset +
                          (i * static_cast<uint32_t>(kCelListEntryStride)));
}

uint32_t ReadArenaListCount(const CelValue& cv, const MemoryView& mem) {
  ArenaListHeader hdr{};
  absl::string_view hdr_bytes =
      mem.ReadSpan(cv.payload.arena_list.header_ptr, sizeof(hdr));
  std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
  return hdr.count;
}

// Encode a backing-returned cel::Value into a CelValue.  Aggregate
// returns POISON since aggregate element equality is out of scope
// here (mirrors arena fast path).
absl::StatusOr<CelValue> EncodeBackingScalar(const cel::Value& v,
                                             ArenaAllocator& alloc) {
  using K = cel::Value::Kind;
  if (v.kind() == K::kMessage || v.kind() == K::kMap || v.kind() == K::kList) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    return err;
  }
  CelValue cv{};
  if (auto s = EncodeValue(v, &cv, alloc); !s.ok()) return s;
  return cv;
}

// Write CEL_BOOL into `out_slot`.  Used by every comparison Impl.
void WriteWireBool(bool v, uint32_t out_slot, MemoryView& mem) {
  CelValue cv{};
  cv.kind = CEL_BOOL;
  cv.payload.b = v ? 1 : 0;
  mem.WriteCelValue(out_slot, cv);
}

// Write CEL_INT into `out_slot`.
void WriteWireInt(int64_t v, uint32_t out_slot, MemoryView& mem) {
  CelValue cv{};
  cv.kind = CEL_INT;
  cv.payload.i = v;
  mem.WriteCelValue(out_slot, cv);
}

// 3VL absorption shared by every kHost Impl: if either operand is
// UNKNOWN / ERROR, write it through and return true (skip work).
// Mirrors `cel_runtime.c::absorb_3vl_binary` plus `_unary`.
bool AbsorbUnary(const CelValue& a, uint32_t out_slot, MemoryView& mem) {
  if (a.kind == CEL_UNKNOWN || a.kind == CEL_ERROR) {
    mem.WriteCelValue(out_slot, a);
    return true;
  }
  return false;
}

bool AbsorbBinary(const CelValue& a, const CelValue& b, uint32_t out_slot,
                  MemoryView& mem) {
  if (a.kind == CEL_UNKNOWN || a.kind == CEL_ERROR) {
    mem.WriteCelValue(out_slot, a);
    return true;
  }
  if (b.kind == CEL_UNKNOWN || b.kind == CEL_ERROR) {
    mem.WriteCelValue(out_slot, b);
    return true;
  }
  return false;
}

}  // namespace

absl::Status CelListSizeImpl(uint32_t out_slot, uint32_t list_slot,
                             const TrampolineContext& ctx) {
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);
  if (AbsorbUnary(list_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (list_cv.kind != CEL_LIST_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListSizeImpl: list ref_slot ",
                     list_cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  WriteWireInt(static_cast<int64_t>(backing->Size()), out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelListInImpl(uint32_t out_slot, uint32_t value_slot,
                           uint32_t list_slot, const TrampolineContext& ctx) {
  CelValue value_cv = ctx.mem.ReadCelValue(value_slot);
  CelValue list_cv = ctx.mem.ReadCelValue(list_slot);
  if (AbsorbBinary(value_cv, list_cv, out_slot, ctx.mem)) {
    return absl::OkStatus();
  }
  if (list_cv.kind != CEL_LIST_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostListBacking* backing =
      ctx.refs.LookupList(list_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelListInImpl: list ref_slot ", list_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }
  const size_t n = backing->Size();
  for (size_t i = 0; i < n; ++i) {
    auto got = backing->At(i, cel::CelType::Int());
    if (!got.ok()) return got.status();
    auto enc_or = EncodeBackingScalar(*got, ctx.alloc);
    if (!enc_or.ok()) return enc_or.status();
    if (HostScalarValueEq(*enc_or, value_cv, ctx.mem)) {
      WriteWireBool(true, out_slot, ctx.mem);
      return absl::OkStatus();
    }
  }
  WriteWireBool(false, out_slot, ctx.mem);
  return absl::OkStatus();
}

namespace {

// Returns the count of `cv` whether arena or host.  Caller has
// already verified kind ∈ {CEL_LIST_ARENA, CEL_LIST_HOST}.
absl::StatusOr<size_t> ListLength(const CelValue& cv,
                                  const TrampolineContext& ctx) {
  if (cv.kind == CEL_LIST_ARENA) {
    return static_cast<size_t>(ReadArenaListCount(cv, ctx.mem));
  }
  const HostListBacking* backing = ctx.refs.LookupList(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "list ref_slot ", cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  return backing->Size();
}

absl::StatusOr<CelValue> ReadListElementAt(const CelValue& cv, size_t i,
                                           const TrampolineContext& ctx) {
  if (cv.kind == CEL_LIST_ARENA) {
    return ReadArenaListElement(cv, static_cast<uint32_t>(i), ctx.mem);
  }
  const HostListBacking* backing = ctx.refs.LookupList(cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "list ref_slot ", cv.payload.ref_slot, " not found in ExternrefTable"));
  }
  auto got = backing->At(i, cel::CelType::Int());
  if (!got.ok()) return got.status();
  return EncodeBackingScalar(*got, ctx.alloc);
}

}  // namespace

namespace {

// Element-wise equality walk for two list operands of any origin
// pair (arena+arena, arena+host, host+host).  Caller has already
// verified both kinds are list-shaped and verified equal lengths.
// Returns OkStatus + `*equal` set; non-OK Status only on
// infrastructure failure (bad ref_slot, backing->At error).
absl::Status WalkListEq(const CelValue& a_cv, const CelValue& b_cv, size_t n,
                        const TrampolineContext& ctx, bool* equal) {
  for (size_t i = 0; i < n; ++i) {
    auto ea_or = ReadListElementAt(a_cv, i, ctx);
    if (!ea_or.ok()) return ea_or.status();
    auto eb_or = ReadListElementAt(b_cv, i, ctx);
    if (!eb_or.ok()) return eb_or.status();
    if (!HostScalarValueEq(*ea_or, *eb_or, ctx.mem)) {
      *equal = false;
      return absl::OkStatus();
    }
  }
  *equal = true;
  return absl::OkStatus();
}

}  // namespace

absl::Status CelListEqImpl(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot,
                           const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  const bool a_ok = (a_cv.kind == CEL_LIST_ARENA || a_cv.kind == CEL_LIST_HOST);
  const bool b_ok = (b_cv.kind == CEL_LIST_ARENA || b_cv.kind == CEL_LIST_HOST);
  if (!a_ok || !b_ok) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  auto na_or = ListLength(a_cv, ctx);
  if (!na_or.ok()) return na_or.status();
  auto nb_or = ListLength(b_cv, ctx);
  if (!nb_or.ok()) return nb_or.status();
  if (*na_or != *nb_or) {
    WriteWireBool(false, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  bool equal = true;
  if (auto s = WalkListEq(a_cv, b_cv, *na_or, ctx, &equal); !s.ok()) return s;
  WriteWireBool(equal, out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelListConcatImpl(uint32_t out_slot, uint32_t a_slot,
                               uint32_t b_slot, const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  // ── Materialisation strategy (DESIGN, follow-up impl) ────────
  //
  // The shipping behaviour for mixed-origin / both-host list
  // concat is to MATERIALISE the host operand(s) into the arena
  // and then run the arena+arena fast path.  Concretely:
  //
  //   1. Allocate a fresh ArenaListHeader + elements run via
  //      `arena_alloc`, sized `a_size + b_size`.  ArenaAllocator's
  //      `Alloc` already reenters wasm for `arena_alloc`, so this
  //      works from inside a host trampoline.
  //   2. For each operand:
  //        - If CEL_LIST_ARENA: memcpy the elements run into the
  //          new run at the right offset.
  //        - If CEL_LIST_HOST: walk `backing->ForEach`, encode each
  //          `cel::Value` into a CelValue (via `EncodeBackingScalar`
  //          extended for aggregates — pending work item), and
  //          write into the destination run.
  //   3. Write `{kind:CEL_LIST_ARENA, arena_list.header_ptr=hdr_off}`
  //      into `out_slot`.  The result is observably an arena list,
  //      which keeps downstream codegen on the fast path.
  //
  // This same strategy applies to mixed-origin map equality (see
  // CelMapEqImpl) and to any future operator that needs to walk
  // both operands as one origin: lift host into arena, then run
  // the arena fast path.  Documented in
  // `doc/implementation-plan/rewrite/m5-kcall-comprehensions.md`
  // §"Cross-origin materialisation" and
  // `doc/implementation-plan/rewrite/map-list-dispatch.md` §6.
  //
  // Current ship state: nested-aggregate elements + the re-entrant
  // arena allocation aren't fully exercised yet, so mixed-origin
  // concat POISONs with TYPE_MISMATCH; follow-up work flips this to
  // actual materialisation.
  WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelMapSizeImpl(uint32_t out_slot, uint32_t map_slot,
                            const TrampolineContext& ctx) {
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  if (AbsorbUnary(map_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (map_cv.kind != CEL_MAP_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapSizeImpl: map ref_slot ", map_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }
  WriteWireInt(static_cast<int64_t>(backing->Size()), out_slot, ctx.mem);
  return absl::OkStatus();
}

absl::Status CelMapInImpl(uint32_t out_slot, uint32_t key_slot,
                          uint32_t map_slot, const TrampolineContext& ctx) {
  CelValue key_cv = ctx.mem.ReadCelValue(key_slot);
  CelValue map_cv = ctx.mem.ReadCelValue(map_slot);
  if (AbsorbBinary(key_cv, map_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (map_cv.kind != CEL_MAP_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostMapBacking* backing = ctx.refs.LookupMap(map_cv.payload.ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelMapInImpl: map ref_slot ", map_cv.payload.ref_slot,
                     " not found in ExternrefTable"));
  }
  std::optional<cel::Value> key = DecodeKey(key_cv, ctx.mem);
  if (!key.has_value()) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  WriteWireBool(backing->ContainsKey(*key), out_slot, ctx.mem);
  return absl::OkStatus();
}

namespace {

// Set-equality walk (langdef §"Equality" — map order is irrelevant).
// For each entry in `a`, look up the same key in `b` and compare
// values; any miss → unequal.  Both backings already verified
// non-null and same Size().  `*equal` ends up false on any miss
// or value mismatch; non-OK Status only on infrastructure failure.
absl::Status WalkMapEq(const HostMapBacking& a, const HostMapBacking& b,
                       const TrampolineContext& ctx, bool* equal) {
  *equal = true;
  absl::Status work_status = absl::OkStatus();
  a.ForEach([&](const cel::Value& k, const cel::Value& va) {
    if (!*equal || !work_status.ok()) return;
    if (!b.ContainsKey(k)) {
      *equal = false;
      return;
    }
    auto vb_or = b.Get(k, cel::CelType::Int());
    if (!vb_or.ok()) {
      work_status = vb_or.status();
      return;
    }
    auto enc_va_or = EncodeBackingScalar(va, ctx.alloc);
    if (!enc_va_or.ok()) {
      work_status = enc_va_or.status();
      return;
    }
    auto enc_vb_or = EncodeBackingScalar(*vb_or, ctx.alloc);
    if (!enc_vb_or.ok()) {
      work_status = enc_vb_or.status();
      return;
    }
    if (!HostScalarValueEq(*enc_va_or, *enc_vb_or, ctx.mem)) {
      *equal = false;
    }
  });
  return work_status;
}

}  // namespace

absl::Status CelMapEqImpl(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot,
                          const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  // Current ship state: only both-host map equality is supported
  // (mixed origins → TYPE_MISMATCH).  Same-arena routes through the
  // dispatcher's arena fast path.  The shipping strategy for
  // arena↔host pairs is to MATERIALISE the host operand into the
  // arena (lift via ForEach + EncodeBackingScalar + arena_alloc) and
  // then run the arena+arena equality walk — same lift-then-walk
  // pattern documented in CelListConcatImpl and described in
  // `rewrite/m5-kcall-comprehensions.md` §"Cross-origin materialisation".
  if (a_cv.kind != CEL_MAP_HOST || b_cv.kind != CEL_MAP_HOST) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostMapBacking* a_backing = ctx.refs.LookupMap(a_cv.payload.ref_slot);
  const HostMapBacking* b_backing = ctx.refs.LookupMap(b_cv.payload.ref_slot);
  if (a_backing == nullptr || b_backing == nullptr) {
    return absl::FailedPreconditionError(
        "CelMapEqImpl: map ref_slot not found in ExternrefTable");
  }
  if (a_backing->Size() != b_backing->Size()) {
    WriteWireBool(false, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  bool equal = true;
  if (auto s = WalkMapEq(*a_backing, *b_backing, ctx, &equal); !s.ok()) {
    return s;
  }
  WriteWireBool(equal, out_slot, ctx.mem);
  return absl::OkStatus();
}

// If `m` is a google.protobuf.Any, unpack its type_url + value
// against the Any descriptor's own pool and return a fresh
// typed-message clone (stashed in `owner` so the caller can keep it
// alive).  Returns the original `m` when it's not an Any; returns
// nullptr on Any-unpack failure (malformed type_url / unknown FQN /
// corrupt bytes).  Used by CelMessageEqImpl to compare an Any
// against either a typed message or another Any uniformly.
static const google::protobuf::Message* absl_nullable PeelAnyForEq(
    const google::protobuf::Message* m,
    std::unique_ptr<google::protobuf::Message>& owner) {
  if (m->GetDescriptor() == nullptr ||
      m->GetDescriptor()->full_name() != "google.protobuf.Any") {
    return m;
  }
  cel::Value peeled = UnpackAnyToValue(*m, m->GetDescriptor()->file()->pool());
  if (peeled.kind() != cel::Value::Kind::kMessage) return nullptr;
  auto backing_or = peeled.MessageBacking();
  if (!backing_or.ok() || (*backing_or)->message() == nullptr) return nullptr;
  const google::protobuf::Message* src = (*backing_or)->message();
  owner.reset(src->New());
  owner->CopyFrom(*src);
  return owner.get();
}

absl::Status CelMessageEqImpl(uint32_t out_slot, uint32_t a_slot,
                              uint32_t b_slot, const TrampolineContext& ctx) {
  CelValue a_cv = ctx.mem.ReadCelValue(a_slot);
  CelValue b_cv = ctx.mem.ReadCelValue(b_slot);
  if (AbsorbBinary(a_cv, b_cv, out_slot, ctx.mem)) return absl::OkStatus();
  if (a_cv.kind != CEL_MESSAGE || b_cv.kind != CEL_MESSAGE) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const HostMessageBacking* a_backing = ctx.refs.Lookup(a_cv.payload.msg_slot);
  const HostMessageBacking* b_backing = ctx.refs.Lookup(b_cv.payload.msg_slot);
  if (a_backing == nullptr || b_backing == nullptr) {
    return absl::FailedPreconditionError(
        "CelMessageEqImpl: message msg_slot not found in ExternrefTable");
  }
  // HostMessageBacking exposes its underlying Message* via the
  // virtual `message()` so both `ProtoBacking` (host-bound) and
  // `OwnedProtoBacking` (proto-literal-built) participate uniformly.
  // Custom non-proto backings return nullptr from `message()` and
  // surface kTypeMismatch (proto-vs-non-proto eq is a spec error per
  // langdef §"Equality").
  const google::protobuf::Message* a_msg = a_backing->message();
  const google::protobuf::Message* b_msg = b_backing->message();
  if (a_msg == nullptr || b_msg == nullptr) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  // Peel either operand if it's a google.protobuf.Any (typical
  // shape: a direct `Any{...}` literal that didn't pass through
  // ProtoBacking::ReadField's unwrap arm).  The peeled owners live
  // for the duration of this call.
  std::unique_ptr<google::protobuf::Message> a_owner;
  std::unique_ptr<google::protobuf::Message> b_owner;
  const google::protobuf::Message* a_cmp = PeelAnyForEq(a_msg, a_owner);
  const google::protobuf::Message* b_cmp = PeelAnyForEq(b_msg, b_owner);
  if (a_cmp == nullptr || b_cmp == nullptr) {
    // Any with malformed type_url / unknown FQN / corrupt bytes —
    // equality is undefined; surface as Error.
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  if (a_cmp->GetDescriptor() != b_cmp->GetDescriptor()) {
    // Cross-descriptor mismatch after peel → unequal, not error.
    WriteWireBool(false, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const bool eq =
      google::protobuf::util::MessageDifferencer::Equals(*a_cmp, *b_cmp);
  WriteWireBool(eq, out_slot, ctx.mem);
  return absl::OkStatus();
}

// ══════════════════════════════════════════════════════════════════
// `cel_host.cel_make_message(type_id, out_slot)`.
//
// Resolves type_id → Descriptor* against the per-Plan
// `bindings.message_types` lookup (populated from `cel.abi.types[]`
// at `Engine::Plan` time).  Allocates a default-constructed proto
// via `MessageFactory::generated_factory()->GetPrototype(desc)
// ->New()`, wraps in `OwnedProtoBacking`, interns into the
// ExternrefTable, and writes a CEL_MESSAGE CelValue with the
// interned slot to `out_slot`.
//
// Spec-level errors (sentinel id, OOR, unknown FQN, prototype-
// missing) write CEL_ERROR to out_slot and return OK; non-OK
// Status is reserved for true infrastructure failures (none today
// — the lookup table is bounds-checked, descriptor null surfaces
// as a clean error).
// ══════════════════════════════════════════════════════════════════

absl::Status CelMakeMessageImpl(uint32_t type_id, uint32_t out_slot,
                                const TrampolineContext& ctx) {
  if (type_id == 0 || type_id >= ctx.bindings.message_types.size()) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const MessageTypeEntry& entry = ctx.bindings.message_types[type_id];
  if (entry.descriptor == nullptr) {
    // FQN was not resolvable against the pool at Plan time — treat
    // as a spec-level type error rather than crashing the eval.
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(
          entry.descriptor);
  if (prototype == nullptr) {
    // Generated factory doesn't know about this descriptor — most
    // likely a dynamic descriptor loaded via SchemaProtoSource.
    // Dynamic-descriptor support is a follow-up tied to the
    // conformance harness's descriptor mode.  Surface as a clean
    // spec error.
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  if (msg == nullptr) {
    return absl::ResourceExhaustedError(
        "CelMakeMessageImpl: prototype->New() returned null");
  }
  auto backing = std::make_shared<OwnedProtoBacking>(std::move(msg));
  const uint32_t slot = ctx.refs.Intern(std::move(backing));
  CelValue out{};
  out.kind = CEL_MESSAGE;
  out.payload.msg_slot = slot;
  ctx.mem.WriteCelValue(out_slot, out);
  return absl::OkStatus();
}

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
  auto mismatch = [&](absl::string_view expected) {
    return absl::InvalidArgumentError(absl::StrCat(
        "SetWrapperInnerValue: `", wrapper_desc.full_name(), "` expects ",
        expected, " but value kind is ", static_cast<int>(value.kind)));
  };
  switch (vf.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      if (value.kind != CEL_BOOL) return mismatch("CEL_BOOL");
      wr.SetBool(&wrapper, &vf, value.payload.b != 0);
      return absl::OkStatus();
    case FD::CPPTYPE_INT32:
      if (value.kind != CEL_INT) return mismatch("CEL_INT");
      wr.SetInt32(&wrapper, &vf, static_cast<int32_t>(ReadInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_INT64:
      if (value.kind != CEL_INT) return mismatch("CEL_INT");
      wr.SetInt64(&wrapper, &vf, ReadInt64(value));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT32:
      if (value.kind != CEL_UINT) return mismatch("CEL_UINT");
      wr.SetUInt32(&wrapper, &vf, static_cast<uint32_t>(ReadUInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT64:
      if (value.kind != CEL_UINT) return mismatch("CEL_UINT");
      wr.SetUInt64(&wrapper, &vf, ReadUInt64(value));
      return absl::OkStatus();
    case FD::CPPTYPE_FLOAT:
      if (value.kind != CEL_DOUBLE) return mismatch("CEL_DOUBLE");
      wr.SetFloat(&wrapper, &vf, static_cast<float>(ReadDouble(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_DOUBLE:
      if (value.kind != CEL_DOUBLE) return mismatch("CEL_DOUBLE");
      wr.SetDouble(&wrapper, &vf, ReadDouble(value));
      return absl::OkStatus();
    case FD::CPPTYPE_STRING:
      if (vf.type() == FD::TYPE_BYTES) {
        if (value.kind != CEL_BYTES) return mismatch("CEL_BYTES");
      } else if (value.kind != CEL_STRING) {
        return mismatch("CEL_STRING");
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
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      if (value.kind != CEL_BOOL) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: field `", field.name(),
            "` is BOOL but value kind is ", static_cast<int>(value.kind)));
      }
      refl->SetBool(&msg, &field, value.payload.b != 0);
      return absl::OkStatus();
    case FD::CPPTYPE_INT32:
      if (value.kind != CEL_INT) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: field `", field.name(),
            "` is INT32 but value kind is ", static_cast<int>(value.kind)));
      }
      refl->SetInt32(&msg, &field, static_cast<int32_t>(ReadInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_INT64:
      if (value.kind != CEL_INT) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: field `", field.name(),
            "` is INT64 but value kind is ", static_cast<int>(value.kind)));
      }
      refl->SetInt64(&msg, &field, ReadInt64(value));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT32:
      if (value.kind != CEL_UINT) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: field `", field.name(),
            "` is UINT32 but value kind is ", static_cast<int>(value.kind)));
      }
      refl->SetUInt32(&msg, &field, static_cast<uint32_t>(ReadUInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT64:
      if (value.kind != CEL_UINT) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: field `", field.name(),
            "` is UINT64 but value kind is ", static_cast<int>(value.kind)));
      }
      refl->SetUInt64(&msg, &field, ReadUInt64(value));
      return absl::OkStatus();
    case FD::CPPTYPE_FLOAT:
      if (value.kind != CEL_DOUBLE) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: field `", field.name(),
            "` is FLOAT but value kind is ", static_cast<int>(value.kind)));
      }
      refl->SetFloat(&msg, &field, static_cast<float>(ReadDouble(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_DOUBLE:
      if (value.kind != CEL_DOUBLE) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: field `", field.name(),
            "` is DOUBLE but value kind is ", static_cast<int>(value.kind)));
      }
      refl->SetDouble(&msg, &field, ReadDouble(value));
      return absl::OkStatus();
    case FD::CPPTYPE_STRING:
      // String-type fields accept CEL_STRING; bytes-type fields
      // accept CEL_BYTES.  Both share the same cpp_type slot
      // (CPPTYPE_STRING) — the wire-type distinction comes from
      // `field.type()`, not cpp_type.
      if (field.type() == FD::TYPE_BYTES) {
        if (value.kind != CEL_BYTES) {
          return absl::InvalidArgumentError(absl::StrCat(
              "CelSetFieldImpl: field `", field.name(),
              "` is BYTES but value kind is ", static_cast<int>(value.kind)));
        }
      } else {
        if (value.kind != CEL_STRING) {
          return absl::InvalidArgumentError(absl::StrCat(
              "CelSetFieldImpl: field `", field.name(),
              "` is STRING but value kind is ", static_cast<int>(value.kind)));
        }
      }
      refl->SetString(&msg, &field, ReadSpanString(value, mem));
      return absl::OkStatus();
    case FD::CPPTYPE_ENUM:
      // langdef §"Enumerated Types": enum values are spec-typed as
      // int.  cel-cpp's checker resolves `Foo.SOME_VALUE` to a
      // Constant int64; codegen flows that as CEL_INT to here.
      if (value.kind != CEL_INT) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: field `", field.name(),
            "` is ENUM but value kind is ", static_cast<int>(value.kind)));
      }
      refl->SetEnumValue(&msg, &field, static_cast<int>(ReadInt64(value)));
      return absl::OkStatus();
    case FD::CPPTYPE_MESSAGE: {
      // langdef §"Field Selection" + cel-cpp behaviour: assigning
      // `null` to a singular message field clears it (equivalent
      // to leaving it unset).  `Foo{m: null} == Foo{}` per the
      // conformance corpus's `set_null/*` rows.  For wrapper-typed
      // fields, langdef line 484-486's unset-reads-as-null rule
      // makes this round-trip with the read-side wrapper peel.
      if (value.kind == CEL_NULL) {
        refl->ClearField(&msg, &field);
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
        return SetWrapperFieldFromScalar(*refl, msg, field, *mt, value, mem);
      }
      // Nested singular message — `Foo{nested: Bar{...}}`.  The
      // outer kStructExpr lowering recursively built `Bar{...}` into
      // a fresh OwnedProtoBacking and wrote a CEL_MESSAGE CelValue
      // at value_slot.  Here we resolve that backing and CopyFrom
      // into a freshly-mutable submessage of the outer field — same
      // pattern as repeated-of-message in AppendRepeatedFromCelValue.
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
      google::protobuf::Message* dst = refl->MutableMessage(&msg, &field);
      return WriteMessageOrPack(dst, *src_msg);
    }
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
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      if (cv.kind != CEL_BOOL) {
        return absl::InvalidArgumentError(
            absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                         "` BOOL element kind=", static_cast<int>(cv.kind)));
      }
      refl.AddBool(&msg, &field, cv.payload.b != 0);
      return absl::OkStatus();
    case FD::CPPTYPE_INT32:
      if (cv.kind != CEL_INT) {
        return absl::InvalidArgumentError(
            absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                         "` INT32 element kind=", static_cast<int>(cv.kind)));
      }
      refl.AddInt32(&msg, &field, static_cast<int32_t>(cv.payload.i));
      return absl::OkStatus();
    case FD::CPPTYPE_INT64:
      if (cv.kind != CEL_INT) {
        return absl::InvalidArgumentError(
            absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                         "` INT64 element kind=", static_cast<int>(cv.kind)));
      }
      refl.AddInt64(&msg, &field, cv.payload.i);
      return absl::OkStatus();
    case FD::CPPTYPE_UINT32:
      if (cv.kind != CEL_UINT) {
        return absl::InvalidArgumentError(
            absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                         "` UINT32 element kind=", static_cast<int>(cv.kind)));
      }
      refl.AddUInt32(&msg, &field, static_cast<uint32_t>(cv.payload.u));
      return absl::OkStatus();
    case FD::CPPTYPE_UINT64:
      if (cv.kind != CEL_UINT) {
        return absl::InvalidArgumentError(
            absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                         "` UINT64 element kind=", static_cast<int>(cv.kind)));
      }
      refl.AddUInt64(&msg, &field, cv.payload.u);
      return absl::OkStatus();
    case FD::CPPTYPE_FLOAT:
      if (cv.kind != CEL_DOUBLE) {
        return absl::InvalidArgumentError(
            absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                         "` FLOAT element kind=", static_cast<int>(cv.kind)));
      }
      refl.AddFloat(&msg, &field, static_cast<float>(cv.payload.d));
      return absl::OkStatus();
    case FD::CPPTYPE_DOUBLE:
      if (cv.kind != CEL_DOUBLE) {
        return absl::InvalidArgumentError(
            absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                         "` DOUBLE element kind=", static_cast<int>(cv.kind)));
      }
      refl.AddDouble(&msg, &field, cv.payload.d);
      return absl::OkStatus();
    case FD::CPPTYPE_STRING: {
      const bool want_bytes = field.type() == FD::TYPE_BYTES;
      if (want_bytes ? cv.kind != CEL_BYTES : cv.kind != CEL_STRING) {
        return absl::InvalidArgumentError(absl::StrCat(
            "CelSetFieldImpl: repeated `", field.name(),
            "` STRING/BYTES element kind=", static_cast<int>(cv.kind)));
      }
      refl.AddString(&msg, &field, ReadSpanString(cv, mem));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_ENUM:
      if (cv.kind != CEL_INT) {
        return absl::InvalidArgumentError(
            absl::StrCat("CelSetFieldImpl: repeated `", field.name(),
                         "` ENUM element kind=", static_cast<int>(cv.kind)));
      }
      refl.AddEnumValue(&msg, &field, static_cast<int>(cv.payload.i));
      return absl::OkStatus();
    case FD::CPPTYPE_MESSAGE: {
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
  }
  ABSL_CHECK(false) << "AppendRepeatedFromCelValue: unknown cpp_type "
                    << static_cast<int>(field.cpp_type());
  return absl::InternalError("unreachable");
}

// Same as `AppendRepeatedFromCelValue` but the source element comes
// from a host-list backing as a `cel::Value` (Activation::Bind
// path).  Per-cpp_type dispatch reads via cel::Value's typed
// accessors instead of CelValue payloads + MemoryView.
absl::Status AppendRepeatedFromHostListValue(
    google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor& field,
    const google::protobuf::Reflection& refl, const cel::Value& v) {
  using FD = google::protobuf::FieldDescriptor;
  using K = cel::Value::Kind;
  switch (field.cpp_type()) {
    case FD::CPPTYPE_BOOL: {
      auto b = v.AsBool();
      if (!b.ok()) return b.status();
      refl.AddBool(&msg, &field, *b);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_INT32: {
      auto i = v.AsInt();
      if (!i.ok()) return i.status();
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
      refl.AddUInt32(&msg, &field, static_cast<uint32_t>(*u));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT64: {
      auto u = v.AsUint();
      if (!u.ok()) return u.status();
      refl.AddUInt64(&msg, &field, *u);
      return absl::OkStatus();
    }
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
    case FD::CPPTYPE_STRING: {
      auto s = (field.type() == FD::TYPE_BYTES) ? v.AsBytes() : v.AsString();
      if (!s.ok()) return s.status();
      refl.AddString(&msg, &field, std::string(*s));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_ENUM: {
      auto i = v.AsInt();
      if (!i.ok()) return i.status();
      refl.AddEnumValue(&msg, &field, static_cast<int>(*i));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_MESSAGE: {
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
      google::protobuf::Message* dst = refl.AddMessage(&msg, &field);
      return WriteMessageOrPack(dst, *src_msg);
    }
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
    backing->ForEach([&](const cel::Value& v) {
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
absl::Status InsertArenaMapEntry(google::protobuf::Message& msg,
                                 const google::protobuf::FieldDescriptor& field,
                                 const google::protobuf::Reflection& refl,
                                 const CelValue& key_cv, const CelValue& val_cv,
                                 const MemoryView& mem,
                                 const ExternrefTable& refs) {
  google::protobuf::Message* entry = refl.AddMessage(&msg, &field);
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(field, 1);
  const google::protobuf::FieldDescriptor* val_fd = MapEntryField(field, 2);
  // Recursive scalar-set on the entry submessage.  For a value
  // typed as message we route through a `CopyFrom` via the same
  // CEL_MESSAGE-source path repeated-of-message uses.
  if (auto s = SetScalarField(*entry, *key_fd, key_cv, mem, &refs); !s.ok()) {
    return s;
  }
  if (val_fd->cpp_type() ==
      google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    if (val_cv.kind != CEL_MESSAGE) {
      return absl::InvalidArgumentError(absl::StrCat(
          "CelSetFieldImpl: map<", key_fd->name(), ", message `",
          val_fd->name(), "`> value kind=", static_cast<int>(val_cv.kind)));
    }
    const HostMessageBacking* src = refs.Lookup(val_cv.payload.msg_slot);
    if (src == nullptr || src->message() == nullptr) {
      return absl::InvalidArgumentError(
          "CelSetFieldImpl: map message-value source has no backing");
    }
    google::protobuf::Message* dst =
        entry->GetReflection()->MutableMessage(entry, val_fd);
    return WriteMessageOrPack(dst, *src->message());
  }
  return SetScalarField(*entry, *val_fd, val_cv, mem, &refs);
}

absl::Status InsertHostMapEntry(google::protobuf::Message& msg,
                                const google::protobuf::FieldDescriptor& field,
                                const google::protobuf::Reflection& refl,
                                const cel::Value& key,
                                const cel::Value& value) {
  google::protobuf::Message* entry = refl.AddMessage(&msg, &field);
  const google::protobuf::FieldDescriptor* key_fd = MapEntryField(field, 1);
  const google::protobuf::FieldDescriptor* val_fd = MapEntryField(field, 2);
  // Reuse `AppendRepeatedFromHostListValue`'s host-value dispatch
  // shape: it's cpp_type-keyed and handles every scalar + message
  // arm.  But we need *Set* on a singular sub-field, not *Add* on
  // a repeated one — so route through a small per-cpp_type
  // dispatcher inline.  (Alternatively: convert cel::Value → CelValue
  // and reuse SetScalarField; the conversion needs arena bytes for
  // strings, which we don't have here.)
  using FD = google::protobuf::FieldDescriptor;
  const google::protobuf::Reflection* entry_refl = entry->GetReflection();
  // Key must be one of the proto-allowed map-key kinds; for any
  // other shape the descriptor wouldn't have legalised the field
  // — but we still bound-check below for defence.
  switch (key_fd->cpp_type()) {
    case FD::CPPTYPE_INT32: {
      auto i = key.AsInt();
      if (!i.ok()) return i.status();
      entry_refl->SetInt32(entry, key_fd, static_cast<int32_t>(*i));
      break;
    }
    case FD::CPPTYPE_INT64: {
      auto i = key.AsInt();
      if (!i.ok()) return i.status();
      entry_refl->SetInt64(entry, key_fd, *i);
      break;
    }
    case FD::CPPTYPE_UINT32: {
      auto u = key.AsUint();
      if (!u.ok()) return u.status();
      entry_refl->SetUInt32(entry, key_fd, static_cast<uint32_t>(*u));
      break;
    }
    case FD::CPPTYPE_UINT64: {
      auto u = key.AsUint();
      if (!u.ok()) return u.status();
      entry_refl->SetUInt64(entry, key_fd, *u);
      break;
    }
    case FD::CPPTYPE_BOOL: {
      auto b = key.AsBool();
      if (!b.ok()) return b.status();
      entry_refl->SetBool(entry, key_fd, *b);
      break;
    }
    case FD::CPPTYPE_STRING: {
      auto s = key.AsString();
      if (!s.ok()) return s.status();
      entry_refl->SetString(entry, key_fd, std::string(*s));
      break;
    }
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("CelSetFieldImpl: map key cpp_type ",
                       static_cast<int>(key_fd->cpp_type()), " not allowed"));
  }
  // Value arm — every cpp_type allowed (per descriptor.proto).
  switch (val_fd->cpp_type()) {
    case FD::CPPTYPE_BOOL: {
      auto b = value.AsBool();
      if (!b.ok()) return b.status();
      entry_refl->SetBool(entry, val_fd, *b);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_INT32: {
      auto i = value.AsInt();
      if (!i.ok()) return i.status();
      entry_refl->SetInt32(entry, val_fd, static_cast<int32_t>(*i));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_INT64: {
      auto i = value.AsInt();
      if (!i.ok()) return i.status();
      entry_refl->SetInt64(entry, val_fd, *i);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT32: {
      auto u = value.AsUint();
      if (!u.ok()) return u.status();
      entry_refl->SetUInt32(entry, val_fd, static_cast<uint32_t>(*u));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_UINT64: {
      auto u = value.AsUint();
      if (!u.ok()) return u.status();
      entry_refl->SetUInt64(entry, val_fd, *u);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_FLOAT: {
      auto d = value.AsDouble();
      if (!d.ok()) return d.status();
      entry_refl->SetFloat(entry, val_fd, static_cast<float>(*d));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_DOUBLE: {
      auto d = value.AsDouble();
      if (!d.ok()) return d.status();
      entry_refl->SetDouble(entry, val_fd, *d);
      return absl::OkStatus();
    }
    case FD::CPPTYPE_STRING: {
      auto s = (val_fd->type() == FD::TYPE_BYTES) ? value.AsBytes()
                                                  : value.AsString();
      if (!s.ok()) return s.status();
      entry_refl->SetString(entry, val_fd, std::string(*s));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_ENUM: {
      auto i = value.AsInt();
      if (!i.ok()) return i.status();
      entry_refl->SetEnumValue(entry, val_fd, static_cast<int>(*i));
      return absl::OkStatus();
    }
    case FD::CPPTYPE_MESSAGE: {
      auto backing_or = value.MessageBacking();
      if (!backing_or.ok()) return backing_or.status();
      const google::protobuf::Message* src_msg = (*backing_or)->message();
      if (src_msg == nullptr) {
        return absl::InvalidArgumentError(
            "CelSetFieldImpl: map message-value backing has no proto");
      }
      google::protobuf::Message* dst =
          entry_refl->MutableMessage(entry, val_fd);
      return WriteMessageOrPack(dst, *src_msg);
    }
  }
  ABSL_CHECK(false) << "InsertHostMapEntry: unknown value cpp_type "
                    << static_cast<int>(val_fd->cpp_type());
  return absl::InternalError("unreachable");
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
          status = InsertArenaMapEntry(msg, field, *refl, k, v, mem, refs);
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
    backing->ForEach([&](const cel::Value& k, const cel::Value& v) {
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

absl::Status CelSetFieldImpl(uint32_t msg_slot, uint32_t field_ref_id,
                             uint32_t value_slot,
                             const TrampolineContext& ctx) {
  const CelValue msg_cv = ctx.mem.ReadCelValue(msg_slot);
  if (msg_cv.kind != CEL_MESSAGE) {
    return absl::InvalidArgumentError(
        absl::StrCat("CelSetFieldImpl: msg_slot kind is ",
                     static_cast<int>(msg_cv.kind), " (expected CEL_MESSAGE)"));
  }
  const HostMessageBacking* backing = ctx.refs.Lookup(msg_cv.payload.msg_slot);
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

  // Route map / repeated source kinds through dedicated walkers.
  // Map check precedes repeated because every proto map field is
  // also `is_repeated()` per descriptor.proto.
  if (field->is_map()) {
    return SetMapField(*msg, *field, value_cv, ctx.mem, ctx.refs);
  }
  if (field->is_repeated()) {
    return SetRepeatedField(*msg, *field, value_cv, ctx.mem, ctx.refs);
  }

  if (value_cv.kind == CEL_UNKNOWN || value_cv.kind == CEL_ERROR) {
    // 3VL on `Foo{a: <unknown>}` is unaddressed; surfacing as a
    // clean trap matches the "trust the checker" stance — a
    // properly-typed CEL program won't pass an Unknown / Error to
    // a typed scalar field.  Revisit when partial-eval ×
    // construction is exercised by a fixture row.
    return absl::UnimplementedError(absl::StrCat(
        "CelSetFieldImpl: 3VL value kind=", static_cast<int>(value_cv.kind),
        " on field set not yet supported"));
  }
  return SetScalarField(*msg, *field, value_cv, ctx.mem, &ctx.refs);
}

// `cel_host.resolve_message_type_name` — descriptor-FQN resolver
// for `type(<message>)`.
//
//   1. Read CEL_MESSAGE at in_slot; look up `payload.msg_slot` in
//      `ctx.refs` → `HostMessageBacking*`.
//   2. Backing's `Message()` → `proto*`; `proto->GetDescriptor()
//      ->full_name()` → FQN std::string.
//   3. Allocate FQN bytes in the per-Eval arena via `ctx.alloc`.
//   4. Stamp `{kind: CEL_TYPE, payload.s: {arena_off, len}}` into
//      out_slot.
absl::Status CelResolveMessageTypeNameImpl(uint32_t out_slot, uint32_t in_slot,
                                           const TrampolineContext& ctx) {
  // Read the CEL_MESSAGE CelValue at `in_slot`; defence-in-depth the
  // kind check (the runtime helper already routes on kind, but a
  // direct caller — e.g. tests — could reach here with a wrong-kind
  // operand).
  const CelValue in_cv = ctx.mem.ReadCelValue(in_slot);
  if (in_cv.kind != CEL_MESSAGE) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  // Dereference the externref backing.  A miss here is a host-
  // adapter bug — the runtime should not have produced a CEL_MESSAGE
  // CelValue whose msg_slot is unmapped.
  const HostMessageBacking* backing = ctx.refs.Lookup(in_cv.payload.msg_slot);
  if (backing == nullptr) {
    WriteWireError(CEL_ERR_HOST_ADAPTER_ERROR, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const google::protobuf::Message* msg = backing->message();
  if (msg == nullptr) {
    // Non-proto backing — no descriptor.  Treat as adapter error;
    // a future milestone may surface a host-supplied type-name on
    // HostMessageBacking instead.
    WriteWireError(CEL_ERR_HOST_ADAPTER_ERROR, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  // `Descriptor::full_name()` returns `absl::string_view` in modern
  // protobuf — copy out into a string_view we own for the alloc below.
  const absl::string_view fqn = msg->GetDescriptor()->full_name();
  // Allocate the FQN bytes in the per-Eval arena and stamp the
  // CelSpan into out_slot.  Same lifetime model as the runtime
  // helper's primitive-name path: bytes outlive `arena_reset` because
  // the arena is reset only at the start of the NEXT Eval, and
  // user-visible Values copy the bytes out via the read-side
  // decoder before the arena resets.
  uint32_t off = 0;
  uint8_t* dst = ctx.alloc.Alloc(fqn.size(), &off);
  if (dst == nullptr && !fqn.empty()) {
    return absl::ResourceExhaustedError(
        absl::StrCat("CelResolveMessageTypeNameImpl: arena OOM (need ",
                     fqn.size(), " bytes for FQN `", fqn, "`)"));
  }
  if (!fqn.empty()) {
    std::memcpy(dst, fqn.data(), fqn.size());
  }
  CelValue out_cv{};
  out_cv.kind = CEL_TYPE;
  out_cv.payload.s.ptr = off;
  out_cv.payload.s.len = static_cast<uint32_t>(fqn.size());
  ctx.mem.WriteCelValue(out_slot, out_cv);
  return absl::OkStatus();
}

// ══════════════════════════════════════════════════════════════════
// Timestamp / duration parse + format kernels are now self-hosted in
// `compiler_v2/runtime/cel_time_parse.cc`; codegen routes the four
// ids there directly.  See
// `doc/implementation-plan/rewrite/phase-c-plan.md` §4.

namespace {

// `WriteInvalidArgumentError` mirrors the runtime-side `poison`
// shape and is still used by the with-TZ accessor trampoline below.
void WriteInvalidArgumentError(uint32_t out_slot, MemoryView& mem) {
  CelValue cv{};
  cv.kind = CEL_ERROR;
  cv.payload.err = WireErrorCode(cel::ErrorCode::kInvalidArgument);
  mem.WriteCelValue(out_slot, cv);
}

}  // namespace

// With-TZ accessor dispatch trampoline.
// ══════════════════════════════════════════════════════════════════
//
// Single host import absorbs all 10 with-TZ accessor surfaces; the
// per-accessor shims in cel_time.c supply the `accessor_kind`
// constant.  Wire enum lives in cel_time.h (`CelTzAccessorKind`)
// and is mirrored here — keep them in lockstep.  Rationale ("1
// dispatch trampoline vs 10 named trampolines": ABI surface count
// savings > switch-branch cost) lives in
// `doc/implementation-plan/rewrite/m7b-duration-timestamp.md`.

namespace {

// Mirrors `CelTzAccessorKind` in cel_time.h.  Append-only.
enum class TzAccessorKind : uint8_t {
  kYear = 0,
  kMonth = 1,
  kDayOfMonth1 = 2,
  kDayOfMonth = 3,
  kDayOfYear = 4,
  kDayOfWeek = 5,
  kHours = 6,
  kMinutes = 7,
  kSeconds = 8,
  kMilliseconds = 9,
};

int64_t ProjectCivilField(const absl::CivilSecond& cs, absl::Weekday weekday,
                          int day_of_year, int64_t ns_in_second,
                          TzAccessorKind kind) {
  switch (kind) {
    case TzAccessorKind::kYear:
      return cs.year();
    case TzAccessorKind::kMonth:
      return cs.month() - 1;  // cel-cpp 0-based
    case TzAccessorKind::kDayOfMonth1:
      return cs.day();  // 1-based
    case TzAccessorKind::kDayOfMonth:
      return cs.day() - 1;  // 0-based
    case TzAccessorKind::kDayOfYear:
      return day_of_year - 1;  // absl 1-based → cel-cpp 0-based
    case TzAccessorKind::kDayOfWeek:
      // absl::Weekday: monday=0..sunday=6.  cel-cpp: sunday=0..saturday=6.
      return (static_cast<int>(weekday) + 1) % 7;
    case TzAccessorKind::kHours:
      return cs.hour();
    case TzAccessorKind::kMinutes:
      return cs.minute();
    case TzAccessorKind::kSeconds:
      return cs.second();
    case TzAccessorKind::kMilliseconds: {
      // Sub-second within the civil second.  Sign-correlated nanos
      // get unix-floor-shifted to match cel-cpp's
      // `ToInt64Milliseconds(t - FloorToSecond(t))`.
      int64_t n = ns_in_second;
      if (n < 0) n += 1'000'000'000;
      return n / 1'000'000;
    }
  }
  return 0;  // unreachable; codegen never emits unknown kinds.
}

}  // namespace

namespace {

// Resolve a TZ string to an `absl::TimeZone`.  Three shapes:
//   - "UTC" / "Z" → UTC.
//   - "+HH:MM" / "-HH:MM" → fixed offset; absl::LoadTimeZone doesn't
//     parse these inline so we do it ourselves (plan §4.3).
//   - IANA name → absl::LoadTimeZone walks the host tzdata.
// Returns false on parse failure or unknown IANA name.
bool ResolveTimeZone(absl::string_view name, absl::TimeZone* out) {
  if (name == "UTC" || name == "Z") {
    *out = absl::UTCTimeZone();
    return true;
  }
  // Fixed offset: `+HH:MM` / `-HH:MM` / `HH:MM` (no sign = +).
  // cel-cpp admits the unsigned form per
  // `runtime/standard/time_functions.cc`.  Trim the sign prefix
  // if present, then validate HH:MM digit layout.
  int sign = 1;
  absl::string_view rest = name;
  if (!rest.empty() && (rest[0] == '+' || rest[0] == '-')) {
    sign = rest[0] == '+' ? 1 : -1;
    rest.remove_prefix(1);
  }
  if (rest.size() == 5 && rest[2] == ':' &&
      std::isdigit(static_cast<unsigned char>(rest[0])) &&
      std::isdigit(static_cast<unsigned char>(rest[1])) &&
      std::isdigit(static_cast<unsigned char>(rest[3])) &&
      std::isdigit(static_cast<unsigned char>(rest[4]))) {
    const int hours = ((rest[0] - '0') * 10) + (rest[1] - '0');
    const int minutes = ((rest[3] - '0') * 10) + (rest[4] - '0');
    if (hours > 23 || minutes > 59) return false;
    *out = absl::FixedTimeZone(sign * ((hours * 3600) + (minutes * 60)));
    return true;
  }
  return absl::LoadTimeZone(std::string(name), out);
}

// 3VL absorb + operand kind guards for the TZ-accessor trampoline.
// Returns true (and writes the result CelValue) if the call has
// already been short-circuited; false to continue.
bool TzAccessorPrelude(uint32_t out_slot, const CelValue& ts_cv,
                       const CelValue& tz_cv, uint32_t accessor_kind,
                       MemoryView& mem) {
  if (ts_cv.kind == CEL_ERROR || ts_cv.kind == CEL_UNKNOWN) {
    mem.WriteCelValue(out_slot, ts_cv);
    return true;
  }
  if (tz_cv.kind == CEL_ERROR || tz_cv.kind == CEL_UNKNOWN) {
    mem.WriteCelValue(out_slot, tz_cv);
    return true;
  }
  if (ts_cv.kind != CEL_TIMESTAMP || tz_cv.kind != CEL_STRING ||
      accessor_kind > static_cast<uint32_t>(TzAccessorKind::kMilliseconds)) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    mem.WriteCelValue(out_slot, err);
    return true;
  }
  return false;
}

}  // namespace

absl::Status CelTimestampTzAccessorImpl(uint32_t out_slot, uint32_t ts_slot,
                                        uint32_t tz_slot,
                                        uint32_t accessor_kind,
                                        const TrampolineContext& ctx) {
  CelValue ts_cv = ctx.mem.ReadCelValue(ts_slot);
  CelValue tz_cv = ctx.mem.ReadCelValue(tz_slot);
  if (TzAccessorPrelude(out_slot, ts_cv, tz_cv, accessor_kind, ctx.mem)) {
    return absl::OkStatus();
  }
  const absl::string_view tz_name =
      ctx.mem.ReadSpan(tz_cv.payload.s.ptr, tz_cv.payload.s.len);
  absl::TimeZone tz;
  if (!ResolveTimeZone(tz_name, &tz)) {
    WriteInvalidArgumentError(out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const absl::Time t = absl::UnixEpoch() +
                       absl::Seconds(ts_cv.payload.ts.seconds) +
                       absl::Nanoseconds(ts_cv.payload.ts.nanos);
  const absl::TimeZone::CivilInfo info = tz.At(t);
  const int day_of_year = absl::GetYearDay(absl::CivilDay(info.cs));
  const absl::Weekday weekday = absl::GetWeekday(absl::CivilDay(info.cs));
  const int64_t result =
      ProjectCivilField(info.cs, weekday, day_of_year, ts_cv.payload.ts.nanos,
                        static_cast<TzAccessorKind>(accessor_kind));
  CelValue out{};
  out.kind = CEL_INT;
  out.payload.i = result;
  ctx.mem.WriteCelValue(out_slot, out);
  return absl::OkStatus();
}

// WKT proto-literal unwrap.  Codegen emits this at the kStructExpr
// tail for `Timestamp{...}` / `Duration{...}` literals.  File-local
// helper: build a `CelValue` carrying the supplied error code with
// no payload data.  Used by `CelWktUnwrapWrapperImpl` to collapse the
// repeated `{kind=CEL_ERROR, payload.err=...}` blocks into one-line
// writes (keeps the parent under the readability-function-size gate).
static CelValue PoisonCelValue(uint32_t err_code) {
  CelValue v{};
  v.kind = CEL_ERROR;
  v.payload.err = err_code;
  return v;
}

// kStructExpr tail-unwrap for the 9 wrapper FQNs.  Reads
// the CEL_MESSAGE at `msg_slot`, peels the inner `value` field via
// the shared `UnpackWrapperMessage` helper (also used by the
// read-side auto-peel in `ReadSingularMessageField`), and writes
// the matching scalar CelValue at `out_slot`.  Cross-checks the
// produced kind against the caller-supplied `wrapper_kind` (1..6
// per CelKind); mismatch surfaces as `CEL_ERR_TYPE_MISMATCH`
// (codegen-regression tripwire).  Mirrors `CelWktUnwrapTimeImpl`.
absl::Status CelWktUnwrapWrapperImpl(uint32_t out_slot, uint32_t msg_slot,
                                     uint32_t wrapper_kind,
                                     const TrampolineContext& ctx) {
  const CelValue in = ctx.mem.ReadCelValue(msg_slot);
  if (in.kind == CEL_ERROR || in.kind == CEL_UNKNOWN) {
    ctx.mem.WriteCelValue(out_slot, in);
    return absl::OkStatus();
  }
  if (in.kind != CEL_MESSAGE) {
    ctx.mem.WriteCelValue(out_slot, PoisonCelValue(CEL_ERR_TYPE_MISMATCH));
    return absl::OkStatus();
  }
  const HostMessageBacking* backing = ctx.refs.Lookup(in.payload.msg_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelWktUnwrapWrapperImpl: msg_slot ", in.payload.msg_slot,
                     " not found in ExternrefTable"));
  }
  const google::protobuf::Message* msg = backing->message();
  if (msg == nullptr) {
    ctx.mem.WriteCelValue(out_slot, PoisonCelValue(CEL_ERR_TYPE_MISMATCH));
    return absl::OkStatus();
  }
  auto wrap = UnpackWrapperMessage(*msg);
  if (!wrap.has_value()) {
    ctx.mem.WriteCelValue(out_slot, PoisonCelValue(CEL_ERR_TYPE_MISMATCH));
    return absl::OkStatus();
  }
  CelValue out_cv{};
  if (auto s = EncodeValue(*wrap, &out_cv, ctx.alloc); !s.ok()) return s;
  // Codegen-regression tripwire: produced inner-scalar kind must
  // match the caller-supplied wrapper_kind (1..6 per CelKind).
  if (out_cv.kind != wrapper_kind) {
    ctx.mem.WriteCelValue(out_slot, PoisonCelValue(CEL_ERR_TYPE_MISMATCH));
    return absl::OkStatus();
  }
  ctx.mem.WriteCelValue(out_slot, out_cv);
  return absl::OkStatus();
}

absl::Status CelWktUnwrapTimeImpl(uint32_t out_slot, uint32_t msg_slot,
                                  const TrampolineContext& ctx) {
  const CelValue in = ctx.mem.ReadCelValue(msg_slot);
  if (in.kind == CEL_ERROR || in.kind == CEL_UNKNOWN) {
    ctx.mem.WriteCelValue(out_slot, in);
    return absl::OkStatus();
  }
  if (in.kind != CEL_MESSAGE) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }
  const HostMessageBacking* backing = ctx.refs.Lookup(in.payload.msg_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("CelWktUnwrapTimeImpl: msg_slot ", in.payload.msg_slot,
                     " not found in ExternrefTable"));
  }
  const google::protobuf::Message* msg = backing->message();
  if (msg == nullptr) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }
  // Reuse `UnpackWellKnownTimeMessage` — same helper the field-read
  // normaliser uses.  Returns nullopt if descriptor doesn't match
  // WKT Timestamp/Duration (which shouldn't happen — codegen only
  // emits this for matching s.name() — but defence-in-depth).
  auto wkt = UnpackWellKnownTimeMessage(*msg);
  if (!wkt.has_value()) {
    CelValue err{};
    err.kind = CEL_ERROR;
    err.payload.err = CEL_ERR_TYPE_MISMATCH;
    ctx.mem.WriteCelValue(out_slot, err);
    return absl::OkStatus();
  }
  CelValue out_cv{};
  if (auto s = EncodeValue(*wkt, &out_cv, ctx.alloc); !s.ok()) return s;
  ctx.mem.WriteCelValue(out_slot, out_cv);
  return absl::OkStatus();
}

}  // namespace celwasm

// ══════════════════════════════════════════════════════════════════
// cel::Value::Message(const google::protobuf::Message&)
// cel::Value::Map(...) / cel::Value::HostMap(...)
//
// Defined in this TU — not in value.cc — so value.cc doesn't need
// to know about ProtoBacking / HostMap.  The dependency is one-way:
// cel_host depends on value; value never depends on cel_host (only
// forward-declares the abstract bases for the shared_ptr slots in
// its variant).
// ══════════════════════════════════════════════════════════════════

namespace celwasm::api {

Value Value::Message(const google::protobuf::Message& m) {
  return Value::HostMessage(std::make_shared<celwasm::ProtoBacking>(&m));
}

Value Value::OwnedMessage(std::unique_ptr<google::protobuf::Message> m) {
  ABSL_CHECK(m != nullptr) << "Value::OwnedMessage: message must not be null";
  return Value::HostMessage(
      std::make_shared<celwasm::OwnedProtoBacking>(std::move(m)));
}

Value Value::Map(std::vector<std::pair<Value, Value>> entries) {
  return Value::HostMap(std::make_shared<celwasm::HostMap>(std::move(entries)));
}

Value Value::HostMap(std::shared_ptr<celwasm::HostMapBacking> backing) {
  ABSL_CHECK(backing != nullptr) << "Value::HostMap: backing must not be null";
  Value r;
  r.kind_ = Kind::kMap;
  r.payload_ = std::move(backing);
  return r;
}

Value Value::List(std::vector<Value> elements) {
  return Value::HostList(
      std::make_shared<celwasm::HostList>(std::move(elements)));
}

Value Value::HostList(std::shared_ptr<celwasm::HostListBacking> backing) {
  ABSL_CHECK(backing != nullptr) << "Value::HostList: backing must not be null";
  Value r;
  r.kind_ = Kind::kList;
  r.payload_ = std::move(backing);
  return r;
}

}  // namespace celwasm::api
