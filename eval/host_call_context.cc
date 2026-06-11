#include "eval/host_call_context.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "eval/attribute.h"
#include "eval/error.h"
#include "eval/internal/cel_host.h"
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "runtime/cel_data.h"
#include "shared/type.h"

namespace celwasm {
namespace {

absl::string_view WireKindName(uint32_t kind) {
  switch (kind) {
    case CEL_NULL:
      return "null";
    case CEL_BOOL:
      return "bool";
    case CEL_INT:
      return "int";
    case CEL_UINT:
      return "uint";
    case CEL_DOUBLE:
      return "double";
    case CEL_STRING:
      return "string";
    case CEL_BYTES:
      return "bytes";
    case CEL_LIST_ARENA:
    case CEL_LIST_HOST:
      return "list";
    case CEL_MAP_ARENA:
    case CEL_MAP_HOST:
      return "map";
    case CEL_MESSAGE:
      return "message";
    case CEL_TYPE:
      return "type";
    case CEL_DURATION:
      return "duration";
    case CEL_TIMESTAMP:
      return "timestamp";
    case CEL_UNKNOWN:
      return "unknown";
    case CEL_ERROR:
      return "error";
    default:
      return "<unrecognized-kind>";
  }
}

absl::Duration DecodeDuration(const CelDurTs& d) {
  return absl::Seconds(d.seconds) + absl::Nanoseconds(d.nanos);
}

Value DecodeWireError(const CelValue& cv) {
  ErrorPayload p;
  // Runtime CEL_ERR_* numerics mirror host ErrorCode 1:1 (cel_data.h ↔
  // error.h); an unrecognized wire byte degrades to kHostAdapterError
  // rather than crashing the decoder.
  const auto code = static_cast<ErrorCode>(cv.payload.err);
  switch (code) {
    case ErrorCode::kOverflow:
    case ErrorCode::kDivideByZero:
    case ErrorCode::kModulusByZero:
    case ErrorCode::kTypeMismatch:
    case ErrorCode::kTypeUnsupported:
    case ErrorCode::kKeyNotFound:
    case ErrorCode::kDuplicateKey:
    case ErrorCode::kIndexOutOfBounds:
    case ErrorCode::kInvalidArgument:
    case ErrorCode::kFieldNotFound:
    case ErrorCode::kUnknownType:
    case ErrorCode::kCustomFnFailed:
    case ErrorCode::kHostAdapterError:
    case ErrorCode::kTimeout:
      p.code = code;
      p.message = std::string(ErrorCodeName(code));
      break;
    default:
      p.code = ErrorCode::kHostAdapterError;
      p.message = absl::StrCat("runtime error code ", cv.payload.err);
      break;
  }
  return Value::Error(std::move(p));
}

// Forward decl — DecodeCelValue and the arena walkers are mutually
// recursive (a list/map element may itself be an aggregate).
absl::StatusOr<Value> DecodeCelValue(const CelValue& cv, const MemoryView& mem,
                                     const ExternrefTable& refs);

// Read a fixed-size POD struct (`ArenaListHeader` / `ArenaMapHeader`)
// out of linear memory via the span reader.
template <typename Header>
absl::StatusOr<Header> ReadArenaHeader(const MemoryView& mem,
                                       uint32_t header_ptr) {
  absl::string_view bytes = mem.ReadSpan(header_ptr, sizeof(Header));
  if (bytes.size() != sizeof(Header)) {
    return absl::FailedPreconditionError(
        absl::StrCat("arena header at offset ", header_ptr, " is truncated"));
  }
  Header h{};
  bytes.copy(reinterpret_cast<char*>(&h), sizeof(Header));
  return h;
}

absl::StatusOr<Value> DecodeArenaList(uint32_t header_ptr,
                                      const MemoryView& mem,
                                      const ExternrefTable& refs) {
  if (header_ptr == 0) return Value::List({});  // empty-list sentinel
  auto hdr_or = ReadArenaHeader<ArenaListHeader>(mem, header_ptr);
  if (!hdr_or.ok()) return hdr_or.status();
  std::vector<Value> elements;
  elements.reserve(hdr_or->count);
  for (uint32_t i = 0; i < hdr_or->count; ++i) {
    const uint32_t off = hdr_or->elements_offset + (i * kCelListEntryStride);
    auto v_or = DecodeCelValue(mem.ReadCelValue(off), mem, refs);
    if (!v_or.ok()) return v_or.status();
    elements.push_back(*std::move(v_or));
  }
  return Value::List(std::move(elements));
}

absl::StatusOr<std::vector<std::pair<Value, Value>>> DecodeArenaMapEntries(
    uint32_t header_ptr, const MemoryView& mem, const ExternrefTable& refs) {
  std::vector<std::pair<Value, Value>> entries;
  if (header_ptr == 0) return entries;  // empty-map sentinel
  auto hdr_or = ReadArenaHeader<ArenaMapHeader>(mem, header_ptr);
  if (!hdr_or.ok()) return hdr_or.status();
  entries.reserve(hdr_or->count);
  for (uint32_t i = 0; i < hdr_or->count; ++i) {
    const uint32_t entry_off =
        hdr_or->entries_offset + (i * kCelMapEntryStride);
    auto k_or = DecodeCelValue(mem.ReadCelValue(entry_off), mem, refs);
    if (!k_or.ok()) return k_or.status();
    auto v_or = DecodeCelValue(mem.ReadCelValue(entry_off + sizeof(CelValue)),
                               mem, refs);
    if (!v_or.ok()) return v_or.status();
    entries.emplace_back(*std::move(k_or), *std::move(v_or));
  }
  return entries;
}

absl::StatusOr<Value> DecodeHostList(const ExternrefTable& refs,
                                     uint32_t ref_slot) {
  const HostListBacking* backing = refs.LookupList(ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "CEL_LIST_HOST ref_slot=", ref_slot, " has no externref entry"));
  }
  std::vector<Value> elements;
  elements.reserve(backing->Size());
  backing->ForEach([&elements](const Value& v) {
    elements.push_back(v);
  });
  return Value::List(std::move(elements));
}

