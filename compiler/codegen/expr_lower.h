// Lowers a `TypedAst` into a Binaryen function.
//
// M2 MVP scope: pure arithmetic, boolean logic, comparisons, and the
// ternary `_?_:_`.  The full CEL surface (identifiers, selects, lists,
// maps, proto field access, comprehensions, strings/bytes, macros)
// is not in this MVP and returns `UnimplementedError` for now.
//
// Error-propagation semantics (overflow, divide-by-zero, NaN
// comparisons, unknown) are out of scope; the MVP emits the
// straight-line WASM instructions and defers three-valued logic to M5.

#ifndef CELWASM_COMPILER_CODEGEN_EXPR_LOWER_H_
#define CELWASM_COMPILER_CODEGEN_EXPR_LOWER_H_

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "compiler/codegen/module.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

// The ABI lowering of a CEL `Repr` to a scalar WASM type.  Returns
// `BinaryenTypeNone()` for Reprs that are not represented by a single
// scalar (lists, maps, messages, strings, bytes — those travel as
// pointers or externref slots, which require more than a single type).
BinaryenType WasmTypeFor(Repr r);

struct LoweredFunction {
  // Handle returned by Binaryen; callers can look it up via
  // `BinaryenGetFunction(mod.raw(), name)`.
  BinaryenFunctionRef absl_nonnull func;
  // The WASM scalar type the function returns.
  BinaryenType result_type;
  // The CEL-level Repr that type encodes.
  Repr result_repr;
};

// Lowers `ast.ast().root_expr()` to a nullary function named `func_name`
// and adds it to `mod`.  The function's return type is the ABI
// lowering of the root expression's Repr.
//
// Returns an error when the AST uses a variant outside the MVP subset
// described at the top of this file, or when it uses a Repr that has
// no scalar WASM lowering in M2.
ABSL_MUST_USE_RESULT absl::StatusOr<LoweredFunction> LowerToEvalFunction(
    const TypedAst& ast, absl::string_view func_name, WasmModule& mod);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_EXPR_LOWER_H_
