#include "compiler_v2/api/instance.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/attribute.h"
#include "compiler_v2/api/error.h"
#include "compiler_v2/api/internal/abi_decode.h"
#include "compiler_v2/api/internal/cel_host.h"
#include "compiler_v2/api/internal/instance_impl.h"
#include "compiler_v2/api/internal/wasmtime_engine_state.h"
#include "compiler_v2/api/value.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/runtime/cel_data.h"
#include "google/protobuf/message.h"
#include "wasm.h"
#include "wasmtime.h"

namespace cel {

namespace {

// Status helpers — same shape as engine.cc's; kept local here to
// avoid coupling instance.cc to engine.cc just for two helpers.
absl::Status WasmtimeErrorToStatus(absl::string_view context,
                                   wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  absl::Status s = absl::FailedPreconditionError(
      absl::StrCat(context, ": ", absl::string_view(msg.data, msg.size)));
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return s;
}

absl::Status WasmTrapToStatus(absl::string_view context, wasm_trap_t* trap) {
  wasm_byte_vec_t msg;
  wasm_trap_message(trap, &msg);
  absl::Status s = absl::InternalError(
      absl::StrCat(context, ": ", absl::string_view(msg.data, msg.size)));
  wasm_byte_vec_delete(&msg);
  wasm_trap_delete(trap);
  return s;
}

// Read `len` bytes at `offset` from the host-owned memory.
// Returns OutOfRange if the span exceeds memory size.
absl::Status ReadMemBytes(wasmtime_context_t* ctx, const wasmtime_memory_t& mem,
                          uint32_t offset, uint32_t len, void* dst) {
  wasmtime_memory_t m = mem;
  const std::size_t size = wasmtime_memory_data_size(ctx, &m);
  if (static_cast<std::uint64_t>(offset) + len > size) {
    return absl::OutOfRangeError(absl::StrCat("ReadMemBytes: [", offset, ", ",
                                              offset + len,
                                              ") exceeds memory size ", size));
  }
  const std::uint8_t* base = wasmtime_memory_data(ctx, &m);
  std::memcpy(dst, base + offset, len);
  return absl::OkStatus();
}

absl::StatusOr<std::string> ReadMemString(wasmtime_context_t* ctx,
                                          const wasmtime_memory_t& mem,
                                          uint32_t offset, uint32_t len) {
  wasmtime_memory_t m = mem;
  const std::size_t size = wasmtime_memory_data_size(ctx, &m);
  if (static_cast<std::uint64_t>(offset) + len > size) {
    return absl::OutOfRangeError(absl::StrCat("ReadMemString: [", offset, ", ",
                                              offset + len,
                                              ") exceeds memory size ", size));
  }
  const std::uint8_t* base = wasmtime_memory_data(ctx, &m);
  return std::string(reinterpret_cast<const char*>(base + offset), len);
}

absl::StatusOr<Value> DecodeCelValueAt(wasmtime_context_t* ctx,
                                       const wasmtime_memory_t& mem,
                                       const celwasm::ExternrefTable& refs,
                                       uint32_t offset);

// Decode an arena list (CEL_LIST_ARENA) by reading its
// `ArenaListHeader` and recursively decoding `count` × 24-byte
// CelValue elements out of the elements run.  Each element decodes
// through `DecodeCelValueAt`, so list values can themselves be
// scalars or nested aggregates.  Wraps the result in a
// `cel::Value::List(...)` (vector-backed `HostList`).
absl::StatusOr<Value> DecodeArenaListAt(wasmtime_context_t* ctx,
                                        const wasmtime_memory_t& mem,
                                        const celwasm::ExternrefTable& refs,
                                        uint32_t header_ptr) {
  ArenaListHeader header;
  if (auto s = ReadMemBytes(ctx, mem, header_ptr, sizeof(header), &header);
      !s.ok()) {
    return s;
  }
  std::vector<Value> elements;
  elements.reserve(header.count);
  for (uint32_t i = 0; i < header.count; ++i) {
    const uint32_t elem_off =
        header.elements_offset + (i * kCelListEntryStride);
    auto e_or = DecodeCelValueAt(ctx, mem, refs, elem_off);
    if (!e_or.ok()) return e_or.status();
    elements.push_back(*std::move(e_or));
  }
  return Value::List(std::move(elements));
}

// Decode an arena map (CEL_MAP_ARENA) by reading its
// `ArenaMapHeader` and recursively decoding `count` (key, value)
// CelValue pairs out of the entries run.  Pairs decode through
// `DecodeCelValueAt`, so map values can themselves be scalars or
// nested aggregates (M3 has no nested maps from codegen, but a
// host-bound map in a future milestone could).  Wraps the result
// in a `cel::Value::Map(...)` (vector-backed `HostMap`).
absl::StatusOr<Value> DecodeArenaMapAt(wasmtime_context_t* ctx,
                                       const wasmtime_memory_t& mem,
                                       const celwasm::ExternrefTable& refs,
                                       uint32_t header_ptr) {
  ArenaMapHeader header;
  if (auto s = ReadMemBytes(ctx, mem, header_ptr, sizeof(header), &header);
      !s.ok()) {
    return s;
  }
  std::vector<std::pair<Value, Value>> entries;
  entries.reserve(header.count);
  for (uint32_t i = 0; i < header.count; ++i) {
    const uint32_t entry_off = header.entries_offset + (i * kCelMapEntryStride);
    auto k_or = DecodeCelValueAt(ctx, mem, refs, entry_off);
    if (!k_or.ok()) return k_or.status();
    auto v_or = DecodeCelValueAt(ctx, mem, refs, entry_off + sizeof(CelValue));
    if (!v_or.ok()) return v_or.status();
    entries.emplace_back(*std::move(k_or), *std::move(v_or));
  }
  return Value::Map(std::move(entries));
}

// M7.F (encoder polish): decode a CEL_LIST_HOST CelValue.  The
// payload's `ref_slot` points at a `HostListBacking` interned by
// the host trampoline (e.g. `ProtoList` from a proto repeated
// field read).  Walk via `ForEach` to collect each element as a
// `cel::Value`, then re-wrap in a fresh vector-backed `HostList`
// for the user — the original backing's lifetime is per-Eval
// (cleared on `ExternrefTable::Reset()`), so the decoded `Value`
// must own its element-side state.
absl::StatusOr<Value> DecodeHostListAt(const celwasm::ExternrefTable& refs,
                                       uint32_t ref_slot) {
  const celwasm::HostListBacking* backing = refs.LookupList(ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Eval: CEL_LIST_HOST ref_slot=", ref_slot, " has no externref entry"));
  }
  std::vector<Value> elements;
  elements.reserve(backing->Size());
  backing->ForEach([&elements](const Value& v) {
    elements.push_back(v);
  });
  return Value::List(std::move(elements));
}

// M7.F: decode a CEL_MESSAGE CelValue.  The payload's `msg_slot`
// points at a `HostMessageBacking` interned by the host trampoline
// (e.g. `OwnedProtoBacking` from a `cel_make_message` literal, or
// `ProtoBacking` from an Activation::Bind).  Per-Eval lifetime: the
// backing goes away on `ExternrefTable::Reset()` between Evals, so
// the decoded `Value` must own its message — `CopyFrom` into a
// fresh heap message wrapped in `Value::OwnedMessage`.
//
// Conformance rows like `TestAllTypes{single_int32: -34}` (with no
// trailing field read) hit this path: $eval's root is the
// constructed message itself; without this arm `Instance::Eval`
// would trap with "kind 10 not yet supported".
absl::StatusOr<Value> DecodeHostMessageAt(const celwasm::ExternrefTable& refs,
                                          uint32_t ref_slot) {
  const celwasm::HostMessageBacking* backing = refs.Lookup(ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Eval: CEL_MESSAGE msg_slot=", ref_slot, " has no externref entry"));
  }
  const google::protobuf::Message* src = backing->message();
  if (src == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("Eval: CEL_MESSAGE msg_slot=", ref_slot,
                     " backing has no proto message"));
  }
  std::unique_ptr<google::protobuf::Message> copy(src->New());
  copy->CopyFrom(*src);
  return Value::OwnedMessage(std::move(copy));
}

// M7.F: decode a CEL_MAP_HOST CelValue.  Mirrors `DecodeHostListAt`
// — walk via `ForEach((k, v))` and re-wrap in a vector-backed
// `HostMap`.  Same per-Eval-lifetime concern: backing goes away on
// `ExternrefTable::Reset()`, so the decoded entries must be owned
// by the returned `Value`.
absl::StatusOr<Value> DecodeHostMapAt(const celwasm::ExternrefTable& refs,
                                      uint32_t ref_slot) {
  const celwasm::HostMapBacking* backing = refs.LookupMap(ref_slot);
  if (backing == nullptr) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Eval: CEL_MAP_HOST ref_slot=", ref_slot, " has no externref entry"));
  }
  std::vector<std::pair<Value, Value>> entries;
  entries.reserve(backing->Size());
  backing->ForEach([&entries](const Value& k, const Value& v) {
    entries.emplace_back(k, v);
  });
  return Value::Map(std::move(entries));
}