absl::StatusOr<Value> DecodeHostMap(const ExternrefTable& refs,
                                    uint32_t ref_slot) {
  const HostMapBacking* backing = refs.LookupMap(ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "CEL_MAP_HOST ref_slot=", ref_slot, " has no externref entry"));
  }
  std::vector<std::pair<Value, Value>> entries;
  entries.reserve(backing->Size());
  backing->ForEach([&entries](const Value& k, const Value& v) {
    entries.emplace_back(k, v);
  });
  return Value::Map(std::move(entries));
}

absl::StatusOr<Value> DecodeHostMessage(const ExternrefTable& refs,
                                        uint32_t msg_slot) {
  const HostMessageBacking* backing = refs.Lookup(msg_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "CEL_MESSAGE msg_slot=", msg_slot, " has no externref entry"));
  }
  const google::protobuf::Message* msg = backing->message();
  if (msg == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "CEL_MESSAGE msg_slot=", msg_slot, " backing has no proto message"));
  }
  // Non-owning view over the live interned message — valid for the
  // callback's duration (the table is reset only after the Eval).
  return Value::Message(*msg);
}

// Decode a CEL_UNKNOWN payload: `payload.unk` is 0 (the legal empty
// UnknownSet) or a byte offset to a 2-word `{ids_off, len}` descriptor
// whose id array carries every attribute identity the unknown merged
// (doc/design/03-abi-and-memory.md §8.2).  Surfaces EVERY id — a
// merged unknown must not collapse to one winner.
absl::StatusOr<Value> DecodeUnknownSet(uint32_t desc_off,
                                       const MemoryView& mem) {
  if (desc_off == 0) return Value::Unknown(std::vector<AttributeId>{});
  uint32_t desc[2];
  const absl::string_view desc_bytes = mem.ReadSpan(desc_off, sizeof(desc));
  if (desc_bytes.size() != sizeof(desc)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CEL_UNKNOWN descriptor at offset ", desc_off, " is out of bounds"));
  }
  std::memcpy(desc, desc_bytes.data(), sizeof(desc));
  const uint32_t ids_off = desc[0];
  const uint32_t len = desc[1];
  const uint64_t ids_bytes = uint64_t{len} * sizeof(uint32_t);
  if (ids_bytes > std::numeric_limits<uint32_t>::max()) {
    return absl::InvalidArgumentError(
        absl::StrCat("CEL_UNKNOWN descriptor at offset ", desc_off,
                     " claims an impossible id count ", len));
  }
  const absl::string_view raw =
      mem.ReadSpan(ids_off, static_cast<uint32_t>(ids_bytes));
  if (raw.size() != ids_bytes) {
    return absl::InvalidArgumentError(
        absl::StrCat("CEL_UNKNOWN id array at offset ", ids_off, " (len ", len,
                     ") is out of bounds"));
  }
  std::vector<AttributeId> attrs;
  attrs.reserve(len);
  for (uint32_t i = 0; i < len; ++i) {
    uint32_t id = 0;
    std::memcpy(&id, raw.data() + (uint64_t{i} * sizeof(uint32_t)), sizeof(id));
    attrs.push_back(AttributeId{id});
  }
  return Value::Unknown(std::move(attrs));
}

