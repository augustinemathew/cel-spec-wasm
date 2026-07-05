#include "eval/internal/wasmtime_engine_state.h"

#include <mutex>

#include "wasmtime/component.h"

namespace celwasm {

void WasmtimeEngineState::EpochEnter() {
  if (epoch_deadline_ticks == 0) return;  // deadline disabled — no timer
  std::lock_guard<std::mutex> lk(epoch_mu);
  // On the idle→active edge, wake the timer if it is deep-parked.  A
  // short linger in the timer loop means a busy back-to-back Eval loop
  // finds it un-parked and skips the notify.
  if (++epoch_active == 1 && epoch_parked) {
    epoch_cv.notify_one();
  }
}

void WasmtimeEngineState::EpochLeave() {
  if (epoch_deadline_ticks == 0) return;
  std::lock_guard<std::mutex> lk(epoch_mu);
  --epoch_active;  // the timer notices on its next tick and re-parks
}

WasmtimeEngineState::~WasmtimeEngineState() {
  // Stop the epoch timer FIRST — it calls
  // `wasmtime_engine_increment_epoch(engine)`, so it must be joined
  // before the engine (or anything it touches) is torn down.
  if (epoch_thread.joinable()) {
    {
      std::lock_guard<std::mutex> lk(epoch_mu);
      epoch_stop = true;
    }
    epoch_cv.notify_all();
    epoch_thread.join();
  }
  // Destruction order: modules + components before engine (wasmtime owns
  // them through their engine in the C API).
  for (auto& [alias, mod] : custom_modules) {
    if (mod.module != nullptr) wasmtime_module_delete(mod.module);
  }
  custom_modules.clear();
  for (auto& c : component_libraries) {
    if (c.component != nullptr) wasmtime_component_delete(c.component);
  }
  component_libraries.clear();
  if (runtime_module != nullptr) {
    wasmtime_module_delete(runtime_module);
  }
  if (engine != nullptr) {
    wasm_engine_delete(engine);
  }
}

}  // namespace celwasm
