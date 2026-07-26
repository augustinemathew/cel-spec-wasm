#include "eval/instance.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
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
#include "compiler/ir/annotations.h"
#include "eval/activation.h"
#include "eval/attribute.h"
#include "eval/error.h"
#include "eval/internal/abi_decode.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/instance_impl.h"
#include "eval/internal/wasmtime_engine_state.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {

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
// `celwasm::Value::List(...)` (vector-backed `HostList`).
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
// in a `celwasm::Value::Map(...)` (vector-backed `HostMap`).
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
// `celwasm::Value`, then re-wrap in a fresh vector-backed `HostList`
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
  // Auto-unpack a top-level `google.protobuf.Any` result.  cel-cpp's
  // runtime returns the unpacked target message for an Any-typed
  // result (the Any contract is "unpack on read" — langdef
  // §"Message Field Selection" admits Any anywhere a message-typed
  // value is wanted).  Conformance rows `dynamic/any/literal`,
  // `any/var`, `any/literal_empty` pin this — `literal_empty`
  // expects an error because an empty Any has no type_url to
  // resolve.
  const google::protobuf::Descriptor* desc = src->GetDescriptor();
  if (desc != nullptr && desc->full_name() == "google.protobuf.Any") {
    const google::protobuf::DescriptorPool* pool =
        desc->file() != nullptr ? desc->file()->pool() : nullptr;
    celwasm::Value unpacked = celwasm::UnpackAnyToValue(*src, pool);
    return unpacked;
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

// Decode a CEL_UNKNOWN payload: `payload.unk` is 0 (the legal empty
// UnknownSet) or a byte offset to a 2-word `{ids_off, len}` descriptor
// whose id array carries every attribute identity the unknown merged
// (doc/design/03-abi-and-memory.md §8.2).  The descriptor lives in the
// guest bump arena (in-eval writers) or the activation buffer (the
// marshal); both survive until this post-Eval decode — the arena is
// rewound only by the NEXT $eval's prelude.
absl::StatusOr<Value> DecodeUnknownSetAt(wasmtime_context_t* ctx,
                                         wasmtime_sharedmemory_t* mem,
                                         uint32_t desc_off) {
  if (desc_off == 0) {
    return Value::Unknown(std::vector<celwasm::AttributeId>{});
  }
  uint32_t desc[2] = {0, 0};
  if (auto s = ReadMemBytes(ctx, mem, desc_off, sizeof(desc), desc); !s.ok()) {
    return s;
  }
  const uint32_t ids_off = desc[0];
  const uint32_t len = desc[1];
  std::vector<uint32_t> ids(len);
  if (len > 0) {
    const uint64_t ids_bytes = uint64_t{len} * sizeof(uint32_t);
    if (ids_bytes > std::numeric_limits<uint32_t>::max()) {
      return absl::InvalidArgumentError(
          absl::StrCat("CEL_UNKNOWN descriptor at offset ", desc_off,
                       " claims an impossible id count ", len));
    }
    if (auto s = ReadMemBytes(ctx, mem, ids_off,
                              static_cast<uint32_t>(ids_bytes), ids.data());
        !s.ok()) {
      return s;
    }
  }
  std::vector<celwasm::AttributeId> attrs;
  attrs.reserve(len);
  for (uint32_t id : ids) {
    attrs.push_back(celwasm::AttributeId{id});
  }
  return Value::Unknown(std::move(attrs));
}

// Decode a 24-byte CelValue at `offset` in linear memory into a
// `celwasm::Value`.  Covers scalars, null, arena maps/lists, and
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
      // PartialEval surfaces CEL_UNKNOWN with `payload.unk` carrying
      // an UnknownSet-descriptor offset (doc/design/03-abi-and-memory.md
      // §8.2).  Dereference it and surface EVERY merged attribute id;
      // embedders compare them against the attributes they marked
      // unknown via PartialEval.
      return DecodeUnknownSetAt(ctx, mem, cv.payload.unk);
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
  constexpr uint64_t k4K = uint64_t{4} * 1024;
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
  //
  // Unchecked invocation: wasi-libc's `malloc` export has the fixed
  // signature `[i32 size] -> [i32 offset]`, proven once at Plan by
  // `CheckAllI32FuncSignature` in engine.cc::BindRuntimeFuncHandles.
  // raw[0] carries the size in and the offset out (results overwrite
  // arguments per the unchecked-call contract); traps still surface
  // through the same `wasm_trap_t` out-param.
  const uint32_t new_capacity = RoundUpTo4K(needed);
  wasmtime_val_raw_t raw[1];
  raw[0].i32 = static_cast<int32_t>(new_capacity);
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_func_call_unchecked(
      ctx, &malloc_fn, raw, /*args_and_results_len=*/1, &trap);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(absl::StrCat("Activation[", first_var_name,
                                              "]: malloc(", new_capacity, ")"),
                                 err);
  }
  if (trap != nullptr) {
    return WasmTrapToStatus(
        absl::StrCat("Activation[", first_var_name, "]: malloc trap"), trap);
  }
  if (raw[0].i32 == 0) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "Activation[", first_var_name, "]: malloc returned NULL (needed ",
        new_capacity, " bytes)"));
  }
  *buf_offset = static_cast<uint32_t>(raw[0].i32);
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