absl::StatusOr<Value> DecodeCelValue(const CelValue& cv, const MemoryView& mem,
                                     const ExternrefTable& refs) {
  switch (cv.kind) {
    case CEL_NULL:
      return Value::Null();
    case CEL_BOOL:
      return Value::Bool(cv.payload.b != 0);
    case CEL_INT:
      return Value::Int(cv.payload.i);
    case CEL_UINT:
      return Value::Uint(cv.payload.u);
    case CEL_DOUBLE:
      return Value::Double(cv.payload.d);
    case CEL_STRING:
      return Value::String(
          std::string(mem.ReadSpan(cv.payload.s.ptr, cv.payload.s.len)));
    case CEL_BYTES:
      return Value::Bytes(std::string(
          mem.ReadSpan(cv.payload.bytes.ptr, cv.payload.bytes.len)));
    case CEL_DURATION:
      return Value::Duration(DecodeDuration(cv.payload.dur));
    case CEL_TIMESTAMP:
      return Value::Timestamp(absl::UnixEpoch() +
                              DecodeDuration(cv.payload.ts));
    case CEL_TYPE:
      return Value::Type(
          std::string(mem.ReadSpan(cv.payload.s.ptr, cv.payload.s.len)));
    case CEL_UNKNOWN:
      return DecodeUnknownSet(cv.payload.unk, mem);
    case CEL_ERROR:
      return DecodeWireError(cv);
    case CEL_LIST_ARENA:
      return DecodeArenaList(cv.payload.arena_list.header_ptr, mem, refs);
    case CEL_MAP_ARENA: {
      auto entries_or =
          DecodeArenaMapEntries(cv.payload.arena_map.header_ptr, mem, refs);
      if (!entries_or.ok()) return entries_or.status();
      return Value::Map(*std::move(entries_or));
    }
    case CEL_LIST_HOST:
      return DecodeHostList(refs, cv.payload.ref_slot);
    case CEL_MAP_HOST:
      return DecodeHostMap(refs, cv.payload.ref_slot);
    case CEL_MESSAGE:
      return DecodeHostMessage(refs, cv.payload.msg_slot);
    default:
      return absl::InvalidArgumentError(absl::StrCat(
          "cannot decode CelValue of kind ", static_cast<int>(cv.kind)));
  }
}

}  // namespace

// ─────────────────────────── HostListView ──────────────────────────

size_t HostListView::Size() const {
  return backing_ != nullptr ? backing_->Size() : count_;
}

