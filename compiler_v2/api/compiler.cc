#include "compiler_v2/api/compiler.h"

#include <string>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/compile.h"

namespace cel {

namespace {

// Serialise a CelType into the `name:Type` spec-string form
// `celwasm::CheckOptions::variable_specs` consumes.  The type grammar
// the parse_and_check.cc type parser accepts:
//   scalars: bool / int / uint / double / string / bytes /
//            duration / timestamp
//   containers: list<T>, map<K,V>
//   messages: fully.qualified.Name
//
// Any later-milestone kind (kMap is the payload of a dyn-dispatched
// map/list at M6; kList likewise) lands here through the recursive
// List/Map arms — but CelType::Kind::kUnknown is an M2-level error
// we surface with a diagnostic.
std::string CelTypeToSpec(const CelType& t) {
  switch (t.kind()) {
    case CelType::Kind::kBool:
      return "bool";
    case CelType::Kind::kInt:
      return "int";
    case CelType::Kind::kUint:
      return "uint";
    case CelType::Kind::kDouble:
      return "double";
    case CelType::Kind::kString:
      return "string";
    case CelType::Kind::kBytes:
      return "bytes";
    case CelType::Kind::kDuration:
      return "duration";
    case CelType::Kind::kTimestamp:
      return "timestamp";
    case CelType::Kind::kMessage:
      return std::string(t.message_fully_qualified_name());
    case CelType::Kind::kList:
      return absl::StrCat("list<", CelTypeToSpec(t.list_element()), ">");
    case CelType::Kind::kMap:
      // Map *variable declarations* travel here fine — parse_and_check
      // parses the spec and the checker records the type — but any
      // code path that materialises a map value at runtime (reads
      // from the workspace slot, lowers a map comprehension, …) is
      // blocked until M6.  The CHECK lives at the runtime edge
      // (e.g. Instance::Eval's activation marshalling), not here.
      return absl::StrCat("map<", CelTypeToSpec(t.map_key()), ",",
                          CelTypeToSpec(t.map_value()), ">");
    case CelType::Kind::kUnknown:
      // Not reachable in happy paths — Build() rejects kUnknown
      // declarations up front.  If we land here it's a caller who
      // default-constructed a CelType and shoved it past Build().
      ABSL_CHECK(false)
          << "CelTypeToSpec: kUnknown CelType reached the spec encoder";
  }
  // Closed enum; any new kind reaching here without a case arm is
  // an invariant violation.
  ABSL_CHECK(false) << "CelTypeToSpec: unhandled CelType::Kind = "
                    << static_cast<int>(t.kind());
}

// Validate one variable declaration.  Reject:
//   - kUnknown type (default-constructed CelType — caller forgot to
//     pick a kind)
//   - empty message FQN (CelType::Message("") slipped through)
absl::Status ValidateDecl(const VariableDeclaration& decl) {
  if (decl.name.empty()) {
    return absl::InvalidArgumentError(
        "Compiler::Builder::DeclareVariable: variable name is empty");
  }
  if (decl.type.kind() == CelType::Kind::kUnknown) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Compiler::Builder::DeclareVariable: variable `", decl.name,
        "` has CelType::Kind::kUnknown (default-constructed CelType "
        "— pick an explicit kind)"));
  }
  if (decl.type.kind() == CelType::Kind::kMessage &&
      decl.type.message_fully_qualified_name().empty()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Compiler::Builder::DeclareVariable: variable `", decl.name,
        "` has Message type with an empty fully-qualified name"));
  }
  return absl::OkStatus();
}

}  // namespace

Compiler::Builder Compiler::NewBuilder() {
  return {};
}

Compiler::Builder& Compiler::Builder::DeclareVariable(const std::string& name,
                                                      const CelType& type) {
  declared_variables_.push_back(VariableDeclaration{name, type});
  return *this;
}

absl::StatusOr<Compiler> Compiler::Builder::Build() && {
  absl::flat_hash_set<std::string> seen;
  seen.reserve(declared_variables_.size());
  for (const auto& decl : declared_variables_) {
    if (auto s = ValidateDecl(decl); !s.ok()) return s;
    if (!seen.insert(decl.name).second) {
      return absl::InvalidArgumentError(
          absl::StrCat("Compiler::Builder::Build: duplicate variable "
                       "declaration `",
                       decl.name, "`"));
    }
  }
  Compiler c;
  c.declared_variables_ = std::move(declared_variables_);
  return c;
}

absl::StatusOr<Program> Compiler::Compile(absl::string_view source,
                                          const CompilerOptions& opts) const {
  celwasm::CompileOptions inner;
  inner.mem_size_bytes = opts.mem_size_bytes;
  inner.check.container = opts.container;
  inner.check.variable_specs.reserve(declared_variables_.size());
  for (const auto& decl : declared_variables_) {
    inner.check.variable_specs.push_back(
        absl::StrCat(decl.name, ":", CelTypeToSpec(decl.type)));
  }
  auto artifact_or = celwasm::Compile(source, inner);
  if (!artifact_or.ok()) return artifact_or.status();
  return Program(std::move(artifact_or->wasm_bytes));
}

}  // namespace cel
