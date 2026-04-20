#include "compiler/bench/bench_fixture.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/module.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/host/host_loader.h"
#include "compiler/runtime/cel_runtime.h"
#include "google/protobuf/message.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm::bench {

absl::StatusOr<LoadedEval> Precompile(absl::string_view cel_source,
                                      std::vector<std::string> variable_specs) {
  CheckOptions opts;
  opts.variable_specs = std::move(variable_specs);
  auto typed = ParseAndCheck(cel_source, opts);
  if (!typed.ok()) return typed.status();

  WasmModule mod;
  auto fn = LowerToEvalFunction(*typed, "eval", mod);
  if (!fn.ok()) return fn.status();
  mod.ExportFunction("eval", "eval");
  if (auto s = mod.Validate(); !s.ok()) return s;

  auto bytes = mod.Serialize();
  if (!bytes.ok()) return bytes.status();
  return LoadEval(*bytes);
}

absl::StatusOr<DecodedString> DecodeStringAt(LoadedEval& loaded,
                                             int32_t offset) {
  wasmtime_context_t* ctx = loaded.context();
  wasmtime_extern_t mem_ext;
  if (!wasmtime_instance_export_get(ctx, &loaded.runtime_instance(), "memory",
                                    6, &mem_ext)) {
    return absl::InternalError("runtime does not export `memory`");
  }
  if (mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
    return absl::InternalError("`memory` export is not a memory");
  }
  const uint8_t* base = wasmtime_memory_data(ctx, &mem_ext.of.memory);
  size_t size = wasmtime_memory_data_size(ctx, &mem_ext.of.memory);

  wasmtime_extern_t mem_base_ext;
  if (!wasmtime_instance_export_get(ctx, &loaded.runtime_instance(),
                                    "cel_mem_base", std::strlen("cel_mem_base"),
                                    &mem_base_ext)) {
    return absl::InternalError("runtime does not export `cel_mem_base`");
  }
  wasmtime_val_t base_off{};
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_func_call(
      ctx, &mem_base_ext.of.func, /*args=*/nullptr, /*nargs=*/0, &base_off,
      /*nresults=*/1, &trap);
  if (err != nullptr || trap != nullptr) {
    return absl::InternalError("cel_mem_base call failed");
  }
  const auto mem_base = static_cast<uint32_t>(base_off.of.i32);
  const uint64_t abs_cv =
      static_cast<uint64_t>(mem_base) + static_cast<uint64_t>(offset);
  if (offset <= 0 || abs_cv + sizeof(CelValue) > size) {
    return absl::OutOfRangeError("CelValue offset is outside memory bounds");
  }
  CelValue v;
  std::memcpy(&v, base + abs_cv, sizeof(v));
  DecodedString out;
  out.kind = v.kind;
  const uint32_t ptr = v.payload.s.ptr;
  const uint32_t len = v.payload.s.len;
  if (len > 0) {
    const uint64_t abs_bytes =
        static_cast<uint64_t>(mem_base) + static_cast<uint64_t>(ptr);
    if (abs_bytes + len > size) {
      return absl::OutOfRangeError(
          "string payload span falls outside runtime memory");
    }
    out.payload.assign(reinterpret_cast<const char*>(base + abs_bytes), len);
  }
  return out;
}

wasmtime_val_t MessageAsExternref(LoadedEval& loaded,
                                  google::protobuf::Message& msg) {
  wasmtime_val_t v{};
  v.kind = WASMTIME_EXTERNREF;
  wasmtime_externref_new(loaded.context(), &msg, /*finalizer=*/nullptr,
                         &v.of.externref);
  return v;
}

}  // namespace celwasm::bench
