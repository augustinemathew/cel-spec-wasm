// `cel::Instance` — the live evaluator.  Per cel-host-surface.md
// §2.3: holds the wasmtime store + instances + per-instance state
// (host-owned memory, linker, runtime instance, expr instance,
// eval_fn).  Thread-owned (single-threaded; bind one per worker).
//
// Constructed by `cel::Engine::Plan(program, bindings)`; never by
// the user directly.  An Instance keeps a `shared_ptr` to the
// originating Engine's wasmtime state, so Engine can be dropped
// while live Instances continue running.
//
// `Eval(activation) → Value` lands in the next commit (Plan §5.5);
// this commit ships the lifecycle + the wasmtime handle ownership
// the smoke test verified end-to-end.

#ifndef CELWASM_COMPILER_V2_API_INSTANCE_H_
#define CELWASM_COMPILER_V2_API_INSTANCE_H_

#include <cstddef>
#include <memory>

namespace celwasm {
struct WasmtimeEngineState;
struct InstanceImpl;
}  // namespace celwasm

namespace cel {

class Engine;

class Instance {
 public:
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  Instance(Instance&&) noexcept;
  Instance& operator=(Instance&&) noexcept;
  ~Instance();

  // Eval / Reset land in the next commit.

  // Linear-memory byte size for this Instance's host-owned memory.
  // Reads through the wasmtime store, so will crash / UB if the
  // store has been freed — that's the point.  Used by lifetime
  // tests to prove an Instance is still functional after the
  // originating Engine handle has been dropped (see
  // InstanceOutlivesEngineThatBuiltIt in engine_test.cc).
  //
  // Will likely be removed or made non-public once a richer surface
  // (Eval, ReadBytes) lands; for M1 it's the only externally
  // observable signal that the Instance's wasmtime resources are
  // intact.
  std::size_t memory_size_bytes() const;

 private:
  friend class Engine;

  // Constructed by Engine::Plan.  Takes ownership of `impl` and
  // shares the engine state via shared_ptr (so the wasm engine +
  // parsed runtime module stay alive even after the originating
  // Engine handle is dropped).
  Instance(std::shared_ptr<celwasm::WasmtimeEngineState> wasmtime,
           std::unique_ptr<celwasm::InstanceImpl> impl);

  std::shared_ptr<celwasm::WasmtimeEngineState> wasmtime_;
  std::unique_ptr<celwasm::InstanceImpl> impl_;
};

}  // namespace cel

#endif  // CELWASM_COMPILER_V2_API_INSTANCE_H_