// Forward decls — defined further down.
absl::Status KindMismatch(absl::string_view name, absl::string_view declared,
                          Value::Kind got);
uint32_t WrapperFqnToCelKind(absl::string_view fqn);

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

// Extract the `value` field of a StringValue / BytesValue WKT wrapper
// message into a `absl::string_view` view onto the message's internal
// buffer.  Returns the view via `*out_sv` and true on hit; false on
// "not a wrapper" or descriptor mismatch (caller falls through).  The
// resulting view is live as long as the underlying message is — the
// caller copies into the arena before doing anything that releases
// the message reference.
bool TryReadWktStringWrapperValue(const Value& v, celwasm::Repr repr,
                                  absl::string_view* out_sv,
                                  std::string* scratch) {
  using FD = google::protobuf::FieldDescriptor;
  if (v.kind() != Value::Kind::kMessage) return false;
  auto backing_or = v.SharedMessageBacking();
  if (!backing_or.ok()) return false;
  const google::protobuf::Message* msg = (*backing_or)->message();
  if (msg == nullptr) return false;
  const google::protobuf::Descriptor* d = msg->GetDescriptor();
  if (d == nullptr) return false;
  const uint32_t fqn_kind = WrapperFqnToCelKind(d->full_name());
  const uint32_t want_kind =
      repr == celwasm::Repr::kString ? CEL_STRING : CEL_BYTES;
  if (fqn_kind == 0 || fqn_kind != want_kind) return false;
  const google::protobuf::Reflection* refl = msg->GetReflection();
  const google::protobuf::FieldDescriptor* vf = d->FindFieldByNumber(1);
  if (refl == nullptr || vf == nullptr) return false;
  if (vf->cpp_type() != FD::CPPTYPE_STRING) return false;
  *out_sv = refl->GetStringReference(*msg, vf, scratch);
  return true;
}

