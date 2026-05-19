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

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/attribute.h"
#include "compiler_v2/api/value.h"

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

  // Run `$eval()` once.
  //
  // The zero-arg overload is for literal-only / variable-free
  // programs (the M1 case) — no host-side binding to marshal.
  //
  // The Activation overload is M2.B: for every variable the ABI
  // declares (cel.abi.variables[]), look up in the activation, encode
  // the bound Value into the 24-byte CelValue wire form, and write
  // it into the variable's workspace slot before calling $eval.  A
  // declared variable missing from the activation surfaces as
  // FailedPrecondition.  An activation-bound Value whose kind
  // doesn't match the declared variable's Repr is InvalidArgument.
  //
  // Returns:
  //   - the decoded `Value` for any scalar `CelKind`
  //     (NULL/BOOL/INT/UINT/DOUBLE/STRING/BYTES) the runtime
  //     produces;
  //   - InvalidArgument if `$eval` returned a kind that has a
  //     wire shape but no Value mapping yet (LIST/MAP/MESSAGE,
  //     etc. land in their respective milestones);
  //   - FailedPrecondition / Internal on wasmtime trap, out-of-
  //     range offset, or missing activation binding.
  //
  // `$eval`'s first instruction (after the variable prelude) is a
  // baked-in `arena_reset(...)` call, so the arena is reset before
  // the body runs every time — calling Eval back-to-back on the
  // same Instance is safe and deterministic.
  ABSL_MUST_USE_RESULT absl::StatusOr<Value> Eval();
  ABSL_MUST_USE_RESULT absl::StatusOr<Value> Eval(const Activation& activation);

  // Partial evaluation — M2.E.  Same activation-marshalling as
  // Eval, plus an unknown-pattern set the host consults at field
  // read time: a select whose attribute_id matches any pattern
  // short-circuits to `Value::Unknown(attribute_id)` instead of
  // descending the proto.  Stub until M2.E ships; calling it today
  // returns `Unimplemented` so the symbol exists for headers
  // referencing it (the failing m2_test.cc e2e suite).
  ABSL_MUST_USE_RESULT absl::StatusOr<Value> PartialEval(
      const Activation& activation,
      absl::Span<const AttributePattern> unknowns);

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
