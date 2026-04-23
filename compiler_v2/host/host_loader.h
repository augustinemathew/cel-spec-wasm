// Two-phase wasmtime loader that materialises an `EvalInstance` from
// a compiled expr wasm module plus the embedded runtime wasm bytes.
//
// Phases:
//   1. Host-side trampolines for `cel.cel_reset`, `cel.cel_alloc`, and
//      `cel_env.cel_log` are installed on a `wasmtime_linker_t`.
//   2. The expr module is instantiated; it defines and exports
//      `memory`.
//   3. The runtime module is instantiated with `cel.memory` bound to
//      the expr's exported memory.  The two modules are memory-paired
//      from that point on — any `i32.load`/`i32.store` the runtime
//      does touches the expr's linear memory.
//
// After `Create`, callers invoke `CallEval()` to run the module's
// `$eval` export.  The return value is the linear-memory offset of a
// 24-byte `CelValue` holding the result; the caller decodes it via
// `ReadBytes` / downstream Value codec (lands in a later commit).
//
// Scope: M1 — scalar literals end-to-end.  `cel_alloc` is never
// called by a pure-literal `$eval` body, so its trampoline traps; a
// later milestone that needs arena-allocating evals will swap the
// trampolines for runtime-forwarding ones.  See host_loader.cc for
// the trampoline contract.
//
// Not thread-safe.  Each `EvalInstance` owns a `wasmtime_store_t`;
// bind one per thread for concurrency.

#ifndef CELWASM_COMPILER_V2_HOST_HOST_LOADER_H_
#define CELWASM_COMPILER_V2_HOST_HOST_LOADER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"

namespace celwasm {

struct EvalInstanceOptions {
  // Wasm export name of the entry function.  Matches
  // `CompileOptions.eval_export_name`.  Default tracks the compile
  // pipeline's default.
  std::string eval_export_name = "eval";

  // Runtime wasm bytes.  Empty → use the embedded
  // `kCelRuntimeWasmBytes` (the common case).  Non-empty → use the
  // caller's bytes (for tests that want to swap in a mock runtime).
  absl::Span<const uint8_t> runtime_wasm_bytes;
};

// Owns the wasmtime state for one loaded (expr, runtime) pair.
// Non-copyable; movable (so it can live inside a StatusOr and be
// handed back by Create).
class EvalInstance {
 public:
  // Two-phase instantiate.  Returns an EvalInstance ready for
  // CallEval, or FailedPrecondition if either module fails to load /
  // instantiate / expose the expected exports.  `expr_wasm_bytes`
  // must be the output of `celwasm::Compile` (or equivalent).
  ABSL_MUST_USE_RESULT static absl::StatusOr<EvalInstance> Create(
      absl::Span<const uint8_t> expr_wasm_bytes, EvalInstanceOptions opts = {});

  ~EvalInstance();
  EvalInstance(const EvalInstance&) = delete;
  EvalInstance& operator=(const EvalInstance&) = delete;
  EvalInstance(EvalInstance&&) noexcept;
  EvalInstance& operator=(EvalInstance&&) noexcept;

  // Invoke `$eval()` and return the linear-memory offset of the
  // resulting `CelValue`.  Surface on trap or type-shape mismatch.
  ABSL_MUST_USE_RESULT absl::StatusOr<uint32_t> CallEval();

  // Read `len` bytes at `offset` from the expr's linear memory.
  // Bounds-checked; returns OutOfRange if `[offset, offset+len)`
  // exceeds `memory_size_bytes()`.
  ABSL_MUST_USE_RESULT absl::StatusOr<std::vector<uint8_t>> ReadBytes(
      uint32_t offset, uint32_t len) const;

  // Linear-memory byte size (one wasm page = 65536 bytes).  Useful
  // for tests and for clamping payload reads.
  size_t memory_size_bytes() const;

  // Opaque impl type.  Forward-declared here (not defined) so host_loader.cc
  // can expose helper free functions that take `EvalInstance::Impl&` without
  // private-access gymnastics; the full definition lives inside host_loader.cc.
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
  explicit EvalInstance(std::unique_ptr<Impl> impl);
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_HOST_HOST_LOADER_H_
