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

namespace celwasm::api {

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
    case CelType::Kind::kType:
      // `type` is a primitive-shaped declaration — the spec
      // parser maps `"type"` to `cel::TypeType` via cel-cpp's
      // standard library variable registration.
      return "type";
    case CelType::Kind::kMessage:
      return std::string(t.message_fully_qualified_name());
    case CelType::Kind::kList:
      return absl::StrCat("list<", CelTypeToSpec(t.list_element()), ">");
    case CelType::Kind::kMap:
      // Map variable declarations travel through parse_and_check;
      // runtime materialisation routes through the host map dispatch
      // arm (see `rewrite/map-list-dispatch.md`).
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

Compiler::Builder& Compiler::Builder::AddLibrary(
    celwasm::FunctionLibrary library) {
  function_libraries_.push_back(std::move(library));
  return *this;
}

Compiler::Builder& Compiler::Builder::AddFunction(
    absl::string_view celfn_source) {
  auto lib_or = celwasm::ParseCelfnSource(celfn_source);
  if (!lib_or.ok()) {
    // Defer to Build() — earlier failure wins (don't overwrite).
    if (deferred_status_.ok()) {
      deferred_status_ =
          absl::Status(lib_or.status().code(),
                       absl::StrCat("Compiler::Builder::AddFunction: ",
                                    lib_or.status().message()));
    }
    return *this;
  }
  function_libraries_.push_back(*std::move(lib_or));
  return *this;
}

absl::StatusOr<Compiler> Compiler::Builder::Build() && {
  if (!deferred_status_.ok()) return deferred_status_;

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

  // M13 Slice C.2 — duplicate overload-id detection ACROSS libraries.
  // Within a single library, `FunctionLibrary::Builder::Build` already
  // rejected duplicates; the cross-library check here catches the case
  // where two separate `AddLibrary` calls each declare the same
  // overload-id.
  absl::flat_hash_set<std::string> seen_overload_ids;
  for (const auto& lib : function_libraries_) {
    for (const auto& d : lib.decls()) {
      if (!seen_overload_ids.insert(d.overload_id).second) {
        return absl::InvalidArgumentError(absl::StrCat(
            "Compiler::Builder::Build: overload-id `", d.overload_id,
            "` is declared by more than one library"));
      }
    }
  }

  Compiler c;
  c.declared_variables_ = std::move(declared_variables_);
  c.function_libraries_ = std::move(function_libraries_);
  return c;
}

absl::StatusOr<Program> Compiler::Compile(absl::string_view source,
                                          const CompilerOptions& opts) const {
  celwasm::CompileOptions inner;
  inner.mem_size_bytes = opts.mem_size_bytes;
  inner.check.container = opts.container;
  inner.optimize_level = opts.optimize_level;
  inner.check.variable_specs.reserve(declared_variables_.size());
  for (const auto& decl : declared_variables_) {
    inner.check.variable_specs.push_back(
        absl::StrCat(decl.name, ":", CelTypeToSpec(decl.type)));
  }
  // M13 Slice C.3 — forward custom-fn libraries to both the checker
  // (call-site resolution) and codegen (OverloadTable registration).
  inner.check.function_libraries = function_libraries_;
  inner.function_libraries = function_libraries_;
  auto artifact_or = celwasm::Compile(source, inner);
  if (!artifact_or.ok()) return artifact_or.status();
  return Program(std::move(artifact_or->wasm_bytes));
}

}  // namespace celwasm::api
