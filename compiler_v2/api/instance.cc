#include "compiler_v2/api/instance.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/attribute.h"
#include "compiler_v2/api/internal/abi_decode.h"
#include "compiler_v2/api/internal/cel_host.h"
#include "compiler_v2/api/internal/instance_impl.h"
#include "compiler_v2/api/internal/wasmtime_engine_state.h"
#include "compiler_v2/api/value.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/runtime/cel_data.h"
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
                                       uint32_t offset);

// Decode an arena list (CEL_LIST_ARENA) by reading its
// `ArenaListHeader` and recursively decoding `count` × 24-byte
// CelValue elements out of the elements run.  Each element decodes
// through `DecodeCelValueAt`, so list values can themselves be
// scalars or nested aggregates.  Wraps the result in a
// `cel::Value::List(...)` (vector-backed `HostList`).
absl::StatusOr<Value> DecodeArenaListAt(wasmtime_context_t* ctx,
                                        const wasmtime_memory_t& mem,
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
    auto e_or = DecodeCelValueAt(ctx, mem, elem_off);
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
    auto k_or = DecodeCelValueAt(ctx, mem, entry_off);
    if (!k_or.ok()) return k_or.status();
    auto v_or = DecodeCelValueAt(ctx, mem, entry_off + sizeof(CelValue));
    if (!v_or.ok()) return v_or.status();
    entries.emplace_back(*std::move(k_or), *std::move(v_or));
  }
  return Value::Map(std::move(entries));
}

// Decode a 24-byte CelValue at `offset` in linear memory into a
// `cel::Value`.  Scalars + null + arena maps land in M1/M3; later
// milestones add lists / messages / unknown / error.
absl::StatusOr<Value> DecodeCelValueAt(wasmtime_context_t* ctx,
                                       const wasmtime_memory_t& mem,
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
      return DecodeArenaListAt(ctx, mem, cv.payload.arena_list.header_ptr);
    case CEL_MAP_ARENA:
      return DecodeArenaMapAt(ctx, mem, cv.payload.arena_map.header_ptr);
    case CEL_UNKNOWN:
      // M2.E: PartialEval surfaces CEL_UNKNOWN with the
      // attribute_id stamped in payload.unk.  Reconstruct an
      // AttributeId carrying that wire id; embedders compare it
      // against the slot they queried via PartialEval.
      return Value::Unknown(AttributeId{cv.payload.unk});
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
// uint / double / null) encode inline; strings and bytes need
// `cel_alloc` — but cel_alloc bumps the ARENA cursor (bytes
// [8, 12)), which `$eval`'s first real instruction (cel_reset)
// resets.  So any span payload we allocate here would get stomped
// on by cel_reset.  Avoid that by deferring string/bytes support
// until we have a cleaner way to host-allocate persistent
// payloads — M3 / M5-era work.  Scalars work end-to-end today.
//
// Repr::kMessage + aggregates land in M2.C / M6; fail loud until
// then.
// ─────────────────────────────────────────────────────────────

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

// M2.C: encode a Value::Message-bound variable into a CEL_MESSAGE
// CelValue.  The bound `HostMessageBacking` is interned into the
// per-Instance `ExternrefTable` and the resulting slot lives in
// `payload.msg_slot`.  The caller is responsible for resetting
// the table between Evals so slot indices don't leak across
// invocations.
absl::Status EncodeMessage(const Value& v, absl::string_view name,
                           CelValue* dst,
                           celwasm::ExternrefTable& refs) {
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
absl::Status EncodeList(const Value& v, absl::string_view name,
                        CelValue* dst,
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

// Dispatch a declared Repr to the right per-kind encoder.  M2.B
// ships scalars; M2.C adds kMessage.  String/bytes activation
// marshalling stays unimplemented (it needs a host-side arena
// allocator that survives the `cel_reset` $eval prelude — pending
// follow-up).
absl::Status EncodeScalarValue(const Value& v, celwasm::Repr repr,
                               absl::string_view name, CelValue* dst,
                               celwasm::ExternrefTable& refs) {
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
      return EncodeMessage(v, name, dst, refs);
    case celwasm::Repr::kList:
      return EncodeList(v, name, dst, refs);
    case celwasm::Repr::kString:
    case celwasm::Repr::kBytes:
    case celwasm::Repr::kMap:
    case celwasm::Repr::kDuration:
    case celwasm::Repr::kTimestamp:
    case celwasm::Repr::kEnum:
    case celwasm::Repr::kType:
    case celwasm::Repr::kUnknown:
      return absl::UnimplementedError(
          absl::StrCat("Activation[", name, "]: Repr=", celwasm::ReprName(repr),
                       " marshal not implemented (pending host-arena work "
                       "for kString/kBytes; later milestones for "
                       "list/map/duration/timestamp/enum/type)"));
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

// For every variable declared in the decoded ABI, look up its bound
// Value in the activation, encode, and write to its workspace slot.
// Missing variable → FailedPrecondition.  Type mismatch between
// declared Repr and bound Value::Kind → InvalidArgument.
absl::Status MarshalActivation(wasmtime_context_t* ctx,
                               const wasmtime_memory_t& mem,
                               const celwasm::abi::CelAbi& abi,
                               const Activation& activation,
                               celwasm::ExternrefTable& refs) {
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
    if (auto s = EncodeScalarValue(*bound, celwasm::DecodeRepr(dv.repr()),
                                   dv.name(), &cv, refs);
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
  return DecodeCelValueAt(ctx, impl_->memory,
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
  if (auto s = MarshalActivation(ctx, impl_->memory, impl_->abi, activation,
                                 impl_->host_env.refs);
      !s.ok()) {
    return s;
  }
  return Eval();
}

absl::StatusOr<Value> Instance::PartialEval(
    const Activation& activation,
    absl::Span<const AttributePattern> unknowns) {
  wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);

  // Reset per-eval state.  Unlike Eval(), we then populate
  // `unknown_patterns` so the cel_host trampoline shorts to
  // CEL_UNKNOWN on FULL pattern matches.
  impl_->host_env.refs.Reset();
  impl_->host_env.bindings.unknown_patterns = unknowns;

  if (auto s = MarshalActivation(ctx, impl_->memory, impl_->abi, activation,
                                 impl_->host_env.refs);
      !s.ok()) {
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