// Decodes a CEL_ERROR-kind CelValue into a `Value::Error`.  Runtime
// `CEL_ERR_*` numerics mirror host `ErrorCode` 1:1 (cel_data.h ↔
// error.h), so a direct cast is correct; an unknown wire byte falls
// through to `kHostAdapterError` rather than crashing the decoder.
Value DecodeCelError(const CelValue& cv) {
  ErrorPayload p;
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

// Decode a 24-byte CelValue at `offset` in linear memory into a
// `cel::Value`.  Scalars + null + arena maps + arena lists land in
// M1/M3/M4; M7.F adds the host-backed list / map arms (via the
// per-Instance ExternrefTable threaded through `refs`).
absl::StatusOr<Value> DecodeCelValueAt(wasmtime_context_t* ctx,
                                       const wasmtime_memory_t& mem,
                                       const celwasm::ExternrefTable& refs,
                                       uint32_t offset) {
  CelValue cv;
  if (auto s = ReadMemBytes(ctx, mem, offset, sizeof(cv), &cv); !s.ok()) {
    return s;
  }
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
    case CEL_STRING: {
      auto bytes_or =
          ReadMemString(ctx, mem, cv.payload.s.ptr, cv.payload.s.len);
      if (!bytes_or.ok()) return bytes_or.status();
      return Value::String(*std::move(bytes_or));
    }
    case CEL_BYTES: {
      auto bytes_or =
          ReadMemString(ctx, mem, cv.payload.bytes.ptr, cv.payload.bytes.len);
      if (!bytes_or.ok()) return bytes_or.status();
      return Value::Bytes(*std::move(bytes_or));
    }
    case CEL_LIST_ARENA:
      return DecodeArenaListAt(ctx, mem, refs,
                               cv.payload.arena_list.header_ptr);
    case CEL_MAP_ARENA:
      return DecodeArenaMapAt(ctx, mem, refs, cv.payload.arena_map.header_ptr);
    case CEL_LIST_HOST:
      return DecodeHostListAt(refs, cv.payload.ref_slot);
    case CEL_MAP_HOST:
      return DecodeHostMapAt(refs, cv.payload.ref_slot);
    case CEL_MESSAGE:
      return DecodeHostMessageAt(refs, cv.payload.msg_slot);
    case CEL_UNKNOWN:
      // M2.E: PartialEval surfaces CEL_UNKNOWN with the
      // attribute_id stamped in payload.unk.  Reconstruct an
      // AttributeId carrying that wire id; embedders compare it
      // against the slot they queried via PartialEval.
      return Value::Unknown(AttributeId{cv.payload.unk});
    case CEL_ERROR:
      return DecodeCelError(cv);
    case CEL_TYPE: {
      // M9.A: type-of-types — payload.s carries (ptr, len) of the
      // type-name string in linear memory.  Copy bytes out into an
      // owned std::string so the returned Value is detachable from
      // the per-Eval arena lifetime.
      auto bytes_or =
          ReadMemString(ctx, mem, cv.payload.s.ptr, cv.payload.s.len);
      if (!bytes_or.ok()) return bytes_or.status();
      return Value::Type(*std::move(bytes_or));
    }
    case CEL_DURATION:
      // m7b §4.6 — CelDurTs uses sign-correlated (seconds, nanos)
      // per Probe D; absl::Seconds(s) + absl::Nanoseconds(ns) is the
      // canonical reconstruction since absl::Duration shares the
      // sign-correlated convention with proto Duration text format.
      return Value::Duration(absl::Seconds(cv.payload.dur.seconds) +
                             absl::Nanoseconds(cv.payload.dur.nanos));
    case CEL_TIMESTAMP:
      return Value::Timestamp(absl::UnixEpoch() +
                              absl::Seconds(cv.payload.ts.seconds) +
                              absl::Nanoseconds(cv.payload.ts.nanos));
    default:
      return absl::InvalidArgumentError(absl::StrCat(
          "Eval returned a CelValue kind ", static_cast<int>(cv.kind),
          " not yet supported by Instance::Eval"));
  }
}