// Encode a kString / kBytes value: copy the payload bytes into the
// host string arena and write the offset+len into the CelValue.
// `arena.cursor` advances by `aligned_len` so the next encoder
// starts at a clean 8-byte boundary.
//
// Per langdef §"Dynamic Values" and cleanup-backlog #18: a bound
// `Value::Message(StringValue{...})` against a string-declared
// variable should peel the wrapper into the inner string (symmetric
// with the numeric `TryEncodeWktWrapperMessage` path).  Mirrors
// cel-cpp's WKT wrapper-binding semantics.
absl::Status EncodeStringOrBytes(const Value& v, absl::string_view name,
                                 celwasm::Repr repr, CelValue* dst,
                                 HostStringArena arena) {
  const Value::Kind expected = repr == celwasm::Repr::kString
                                   ? Value::Kind::kString
                                   : Value::Kind::kBytes;
  absl::string_view sv;
  std::string scratch;
  if (TryReadWktStringWrapperValue(v, repr, &sv, &scratch)) {
    // Fall through with `sv` populated; skip the kind check + AsString.
  } else {
    if (v.kind() != expected) {
      return KindMismatch(
          name, repr == celwasm::Repr::kString ? "string" : "bytes", v.kind());
    }
    auto sv_or = repr == celwasm::Repr::kString ? v.AsString() : v.AsBytes();
    if (!sv_or.ok()) return sv_or.status();
    sv = *sv_or;
  }
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
// `WrapperKindFromFqn` in `compiler/codegen/expr_lower.cc`.
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
// `compiler/ir/typed_ast.cc:56` mapping collapses it to the
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
// Defined below; the sizing pre-pass and the marshal must agree on
// which variables an unknown pattern blanks.
std::optional<uint32_t> BareVariableUnknownId(
    const celwasm::CelHostBindings& bindings, absl::string_view name);

absl::StatusOr<uint32_t> TotalHostStringBytes(
    const celwasm::abi::CelAbi& abi, const Activation& activation,
    const celwasm::CelHostBindings& bindings) {
  uint32_t total = 0;
  for (const celwasm::abi::VariableEntry& dv : abi.variables()) {
    const celwasm::Repr repr = celwasm::DecodeRepr(dv.repr());
    if (repr != celwasm::Repr::kString && repr != celwasm::Repr::kBytes &&
        repr != celwasm::Repr::kType) {
      continue;
    }
    // Skip variables a FULL unknown pattern blanks: their slot holds a
    // CEL_UNKNOWN descriptor rather than a payload, so they consume no
    // string arena — and resolving them here would invoke a lazy
    // binder for a variable the caller declared opaque.
    if (BareVariableUnknownId(bindings, dv.name()).has_value()) continue;
    // Resolve, not a bare lookup: a lazy binder runs here and its
    // value is memoized, so the marshal below sees the same bytes this
    // pre-pass budgeted for.
    auto resolved = activation.Resolve(dv.name());
    if (!resolved.ok()) return resolved.status();
    const Value* bound = *resolved;
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
    } else if ((repr == celwasm::Repr::kString ||
                repr == celwasm::Repr::kBytes) &&
               bound->kind() == Value::Kind::kMessage) {
      // WKT wrapper peel (StringValue / BytesValue → inner string/bytes):
      // EncodeStringOrBytes copies the wrapper's `value` field into the
      // arena.  Probe the same path here so the pre-pass budgets the
      // matching bytes.  Per cleanup-backlog #18.
      absl::string_view sv;
      std::string scratch;
      if (TryReadWktStringWrapperValue(*bound, repr, &sv, &scratch)) {
        total += static_cast<uint32_t>(sv.size());
      }
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
// Partial-eval: a variable named by a FULL unknown pattern is opaque
// — its workspace slot holds CEL_UNKNOWN instead of a marshaled bound
// value, and CEL's 3VL absorption then propagates the unknown through
// every operation that reads it (`x + 1`, `x == y`, `x[0]`, `x.f`,
// …) per langdef "Partial state".  This is the bare-variable
// counterpart to the `.field`-select unknown the cel_host trampoline
// produces; both consult the same `unknown_patterns` set.
//
// Returns the interned attribute_id to stamp into the CEL_UNKNOWN
// payload when the bare variable (root name + empty qualifier path)
// is kFull-matched by some pattern; std::nullopt when no pattern
// fully matches (the caller encodes the bound value as usual).  A
// pattern that matches only a SUB-attribute (`x.foo` vs bare `x`) is
// kPartial, not kFull, so it does NOT blank the whole slot — the
// select trampoline handles that narrower case.
//
// The unknown verdict is INDEPENDENT of whether the variable is bound:
// an unknown variable need not appear in the Activation, and a binding
// that IS present is deliberately ignored (the pattern wins) — marking
// a variable unknown means "pretend its value is not yet known," even
// if a concrete value happens to be available.
std::optional<uint32_t> BareVariableUnknownId(
    const celwasm::CelHostBindings& bindings, absl::string_view name) {
  if (bindings.unknown_patterns.empty()) return std::nullopt;
  const celwasm::Attribute bare{std::string(name)};
  const bool full = std::any_of(
      bindings.unknown_patterns.begin(), bindings.unknown_patterns.end(),
      [&bare](const celwasm::AttributePattern& p) {
        return p.IsMatch(bare) == celwasm::AttributePattern::MatchType::kFull;
      });
  if (!full) return std::nullopt;
  // The interned id is the index into the attribute table; every
  // referenced free ident is interned (resolve_pass PostVisitIdent),
  // so a referenced variable always has a bare-path row.  Fall back to
  // the sentinel 0 if somehow absent — still surfaces UNKNOWN.
  for (size_t i = 0; i < bindings.attributes.size(); ++i) {
    const celwasm::AttributeEntry& a = bindings.attributes[i];
    if (a.qualifiers.empty() && a.root_variable == name) {
      return static_cast<uint32_t>(i);
    }
  }
  return 0u;
}

// Bytes the activation buffer reserves per fully-unknown variable:
// the 2-word `{ids_off, len}` UnknownSet descriptor plus one u32
// attribute id, padded to the 8-byte cursor discipline the string
// encoder uses.
constexpr uint32_t kUnknownDescriptorBytes = 16;

// Count the declared variables a FULL unknown pattern blanks — the
// activation-buffer pre-pass budgets `kUnknownDescriptorBytes` for
// each (mirrors TotalHostStringBytes for string payloads).
uint32_t CountUnknownVariables(const celwasm::abi::CelAbi& abi,
                               const celwasm::CelHostBindings& bindings) {
  if (bindings.unknown_patterns.empty()) return 0;
  uint32_t n = 0;
  for (const celwasm::abi::VariableEntry& dv : abi.variables()) {
    if (BareVariableUnknownId(bindings, dv.name()).has_value()) ++n;
  }
  return n;
}

// Write a fully-unknown variable's CelValue: CEL_UNKNOWN whose
// `payload.unk` is a 1-element UnknownSet descriptor carrying
// `attr_id` (doc/design/03-abi-and-memory.md §8.2 — never the raw
// id; the kernel merge dereferences `payload.unk` as a descriptor
// offset).
//
// Lifetime: the descriptor is minted in the ACTIVATION BUFFER, not
// the guest bump arena.  The marshal runs BEFORE $eval, whose first
// instruction is `arena_reset` — an arena-minted descriptor would be
// rewound and zero-filled by the first in-eval `arena_alloc`.  The
// activation buffer is malloc'd from the dlmalloc heap (wasm
// reentry) and stable for the whole Eval, so the descriptor survives
// kernel merges and the post-Eval result decode (the same lifetime
// argument as the string payload bytes sharing this buffer).
absl::Status EncodeUnknownVariable(absl::string_view name, uint32_t attr_id,
                                   CelValue* dst, HostStringArena arena) {
  if (static_cast<uint64_t>(*arena.cursor) + kUnknownDescriptorBytes >
      arena.capacity) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "Activation[", name, "]: host string arena OOM (need ",
        kUnknownDescriptorBytes, " bytes for the UnknownSet descriptor; ",
        "cursor=", *arena.cursor, ", capacity=", arena.capacity, ")"));
  }
  const uint32_t offset = arena.floor + *arena.cursor;
  uint8_t* base = wasmtime_sharedmemory_data(arena.mem);
  // Layout: descriptor {ids_off, len} at [offset, offset+8), the
  // single id at [offset+8, offset+12).
  const uint32_t words[3] = {
      offset + static_cast<uint32_t>(2 * sizeof(uint32_t)), 1, attr_id};
  std::memcpy(base + offset, words, sizeof(words));
  *arena.cursor += kUnknownDescriptorBytes;
  dst->kind = CEL_UNKNOWN;
  dst->payload.unk = offset;
  return absl::OkStatus();
}

