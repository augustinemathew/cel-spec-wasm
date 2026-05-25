#include "compiler_v2/api/instance.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/log/absl_check.h"
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
#include "compiler_v2/runtime/cel_layout.h"
#include "google/protobuf/message.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm::api {

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
absl::Status ReadMemBytes(wasmtime_context_t* ctx, wasmtime_sharedmemory_t* mem,
                          uint32_t offset, uint32_t len, void* dst) {
  (void)ctx;
  const std::size_t size = wasmtime_sharedmemory_data_size(mem);
  if (static_cast<std::uint64_t>(offset) + len > size) {
    return absl::OutOfRangeError(absl::StrCat("ReadMemBytes: [", offset, ", ",
                                              offset + len,
                                              ") exceeds memory size ", size));
  }
  const std::uint8_t* base = wasmtime_sharedmemory_data(mem);
  std::memcpy(dst, base + offset, len);
  return absl::OkStatus();
}

absl::StatusOr<std::string> ReadMemString(wasmtime_context_t* ctx,
                                          wasmtime_sharedmemory_t* mem,
                                          uint32_t offset, uint32_t len) {
  (void)ctx;
  const std::size_t size = wasmtime_sharedmemory_data_size(mem);
  if (static_cast<std::uint64_t>(offset) + len > size) {
    return absl::OutOfRangeError(absl::StrCat("ReadMemString: [", offset, ", ",
                                              offset + len,
                                              ") exceeds memory size ", size));
  }
  const std::uint8_t* base = wasmtime_sharedmemory_data(mem);
  return std::string(reinterpret_cast<const char*>(base + offset), len);
}

absl::StatusOr<Value> DecodeCelValueAt(wasmtime_context_t* ctx,
                                       wasmtime_sharedmemory_t* mem,
                                       const celwasm::ExternrefTable& refs,
                                       uint32_t offset);