absl::StatusOr<Value> HostListView::At(size_t index) const {
  if (backing_ != nullptr) {
    return backing_->At(index, CelType{});
  }
  if (index >= count_) {
    return absl::InvalidArgumentError(
        absl::StrCat("list index ", index, " out of range [0, ", count_, ")"));
  }
  const uint32_t off =
      elements_offset_ + (static_cast<uint32_t>(index) * kCelListEntryStride);
  return DecodeCelValue(mem_->ReadCelValue(off), *mem_, *refs_);
}

// ─────────────────────────── HostMapView ───────────────────────────

size_t HostMapView::Size() const {
  return backing_ != nullptr ? backing_->Size() : count_;
}

namespace {
// Snapshot an arena map's entries.  Get / ContainsKey build a local,
// vector-backed `HostMap` from these so they inherit `HostMap`'s
// spec-correct map-key equality (cross-type numeric int/uint,
// structural bool/string) instead of reimplementing it.  Per-call cost
// is acceptable on the arena-map argument edge; the common, motivating
// maps are externref-backed and take the direct backing path above.
// (`HostMap` is neither copyable nor movable, so it cannot be returned
// by value — the caller constructs it in place from these entries.)
absl::StatusOr<std::vector<std::pair<Value, Value>>> ReadArenaMapEntries(
    uint32_t count, uint32_t entries_offset, const MemoryView& mem,
    const ExternrefTable& refs) {
  std::vector<std::pair<Value, Value>> entries;
  entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry_off = entries_offset + (i * kCelMapEntryStride);
    auto k_or = DecodeCelValue(mem.ReadCelValue(entry_off), mem, refs);
    if (!k_or.ok()) return k_or.status();
    auto v_or = DecodeCelValue(mem.ReadCelValue(entry_off + sizeof(CelValue)),
                               mem, refs);
    if (!v_or.ok()) return v_or.status();
    entries.emplace_back(*std::move(k_or), *std::move(v_or));
  }
  return entries;
}
}  // namespace

absl::StatusOr<Value> HostMapView::Get(const Value& key) const {
  if (backing_ != nullptr) {
    return backing_->Get(key, CelType{});
  }
  auto entries_or = ReadArenaMapEntries(count_, entries_offset_, *mem_, *refs_);
  if (!entries_or.ok()) return entries_or.status();
  HostMap hm(*std::move(entries_or));
  return hm.Get(key, CelType{});
}

bool HostMapView::ContainsKey(const Value& key) const {
  if (backing_ != nullptr) {
    return backing_->ContainsKey(key);
  }
  auto entries_or = ReadArenaMapEntries(count_, entries_offset_, *mem_, *refs_);
  if (!entries_or.ok()) return false;
  HostMap hm(*std::move(entries_or));
  return hm.ContainsKey(key);
}

// ───────────────────────── HostCallContext ─────────────────────────

namespace {
absl::Status OutOfRange(int i, int n) {
  return absl::OutOfRangeError(
      absl::StrCat("arg index ", i, " out of range [0, ", n, ")"));
}
absl::Status WrongKind(int i, absl::string_view want, uint32_t got) {
  return absl::InvalidArgumentError(absl::StrCat("arg ", i, ": expected ", want,
                                                 ", got ", WireKindName(got)));
}
}  // namespace

absl::StatusOr<bool> HostCallContext::ArgBool(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind != CEL_BOOL) return WrongKind(i, "bool", cv.kind);
  return cv.payload.b != 0;
}

absl::StatusOr<int64_t> HostCallContext::ArgInt(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind != CEL_INT) return WrongKind(i, "int", cv.kind);
  return cv.payload.i;
}

absl::StatusOr<uint64_t> HostCallContext::ArgUint(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind != CEL_UINT) return WrongKind(i, "uint", cv.kind);
  return cv.payload.u;
}

absl::StatusOr<double> HostCallContext::ArgDouble(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind != CEL_DOUBLE) return WrongKind(i, "double", cv.kind);
  return cv.payload.d;
}

absl::StatusOr<absl::string_view> HostCallContext::ArgString(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind != CEL_STRING) return WrongKind(i, "string", cv.kind);
  return mem_.ReadSpan(cv.payload.s.ptr, cv.payload.s.len);
}

