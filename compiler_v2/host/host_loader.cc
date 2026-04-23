#include "compiler_v2/host/host_loader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/host/cel_log.h"
#include "compiler_v2/runtime/cel_runtime_wasm_bytes.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {

namespace {

// Arena cursor / limit live at fixed linear-memory offsets per
// runtime/cel_memory.h §8.2.  `cel_reset(base, limit)` writes the
// pair; M1's host-side trampoline replicates that so we don't need a
// cross-module call into the runtime's C implementation.
constexpr uint32_t kArenaCursorOffset = 8;
constexpr uint32_t kArenaLimitOffset = 12;

// Pulled out so every wasmtime error turns into an absl::Status with
// a uniform message shape — easier to match on in tests and clearer
// when the failure surfaces in a log.
absl::Status WasmtimeErrorToStatus(absl::string_view context,
                                   wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  std::string text(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return absl::FailedPreconditionError(absl::StrCat(context, ": ", text));
}

// wasm_trap_t carries the same shape — emit messages the same way.
absl::Status WasmTrapToStatus(absl::string_view context, wasm_trap_t* trap) {
  wasm_byte_vec_t msg;
  wasm_trap_message(trap, &msg);
  std::string text(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasm_trap_delete(trap);
  return absl::InternalError(absl::StrCat(context, ": ", text));
}

// Host-side trampoline for `cel.cel_reset(base: i32, limit: i32)
// -> ()`.  Writes the two u32s to linear-memory bytes 8..16 of the
// caller's exported memory so the runtime's arena starts fresh at
// the top of every `$eval`.  Matches cel_runtime.c::cel_reset
// semantics byte-for-byte; a later milestone that exercises
// arena-backed cel_alloc will swap this out for a trampoline that
// forwards into the runtime module's export.
wasm_trap_t* CelResetTrampoline(void* /*data*/, wasmtime_caller_t* caller,
                                const wasmtime_val_t* args, size_t nargs,
                                wasmtime_val_t* /*results*/,
                                size_t /*nresults*/) {
  ABSL_CHECK(nargs == 2) << "cel_reset expects (i32, i32)";
  const auto base = static_cast<uint32_t>(args[0].of.i32);
  const auto limit = static_cast<uint32_t>(args[1].of.i32);

  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  wasmtime_extern_t ext;
  const char kName[] = "memory";
  if (!wasmtime_caller_export_get(caller, kName, sizeof(kName) - 1, &ext) ||
      ext.kind != WASMTIME_EXTERN_MEMORY) {
    // Should be impossible — the caller (expr module) always exports
    // memory.  Trap hard so the bug is visible rather than silently
    // dropping the arena reset.
    const char kMsg[] = "cel_reset: caller has no exported `memory`";
    return wasmtime_trap_new(kMsg, sizeof(kMsg) - 1);
  }
  uint8_t* data = wasmtime_memory_data(ctx, &ext.of.memory);
  const size_t size = wasmtime_memory_data_size(ctx, &ext.of.memory);
  ABSL_CHECK(size >= kArenaLimitOffset + sizeof(uint32_t))
      << "cel_reset: memory too small for arena cursor";
  std::memcpy(data + kArenaCursorOffset, &base, sizeof(uint32_t));
  std::memcpy(data + kArenaLimitOffset, &limit, sizeof(uint32_t));
  wasmtime_extern_delete(&ext);
  (void)ctx;  // not needed after this point
  return nullptr;
}

// Host-side trampoline for `cel.cel_alloc(nbytes: i32) -> i32`.  M1
// scalar-literal evals never hit this path (literals live in
// `.rodata`; no runtime allocation).  Trap loudly if it ever fires —
// an expr that allocates at M1 is a codegen regression, and silent
// fallbacks would hide it.
wasm_trap_t* CelAllocTrampoline(void* /*data*/, wasmtime_caller_t* /*caller*/,
                                const wasmtime_val_t* /*args*/,
                                size_t /*nargs*/, wasmtime_val_t* /*results*/,
                                size_t /*nresults*/) {
  const char kMsg[] =
      "cel_alloc: unexpected host-side call at M1 (scalar-literal "
      "evals should never allocate)";
  return wasmtime_trap_new(kMsg, sizeof(kMsg) - 1);
}

// Two-i32 -> () type, shared by both `cel_reset` (2 params, 0 returns)
// and `cel_alloc` (1 param, 1 return) — each allocates its own copy.
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

// Define `cel.cel_reset` + `cel.cel_alloc` host trampolines on the
// linker.  `cel_env.cel_log` is installed via `RegisterCelLog`
// separately — it's the existing M1-ready host-side binding.
absl::Status InstallHostImports(wasmtime_linker_t* linker) {
  auto s = DefineLinkerFunc(linker, "cel", "cel_reset", I32I32ToVoidType(),
                            CelResetTrampoline);
  if (!s.ok()) return s;
  s = DefineLinkerFunc(linker, "cel", "cel_alloc", I32ToI32Type(),
                       CelAllocTrampoline);
  if (!s.ok()) return s;
  return RegisterCelLog(linker);
}

absl::StatusOr<wasmtime_instance_t> InstantiateModule(
    wasmtime_context_t* ctx, wasmtime_linker_t* linker,
    wasmtime_module_t* module, absl::string_view module_name_for_errors) {
  wasmtime_instance_t instance;
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_linker_instantiate(linker, ctx, module, &instance, &trap);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(
        absl::StrCat("instantiate(", module_name_for_errors, ")"), err);
  }
  if (trap != nullptr) {
    return WasmTrapToStatus(
        absl::StrCat("instantiate(", module_name_for_errors, ") trapped"),
        trap);
  }
  return instance;
}

absl::StatusOr<wasmtime_memory_t> GetMemoryExport(
    wasmtime_context_t* ctx, const wasmtime_instance_t& instance) {
  wasmtime_extern_t ext;
  const char kName[] = "memory";
  if (!wasmtime_instance_export_get(ctx, &instance, kName, sizeof(kName) - 1,
                                    &ext)) {
    return absl::FailedPreconditionError(
        "expr module does not export `memory`");
  }
  if (ext.kind != WASMTIME_EXTERN_MEMORY) {
    wasmtime_extern_delete(&ext);
    return absl::FailedPreconditionError(
        "expr module exports `memory` but it is not a memory kind");
  }
  return ext.of.memory;
}

absl::StatusOr<wasmtime_func_t> GetFuncExport(
    wasmtime_context_t* ctx, const wasmtime_instance_t& instance,
    absl::string_view name) {
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(ctx, &instance, name.data(), name.size(),
                                    &ext)) {
    return absl::FailedPreconditionError(
        absl::StrCat("expr module does not export `", name, "`"));
  }
  if (ext.kind != WASMTIME_EXTERN_FUNC) {
    wasmtime_extern_delete(&ext);
    return absl::FailedPreconditionError(absl::StrCat(
        "expr module exports `", name, "` but it is not a function"));
  }
  return ext.of.func;
}

absl::Status DefineMemoryOnLinker(wasmtime_linker_t* linker,
                                  wasmtime_context_t* ctx,
                                  wasmtime_memory_t memory) {
  const char kModule[] = "cel";
  const char kName[] = "memory";
  wasmtime_extern_t ext;
  ext.kind = WASMTIME_EXTERN_MEMORY;
  ext.of.memory = memory;
  wasmtime_error_t* err =
      wasmtime_linker_define(linker, ctx, kModule, sizeof(kModule) - 1, kName,
                             sizeof(kName) - 1, &ext);
  if (err != nullptr) {
    return WasmtimeErrorToStatus("linker.define(cel.memory)", err);
  }
  return absl::OkStatus();
}

}  // namespace