// ─────────────────────────────────────────────────────────────
// Activation → CelValue marshal.
//
// For each declared variable the ABI lists, look up in the
// activation, encode the Value into a 24-byte CelValue, and write
// it into the variable's workspace slot.  Scalars (bool / int /
// uint / double / null) encode inline; aggregates (kMessage / kList)
// route through `ExternrefTable`.
//
// String / bytes — Slice 0 of the conformance unlock plan:
// the bound payload bytes can NOT live in the wasm-side `cel_alloc`
// arena, because `$eval`'s prelude calls `cel_reset` which rewinds
// the bump pointer to `arena_base`, and the first in-eval
// `cel_alloc` then zero-fills the bytes we just wrote there.
// Instead, we maintain a **host-managed string arena** in linear
// memory above `arena_limit` (the codegen's `cel_reset` second arg
// — set to the same value as the host's initial memory size).
// `wasmtime_memory_grow` extends linear memory beyond that
// threshold; the runtime never touches the tail because every
// `cel_alloc` bounds-checks against `arena_limit`.  See
// `EnsureHostStringArenaCapacity` below.
//
// Repr::kMap / kDuration / kTimestamp / kEnum / kType / kUnknown
// activation marshalling lands in M7-era work; fail loud until then.
// ─────────────────────────────────────────────────────────────

// Wasm pages are always 64KiB.  Mirror the `wasmtime_memorytype_new`
// `page_size_log2=16` choice in engine.cc.
constexpr uint32_t kWasmPageSize = 64u * 1024u;