absl::StatusOr<absl::string_view> HostCallContext::ArgBytes(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind != CEL_BYTES) return WrongKind(i, "bytes", cv.kind);
  return mem_.ReadSpan(cv.payload.bytes.ptr, cv.payload.bytes.len);
}

absl::StatusOr<absl::Duration> HostCallContext::ArgDuration(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind != CEL_DURATION) return WrongKind(i, "duration", cv.kind);
  return DecodeDuration(cv.payload.dur);
}

absl::StatusOr<absl::Time> HostCallContext::ArgTimestamp(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind != CEL_TIMESTAMP) return WrongKind(i, "timestamp", cv.kind);
  return absl::UnixEpoch() + DecodeDuration(cv.payload.ts);
}

bool HostCallContext::ArgIsNull(int i) const {
  if (i < 0 || i >= NumArgs()) return false;
  return mem_.ReadCelValue(arg_slots_[i]).kind == CEL_NULL;
}

absl::StatusOr<const google::protobuf::Message*> HostCallContext::ArgProto(
    int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind != CEL_MESSAGE) return WrongKind(i, "message", cv.kind);
  const HostMessageBacking* backing = refs_.Lookup(cv.payload.msg_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("arg ", i, ": msg_slot ", cv.payload.msg_slot,
                     " has no externref entry"));
  }
  const google::protobuf::Message* msg = backing->message();
  if (msg == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("arg ", i, ": backing is not a proto message"));
  }
  return msg;
}

absl::StatusOr<HostListView> HostCallContext::ArgList(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind == CEL_LIST_HOST) {
    const HostListBacking* backing = refs_.LookupList(cv.payload.ref_slot);
    if (backing == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("arg ", i, ": list ref_slot ", cv.payload.ref_slot,
                       " has no externref entry"));
    }
    return HostListView(backing);
  }
  if (cv.kind == CEL_LIST_ARENA) {
    const uint32_t header_ptr = cv.payload.arena_list.header_ptr;
    if (header_ptr == 0) {
      return HostListView(&mem_, &refs_, /*count=*/0, /*elements_offset=*/0);
    }
    auto hdr_or = ReadArenaHeader<ArenaListHeader>(mem_, header_ptr);
    if (!hdr_or.ok()) return hdr_or.status();
    return HostListView(&mem_, &refs_, hdr_or->count, hdr_or->elements_offset);
  }
  return WrongKind(i, "list", cv.kind);
}

absl::StatusOr<HostMapView> HostCallContext::ArgMap(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  CelValue cv = mem_.ReadCelValue(arg_slots_[i]);
  if (cv.kind == CEL_MAP_HOST) {
    const HostMapBacking* backing = refs_.LookupMap(cv.payload.ref_slot);
    if (backing == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("arg ", i, ": map ref_slot ", cv.payload.ref_slot,
                       " has no externref entry"));
    }
    return HostMapView(backing);
  }
  if (cv.kind == CEL_MAP_ARENA) {
    const uint32_t header_ptr = cv.payload.arena_map.header_ptr;
    if (header_ptr == 0) {
      return HostMapView(&mem_, &refs_, /*count=*/0, /*entries_offset=*/0);
    }
    auto hdr_or = ReadArenaHeader<ArenaMapHeader>(mem_, header_ptr);
    if (!hdr_or.ok()) return hdr_or.status();
    return HostMapView(&mem_, &refs_, hdr_or->count, hdr_or->entries_offset);
  }
  return WrongKind(i, "map", cv.kind);
}

absl::StatusOr<Value> HostCallContext::ArgValue(int i) const {
  if (i < 0 || i >= NumArgs()) return OutOfRange(i, NumArgs());
  return DecodeCelValue(mem_.ReadCelValue(arg_slots_[i]), mem_, refs_);
}

// ── return setters ──

