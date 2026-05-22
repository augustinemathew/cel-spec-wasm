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

#include <cstdint>
#include <memory>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/api/host_callback.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"

namespace celwasm {
struct WasmtimeEngineState;
}  // namespace celwasm

namespace cel {

// Raw low-level callback type for `Engine::AddFunction` — the impl
// for a single `@host.<name>` declaration in a `.celfn` library.
//
// The engine invokes the callback when a Planned program's wasm
// imports `cel_fn.<overload_id>` and that import is reached during
// `Instance::Eval`.  Contract:
//
//   - `memory` points at the program's shared `cel.memory` linear
//     memory.  `mem_size` is the total memory size in bytes.
//   - `out_slot` is the byte offset of the 24-byte CelValue the
//     callback must write the result to.
//   - `arg_slots` are the byte offsets of the input CelValues (one
//     per `.celfn`-declared parameter, including the `this`-receiver
//     if any).  Empty for a zero-arg callback.
//   - Return OK on success.  Returning a non-OK status traps the
//     wasm execution with the error message.
//
// CelValue layout is the canonical 24-byte shape from
// `compiler_v2/runtime/cel_data.h`.  Slice C.1 ships this raw
// shape; Slice C.2 wires the typed `cel::FunctionImpl` (from
// `api/activation.h`, signature `Value(Span<const Value>) const`)
// on top as the user-facing layer, with a coercion shim that
// decodes raw CelValues into typed `Value`s and back.
using HostCallback = std::function<absl::Status(
    uint8_t* memory, size_t mem_size, uint32_t out_slot,
    absl::Span<const uint32_t> arg_slots)>;

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
  // those imports; binds the runtime's arena_reset / arena_alloc exports
  // back onto the linker as cel.arena_reset / cel.arena_alloc; parses
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

  // Register a foreign wasm module under `alias`.  The module's
  // exports become available to Planned programs as wasm imports of
  // the form `(import "<alias>" "<helper>" …)`.  Slice E will broaden
  // this to support modules that define their own memory (current
  // Probe 2/3 shape); the v1 constraint is that the foreign module
  // imports `cel.memory` from the engine.
  //
  // Conflict checks at registration time:
  //   - `alias` already registered → AlreadyExists
  //   - `alias` matches a reserved name (`cel`, `cel_host`, `cel_env`,
  //     `cel_fn`, `host`) → InvalidArgument
  //   - The module's wasm bytes fail to parse → InvalidArgument
  //
  // **NOT thread-safe** with concurrent calls to itself or to
  // `AddFunction`.  Engine setup is single-threaded; only `Plan`
  // promises concurrent-safe operation.  Configure once at startup,
  // then `Plan` from many threads.
  ABSL_MUST_USE_RESULT absl::Status AddModule(
      absl::string_view alias, absl::Span<const uint8_t> wasm_bytes);

  // Register a C++ callback as the impl for a `@host.<name>`
  // declaration.  `overload_id` matches the synthesised id from
  // `FunctionLibrary::Builder::AddHost(...)` (e.g. `upper_string`,
  // `is_admin_message_acme_User`).  `num_args` is the total wasm
  // function arity — `params.size() + 1` (the +1 is the out_slot
  // every callback receives).  Matches `OverloadImpl::num_args` 1:1.
  //
  // Conflict checks:
  //   - `overload_id` already registered → AlreadyExists
  //
  // **NOT thread-safe** — same contract as `AddModule`.
  ABSL_MUST_USE_RESULT absl::Status AddFunction(absl::string_view overload_id,
                                                uint8_t num_args,
                                                HostCallback impl);

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
