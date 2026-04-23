#include "compiler_v2/api/internal/wasmtime_engine_state.h"

namespace celwasm {

WasmtimeEngineState::~WasmtimeEngineState() {
  if (runtime_module != nullptr) {
    wasmtime_module_delete(runtime_module);
  }
  if (engine != nullptr) {
    wasm_engine_delete(engine);
  }
}

}  // namespace celwasm
