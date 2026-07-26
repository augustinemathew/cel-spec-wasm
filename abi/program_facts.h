// Describe a compiled `.wasm` from its `cel.abi` section: what it
// declares, and what it requires to run.
//
// Lives beside the other artifact-introspection libraries
// (`abi/plugin.h`, `abi/wasm_binary.h`) rather than in a tool, because
// nothing about it is CLI-specific — any embedder, binding, or
// build-time check that wants to know "what does this artifact need?"
// asks here.  `tools/cel` keeps only the rendering of these facts to a
// terminal.
//
// Reads the `cel.abi` custom section directly — no wasmtime, no
// `Engine` — so `inspect` works on a program this build could not
// actually run.
//
// Both the declared variables and the required custom functions come
// from `cel.abi` — the latter from `required_functions` (field 8),
// emitted from the post-optimize import surface and verified at
// `Engine::Plan`.  Reading them here rather than walking the wasm
// import section keeps one source of truth: the section is authored
// from the final module, so it stays correct across optimize levels
// (Binaryen drops unused imports at O1+).

#ifndef CELWASM_ABI_PROGRAM_FACTS_H_
#define CELWASM_ABI_PROGRAM_FACTS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/ir/annotations.h"

namespace celwasm::abi {

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

// One custom function the program will demand at `Plan`.
struct RequiredFn {
  std::string name;       // source-level name
  std::string signature;  // `.celfn` spelling, via abi::RenderSignature
  // `@host` functions are C++ in the embedder's process; the CLI
  // cannot supply them.  Plugin functions are satisfiable with a
  // wasm artifact.
  bool is_host = false;
};

// What a program declares, and what it requires to run.
struct ProgramFacts {
  std::vector<DeclaredVar> vars;
  // From `cel.abi.required_functions`; empty for a program that calls
  // no custom functions, and for programs compiled before the field
  // existed (the check no-ops rather than claiming "none" falsely —
  // see `has_required_fn_table`).
  std::vector<RequiredFn> required_fns;
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

}  // namespace celwasm::abi

#endif  // CELWASM_ABI_PROGRAM_FACTS_H_
