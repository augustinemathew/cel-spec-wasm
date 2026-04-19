#ifndef CELWASM_COMPILER_FRONTEND_PARSE_AND_CHECK_H_
#define CELWASM_COMPILER_FRONTEND_PARSE_AND_CHECK_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

// Options for `ParseAndCheck`.
//
// `variable_specs` entries are of the form `name:Type` where `Type` is one of:
//   - a primitive: bool, int, uint, double, string, bytes, null_type
//   - a well-known: timestamp, duration, any
//   - a parameterized type: list<T>, map<K,V> (recursively composed)
//   - a protobuf message's fully-qualified name, e.g. `google.example.Request`
struct CheckOptions {
  // Path to a binary-serialized `google.protobuf.FileDescriptorSet` that
  // describes protobuf message types referenced by variables.  When empty only
  // the CEL well-known descriptors are available.
  std::string schema_path;

  // `name:Type` variable declarations injected into the checker's env.
  std::vector<std::string> variable_specs;

  // Package container used for name resolution (CEL-Go `container` / CEL-Java
  // `container`).
  std::string container;

  // Source description passed to the parser / checker for diagnostics.
  std::string description = "<input>";
};

// Parses and type-checks `expression` per `opts`.  On success returns a
// `TypedAst` whose `ast()` is a checked `cel::Ast` and whose `annotations()`
// has been seeded with a `Repr` for every typed node.
absl::StatusOr<TypedAst> ParseAndCheck(absl::string_view expression,
                                       const CheckOptions& opts);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_FRONTEND_PARSE_AND_CHECK_H_
