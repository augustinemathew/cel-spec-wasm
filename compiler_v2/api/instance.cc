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
#include "compiler_v2/api/internal/instance_impl.h"
#include "compiler_v2/api/internal/wasmtime_engine_state.h"
#include "compiler_v2/api/value.h"
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

}  // namespace cel
