#include "compiler_v2/api/internal/instance_impl.h"

namespace celwasm {

InstanceImpl::~InstanceImpl() {
  // Order: parsed expr_module first (lives in the engine, but it's
  // only referenced by linker_instantiate's product, not by the
  // store).  Then linker.  Then store (which owns both instance
  // handles).  Phase C: also dispose the shared-memory clone held
  // here; the underlying memory is owned by the store, but
  // `wasmtime_sharedmemory_clone` (used by `BindRuntimeMemory`)
  // bumps a refcount that must be balanced.  The wasm_engine_t is
  // owned externally via shared_ptr<WasmtimeEngineState> on the
  // public Instance and outlives this struct.
  if (expr_module != nullptr) {
    wasmtime_module_delete(expr_module);
  }
  if (memory != nullptr) {
    wasmtime_sharedmemory_delete(memory);
  }
  if (linker != nullptr) {
    wasmtime_linker_delete(linker);
  }
  if (store != nullptr) {
    wasmtime_store_delete(store);
  }
}

}  // namespace celwasm
