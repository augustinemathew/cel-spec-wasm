#ifndef CELWASM_COMPILER_FRONTEND_PARSE_AND_CHECK_H_
#define CELWASM_COMPILER_FRONTEND_PARSE_AND_CHECK_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"
#include "compiler/ir/typed_ast.h"

namespace google::protobuf {
class DescriptorPool;
}  // namespace google::protobuf

namespace celwasm {

// Maximum expression nesting depth (AST levels; a leaf is depth 1)
// the compile pipeline admits.  Codegen emits an expression as a
// nested wasm expression tree of the same depth, and both the host
// toolchain (lowering, Binaryen) and wasmtime's Plan-time validation
// / Cranelift JIT walk that tree recursively — one native stack
// frame per nesting level — so unbounded depth lets a deep
// left-associative chain (`a+b+c+...`) overflow the ~8 MiB native
// stack at depth ≈4.6k: a SIGSEGV on valid CEL.  `ParseAndCheck`
// rejects deeper expressions with `ResourceExhausted`.  The limit
// clears the 1000-term arithmetic benchmark and realistic policies
// with wide margin; it is an interim bound until codegen flattens
// operand nesting, at which point it can be dropped
// (doc/implementation-plan/cleanup-backlog.md #45).
inline constexpr int kMaxExpressionNestingDepth = 2048;

// Options for `ParseAndCheck`.
//
// `variable_specs` entries are of the form `name:Type` where `Type` is one of:
//   - a primitive: bool, int, uint, double, string, bytes, null_type
//   - a well-known: timestamp, duration, any
//   - a parameterized type: list<T>, map<K,V> (recursively composed)
//   - a protobuf message's fully-qualified name, e.g. `google.example.Request`
struct CheckOptions {
  // Descriptor pool that message-typed variable declarations and proto
  // expressions resolve against.  The supplied pool resolves a type first;
  // any type it does not contain falls back to the process-wide
  // `generated_pool()` (so well-known types and statically-linked messages
  // always resolve even when the supplied pool carries only the caller's
  // schema).  `nullptr` (the default) resolves against `generated_pool()`
  // alone.  Borrowed — the pool must outlive every `ParseAndCheck` that
  // reads it.  Takes precedence over `schema` when both are set.
  const google::protobuf::DescriptorPool* descriptor_pool = nullptr;

  // `name:Type` variable declarations injected into the checker's env.
  std::vector<std::string> variable_specs;

  // Package container used for name resolution (CEL-Go `container` / CEL-Java
  // `container`).
  std::string container;

  // Source description passed to the parser / checker for diagnostics.
  std::string description = "<input>";

  // M13 Slice C.3 — custom-fn declarations to register with the
  // checker so call-sites like `name.is_number()` resolve.  Each
  // library's decls become individual `cel::FunctionDecl`s on the
  // `TypeCheckerBuilder`.  Cross-library overload-id collisions are
  // assumed already filtered by `Compiler::Builder::Build` (the
  // upstream surface).  When empty (the default), no custom fns are
  // visible to the checker.
  std::vector<FunctionLibrary> function_libraries;
};

// Parses, type-checks, and validates that `expression` falls inside the
// static subset (no DYN / ERROR / type-param / function / unset nodes).
// On success returns a `TypedAst` whose `ast()` is a checked `cel::Ast` and
// whose `annotations()` has been seeded with a `Repr` for every typed node.
absl::StatusOr<TypedAst> ParseAndCheck(absl::string_view expression,
                                       const CheckOptions& opts);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_FRONTEND_PARSE_AND_CHECK_H_
