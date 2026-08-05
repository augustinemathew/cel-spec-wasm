// Emit the `cel.abi` custom section from the compile-time layout.
//
// Compile-side counterpart to `eval/internal/abi_decode.h`
// (load-time decoder).  Compile.cc calls `EmitCelAbi` after
// LayoutPass + LowerToEvalFunction and before module.Serialize();
// Engine::Plan calls the decoder after reading the wasm bytes.
//
// Wire format: a serialized `celwasm.abi.CelAbi` proto, wrapped in
// a wasm custom section named `"cel.abi"` (appended to the module
// via `WasmModule::AddCustomSection`).

#ifndef CELWASM_ABI_CEL_ABI_EMIT_H_
#define CELWASM_ABI_CEL_ABI_EMIT_H_

#include <vector>

#include "abi/cel_abi.pb.h"
#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/layout_pass.h"
#include "compiler/codegen/module.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

// Build a `CelAbi` message from the compile-time layout + codegen
// output.  Populates:
//   - `variables[]` from `layout.variables`
//   - `fields[]` from `field_refs` (field intern table, one
//     row per kSelect; index 0 is the reserved sentinel)
//   - `attributes[]` from `layout.attributes` (attribute intern
//     table; index 0 is the reserved sentinel)
//   - `types[]` from `layout.message_types` (message-type intern
//     table; index 0 is the reserved sentinel)
//   - `link_mode` from the caller — the compile pipeline stamps the
//     mode it emitted the module under (metadata for embedder
//     tooling, not an engine routing input; see
//     doc/implementation-plan/rewrite/m28-configurable-linking.md §6)
// The version field is set to 1 (bumped on any breaking schema
// change).
// `declared` supplies each free variable's full declared type, matched
// by name onto `layout.variables`.  The layout carries only `repr`
// (the wire kind the marshal encodes against); the declared `CelType`
// is what lets a consumer print `xs:list<int>` or parse a literal
// against the real type.  Pass an empty span to omit types — entries
// then carry `repr` alone, exactly as before the field existed.
ABSL_MUST_USE_RESULT absl::StatusOr<celwasm::abi::CelAbi> BuildCelAbi(
    const StaticLayout& layout, absl::Span<const FieldRefRow> field_refs,
    celwasm::abi::LinkMode link_mode,
    absl::Span<const celwasm::Variable> declared = {});

// Build the `required_functions[]` table from the FINAL module's
// import surface: one row per `cel_fn` import in `imports` (module
// import order preserved), populated from the matching decl across
// `libraries`.  Imports from any other module (`cel`, `cel_host`)
// contribute nothing.
//
// `imports` MUST be the post-optimize import list
// (`WasmModule::ListFunctionImports()` after `Optimize`): Binaryen
// drops unused imports at O1+ while wasmtime demands every SURVIVING
// import at link time, so deriving the table from anything but the
// final module desyncs per optimize level.
//
// Backend mapping: kHost -> HOST.  A `cel_fn` import with no
// matching decl is an invariant violation (codegen installed the
// import FROM these libraries) and CHECK-fails.
std::vector<celwasm::abi::RequiredFunction> BuildRequiredFunctions(
    absl::Span<const WasmModule::FunctionImportName> imports,
    absl::Span<const FunctionLibrary> libraries);

}  // namespace celwasm

#endif  // CELWASM_ABI_CEL_ABI_EMIT_H_