// ————————— Impl —————————

struct EvalInstance::Impl {
  wasm_engine_t* engine = nullptr;
  wasmtime_store_t* store = nullptr;
  wasmtime_linker_t* linker = nullptr;
  wasmtime_module_t* expr_module = nullptr;
  wasmtime_module_t* runtime_module = nullptr;
  // wasmtime handle structs are trivially default-constructed; the
  // `{}`s silence cppcoreguidelines-pro-type-member-init without
  // changing runtime behaviour (they're overwritten before use).
  wasmtime_instance_t expr_instance{};
  wasmtime_instance_t runtime_instance{};
  wasmtime_memory_t memory{};
  wasmtime_func_t eval_fn{};
  EvalInstanceOptions opts;

  ~Impl() {
    if (expr_module != nullptr) wasmtime_module_delete(expr_module);
    if (runtime_module != nullptr) wasmtime_module_delete(runtime_module);
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

// ——— Create phases ———
// Factored into three helpers so EvalInstance::Create stays under the
// 60-line function-size ceiling enforced by clang-tidy.  Each helper
// mutates `impl` in place and returns OK on success; failures bubble
// up through Create's chain.

absl::Status InitWasmtimeHandles(EvalInstance::Impl& impl) {
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
  // Install host-side trampolines for cel.cel_reset, cel.cel_alloc,
  // and cel_env.cel_log.  Expr + runtime both resolve at least one of
  // these when instantiated, so they must be defined first.
  return InstallHostImports(impl.linker);
}

absl::Status InstantiateExprModule(EvalInstance::Impl& impl,
                                   absl::Span<const uint8_t> wasm_bytes) {
  wasmtime_error_t* err = wasmtime_module_new(
      impl.engine, wasm_bytes.data(), wasm_bytes.size(), &impl.expr_module);
  if (err != nullptr) return WasmtimeErrorToStatus("module_new(expr)", err);
  wasmtime_context_t* ctx = wasmtime_store_context(impl.store);
  auto inst_or = InstantiateModule(ctx, impl.linker, impl.expr_module, "expr");
  if (!inst_or.ok()) return inst_or.status();
  impl.expr_instance = *inst_or;
  auto mem_or = GetMemoryExport(ctx, impl.expr_instance);
  if (!mem_or.ok()) return mem_or.status();
  impl.memory = *mem_or;
  return absl::OkStatus();
}

absl::Status InstantiateRuntimeModule(EvalInstance::Impl& impl) {
  wasmtime_context_t* ctx = wasmtime_store_context(impl.store);
  if (auto s = DefineMemoryOnLinker(impl.linker, ctx, impl.memory); !s.ok()) {
    return s;
  }
  const absl::Span<const uint8_t> bytes =
      impl.opts.runtime_wasm_bytes.empty()
          ? absl::MakeConstSpan(
                reinterpret_cast<const uint8_t*>(kCelRuntimeWasmBytes),
                kCelRuntimeWasmBytesSize)
          : impl.opts.runtime_wasm_bytes;
  wasmtime_error_t* err = wasmtime_module_new(
      impl.engine, bytes.data(), bytes.size(), &impl.runtime_module);
  if (err != nullptr) return WasmtimeErrorToStatus("module_new(runtime)", err);
  auto inst_or =
      InstantiateModule(ctx, impl.linker, impl.runtime_module, "runtime");
  if (!inst_or.ok()) return inst_or.status();
  impl.runtime_instance = *inst_or;
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<EvalInstance> EvalInstance::Create(
    absl::Span<const uint8_t> expr_wasm_bytes, EvalInstanceOptions opts) {
  auto impl = std::make_unique<Impl>();
  impl->opts = std::move(opts);

  if (auto s = InitWasmtimeHandles(*impl); !s.ok()) return s;
  if (auto s = InstantiateExprModule(*impl, expr_wasm_bytes); !s.ok()) return s;
  if (auto s = InstantiateRuntimeModule(*impl); !s.ok()) return s;

  // Resolve the entry function for later CallEval invocations.
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  auto fn_or =
      GetFuncExport(ctx, impl->expr_instance, impl->opts.eval_export_name);
  if (!fn_or.ok()) return fn_or.status();
  impl->eval_fn = *fn_or;

  return EvalInstance(std::move(impl));
}

absl::StatusOr<uint32_t> EvalInstance::CallEval() {
  wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);
  wasmtime_val_t result;
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_func_call(ctx, &impl_->eval_fn, /*args=*/nullptr, /*nargs=*/0,
                         &result, /*nresults=*/1, &trap);
  if (err != nullptr) {
    return WasmtimeErrorToStatus("func_call($eval)", err);
  }
  if (trap != nullptr) {
    return WasmTrapToStatus("$eval trapped", trap);
  }
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
