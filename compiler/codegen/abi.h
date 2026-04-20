// Emits the `cel.abi` WASM custom section.
//
// The section is the static contract between the compiler and any
// host that will run the emitted module (see
// `doc/wasm-compiler-design.md` Appendix A).  It is a serialized
// `celwasm.CelAbi` proto; hosts parse it *before* instantiating the
// module and use it to seed their runtime tables and reject modules
// whose required imports they cannot satisfy.
//
// Scope:
//   - `version` / `cel_source` / `checked` populated from the typed
//     AST via `cel::AstToCheckedExpr`.
//   - `fields` intern table — populated by the select lowering with
//     one entry per distinct `(proto_field_number, name)` pair.
//     Empty for expressions that do not read any fields.
//   - `function_set` records every import the emitted module
//     declares, so a host can diff it against its own impl up
//     front.
//   - `layout` defaults to `initial_pages = 1`, `max_pages = 0`
//     (no cap) — matching `WasmModule::SetMemory` when the codegen
//     MVP installs a memory.
//
// A round-trip test in `abi_test.cc` reads the section back out of
// the serialized `.wasm`, re-parses the `CelAbi`, and asserts every
// field.  That test is the regression harness for any future
// change to either the schema or the writer.

#ifndef CELWASM_COMPILER_CODEGEN_ABI_H_
#define CELWASM_COMPILER_CODEGEN_ABI_H_

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/codegen/cel_abi.pb.h"
#include "compiler/codegen/module.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

// Format version written into `CelAbi.version`.  Bump only when a
// field is removed or its semantics change; additive-only changes
// keep version the same so old hosts still parse successfully.
constexpr uint32_t kCelAbiVersion = 1;

// Custom-section name — the one string every compliant host has to
// recognise.  Kept in a constant because it appears in three
// places: the writer, the reader (in tests and hosts), and the
// design doc.
inline constexpr absl::string_view kCelAbiSectionName = "cel.abi";

// Populates a `CelAbi` from the typed AST plus the original source
// text.  Returns `InvalidArgumentError` if `typed.ast()` cannot be
// converted to a `CheckedExpr` (which would be a cel-cpp-internal
// corruption — should not fire for any AST the frontend returns).
//
// The returned message is entirely owned by the caller; it holds
// no references into `typed` once this function returns.
ABSL_MUST_USE_RESULT absl::StatusOr<CelAbi> BuildCelAbi(
    const TypedAst& typed, absl::string_view cel_source);

// Serializes `abi` and attaches it as a custom section named
// `cel.abi` on `mod`.  Returns `InternalError` if the proto fails
// to serialize (which for M2 payloads should be impossible — there
// are no required fields).
ABSL_MUST_USE_RESULT absl::Status AttachCelAbiSection(WasmModule& mod,
                                                      const CelAbi& abi);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_ABI_H_
