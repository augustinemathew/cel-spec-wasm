#include "tools/cel/abi_describe.h"

#include <cstdint>
#include <string>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/ir/annotations.h"
#include "eval/internal/abi_decode.h"

namespace celwasm::tools::cel {

absl::StatusOr<std::string> ScalarTypeSpecForRepr(Repr repr,
                                                  absl::string_view var_name) {
  switch (repr) {
    case Repr::kBool:
      return "bool";
    case Repr::kInt:
      return "int";
    case Repr::kUint:
      return "uint";
    case Repr::kDouble:
      return "double";
    case Repr::kString:
      return "string";
    case Repr::kBytes:
      return "bytes";
    case Repr::kDuration:
      return "duration";
    case Repr::kTimestamp:
      return "timestamp";
    default:
      break;
  }
  // Aggregate and enum reprs have no complete type on the wire: a
  // list's element type, a map's key/value types, and a message's FQN
  // are all absent from `cel.abi`.  Point the caller at the form that
  // supplies them explicitly rather than guessing.
  return absl::InvalidArgumentError(absl::StrCat(
      "--var ", var_name, ": the program declares `", var_name, "` as ",
      ReprName(repr),
      ", whose full type is not carried in the program's cel.abi; bind it "
      "with the explicit form `--var ",
      var_name, ":<Type>=<value>`"));
}

absl::StatusOr<ProgramFacts> DescribeProgram(
    absl::Span<const uint8_t> wasm_bytes) {
  ProgramFacts facts;
  auto abi = DecodeCelAbiFromWasm(wasm_bytes);
  if (!abi.ok()) {
    // A module with no `cel.abi` section is describable — it simply
    // declares nothing.  Anything else is a malformed input.
    if (abi.status().code() != absl::StatusCode::kNotFound) {
      return abi.status();
    }
    return facts;
  }
  facts.has_abi_section = true;
  facts.abi_version = abi->version();
  facts.runtime_abi_version = abi->runtime_abi_version();
  facts.static_linked = abi->link_mode() == celwasm::abi::LINK_MODE_STATIC;
  facts.vars.reserve(abi->variables_size());
  for (const celwasm::abi::VariableEntry& v : abi->variables()) {
    const Repr repr = DecodeRepr(v.repr());
    facts.vars.push_back(
        DeclaredVar{v.name(), repr, std::string(ReprName(repr))});
  }
  return facts;
}

std::string FormatProgramFacts(const ProgramFacts& facts) {
  if (!facts.has_abi_section) {
    return "vars:  <none — this module carries no cel.abi section>\n";
  }
  std::vector<std::string> rendered;
  rendered.reserve(facts.vars.size());
  for (const DeclaredVar& v : facts.vars) {
    rendered.push_back(absl::StrCat(v.name, ":", v.type_name));
  }
  return absl::StrCat(
      "vars:  ", rendered.empty() ? "none" : absl::StrJoin(rendered, ", "),
      "\n", "link:  ", facts.static_linked ? "static" : "dynamic",
      " (cel.abi v", facts.abi_version, ", runtime abi v",
      facts.runtime_abi_version, ")\n");
}

}  // namespace celwasm::tools::cel
