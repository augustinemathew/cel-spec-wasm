// Internal — owns the wasmtime state held by `celwasm::Engine`: the
// `wasm_engine_t` plus the parsed `cel_runtime.wasm` module.
//
// Lives in `celwasm::` per cel-host-surface.md §1: public API is
// `cel::`, internal machinery is `celwasm::`.  Held by
// `std::shared_ptr` so future Instances minted by `Engine::Plan`
// can keep the engine state alive past the user's `Engine`
// handle (e.g. an Instance handed off to a thread, or a Program
// pre-parsed and cached by the Engine).
//
// Two fields, both wasmtime-thread-safe per upstream docs:
//
//   - engine: `wasm_engine_t*` — the JIT.  One per Engine.
//             Created in `Engine::Builder::Build()`.
//   - runtime_module: parsed `cel_runtime.wasm`.  Reused across
//             every `Engine::Plan(...)` to avoid the ~166us
//             per-Plan reparse cost.  See
//             doc/implementation-plan/rewrite/two-phase-runtime-isolation.md
//             §2 for raw bench numbers.
//
// Destruction order: runtime_module before engine (modules are
// owned by their engine in wasmtime's C API).

#ifndef CELWASM_EVAL_INTERNAL_WASMTIME_ENGINE_STATE_H_
#define CELWASM_EVAL_INTERNAL_WASMTIME_ENGINE_STATE_H_

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "compiler/celfn/function_library.h"
#include "eval/host_callback.h"  // for celwasm::HostCallback
#include "eval/resource_limits.h"
#include "wasm.h"
#include "wasmtime.h"

// Forward-declare the component-model handle so this header does
// NOT require -DWASMTIME_FEATURE_COMPONENT_MODEL.  The component
// surface compiles only in TUs that opt in (engine.cc,
// cel_component.cc).  Upstream's typedef in wasmtime/component/component.h
// is `typedef struct wasmtime_component_t wasmtime_component_t;`
// (struct tag and typedef name match — a common wasmtime idiom);
// the forward decl below must mirror that to avoid
// "typedef redefinition with different types" at TUs that include
// both headers.
extern "C" {
typedef struct wasmtime_component_t wasmtime_component_t;
}

namespace celwasm {

// M13 Slice C.1: a foreign wasm module registered via
// `Engine::AddModule(alias, bytes)`.  Parsed at registration time
// (so syntactic errors surface there), instantiated per-Plan into
// the fresh store (so concurrent Plans don't share instances).
//
// `helper_exports` lists the function-export names that look like
// CEL overload ids (skips toolchain noise like `_initialize`,
// `__data_end`, etc.).  Used by the engine's conflict-detection +
// import-resolution paths.
struct RegisteredCustomModule {
  wasmtime_module_t* module = nullptr;
  std::vector<std::string> helper_exports;
};

// `Engine::AddFunction`-registered host callback + arity.  Held as
// a node in the map so the address of `callback` is stable across
// later insertions (the wasmtime func-callback registration captures
// `&callback` as its `env` pointer).
struct RegisteredHostCallback {
  // Total wasm function arity — params.size() + 1 (out_slot).
  // Matches the underlying `OverloadDef::num_args`.
  std::uint8_t num_args = 0;
  celwasm::HostCallback callback;
};

// A Component-Model component registered via `Engine::AddComponent`
// — the parsed `wasmtime_component_t` plus the FunctionLibrary that
// names which exports back which CEL decls.  The component is shared
// across Plans (each Plan instantiates it into its own per-Plan store).
// The library lives by value here so the decl signatures stay
// reachable from the per-Plan instantiation step in engine.cc.
struct RegisteredComponent {
  wasmtime_component_t* component = nullptr;
  celwasm::FunctionLibrary library;
};

struct WasmtimeEngineState {
  wasm_engine_t* engine = nullptr;
  wasmtime_module_t* runtime_module = nullptr;

  // Sandbox bounds applied to every Plan/Eval on this Engine (see
  // eval/resource_limits.h).  Copied from the Builder at Build().
  ResourceLimits limits;

  // Precomputed epoch deadline, in ticks of the background epoch
  // thread (one tick == `kEpochTickInterval`).  Zero means the
  // wall-clock deadline is disabled (`limits.max_eval_time <= 0`), in
  // which case no epoch config, no timer thread, and no per-Eval
  // deadline are installed.  Copied into `InstanceImpl` at Plan time
  // so `Instance::Eval` can re-arm the store deadline without a
  // back-reference to the engine.
  uint64_t epoch_deadline_ticks = 0;

  // Background timer that advances wasmtime's epoch clock so an
  // epoch deadline actually fires.  Runs iff `epoch_deadline_ticks >
  // 0`; started in `InitWasmtime` after `engine` is created and
  // joined FIRST in the destructor (before `engine` is deleted — the
  // thread calls `wasmtime_engine_increment_epoch(engine)`).
  std::thread epoch_thread;
  std::mutex epoch_mu;
  std::condition_variable epoch_cv;
  bool epoch_stop = false;  // guarded by epoch_mu

  // M13 Slice C.1 — engine-owned custom-fn state.  Populated by
  // `Engine::AddModule` and `Engine::AddFunction`; consumed by
  // `Engine::Plan` when resolving caller-side wasm imports.
  std::map<std::string, RegisteredCustomModule> custom_modules;
  std::map<std::string, RegisteredHostCallback> host_callbacks;

  // Component-Model components registered via `Engine::AddComponent`.
  // Order-preserving vector (vs map) — there is no natural keying name
  // for a component the way `alias` keys a `RegisteredCustomModule`;
  // distinct components are distinguished by the overload-ids the
  // library declares.  Conflict detection still rejects duplicate
  // overload-ids (against host_callbacks + other components).
  std::vector<RegisteredComponent> component_libraries;

  WasmtimeEngineState() = default;
  ~WasmtimeEngineState();
  WasmtimeEngineState(const WasmtimeEngineState&) = delete;
  WasmtimeEngineState& operator=(const WasmtimeEngineState&) = delete;
};

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_WASMTIME_ENGINE_STATE_H_
