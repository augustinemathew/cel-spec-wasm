#include "compiler_v2/host/host_loader.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/host/cel_log.h"
#include "compiler_v2/runtime/cel_runtime.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {

namespace {

// Pulled out so every wasmtime error turns into an absl::Status with a
// uniform message shape — easier to match on in tests and clearer when
// the failure surfaces in a log.
absl::Status WasmtimeErrorToStatus(absl::string_view context,
                                   wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  std::string text(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return absl::FailedPreconditionError(absl::StrCat(context, ": ", text));
}

absl::Status WasmTrapToStatus(absl::string_view context, wasm_trap_t* trap) {
  wasm_byte_vec_t msg;
  wasm_trap_message(trap, &msg);
  std::string text(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasm_trap_delete(trap);
  return absl::InternalError(absl::StrCat(context, ": ", text));
}

// Grab the caller's exported `memory` as a raw host pointer.  Used by
// the `cel.cel_reset` / `cel.cel_alloc` trampolines to hand the
// native runtime a pointer to the expr wasm instance's linear memory.
// Returns nullptr on a caller that doesn't export memory (callee
// should trap in that case — never expected at runtime).
uint8_t* CallerMemoryBase(wasmtime_caller_t* caller) {
  wasmtime_extern_t ext;
  const char kName[] = "memory";
  if (!wasmtime_caller_export_get(caller, kName, sizeof(kName) - 1, &ext) ||
      ext.kind != WASMTIME_EXTERN_MEMORY) {
    return nullptr;
  }
  uint8_t* data =
      wasmtime_memory_data(wasmtime_caller_context(caller), &ext.of.memory);
  wasmtime_extern_delete(&ext);
  return data;
}

// Host trampoline: `cel.cel_reset(base: i32, limit: i32) -> ()`.
// Thin wrapper — the actual semantics live in
// `runtime/cel_runtime.c::cel_reset_native`.  No duplication.
wasm_trap_t* CelResetTrampoline(void* /*data*/, wasmtime_caller_t* caller,
                                const wasmtime_val_t* args, size_t nargs,
                                wasmtime_val_t* /*results*/,
                                size_t /*nresults*/) {
  ABSL_CHECK(nargs == 2) << "cel_reset expects (i32, i32)";
  uint8_t* mem = CallerMemoryBase(caller);
  if (mem == nullptr) {
    const char kMsg[] = "cel_reset: caller has no exported `memory`";
    return wasmtime_trap_new(kMsg, sizeof(kMsg) - 1);
  }
  cel_reset_native(mem, static_cast<uint32_t>(args[0].of.i32),
                   static_cast<uint32_t>(args[1].of.i32));
  return nullptr;
}

// Host trampoline: `cel.cel_alloc(nbytes: i32) -> i32`.  Same thin-
// wrapper contract as `cel_reset`.  M1 scalar-literal evals never hit
// this path (literals live in `.rodata`; no runtime allocation), but
// the binding is correct and ready for M3+.
wasm_trap_t* CelAllocTrampoline(void* /*data*/, wasmtime_caller_t* caller,
                                const wasmtime_val_t* args, size_t nargs,
                                wasmtime_val_t* results, size_t nresults) {
  ABSL_CHECK(nargs == 1) << "cel_alloc expects (i32)";
  ABSL_CHECK(nresults == 1) << "cel_alloc returns i32";
  uint8_t* mem = CallerMemoryBase(caller);
  if (mem == nullptr) {
    const char kMsg[] = "cel_alloc: caller has no exported `memory`";
    return wasmtime_trap_new(kMsg, sizeof(kMsg) - 1);
  }
  const uint32_t out =
      cel_alloc_native(mem, static_cast<uint32_t>(args[0].of.i32));
  results[0].kind = WASMTIME_I32;
  results[0].of.i32 = static_cast<int32_t>(out);
  return nullptr;
}

// ——— wasmtime functype factories + linker define helper ———

wasm_functype_t* I32I32ToVoidType() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* ps[2] = {wasm_valtype_new(WASM_I32),
                           wasm_valtype_new(WASM_I32)};
  wasm_valtype_vec_new(&params, 2, ps);
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

wasm_functype_t* I32ToI32Type() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* ps[1] = {wasm_valtype_new(WASM_I32)};
  wasm_valtype_t* rs[1] = {wasm_valtype_new(WASM_I32)};
  wasm_valtype_vec_new(&params, 1, ps);
  wasm_valtype_vec_new(&results, 1, rs);
  return wasm_functype_new(&params, &results);
}

absl::Status DefineLinkerFunc(wasmtime_linker_t* linker,
                              absl::string_view module, absl::string_view name,
                              wasm_functype_t* type,
                              wasmtime_func_callback_t callback) {
  wasmtime_error_t* err = wasmtime_linker_define_func(
      linker, module.data(), module.size(), name.data(), name.size(), type,
      callback, /*data=*/nullptr, /*finalizer=*/nullptr);
  wasm_functype_delete(type);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(
        absl::StrCat("linker.define(", module, ".", name, ")"), err);
  }
  return absl::OkStatus();
}

absl::Status InstallHostImports(wasmtime_linker_t* linker) {
  auto s = DefineLinkerFunc(linker, "cel", "cel_reset", I32I32ToVoidType(),
                            CelResetTrampoline);
  if (!s.ok()) return s;
  s = DefineLinkerFunc(linker, "cel", "cel_alloc", I32ToI32Type(),
                       CelAllocTrampoline);
  if (!s.ok()) return s;
  return RegisterCelLog(linker);
}

}  // namespace

