// Describe a compiled `.wasm` from its `cel.abi` section.
//
// Shared by `cel inspect` (which prints the description) and `cel run`
// (which uses it to type `--var` values), so the two cannot disagree
// about what an artifact declares.
//
// Reads the `cel.abi` custom section directly — no wasmtime, no
// `Engine` — so `inspect` works on a program this build could not
// actually run.
//
// Scope note: this reports what a program *declares*, not everything
// it *requires*.  The list of custom functions and foreign modules a
// program demands is not on the wire today; `cel.abi` field 8
// (`required_functions`, specified in `rewrite/m35-component-
// ergonomics.md` §5.1) adds it, emitted from the post-optimize import
// surface and verified at `Engine::Plan`.  Reporting those belongs
// here once that field lands; deriving them independently by walking
// the wasm import section would be a second, weaker source of truth.

#ifndef CELWASM_TOOLS_CEL_ABI_DESCRIBE_H_
#define CELWASM_TOOLS_CEL_ABI_DESCRIBE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/ir/annotations.h"

namespace celwasm::tools::cel {

// One declared free variable.
//
// `cel.abi` carries only a numeric `repr`, not a full `CelType` — the
// wire has no element type for a list, no key/value types for a map,
// and no message FQN (`VariableEntry` reserves a field for that, still
// unused).  So `type_name` is the repr's name (`int`, `map`,
// `message`), never a parameterized form.
struct DeclaredVar {
  std::string name;
  Repr repr = Repr::kUnknown;
  std::string type_name;
};

// What a program declares.
struct ProgramFacts {
  std::vector<DeclaredVar> vars;
  bool static_linked = false;
  uint32_t abi_version = 0;
  uint32_t runtime_abi_version = 0;
  // False when the module carries no `cel.abi` section at all (a
  // hand-written WAT fixture, or a module predating the section).
  bool has_abi_section = false;
};

// The type-spec token a `--var name=value` binding parses against,
// for a variable whose only type information is its repr — i.e. what
// `run` splices in to reconstruct the full `name:Type=value` form the
// var parser consumes.
//
// Returns InvalidArgument for reprs with no complete spelling on the
// wire (list/map/message/enum, and `type` which the var grammar does
// not accept); those need the explicit `--var name:Type=value` form,
// and the error says so.
absl::StatusOr<std::string> ScalarTypeSpecForRepr(Repr repr,
                                                  absl::string_view var_name);

// Decode `wasm_bytes`.  InvalidArgument if the bytes are not a wasm
// module or the section is malformed; a missing `cel.abi` section is
// not an error (see `has_abi_section`).
absl::StatusOr<ProgramFacts> DescribeProgram(
    absl::Span<const uint8_t> wasm_bytes);

// Render `facts` as the `cel inspect` report.
std::string FormatProgramFacts(const ProgramFacts& facts);

}  // namespace celwasm::tools::cel

#endif  // CELWASM_TOOLS_CEL_ABI_DESCRIBE_H_
