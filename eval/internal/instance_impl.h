// Internal — pImpl payload for `celwasm::Instance`.  Defined here so
// `Engine::Plan` (in engine.cc) and `Instance` (instance.cc) both
// see the full layout.  Public Instance.h forward-declares this
// type and holds it via `std::unique_ptr`.
//
// Owns all the per-Plan wasmtime handles.  Destructor order
// matters: drop the parsed expr_module before the linker, the
// linker before the store, the store before the engine state
// (engine state is held externally via a shared_ptr on the public
// Instance — outlives this struct).

#ifndef CELWASM_EVAL_INTERNAL_INSTANCE_IMPL_H_
#define CELWASM_EVAL_INTERNAL_INSTANCE_IMPL_H_

#include <memory>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "eval/host_callback.h"
#include "eval/internal/cel_host_wasmtime.h"
#include "wasmtime.h"

namespace celwasm {

// M13 Slice C.1 — per-Plan host-callback env.  Carries the
// engine-registered `celwasm::HostCallback` plus a borrowed pointer to
// the per-Instance shared memory.  The wasmtime linker callback
// gets this struct's address as its `env` argument; the struct
// outlives the linker via `InstanceImpl::host_fn_envs`.
struct HostFnEnv {
  // Borrowed; points into `WasmtimeEngineState::host_callbacks`.
  // The shared_ptr<WasmtimeEngineState> on the public Instance
  // outlives this struct, so the pointer stays valid.
  const celwasm::HostCallback* callback = nullptr;
  // Borrowed from `InstanceImpl::memory` (shared with the runtime
  // + every foreign module instantiated into this store).
  wasmtime_sharedmemory_t* memory = nullptr;
};

struct InstanceImpl {
  wasmtime_store_t* store = nullptr;
  wasmtime_linker_t* linker = nullptr;
  wasmtime_module_t* expr_module = nullptr;
  // Phase C: the runtime is built for wasm32-wasi-threads and exports
  // its memory as `shared`.  All host-side reads / writes go through
  // the shared-memory API (`wasmtime_sharedmemory_data` /
  // `wasmtime_sharedmemory_data_size`), which returns a stable base
  // pointer and does not take a `wasmtime_context_t*`.  The pointer is
  // owned by this struct and deleted in the destructor.
  wasmtime_sharedmemory_t* memory = nullptr;
  wasmtime_instance_t runtime_instance{};
  wasmtime_instance_t expr_instance{};
  wasmtime_func_t eval_fn{};

  // Parsed `cel.abi` custom section.  Populated by Engine::Plan;
  // consumed by Instance::Eval(Activation) to walk the declared
  // variables when marshalling bound values into linear memory
  // before the $eval call.  Empty for M1-era modules that don't
  // ship a section — a variable-free Eval still works.
  celwasm::abi::CelAbi abi;

  // Layer 3 callback payload.  Lives here for the instance's
  // lifetime so the pointer stashed in the linker callback stays
  // valid across every Eval.  Populated by Engine::Plan.
  CelHostCallbackEnv host_env;

  // M7: activation buffer — host-allocated region inside the
  // runtime's linear memory where kString / kBytes payloads from
  // Activation get written before each Eval.  Allocated via wasm
  // reentry into `host_env.malloc_fn` on first need; replaced (via
  // a fresh malloc) when a later Eval needs more capacity.  Lives
  // OUTSIDE the bump arena — arena_reset (the first instruction of
  // $eval) would otherwise wipe the bytes the marshaller just wrote.
  //
  // `activation_buf_offset` is the offset returned by malloc; the
  // writeable region is `[offset, offset + capacity)`.  Reset to 0
  // (cursor) between Evals — each Eval rewinds and reuses the region.
  uint32_t activation_buf_offset = 0;
  uint32_t activation_buf_capacity = 0;

  // M13 Slice C.1 — per-Plan host-callback envs.  One entry per
  // host fn the engine has registered (via `Engine::AddFunction`).
  // Stable address — heap-allocated unique_ptrs — so the linker
  // callback's stashed env pointer stays valid for the instance's
  // lifetime.
  std::vector<std::unique_ptr<HostFnEnv>> host_fn_envs;

  InstanceImpl() = default;
  ~InstanceImpl();
  InstanceImpl(const InstanceImpl&) = delete;
  InstanceImpl& operator=(const InstanceImpl&) = delete;
};

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_INSTANCE_IMPL_H_
