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
  // hands a `CEL_MAP_HOST` slot back to wasm.
  // M4.G: REPEATED (non-map) fields land as `Value::HostList(
  // ProtoList{…})` — same intern path, separate ExternrefTable
  // namespace.  `is_map()` is checked first because every map field
  // is also `is_repeated()` per descriptor.proto.
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
    case K::kTimestamp:
      ABSL_CHECK(false) << "EncodeValue: kind " << static_cast<int>(v.kind())
                        << " is a stub until later milestone";
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
  const size_t count = static_cast<size_t>(refl->FieldSize(*owner_, field_));
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
// M5.D step 2 — aggregate-op kHost trampolines.
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
// returns false here for now and lands as M6 follow-up.
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
// for this slice (M6 follow-up; mirrors arena fast path).
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
  //      `cel_alloc`, sized `a_size + b_size`.  ArenaAllocator's
  //      `Alloc` already reenters wasm for `cel_alloc`, so this
  //      works from inside a host trampoline.
  //   2. For each operand:
  //        - If CEL_LIST_ARENA: memcpy the elements run into the
  //          new run at the right offset.
  //        - If CEL_LIST_HOST: walk `backing->ForEach`, encode each
  //          `cel::Value` into a CelValue (via `EncodeBackingScalar`
  //          extended for aggregates — the M6 work item), and
  //          write into the destination run.
  //   3. Write `{kind:CEL_LIST_ARENA, arena_list.header_ptr=hdr_off}`
  //      into `out_slot`.  The result is observably an arena list,
  //      which keeps downstream codegen on the fast path.
  //
  // This same strategy applies to mixed-origin map equality (see
  // CelMapEqImpl) and to any future operator that needs to walk
  // both operands as one origin: lift host into arena, then run
  // the arena fast path.  Documented in
  // `doc/implementation-plan/rewrite/m5-kcall-comprehensions.md
  //  §"Cross-origin materialisation"` and
  // `doc/implementation-plan/rewrite/map-list-dispatch.md §6`.
  //
  // M5.D step 2 ship state: nested-aggregate elements + the
  // re-entrant arena allocation aren't fully exercised yet, so
  // mixed-origin concat POISONs with TYPE_MISMATCH for now.  M6
  // (or earlier follow-up) flips this to actual materialisation.
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
  // M5.D step 2 ship state: only both-host map equality is supported
  // (mixed origins → TYPE_MISMATCH).  Same-arena routes through the
  // dispatcher's arena fast path.  The shipping strategy for
  // arena↔host pairs is to MATERIALISE the host operand into the
  // arena (lift via ForEach + EncodeBackingScalar + cel_alloc) and
  // then run the arena+arena equality walk — same lift-then-walk
  // pattern documented in CelListConcatImpl and described in
  // `m5-kcall-comprehensions.md §"Cross-origin materialisation"`.
  // The lift body lands as an M6 follow-up.
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
  // M5.D step 2 ship state: ProtoBacking is the only concrete that
  // exposes its underlying `google::protobuf::Message*`.  Custom
  // embedder backings have no equality contract yet — POISON with
  // TYPE_MISMATCH (M6 will define a virtual `Equals(other)` on
  // HostMessageBacking).  MessageDifferencer handles cross-descriptor
  // comparison by erroring out — our caller reads two ProtoBackings
  // typed at the same descriptor, so this is the common case.
  const auto* a_proto = dynamic_cast<const ProtoBacking*>(a_backing);
  const auto* b_proto = dynamic_cast<const ProtoBacking*>(b_backing);
  if (a_proto == nullptr || b_proto == nullptr) {
    WriteWireError(CEL_ERR_TYPE_MISMATCH, out_slot, ctx.mem);
    return absl::OkStatus();
  }
  const bool eq = google::protobuf::util::MessageDifferencer::Equals(
      *a_proto->message(), *b_proto->message());
  WriteWireBool(eq, out_slot, ctx.mem);
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

}  // namespace cel
