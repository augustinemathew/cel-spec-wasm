#include "eval/internal/wasmtime_engine_state.h"

#include "wasmtime/component.h"

namespace celwasm {

WasmtimeEngineState::~WasmtimeEngineState() {
  // Destruction order: modules + plugins before engine (wasmtime owns
  // them through their engine in the C API).
  for (auto& [alias, mod] : custom_modules) {
    if (mod.module != nullptr) wasmtime_module_delete(mod.module);
  }
  custom_modules.clear();
  for (auto& c : plugin_registry) {
    if (c.component != nullptr) wasmtime_component_delete(c.component);
  }
  plugin_registry.clear();
  if (runtime_module != nullptr) {
    wasmtime_module_delete(runtime_module);
  }
  if (engine != nullptr) {
    wasm_engine_delete(engine);
  }
}

}  // namespace celwasm
