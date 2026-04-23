#include "compiler_v2/api/instance.h"

#include <cstddef>
#include <memory>
#include <utility>

#include "compiler_v2/api/internal/instance_impl.h"
#include "compiler_v2/api/internal/wasmtime_engine_state.h"
#include "wasmtime.h"

namespace cel {

Instance::Instance(std::shared_ptr<celwasm::WasmtimeEngineState> wasmtime,
                   std::unique_ptr<celwasm::InstanceImpl> impl)
    : wasmtime_(std::move(wasmtime)), impl_(std::move(impl)) {}

Instance::~Instance() = default;
Instance::Instance(Instance&&) noexcept = default;
Instance& Instance::operator=(Instance&&) noexcept = default;

std::size_t Instance::memory_size_bytes() const {
  wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);
  // wasmtime_memory_data_size takes a pointer; pull a local copy
  // out of the InstanceImpl so the const method can take its
  // address without const_cast'ing through the impl.
  wasmtime_memory_t mem = impl_->memory;
  return wasmtime_memory_data_size(ctx, &mem);
}

}  // namespace cel
