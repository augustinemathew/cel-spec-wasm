// Describe a compiled `.wasm` from its `cel.abi` section: what it
// declares, and what it requires to run.
//
// Lives beside the other artifact-introspection libraries
// (`abi/wasm_binary.h`) rather than in a tool, because
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
  // The declared type in the `--var` type-spec grammar
  // (`list<int>`, `map<string,int>`, `acme.User`) when the program
  // carries one, so it can be printed *and* pasted straight into a
  // `--var name:<type>=<value>` binding.
  //
  // Programs emitted before `cel.abi.VariableEntry.type` existed carry
  // no type; `type_spec` is then the bare repr name (`list`) and
  // `has_full_type` is false.  Never invent a type from `repr` — a
  // wrong element type parses a literal wrongly.
  std::string type_spec;
  bool has_full_type = false;
};

// One custom function the program will demand at `Plan` — a `@host.`
// C++ callback in the embedder's process.  Backend verification
// (including rejecting rows whose wire backend this build does not
// recognise) happens at `Engine::Plan`, not here — see
// `eval/internal/required_fn_check.cc`.
struct RequiredFn {
  std::string name;       // source-level name
  std::string signature;  // `.celfn` spelling, via abi::RenderSignature
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

// The type spec a `--var name=value` binding parses against — what
// `run` splices in to rebuild the `name:Type=value` form the var
// parser consumes.
//
// Uses `var.type_spec` when the program carries a full type.  Falls
// back to the repr name for older artifacts, which works for scalars
// and returns InvalidArgument for aggregates — whose element/key/value
// types are genuinely unknown there — naming the explicit
// `--var name:Type=value` form as the way out.
absl::StatusOr<std::string> TypeSpecForBinding(const DeclaredVar& var);

// Decode `wasm_bytes`.  InvalidArgument if the bytes are not a wasm
// module or the section is malformed; a missing `cel.abi` section is
// not an error (see `has_abi_section`).
absl::StatusOr<ProgramFacts> DescribeProgram(
    absl::Span<const uint8_t> wasm_bytes);

}  // namespace celwasm::abi

#endif  // CELWASM_ABI_PROGRAM_FACTS_H_