// Round `bytes` up to the next multiple of `kWasmPageSize`.
uint32_t RoundUpToPage(uint64_t bytes) {
  return static_cast<uint32_t>(((bytes + kWasmPageSize - 1) / kWasmPageSize) *
                               kWasmPageSize);
}

// Ensure the host-side string arena is initialized + has at least
// `needed` bytes of capacity above the arena_limit floor.  Captures
// `arena_floor` lazily on first call (= the byte size of the host
// memory at instantiation, which is exactly what codegen baked into
// `cel_reset(arena_base, arena_limit)`'s second arg).  Grows the
// memory by whole pages on demand.  Returns ResourceExhausted if
// `wasmtime_memory_grow` rejects the request (engine memorytype was
// created with `max_present=false`, so this should only happen on
// genuine address-space exhaustion).
absl::Status EnsureHostStringArenaCapacity(wasmtime_context_t* ctx,
                                           const wasmtime_memory_t& mem,
                                           absl::string_view first_var_name,
                                           uint32_t* absl_nonnull floor,
                                           uint32_t* absl_nonnull capacity,
                                           uint32_t needed) {
  wasmtime_memory_t m = mem;
  if (*floor == 0) {
    // First call — record the initial mem size (= arena_limit).
    *floor = static_cast<uint32_t>(wasmtime_memory_data_size(ctx, &m));
  }
  if (needed <= *capacity) return absl::OkStatus();

  const uint32_t new_capacity = RoundUpToPage(needed);
  const uint32_t delta_bytes = new_capacity - *capacity;
  const uint32_t delta_pages = delta_bytes / kWasmPageSize;
  uint64_t prev_pages = 0;
  wasmtime_error_t* err =
      wasmtime_memory_grow(ctx, &m, delta_pages, &prev_pages);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(
        absl::StrCat("Activation[", first_var_name,
                     "]: host-arena memory.grow(", delta_pages, " pages)"),
        err);
  }
  *capacity = new_capacity;
  return absl::OkStatus();
}

// Forward decl — defined further down with the other per-Repr
// encoders.  EncodeStringOrBytes ships its own kind check via this.
absl::Status KindMismatch(absl::string_view name, absl::string_view declared,
                          Value::Kind got);

// Bundle of host arena state that the kString / kBytes encoder
// reads + advances.  Kept here (not on `EncoderContext` below) so a
// caller that doesn't drive the arena (every other Repr) doesn't
// have to fabricate an unused slot.
struct HostStringArena {
  wasmtime_context_t* absl_nonnull ctx;
  wasmtime_memory_t mem;
  uint32_t floor;
  uint32_t capacity;
  uint32_t* absl_nonnull cursor;  // bytes used since `floor`.
};

// Encode a kString / kBytes value: copy the payload bytes into the
// host string arena and write the offset+len into the CelValue.
// `arena.cursor` advances by `aligned_len` so the next encoder
// starts at a clean 8-byte boundary.
absl::Status EncodeStringOrBytes(const Value& v, absl::string_view name,
                                 celwasm::Repr repr, CelValue* dst,
                                 HostStringArena arena) {
  const Value::Kind expected = repr == celwasm::Repr::kString
                                   ? Value::Kind::kString
                                   : Value::Kind::kBytes;
  if (v.kind() != expected) {
    return KindMismatch(
        name, repr == celwasm::Repr::kString ? "string" : "bytes", v.kind());
  }
  auto sv_or = repr == celwasm::Repr::kString ? v.AsString() : v.AsBytes();
  if (!sv_or.ok()) return sv_or.status();
  const absl::string_view sv = *sv_or;
  const auto len = static_cast<uint32_t>(sv.size());
  const uint32_t aligned = (len + 7u) & ~uint32_t{7u};

  if (static_cast<uint64_t>(*arena.cursor) + aligned > arena.capacity) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "Activation[", name, "]: host string arena OOM (need ", aligned,
        " bytes; cursor=", *arena.cursor, ", capacity=", arena.capacity, ")"));
  }
  const uint32_t offset = arena.floor + *arena.cursor;
  if (len > 0) {
    wasmtime_memory_t m = arena.mem;
    uint8_t* base = wasmtime_memory_data(arena.ctx, &m);
    std::memcpy(base + offset, sv.data(), len);
  }
  *arena.cursor += aligned;
  dst->kind = repr == celwasm::Repr::kString ? CEL_STRING : CEL_BYTES;
  dst->payload.s.ptr = offset;
  dst->payload.s.len = len;
  return absl::OkStatus();
}

// Per-Repr encoder helpers.  Each returns OK on type match +
// successful write, InvalidArgument on kind mismatch.  Only the
// fields that matter for the kind are written; pad bytes stay zero
// (the caller zeroed `dst` first).

absl::Status KindMismatch(absl::string_view name, absl::string_view declared,
                          Value::Kind got) {
  return absl::InvalidArgumentError(
      absl::StrCat("Activation[", name, "]: declared ", declared, ", bound ",
                   ValueKindName(got)));
}

