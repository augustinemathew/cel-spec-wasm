#ifndef CELWASM_COMPILER_V2_FRONTEND_PARSE_AND_CHECK_H_
#define CELWASM_COMPILER_V2_FRONTEND_PARSE_AND_CHECK_H_

#include <string>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/celfn/function_library.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

// Path to a textual `.proto` source file describing protobuf message types
// referenced by variables.  Parsed in-process with
// `google::protobuf::compiler::Parser`; imports other than CEL well-known
// types are not resolved at parse time, so use `SchemaDescriptorSet` for
// multi-file schemas that reference one another.
struct SchemaProtoSource {
  std::string path;
};

// Path to a binary-serialized `google.protobuf.FileDescriptorSet` (the output
// of `protoc --descriptor_set_out=...`) describing protobuf message types
// referenced by variables.  Preferred for multi-file schemas or when the
// caller already runs `protoc` in their build.
struct SchemaDescriptorSet {
  std::string path;
};

// Options for `ParseAndCheck`.
//
// `variable_specs` entries are of the form `name:Type` where `Type` is one of:
//   - a primitive: bool, int, uint, double, string, bytes, null_type
//   - a well-known: timestamp, duration, any
//   - a parameterized type: list<T>, map<K,V> (recursively composed)
//   - a protobuf message's fully-qualified name, e.g. `google.example.Request`
struct CheckOptions {
  // Schema source for protobuf message types referenced by variables.
  // `std::monostate` (the default) means "use the process-wide generated
  // descriptor pool only".
  std::variant<std::monostate, SchemaProtoSource, SchemaDescriptorSet> schema;

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

#endif  // CELWASM_COMPILER_V2_FRONTEND_PARSE_AND_CHECK_H_