absl::Status HostCallContext::ReturnBool(bool v) {
  return EncodeValueToSlot(Value::Bool(v), out_slot_, mem_, refs_, arena_);
}
absl::Status HostCallContext::ReturnInt(int64_t v) {
  return EncodeValueToSlot(Value::Int(v), out_slot_, mem_, refs_, arena_);
}
absl::Status HostCallContext::ReturnUint(uint64_t v) {
  return EncodeValueToSlot(Value::Uint(v), out_slot_, mem_, refs_, arena_);
}
absl::Status HostCallContext::ReturnDouble(double v) {
  return EncodeValueToSlot(Value::Double(v), out_slot_, mem_, refs_, arena_);
}
absl::Status HostCallContext::ReturnString(absl::string_view v) {
  return EncodeValueToSlot(Value::String(std::string(v)), out_slot_, mem_,
                           refs_, arena_);
}
absl::Status HostCallContext::ReturnBytes(absl::string_view v) {
  return EncodeValueToSlot(Value::Bytes(std::string(v)), out_slot_, mem_, refs_,
                           arena_);
}
absl::Status HostCallContext::ReturnDuration(absl::Duration v) {
  return EncodeValueToSlot(Value::Duration(v), out_slot_, mem_, refs_, arena_);
}
absl::Status HostCallContext::ReturnTimestamp(absl::Time v) {
  return EncodeValueToSlot(Value::Timestamp(v), out_slot_, mem_, refs_, arena_);
}
absl::Status HostCallContext::ReturnNull() {
  return EncodeValueToSlot(Value::Null(), out_slot_, mem_, refs_, arena_);
}
absl::Status HostCallContext::ReturnProto(
    std::unique_ptr<google::protobuf::Message> m) {
  return EncodeValueToSlot(Value::OwnedMessage(std::move(m)), out_slot_, mem_,
                           refs_, arena_);
}
absl::Status HostCallContext::ReturnList(absl::Span<const Value> elems) {
  return EncodeValueToSlot(Value::List({elems.begin(), elems.end()}), out_slot_,
                           mem_, refs_, arena_);
}
absl::Status HostCallContext::ReturnMap(
    absl::Span<const std::pair<Value, Value>> entries) {
  return EncodeValueToSlot(Value::Map({entries.begin(), entries.end()}),
                           out_slot_, mem_, refs_, arena_);
}

absl::Status HostCallContext::ReturnUnknown() {
  // Function-origin unknown: a 1-element UnknownSet descriptor
  // carrying the reserved sentinel id (the descriptor wire shape is
  // mandatory — a raw sentinel in `payload.unk` would be dereferenced
  // as a descriptor offset by `cel_unknown_merge`).  Bypasses the
  // shared encoder, whose backing-side contract forbids kUnknown.
  CelValue cv{};
  const uint32_t ids[] = {kFunctionUnknownSentinel};
  if (auto s = EncodeUnknownSet(ids, arena_, &cv); !s.ok()) return s;
  mem_.WriteCelValue(out_slot_, cv);
  return absl::OkStatus();
}

absl::Status HostCallContext::ReturnError(ErrorPayload payload) {
  return EncodeValueToSlot(Value::Error(std::move(payload)), out_slot_, mem_,
                           refs_, arena_);
}

absl::Status HostCallContext::ReturnValue(const Value& v) {
  if (v.IsUnknown()) {
    // Preserve the full attribute-id set (a function-origin sentinel
    // round-trips unchanged; a propagated input unknown keeps its
    // real ids).  Written directly for the same reason as
    // ReturnUnknown.
    auto attrs_or = v.UnknownAttributes();
    if (!attrs_or.ok()) return attrs_or.status();
    std::vector<uint32_t> ids;
    ids.reserve(attrs_or->size());
    for (const AttributeId& a : *attrs_or) {
      ids.push_back(a.id);
    }
    CelValue cv{};
    if (auto s = EncodeUnknownSet(ids, arena_, &cv); !s.ok()) return s;
    mem_.WriteCelValue(out_slot_, cv);
    return absl::OkStatus();
  }
  return EncodeValueToSlot(v, out_slot_, mem_, refs_, arena_);
}

}  // namespace celwasm
