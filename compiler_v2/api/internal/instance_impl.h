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

#include "compiler_v2/abi/cel_abi.pb.h"
#include "compiler_v2/api/internal/cel_host_wasmtime.h"
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

  // Slice 0 — host-managed string/bytes activation arena.  Lives in
  // linear memory above `arena_limit` (the wasm-side arena ceiling
  // codegen baked into `arena_reset`'s second arg), grown via
  // `wasmtime_memory_grow` on demand.  `host_string_arena_floor` is
  // captured on the first Eval as the byte size of the host-owned
  // memory at instantiation time — that's the value codegen used
  // for `arena_limit`, and the floor above which our region starts.
  // `host_string_arena_capacity` tracks how many bytes past the
  // floor we've grown to so far; `[floor, floor + capacity)` is the
  // region the marshaller writes activation strings into.  Reset to
  // 0 between Evals — each Eval rewinds and reuses the region.
  // (Strings only need to survive a single Eval call; the next
  // Eval's marshal overwrites.)
  uint32_t host_string_arena_floor = 0;
  uint32_t host_string_arena_capacity = 0;

  InstanceImpl() = default;
  ~InstanceImpl();
  InstanceImpl(const InstanceImpl&) = delete;
  InstanceImpl& operator=(const InstanceImpl&) = delete;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_API_INTERNAL_INSTANCE_IMPL_H_
