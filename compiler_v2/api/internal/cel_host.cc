#include "compiler_v2/api/internal/cel_host.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
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
    const cel::CelType& /*expected_type*/) const {
  ABSL_CHECK(msg_ != nullptr) << "ProtoBacking::ReadField: null message";
  const google::protobuf::FieldDescriptor* field =
      ResolveFieldDescriptor(*msg_, field_number, field_name);
  if (field == nullptr) return FieldNotFound(field_name);

  // M3.G: map fields land here as `Value::HostMap(ProtoMap{…})` —
  // the trampoline interns the backing into the ExternrefTable and
  // hands a `CEL_MAP_HOST` slot back to wasm.  Repeated (non-map)
  // fields stay TypeUnsupported until the lists slice.
  if (field->is_map()) {
    return cel::Value::HostMap(std::make_shared<ProtoMap>(msg_, field));
  }
  if (field->is_repeated()) {
    return TypeUnsupported(field_name);
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
// ProtoMap (M3.G, reflection-backed).  TU-internal via `static` so
// the symbols don't escape this translation unit.

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
std::optional<cel::Value> DecodeKey(const CelValue& cv,
                                    const MemoryView& mem) {
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
    case cel::ErrorCode::kOverflow:        return CEL_ERR_OVERFLOW;
    case cel::ErrorCode::kDivideByZero:    return CEL_ERR_DIVIDE_BY_ZERO;
    case cel::ErrorCode::kModulusByZero:   return CEL_ERR_MODULUS_BY_ZERO;
    case cel::ErrorCode::kTypeMismatch:    return CEL_ERR_TYPE_MISMATCH;
    case cel::ErrorCode::kTypeUnsupported: return CEL_ERR_TYPE_UNSUPPORTED;
    case cel::ErrorCode::kKeyNotFound:     return CEL_ERR_NO_SUCH_KEY;
    case cel::ErrorCode::kFieldNotFound:   return CEL_ERR_FIELD_NOT_FOUND;
    case cel::ErrorCode::kHostAdapterError:
      return CEL_ERR_HOST_ADAPTER_ERROR;
    default:                               return CEL_ERR_TYPE_MISMATCH;
  }
}

// Build a CEL_ERROR CelValue with the given wire code and write it
// to `out_slot`.  Used by every Layer-2 arm that surfaces a
// spec-level error in-wire (rather than returning non-OK Status).
void WriteWireError(uint32_t wire_code, uint32_t out_slot,
                    MemoryView& mem) {
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = wire_code;
  mem.WriteCelValue(out_slot, err);
}

// Encode the (string|bytes) span via the per-eval ArenaAllocator.
absl::Status EncodeSpan(const cel::Value& v, CelValue* out,
                        ArenaAllocator& alloc) {
  using K = cel::Value::Kind;
  absl::string_view s =
      v.kind() == K::kString ? *v.AsString() : *v.AsBytes();
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

// Encode a cel::Value into a CelValue, allocating string/bytes
// payloads through the per-eval ArenaAllocator.  Returns non-OK
// Status on infrastructure failure (arena OOM); spec-level errors
// inside the input Value already encode as `{kind:CEL_ERROR, err:…}`.
absl::Status EncodeValue(const cel::Value& v, CelValue* out,
                         ArenaAllocator& alloc) {
  using K = cel::Value::Kind;
  switch (v.kind()) {
    case K::kNull:    out->kind = CEL_NULL; return absl::OkStatus();
    case K::kBool:    out->kind = CEL_BOOL;
                      out->payload.b = *v.AsBool() ? 1 : 0;
                      return absl::OkStatus();
    case K::kInt:     out->kind = CEL_INT;
                      out->payload.i = *v.AsInt();
                      return absl::OkStatus();
    case K::kUint:    out->kind = CEL_UINT;
                      out->payload.u = *v.AsUint();
                      return absl::OkStatus();
    case K::kDouble:  out->kind = CEL_DOUBLE;
                      out->payload.d = *v.AsDouble();
                      return absl::OkStatus();
    case K::kString:
    case K::kBytes:   return EncodeSpan(v, out, alloc);
    case K::kError: {
      const cel::ErrorPayload* e = *v.ErrorInfo();
      out->kind = CEL_ERROR;
      out->payload.err = WireErrorCode(e->code);
      return absl::OkStatus();
    }
    case K::kUnknown:
      // M4 wires `cel_unknown_pattern_match` into this path; for now
      // the Layer 2 contract is that backings don't return unknowns —
      // operand-pair propagation happens before this encoder.
      ABSL_CHECK(false) << "EncodeValue: kUnknown is a stub until M4";
    case K::kMessage:
    case K::kMap:
    case K::kList:
    case K::kDuration:
    case K::kTimestamp:
      ABSL_CHECK(false) << "EncodeValue: kind " << static_cast<int>(v.kind())
                        << " is a stub until later milestone";
  }
  ABSL_CHECK(false) << "EncodeValue: unhandled kind "
                    << static_cast<int>(v.kind());
}

}  // namespace

absl::Status CelMapLookupImpl(uint32_t out_slot, uint32_t map_slot,
                              uint32_t key_slot,
                              const TrampolineContext& ctx) {
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
    return absl::FailedPreconditionError(absl::StrCat(
        "CelMapLookupImpl: map ref_slot ", map_cv.payload.ref_slot,
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

  CelValue out{};
  if (auto s = EncodeValue(*got, &out, ctx.alloc); !s.ok()) return s;
  ctx.mem.WriteCelValue(out_slot, out);
  return absl::OkStatus();
}

// ══════════════════════════════════════════════════════════════════
// ProtoMap — proto-reflection over a single map field (M3.G).
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
      << "ProtoMap: entry of `" << field.name() << "` missing field "
      << number;
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
    const cel::Value& key,
    const cel::CelType& /*expected_value_type*/) const {
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
    absl::FunctionRef<void(const cel::Value&, const cel::Value&)> visit)
    const {
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
// Layer-2 trampoline bodies — `CelGetFieldImpl` / `CelHasFieldImpl`
// (M2.C.0b).
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
std::optional<cel::Attribute> ResolveAttribute(
    const CelHostBindings& bindings, uint32_t attribute_id) {
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
  return cel::Attribute(std::string(operand_attr.variable_name()),
                        std::move(path));
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
  for (const cel::AttributePattern& pat : bindings.unknown_patterns) {
    if (pat.IsMatch(eff) == cel::AttributePattern::MatchType::kFull) {
      return true;
    }
  }
  return false;
}

// Marshal a `cel::Value` returned by `backing->ReadField` into the
// 24-byte CelValue at `out_slot`.  Scalars + null + error encode
// inline / via arena (existing `EncodeValue`); kMessage interns the
// nested backing into the externref table and writes the slot.
absl::Status EncodeFieldResult(const cel::Value& v, uint32_t out_slot,
                               const TrampolineContext& ctx) {
  if (v.kind() == cel::Value::Kind::kMessage) {
    auto sub_or = v.SharedMessageBacking();
    if (!sub_or.ok()) return sub_or.status();
    const uint32_t slot = ctx.refs.Intern(*std::move(sub_or));
    CelValue cv{};
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = slot;
    ctx.mem.WriteCelValue(out_slot, cv);
    return absl::OkStatus();
  }
  CelValue cv{};
  if (auto s = EncodeValue(v, &cv, ctx.alloc); !s.ok()) return s;
  ctx.mem.WriteCelValue(out_slot, cv);
  return absl::OkStatus();
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

  if (MatchesAnyUnknownPattern(ctx.bindings, attribute_id,
                               field->field_name)) {
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
  auto prelude_or = RunFieldPrelude(out_slot, msg_slot, field_ref_id,
                                    attribute_id, ctx);
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
  auto prelude_or = RunFieldPrelude(out_slot, msg_slot, field_ref_id,
                                    attribute_id, ctx);
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

namespace cel {

Value Value::Message(const google::protobuf::Message& m) {
  return Value::HostMessage(std::make_shared<celwasm::ProtoBacking>(&m));
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

}  // namespace cel
