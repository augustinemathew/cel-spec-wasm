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
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"

// Forward-declare protobuf::Descriptor to keep protobuf out of the
// header's transitive set.  Callers that build a Compiler with
// RegisterMessageType already include protobuf headers.
namespace google::protobuf {
class Descriptor;
}

namespace cel {

// One declared free variable: name + static type.  Introspected via
// `Compiler::declared_variables()`; the checker resolves each ident
// expression against this list at `Compile` time.
//
// Named `VariableDeclaration` rather than `VariableDecl` to avoid an
// ODR collision with `cel::VariableDecl` from cel-cpp's
// `common/decl.h` — both live in `namespace cel::` and a clash causes
// a subtle crash when the checker builder constructs its standard-
// library declarations.
struct VariableDeclaration {
  std::string name;
  CelType type;
};

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

  // Copyable + movable — a Compiler is pure data (declarations +
  // registered descriptors).  Descriptors are non-owning pointers
  // into a `DescriptorPool` the caller manages (typically
  // `generated_pool()`).
  Compiler(const Compiler&) = default;
  Compiler& operator=(const Compiler&) = default;
  Compiler(Compiler&&) noexcept = default;
  Compiler& operator=(Compiler&&) noexcept = default;
  ~Compiler() = default;

  // Compile a CEL source string to a Program (wasm bytes).  Runs the
  // full pipeline (parse → check → resolve → layout → module → lower
  // → assemble).  No wasmtime involvement; the Program is just
  // bytes + ABI.
  //
  // Status mapping flows through from the underlying pipeline:
  //   - InvalidArgument: parse / check failure, or a static-subset
  //     violation
  //   - Unimplemented:   AST shape the current milestone doesn't
  //                      handle yet
  //   - FailedPrecondition: binaryen validate failure
  ABSL_MUST_USE_RESULT absl::StatusOr<Program> Compile(
      absl::string_view source, CompilerOptions opts = {}) const;

  // Introspection — declarations visible to this Compiler.
  absl::Span<const VariableDeclaration> declared_variables() const {
    return declared_variables_;
  }

 private:
  friend class Builder;
  Compiler() = default;  // Builder constructs.

  std::vector<VariableDeclaration> declared_variables_;
  // Non-owning descriptors registered via Builder::RegisterMessageType.
  // M2 uses them for forward-compat introspection / later schema
  // export; the checker itself looks up message types by FQN against
  // the process-wide generated DescriptorPool (cc_proto_library-linked
  // descriptors are reachable there, which covers every M2 call site).
  std::vector<const google::protobuf::Descriptor*> registered_messages_;
};

class Compiler::Builder {
 public:
  Builder() = default;

  ~Builder() = default;
  Builder(const Builder&) = delete;
  Builder& operator=(const Builder&) = delete;
  Builder(Builder&&) noexcept = default;
  Builder& operator=(Builder&&) noexcept = default;

  // Declare a free variable available to every expression this
  // Compiler compiles.  The checker resolves each ident against this
  // list; a reference to an undeclared variable fails at compile
  // time with `InvalidArgument`.  Duplicate names on the same Builder
  // are rejected by Build() with `InvalidArgument`, not silently
  // deduped.
  //
  // Args are taken by const reference and copied into the internal
  // decl vector.  Declaration is not a hot path; a handful of string
  // copies per Build() is well below the noise floor.
  //
  // Returns `*this` so calls chain:
  //     Compiler::Builder b;
  //     b.DeclareVariable("x", CelType::Int())
  //      .DeclareVariable("s", CelType::String());
  //     auto c = std::move(b).Build();
  // The ONLY consuming method on Builder is `Build()`; every setter
  // mutates `*this` in place and returns an lvalue reference.
  Builder& DeclareVariable(const std::string& name, const CelType& type);

  // Register a protobuf message type for use in declarations
  // (`CelType::Message("fqn")`) and field reads.  Non-owning: `desc`
  // must outlive any Program compiled from the resulting Compiler.
  // M2 relies on the process-wide generated descriptor pool for
  // lookup, so this call is only required for introspection /
  // forward-compat — but calling it is the documented path and the
  // surface is locked now.
  Builder& RegisterMessageType(
      const google::protobuf::Descriptor* absl_nonnull desc);

  // Materialises the Compiler.  Consumes the Builder — chain from a
  // named local with `std::move(b).Build()`.
  //
  // Returns InvalidArgument on:
  //   - duplicate variable names declared on this Builder
  //   - a variable declared with CelType::Kind::kUnknown
  //   - a variable of Message type whose FQN is empty
  ABSL_MUST_USE_RESULT absl::StatusOr<Compiler> Build() &&;

 private:
  std::vector<VariableDeclaration> declared_variables_;
  std::vector<const google::protobuf::Descriptor*> registered_messages_;
};

}  // namespace cel

#endif  // CELWASM_COMPILER_V2_API_COMPILER_H_
