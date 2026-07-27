#include "eval/internal/instance_impl.h"

#include "absl/log/absl_log.h"
#include "absl/status/status.h"

namespace celwasm {

// NOLINTNEXTLINE(bugprone-exception-escape): any bad_alloc inside the
// gcov dump / logging is process-fatal by repo policy (never caught).
InstanceImpl::~InstanceImpl() {
  // Flush wasm-side gcov counters (no-op unless collection is active)
  // while the store — and with it the guest memory the counters live
  // in — is still alive.
  if (gcov_env != nullptr && store != nullptr) {
    const absl::Status s = DumpWasmGcov(wasmtime_store_context(store),
                                        helpers_instance, gcov_env.get());
    if (!s.ok()) {
      ABSL_LOG(WARNING) << s;
    }
  }
  // Order: parsed expr_module first (lives in the engine, but it's
  // only referenced by linker_instantiate's product, not by the
  // store).  Then linker.  Then store (which owns both instance
  // handles).  Phase C: also dispose the shared-memory clone held
  // here; the underlying memory is owned by the store, but
  // `wasmtime_sharedmemory_clone` (used by `CacheRuntimeMemory`)
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
