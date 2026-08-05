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

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "abi/cel_abi.pb.h"
#include "eval/host_callback.h"  // for celwasm::HostCallback
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {

// `Engine::AddFunction`-registered host callback + arity.  Held as
// a node in the map so the address of `callback` is stable across
// later insertions (the wasmtime func-callback registration captures
// `&callback` as its `env` pointer).
struct RegisteredHostCallback {
  // Total wasm function arity — params.size() + 1 (out_slot).
  // Matches the underlying `OverloadDef::num_args`.
  std::uint8_t num_args = 0;
  celwasm::HostCallback callback;
  // Full declared signature, captured ONLY when the registration
  // came through `Engine::BindFunction` (which parses a `.celfn`
  // decl).  Plan's required-function verification uses it for the
  // full recursive type compare; raw `AddFunction` /
  // `AddTypedFunction` registrations leave it empty and stay
  // arity-only at that check.
  std::optional<celwasm::abi::RequiredFunction> decl_signature;
};

struct WasmtimeEngineState {
  wasm_engine_t* engine = nullptr;
  wasmtime_module_t* runtime_module = nullptr;

  // Engine-owned custom-fn state: populated by
  // `Engine::AddFunction`; consumed by `Engine::Plan` when
  // resolving caller-side wasm imports.
  std::map<std::string, RegisteredHostCallback> host_callbacks;

  WasmtimeEngineState() = default;
  ~WasmtimeEngineState();
  WasmtimeEngineState(const WasmtimeEngineState&) = delete;
  WasmtimeEngineState& operator=(const WasmtimeEngineState&) = delete;
};

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_WASMTIME_ENGINE_STATE_H_