// Decode an arena list (CEL_LIST_ARENA) by reading its
// `ArenaListHeader` and recursively decoding `count` × 24-byte
// CelValue elements out of the elements run.  Each element decodes
// through `DecodeCelValueAt`, so list values can themselves be
// scalars or nested aggregates.  Wraps the result in a
// `celwasm::api::Value::List(...)` (vector-backed `HostList`).
absl::StatusOr<Value> DecodeArenaListAt(wasmtime_context_t* ctx,
                                        wasmtime_sharedmemory_t* mem,
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
// in a `celwasm::api::Value::Map(...)` (vector-backed `HostMap`).
absl::StatusOr<Value> DecodeArenaMapAt(wasmtime_context_t* ctx,
                                       wasmtime_sharedmemory_t* mem,
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

// Decode a CEL_LIST_HOST CelValue.  The payload's `ref_slot`
// points at a `HostListBacking` interned by
// the host trampoline (e.g. `ProtoList` from a proto repeated
// field read).  Walk via `ForEach` to collect each element as a
// `celwasm::api::Value`, then re-wrap in a fresh vector-backed `HostList`
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

// Decode a CEL_MESSAGE CelValue.  The payload's `msg_slot`
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

// Decode a CEL_MAP_HOST CelValue.  Mirrors `DecodeHostListAt`
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
// `celwasm::api::Value`.  Covers scalars, null, arena maps/lists, and
// host-backed list / map arms (via the per-Instance ExternrefTable
// threaded through `refs`).
absl::StatusOr<Value> DecodeCelValueAt(wasmtime_context_t* ctx,
                                       wasmtime_sharedmemory_t* mem,
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
      // PartialEval surfaces CEL_UNKNOWN with the attribute_id
      // stamped in payload.unk.  Reconstruct an AttributeId
      // carrying that wire id; embedders compare it against the
      // slot they queried via PartialEval.
      return Value::Unknown(celwasm::AttributeId{cv.payload.unk});
    case CEL_ERROR:
      return DecodeCelError(cv);
    case CEL_TYPE: {
      // type-of-types — payload.s carries (ptr, len) of the
      // type-name string in linear memory.  Copy bytes out into an
      // owned std::string so the returned Value is detachable from
      // the per-Eval arena lifetime.  See
      // `rewrite/m9-type-subsystem.md`.
      auto bytes_or =
          ReadMemString(ctx, mem, cv.payload.s.ptr, cv.payload.s.len);
      if (!bytes_or.ok()) return bytes_or.status();
      return Value::Type(*std::move(bytes_or));
    }
    case CEL_DURATION:
      // CelDurTs uses sign-correlated (seconds, nanos) — see
      // `rewrite/m7b-duration-timestamp.md` §4.6.  absl::Seconds(s)
      // + absl::Nanoseconds(ns) is the canonical reconstruction
      // since absl::Duration shares the sign-correlated convention
      // with proto Duration text format.
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
// String / bytes — activation buffer:
// the bound payload bytes can NOT live in the wasm-side
// `arena_alloc` arena, because `$eval`'s prelude calls `arena_reset`
// which rewinds the bump pointer to 0 and the first in-eval
// `arena_alloc` zero-fills the bytes we just wrote there.  Instead,
// we malloc a buffer inside the runtime's linear memory once per
// Instance (via wasm reentry into wasi-libc's dlmalloc) and reuse
// it across Evals.  See `EnsureActivationBuffer` below and
// `rewrite/wasi/DESIGN.md` §6.
// ─────────────────────────────────────────────────────────────

// Round `bytes` up to the next multiple of 4 KB.  4 KB matches the
// minimum dlmalloc chunk size on wasi-libc and amortises the cost
// of growing the activation buffer over many small Evals.
uint32_t RoundUpTo4K(uint64_t bytes) {
  constexpr uint64_t k4K = 4u * 1024u;
  return static_cast<uint32_t>(((bytes + k4K - 1) / k4K) * k4K);
}

// Ensure the per-Instance activation buffer has at least `needed`
// bytes of capacity.  Lazily malloc'd via wasm reentry on first
// need; replaced with a fresh malloc when a later Eval needs more
// (the previous buffer is left to dlmalloc's free list — no
// explicit free since dlmalloc reclaims on the next sized alloc).
//
// Returns ResourceExhausted if wasi-libc's dlmalloc fails to grow
// linear memory enough to satisfy the request — i.e. genuine wasm
// address-space exhaustion (4 GB cap).
absl::Status EnsureActivationBuffer(wasmtime_context_t* ctx,
                                    wasmtime_func_t malloc_fn,
                                    absl::string_view first_var_name,
                                    uint32_t* absl_nonnull buf_offset,
                                    uint32_t* absl_nonnull buf_capacity,
                                    uint32_t needed) {
  if (needed <= *buf_capacity) return absl::OkStatus();

  // Allocate a 4-KB-rounded buffer; for very small activations this
  // is one malloc and we never grow.
  const uint32_t new_capacity = RoundUpTo4K(needed);
  wasmtime_val_t arg;
  arg.kind = WASMTIME_I32;
  arg.of.i32 = static_cast<int32_t>(new_capacity);
  wasmtime_val_t result;
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_func_call(ctx, &malloc_fn, &arg, 1, &result, 1, &trap);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(absl::StrCat("Activation[", first_var_name,
                                              "]: malloc(", new_capacity, ")"),
                                 err);
  }
  if (trap != nullptr) {
    return WasmTrapToStatus(
        absl::StrCat("Activation[", first_var_name, "]: malloc trap"), trap);
  }
  if (result.kind != WASMTIME_I32 || result.of.i32 == 0) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "Activation[", first_var_name, "]: malloc returned NULL (needed ",
        new_capacity, " bytes)"));
  }
  *buf_offset = static_cast<uint32_t>(result.of.i32);
  *buf_capacity = new_capacity;
  // A15 (DESIGN §5): the malloc'd buffer must sit above the
  // reserved low region (where the expr rodata + wasi-libc data
  // live).  dlmalloc's heap floor is __heap_base, already
  // validated >= kReservedLowMemoryBytes in engine.cc::
  // InstantiateRuntime, so this is an invariant by construction.
  ABSL_CHECK_GE(*buf_offset, CELWASM_RESERVED_LOW_MEMORY_BYTES)
      << "DESIGN A15: activation buffer overlaps reserved region";
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
  wasmtime_sharedmemory_t* absl_nonnull mem;
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
    uint8_t* base = wasmtime_sharedmemory_data(arena.mem);
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

