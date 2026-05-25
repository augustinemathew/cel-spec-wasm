// Emit the `cel.abi` custom section from the compile-time layout.
//
// Compile-side counterpart to `compiler_v2/api/internal/abi_decode.h`
// (load-time decoder).  Compile.cc calls `EmitCelAbi` after
// LayoutPass + LowerToEvalFunction and before module.Serialize();
// Engine::Plan calls the decoder after reading the wasm bytes.
//
// Wire format: a serialized `celwasm.abi.CelAbi` proto, wrapped in
// a wasm custom section named `"cel.abi"` (appended to the module
// via `WasmModule::AddCustomSection`).

#ifndef CELWASM_COMPILER_V2_ABI_CEL_ABI_EMIT_H_
#define CELWASM_COMPILER_V2_ABI_CEL_ABI_EMIT_H_

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "compiler_v2/abi/cel_abi.pb.h"
#include "compiler_v2/codegen/expr_lower.h"
#include "compiler_v2/codegen/layout_pass.h"

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
// The version field is set to 1 (bumped on any breaking schema
// change).
ABSL_MUST_USE_RESULT absl::StatusOr<celwasm::abi::CelAbi> BuildCelAbi(
    const StaticLayout& layout, absl::Span<const FieldRefRow> field_refs);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_ABI_CEL_ABI_EMIT_H_
