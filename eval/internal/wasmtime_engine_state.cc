#include "compiler_v2/api/internal/wasmtime_engine_state.h"

namespace celwasm {

WasmtimeEngineState::~WasmtimeEngineState() {
  // Destruction order: modules before engine (wasmtime owns modules
  // through their engine in the C API).
  for (auto& [alias, mod] : custom_modules) {
    if (mod.module != nullptr) wasmtime_module_delete(mod.module);
  }
  custom_modules.clear();
  if (runtime_module != nullptr) {
    wasmtime_module_delete(runtime_module);
  }
  if (engine != nullptr) {
    wasm_engine_delete(engine);
  }
}

}  // namespace celwasm
