// `celwasm::api::Program` — the compiled artifact, output of
// `celwasm::api::Compiler::Compile(source)`.
//
// Pure data: wasm bytes + (future) parsed ABI metadata.  No
// wasmtime dependency; safe to copy across process boundaries
// (serialize the bytes, reconstruct on the other end via
// `Program::FromWasm(bytes)`).  An `Engine` is required to Plan a
// Program into an Instance — see compiler_v2/api/engine.h — but
// the Program itself never holds engine state.
//
// This split (Compiler / Program / Engine / Instance) replaces an
// earlier draft that pinned wasm engine state to the Compiler.
// See doc/implementation-plan/rewrite/two-phase-runtime-isolation.md
// §4 (revised) for why role separation matters.
//
// Future fields (later milestones add):
//   - parsed `Abi` (declared variables, required imports,
//     custom_functions[], etc., decoded from the cel.abi custom
//     section of the wasm bytes).

#ifndef CELWASM_COMPILER_V2_API_PROGRAM_H_
#define CELWASM_COMPILER_V2_API_PROGRAM_H_

#include <cstdint>
#include <utility>
#include <vector>

#include "absl/types/span.h"

namespace celwasm::api {

class Program {
 public:
  // Construct a Program directly from wasm bytes.  Used by both
  // `Compiler::Compile` (compiled in-process) and the cross-process
  // load path (bytes shipped from elsewhere, e.g. a cache).  No
  // validation of the bytes; the wasmtime parse happens later in
  // `Engine::Plan`.
  explicit Program(std::vector<uint8_t> wasm_bytes)
      : wasm_bytes_(std::move(wasm_bytes)) {}

  // Pure data type: copyable + movable.  No external resources.
  Program(const Program&) = default;
  Program& operator=(const Program&) = default;
  Program(Program&&) noexcept = default;
  Program& operator=(Program&&) noexcept = default;
  ~Program() = default;

  // Raw wasm bytes for serialization or for handing to Engine::Plan.
  // Lifetime tied to the Program; copy if you need them beyond it.
  absl::Span<const uint8_t> wasm_bytes() const {
    return absl::MakeConstSpan(wasm_bytes_);
  }

 private:
  std::vector<uint8_t> wasm_bytes_;
};

}  // namespace celwasm::api

#endif  // CELWASM_COMPILER_V2_API_PROGRAM_H_
