// Internal — owns the wasmtime state held by `cel::Engine`: the
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

#ifndef CELWASM_COMPILER_V2_API_INTERNAL_WASMTIME_ENGINE_STATE_H_
#define CELWASM_COMPILER_V2_API_INTERNAL_WASMTIME_ENGINE_STATE_H_

#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {

struct WasmtimeEngineState {
  wasm_engine_t* engine = nullptr;
  wasmtime_module_t* runtime_module = nullptr;

  WasmtimeEngineState() = default;
  ~WasmtimeEngineState();
  WasmtimeEngineState(const WasmtimeEngineState&) = delete;
  WasmtimeEngineState& operator=(const WasmtimeEngineState&) = delete;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_API_INTERNAL_WASMTIME_ENGINE_STATE_H_
