// Internal — pImpl payload for `cel::Instance`.  Defined here so
// `Engine::Plan` (in engine.cc) and `Instance` (instance.cc) both
// see the full layout.  Public Instance.h forward-declares this
// type and holds it via `std::unique_ptr`.
//
// Owns all the per-Plan wasmtime handles.  Destructor order
// matters: drop the parsed expr_module before the linker, the
// linker before the store, the store before the engine state
// (engine state is held externally via a shared_ptr on the public
// Instance — outlives this struct).

#ifndef CELWASM_COMPILER_V2_API_INTERNAL_INSTANCE_IMPL_H_
#define CELWASM_COMPILER_V2_API_INTERNAL_INSTANCE_IMPL_H_

#include "wasmtime.h"

namespace celwasm {

struct InstanceImpl {
  wasmtime_store_t* store = nullptr;
  wasmtime_linker_t* linker = nullptr;
  wasmtime_module_t* expr_module = nullptr;
  wasmtime_memory_t memory{};
  wasmtime_instance_t runtime_instance{};
  wasmtime_instance_t expr_instance{};
  wasmtime_func_t eval_fn{};

  InstanceImpl() = default;
  ~InstanceImpl();
  InstanceImpl(const InstanceImpl&) = delete;
  InstanceImpl& operator=(const InstanceImpl&) = delete;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_API_INTERNAL_INSTANCE_IMPL_H_