// FQN → CEL_* kind table for the 9 WKT wrappers.  Pulled out of
// `TryEncodeWktWrapperMessage` so the parent stays under the
// readability-function-size gate.  Returns 0 (not a valid CelKind
// for the unwrap path) if `fqn` isn't a wrapper.  Mirrors
// `WrapperKindFromFqn` in `compiler_v2/codegen/expr_lower.cc`.
uint32_t WrapperFqnToCelKind(absl::string_view fqn) {
  if (fqn == "google.protobuf.BoolValue") return CEL_BOOL;
  if (fqn == "google.protobuf.Int32Value" ||
      fqn == "google.protobuf.Int64Value") {
    return CEL_INT;
  }
  if (fqn == "google.protobuf.UInt32Value" ||
      fqn == "google.protobuf.UInt64Value") {
    return CEL_UINT;
  }
  if (fqn == "google.protobuf.FloatValue" ||
      fqn == "google.protobuf.DoubleValue") {
    return CEL_DOUBLE;
  }
  if (fqn == "google.protobuf.StringValue") return CEL_STRING;
  if (fqn == "google.protobuf.BytesValue") return CEL_BYTES;
  return 0;
}

// Peel the inner numeric `value` field of a wrapper-message into a
// matching `CelValue` payload.  Returns true on success; false if
// the cpp_type is one of the string/bytes wrappers (caller handles
// those via the arena-side path) or any non-wrapper cpp_type
// (defence in depth; `WrapperFqnToCelKind` should have gated entry).
bool WriteNumericWrapperPayload(const google::protobuf::Reflection& refl,
                                const google::protobuf::Message& msg,
                                const google::protobuf::FieldDescriptor& vf,
                                CelValue* dst) {
  using FD = google::protobuf::FieldDescriptor;
  switch (vf.cpp_type()) {
    case FD::CPPTYPE_BOOL:
      dst->payload.b = refl.GetBool(msg, &vf) ? 1 : 0;
      return true;
    case FD::CPPTYPE_INT32:
      dst->payload.i = refl.GetInt32(msg, &vf);
      return true;
    case FD::CPPTYPE_INT64:
      dst->payload.i = refl.GetInt64(msg, &vf);
      return true;
    case FD::CPPTYPE_UINT32:
      dst->payload.u = refl.GetUInt32(msg, &vf);
      return true;
    case FD::CPPTYPE_UINT64:
      dst->payload.u = refl.GetUInt64(msg, &vf);
      return true;
    case FD::CPPTYPE_FLOAT:
      dst->payload.d = refl.GetFloat(msg, &vf);
      return true;
    case FD::CPPTYPE_DOUBLE:
      dst->payload.d = refl.GetDouble(msg, &vf);
      return true;
    default:
      // String / bytes: out of band for the numeric fast path.
      return false;
  }
}

// Wrapper-coercion at activation bind.  When a declared variable
// is a wrapper type (`Int32Value`, `BoolValue`, …) the
// `compiler_v2/ir/typed_ast.cc:56` mapping collapses it to the
// matching scalar Repr (see `rewrite/m8-wrapper-types.md`).  When the embedder binds a
// `Value::Message(Int32Value{value: 5})` against an Int32Value-
// declared variable, the Encode path that fires here is `EncodeInt`
// — not `EncodeMessage`.  This helper detects that case and peels
// the inner scalar from the bound wrapper proto.  Returns true and
// writes `dst` on hit; false (caller falls through to its kind-
// mismatch error path) on miss.  `want_kind` gates the FQN match so
// a BoolValue bound against an Int32 variable is rejected as a
// kind mismatch rather than silently coerced.
bool TryEncodeWktWrapperMessage(const Value& v, uint32_t want_kind,
                                CelValue* dst) {
  if (v.kind() != Value::Kind::kMessage) return false;
  auto backing_or = v.SharedMessageBacking();
  if (!backing_or.ok()) return false;
  const google::protobuf::Message* msg = (*backing_or)->message();
  if (msg == nullptr) return false;
  const google::protobuf::Descriptor* d = msg->GetDescriptor();
  if (d == nullptr) return false;
  const uint32_t fqn_kind = WrapperFqnToCelKind(d->full_name());
  if (fqn_kind == 0 || fqn_kind != want_kind) return false;
  const google::protobuf::Reflection* refl = msg->GetReflection();
  const google::protobuf::FieldDescriptor* vf = d->FindFieldByNumber(1);
  if (refl == nullptr || vf == nullptr) return false;
  dst->kind = want_kind;
  return WriteNumericWrapperPayload(*refl, *msg, *vf, dst);
}