absl::Status EncodeNull(const Value& v, absl::string_view name, CelValue* dst) {
  if (v.kind() != Value::Kind::kNull) {
    return KindMismatch(name, "null", v.kind());
  }
  dst->kind = CEL_NULL;
  return absl::OkStatus();
}

absl::Status EncodeBool(const Value& v, absl::string_view name, CelValue* dst) {
  if (v.kind() != Value::Kind::kBool) {
    return KindMismatch(name, "bool", v.kind());
  }
  auto b = v.AsBool();
  if (!b.ok()) return b.status();
  dst->kind = CEL_BOOL;
  dst->payload.b = *b ? 1 : 0;
  return absl::OkStatus();
}

absl::Status EncodeInt(const Value& v, absl::string_view name, CelValue* dst) {
  if (v.kind() != Value::Kind::kInt) {
    return KindMismatch(name, "int", v.kind());
  }
  auto i = v.AsInt();
  if (!i.ok()) return i.status();
  dst->kind = CEL_INT;
  dst->payload.i = *i;
  return absl::OkStatus();
}

absl::Status EncodeUint(const Value& v, absl::string_view name, CelValue* dst) {
  if (v.kind() != Value::Kind::kUint) {
    return KindMismatch(name, "uint", v.kind());
  }
  auto u = v.AsUint();
  if (!u.ok()) return u.status();
  dst->kind = CEL_UINT;
  dst->payload.u = *u;
  return absl::OkStatus();
}

absl::Status EncodeDouble(const Value& v, absl::string_view name,
                          CelValue* dst) {
  if (v.kind() != Value::Kind::kDouble) {
    return KindMismatch(name, "double", v.kind());
  }
  auto d = v.AsDouble();
  if (!d.ok()) return d.status();
  dst->kind = CEL_DOUBLE;
  dst->payload.d = *d;
  return absl::OkStatus();
}

// Forward decl of the WKT-message coercion helper defined just
// below `EncodeMessage`.
bool TryEncodeWktTimeMessage(const Value& v, uint32_t want_kind,
                             CelValue* dst);

absl::Status EncodeDuration(const Value& v, absl::string_view name,
                            CelValue* dst) {
  // Accept Value::Message(google.protobuf.Duration) too — the type
  // checker treats it as the same surface as Value::Duration.
  if (TryEncodeWktTimeMessage(v, CEL_DURATION, dst)) {
    return absl::OkStatus();
  }
  if (v.kind() != Value::Kind::kDuration) {
    return KindMismatch(name, "duration", v.kind());
  }
  auto d_or = v.AsDuration();
  if (!d_or.ok()) return d_or.status();
  dst->kind = CEL_DURATION;
  celwasm::DecomposeAbslDuration(*d_or, &dst->payload.dur);
  return absl::OkStatus();
}

absl::Status EncodeTimestamp(const Value& v, absl::string_view name,
                             CelValue* dst) {
  if (TryEncodeWktTimeMessage(v, CEL_TIMESTAMP, dst)) {
    return absl::OkStatus();
  }
  if (v.kind() != Value::Kind::kTimestamp) {
    return KindMismatch(name, "timestamp", v.kind());
  }
  auto t_or = v.AsTimestamp();
  if (!t_or.ok()) return t_or.status();
  dst->kind = CEL_TIMESTAMP;
  celwasm::DecomposeAbslDuration(*t_or - absl::UnixEpoch(), &dst->payload.ts);
  return absl::OkStatus();
}

// M2.C: encode a Value::Message-bound variable into a CEL_MESSAGE
// CelValue.  The bound `HostMessageBacking` is interned into the
// per-Instance `ExternrefTable` and the resulting slot lives in
// `payload.msg_slot`.  The caller is responsible for resetting
// the table between Evals so slot indices don't leak across
// invocations.
// m7b §3.4 — well-known-type bind normaliser.  When a variable is
// declared `google.protobuf.Timestamp` / `Duration` and the bound
// Value is a `Value::Message` carrying the matching WKT proto,
// peel (seconds, nanos) into a CelDurTs payload so the variable
// arrives at codegen as the matching `CEL_TIMESTAMP` /
// `CEL_DURATION` kind.  Returns true and writes `dst` on hit;
// false (so the caller falls through to its normal kind-mismatch
// error path) on miss.
bool TryEncodeWktTimeMessage(const Value& v, uint32_t want_kind,
                             CelValue* dst) {
  if (v.kind() != Value::Kind::kMessage) return false;
  auto backing_or = v.SharedMessageBacking();
  if (!backing_or.ok()) return false;
  const google::protobuf::Message* msg = (*backing_or)->message();
  if (msg == nullptr) return false;
  const google::protobuf::Descriptor* d = msg->GetDescriptor();
  if (d == nullptr) return false;
  const absl::string_view fqn = d->full_name();
  const bool is_timestamp = want_kind == CEL_TIMESTAMP &&
                            fqn == "google.protobuf.Timestamp";
  const bool is_duration = want_kind == CEL_DURATION &&
                           fqn == "google.protobuf.Duration";
  if (!is_timestamp && !is_duration) return false;
  const google::protobuf::Reflection* refl = msg->GetReflection();
  const google::protobuf::FieldDescriptor* sf = d->FindFieldByNumber(1);
  const google::protobuf::FieldDescriptor* nf = d->FindFieldByNumber(2);
  if (refl == nullptr || sf == nullptr || nf == nullptr) return false;
  dst->kind = want_kind;
  dst->payload.dur = CelDurTs{.seconds = refl->GetInt64(*msg, sf),
                              .nanos = refl->GetInt32(*msg, nf),
                              ._pad = 0};
  return true;
}

