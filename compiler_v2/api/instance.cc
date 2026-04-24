#include "compiler_v2/api/instance.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/internal/abi_decode.h"
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

// Decode a 24-byte CelValue at `offset` in linear memory into a
// `cel::Value`.  Spec milestones add more arms; M1 covers all
// scalar kinds + null.
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
    default:
      return absl::InvalidArgumentError(absl::StrCat(
          "Eval returned a CelValue kind ", static_cast<int>(cv.kind),
          " not yet supported by Instance::Eval (M1 covers "
          "scalars only)"));
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

// Dispatch a declared Repr to the right per-kind encoder.  M2.B
// ships scalars; later-milestone reprs land in the Unimplemented
// tail with the milestone tag named in the error message.
absl::Status EncodeScalarValue(const Value& v, celwasm::Repr repr,
                               absl::string_view name, CelValue* dst) {
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
    case celwasm::Repr::kString:
    case celwasm::Repr::kBytes:
    case celwasm::Repr::kMessage:
    case celwasm::Repr::kList:
    case celwasm::Repr::kMap:
    case celwasm::Repr::kDuration:
    case celwasm::Repr::kTimestamp:
    case celwasm::Repr::kEnum:
    case celwasm::Repr::kType:
    case celwasm::Repr::kUnknown:
      return absl::UnimplementedError(
          absl::StrCat("Activation[", name, "]: Repr=", celwasm::ReprName(repr),
                       " marshal not implemented at M2.B (pending "
                       "string/bytes arena work + M2.C message ref)"));
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
                               const celwasm::DecodedCelAbi& abi,
                               const Activation& activation) {
  const size_t mem_size = [&]() {
    wasmtime_memory_t m = mem;
    return wasmtime_memory_data_size(ctx, &m);
  }();

  for (const celwasm::DecodedVariable& dv : abi.variables) {
    if (static_cast<std::uint64_t>(dv.slot_offset) + sizeof(CelValue) >
        mem_size) {
      return absl::OutOfRangeError(
          absl::StrCat("Activation[", dv.name, "]: slot offset ",
                       dv.slot_offset, " + 24 exceeds memory size ", mem_size));
    }
    const Value* bound = activation.Find(dv.name);
    if (bound == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("Activation: variable `", dv.name,
                       "` declared on Compiler but not bound on Activation"));
    }
    CelValue cv{};
    if (auto s = EncodeScalarValue(*bound, dv.repr, dv.name, &cv); !s.ok()) {
      return s;
    }
    WriteCelValueAt(ctx, mem, dv.slot_offset, cv);
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
  // For every declared variable, look up in the activation and
  // write its CelValue into the pre-assigned workspace slot.  This
  // must run BEFORE the $eval call — $eval's prelude writes each
  // slot offset into a wasm local, then the body reads
  // `local.get local_index` to get the offset of the CelValue we
  // just placed.
  if (auto s = MarshalActivation(ctx, impl_->memory, impl_->abi, activation);
      !s.ok()) {
    return s;
  }
  return Eval();
}

absl::StatusOr<Value> Instance::PartialEval(
    const Activation& /*activation*/,
    absl::Span<const AttributePattern> /*unknowns*/) {
  // M2.E lights this up — the Activation marshal is the same as
  // the full-Eval path, but the cel_host trampoline consults the
  // unknown_patterns set and writes Value::Unknown(attribute_id)
  // into the select's out_slot when a match fires.  Until that
  // lands, surface Unimplemented so the symbol exists (e2e test
  // suite links) and any caller gets a clear error.
  return absl::UnimplementedError(
      "Instance::PartialEval: not implemented until M2.E "
      "(ResolvePass attribute_pool + cel_host unknown-pattern "
      "consultation)");
}

}  // namespace cel
