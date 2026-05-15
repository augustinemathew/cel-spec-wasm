#include "compiler_v2/api/engine.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/api/internal/abi_decode.h"
#include "compiler_v2/api/internal/instance_impl.h"
#include "compiler_v2/api/internal/wasmtime_engine_state.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/host/cel_log.h"
#include "compiler_v2/runtime/cel_runtime_wasm_bytes.h"
#include "google/protobuf/descriptor.h"
#include "wasm.h"
#include "wasmtime.h"

namespace cel {

namespace {

// ——— Status / error helpers ———

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

absl::StatusOr<std::shared_ptr<celwasm::WasmtimeEngineState>> InitWasmtime() {
  auto state = std::make_shared<celwasm::WasmtimeEngineState>();
  // M3.C: the cel_runtime module's `cel_map_lookup` dispatcher emits
  // `return_call` (via clang `__attribute__((musttail))` + `-mtail-call`).
  // wasmtime rejects modules using the tail-call feature unless the host
  // opts in via the engine's config.
  wasm_config_t* config = wasm_config_new();
  if (config == nullptr) {
    return absl::InternalError("wasm_config_new returned null");
  }
  wasmtime_config_wasm_tail_call_set(config, true);
  state->engine = wasm_engine_new_with_config(config);
  if (state->engine == nullptr) {
    return absl::InternalError("wasm_engine_new_with_config returned null");
  }
  wasmtime_error_t* err = wasmtime_module_new(
      state->engine, celwasm::kCelRuntimeWasmBytes,
      celwasm::kCelRuntimeWasmBytesSize, &state->runtime_module);
  if (err != nullptr) {
    return absl::InternalError(
        absl::StrCat("wasmtime_module_new(runtime): ",
                     WasmtimeErrorToStatus("", err).message()));
  }
  return state;
}

// ——— Plan helpers ———

// Allocates the per-Plan store + a 2-page host-owned memory.  Two
// pages matches cel_runtime.wasm's --import-memory min=2 (per
// runtime/BUILD.bazel).  `max_present=false` so `wasmtime_memory_grow`
// can extend the memory beyond the initial reservation — Slice 0 of
// the conformance unlock plan needs a host-managed region above
// `mem_size_bytes` (= the codegen's `arena_limit`) to host
// activation-marshalled string / bytes payloads that survive
// `cel_reset` and subsequent `cel_alloc` calls inside `$eval`.
// Wasm-side `cel_alloc`'s bounds check stays at `arena_limit`, so
// the runtime never reaches into the grown tail.
absl::Status InitStoreAndMemory(celwasm::WasmtimeEngineState* state,
                                celwasm::InstanceImpl* impl) {
  impl->store = wasmtime_store_new(state->engine, nullptr, nullptr);
  if (impl->store == nullptr) {
    return absl::InternalError("wasmtime_store_new returned null");
  }
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  wasm_memorytype_t* mty = nullptr;
  wasmtime_error_t* err = wasmtime_memorytype_new(
      /*min=*/2, /*max_present=*/false, /*max=*/0, /*is_64=*/false,
      /*shared=*/false, /*page_size_log2=*/16, &mty);
  if (err != nullptr) return WasmtimeErrorToStatus("memorytype_new", err);
  err = wasmtime_memory_new(ctx, mty, &impl->memory);
  wasm_memorytype_delete(mty);
  if (err != nullptr) return WasmtimeErrorToStatus("memory_new", err);
  return absl::OkStatus();
}

// Wires cel_env.cel_log + cel.memory + cel_host.* onto a fresh
// linker.  The cel_host imports point at trampolines whose
// stub-bodies fire `ABSL_CHECK(false)` (`CelGetFieldImpl` /
// `CelHasFieldImpl` until M2.C.0b lands the real bodies);
// `cel_host.cel_map_lookup` is the only host-resident M3 path —
// arena-resident map programs evaluate without ever calling it.
// We register all three regardless so the runtime module
// instantiates: the runtime's static import list demands every
// `cel_host.*` symbol by name (no lazy import tracking, per the
// project rule).
absl::Status InitLinker(celwasm::WasmtimeEngineState* state,
                        celwasm::InstanceImpl* impl) {
  impl->linker = wasmtime_linker_new(state->engine);
  if (impl->linker == nullptr) {
    return absl::InternalError("wasmtime_linker_new returned null");
  }
  if (auto s = celwasm::RegisterCelLog(impl->linker); !s.ok()) return s;
  if (auto s = celwasm::RegisterCelHostImports(impl->linker, &impl->host_env);
      !s.ok()) {
    return s;
  }
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  wasmtime_extern_t mem_ext;
  mem_ext.kind = WASMTIME_EXTERN_MEMORY;
  mem_ext.of.memory = impl->memory;
  wasmtime_error_t* err = wasmtime_linker_define(impl->linker, ctx, "cel", 3,
                                                 "memory", 6, &mem_ext);
  if (err != nullptr) {
    return WasmtimeErrorToStatus("linker.define(cel.memory)", err);
  }
  return absl::OkStatus();
}

// Pulls a function export off `inst` and binds it onto the linker
// under (cel, name).  Used to wire the runtime's cel_reset /
// cel_alloc exports as imports the expr module sees.
absl::Status BindRuntimeExport(wasmtime_linker_t* linker,
                               wasmtime_context_t* ctx,
                               const wasmtime_instance_t& inst,
                               absl::string_view name) {
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(ctx, &inst, name.data(), name.size(),
                                    &ext)) {
    return absl::FailedPreconditionError(
        absl::StrCat("runtime instance has no export `", name, "`"));
  }
  wasmtime_error_t* err = wasmtime_linker_define(
      linker, ctx, "cel", 3, name.data(), name.size(), &ext);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(absl::StrCat("linker.define(cel.", name, ")"),
                                 err);
  }
  return absl::OkStatus();
}