absl::Status EncodeMessage(const Value& v, absl::string_view name,
                           CelValue* dst, celwasm::ExternrefTable& refs) {
  if (v.kind() != Value::Kind::kMessage) {
    return KindMismatch(name, "message", v.kind());
  }
  auto backing_or = v.SharedMessageBacking();
  if (!backing_or.ok()) return backing_or.status();
  const uint32_t slot = refs.Intern(*std::move(backing_or));
  dst->kind = CEL_MESSAGE;
  dst->payload.msg_slot = slot;
  return absl::OkStatus();
}

// M4.H: encode a Value::List / Value::HostList-bound variable into
// a CEL_LIST_HOST CelValue.  The bound `HostListBacking` is
// interned into the per-Instance `ExternrefTable` (independent
// `list_backings_` namespace from messages and maps) and the
// resulting slot lives in `payload.ref_slot`.  Same shape as
// `EncodeMessage`, but routes through `InternList` so the
// trampoline's `LookupList` finds it.
absl::Status EncodeList(const Value& v, absl::string_view name, CelValue* dst,
                        celwasm::ExternrefTable& refs) {
  if (v.kind() != Value::Kind::kList) {
    return KindMismatch(name, "list", v.kind());
  }
  auto backing_or = v.SharedListBacking();
  if (!backing_or.ok()) return backing_or.status();
  const uint32_t slot = refs.InternList(*std::move(backing_or));
  dst->kind = CEL_LIST_HOST;
  dst->payload.ref_slot = slot;
  return absl::OkStatus();
}

// M9.A: encode a Value::Type-bound variable into a CEL_TYPE CelValue.
// The bound name string is copied into the host string arena above
// `arena_limit` (same arena kString / kBytes use); the resulting
// CelSpan lives in `payload.s`.  Same lifetime as kString — bytes
// outlive `cel_reset` because the arena floor is fixed at instantiation
// time.
absl::Status EncodeType(const Value& v, absl::string_view name, CelValue* dst,
                        HostStringArena arena) {
  if (v.kind() != Value::Kind::kType) {
    return KindMismatch(name, "type", v.kind());
  }
  auto sv_or = v.AsType();
  if (!sv_or.ok()) return sv_or.status();
  const absl::string_view sv = *sv_or;
  const auto len = static_cast<uint32_t>(sv.size());
  const uint32_t aligned = (len + 7u) & ~uint32_t{7u};

  if (static_cast<uint64_t>(*arena.cursor) + aligned > arena.capacity) {
    return absl::ResourceExhaustedError(
        absl::StrCat("Activation[", name, "]: host string arena OOM (need ",
                     aligned, " bytes for type name; cursor=", *arena.cursor,
                     ", capacity=", arena.capacity, ")"));
  }
  const uint32_t offset = arena.floor + *arena.cursor;
  if (len > 0) {
    wasmtime_memory_t m = arena.mem;
    uint8_t* base = wasmtime_memory_data(arena.ctx, &m);
    std::memcpy(base + offset, sv.data(), len);
  }
  *arena.cursor += aligned;
  dst->kind = CEL_TYPE;
  dst->payload.s.ptr = offset;
  dst->payload.s.len = len;
  return absl::OkStatus();
}

// Bundle of host-side state the variable encoder needs.  Reduces
// the per-Repr dispatch's parameter count below the lint gate; only
// the kString / kBytes path actually consults `arena`, but the
// bundle keeps the call sites uniform.
struct EncoderContext {
  celwasm::ExternrefTable& refs;
  HostStringArena arena;
};

