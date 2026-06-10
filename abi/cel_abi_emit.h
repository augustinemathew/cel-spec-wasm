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

#include "abi/cel_abi.pb.h"
#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/layout_pass.h"

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
ABSL_MUST_USE_RESULT absl::StatusOr<celwasm::abi::CelAbi> BuildCelAbi(
    const StaticLayout& layout, absl::Span<const FieldRefRow> field_refs,
    celwasm::abi::LinkMode link_mode);

}  // namespace celwasm

#endif  // CELWASM_ABI_CEL_ABI_EMIT_H_
