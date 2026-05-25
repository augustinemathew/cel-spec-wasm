#include "compiler_v2/api/engine.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/abi/runtime_catalogue.h"
#include "compiler_v2/api/internal/abi_decode.h"
#include "compiler_v2/api/internal/instance_impl.h"
#include "compiler_v2/api/internal/wasmtime_engine_state.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/host/cel_log.h"
#include "compiler_v2/runtime/cel_layout.h"
#include "compiler_v2/runtime/cel_runtime_wasm_bytes.h"
#include "google/protobuf/descriptor.h"
#include "wasi.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm::api {

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

// Phase C: register wasmtime's built-in WASI preview1 implementation
// onto the linker.  Replaces the hand-rolled random_get / sched_yield
// stubs once absl + cctz are linked into cel_runtime.wasm — those
// libs pull in ~10 wasi imports (environ_get / fd_close / fd_seek /
// proc_exit / ...) on top, all of which wasmtime can resolve via its
// reference implementation.  The actual WASI behaviour (env, stdio)
// is supplied per-instance via `wasmtime_context_set_wasi`.
absl::Status RegisterWasiStubs(wasmtime_linker_t* linker) {
  wasmtime_error_t* err = wasmtime_linker_define_wasi(linker);
  if (err != nullptr) {
    return WasmtimeErrorToStatus("linker.define_wasi", err);
  }
  return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<celwasm::WasmtimeEngineState>> InitWasmtime() {
  auto state = std::make_shared<celwasm::WasmtimeEngineState>();
  // The cel_runtime module's aggregate-op dispatchers
  // (`cel_map_lookup`, etc.) emit `return_call` via clang
  // `__attribute__((musttail))` + `-mtail-call`.  wasmtime rejects
  // modules using the tail-call feature unless the host opts in.
  wasm_config_t* config = wasm_config_new();
  if (config == nullptr) {
    return absl::InternalError("wasm_config_new returned null");
  }
  wasmtime_config_wasm_tail_call_set(config, true);
  // cel_runtime.wasm is built against wasm32-wasi-threads (cctz
  // needs `<mutex>`).  The module declares its memory as shared and
  // may import `wasi_snapshot_preview1.sched_yield`; both require
  // the wasm threads proposal + shared memory support enabled here.
  // The runtime never *calls* threading primitives — wasm-ld just
  // keeps these imports alive as part of wasi-libc's surface.  See
  // `rewrite/phase-c-plan.md` §7.2.
  wasmtime_config_wasm_threads_set(config, true);
  wasmtime_config_shared_memory_set(config, true);
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

// Allocates the per-Plan store.  Memory ownership: the runtime
// module declares + exports its own memory via
// `runtime/BUILD.bazel:--export=memory`.  After `InstantiateRuntime`
// finishes, `PullRuntimeMemory` pulls the exported memory off
// `runtime_instance` and binds it on the linker as `cel.memory` so
// the expr module's `(import "cel" "memory")` resolves to the same
// backing store.  See `rewrite/wasi/DESIGN.md` §3-4.
absl::Status InitStore(celwasm::WasmtimeEngineState* state,
                       celwasm::InstanceImpl* impl) {
  impl->store = wasmtime_store_new(state->engine, nullptr, nullptr);
  if (impl->store == nullptr) {
    return absl::InternalError("wasmtime_store_new returned null");
  }
  // cel_runtime.wasm links absl + cctz, which pulls in wasi-libc
  // functions that reference env / stdio / clocks.  Hand wasmtime
  // a minimal wasi_config (no inherited env / stdio) so the
  // imports defined via `wasmtime_linker_define_wasi` resolve into a
  // sandboxed implementation that returns deterministic "empty
  // environment" responses.  Without this call wasmtime's WASI
  // functions trap on first use.
  wasi_config_t* wasi = wasi_config_new();
  if (wasi == nullptr) {
    return absl::InternalError("wasi_config_new returned null");
  }
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  wasmtime_error_t* err = wasmtime_context_set_wasi(ctx, wasi);
  // `wasmtime_context_set_wasi` takes ownership of `wasi`; do not
  // free here even on error.
  if (err != nullptr) {
    return WasmtimeErrorToStatus("context_set_wasi", err);
  }
  return absl::OkStatus();
}

// Wires cel_env.cel_log + cel_host.* + WASI stubs onto a fresh
// linker.  `cel.memory` is NOT bound here — the runtime module
// owns + exports its own memory; the binding lands after
// `InstantiateRuntime` (see `BindRuntimeMemory`).
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
  if (auto s = RegisterWasiStubs(impl->linker); !s.ok()) return s;
  return absl::OkStatus();
}

// Pull the runtime instance's exported `memory` and bind it on the
// linker as `cel.memory` so the expr module's
// `(import "cel" "memory" ...)` resolves to the same backing
// store.  Cache the handle on InstanceImpl so the host's activation
// marshalling + result decode can call `wasmtime_memory_data`
// against it without re-pulling.
absl::Status BindRuntimeMemory(wasmtime_context_t* ctx,
                               celwasm::InstanceImpl* impl) {
  wasmtime_extern_t mem_ext;
  if (!wasmtime_instance_export_get(ctx, &impl->runtime_instance, "memory", 6,
                                    &mem_ext)) {
    return absl::FailedPreconditionError(
        "runtime instance has no export `memory`");
  }
  // Phase C: the runtime is built for wasm32-wasi-threads and exports
  // its memory as shared.  The expr module imports `cel.memory` with
  // matching shared shape (codegen sets `shared=true` on the import,
  // see `WasmModule::AddMemoryImport`).
  if (mem_ext.kind != WASMTIME_EXTERN_SHAREDMEMORY) {
    return absl::FailedPreconditionError(absl::StrCat(
        "`memory` is not a shared memory (kind=", mem_ext.kind, ")"));
  }
  // The handle pulled out of `wasmtime_instance_export_get` is a
  // shared-memory pointer owned by the store; we clone it so this
  // InstanceImpl owns its own refcounted handle (deleted in the dtor).
  impl->memory = wasmtime_sharedmemory_clone(mem_ext.of.sharedmemory);
  wasmtime_error_t* err = wasmtime_linker_define(impl->linker, ctx, "cel", 3,
                                                 "memory", 6, &mem_ext);
  if (err != nullptr) {
    return WasmtimeErrorToStatus("linker.define(cel.memory)", err);
  }
  return absl::OkStatus();
}

// Pulls a function export off `inst` and binds it onto the linker
// under (cel, name).  Used to wire the runtime's arena_reset /
// arena_alloc exports as imports the expr module sees.
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
// Categories below mirror the seed grouping in
// The pre-2026-05-22 hand-maintained `kRuntimeExports` array was
// removed in favour of `compiler_v2/abi/runtime_catalogue` — the
// `cel`-namespace span there drives both `BindAllRuntimeExports`
// below and codegen's import-declaration pass in
// `compile.cc::InstallOverloadImports`.  Single source of truth.

absl::Status BindAllRuntimeExports(celwasm::InstanceImpl* impl,
                                   wasmtime_context_t* ctx) {
  // Derive the binding list from the ABI catalogue's `cel` namespace.
  // This is the single source of truth — codegen's import-declaration
  // pass (`compile.cc::InstallOverloadImports`) consumes the same
  // catalogue, so the import set + the bind set can't drift.  Each
  // helper name routes through the same `BindRuntimeExport` wrapper
  // that handles the wasmtime linker plumbing.
  for (const auto& h : celwasm::abi::CelRuntimeHelpers()) {
    // `arena_alloc` is bound separately under a fixed wasmtime handle
    // (see `BindRuntimeFuncHandles`) so cel_host trampolines can call
    // it without round-tripping through the linker.  Also re-binding
    // it under cel.arena_alloc here so the expr module's import call
    // resolves identically.
    if (auto s = BindRuntimeExport(impl->linker, ctx, impl->runtime_instance,
                                   std::string(h.name).c_str());
        !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

// Populate the layer-3 callback env's `arena_alloc` + `malloc`
// func handles + memory handle.  cel_host trampolines call into
// these for span payload allocation + activation marshalling.
absl::Status BindRuntimeFuncHandles(celwasm::InstanceImpl* impl,
                                    wasmtime_context_t* ctx) {
  impl->host_env.memory = impl->memory;
  wasmtime_extern_t alloc_ext;
  if (!wasmtime_instance_export_get(ctx, &impl->runtime_instance, "arena_alloc",
                                    11, &alloc_ext)) {
    return absl::FailedPreconditionError(
        "runtime instance has no export `arena_alloc` (cel_host needs it)");
  }
  if (alloc_ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError("`arena_alloc` is not a function");
  }
  impl->host_env.arena_alloc_fn = alloc_ext.of.func;

  // M7: handle for the runtime's `malloc` (wasi-libc dlmalloc).
  // Used to allocate the activation buffer — payloads that must
  // survive arena_reset and therefore can't live in the bump arena.
  wasmtime_extern_t malloc_ext;
  if (!wasmtime_instance_export_get(ctx, &impl->runtime_instance, "malloc", 6,
                                    &malloc_ext)) {
    return absl::FailedPreconditionError(
        "runtime instance has no export `malloc`");
  }
  if (malloc_ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError("`malloc` is not a function");
  }
  impl->host_env.malloc_fn = malloc_ext.of.func;
  return absl::OkStatus();
}

// Seed the runtime's bump arena before any eval runs.  `arena_alloc`
// traps on !initialized (see cel_arena.c "Unimplemented features"
// rule); arena_init must be called exactly once per Instance with
// the design's default capacity.
absl::Status SeedRuntimeArena(celwasm::InstanceImpl* impl,
                              wasmtime_context_t* ctx) {
  wasmtime_extern_t init_ext;
  if (!wasmtime_instance_export_get(ctx, &impl->runtime_instance, "arena_init",
                                    10, &init_ext)) {
    return absl::FailedPreconditionError(
        "runtime instance has no export `arena_init`");
  }
  if (init_ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError("`arena_init` is not a function");
  }
  wasmtime_val_t arg;
  arg.kind = WASMTIME_I32;
  arg.of.i32 = static_cast<int32_t>(CELWASM_ARENA_CAPACITY_BYTES);
  wasm_trap_t* init_trap = nullptr;
  wasmtime_error_t* init_err =
      wasmtime_func_call(ctx, &init_ext.of.func, &arg, /*nargs=*/1,
                         /*results=*/nullptr, /*nresults=*/0, &init_trap);
  if (init_err != nullptr) {
    return WasmtimeErrorToStatus("arena_init(CELWASM_ARENA_CAPACITY_BYTES)",
                                 init_err);
  }
  if (init_trap != nullptr) {
    return WasmTrapToStatus("arena_init trapped", init_trap);
  }
  return absl::OkStatus();
}

// A13 + A14 (DESIGN §5) invariant checks on the just-instantiated
// runtime: memory page floor + `__heap_base` global above the
// reserved low region.  Both are `ABSL_CHECK`s — regression
// surfaces as a fail-loud crash with the design-doc citation in
// the message.
void EnforceRuntimeMemoryInvariants(celwasm::InstanceImpl* impl,
                                    wasmtime_context_t* ctx) {
  ABSL_CHECK_GE(wasmtime_sharedmemory_size(impl->memory),
                CELWASM_INITIAL_MEMORY_PAGES)
      << "DESIGN A13: wasm memory page count below design floor";
  wasmtime_extern_t heap_base_ext;
  if (!wasmtime_instance_export_get(ctx, &impl->runtime_instance, "__heap_base",
                                    11, &heap_base_ext)) {
    // Pre-WASI / stripped build — A14 is a soft check.
    return;
  }
  ABSL_CHECK_EQ(heap_base_ext.kind, WASMTIME_EXTERN_GLOBAL)
      << "DESIGN A14: __heap_base must be a global";
  wasmtime_val_t hb_val;
  wasmtime_global_get(ctx, &heap_base_ext.of.global, &hb_val);
  ABSL_CHECK_EQ(hb_val.kind, WASMTIME_I32)
      << "DESIGN A14: __heap_base must be i32";
  ABSL_CHECK_GE(static_cast<uint32_t>(hb_val.of.i32),
                CELWASM_RESERVED_LOW_MEMORY_BYTES)
      << "DESIGN A14: __heap_base below reserved low region";
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
  // M6: pull the runtime's exported `memory` and bind it on the
  // linker BEFORE the expr module instantiates.  Also caches the
  // handle on impl->memory for the activation marshaller + decoder.
  if (auto s = BindRuntimeMemory(ctx, impl); !s.ok()) return s;
  EnforceRuntimeMemoryInvariants(impl, ctx);
  if (auto s = BindAllRuntimeExports(impl, ctx); !s.ok()) return s;
  if (auto s = BindRuntimeFuncHandles(impl, ctx); !s.ok()) return s;
  return SeedRuntimeArena(impl, ctx);
}

// ── M13 Slice C.1 — custom-fn helpers ────────────────────────────

// Reserved module-alias names — Engine::AddModule rejects these.
// `cel` / `cel_host` / `cel_env` / `cel_fn` are the engine's own
// import namespaces (runtime + host trampolines + custom-fn host
// callbacks).  `host` is reserved per the .celfn IDL convention
// (`@host.X` is the only legal host syntax; using `host` as a
// foreign alias would create confusing decls).
// `wasi_snapshot_preview1` is the WASI preview1 namespace —
// `RegisterWasiStubs` already populates the linker with this
// module, and overriding it would silently mask a stub.
bool IsReservedAlias(absl::string_view alias) {
  return alias == "cel" || alias == "cel_host" || alias == "cel_env" ||
         alias == "cel_fn" || alias == "host" ||
         alias == "wasi_snapshot_preview1";
}

// Functype for host callbacks: `arity` × i32 params, no results.
// Both AddModule's per-helper export bindings and AddFunction's
// host callbacks use this signature.
wasm_functype_t* MakeI32sToVoidFuncType(std::uint8_t arity) {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_new_uninitialized(&params, arity);
  for (size_t i = 0; i < arity; ++i) {
    params.data[i] = wasm_valtype_new(WASM_I32);
  }
  wasm_valtype_vec_t results;
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

// Build a wasm trap from an `absl::Status`'s message.  Used by the
// host-callback trampoline to surface user errors as wasm traps.
wasm_trap_t* TrapFromStatus(absl::string_view msg) {
  wasm_byte_vec_t m;
  wasm_byte_vec_new(&m, msg.size(), msg.data());
  wasm_trap_t* t = wasm_trap_new(nullptr, &m);
  wasm_byte_vec_delete(&m);
  return t;
}

// Trampoline: adapts wasmtime's func_callback_t shape to our raw
// HostCallback.  `env_ptr` points at a `HostFnEnv` (allocated
// per-Plan in `RegisterHostCallbacks`); reads memory through the
// env's borrowed `wasmtime_sharedmemory_t*` (matching the shape in
// `cel_host_wasmtime.cc::HostFieldTrampoline`).  We do NOT use
// `wasmtime_caller_export_get(caller, "memory", ...)` because the
// production expr codegen IMPORTS memory rather than exporting it,
// so the caller wouldn't satisfy that lookup.
//
// First arg is `out_slot`, remaining args are `arg_slots`.
wasm_trap_t* HostCallbackTrampoline(void* env_ptr, wasmtime_caller_t* caller,
                                    const wasmtime_val_t* args, size_t nargs,
                                    wasmtime_val_t* /*results*/,
                                    size_t /*nresults*/) {
  auto* env = static_cast<celwasm::HostFnEnv*>(env_ptr);
  if (env == nullptr || env->callback == nullptr ||
      !static_cast<bool>(*env->callback)) {
    return TrapFromStatus("host callback env was null");
  }
  if (env->memory == nullptr) {
    return TrapFromStatus("host callback env missing memory pointer");
  }
  if (nargs < 1) {
    return TrapFromStatus("host callback needs at least one arg (out_slot)");
  }
  (void)caller;  // unused — shared memory is reached via env

  uint8_t* mem = wasmtime_sharedmemory_data(env->memory);
  const size_t mem_size = wasmtime_sharedmemory_data_size(env->memory);

  const auto out_slot = static_cast<uint32_t>(args[0].of.i32);
  std::vector<uint32_t> arg_slots;
  arg_slots.reserve(nargs - 1);
  for (size_t i = 1; i < nargs; ++i) {
    arg_slots.push_back(static_cast<uint32_t>(args[i].of.i32));
  }

  absl::Status s = (*env->callback)(mem, mem_size, out_slot, arg_slots);
  if (!s.ok()) {
    return TrapFromStatus(s.message());
  }
  return nullptr;
}

// Instantiate each registered foreign custom module in the per-Plan
// store + bind its function exports under its alias on the linker.
// Runs after `InstantiateRuntime` (so cel.memory + cel.arena_alloc
// etc. are bound) and before `InstantiateExpr`.  Calls
// `_initialize` on each module if exported (§4.5.2 point 4).
absl::Status InstantiateAndBindCustomModules(
    celwasm::WasmtimeEngineState* state, celwasm::InstanceImpl* impl) {
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  for (const auto& [alias, mod] : state->custom_modules) {
    wasmtime_instance_t inst;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_linker_instantiate(
        impl->linker, ctx, mod.module, &inst, &trap);
    if (err != nullptr) {
      return WasmtimeErrorToStatus(absl::StrCat("instantiate(", alias, ")"),
                                   err);
    }
    if (trap != nullptr) {
      return WasmTrapToStatus(absl::StrCat("instantiate(", alias, ") trapped"),
                              trap);
    }

    wasmtime_extern_t init_ext;
    if (wasmtime_instance_export_get(ctx, &inst, "_initialize", 11,
                                     &init_ext) &&
        init_ext.kind == WASMTIME_EXTERN_FUNC) {
      wasmtime_func_t init_fn = init_ext.of.func;
      wasm_trap_t* init_trap = nullptr;
      err = wasmtime_func_call(ctx, &init_fn, /*args=*/nullptr, /*nargs=*/0,
                               /*results=*/nullptr, /*nresults=*/0, &init_trap);
      if (err != nullptr) {
        return WasmtimeErrorToStatus(absl::StrCat(alias, "._initialize"), err);
      }
      if (init_trap != nullptr) {
        return WasmTrapToStatus(absl::StrCat(alias, "._initialize trapped"),
                                init_trap);
      }
    }

    for (const auto& helper : mod.helper_exports) {
      wasmtime_extern_t ext;
      if (!wasmtime_instance_export_get(ctx, &inst, helper.data(),
                                        helper.size(), &ext)) {
        continue;
      }
      wasmtime_error_t* def_err =
          wasmtime_linker_define(impl->linker, ctx, alias.data(), alias.size(),
                                 helper.data(), helper.size(), &ext);
      if (def_err != nullptr) {
        return WasmtimeErrorToStatus(
            absl::StrCat("define(", alias, ".", helper, ")"), def_err);
      }
    }
  }
  return absl::OkStatus();
}

// Register each `Engine::AddFunction`-registered callback on the
// linker as `cel_fn.<overload_id>`.  Each callback gets its own
// wasm_functype_t reflecting the user-supplied arity + its own
// per-Plan `HostFnEnv` carrying the InstanceImpl's shared memory
// pointer.  The env structs live on `impl->host_fn_envs` so their
// addresses stay stable for the instance's lifetime.
absl::Status RegisterHostCallbacks(celwasm::WasmtimeEngineState* state,
                                   celwasm::InstanceImpl* impl) {
  for (auto& [overload_id, rcb] : state->host_callbacks) {
    auto env = std::make_unique<celwasm::HostFnEnv>();
    env->callback = &rcb.callback;
    env->memory = impl->memory;  // borrowed; outlives the linker
    void* env_ptr = env.get();
    impl->host_fn_envs.push_back(std::move(env));

    wasm_functype_t* ftype = MakeI32sToVoidFuncType(rcb.num_args);
    wasmtime_error_t* err = wasmtime_linker_define_func(
        impl->linker, "cel_fn", 6, overload_id.data(), overload_id.size(),
        ftype, HostCallbackTrampoline, /*data=*/env_ptr,
        /*finalizer=*/nullptr);
    wasm_functype_delete(ftype);
    if (err != nullptr) {
      return WasmtimeErrorToStatus(
          absl::StrCat("linker.define_func(cel_fn.", overload_id, ")"), err);
    }
  }
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
  if (auto s = InitStore(wasmtime_.get(), impl.get()); !s.ok()) {
    return s;
  }
  if (auto s = InitLinker(wasmtime_.get(), impl.get()); !s.ok()) return s;
  if (auto s = InstantiateRuntime(wasmtime_.get(), impl.get()); !s.ok()) {
    return s;
  }
  // M13 Slice C.1: bind engine-registered custom modules + host
  // callbacks onto the linker before instantiating the expression.
  // Order matters — the expr module's `(import "rules" "allow_...")`
  // / `(import "cel_fn" "upper_...")` imports get resolved against
  // whatever's on the linker at instantiate time.
  if (auto s = InstantiateAndBindCustomModules(wasmtime_.get(), impl.get());
      !s.ok()) {
    return s;
  }
  if (auto s = RegisterHostCallbacks(wasmtime_.get(), impl.get()); !s.ok()) {
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
  // NotFound is tolerated: minimal / synthetic WAT fixtures don't
  // carry the section, and a variable-free Eval() still works —
  // the decoded abi just stays empty.
  auto abi_or = celwasm::DecodeCelAbiFromWasm(program.wasm_bytes());
  if (abi_or.ok()) {
    if (auto s = celwasm::abi::CheckRuntimeAbiVersion(*abi_or); !s.ok()) {
      return s;
    }
    impl->abi = *std::move(abi_or);
  } else if (abi_or.status().code() != absl::StatusCode::kNotFound) {
    return abi_or.status();
  }

  // Populate host_env.bindings (field_refs + attributes) from the
  // decoded ABI so the cel_host trampolines can resolve
  // field_ref_id → (field_number, field_name) and attribute_id →
  // (root, qualifiers).  unknown_patterns stays empty for Eval();
  // PartialEval rebinds it per-call.
  //
  // Pass the generated descriptor pool so `BuildCelHostBindings`
  // can resolve `cel.abi.types[]` FQNs to `Descriptor*` for
  // `cel_make_message` lookups.  Statically-linked cc_proto_library
  // descriptors are reachable through `generated_pool()`; dynamic
  // schemas (SchemaProtoSource) are a follow-up.
  celwasm::BuildCelHostBindings(
      impl->abi, google::protobuf::DescriptorPool::generated_pool(),
      impl->host_env);

  return Instance(wasmtime_, std::move(impl));
}

// ——— Engine::AddModule + Engine::AddFunction (M13 Slice C.1) ———

absl::Status Engine::AddModule(absl::string_view alias,
                               absl::Span<const uint8_t> wasm_bytes) {
  if (alias.empty()) {
    return absl::InvalidArgumentError(
        "Engine::AddModule: alias must be non-empty");
  }
  if (IsReservedAlias(alias)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Engine::AddModule: `", alias, "` is a reserved alias"));
  }
  const std::string alias_str(alias);
  if (wasmtime_->custom_modules.find(alias_str) !=
      wasmtime_->custom_modules.end()) {
    return absl::AlreadyExistsError(
        absl::StrCat("module alias `", alias, "` already registered"));
  }

  // Parse the wasm bytes at registration time — surfaces syntactic
  // errors here, not at Plan time.  The parsed wasmtime_module_t*
  // is reusable across Plan calls (each Plan instantiates it into
  // its fresh store).
  celwasm::RegisteredCustomModule entry;
  wasmtime_error_t* err = wasmtime_module_new(
      wasmtime_->engine, wasm_bytes.data(), wasm_bytes.size(), &entry.module);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(absl::StrCat("Engine::AddModule(", alias, ")"),
                                 err);
  }

  // Snapshot the module's function exports — for downstream
  // cross-module conflict detection + import resolution at Plan
  // time.  Needs an instance, so spin up a throwaway store +
  // empty linker and instantiate; modules with imports beyond
  // `cel.memory` would fail here (Slice C.1 requires foreign
  // modules to be self-contained or to only import cel.memory,
  // which we don't have yet at this point).
  //
  // For C.1, accept "may fail to introspect imports" and snapshot
  // exports lazily — i.e., walk the module's export TYPES (no
  // instance needed) via `wasmtime_module_exports`.  That gives us
  // the export names without instantiating.
  wasm_exporttype_vec_t exports;
  wasmtime_module_exports(entry.module, &exports);
  for (size_t i = 0; i < exports.size; ++i) {
    const wasm_name_t* name = wasm_exporttype_name(exports.data[i]);
    const wasm_externtype_t* xt = wasm_exporttype_type(exports.data[i]);
    if (wasm_externtype_kind(xt) != WASM_EXTERN_FUNC) continue;
    const absl::string_view nm(name->data, name->size);
    if (nm.empty() || nm[0] == '_') continue;
    entry.helper_exports.emplace_back(nm);
  }
  wasm_exporttype_vec_delete(&exports);

  wasmtime_->custom_modules.emplace(alias_str, std::move(entry));
  return absl::OkStatus();
}

absl::Status Engine::AddFunction(absl::string_view overload_id,
                                 std::uint8_t num_args, HostCallback impl) {
  if (overload_id.empty()) {
    return absl::InvalidArgumentError(
        "Engine::AddFunction: overload_id must be non-empty");
  }
  if (num_args < 1) {
    return absl::InvalidArgumentError(
        "Engine::AddFunction: num_args must be ≥ 1 (out_slot is required)");
  }
  if (!static_cast<bool>(impl)) {
    return absl::InvalidArgumentError("Engine::AddFunction: impl is empty");
  }
  const std::string id_str(overload_id);
  if (wasmtime_->host_callbacks.find(id_str) !=
      wasmtime_->host_callbacks.end()) {
    return absl::AlreadyExistsError(absl::StrCat(
        "host overload-id `", overload_id, "` already registered"));
  }
  celwasm::RegisteredHostCallback entry{num_args, std::move(impl)};
  wasmtime_->host_callbacks.emplace(id_str, std::move(entry));
  return absl::OkStatus();
}

// ——— Engine::Builder ———

absl::StatusOr<Engine> Engine::Builder::Build() && {
  auto state_or = InitWasmtime();
  if (!state_or.ok()) return state_or.status();
  return Engine(std::move(*state_or));
}

}  // namespace celwasm::api