// Marshal one declared variable into its workspace slot: CEL_UNKNOWN
// when a pattern fully matches the bare variable (PartialEval; binding
// ignored, may be absent), else the encoded bound value.  Bounds-checks
// the slot against `mem_size` first.
absl::Status MarshalOneVariable(wasmtime_context_t* absl_nonnull ctx,
                                celwasm::InstanceImpl* absl_nonnull impl,
                                const Activation& activation,
                                const celwasm::abi::VariableEntry& dv,
                                size_t mem_size, EncoderContext& ec) {
  wasmtime_sharedmemory_t* mem = impl->memory;
  if (static_cast<std::uint64_t>(dv.slot_offset()) + sizeof(CelValue) >
      mem_size) {
    return absl::OutOfRangeError(
        absl::StrCat("Activation[", dv.name(), "]: slot offset ",
                     dv.slot_offset(), " + 24 exceeds memory size ", mem_size));
  }
  // PartialEval: a fully-unknown variable's slot holds CEL_UNKNOWN
  // regardless of whether it is bound — the pattern wins, and the
  // variable need not appear in the Activation.
  if (std::optional<uint32_t> unk_id =
          BareVariableUnknownId(impl->host_env.bindings, dv.name());
      unk_id.has_value()) {
    CelValue unk{};
    if (auto s = EncodeUnknownVariable(dv.name(), *unk_id, &unk, ec.arena);
        !s.ok()) {
      return s;
    }
    WriteCelValueAt(ctx, mem, dv.slot_offset(), unk);
    return absl::OkStatus();
  }
  // Resolved after the unknown check above, so a lazy binder never
  // runs for a variable the caller declared opaque.
  auto resolved = activation.Resolve(dv.name());
  if (!resolved.ok()) return resolved.status();
  const Value* bound = *resolved;
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
  return absl::OkStatus();
}