// Dispatch a declared Repr to the right per-kind encoder.  Slice 0
// adds the kString / kBytes arms — payload bytes land in the host
// string arena above `arena_limit`.  kMap / kDuration /
// kTimestamp / kEnum / kType / kUnknown stay unimplemented pending
// later milestones.
absl::Status EncodeBoundValue(const Value& v, celwasm::Repr repr,
                              absl::string_view name, CelValue* dst,
                              EncoderContext& ec) {
  switch (repr) {
    case celwasm::Repr::kNull:
      return EncodeNull(v, name, dst);
    case celwasm::Repr::kBool:
      return EncodeBool(v, name, dst);
    case celwasm::Repr::kInt:
      return EncodeInt(v, name, dst);
    case celwasm::Repr::kUint:
      return EncodeUint(v, name, dst);
    case celwasm::Repr::kDouble:
      return EncodeDouble(v, name, dst);
    case celwasm::Repr::kMessage:
      return EncodeMessage(v, name, dst, ec.refs);
    case celwasm::Repr::kList:
      return EncodeList(v, name, dst, ec.refs);
    case celwasm::Repr::kString:
    case celwasm::Repr::kBytes:
      return EncodeStringOrBytes(v, name, repr, dst, ec.arena);
    case celwasm::Repr::kType:
      // M9.A: type-of-types bind through the host string arena
      // (same path as kString / kBytes — see `EncodeType`).
      return EncodeType(v, name, dst, ec.arena);
    case celwasm::Repr::kDuration:
      return EncodeDuration(v, name, dst);
    case celwasm::Repr::kTimestamp:
      return EncodeTimestamp(v, name, dst);
    case celwasm::Repr::kMap:
    case celwasm::Repr::kEnum:
    case celwasm::Repr::kUnknown:
      return absl::UnimplementedError(absl::StrCat(
          "Activation[", name, "]: Repr=", celwasm::ReprName(repr),
          " marshal not implemented (later milestones for "
          "map/enum/unknown)"));
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "Activation[", name, "]: unknown Repr=", static_cast<int>(repr)));
}

// Write CelValue `cv` into linear memory at `offset`.  Caller must
// have validated `offset + sizeof(CelValue) <= mem_size`.
void WriteCelValueAt(wasmtime_context_t* ctx, const wasmtime_memory_t& mem,
                     uint32_t offset, const CelValue& cv) {
  wasmtime_memory_t m = mem;
  uint8_t* base = wasmtime_memory_data(ctx, &m);
  std::memcpy(base + offset, &cv, sizeof(cv));
}

// Sum the aligned-up byte sizes of every kString / kBytes / kType
// activation binding.  Used pre-pass to know how much host arena to
// reserve before any encoder writes — growing memory mid-loop would
// invalidate any previously-cached `wasmtime_memory_data` pointer.
uint32_t TotalHostStringBytes(const celwasm::abi::CelAbi& abi,
                              const Activation& activation) {
  uint32_t total = 0;
  for (const celwasm::abi::VariableEntry& dv : abi.variables()) {
    const celwasm::Repr repr = celwasm::DecodeRepr(dv.repr());
    if (repr != celwasm::Repr::kString && repr != celwasm::Repr::kBytes &&
        repr != celwasm::Repr::kType) {
      continue;
    }
    const Value* bound = activation.Find(dv.name());
    if (bound == nullptr) continue;  // missing variable surfaces below.
    if (repr == celwasm::Repr::kString &&
        bound->kind() == Value::Kind::kString) {
      total += static_cast<uint32_t>(bound->AsString()->size());
    } else if (repr == celwasm::Repr::kBytes &&
               bound->kind() == Value::Kind::kBytes) {
      total += static_cast<uint32_t>(bound->AsBytes()->size());
    } else if (repr == celwasm::Repr::kType &&
               bound->kind() == Value::Kind::kType) {
      // M9.A: type-name strings live in the same host arena as
      // kString/kBytes payloads (see EncodeType in cel_host.cc).
      total += static_cast<uint32_t>(bound->AsType()->size());
    }
    total = (total + 7u) & ~uint32_t{7u};
  }
  return total;
}

// For every variable declared in the decoded ABI, look up its bound
// Value in the activation, encode, and write to its workspace slot.
// Missing variable → FailedPrecondition.  Type mismatch between
// declared Repr and bound Value::Kind → InvalidArgument.
//
// Takes the whole `InstanceImpl` so the host-string-arena bookkeeping
// (`host_string_arena_floor` / `host_string_arena_capacity`) lives at
// instance scope without inflating MarshalActivation's parameter list
// past the lint gate.
absl::Status MarshalActivation(wasmtime_context_t* absl_nonnull ctx,
                               celwasm::InstanceImpl* absl_nonnull impl,
                               const Activation& activation) {
  const wasmtime_memory_t& mem = impl->memory;
  const celwasm::abi::CelAbi& abi = impl->abi;

  // Pre-pass: ensure host string arena has room for every kString /
  // kBytes payload before any encoder runs.  Memory.grow can move
  // wasmtime_memory_data's pointer; doing the grow up-front means
  // every per-variable encoder sees a stable base pointer.
  const uint32_t need = TotalHostStringBytes(abi, activation);
  if (need > 0) {
    const absl::string_view first_name = abi.variables_size() > 0
                                             ? abi.variables(0).name()
                                             : absl::string_view{};
    if (auto s = EnsureHostStringArenaCapacity(
            ctx, mem, first_name, &impl->host_string_arena_floor,
            &impl->host_string_arena_capacity, need);
        !s.ok()) {
      return s;
    }
  }
  uint32_t arena_cursor = 0;
  EncoderContext ec{
      impl->host_env.refs,
      HostStringArena{ctx, mem, impl->host_string_arena_floor,
                      impl->host_string_arena_capacity, &arena_cursor}};

  // Re-read mem_size AFTER any grow — workspace bounds checks below
  // need the fresh size, and the slot offsets are below `arena_floor`
  // so they always live in the original memory region.
  const size_t mem_size = [&]() {
    wasmtime_memory_t m = mem;
    return wasmtime_memory_data_size(ctx, &m);
  }();

  for (const celwasm::abi::VariableEntry& dv : abi.variables()) {
    if (static_cast<std::uint64_t>(dv.slot_offset()) + sizeof(CelValue) >
        mem_size) {
      return absl::OutOfRangeError(absl::StrCat(
          "Activation[", dv.name(), "]: slot offset ", dv.slot_offset(),
          " + 24 exceeds memory size ", mem_size));
    }
    const Value* bound = activation.Find(dv.name());
    if (bound == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("Activation: variable `", dv.name(),
                       "` declared on Compiler but not bound on Activation"));
    }
    CelValue cv{};
    if (auto s = EncodeBoundValue(*bound, celwasm::DecodeRepr(dv.repr()),
                                  dv.name(), &cv, ec);
        !s.ok()) {
      return s;
    }
    WriteCelValueAt(ctx, mem, dv.slot_offset(), cv);
  }
  return absl::OkStatus();
}

}  // namespace