absl::Status EncodeNull(const Value& v, absl::string_view name, CelValue* dst) {
  if (v.kind() != Value::Kind::kNull) {
    return KindMismatch(name, "null", v.kind());
  }
  dst->kind = CEL_NULL;
  return absl::OkStatus();
}

// Wrapper-bind semantics: a `Value::Null()` bound against a
// wrapper-typed declared variable (which `typed_ast.cc:56` has
// already collapsed to scalar Repr) reads as `null` per langdef
// §"Dynamic Values" line 484-486.  At the encoder, this means
// writing `CEL_NULL` to the slot regardless of the slot's static
// scalar kind — the runtime kernel polymorphically handles
// `CEL_NULL == null` and the 3VL ladder.  Non-wrapper plain scalar
// variables that the embedder mis-binds with `Value::Null()` get
// the same permissive treatment; cel-cpp's checker is the strict
// gate, not the runtime marshaller.
bool TryEncodeNullToScalarSlot(const Value& v, CelValue* dst) {
  if (v.kind() != Value::Kind::kNull) return false;
  dst->kind = CEL_NULL;
  return true;
}

absl::Status EncodeBool(const Value& v, absl::string_view name, CelValue* dst) {
  if (TryEncodeNullToScalarSlot(v, dst)) return absl::OkStatus();
  if (TryEncodeWktWrapperMessage(v, CEL_BOOL, dst)) return absl::OkStatus();
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
  if (TryEncodeNullToScalarSlot(v, dst)) return absl::OkStatus();
  if (TryEncodeWktWrapperMessage(v, CEL_INT, dst)) return absl::OkStatus();
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
  if (TryEncodeNullToScalarSlot(v, dst)) return absl::OkStatus();
  if (TryEncodeWktWrapperMessage(v, CEL_UINT, dst)) return absl::OkStatus();
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
  if (TryEncodeNullToScalarSlot(v, dst)) return absl::OkStatus();
  if (TryEncodeWktWrapperMessage(v, CEL_DOUBLE, dst)) return absl::OkStatus();
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
bool TryEncodeWktTimeMessage(const Value& v, uint32_t want_kind, CelValue* dst);

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

// Encode a Value::Message-bound variable into a CEL_MESSAGE
// CelValue.  The bound `HostMessageBacking` is interned into the
// per-Instance `ExternrefTable` and the resulting slot lives in
// `payload.msg_slot`.  The caller is responsible for resetting
// the table between Evals so slot indices don't leak across
// invocations.
//
// Well-known-type bind normaliser.  When a variable is declared
// `google.protobuf.Timestamp` / `Duration` and the bound Value is
// a `Value::Message` carrying the matching WKT proto, peel
// (seconds, nanos) into a CelDurTs payload so the variable
// arrives at codegen as the matching `CEL_TIMESTAMP` /
// `CEL_DURATION` kind (see `rewrite/m7b-duration-timestamp.md` §3.4).
// Returns true and writes `dst` on hit; false (so the caller
// falls through to its normal kind-mismatch error path) on miss.
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
  const bool is_timestamp =
      want_kind == CEL_TIMESTAMP && fqn == "google.protobuf.Timestamp";
  const bool is_duration =
      want_kind == CEL_DURATION && fqn == "google.protobuf.Duration";
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

// Encode a Value::List / Value::HostList-bound variable into a
// CEL_LIST_HOST CelValue.  The bound `HostListBacking` is
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

// Encode a Value::Map / Value::HostMap-bound variable into a
// CEL_MAP_HOST CelValue.  Mirrors `EncodeList` for the map shape:
// the bound `HostMapBacking` is interned into the per-Instance
// `ExternrefTable` (independent `map_backings_` namespace from
// messages and lists) and the resulting slot lives in
// `payload.ref_slot`.  Routes through `InternMap` so the
// trampoline's `LookupMap` finds it; the CEL_MAP_HOST runtime
// dispatch (cel_map_lookup, cel_map_iter_init/key_at/value_at/
// next) already consumes this shape — proto map fields use the
// same wire encoding via `ProtoMap`.
absl::Status EncodeMap(const Value& v, absl::string_view name, CelValue* dst,
                       celwasm::ExternrefTable& refs) {
  if (v.kind() != Value::Kind::kMap) {
    return KindMismatch(name, "map", v.kind());
  }
  auto backing_or = v.SharedMapBacking();
  if (!backing_or.ok()) return backing_or.status();
  const uint32_t slot = refs.InternMap(*std::move(backing_or));
  dst->kind = CEL_MAP_HOST;
  dst->payload.ref_slot = slot;
  return absl::OkStatus();
}

// Encode a Value::Type-bound variable into a CEL_TYPE CelValue.
// The bound name string is copied into the host string arena above
// `arena_limit` (same arena kString / kBytes use); the resulting
// CelSpan lives in `payload.s`.  Same lifetime as kString — bytes
// outlive `arena_reset` because the arena floor is fixed at instantiation
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
    uint8_t* base = wasmtime_sharedmemory_data(arena.mem);
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

// Dispatch a declared Repr to the right per-kind encoder.  String
// / bytes payload bytes land in the activation buffer (malloc'd
// inside linear memory via wasm reentry).  Map / enum / unknown
// activation marshalling not yet implemented.
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
      // type-of-types binds through the host string arena
      // (same path as kString / kBytes — see `EncodeType`).
      return EncodeType(v, name, dst, ec.arena);
    case celwasm::Repr::kDuration:
      return EncodeDuration(v, name, dst);
    case celwasm::Repr::kTimestamp:
      return EncodeTimestamp(v, name, dst);
    case celwasm::Repr::kMap:
      return EncodeMap(v, name, dst, ec.refs);
    case celwasm::Repr::kEnum:
    case celwasm::Repr::kUnknown:
      return absl::UnimplementedError(
          absl::StrCat("Activation[", name, "]: Repr=", celwasm::ReprName(repr),
                       " marshal not implemented (later milestones for "
                       "enum/unknown)"));
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "Activation[", name, "]: unknown Repr=", static_cast<int>(repr)));
}