// ————————— Impl —————————

struct EvalInstance::Impl {
  wasm_engine_t* engine = nullptr;
  wasmtime_store_t* store = nullptr;
  wasmtime_linker_t* linker = nullptr;
  wasmtime_module_t* expr_module = nullptr;
  wasmtime_instance_t expr_instance{};
  wasmtime_memory_t memory{};
  wasmtime_func_t eval_fn{};
  EvalInstanceOptions opts;

  ~Impl() {
    if (expr_module != nullptr) wasmtime_module_delete(expr_module);
    if (linker != nullptr) wasmtime_linker_delete(linker);
    if (store != nullptr) wasmtime_store_delete(store);
    if (engine != nullptr) wasm_engine_delete(engine);
  }
};

EvalInstance::EvalInstance(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
EvalInstance::~EvalInstance() = default;
EvalInstance::EvalInstance(EvalInstance&&) noexcept = default;
EvalInstance& EvalInstance::operator=(EvalInstance&&) noexcept = default;

namespace {

absl::Status InitHandles(EvalInstance::Impl& impl) {
  impl.engine = wasm_engine_new();
  if (impl.engine == nullptr) {
    return absl::InternalError("wasm_engine_new returned null");
  }
  impl.store = wasmtime_store_new(impl.engine, /*data=*/nullptr,
                                  /*finalizer=*/nullptr);
  if (impl.store == nullptr) {
    return absl::InternalError("wasmtime_store_new returned null");
  }
  impl.linker = wasmtime_linker_new(impl.engine);
  if (impl.linker == nullptr) {
    return absl::InternalError("wasmtime_linker_new returned null");
  }
  return InstallHostImports(impl.linker);
}

// Compiles + instantiates the expr module.  Expr imports
// cel.cel_reset + cel.cel_alloc (host trampolines) + cel_env.cel_log;
// it defines and exports its own memory (M1 design §8.2).  No
// runtime wasm module is involved — the native `cel_runtime`
// cc_library provides cel_reset_native / cel_alloc_native directly.
absl::Status CompileAndInstantiateExpr(EvalInstance::Impl& impl,
                                       absl::Span<const uint8_t> wasm_bytes) {
  wasmtime_error_t* err = wasmtime_module_new(
      impl.engine, wasm_bytes.data(), wasm_bytes.size(), &impl.expr_module);
  if (err != nullptr) {
    return WasmtimeErrorToStatus("module_new(expr)", err);
  }
  wasmtime_context_t* ctx = wasmtime_store_context(impl.store);
  wasmtime_instance_t inst;
  wasm_trap_t* trap = nullptr;
  err = wasmtime_linker_instantiate(impl.linker, ctx, impl.expr_module, &inst,
                                    &trap);
  if (err != nullptr) {
    return WasmtimeErrorToStatus("instantiate(expr)", err);
  }
  if (trap != nullptr) {
    return WasmTrapToStatus("instantiate(expr) trapped", trap);
  }
  impl.expr_instance = inst;
  return absl::OkStatus();
}

absl::Status ResolveExports(EvalInstance::Impl& impl) {
  wasmtime_context_t* ctx = wasmtime_store_context(impl.store);
  wasmtime_extern_t mem_ext;
  const char kMem[] = "memory";
  if (!wasmtime_instance_export_get(ctx, &impl.expr_instance, kMem,
                                    sizeof(kMem) - 1, &mem_ext) ||
      mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
    return absl::FailedPreconditionError(
        "expr module does not export `memory`");
  }
  impl.memory = mem_ext.of.memory;

  wasmtime_extern_t fn_ext;
  const absl::string_view fn_name = impl.opts.eval_export_name;
  if (!wasmtime_instance_export_get(ctx, &impl.expr_instance, fn_name.data(),
                                    fn_name.size(), &fn_ext) ||
      fn_ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError(
        absl::StrCat("expr module does not export `", fn_name, "`"));
  }
  impl.eval_fn = fn_ext.of.func;
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<EvalInstance> EvalInstance::Create(
    absl::Span<const uint8_t> expr_wasm_bytes, EvalInstanceOptions opts) {
  auto impl = std::make_unique<Impl>();
  impl->opts = std::move(opts);
  if (auto s = InitHandles(*impl); !s.ok()) return s;
  if (auto s = CompileAndInstantiateExpr(*impl, expr_wasm_bytes); !s.ok()) {
    return s;
  }
  if (auto s = ResolveExports(*impl); !s.ok()) return s;
  return EvalInstance(std::move(impl));
}

absl::StatusOr<uint32_t> EvalInstance::CallEval() {
  wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);
  wasmtime_val_t result;
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_func_call(ctx, &impl_->eval_fn, /*args=*/nullptr, /*nargs=*/0,
                         &result, /*nresults=*/1, &trap);
  if (err != nullptr) return WasmtimeErrorToStatus("func_call($eval)", err);
  if (trap != nullptr) return WasmTrapToStatus("$eval trapped", trap);
  if (result.kind != WASMTIME_I32) {
    return absl::FailedPreconditionError(absl::StrCat(
        "$eval returned non-i32 (kind=", static_cast<int>(result.kind), ")"));
  }
  return static_cast<uint32_t>(result.of.i32);
}

absl::StatusOr<std::vector<uint8_t>> EvalInstance::ReadBytes(
    uint32_t offset, uint32_t len) const {
  wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);
  const uint8_t* data = wasmtime_memory_data(ctx, &impl_->memory);
  const size_t size = wasmtime_memory_data_size(ctx, &impl_->memory);
  if (static_cast<uint64_t>(offset) + len > size) {
    return absl::OutOfRangeError(absl::StrCat("ReadBytes: [", offset, ", ",
                                              offset + len,
                                              ") exceeds memory size ", size));
  }
  return std::vector<uint8_t>(data + offset, data + offset + len);
}

size_t EvalInstance::memory_size_bytes() const {
  wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);
  return wasmtime_memory_data_size(ctx, &impl_->memory);
}

}  // namespace celwasm
