#include "compiler_v2/api/internal/instance_impl.h"

namespace celwasm {

InstanceImpl::~InstanceImpl() {
  // Order: parsed expr_module first (lives in the engine, but it's
  // only referenced by linker_instantiate's product, not by the
  // store).  Then linker.  Then store (which owns the host-allocated
  // memory + the two instance handles).  The wasm_engine_t is owned
  // externally via shared_ptr<WasmtimeEngineState> on the public
  // Instance and outlives this struct.
  if (expr_module != nullptr) {
    wasmtime_module_delete(expr_module);
  }
  if (linker != nullptr) {
    wasmtime_linker_delete(linker);
  }
  if (store != nullptr) {
    wasmtime_store_delete(store);
  }
}

}  // namespace celwasm