// Write CelValue `cv` into linear memory at `offset`.  Caller must
// have validated `offset + sizeof(CelValue) <= mem_size`.
void WriteCelValueAt(wasmtime_context_t* /*ctx*/, wasmtime_sharedmemory_t* mem,
                     uint32_t offset, const CelValue& cv) {
  uint8_t* base = wasmtime_sharedmemory_data(mem);
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
      // type-name strings live in the same host arena as
      // kString/kBytes payloads (see EncodeType above).
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
// Takes the whole `InstanceImpl` so the activation-buffer bookkeeping
// (`activation_buf_offset` / `activation_buf_capacity`) lives at
// instance scope without inflating MarshalActivation's parameter list
// past the lint gate.
absl::Status MarshalActivation(wasmtime_context_t* absl_nonnull ctx,
                               celwasm::InstanceImpl* absl_nonnull impl,
                               const Activation& activation) {
  wasmtime_sharedmemory_t* mem = impl->memory;
  const celwasm::abi::CelAbi& abi = impl->abi;

  // Pre-pass: ensure the activation buffer has room for every
  // kString / kBytes payload before any encoder runs.  A malloc'd
  // buffer's pointer doesn't move under us across reentry calls (no
  // memory.grow side-effect on the same Eval), so the encoders that
  // follow can read `wasmtime_memory_data` once and use it stably.
  const uint32_t need = TotalHostStringBytes(abi, activation);
  if (need > 0) {
    const absl::string_view first_name = abi.variables_size() > 0
                                             ? abi.variables(0).name()
                                             : absl::string_view{};
    if (auto s = EnsureActivationBuffer(
            ctx, impl->host_env.malloc_fn, first_name,
            &impl->activation_buf_offset, &impl->activation_buf_capacity, need);
        !s.ok()) {
      return s;
    }
  }
  uint32_t arena_cursor = 0;
  EncoderContext ec{
      impl->host_env.refs,
      HostStringArena{ctx, mem, impl->activation_buf_offset,
                      impl->activation_buf_capacity, &arena_cursor}};

  // Re-read mem_size AFTER any grow — workspace bounds checks below
  // need the fresh size, and the slot offsets are below `arena_floor`
  // so they always live in the original memory region.
  const size_t mem_size = wasmtime_sharedmemory_data_size(mem);
  (void)ctx;  // shared-memory APIs don't take a context.

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
  return wasmtime_sharedmemory_data_size(impl_->memory);
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
    const Activation& activation,
    absl::Span<const celwasm::AttributePattern> unknowns) {
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

}  // namespace celwasm::api
