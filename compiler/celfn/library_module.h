#ifndef CELWASM_COMPILER_V2_CELFN_LIBRARY_MODULE_H_
#define CELWASM_COMPILER_V2_CELFN_LIBRARY_MODULE_H_

// Compiles the CEL-defined function bodies in a `FunctionLibrary` into
// a single wasm module exporting each body under its `overload_id`.
// This is the producer side of the M13 CEL-defined backend (§4.4 of
// `doc/implementation-plan/rewrite/m13-custom-fns.md`): the bundled
// module imports `cel.memory` from the engine and one helper import
// per built-in / host / sibling-call the bodies reach, and exports
// one `(out_slot, *arg_slots) → ()` wasm function per CEL-defined
// decl.
//
// `Engine::Plan` registers the produced bytes under the library's
// `module_name()` so the caller expression's
// `(import "<module_name>" "<overload_id>" ...)` resolves into this
// bundled module.

#include <cstdint>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "compiler/celfn/function_library.h"
#include "compiler/internal/compile.h"

namespace celwasm {

// Compiles every `kCelDefined` decl in `lib` and bundles the lowered
// functions into one wasm module.  Returns the serialised module
// bytes.
//
// `parent_opts` carries the descriptor pool source, the container
// override, the optimize level, and the memory size — every body
// shares those settings with the caller expression so a `proto(...)`
// param resolves against the same descriptor pool the caller used.
//
// Pre-conditions: `lib.module_name()` is non-empty (a CEL-defined
// fn must come from a `Module foo;` directive); `lib` carries at
// least one `kCelDefined` decl.  Both are validated and surface as
// `FailedPrecondition` if violated.
//
// Returns `InvalidArgument` for any body that fails to type-check
// against its declared signature (the parameter set + return type
// drawn from `CelfnDecl`).
ABSL_MUST_USE_RESULT absl::StatusOr<std::vector<uint8_t>> CompileLibraryBodies(
    const FunctionLibrary& lib, const CompileOptions& parent_opts);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CELFN_LIBRARY_MODULE_H_