// Every runtime export the expr module may import.  No lazy
// tracking — codegen always links the runtime fully (per repo
// rule); a missing entry here surfaces as
// `instantiate(expr): unknown import: cel::<name>`.
//
// M5.F adds every kBuiltinSeeds helper that ships a runtime
// export today.  M5.D step 2 added the seven aggregate-op
// dispatchers (`cel_list_size` / `cel_list_in` / `cel_list_eq` /
// `cel_list_concat` / `cel_map_size` / `cel_map_in` / `cel_map_eq`),
// each `__attribute__((musttail))`-dispatching to either an
// `_arena` fast path or a `cel_host.*` import.
absl::Status BindAllRuntimeExports(celwasm::InstanceImpl* impl,
                                   wasmtime_context_t* ctx) {
  static const char* const kRuntimeExports[] = {
      "cel_reset", "cel_alloc", "cel_map_create", "cel_map_insert",
      "cel_map_lookup_arena", "cel_map_lookup", "cel_list_create",
      "cel_list_set", "cel_list_at_arena", "cel_list_at",
      // M5.B step 1: arithmetic helpers.
      "cel_int_add_at_vv", "cel_int_sub_at_vv", "cel_int_mul_at_vv",
      "cel_int_div_at_vv", "cel_int_mod_at_vv", "cel_int_neg_at_v",
      "cel_uint_add_at_vv", "cel_uint_sub_at_vv", "cel_uint_mul_at_vv",
      "cel_uint_div_at_vv", "cel_uint_mod_at_vv", "cel_double_add_at_vv",
      "cel_double_sub_at_vv", "cel_double_mul_at_vv", "cel_double_div_at_vv",
      "cel_double_neg_at_v",
      // M5.B step 1: comparison helpers.
      "cel_int_lt_at_vv", "cel_int_le_at_vv", "cel_int_gt_at_vv",
      "cel_int_ge_at_vv", "cel_uint_lt_at_vv", "cel_uint_le_at_vv",
      "cel_uint_gt_at_vv", "cel_uint_ge_at_vv", "cel_double_lt_at_vv",
      "cel_double_le_at_vv", "cel_double_gt_at_vv", "cel_double_ge_at_vv",
      "cel_bool_lt_at_vv", "cel_bool_le_at_vv", "cel_bool_gt_at_vv",
      "cel_bool_ge_at_vv",
      // M5.B step 2: cross-type numeric ladder.
      "cel_numeric_lt_at_vv", "cel_numeric_le_at_vv", "cel_numeric_gt_at_vv",
      "cel_numeric_ge_at_vv",
      // M5.C: string + bytes ops.
      "cel_string_concat_at_vv", "cel_string_size_at_v", "cel_string_lt_at_vv",
      "cel_string_le_at_vv", "cel_string_gt_at_vv", "cel_string_ge_at_vv",
      "cel_string_contains_at_vv", "cel_string_starts_with_at_vv",
      "cel_string_ends_with_at_vv", "cel_bytes_concat_at_vv",
      "cel_bytes_size_at_v", "cel_bytes_lt_at_vv", "cel_bytes_le_at_vv",
      "cel_bytes_gt_at_vv", "cel_bytes_ge_at_vv",
      // M5.D step 2: aggregate kDynamic dispatchers.
      "cel_list_size", "cel_list_in", "cel_list_eq", "cel_list_concat",
      "cel_map_size", "cel_map_in", "cel_map_eq",
      // M5.B step 2b: polymorphic equality.
      "cel_equals_at_vv", "cel_not_equals_at_vv",
      // M5.G (Slice 2): 3VL / control-flow helpers.
      "cel_and", "cel_or", "cel_not", "cel_unknown_merge", "cel_copy_slot",
      // M9.B: type-of helper.
      "cel_type_of_at_v"};
  for (const char* name : kRuntimeExports) {
    if (auto s =
            BindRuntimeExport(impl->linker, ctx, impl->runtime_instance, name);
        !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

absl::Status InstantiateRuntime(celwasm::WasmtimeEngineState* state,
                                celwasm::InstanceImpl* impl) {
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_linker_instantiate(
      impl->linker, ctx, state->runtime_module, &impl->runtime_instance, &trap);
  if (err != nullptr) return WasmtimeErrorToStatus("instantiate(runtime)", err);
  if (trap != nullptr) {
    return WasmTrapToStatus("instantiate(runtime) trapped", trap);
  }
  if (auto s = BindAllRuntimeExports(impl, ctx); !s.ok()) return s;

  // Populate the layer-3 callback env now that the runtime
  // instance is live: the cel_host trampolines need a func handle
  // to `cel_alloc` (for span payload allocation) + the memory
  // handle (for CelValue + span reads/writes).  Both are tied to
  // this store; resetting the table happens per-Eval.
  impl->host_env.memory = impl->memory;
  wasmtime_extern_t alloc_ext;
  if (!wasmtime_instance_export_get(ctx, &impl->runtime_instance, "cel_alloc",
                                    9, &alloc_ext)) {
    return absl::FailedPreconditionError(
        "runtime instance has no export `cel_alloc` (cel_host needs it)");
  }
  if (alloc_ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError("`cel_alloc` is not a function");
  }
  impl->host_env.cel_alloc_fn = alloc_ext.of.func;
  return absl::OkStatus();
}

absl::Status InstantiateExpr(celwasm::WasmtimeEngineState* state,
                             celwasm::InstanceImpl* impl,
                             absl::Span<const uint8_t> bytes) {
  wasmtime_error_t* err = wasmtime_module_new(state->engine, bytes.data(),
                                              bytes.size(), &impl->expr_module);
  if (err != nullptr) return WasmtimeErrorToStatus("module_new(expr)", err);
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  wasm_trap_t* trap = nullptr;
  err = wasmtime_linker_instantiate(impl->linker, ctx, impl->expr_module,
                                    &impl->expr_instance, &trap);
  if (err != nullptr) return WasmtimeErrorToStatus("instantiate(expr)", err);
  if (trap != nullptr) {
    return WasmTrapToStatus("instantiate(expr) trapped", trap);
  }
  wasmtime_extern_t ext;
  if (!wasmtime_instance_export_get(ctx, &impl->expr_instance, "eval", 4,
                                    &ext)) {
    return absl::FailedPreconditionError("expr module does not export `eval`");
  }
  if (ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError(
        "expr module's `eval` export is not a function");
  }
  impl->eval_fn = ext.of.func;
  return absl::OkStatus();
}

}  // namespace

// ——— Engine ———

Engine::Engine(std::shared_ptr<celwasm::WasmtimeEngineState> wasmtime)
    : wasmtime_(std::move(wasmtime)) {}

Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

Engine::Builder Engine::NewBuilder() {
  return {};
}

// ——— Engine::Plan ———

absl::StatusOr<Instance> Engine::Plan(const Program& program) const {
  auto impl = std::make_unique<celwasm::InstanceImpl>();
  if (auto s = InitStoreAndMemory(wasmtime_.get(), impl.get()); !s.ok()) {
    return s;
  }
  if (auto s = InitLinker(wasmtime_.get(), impl.get()); !s.ok()) return s;
  if (auto s = InstantiateRuntime(wasmtime_.get(), impl.get()); !s.ok()) {
    return s;
  }
  if (auto s =
          InstantiateExpr(wasmtime_.get(), impl.get(), program.wasm_bytes());
      !s.ok()) {
    return s;
  }
  // Decode the `cel.abi` custom section and park it on the Instance.
  // Instance::Eval(Activation) consults `impl->abi.by_name` at
  // call time to marshal bound values into their workspace slots.
  // NotFound is tolerated: M1-era modules + synthetic WAT fixtures
  // don't carry the section, and a variable-free Eval() still
  // works — the decoded abi just stays empty.
  auto abi_or = celwasm::DecodeCelAbiFromWasm(program.wasm_bytes());
  if (abi_or.ok()) {
    impl->abi = *std::move(abi_or);
  } else if (abi_or.status().code() != absl::StatusCode::kNotFound) {
    return abi_or.status();
  }

  // M2.C: populate host_env.bindings (field_refs + attributes) from
  // the decoded ABI so the cel_host trampolines can resolve
  // field_ref_id → (field_number, field_name) and attribute_id →
  // (root, qualifiers).  unknown_patterns stays empty for Eval();
  // PartialEval rebinds it per-call.
  //
  // M7.A: pass the generated descriptor pool so `BuildCelHostBindings`
  // can resolve `cel.abi.types[]` FQNs to `Descriptor*` for
  // `cel_make_message` lookups.  Statically-linked cc_proto_library
  // descriptors are reachable through `generated_pool()`; dynamic
  // schemas (SchemaProtoSource) are an M7.A-polish follow-up.
  celwasm::BuildCelHostBindings(
      impl->abi, google::protobuf::DescriptorPool::generated_pool(),
      impl->host_env);

  return Instance(wasmtime_, std::move(impl));
}

// ——— Engine::Builder ———

absl::StatusOr<Engine> Engine::Builder::Build() && {
  auto state_or = InitWasmtime();
  if (!state_or.ok()) return state_or.status();
  return Engine(std::move(*state_or));
}

}  // namespace cel
