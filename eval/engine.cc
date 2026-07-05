#include "eval/engine.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "abi/runtime_catalogue.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"
#include "compiler/program.h"
#include "eval/host/cel_log.h"
#include "eval/host_call_context.h"
#include "eval/internal/abi_decode.h"
#include "eval/internal/cel_component.h"
#include "eval/internal/cel_host_wasmtime.h"
#include "eval/internal/instance_impl.h"
#include "eval/internal/module_imports.h"
#include "eval/internal/wasmtime_engine_state.h"
#include "eval/resource_limits.h"
#include "eval/typed_function.h"
#include "google/protobuf/descriptor.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_runtime_wasm_bytes.h"
#include "wasi.h"
#include "wasm.h"
#include "wasmtime.h"
#include "wasmtime/component.h"
#include "wasmtime/component/val.h"

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

// How long the idle-parked timer keeps ticking after the last Eval
// finishes before deep-parking.  Bridges the sub-25ms gaps in a busy
// back-to-back Eval loop so it doesn't churn the park/wake handshake
// (a futex wake per Eval).  Also the worst-case extra latency before
// the deadline starts being enforced for an Eval that begins mid-linger
// — negligible against any realistic (>=25ms) deadline.
constexpr std::chrono::milliseconds kEpochIdleLinger{25};

struct EpochSchedule {
  std::chrono::nanoseconds tick;  // timer tick period
  uint64_t ticks;                 // ticks spanning the deadline
};

// Coarse, deadline-adaptive tick: ~max_eval_time/16, clamped to
// [1ms, 100ms].  A 1s deadline ticks ~16×/s (not 1000×), a 50ms
// deadline still gets ~16 checks, and a very long deadline is capped at
// 100ms granularity.  `ticks` is how many ticks span the deadline (the
// value armed via `wasmtime_context_set_epoch_deadline`).
// Precondition: d > 0.
EpochSchedule ComputeEpochSchedule(absl::Duration d) {
  absl::Duration tick = d / 16;
  if (tick < absl::Milliseconds(1)) tick = absl::Milliseconds(1);
  if (tick > absl::Milliseconds(100)) tick = absl::Milliseconds(100);
  const int64_t tick_ns = absl::ToInt64Nanoseconds(tick);
  const int64_t d_ns = absl::ToInt64Nanoseconds(d);
  uint64_t ticks = static_cast<uint64_t>((d_ns + tick_ns - 1) / tick_ns);
  if (ticks == 0) ticks = 1;
  return {std::chrono::nanoseconds(tick_ns), ticks};
}

// The per-Engine epoch timer body.  Advances wasmtime's epoch clock so
// a store epoch deadline can fire — but only WHILE an evaluation is in
// flight (`epoch_active > 0`).  When idle it deep-parks on `epoch_cv`
// at zero cost until `EpochEnter` wakes it or the destructor stops it.
void RunEpochTimer(celwasm::WasmtimeEngineState* state) {
  const std::chrono::nanoseconds tick = state->epoch_tick_interval;
  std::unique_lock<std::mutex> lk(state->epoch_mu);
  while (!state->epoch_stop) {
    if (state->epoch_active > 0) {
      // Active: tick at the coarse cadence; wake early when the last
      // Eval finishes (active hits 0) or on shutdown.
      state->epoch_cv.wait_for(lk, tick, [state]() {
        return state->epoch_stop || state->epoch_active == 0;
      });
      if (state->epoch_stop) break;
      if (state->epoch_active > 0) {
        wasmtime_engine_increment_epoch(state->engine);
      }
    } else {
      // Idle: linger briefly, then deep-park at zero cost if still idle.
      state->epoch_cv.wait_for(lk, kEpochIdleLinger, [state]() {
        return state->epoch_stop || state->epoch_active > 0;
      });
      if (state->epoch_stop) break;
      if (state->epoch_active == 0) {
        state->epoch_parked = true;
        state->epoch_cv.wait(lk, [state]() {
          return state->epoch_stop || state->epoch_active > 0;
        });
        state->epoch_parked = false;
      }
    }
  }
}

// Start the idle-parked epoch timer.  Runs until `~WasmtimeEngineState`
// sets `epoch_stop` and joins.  Only called when a deadline is enabled.
void StartEpochTimer(celwasm::WasmtimeEngineState* state) {
  state->epoch_thread = std::thread([state]() {
    RunEpochTimer(state);
  });
}

