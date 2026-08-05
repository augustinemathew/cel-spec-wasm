#include "eval/internal/wasmtime_engine_state.h"

namespace celwasm {

WasmtimeEngineState::~WasmtimeEngineState() {
  // Destruction order: modules before engine (wasmtime owns them
  // through their engine in the C API).
  if (runtime_module != nullptr) {
    wasmtime_module_delete(runtime_module);
  }
  if (engine != nullptr) {
    wasm_engine_delete(engine);
  }
}

}  // namespace celwasm
