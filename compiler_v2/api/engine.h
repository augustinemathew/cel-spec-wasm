// `cel::Engine` — runtime side of the host surface.  Owns the
// wasm execution machinery shared across all evaluations: the
// `wasm_engine_t` and the parsed `cel_runtime.wasm` module.
//
// Per doc/implementation-plan/rewrite/two-phase-runtime-isolation.md
// §4 (revised): role-separated from `cel::Compiler`.  Compiler is
// pure compile-time and has no wasmtime dependency.  Engine is
// pure runtime and has no compile-time dependency.  A Program
// (bytes + ABI) is the serialization boundary between them.
//
// Lifecycle:
//
//   cel::Compiler  ─Compile(source)─►  cel::Program
//                                            │
//                                            ▼
//                  cel::Engine ─Plan(program, bindings)─►  cel::Instance
//
// The Engine is process-shared (or per-tenant in multi-tenant
// hosts).  `wasm_engine_t` and `wasmtime_module_t` are documented
// thread-safe for concurrent access; many threads safely call
// `Engine::Plan(...)` against the same Engine.
//
// Bench-justified: caching the parsed cel_runtime.wasm on the
// Engine gives a ~34x per-Plan speedup over re-parsing it (186us
// → 5.5us); sharing the engine across the process gives another
// ~64x cold-to-hot (351us → 5.5us).  See §2 of the plan doc for
// raw numbers.
//
// Plan() lands in a later commit; this one only stands up the
// engine + parsed runtime fixture and the Builder.

#ifndef CELWASM_COMPILER_V2_API_ENGINE_H_
#define CELWASM_COMPILER_V2_API_ENGINE_H_

#include <memory>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"

namespace celwasm {
struct WasmtimeEngineState;
}  // namespace celwasm

namespace cel {

class Engine {
 public:
  class Builder;
  static Builder NewBuilder();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) noexcept;
  Engine& operator=(Engine&&) noexcept;
  ~Engine();

  // Build an Instance from a Program, ready for evaluation.  M1
  // re-parses the program's wasm bytes via wasmtime_module_new on
  // every Plan; future commits add an Engine-side cache keyed by
  // program identity if profiles demand it.
  //
  // Wiring (per Plan §5.4): host-allocates a 2-page wasmtime_memory_t
  // in a fresh store; binds it as cel.memory on a fresh linker;
  // installs cel_env.cel_log; instantiates cel_runtime.wasm against
  // those imports; binds the runtime's cel_reset / cel_alloc exports
  // back onto the linker as cel.cel_reset / cel.cel_alloc; parses
  // and instantiates the expr module against the now-complete linker;
  // looks up the eval export.
  //
  // FailedPrecondition on:
  //   - module_new(expr): malformed bytes
  //   - instantiate failures (missing imports, etc.)
  //   - missing `eval` export
  //   - wasmtime trap during instantiation
  //
  // Plan is safe to call concurrently: each call creates a fresh
  // store + linker + memory, sharing only the engine + parsed
  // runtime module via the shared_ptr.  wasmtime engines and modules
  // are documented thread-safe.
  ABSL_MUST_USE_RESULT absl::StatusOr<Instance> Plan(
      const Program& program) const;

 private:
  friend class Builder;

  // Built only by Builder::Build(); the shared_ptr's control
  // block is set up there so destruction here works without
  // WasmtimeEngineState being a complete type.
  explicit Engine(std::shared_ptr<celwasm::WasmtimeEngineState> wasmtime);

  // shared_ptr (vs unique_ptr) so future Instances minted by Plan
  // can hold a back-reference to the engine state and outlive the
  // Engine handle the user originally constructed.  Internal
  // detail; not exposed.
  std::shared_ptr<celwasm::WasmtimeEngineState> wasmtime_;
};

class Engine::Builder {
 public:
  Builder() = default;

  // Special members defaulted while M1 has no per-Builder state.
  // When future commits add tunables (e.g. wasmtime config flags)
  // the .cc will need definitions.
  ~Builder() = default;
  Builder(const Builder&) = delete;
  Builder& operator=(const Builder&) = delete;
  Builder(Builder&&) noexcept = default;
  Builder& operator=(Builder&&) noexcept = default;

  // Allocate the wasm engine + parse `cel_runtime.wasm` into a
  // module.  Returns Internal on wasmtime allocation failure.
  // Single-use: && enforces consumption at the call site.
  ABSL_MUST_USE_RESULT absl::StatusOr<Engine> Build() &&;
};

}  // namespace cel

#endif  // CELWASM_COMPILER_V2_API_ENGINE_H_