absl::StatusOr<std::shared_ptr<celwasm::WasmtimeEngineState>> InitWasmtime(
    bool jit_perf_map, const celwasm::ResourceLimits& limits) {
  auto state = std::make_shared<celwasm::WasmtimeEngineState>();
  const bool deadline_enabled = limits.max_eval_time > absl::ZeroDuration();
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
  // Wall-clock eval deadline (eval/resource_limits.h): enabling epoch
  // interruption makes wasmtime insert deadline checks at loop
  // back-edges and call entries so a runaway component (or its
  // constructor) can be trapped instead of hanging the host.  The
  // deadline is armed per-store in InitStore / per-Eval in
  // Instance::Eval; the background timer that advances the epoch is
  // started below.  Left off entirely when the deadline is disabled so
  // the trusted-only path pays neither the check nor a timer thread.
  if (deadline_enabled) {
    wasmtime_config_epoch_interruption_set(config, true);
  }
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
  state->limits = limits;
  if (deadline_enabled) {
    const EpochSchedule sched = ComputeEpochSchedule(limits.max_eval_time);
    state->epoch_tick_interval = sched.tick;
    state->epoch_deadline_ticks = sched.ticks;
    StartEpochTimer(state.get());
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
  // Memory cap (eval/resource_limits.h): bound how far any single
  // linear memory in this store — the expression runtime's and each
  // component's — may grow.  A negative value keeps wasmtime's default
  // (unlimited); only `memory_size` is constrained, the other
  // dimensions keep their defaults.
  if (state->limits.max_memory_bytes > 0) {
    wasmtime_store_limiter(impl->store,
                           static_cast<int64_t>(state->limits.max_memory_bytes),
                           /*table_elements=*/-1, /*instances=*/-1,
                           /*tables=*/-1, /*memories=*/-1);
  }
  // Carry the per-Eval deadline into the Instance so `Instance::Eval`
  // can re-arm it without a back-reference to the engine.  Arm it now
  // too: with epoch interruption enabled, Plan-time wasm execution
  // (runtime + expr instantiation, and any untrusted component
  // constructor) is itself checked, so a component that hangs in its
  // ctor is bounded here rather than hanging Plan.
  impl->epoch_deadline_ticks = state->epoch_deadline_ticks;
  if (impl->epoch_deadline_ticks > 0) {
    wasmtime_context_set_epoch_deadline(wasmtime_store_context(impl->store),
                                        impl->epoch_deadline_ticks);
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
// `e2e/cctz_doubles_test.cc`), but a future surface might land a C++
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

// ── m24 Component-backed kForeignComponent decls ─────────────────────
//
// A `kForeignComponent` decl is dispatched as a `cel_fn.<helper>` host
// callback (m24 §2-§3) — the wasm import shape is identical to a
// `kHost` decl, only the callback body differs.  At Plan time we walk
// every component the embedder registered via `Engine::AddComponent`,
// instantiate it into the per-Plan store, and bind each declared fn
// directly on the linker with a trampoline that marshals
//   arg CelValues → wasmtime_component_val_t via cel_component::Lift
//   → wasmtime_component_func_call
//   → result wasmtime_component_val_t → CelValue via cel_component::Lower
//
// `ComponentFnEnv` holds the per-Plan state captured by the
// trampoline.  Pinned via `std::shared_ptr<void>` on
// InstanceImpl::component_fn_envs.

struct ComponentFnEnv {
  // Per-Plan handle into the just-instantiated component instance.
  wasmtime_component_func_t func{};
  // Type witnesses for marshaling.  Copied at Plan time from
  // RegisteredComponent::library — the engine state lives via a
  // shared_ptr on Instance, so the library is reachable, but copying
  // the per-decl types here keeps the trampoline's data-dependence
  // graph independent of the library map's iterator-stability rules.
  std::vector<celwasm::CelfnType> param_types;
  celwasm::CelfnType return_type;
  // Descriptor pool for proto(...) args / returns (m24 §8).
  const google::protobuf::DescriptorPool* pool = nullptr;
  // Borrowed; points at InstanceImpl::host_env.  Provides the
  // per-eval externref table + arena_alloc + shared memory handle
  // the marshalling layer needs (identical role to HostFnEnv's
  // host_env field).
  celwasm::CelHostCallbackEnv* host_env = nullptr;
};

// Lift every CelValue arg into a component val per the decl's param
// types.  On error mid-loop, delete the partially-built vec before
// bailing — wasmtime_component_val_delete is no-op for the
// kSentinelKind=BOOL slots the init-loop leaves behind, but calling
// it on the lifted ones is the only way to release their
// allocations.
absl::Status LiftComponentArgs(
    const celwasm::ComponentFnEnv& env, celwasm::HostCallContext& call_ctx,
    celwasm::CelComponentContext& cc,
    std::vector<wasmtime_component_val_t>& arg_vals) {
  arg_vals.resize(env.param_types.size());
  for (auto& v : arg_vals) {
    v.kind = WASMTIME_COMPONENT_BOOL;
    v.of.boolean = false;
  }
  for (size_t i = 0; i < env.param_types.size(); ++i) {
    auto v = call_ctx.ArgValue(static_cast<int>(i));
    absl::Status s = v.ok() ? celwasm::LiftCelToComponent(env.param_types[i],
                                                          *v, cc, &arg_vals[i])
                            : v.status();
    if (!s.ok()) {
      for (auto& w : arg_vals) {
        wasmtime_component_val_delete(&w);
      }
      return s;
    }
  }
  return absl::OkStatus();
}

// Invoke the component fn and lower its single result (per m24 §3 /
// §6 — a decl declares one CelfnType return) back into the
// out_slot.  Releases `arg_vals` regardless of outcome.
absl::Status CallComponentAndLowerResult(
    celwasm::ComponentFnEnv* env, wasmtime_context_t* ctx,
    celwasm::CelComponentContext& cc,
    std::vector<wasmtime_component_val_t>& arg_vals,
    celwasm::HostCallContext& call_ctx) {
  wasmtime_component_val_t result_val{};
  result_val.kind = WASMTIME_COMPONENT_BOOL;
  result_val.of.boolean = false;
  wasmtime_error_t* err = wasmtime_component_func_call(
      &env->func, ctx, arg_vals.data(), arg_vals.size(), &result_val, 1);
  for (auto& w : arg_vals) {
    wasmtime_component_val_delete(&w);
  }
  if (err != nullptr) {
    return WasmtimeErrorToStatus("component func call", err);
  }
  celwasm::Value result_value;
  auto lower_status = celwasm::LowerComponentToCel(env->return_type, result_val,
                                                   cc, &result_value);
  wasmtime_component_val_delete(&result_val);
  if (!lower_status.ok()) {
    return lower_status;
  }
  return call_ctx.ReturnValue(result_value);
}

wasm_trap_t* ComponentCallbackTrampoline(
    void* env_ptr, wasmtime_caller_t* caller, const wasmtime_val_t* args,
    size_t nargs, wasmtime_val_t* /*results*/, size_t /*nresults*/) {
  auto* env = static_cast<celwasm::ComponentFnEnv*>(env_ptr);
  if (env == nullptr || env->host_env == nullptr) {
    return TrapFromStatus("component callback env was null");
  }
  if (nargs < 1) {
    return TrapFromStatus(
        "component callback needs at least one arg (out_slot)");
  }
  celwasm::CelHostCallbackEnv* he = env->host_env;
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

  // 3VL absorb — identical contract to HostCallbackTrampoline.  An
  // error / unknown arg short-circuits before any marshaling.
  if (AbsorbUnknownOrErrorArg(mem, arg_slots, out_slot)) {
    return nullptr;
  }

  if (arg_slots.size() != env->param_types.size()) {
    return TrapFromStatus(
        absl::StrCat("component callback: arity mismatch (decl says ",
                     env->param_types.size(), " params, got ", arg_slots.size(),
                     " arg slots)"));
  }

  celwasm::HostCallContext call_ctx(mem, he->refs, alloc, out_slot, arg_slots);
  celwasm::CelComponentContext cc;
  cc.pool = env->pool;

  std::vector<wasmtime_component_val_t> arg_vals;
  if (auto s = LiftComponentArgs(*env, call_ctx, cc, arg_vals); !s.ok()) {
    return TrapFromStatus(s.message());
  }
  if (auto s = CallComponentAndLowerResult(env, ctx, cc, arg_vals, call_ctx);
      !s.ok()) {
    return TrapFromStatus(s.message());
  }
  return nullptr;
}

// Host callback for `wasi:random/random@0.2.0 get-random-bytes(len:
// u64) -> list<u8>` — fills the result with `len` zero bytes.  This
// is the m26 #44 mitigation: libc++'s std::string hash-seed init
// reads from this import as part of static initialisation in the
// wasi-sdk wasi-preview2 libc++ build, but the wasmtime v43 C API
// exposes no per-store WasiCtx setter to satisfy the real preview2
// random impl.  Returning zeros is safe (libc++ doesn't depend on
// the seed being unpredictable for correctness; the protection it
// gives is against adversarial hash flooding, which the demo's
// in-process embedding doesn't face) and keeps the hash machinery
// functional so std::string operations work.
//
// The 7-parameter signature is fixed by the wasmtime C API host-fn
// callback typedef (`wasmtime_component_linker_instance_add_func`);
// the params-over-threshold flag has nothing to split.
// NOLINTNEXTLINE(readability-function-size)
wasmtime_error_t* RandomGetBytesStub(
    void* /*data*/, wasmtime_context_t* /*ctx*/,
    const wasmtime_component_func_type_t* /*type*/,
    wasmtime_component_val_t* args, size_t nargs,
    wasmtime_component_val_t* results, size_t nresults) {
  ABSL_CHECK_EQ(nargs, 1u);
  ABSL_CHECK_EQ(nresults, 1u);
  // `len` is component-controlled.  libc++'s hash-seed init asks for a
  // handful of bytes; a real preview2 random impl would too.  Cap the
  // request so a component asking for a huge count cannot drive an
  // unbounded host allocation here (the store memory limiter bounds
  // the component's OWN memory, but this list is allocated host-side).
  // Beyond the cap, return an error so the component's call fails
  // cleanly rather than OOMing the host.
  constexpr uint64_t kMaxRandomBytes = uint64_t{1} << 20;  // 1 MiB
  const uint64_t len = args[0].of.u64;
  if (len > kMaxRandomBytes) {
    return wasmtime_error_new(
        "wasi:random/random get-random-bytes: requested length exceeds the "
        "1 MiB cap");
  }
  results[0].kind = WASMTIME_COMPONENT_LIST;
  wasmtime_component_vallist_new_uninit(&results[0].of.list,
                                        static_cast<size_t>(len));
  // Deterministic but non-zero bytes — libc++'s hash machinery
  // sometimes special-cases all-zero seeds, which prevented Greet
  // (std::to_string + std::string concat) from progressing past
  // hash-seed init when we returned zeros.  A simple LCG over the
  // byte index gives non-zero, non-constant output without pulling
  // a real RNG.
  for (size_t i = 0; i < len; ++i) {
    results[0].of.list.data[i].kind = WASMTIME_COMPONENT_U8;
    results[0].of.list.data[i].of.u8 =
        static_cast<uint8_t>(((i * 0xA5u) + 0x5Au) & 0xFFu);
  }
  return nullptr;
}

// Wire two things on the linker:
//   1. `wasi:random/random@0.2.0::get-random-bytes` → RandomGetBytesStub.
//   2. Everything else the component imports but we don't satisfy →
//      `wasmtime_component_linker_define_unknown_imports_as_traps` so
//      a runaway libc++ call to e.g. `wasi:clocks/wall-clock.now`
//      surfaces with a wasmtime trap naming the missing interface
//      instead of a generic "cannot leave component instance".
absl::Status InstallWasiRandomStubAndTrapStubs(
    wasmtime_component_linker_t* clinker,
    const wasmtime_component_t* component) {
  constexpr absl::string_view kRandomIface = "wasi:random/random@0.2.0";
  constexpr absl::string_view kGetRandomBytes = "get-random-bytes";

  // Allow shadowing so our random impl (defined below) takes
  // precedence over the trap-stub installed first.
  wasmtime_component_linker_allow_shadowing(clinker, true);

  // First: trap-stub everything the component imports.  Each such
  // import becomes a `wasm trap: <name> has not been defined` when
  // actually called.  This includes the wasi:random get-random-bytes
  // we'll shadow next.
  if (auto* err = wasmtime_component_linker_define_unknown_imports_as_traps(
          clinker, component);
      err != nullptr) {
    return WasmtimeErrorToStatus("linker_define_unknown_imports_as_traps", err);
  }

  // Then: replace the wasi:random/random.get-random-bytes trap-stub
  // with our zero-bytes impl.
  wasmtime_component_linker_instance_t* root =
      wasmtime_component_linker_root(clinker);
  if (root == nullptr) {
    return absl::InternalError("wasmtime_component_linker_root returned null");
  }
  wasmtime_component_linker_instance_t* random_iface = nullptr;
  if (auto* err = wasmtime_component_linker_instance_add_instance(
          root, kRandomIface.data(), kRandomIface.size(), &random_iface);
      err != nullptr) {
    wasmtime_component_linker_instance_delete(root);
    return WasmtimeErrorToStatus(
        "linker_instance_add_instance(wasi:random/random@0.2.0)", err);
  }
  if (auto* err = wasmtime_component_linker_instance_add_func(
          random_iface, kGetRandomBytes.data(), kGetRandomBytes.size(),
          &RandomGetBytesStub, /*data=*/nullptr, /*finalizer=*/nullptr);
      err != nullptr) {
    wasmtime_component_linker_instance_delete(random_iface);
    wasmtime_component_linker_instance_delete(root);
    return WasmtimeErrorToStatus("linker_instance_add_func(get-random-bytes)",
                                 err);
  }
  wasmtime_component_linker_instance_delete(random_iface);
  wasmtime_component_linker_instance_delete(root);
  return absl::OkStatus();
}

// Instantiate one registered component into the per-Plan store.
//
// Components produced by the `cel_wasm_component` Starlark macro
// (m26 §6) target `wasm32-wasip2`, so their core wasm pulls
// libc / libc++ that import `wasi:io / cli / clocks / filesystem
// / random` even when the author's user_fns.cc never explicitly
// touches stdio or the filesystem.
//
// The wasmtime v43 C API exposes
// `wasmtime_component_linker_add_wasip2` (sets up the import
// declarations) but has NO matching per-store wasi-preview2
// context setter — only preview1's `wasmtime_context_set_wasi`
// exists at //wasmtime/store.h, and `wasi_config_t` is preview1.
// Without a per-store WasiCtx the preview2 random impl traps
// libc++'s hash-seed init with "cannot leave component instance".
//
// Smallest unlock: define our own `wasi:random/random@0.2.0
//   get-random-bytes` host fn that returns the requested number
// of zero bytes — enough to satisfy libc++'s hash-seed precondition
// without bringing in a real WASI context.  The other imports
// (`wasi:io / cli / clocks / filesystem`) are wired as trap stubs
// via `wasmtime_component_linker_define_unknown_imports_as_traps`;
// if a fn actually reaches them the trap names the missing
// interface, surfacing the m26 #44 gap with a clear message.
//
// Pure-WAT components from `foreign_component_dispatch_test`
// carry no such imports; the wiring is a no-op for them.
absl::Status InstantiateOneComponent(celwasm::WasmtimeEngineState* state,
                                     wasmtime_context_t* ctx,
                                     const celwasm::RegisteredComponent& reg,
                                     wasmtime_component_instance_t* cinst) {
  wasmtime_component_linker_t* clinker =
      wasmtime_component_linker_new(state->engine);
  if (clinker == nullptr) {
    return absl::InternalError("wasmtime_component_linker_new returned null");
  }
  if (auto status = InstallWasiRandomStubAndTrapStubs(clinker, reg.component);
      !status.ok()) {
    wasmtime_component_linker_delete(clinker);
    return status;
  }
  wasmtime_error_t* cerr =
      wasmtime_component_linker_instantiate(clinker, ctx, reg.component, cinst);
  wasmtime_component_linker_delete(clinker);
  if (cerr != nullptr) {
    return WasmtimeErrorToStatus("instantiate(component)", cerr);
  }
  return absl::OkStatus();
}

// The codegen wasm import shape uses `overload_id` in
// snake_case (`add_int_int`).  Component-Model exports are
// RESTRICTED to kebab-case identifiers — the underscore form
// is rejected at component parse time with
// "not a valid extern name".  Convert here so the embedder
// can write WIT in its native kebab form and the engine
// resolves it against the snake-case overload id from
// codegen.  The celfnc generator emits the kebab form for
// the WIT export name; this is the matching consumer-side
// translation.  Proto fqns carry CamelCase last segments
// (e.g. `acme.User`) which become lowercase in WIT (lower-only
// identifier rule).  Mirror SnakeToKebab here.
std::string OverloadIdToKebab(absl::string_view overload_id) {
  std::string export_name(overload_id);
  for (char& c : export_name) {
    if (c == '_') {
      c = '-';
    } else if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return export_name;
}

// Bind one kForeignComponent decl: resolve its (kebab-case) export
// off the instantiated component, build the per-Plan ComponentFnEnv,
// and define the `cel_fn.<overload_id>` trampoline on the linker.
absl::Status BindOneComponentDecl(celwasm::InstanceImpl* impl,
                                  wasmtime_context_t* ctx,
                                  const wasmtime_component_instance_t& cinst,
                                  wasmtime_component_export_index_t* iface_idx,
                                  const celwasm::CelfnDecl& decl) {
  const std::string export_name = OverloadIdToKebab(decl.overload_id);
  wasmtime_component_export_index_t* exp_idx =
      wasmtime_component_instance_get_export_index(
          &cinst, ctx, iface_idx, export_name.data(), export_name.size());
  if (exp_idx == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("component does not export `", export_name,
                     "` (CEL "
                     "overload-id `",
                     decl.overload_id, "` in kebab form)"));
  }
  auto env = std::make_shared<celwasm::ComponentFnEnv>();
  const bool got =
      wasmtime_component_instance_get_func(&cinst, ctx, exp_idx, &env->func);
  wasmtime_component_export_index_delete(exp_idx);
  if (!got) {
    return absl::FailedPreconditionError(absl::StrCat(
        "component export `", decl.overload_id, "` is not a function"));
  }
  env->param_types.reserve(decl.params.size());
  for (const auto& p : decl.params) {
    env->param_types.push_back(p.type);
  }
  env->return_type = decl.return_type;
  env->pool = google::protobuf::DescriptorPool::generated_pool();
  env->host_env = &impl->host_env;
  void* env_ptr = env.get();
  impl->component_fn_envs.push_back(env);

  wasm_functype_t* ftype = MakeI32sToVoidFuncType(decl.num_args);
  wasmtime_error_t* err = wasmtime_linker_define_func(
      impl->linker, "cel_fn", 6, decl.overload_id.data(),
      decl.overload_id.size(), ftype, ComponentCallbackTrampoline,
      /*data=*/env_ptr, /*finalizer=*/nullptr);
  wasm_functype_delete(ftype);
  if (err != nullptr) {
    return WasmtimeErrorToStatus(
        absl::StrCat("linker.define_func(cel_fn.", decl.overload_id, ")"), err);
  }
  return absl::OkStatus();
}

// Bind every kForeignComponent decl in `reg.library` against the
// just-instantiated `cinst`.  When the embedder set
// lib.wit_interface() (the standard path for
// `cel_wasm_component`-built components, whose exports nest under
// `cel:<module>/fns@<ver>`), look up that interface instance once
// and use it as the parent index for every decl.  When unset, all
// decl lookups go against the component's top level (the pure-WAT
// `foreign_component_dispatch_test` path).
absl::Status BindComponentLibraryDecls(
    celwasm::InstanceImpl* impl, wasmtime_context_t* ctx,
    const celwasm::RegisteredComponent& reg,
    const wasmtime_component_instance_t& cinst) {
  wasmtime_component_export_index_t* iface_idx = nullptr;
  if (!reg.library.wit_interface().empty()) {
    const auto& iface = reg.library.wit_interface();
    iface_idx = wasmtime_component_instance_get_export_index(
        &cinst, ctx, /*instance_export_index=*/nullptr, iface.data(),
        iface.size());
    if (iface_idx == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("component does not export interface `", iface, "`"));
    }
  }
  absl::Status status = absl::OkStatus();
  for (const auto& decl : reg.library.decls()) {
    if (decl.backend != celwasm::CelfnDecl::Backend::kForeignComponent) {
      continue;
    }
    status = BindOneComponentDecl(impl, ctx, cinst, iface_idx, decl);
    if (!status.ok()) break;
  }
  if (iface_idx != nullptr) {
    wasmtime_component_export_index_delete(iface_idx);
  }
  return status;
}

absl::Status InstantiateAndBindComponents(celwasm::WasmtimeEngineState* state,
                                          celwasm::InstanceImpl* impl) {
  if (state->component_libraries.empty()) {
    return absl::OkStatus();
  }
  wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
  for (auto& reg : state->component_libraries) {
    wasmtime_component_instance_t cinst{};
    if (auto s = InstantiateOneComponent(state, ctx, reg, &cinst); !s.ok()) {
      return s;
    }
    if (auto s = BindComponentLibraryDecls(impl, ctx, reg, cinst); !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

// Everything the embedder registered on the Engine, bound onto the
// per-Plan linker before the expr module instantiates — the expr
// module's `(import "rules" "allow_...")` / `(import "cel_fn"
// "upper_...")` imports get resolved against whatever's on the
// linker at instantiate time.
//   1. M13 Slice C.1: foreign custom modules + host callbacks.
//   2. m24 §3.5: Component-Model components — each instantiated into
//      the per-Plan store, its declared fns bound as
//      `cel_fn.<overload_id>` host-callback trampolines.  Runs AFTER
//      RegisterHostCallbacks so a duplicate overload-id would
//      already have been caught at AddComponent / AddFunction
//      registration.
absl::Status BindRegisteredExtensions(celwasm::WasmtimeEngineState* state,
                                      celwasm::InstanceImpl* impl) {
  if (auto s = InstantiateAndBindCustomModules(state, impl); !s.ok()) {
    return s;
  }
  if (auto s = RegisterHostCallbacks(state, impl); !s.ok()) {
    return s;
  }
  return InstantiateAndBindComponents(state, impl);
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

// Decl-side backend spelling for diagnostics.
absl::string_view BackendName(celwasm::CelfnDecl::Backend backend) {
  switch (backend) {
    case celwasm::CelfnDecl::Backend::kHost:
      return "@host";
    case celwasm::CelfnDecl::Backend::kCelDefined:
      return "@native";
    case celwasm::CelfnDecl::Backend::kForeignComponent:
      return "@component";
  }
  ABSL_CHECK(false) << "BackendName: unhandled CelfnDecl::Backend = "
                    << static_cast<int>(backend);
  return "unreachable";
}

// Compatibility between a callable's canonical C++ parameter kind and
// the CEL type declared at the same position.  `Value` matches any
// declared type; `absl::string_view` serves both string and bytes;
// both proto spellings serve proto(...).  `null` / `type` /
// `optional<T>` have no canonical C++ spelling, so only `Value` (the
// early return) can receive them.
bool CppParamMatchesDeclType(celwasm::HostParamKind cpp_kind,
                             celwasm::CelfnType::Kind decl_kind) {
  using HK = celwasm::HostParamKind;
  if (cpp_kind == HK::kValue) return true;
  switch (decl_kind) {
    case celwasm::CelfnType::Kind::kBool:
      return cpp_kind == HK::kBool;
    case celwasm::CelfnType::Kind::kInt:
      return cpp_kind == HK::kInt;
    case celwasm::CelfnType::Kind::kUint:
      return cpp_kind == HK::kUint;
    case celwasm::CelfnType::Kind::kDouble:
      return cpp_kind == HK::kDouble;
    case celwasm::CelfnType::Kind::kString:
    case celwasm::CelfnType::Kind::kBytes:
      return cpp_kind == HK::kStringOrBytes;
    case celwasm::CelfnType::Kind::kDuration:
      return cpp_kind == HK::kDuration;
    case celwasm::CelfnType::Kind::kTimestamp:
      return cpp_kind == HK::kTimestamp;
    case celwasm::CelfnType::Kind::kList:
      return cpp_kind == HK::kList;
    case celwasm::CelfnType::Kind::kMap:
      return cpp_kind == HK::kMap;
    case celwasm::CelfnType::Kind::kProto:
      return cpp_kind == HK::kProto || cpp_kind == HK::kProtoMessagePtr;
    case celwasm::CelfnType::Kind::kNull:
    case celwasm::CelfnType::Kind::kType:
    case celwasm::CelfnType::Kind::kOptional:
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
        BackendName(decl.backend),
        ".` backend; only `@host.` declarations can bind a C++ callable"));
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

absl::StatusOr<Instance> Engine::Plan(const Program& program) const {
  auto impl = std::make_unique<celwasm::InstanceImpl>();
  // Decode the cel.abi section off the raw Program bytes first —
  // it needs no wasmtime state, and an early decode lets the
  // link-mode tripwire below fire before any instantiation work.
  auto abi_present = DecodeAbiAndBindHostEnv(impl.get(), program);
  if (!abi_present.ok()) return abi_present.status();
  if (auto s = InitPlanState(wasmtime_.get(), impl.get(), program); !s.ok()) {
    return s;
  }
  // InitPlanState armed the store's epoch deadline; keep the timer
  // ticking across instantiation so an untrusted component that hangs
  // in its own constructor is bounded here rather than hanging Plan.
  EpochActiveScope epoch_scope(wasmtime_.get());
  // Route on the module's actual import shape; when a cel.abi
  // section is present, cross-check its link_mode label against
  // that shape (mislabeled / corrupted artifacts fail here).
  const bool is_static = !ModuleImportsCelNamespace(impl->expr_module);
  if (*abi_present) {
    if (auto s = ValidateLinkModeLabel(impl->abi.link_mode(), is_static);
        !s.ok()) {
      return s;
    }
  }
  if (!is_static) {
    if (auto s = InstantiateRuntime(wasmtime_.get(), impl.get()); !s.ok()) {
      return s;
    }
  }
  if (auto s = BindRegisteredExtensions(wasmtime_.get(), impl.get()); !s.ok()) {
    return s;
  }
  if (auto s = InstantiateExpr(impl.get()); !s.ok()) return s;
  if (is_static) {
    if (auto s = BindStaticModeHelpers(impl.get()); !s.ok()) return s;
  }
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
    if (!CppParamMatchesDeclType(fn.param_kinds[i], decl.params[i].type.kind)) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Engine::BindFunction: `", decl.fn_name, "` parameter ", i,
          " is declared as CEL `", decl.params[i].type.Argkind(),
          "` but the callable's parameter is `",
          HostParamKindName(fn.param_kinds[i]), "`"));
    }
  }
  return AddFunction(decl.overload_id, fn.num_args, std::move(fn.callback));
}

// ——— Engine::AddComponent ———
//
// Registers a Component-Model component as the backend for the
// `kForeignComponent` decls in `lib`.  At registration time this does
// only cheap, eager work:
//
//   1. Reject empty component bytes.
//   2. Conflict-check the decls' overload-ids against every
//      already-registered host callback and prior component.
//   3. Parse the component bytes (surfacing malformed-component
//      errors here rather than at first Plan) and store the parsed
//      `wasmtime_component_t*` alongside the library.
//
// It does NOT instantiate the component or check that the declared
// fns are actually exported — that is deferred to per-Plan
// instantiation (`InstantiateAndBindComponents`), where a missing
// export fails Plan with FailedPrecondition and a signature mismatch
// is caught per value at call time.

absl::Status Engine::AddComponent(absl::Span<const uint8_t> component_bytes,
                                  const FunctionLibrary& lib) {
  if (component_bytes.empty()) {
    return absl::InvalidArgumentError(
        "Engine::AddComponent: component_bytes must be non-empty");
  }
  // Conflict-check the overload-ids in `lib` against every callback
  // already registered (host_callbacks + prior components).  Catching
  // it here turns the failure into a clean AlreadyExists at
  // registration time, before the per-Plan linker_define_func would
  // surface a less-helpful "duplicate import" error.
  for (const auto& decl : lib.decls()) {
    if (decl.backend != CelfnDecl::Backend::kForeignComponent) continue;
    if (wasmtime_->host_callbacks.find(decl.overload_id) !=
        wasmtime_->host_callbacks.end()) {
      return absl::AlreadyExistsError(absl::StrCat(
          "Engine::AddComponent: overload-id `", decl.overload_id,
          "` is already bound by an earlier `AddFunction` registration"));
    }
    for (const auto& prior : wasmtime_->component_libraries) {
      for (const auto& prior_decl : prior.library.decls()) {
        if (prior_decl.backend != CelfnDecl::Backend::kForeignComponent) {
          continue;
        }
        if (prior_decl.overload_id == decl.overload_id) {
          return absl::AlreadyExistsError(absl::StrCat(
              "Engine::AddComponent: overload-id `", decl.overload_id,
              "` is already bound by a previously-registered component"));
        }
      }
    }
  }
  // Parse the component bytes.  Surfaces malformed-component errors
  // here rather than at first Plan.  The parsed
  // `wasmtime_component_t*` is shared across Plans (each Plan
  // instantiates it into its own per-Plan store), mirroring how
  // RegisteredCustomModule's parsed `wasmtime_module_t*` is reused.
  wasmtime_component_t* component = nullptr;
  wasmtime_error_t* err =
      wasmtime_component_new(wasmtime_->engine, component_bytes.data(),
                             component_bytes.size(), &component);
  if (err != nullptr) {
    return WasmtimeErrorToStatus("Engine::AddComponent: parse component", err);
  }
  celwasm::RegisteredComponent entry;
  entry.component = component;
  entry.library = lib;
  wasmtime_->component_libraries.push_back(std::move(entry));
  return absl::OkStatus();
}

// ——— Engine::Builder ———

absl::StatusOr<Engine> Engine::Builder::Build() const&& {
  auto state_or = InitWasmtime(jit_perf_map_, limits_);
  if (!state_or.ok()) return state_or.status();
  return Engine(std::move(*state_or));
}

}  // namespace celwasm