// Both fields ARE initialized in the member-init list below; the
// `cppcoreguidelines-pro-type-member-init` warning is a false
// positive triggered by `impl_` being a unique_ptr to a forward-
// declared type at the header.
Instance::Instance(  // NOLINT(cppcoreguidelines-pro-type-member-init)
    std::shared_ptr<celwasm::WasmtimeEngineState> wasmtime,
    std::unique_ptr<celwasm::InstanceImpl> impl)
    : wasmtime_(std::move(wasmtime)), impl_(std::move(impl)) {}

Instance::~Instance() = default;
Instance::Instance(Instance&&) noexcept = default;
Instance& Instance::operator=(Instance&&) noexcept = default;

std::size_t Instance::memory_size_bytes() const {
  wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);
  wasmtime_memory_t mem = impl_->memory;
  return wasmtime_memory_data_size(ctx, &mem);
}

absl::StatusOr<Value> Instance::Eval() {
  wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);
  wasmtime_func_t fn = impl_->eval_fn;
  wasmtime_val_t result{};
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_func_call(ctx, &fn, /*args=*/nullptr, /*nargs=*/0, &result,
                         /*nresults=*/1, &trap);
  if (err != nullptr) return WasmtimeErrorToStatus("Eval (func_call)", err);
  if (trap != nullptr) return WasmTrapToStatus("Eval trapped", trap);
  if (result.kind != WASMTIME_I32) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Eval: $eval returned non-i32 (kind=", static_cast<int>(result.kind),
        ")"));
  }
  return DecodeCelValueAt(ctx, impl_->memory, impl_->host_env.refs,
                          static_cast<uint32_t>(result.of.i32));
}

absl::StatusOr<Value> Instance::Eval(const Activation& activation) {
  wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);
  // Reset the externref table before each Eval so message slots
  // from prior evals don't leak into this one (per the
  // ExternrefTable contract: monotonic per-Eval, Reset() clears
  // between Evals).  Also clear any unknown patterns left from a
  // prior PartialEval — empty-pattern set is the Eval contract.
  impl_->host_env.refs.Reset();
  impl_->host_env.bindings.unknown_patterns = {};

  // For every declared variable, look up in the activation and
  // write its CelValue into the pre-assigned workspace slot.  This
  // must run BEFORE the $eval call — $eval's prelude writes each
  // slot offset into a wasm local, then the body reads
  // `local.get local_index` to get the offset of the CelValue we
  // just placed.
  if (auto s = MarshalActivation(ctx, impl_.get(), activation); !s.ok()) {
    return s;
  }
  return Eval();
}

absl::StatusOr<Value> Instance::PartialEval(
    const Activation& activation, absl::Span<const AttributePattern> unknowns) {
  wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);

  // Reset per-eval state.  Unlike Eval(), we then populate
  // `unknown_patterns` so the cel_host trampoline shorts to
  // CEL_UNKNOWN on FULL pattern matches.
  impl_->host_env.refs.Reset();
  impl_->host_env.bindings.unknown_patterns = unknowns;

  if (auto s = MarshalActivation(ctx, impl_.get(), activation); !s.ok()) {
    // Reset before bailing — the next call must see a clean state
    // even on this failure path.
    impl_->host_env.bindings.unknown_patterns = {};
    return s;
  }
  auto result = Eval();
  // Eval() resets unknown_patterns itself, but only at the *next*
  // Eval invocation.  Clear here too so a follow-up Eval() that
  // never gets called doesn't observe stale state.
  impl_->host_env.bindings.unknown_patterns = {};
  return result;
}

}  // namespace cel
