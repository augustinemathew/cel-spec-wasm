// Two-module host loader.
//
// The compiler emits two wasm modules per expression deployment: a
// shared `runtime.wasm` (compiled once, embedded in the library via
// cel_runtime_wasm_bytes.h) and a per-expression `eval.wasm` that
// imports `cel.memory`, `cel.cel_alloc`, `cel.cel_make_string_view`,
// etc. from the runtime.  This loader owns a wasmtime engine + store,
// instantiates the runtime once, and wires its exports under the
// module namespace `"cel"` via a `wasmtime_linker_t` before
// instantiating the eval module.
//
// Scope intentionally small — the MVP needs only "call nullary `eval`
// and return the scalar."  Host ABI imports (cel_host.*, cel_fn.*) land
// in later milestones and will plug into the same linker; their
// registration will fit as additional `Define*` calls on the linker
// before `Instantiate`.

#ifndef CELWASM_COMPILER_HOST_HOST_LOADER_H_
#define CELWASM_COMPILER_HOST_HOST_LOADER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "compiler/host/cel_host_wasmtime.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {

// Owns every wasmtime handle involved in running an eval module:
// engine, store, both compiled modules, the linker, and both
// instances.  Moving transfers ownership; destruction releases
// everything in reverse construction order.
//
// Not copyable.  Instances are not thread-safe — wasmtime `store`s are
// single-threaded, and all Call* methods must be invoked from the
// thread that created the LoadedEval.
class LoadedEval {
 public:
  LoadedEval() = default;
  ~LoadedEval();

  LoadedEval(const LoadedEval&) = delete;
  LoadedEval& operator=(const LoadedEval&) = delete;

  LoadedEval(LoadedEval&& other) noexcept;
  LoadedEval& operator=(LoadedEval&& other) noexcept;

  // Invokes the eval module's `eval` export with zero args and one
  // scalar result (the M2 MVP shape).  Internally calls
  // `cel_reset()` on the runtime beforehand so every call starts with
  // an empty arena — per-call isolation matches the design doc's
  // "stateless evaluation" contract.
  ABSL_MUST_USE_RESULT absl::StatusOr<wasmtime_val_t> CallNullaryEval();

  // Invokes the eval module's `eval` export with caller-supplied `args`
  // and expects exactly one scalar result.  `args` must match the eval
  // function's param signature (produced by `LowerToEvalFunction` from
  // `TypedAst::variables()`): one `wasmtime_val_t` per declared
  // variable, in declaration order.  Calls `cel_reset` on the runtime
  // before invoking eval, same as `CallNullaryEval`.
  ABSL_MUST_USE_RESULT absl::StatusOr<wasmtime_val_t> CallEval(
      absl::Span<const wasmtime_val_t> args);

  // Raw handle accessors for tests that need to poke beyond
  // CallNullaryEval — e.g. to inspect the runtime's linear memory
  // directly or to walk a returned CelValue offset.
  wasmtime_context_t* absl_nullable context() const;
  const wasmtime_instance_t& eval_instance() const {
    return eval_instance_;
  }
  const wasmtime_instance_t& runtime_instance() const {
    return runtime_instance_;
  }

 private:
  friend absl::StatusOr<LoadedEval> LoadEval(
      absl::Span<const uint8_t> eval_wasm_bytes);

  void Reset() noexcept;  // free owned handles; leave fields nulled

  wasm_engine_t* engine_ = nullptr;
  wasmtime_store_t* store_ = nullptr;
  wasmtime_module_t* runtime_mod_ = nullptr;
  wasmtime_module_t* eval_mod_ = nullptr;
  wasmtime_linker_t* linker_ = nullptr;
  // Held behind unique_ptr because CelHostEnv is non-movable (its
  // address is captured as void* callback-data by wasmtime's linker);
  // LoadedEval itself is movable, so the env travels as a pointer.
  std::unique_ptr<CelHostEnv> host_env_;
  wasmtime_instance_t runtime_instance_{};
  wasmtime_instance_t eval_instance_{};
  bool has_instances_ = false;
};

// Loads the embedded runtime (kCelRuntimeWasmBytes) and `eval_wasm_bytes`
// into a fresh wasmtime engine+store, wires the runtime's exports under
// the module namespace `"cel"`, and instantiates the eval module via a
// linker.  Returns a `LoadedEval` on success.  Every failure mode
// (runtime decode, eval decode, unresolved import, start-function trap)
// surfaces as a status with a descriptive message; wasmtime's raw error
// text is appended after a `: ` separator.
ABSL_MUST_USE_RESULT absl::StatusOr<LoadedEval> LoadEval(
    absl::Span<const uint8_t> eval_wasm_bytes);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_HOST_HOST_LOADER_H_
