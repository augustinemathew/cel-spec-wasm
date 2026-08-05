#include "eval/engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "abi/celfn_wire.h"
#include "abi/runtime_catalogue.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"
#include "compiler/program.h"
#include "eval/host/cel_log.h"
#include "eval/host_call_context.h"
#include "eval/internal/abi_decode.h"
#include "eval/internal/cel_host_wasmtime.h"
#include "eval/internal/instance_impl.h"
#include "eval/internal/module_imports.h"
#include "eval/internal/required_fn_check.h"
#include "eval/internal/wasmtime_engine_state.h"
#include "eval/typed_function.h"
#include "google/protobuf/descriptor.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_runtime_wasm_bytes.h"
#include "wasi.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {

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

// One-time signature proof for the exports the per-Eval hot path
// invokes through `wasmtime_func_call_unchecked` (Instance::Eval's
// `$eval`, the activation-buffer `malloc`, WasmtimeArenaAllocator's
// `arena_alloc`).  The unchecked entry skips wasmtime's per-call
// type/arity checking, so the check runs exactly once, here at Plan:
// `fn` must be `[i32 x want_params] -> [i32 x want_results]`.  A
// mismatched export fails Plan with FailedPrecondition instead of
// becoming undefined behaviour on the first Eval.
absl::Status CheckAllI32FuncSignature(wasmtime_context_t* ctx,
                                      const wasmtime_func_t& fn,
                                      size_t want_params, size_t want_results,
                                      absl::string_view name) {
  wasm_functype_t* ty = wasmtime_func_type(ctx, &fn);
  const wasm_valtype_vec_t* params = wasm_functype_params(ty);
  const wasm_valtype_vec_t* results = wasm_functype_results(ty);
  bool ok = params->size == want_params && results->size == want_results;
  for (size_t i = 0; ok && i < params->size; ++i) {
    ok = wasm_valtype_kind(params->data[i]) == WASM_I32;
  }
  for (size_t i = 0; ok && i < results->size; ++i) {
    ok = wasm_valtype_kind(results->data[i]) == WASM_I32;
  }
  wasm_functype_delete(ty);
  if (!ok) {
    return absl::FailedPreconditionError(
        absl::StrCat("`", name, "` export signature must be [i32 x ",
                     want_params, "] -> [i32 x ", want_results,
                     "] (the per-Eval hot path calls it unchecked)"));
  }
  return absl::OkStatus();
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

absl::StatusOr<std::shared_ptr<celwasm::WasmtimeEngineState>> InitWasmtime(
    bool jit_perf_map) {
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
  // Opt-in JIT symbolication for sampling profilers — see
  // `Engine::Builder::EnableJitPerfMap`.
  if (jit_perf_map) {
    wasmtime_config_profiler_set(config, WASMTIME_PROFILING_STRATEGY_PERFMAP);
  }
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
// `runtime/BUILD.bazel:--export=memory`.  After the helpers
// instance exists, `CacheRuntimeMemory` pulls the exported memory
// off it; in dynamic mode `DefineCelLinkerBindings` additionally
// binds it on the linker as `cel.memory` so the expr module's
// `(import "cel" "memory")` resolves to the same backing store.
// See `rewrite/wasi/DESIGN.md` §3-4.
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
// owns + exports its own memory; in dynamic mode the binding lands
// after `InstantiateRuntime` (see `DefineCelLinkerBindings`).
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

// Pull the helpers instance's exported `memory` into `*out`.
// Shared by the handle-caching half (`CacheRuntimeMemory`, both link
// modes) and the linker-population half (`DefineCelLinkerBindings`,
// dynamic mode only).
absl::Status GetRuntimeMemoryExport(wasmtime_context_t* ctx,
                                    celwasm::InstanceImpl* impl,
                                    wasmtime_extern_t* out) {
  if (!wasmtime_instance_export_get(ctx, &impl->helpers_instance, "memory", 6,
                                    out)) {
    return absl::FailedPreconditionError(
        "runtime instance has no export `memory`");
  }
  // Phase C: the runtime is built for wasm32-wasi-threads and exports
  // its memory as shared.  The expr module imports `cel.memory` with
  // matching shared shape (codegen sets `shared=true` on the import,
  // see `WasmModule::AddMemoryImport`).
  if (out->kind != WASMTIME_EXTERN_SHAREDMEMORY) {
    return absl::FailedPreconditionError(
        absl::StrCat("`memory` is not a shared memory (kind=", out->kind, ")"));
  }
  return absl::OkStatus();
}

// Cache the exported memory handle on InstanceImpl so the host's
// activation marshalling + result decode can call
// `wasmtime_sharedmemory_data` against it without re-pulling.  Runs
// in both link modes.
absl::Status CacheRuntimeMemory(wasmtime_context_t* ctx,
                                celwasm::InstanceImpl* impl) {
  wasmtime_extern_t mem_ext;
  if (auto s = GetRuntimeMemoryExport(ctx, impl, &mem_ext); !s.ok()) return s;
  // The handle pulled out of `wasmtime_instance_export_get` is a
  // shared-memory pointer owned by the store; we clone it so this
  // InstanceImpl owns its own refcounted handle (deleted in the dtor).
  impl->memory = wasmtime_sharedmemory_clone(mem_ext.of.sharedmemory);

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
// removed in favour of `abi/runtime_catalogue` — the
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
    if (auto s = BindRuntimeExport(impl->linker, ctx, impl->helpers_instance,
                                   h.name());
        !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

// Linker-population half — dynamic mode only.  Defines `cel.memory`
// plus every cel.* runtime helper export on the linker so the expr
// module's `(import "cel" ...)` set resolves at `InstantiateExpr`.
// Must run BEFORE the expr module instantiates.  kStatic Programs
// import nothing from `cel`, so this never runs for them.
absl::Status DefineCelLinkerBindings(celwasm::InstanceImpl* impl,
                                     wasmtime_context_t* ctx) {
  wasmtime_extern_t mem_ext;
  if (auto s = GetRuntimeMemoryExport(ctx, impl, &mem_ext); !s.ok()) return s;
  wasmtime_error_t* err = wasmtime_linker_define(impl->linker, ctx, "cel", 3,
                                                 "memory", 6, &mem_ext);
  if (err != nullptr) {
    return WasmtimeErrorToStatus("linker.define(cel.memory)", err);
  }
  return BindAllRuntimeExports(impl, ctx);
}

// Populate the layer-3 callback env's `arena_alloc` + `malloc`
// func handles + memory handle.  cel_host trampolines call into
// these for span payload allocation + activation marshalling.
absl::Status BindRuntimeFuncHandles(celwasm::InstanceImpl* impl,
                                    wasmtime_context_t* ctx) {
  impl->host_env.memory = impl->memory;
  // Seed the per-Eval base/size cache (CelHostCallbackEnv::mem_base
  // docs).  The base is stable for the life of the shared memory —
  // across memory.grow — so fetching it once here is safe; the size
  // snapshot is re-seeded at the top of every Eval and refreshed on
  // bounds miss by WasmtimeMemoryView.
  impl->host_env.mem_base = wasmtime_sharedmemory_data(impl->memory);
  impl->host_env.mem_size =
      static_cast<uint32_t>(wasmtime_sharedmemory_data_size(impl->memory));
  wasmtime_extern_t alloc_ext;
  if (!wasmtime_instance_export_get(ctx, &impl->helpers_instance, "arena_alloc",
                                    11, &alloc_ext)) {
    return absl::FailedPreconditionError(
        "runtime instance has no export `arena_alloc` (cel_host needs it)");
  }
  if (alloc_ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError("`arena_alloc` is not a function");
  }
  // `arena_alloc(i32 size) -> i32 offset`; WasmtimeArenaAllocator::
  // Alloc calls it unchecked, so prove the signature once here.
  if (auto s = CheckAllI32FuncSignature(ctx, alloc_ext.of.func,
                                        /*want_params=*/1, /*want_results=*/1,
                                        "arena_alloc");
      !s.ok()) {
    return s;
  }
  impl->host_env.arena_alloc_fn = alloc_ext.of.func;

  // M7: handle for the runtime's `malloc` (wasi-libc dlmalloc).
  // Used to allocate the activation buffer — payloads that must
  // survive arena_reset and therefore can't live in the bump arena.
  wasmtime_extern_t malloc_ext;
  if (!wasmtime_instance_export_get(ctx, &impl->helpers_instance, "malloc", 6,
                                    &malloc_ext)) {
    return absl::FailedPreconditionError(
        "runtime instance has no export `malloc`");
  }
  if (malloc_ext.kind != WASMTIME_EXTERN_FUNC) {
    return absl::FailedPreconditionError("`malloc` is not a function");
  }
  // `malloc(i32 size) -> i32 offset`; EnsureActivationBuffer
  // (instance.cc) calls it unchecked, so prove the signature once
  // here.
  if (auto s = CheckAllI32FuncSignature(ctx, malloc_ext.of.func,
                                        /*want_params=*/1, /*want_results=*/1,
                                        "malloc");
      !s.ok()) {
    return s;
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
  if (!wasmtime_instance_export_get(ctx, &impl->helpers_instance, "arena_init",
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
  if (!wasmtime_instance_export_get(ctx, &impl->helpers_instance, "__heap_base",
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

// m28 — the mode-independent post-instantiate bindings: everything
// that reads from `impl->helpers_instance` to populate host-side
// handles + state (memory handle cache, memory invariants, host_env
// func handles, arena seed).  Called once after the helpers_instance
// is populated, in either link mode:
//   - kDynamic: helpers_instance was just set by
//     `InstantiateRuntime`'s `wasmtime_linker_instantiate(runtime)`.
//   - kStatic:  helpers_instance was just aliased from `expr_instance`
//     after `InstantiateExpr(static Program)`.
// Linker population for the expr module's `cel.*` imports is NOT
// here — that half is dynamic-only (`DefineCelLinkerBindings`); a
// kStatic Program imports nothing from `cel`, so it gets no linker
// defines at all.
absl::Status BindHelpersInstance(celwasm::InstanceImpl* impl,
                                 wasmtime_context_t* ctx) {
  if (auto s = CacheRuntimeMemory(ctx, impl); !s.ok()) return s;
  EnforceRuntimeMemoryInvariants(impl, ctx);
  if (auto s = BindRuntimeFuncHandles(impl, ctx); !s.ok()) return s;
  return SeedRuntimeArena(impl, ctx);
}

// m28 — static-mode tail of `Engine::Plan`: the freshly-instantiated
// Program IS the helpers source.  Alias `helpers_instance` onto it,
// run the wasi-libc ctor chain once, then run the shared
// post-instantiate bindings (memory clone, host_env handle
// population, arena_init seed).
//
// The `__wasm_call_ctors` invocation is defense-in-depth: in kDynamic
// mode the wasi-libc command-mode wrappers ran ctors per cross-module
// call; the strip tool removes those wrappers but explicitly exports
// `__wasm_call_ctors` so it survives DCE.  Today the tested
// expression surface is empirically zero-init-safe (see
// `e2e/static_init_test.cc`), but a future surface might land a C++
// static that requires constructor init.  One-time prophylactic; if
// the export is absent (older stripped bytes) we skip silently for
// back-compat.
absl::Status BindStaticModeHelpers(celwasm::InstanceImpl* impl) {
  impl->helpers_instance = impl->expr_instance;
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  wasmtime_extern_t ctors_ext;
  if (wasmtime_instance_export_get(ctx, &impl->expr_instance,
                                   "__wasm_call_ctors", 17, &ctors_ext) &&
      ctors_ext.kind == WASMTIME_EXTERN_FUNC) {
    wasm_trap_t* trap = nullptr;
    auto err = wasmtime_func_call(ctx, &ctors_ext.of.func, /*args=*/nullptr,
                                  /*nargs=*/0, /*results=*/nullptr,
                                  /*nresults=*/0, &trap);
    if (err != nullptr) {
      return WasmtimeErrorToStatus("__wasm_call_ctors", err);
    }
    if (trap != nullptr) {
      return WasmTrapToStatus("__wasm_call_ctors trapped", trap);
    }
  }
  return BindHelpersInstance(impl, ctx);
}

// Reject a Program whose cel.abi declares a variable slot extending
// past the reserved low-memory window.  The compiler never emits such
// a slot (Compile validates the whole rodata+workspace region against
// `CELWASM_RESERVED_LOW_MEMORY_BYTES` before serializing); a Program
// claiming one is corrupt, stale, or hand-crafted, and honoring it
// would have the host marshal (`Instance::Eval(Activation)`) write
// CelValue bytes over the runtime's static data / heap in the shared
// memory.  Validated at Plan time — the earliest stage that sees the
// decoded ABI — so a bad Program fails loudly once, not per Eval.
absl::Status ValidateAbiSlotExtents(const celwasm::abi::CelAbi& abi) {
  for (const auto& variable : abi.variables()) {
    const uint64_t slot_end =
        static_cast<uint64_t>(variable.slot_offset()) + sizeof(CelValue);
    if (slot_end > CELWASM_RESERVED_LOW_MEMORY_BYTES) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Plan: cel.abi variable `", variable.name(), "` slot [",
          variable.slot_offset(), ", ", slot_end, ") extends past the ",
          CELWASM_RESERVED_LOW_MEMORY_BYTES,
          "-byte low-memory window reserved for the expression's static "
          "region — the Program is corrupt or was not produced by this "
          "compiler"));
    }
  }
  return absl::OkStatus();
}

// Decode the `cel.abi` custom section and park it on the Instance,
// then populate host_env.bindings from it.  Runs FIRST in
// `Engine::Plan` — the decode walks the Program's raw bytes only and
// needs no compiled module or instantiation state.  Returns true iff
// the section was present and decoded; `ValidateLinkModeLabel` keys
// its mismatch check on that signal.  NotFound is tolerated:
// minimal / synthetic WAT fixtures don't carry the section, and a
// variable-free Eval() still works — the decoded abi just stays
// empty (and the link-mode label goes unvalidated).
//
// Instance::Eval(Activation) consults `impl->abi` at call time to
// marshal bound values into their workspace slots.
//
// host_env.bindings (field_refs + attributes) let the cel_host
// trampolines resolve field_ref_id → (field_number, field_name) and
// attribute_id → (root, qualifiers).  unknown_patterns stays empty
// for Eval(); PartialEval rebinds it per-call.  The generated
// descriptor pool lets `BuildCelHostBindings` resolve
// `cel.abi.types[]` FQNs to `Descriptor*` for `cel_make_message`
// lookups; statically-linked cc_proto_library descriptors are
// reachable through `generated_pool()`, dynamic schemas
// (SchemaProtoSource) are a follow-up.
absl::StatusOr<bool> DecodeAbiAndBindHostEnv(celwasm::InstanceImpl* impl,
                                             const Program& program) {
  bool abi_present = false;
  auto abi_or = celwasm::DecodeCelAbiFromWasm(program.wasm_bytes());
  if (abi_or.ok()) {
    if (auto s = celwasm::abi::CheckRuntimeAbiVersion(*abi_or); !s.ok()) {
      return s;
    }
    if (auto s = ValidateAbiSlotExtents(*abi_or); !s.ok()) {
      return s;
    }
    impl->abi = *std::move(abi_or);
    abi_present = true;
  } else if (abi_or.status().code() != absl::StatusCode::kNotFound) {
    return abi_or.status();
  }
  celwasm::BuildCelHostBindings(
      impl->abi, google::protobuf::DescriptorPool::generated_pool(),
      impl->host_env);
  return abi_present;
}

// Tripwire for mislabeled / corrupted Programs (cache validators,
// cross-process shipping): when a cel.abi section is present, its
// `link_mode` label must agree with the import-derived routing
// (`is_static` ⇔ no `cel.*` imports).  Routing itself stays driven
// by the import shape — the label is only cross-checked.  Unknown
// future enum values (the wire is an open set) are NOT validated;
// only the two labels this engine knows can contradict the shape.
absl::Status ValidateLinkModeLabel(celwasm::abi::LinkMode label,
                                   bool is_static) {
  if (label == celwasm::abi::LINK_MODE_STATIC && !is_static) {
    return absl::FailedPreconditionError(
        "cel.abi link_mode label says LINK_MODE_STATIC but the module "
        "imports from the `cel` namespace (dynamic-link shape) — the "
        "Program is mislabeled or corrupted");
  }
  if (label == celwasm::abi::LINK_MODE_DYNAMIC && is_static) {
    return absl::FailedPreconditionError(
        "cel.abi link_mode label says LINK_MODE_DYNAMIC but the module has "
        "no `cel` namespace imports (static-link shape) — the Program is "
        "mislabeled or corrupted");
  }
  return absl::OkStatus();
}

// Dynamic mode only: instantiate the standalone cel_runtime.wasm,
// run the shared post-instantiate bindings, then populate the linker
// with the `cel.*` defines the expr module's imports will resolve
// against at `InstantiateExpr`.
absl::Status InstantiateRuntime(celwasm::WasmtimeEngineState* state,
                                celwasm::InstanceImpl* impl) {
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_linker_instantiate(
      impl->linker, ctx, state->runtime_module, &impl->helpers_instance, &trap);
  if (err != nullptr) return WasmtimeErrorToStatus("instantiate(runtime)", err);
  if (trap != nullptr) {
    return WasmTrapToStatus("instantiate(runtime) trapped", trap);
  }
  if (auto s = BindHelpersInstance(impl, ctx); !s.ok()) return s;
  return DefineCelLinkerBindings(impl, ctx);
}

// ── M13 Slice C.1 — custom-fn helpers ────────────────────────────

// Functype for host callbacks: `arity` × i32 params, no results.
// AddFunction's host callbacks use this signature.
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
//
// `wasm_trap_new` expects a NUL-terminated message ("stringz"): the
// wasmtime C API reads the bytes up to the terminator, and the
// underlying Rust shim panics (aborting the process) if the buffer
// is not NUL-terminated.  We copy the message into a local string —
// which guarantees a trailing '\0' at `data()[size()]` — and pass a
// length that includes that terminator.
wasm_trap_t* TrapFromStatus(absl::string_view msg) {
  std::string z(msg);
  wasm_byte_vec_t m;
  // +1 to include the NUL terminator std::string guarantees.
  wasm_byte_vec_new(&m, z.size() + 1, z.c_str());
  wasm_trap_t* t = wasm_trap_new(nullptr, &m);
  wasm_byte_vec_delete(&m);
  return t;
}

// 3VL operand absorption.  Scans the arg slots; if any is CEL_ERROR
// (error wins over unknown) or CEL_UNKNOWN, writes that value verbatim
// (its UnknownSet descriptor — the full attribute-id set — preserved)
// to `out_slot` and returns true — the callback must NOT run, matching
// CEL dispatch semantics where a function is not invoked on error /
// unknown operands.  Known residual vs cel-cpp: with SEVERAL unknown
// args, the first arg's set propagates un-merged (cel-cpp's
// function_step merges unknown args); each arg's own set is intact.
bool AbsorbUnknownOrErrorArg(celwasm::MemoryView& mem,
                             absl::Span<const uint32_t> arg_slots,
                             uint32_t out_slot) {
  bool have_error = false;
  bool have_unknown = false;
  CelValue propagate{};
  for (uint32_t slot : arg_slots) {
    const CelValue cv = mem.ReadCelValue(slot);
    if (cv.kind == CEL_ERROR && !have_error) {
      have_error = true;
      propagate = cv;
    } else if (cv.kind == CEL_UNKNOWN && !have_unknown && !have_error) {
      have_unknown = true;
      propagate = cv;
    }
  }
  if (have_error || have_unknown) {
    mem.WriteCelValue(out_slot, propagate);
    return true;
  }
  return false;
}

// Trampoline: adapts wasmtime's func_callback_t shape to a typed
// `HostCallContext`.  `env_ptr` points at a `HostFnEnv` (allocated
// per-Plan in `RegisterHostCallbacks`) whose `host_env` carries the
// per-Instance shared memory, arena_alloc export, and externref table
// — the same context the built-in cel_host trampolines build (see
// `cel_host_wasmtime.cc::HostFieldTrampoline`).  First arg is
// `out_slot`, remaining args are `arg_slots`.  Unknown / error args are
// absorbed before the callback runs (see AbsorbUnknownOrErrorArg).
wasm_trap_t* HostCallbackTrampoline(void* env_ptr, wasmtime_caller_t* caller,
                                    const wasmtime_val_t* args, size_t nargs,
                                    wasmtime_val_t* /*results*/,
                                    size_t /*nresults*/) {
  auto* env = static_cast<celwasm::HostFnEnv*>(env_ptr);
  if (env == nullptr || env->callback == nullptr ||
      !static_cast<bool>(*env->callback) || env->host_env == nullptr) {
    return TrapFromStatus("host callback env was null");
  }
  celwasm::CelHostCallbackEnv* he = env->host_env;
  if (he->memory == nullptr) {
    return TrapFromStatus("host callback env missing memory pointer");
  }
  if (nargs < 1) {
    return TrapFromStatus("host callback needs at least one arg (out_slot)");
  }

  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  // Hot-path view: base cached at Plan time, size snapshot shared
  // per-Eval through the env (see CelHostCallbackEnv::mem_base).
  celwasm::WasmtimeMemoryView mem(he->memory, he->mem_base, &he->mem_size);
  celwasm::WasmtimeArenaAllocator alloc(ctx, he->arena_alloc_fn, he->memory);

  const auto out_slot = static_cast<uint32_t>(args[0].of.i32);
  std::vector<uint32_t> arg_slots;
  arg_slots.reserve(nargs - 1);
  for (size_t i = 1; i < nargs; ++i) {
    arg_slots.push_back(static_cast<uint32_t>(args[i].of.i32));
  }

  if (AbsorbUnknownOrErrorArg(mem, arg_slots, out_slot)) {
    return nullptr;
  }

  celwasm::HostCallContext call_ctx(mem, he->refs, alloc, out_slot, arg_slots);
  absl::Status s = (*env->callback)(call_ctx);
  if (!s.ok()) {
    return TrapFromStatus(s.message());
  }
  return nullptr;
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
    // Borrowed; `impl->host_env` lives for the instance's lifetime and
    // is fully populated (memory / arena_alloc_fn / refs) before any
    // Eval reaches this trampoline.
    env->host_env = &impl->host_env;
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

// Compile the Program's wasm into a wasmtime_module_t before any
// instantiation, so `Engine::Plan` can introspect its imports to
// decide whether the standalone cel_runtime is needed (dynamic link
// mode) or the Program carries its own (static).
absl::Status CompileExprModule(celwasm::WasmtimeEngineState* state,
                               celwasm::InstanceImpl* impl,
                               absl::Span<const uint8_t> bytes) {
  wasmtime_error_t* err = wasmtime_module_new(state->engine, bytes.data(),
                                              bytes.size(), &impl->expr_module);
  if (err != nullptr) return WasmtimeErrorToStatus("module_new(expr)", err);
  return absl::OkStatus();
}

// Mode-independent front half of `Engine::Plan`: per-Plan store +
// linker, then compile the Program's wasm to a `wasmtime_module_t`
// BEFORE any instantiation so Plan can introspect its imports and
// route on `LinkMode`.  kStatic Programs (bundled runtime, no `cel.*`
// imports) skip the standalone cel_runtime instantiation; kDynamic
// Programs need cel.* bindings on the linker before expr can
// instantiate.
absl::Status InitPlanState(celwasm::WasmtimeEngineState* state,
                           celwasm::InstanceImpl* impl,
                           const Program& program) {
  if (auto s = InitStore(state, impl); !s.ok()) return s;
  if (auto s = InitLinker(state, impl); !s.ok()) return s;
  return CompileExprModule(state, impl, program.wasm_bytes());
}

// Instantiates the pre-compiled `impl->expr_module` and pulls out the
// `eval` export.  Linker bindings (cel.*, cel_host.*, cel_fn.*, WASI)
// must already be installed on `impl->linker` before this runs.
absl::Status InstantiateExpr(celwasm::InstanceImpl* impl) {
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err = wasmtime_linker_instantiate(
      impl->linker, ctx, impl->expr_module, &impl->expr_instance, &trap);
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
  // `$eval() -> i32 result_offset`; Instance::Eval calls it
  // unchecked on every evaluation, so prove the signature once here
  // — Program bytes can come from disk, and a wrong-shaped `eval`
  // export must fail Plan, not become UB at the first Eval.
  if (auto s = CheckAllI32FuncSignature(ctx, ext.of.func, /*want_params=*/0,
                                        /*want_results=*/1, "eval");
      !s.ok()) {
    return s;
  }
  impl->eval_fn = ext.of.func;
  return absl::OkStatus();
}

// ——— Engine::BindFunction helpers ———

// Compatibility between a callable's canonical C++ parameter kind and
// the CEL type declared at the same position.  `Value` matches any
// declared type; `absl::string_view` serves both string and bytes;
// both proto spellings serve proto(...).  `null` / `type` /
// `optional<T>` have no canonical C++ spelling, so only `Value` (the
// early return) can receive them.
bool CppParamMatchesDeclType(celwasm::HostParamKind cpp_kind,
                             celwasm::CelType::Kind decl_kind) {
  using HK = celwasm::HostParamKind;
  if (cpp_kind == HK::kValue) return true;
  switch (decl_kind) {
    case celwasm::CelType::Kind::kBool:
      return cpp_kind == HK::kBool;
    case celwasm::CelType::Kind::kInt:
      return cpp_kind == HK::kInt;
    case celwasm::CelType::Kind::kUint:
      return cpp_kind == HK::kUint;
    case celwasm::CelType::Kind::kDouble:
      return cpp_kind == HK::kDouble;
    case celwasm::CelType::Kind::kString:
    case celwasm::CelType::Kind::kBytes:
      return cpp_kind == HK::kStringOrBytes;
    case celwasm::CelType::Kind::kDuration:
      return cpp_kind == HK::kDuration;
    case celwasm::CelType::Kind::kTimestamp:
      return cpp_kind == HK::kTimestamp;
    case celwasm::CelType::Kind::kList:
      return cpp_kind == HK::kList;
    case celwasm::CelType::Kind::kMap:
      return cpp_kind == HK::kMap;
    case celwasm::CelType::Kind::kMessage:
      return cpp_kind == HK::kProto || cpp_kind == HK::kProtoMessagePtr;
    case celwasm::CelType::Kind::kUnknown:
    case celwasm::CelType::Kind::kNull:
    case celwasm::CelType::Kind::kType:
    case celwasm::CelType::Kind::kOptional:
      return false;
  }
  return false;
}

// Front half of `Engine::BindFunction`'s validation: parse + require
// exactly one declaration with the `@host.` backend.
absl::StatusOr<celwasm::CelfnDecl> ParseSingleHostDecl(
    absl::string_view celfn_decl) {
  auto lib_or = celwasm::ParseCelfnSource(celfn_decl);
  if (!lib_or.ok()) {
    return absl::Status(
        lib_or.status().code(),
        absl::StrCat("Engine::BindFunction: ", lib_or.status().message()));
  }
  const std::vector<celwasm::CelfnDecl>& decls = lib_or->decls();
  if (decls.size() != 1) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Engine::BindFunction: expected exactly one declaration, found ",
        decls.size()));
  }
  const celwasm::CelfnDecl& decl = decls.front();
  if (decl.backend != celwasm::CelfnDecl::Backend::kHost) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Engine::BindFunction: declaration `", decl.fn_name, "` uses the `",
        celwasm::BackendPrefix(decl.backend),
        "` backend; only `@host.` declarations can bind a C++ callable"));
  }
  return decl;
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

namespace {

// The link-mode-dependent half of Plan: route on the module's actual
// import shape, cross-check the cel.abi link_mode label against that
// shape when the section is present (mislabeled / corrupted artifacts
// fail here), then instantiate in the order that mode requires.
absl::Status InstantiateForLinkMode(celwasm::WasmtimeEngineState* state,
                                    celwasm::InstanceImpl* impl,
                                    bool abi_present) {
  const bool is_static = !ModuleImportsCelNamespace(impl->expr_module);
  if (abi_present) {
    if (auto s = ValidateLinkModeLabel(impl->abi.link_mode(), is_static);
        !s.ok()) {
      return s;
    }
  }
  if (!is_static) {
    if (auto s = InstantiateRuntime(state, impl); !s.ok()) return s;
  }
  // Host callbacks bind onto the per-Plan linker before the expr
  // module instantiates — its `(import "cel_fn" "upper_...")` imports
  // resolve against whatever's on the linker at instantiate time.
  if (auto s = RegisterHostCallbacks(state, impl); !s.ok()) return s;
  if (auto s = InstantiateExpr(impl); !s.ok()) return s;
  if (is_static) {
    if (auto s = BindStaticModeHelpers(impl); !s.ok()) return s;
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<Instance> Engine::Plan(const Program& program) const {
  auto impl = std::make_unique<celwasm::InstanceImpl>();
  // Decode the cel.abi section off the raw Program bytes first —
  // it needs no wasmtime state, and an early decode lets the
  // link-mode tripwire below fire before any instantiation work.
  auto abi_present = DecodeAbiAndBindHostEnv(impl.get(), program);
  if (!abi_present.ok()) return abi_present.status();
  // Verify every required custom function against the registered
  // host callbacks before any wasmtime work — a missing or drifted
  // registration fails here with the frozen
  // m35-plugin-ergonomics.md §5.3 diagnostics instead of an
  // opaque link error or a call-time trap.  Reads only
  // registration-frozen state; Plan stays concurrent-safe.
  if (auto s = celwasm::CheckRequiredFunctions(impl->abi,
                                               wasmtime_->host_callbacks);
      !s.ok()) {
    return s;
  }
  if (auto s = InitPlanState(wasmtime_.get(), impl.get(), program); !s.ok()) {
    return s;
  }
  if (auto s =
          InstantiateForLinkMode(wasmtime_.get(), impl.get(), *abi_present);
      !s.ok()) {
    return s;
  }
  return Instance(wasmtime_, std::move(impl));
}

// ——— Engine::AddFunction (host callbacks) ———

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

// ——— Engine::BindFunction (declaration-first registration) ———

absl::Status Engine::BindParsedFunction(absl::string_view celfn_decl,
                                        TypedFunction fn) {
  auto decl_or = ParseSingleHostDecl(celfn_decl);
  if (!decl_or.ok()) return decl_or.status();
  const CelfnDecl& decl = *decl_or;
  if (fn.param_kinds.size() != decl.params.size()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Engine::BindFunction: `", decl.fn_name, "` declares ",
        decl.params.size(), " parameter(s) but the callable takes ",
        fn.param_kinds.size()));
  }
  for (size_t i = 0; i < decl.params.size(); ++i) {
    if (!CppParamMatchesDeclType(fn.param_kinds[i],
                                 decl.params[i].type.kind())) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Engine::BindFunction: `", decl.fn_name, "` parameter ", i,
          " is declared as CEL `", ArgkindSlug(decl.params[i].type),
          "` but the callable's parameter is `",
          HostParamKindName(fn.param_kinds[i]), "`"));
    }
  }
  if (auto s =
          AddFunction(decl.overload_id, fn.num_args, std::move(fn.callback));
      !s.ok()) {
    return s;
  }
  // Capture the parsed decl's full signature on the registry entry —
  // this is what upgrades the Plan-time required-function check from
  // arity-only (raw AddFunction / AddTypedFunction) to the full
  // recursive type compare for BindFunction registrations.
  wasmtime_->host_callbacks.at(decl.overload_id).decl_signature =
      RequiredFunctionFromDecl(decl);
  return absl::OkStatus();
}

// ——— Engine::Builder ———

absl::StatusOr<Engine> Engine::Builder::Build() const&& {
  auto state_or = InitWasmtime(jit_perf_map_);
  if (!state_or.ok()) return state_or.status();
  return Engine(std::move(*state_or));
}

}  // namespace celwasm