absl::Status MarshalActivation(wasmtime_context_t* absl_nonnull ctx,
                               celwasm::InstanceImpl* absl_nonnull impl,
                               const Activation& activation) {
  wasmtime_sharedmemory_t* mem = impl->memory;
  const celwasm::abi::CelAbi& abi = impl->abi;

  // Each evaluation re-invokes its lazy binders: drop values memoized
  // by the previous one before anything reads the activation.
  activation.ClearLazyCache();

  // Pre-pass: ensure the activation buffer has room for every
  // kString / kBytes payload AND every fully-unknown variable's
  // UnknownSet descriptor before any encoder runs.  A malloc'd
  // buffer's pointer doesn't move under us across reentry calls (no
  // memory.grow side-effect on the same Eval), so the encoders that
  // follow can read `wasmtime_memory_data` once and use it stably.
  auto string_bytes =
      TotalHostStringBytes(abi, activation, impl->host_env.bindings);
  if (!string_bytes.ok()) return string_bytes.status();
  const uint32_t need =
      *string_bytes + (kUnknownDescriptorBytes *
                       CountUnknownVariables(abi, impl->host_env.bindings));
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
    if (auto s = MarshalOneVariable(ctx, impl, activation, dv, mem_size, ec);
        !s.ok()) {
      return s;
    }
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
  // Re-seed the per-Eval linear-memory size snapshot the host
  // trampolines bounds-check against (see
  // `CelHostCallbackEnv::mem_base` / `mem_size`): the activation
  // marshal that ran just before this call may have grown memory
  // (activation-buffer malloc), and growth from prior Evals must be
  // visible.  The cached base pointer needs no re-seed — wasmtime
  // shared memories keep a stable base across memory.grow (pinned by
  // memory_grow_stability_test.cc).  Mid-$eval growth past this
  // snapshot is handled by WasmtimeMemoryView's refresh-on-bounds-
  // miss path.
  impl_->host_env.mem_size =
      static_cast<uint32_t>(wasmtime_sharedmemory_data_size(impl_->memory));
  wasmtime_func_t fn = impl_->eval_fn;
  // Unchecked invocation: `$eval`'s signature is the fixed export
  // contract `[] -> [i32 result_offset]`, proven once at Plan by
  // `CheckAllI32FuncSignature` in engine.cc::InstantiateExpr — so the
  // per-call type/arity checking (and the RegisteredType refcount
  // churn it drags in) that `wasmtime_func_call` performs proves
  // nothing new here.  The raw buffer needs capacity
  // max(nparams, nresults) = 1; the i32 result lands at raw[0].
  // Traps still surface through the same `wasm_trap_t` out-param,
  // so the trap → Status conversion below is unchanged.
  wasmtime_val_raw_t raw[1] = {};
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_func_call_unchecked(
      ctx, &fn, raw, /*args_and_results_len=*/1, &trap);
  if (err != nullptr) return WasmtimeErrorToStatus("Eval (func_call)", err);
  if (trap != nullptr) return WasmTrapToStatus("Eval trapped", trap);
  return DecodeCelValueAt(ctx, impl_->memory, impl_->host_env.refs,
                          static_cast<uint32_t>(raw[0].i32));
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

}  // namespace celwasm
