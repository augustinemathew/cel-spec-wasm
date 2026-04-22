#ifndef CELWASM_COMPILER_V2_CODEGEN_EXPR_LOWER_H_
#define CELWASM_COMPILER_V2_CODEGEN_EXPR_LOWER_H_

// Lowers a fully-resolved, fully-laid-out `TypedAst` into the `$eval`
// wasm function.  M1 handles only the `kConst` arm: every literal's
// value lives at a known rodata offset, so `$eval`'s body is a two-
// instruction block — `call $cel_reset(<arena_base>, <arena_limit>)`
// followed by `i32.const <root_rodata_offset>` — and the function
// returns the root literal's CelValue offset.
//
// Non-kConst expression kinds return `absl::UnimplementedError` naming
// the kind.  This is a designed rejection path, not a stub crash: the
// checker accepts `1 + 2` (a kCall) but M1 compilation of arithmetic
// lands at M3/M4, and the CLI is expected to surface the unimplemented
// status to the user.
//
// Caller responsibilities (M1): before calling `LowerToEvalFunction`
// the caller must have installed memory + the `cel.cel_reset` function
// import on `mod`.  `LowerToEvalFunction` only adds the function
// definition; the export is left to the caller so CLI callers can
// export under a different external name if they choose.

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "compiler_v2/codegen/layout_pass.h"
#include "compiler_v2/codegen/module.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

// The internal-name codegen uses when it emits `BinaryenCall` targeting
// the runtime's `cel_reset`.  Callers that install the import under a
// different internal name will produce a module Binaryen rejects at
// validate time.
inline constexpr absl::string_view kCelResetInternalName = "cel_reset";

struct LoweringOptions {
  // Total linear-memory size in bytes.  `cel_reset` is called with
  // `(arena_base, arena_limit = mem_size_bytes)` at the top of every
  // `$eval` body so the runtime arena spans `[arena_base, mem_size_bytes)`.
  // Default is one wasm page (64 KiB) — enough for M1 (pure literal
  // eval touches only rodata) and the M2/M3 surface covered by the
  // e2e suite.
  uint32_t mem_size_bytes = 64u * 1024u;
};

struct LoweredFunction {
  // Binaryen-owned handle; callers can look the function up later via
  // `BinaryenGetFunction(mod.raw(), name)`.  The function's signature
  // is `() -> i32` — the CelValue offset of the root expression.
  BinaryenFunctionRef absl_nonnull func;
};

// Adds a nullary `$eval` function named `func_name` to `mod`.  The
// function body is a block of type `i32`:
//
//   (block $eval (result i32)
//     (call $cel_reset (i32.const <arena_base>) (i32.const <arena_limit>))
//     (i32.const <root_rodata_offset>))
//
// Fails with `UnimplementedError` for any expression kind outside the
// M1 subset (kConst only).  Fails with `InvalidArgumentError` if the
// root expression has no storage annotation (LayoutPass was skipped)
// or its storage is not `kStaticRodata` (impossible at M1 but checked
// defensively — a later milestone accidentally flowing a workspace
// offset through here would otherwise emit a subtly wrong i32.const).
ABSL_MUST_USE_RESULT absl::StatusOr<LoweredFunction> LowerToEvalFunction(
    const TypedAst& ast, const StaticLayout& layout,
    absl::string_view func_name, WasmModule& mod,
    const LoweringOptions& opts = {});

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CODEGEN_EXPR_LOWER_H_
