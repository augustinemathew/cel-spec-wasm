#include "abi/program_facts.h"

#include <cstdint>
#include <string>
#include <vector>

#include "abi/cel_abi.pb.h"
#include "abi/celfn_wire.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/ir/annotations.h"
#include "eval/internal/abi_decode.h"

namespace celwasm::abi {

namespace {

// `abi::Type` → the `--var` type-spec grammar.
//
// Deliberately not `RenderType` from celfn_wire: that renders the
// `.celfn` grammar (`proto(acme.User)`, `Duration`), which the var
// parser does not accept.  Two grammars, two renderers — this one
// exists to be pasted back into a `--var` flag.
std::string RenderVarTypeSpec(const Type& t) {
  switch (t.kind()) {
    case Type::KIND_BOOL:
      return "bool";
    case Type::KIND_INT:
      return "int";
    case Type::KIND_UINT:
      return "uint";
    case Type::KIND_DOUBLE:
      return "double";
    case Type::KIND_STRING:
      return "string";
    case Type::KIND_BYTES:
      return "bytes";
    case Type::KIND_DURATION:
      return "duration";
    case Type::KIND_TIMESTAMP:
      return "timestamp";
    case Type::KIND_TYPE:
      return "type";
    case Type::KIND_PROTO:
      return t.proto_fqn();
    case Type::KIND_LIST:
      return absl::StrCat(
          "list<", t.params_size() == 1 ? RenderVarTypeSpec(t.params(0)) : "?",
          ">");
    case Type::KIND_MAP:
      return absl::StrCat(
          "map<", t.params_size() == 2 ? RenderVarTypeSpec(t.params(0)) : "?",
          ",", t.params_size() == 2 ? RenderVarTypeSpec(t.params(1)) : "?",
          ">");
    default:
      // Open-set wire data is described, never rejected.
      break;
  }
  return absl::StrCat("<kind ", static_cast<int>(t.kind()), ">");
}

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

}  // namespace

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
  facts.static_linked = abi->link_mode() == LINK_MODE_STATIC;
  facts.vars.reserve(abi->variables_size());
  for (const VariableEntry& v : abi->variables()) {
    const Repr repr = DecodeRepr(v.repr());
    DeclaredVar out{v.name(), repr, std::string(ReprName(repr)), false};
    if (v.has_type()) {
      out.type_spec = RenderVarTypeSpec(v.type());
      out.has_full_type = true;
    }
    facts.vars.push_back(std::move(out));
  }
  facts.required_fns.reserve(abi->required_functions_size());
  for (const RequiredFunction& fn : abi->required_functions()) {
    facts.required_fns.push_back(
        RequiredFn{fn.fn_name(), ::celwasm::RenderSignature(fn),
                   fn.backend() == RequiredFunction::HOST});
  }
  return facts;
}

absl::StatusOr<std::string> TypeSpecForBinding(const DeclaredVar& var) {
  if (var.has_full_type) return var.type_spec;
  return ScalarTypeSpecForRepr(var.repr, var.name);
}

}  // namespace celwasm::abi
