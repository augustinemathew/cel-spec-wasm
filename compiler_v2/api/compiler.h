// `cel::Compiler` — pure compile-time front end.  Per
// cel-host-surface.md §2.1 (with the role-split correction in
// doc/implementation-plan/rewrite/two-phase-runtime-isolation.md
// §4): holds declarations + the compile-time descriptor pool.
// Immutable after `Builder::Build()`.  No wasmtime dependency.
// One Compiler can produce many Programs.
//
// Programs from this Compiler can be:
//   - serialized (via `Program::wasm_bytes()`) and shipped
//     elsewhere
//   - executed in this process by passing them to
//     `cel::Engine::Plan(program, bindings)`
//
// The Compiler itself never touches a wasm engine.

#ifndef CELWASM_COMPILER_V2_API_COMPILER_H_
#define CELWASM_COMPILER_V2_API_COMPILER_H_

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/program.h"

namespace cel {

// Per-compilation tunables.  Per cel-host-surface.md §2.1:
// declarations live on the Compiler/Builder; opts only tunes how
// a specific expression is lowered.  M1 carries one knob; future
// commits add debug_layout, allowed_overloads, etc.
struct CompilerOptions {
  // Total linear-memory size in bytes, forwarded to the underlying
  // pipeline's `mem_size_bytes`.  Default is two wasm pages
  // (128 KiB) — matches cel_runtime.wasm's `--import-memory` min=2.
  // Raise this when an expression needs a larger arena.
  uint32_t mem_size_bytes = 128u * 1024u;
};

class Compiler {
 public:
  class Builder;
  static Builder NewBuilder();

  // Pure-data class for M1 (no held wasmtime resources).  Copyable
  // when future declarations land — for now movable + copyable both
  // work.
  Compiler(const Compiler&) = default;
  Compiler& operator=(const Compiler&) = default;
  Compiler(Compiler&&) noexcept = default;
  Compiler& operator=(Compiler&&) noexcept = default;
  ~Compiler() = default;

  // Compile a CEL source string to a Program (wasm bytes).  Runs the
  // M1 pipeline (parse → check → resolve → layout → module → lower
  // → assemble).  No wasmtime involvement; the Program is just
  // bytes + ABI.
  //
  // Status mapping flows through from the underlying pipeline:
  //   - InvalidArgument: parse / check failure, or a static-subset
  //     violation
  //   - Unimplemented:   AST shape M1 doesn't handle yet
  //   - FailedPrecondition: binaryen validate failure
  ABSL_MUST_USE_RESULT absl::StatusOr<Program> Compile(
      absl::string_view source, CompilerOptions opts = {}) const;

 private:
  friend class Builder;
  Compiler() = default;  // Builder constructs.
};

class Compiler::Builder {
 public:
  Builder() = default;

  // Special members defaulted while the Builder has no state.  When
  // future commits add declarations (DeclareVariable,
  // RegisterFunction, …) the .cc will need definitions.
  ~Builder() = default;
  Builder(const Builder&) = delete;
  Builder& operator=(const Builder&) = delete;
  Builder(Builder&&) noexcept = default;
  Builder& operator=(Builder&&) noexcept = default;

  // Materialises the Compiler.  M1 has no per-Builder state to
  // validate, so this never fails — but the &&-consume + StatusOr
  // shape is established now so future declaration validation
  // (collisions, type mismatches, etc.) can return errors without
  // a signature change.
  ABSL_MUST_USE_RESULT absl::StatusOr<Compiler> Build() &&;
};

}  // namespace cel

#endif  // CELWASM_COMPILER_V2_API_COMPILER_H_
