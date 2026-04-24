#include "compiler_v2/codegen/expr_lower.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "common/expr.h"
#include "compiler_v2/codegen/layout_pass.h"
#include "compiler_v2/codegen/module.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

namespace {

// Human-readable name for an `ExprKindCase`, used in unimplemented
// diagnostics so the error points at the offending kind by name rather
// than an integer enum value.
absl::string_view ExprKindName(cel::ExprKindCase k) {
  switch (k) {
    case cel::ExprKindCase::kUnspecifiedExpr:
      return "unspecified";
    case cel::ExprKindCase::kConstant:
      return "constant";
    case cel::ExprKindCase::kIdentExpr:
      return "ident";
    case cel::ExprKindCase::kSelectExpr:
      return "select";
    case cel::ExprKindCase::kCallExpr:
      return "call";
    case cel::ExprKindCase::kListExpr:
      return "list";
    case cel::ExprKindCase::kStructExpr:
      return "struct";
    case cel::ExprKindCase::kMapExpr:
      return "map";
    case cel::ExprKindCase::kComprehensionExpr:
      return "comprehension";
  }
  // Closed enum; reaching here means cel-cpp grew a variant we haven't
  // mirrored.  Crash loudly per CLAUDE.md — a silent fallback here
  // would miscompile on a new kind.
  ABSL_CHECK(false) << "ExprKindName: unknown ExprKindCase "
                    << static_cast<int>(k);
  return "<unreachable>";
}

absl::Status Unimplemented(cel::ExprKindCase kind, int64_t id) {
  return absl::UnimplementedError(
      absl::StrCat("expr_lower: expression kind `", ExprKindName(kind),
                   "` is not supported at M1 (expr id ", id, ")"));
}

// Loads the root's rodata offset as an `i32.const`.  M1's only storage
// kind is `kStaticRodata`, so every kConst node lowers to a single
// constant instruction.
BinaryenExpressionRef EmitStorageLoad(WasmModule& mod, const Storage& storage) {
  ABSL_CHECK(storage.kind == StorageKind::kStaticRodata)
      << "expr_lower: EmitStorageLoad called with storage kind "
      << static_cast<int>(storage.kind) << "; M1 only emits kStaticRodata";
  return BinaryenConst(
      mod.raw(), BinaryenLiteralInt32(static_cast<int32_t>(storage.payload)));
}

// Emits `call $cel_reset(arena_base, arena_limit)`.  This is the first
// instruction of every `$eval` body: it writes the arena cursor/limit
// pair to linear-memory bytes 8/12, giving each eval a fresh arena.
// Codegen bakes both arguments as compile-time constants.
BinaryenExpressionRef EmitCelResetCall(WasmModule& mod, uint32_t arena_base,
                                       uint32_t arena_limit) {
  BinaryenExpressionRef args[2] = {
      BinaryenConst(mod.raw(),
                    BinaryenLiteralInt32(static_cast<int32_t>(arena_base))),
      BinaryenConst(mod.raw(),
                    BinaryenLiteralInt32(static_cast<int32_t>(arena_limit))),
  };
  const std::string name(kCelResetInternalName);
  return BinaryenCall(mod.raw(), name.c_str(), args, 2, BinaryenTypeNone());
}

}  // namespace

absl::StatusOr<LoweredFunction> LowerToEvalFunction(
    const TypedAst& ast, const StaticLayout& layout,
    absl::string_view func_name, WasmModule& mod, const LoweringOptions& opts) {
  ABSL_CHECK(ast.has_ast())
      << "LowerToEvalFunction: TypedAst has no checked cel::Ast";

  const cel::Expr& root = ast.ast().root_expr();
  const cel::ExprKindCase kind = root.kind_case();

  // M1 gate: only kConstant roots are lowered.  Non-root kConst (e.g.
  // operands of a kCall) have been packed into rodata by LayoutPass,
  // but the root's kind is what determines whether `$eval` has a
  // codegen strategy at all.
  if (kind != cel::ExprKindCase::kConstant) {
    return Unimplemented(kind, root.id());
  }

  const NodeAnnotation* ann = layout.annotations.Find(root.id());
  if (ann == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("expr_lower: root expr id ", root.id(),
                     " has no NodeAnnotation — LayoutPass was not run"));
  }
  if (ann->storage.kind != StorageKind::kStaticRodata) {
    return absl::InvalidArgumentError(absl::StrCat(
        "expr_lower: root kConst node id ", root.id(), " has storage kind ",
        static_cast<int>(ann->storage.kind), "; M1 expects kStaticRodata"));
  }

  // Build `(block (result i32) (call $cel_reset ...) (i32.const <off>))`.
  // The block's last expression supplies its value, so the i32.const is
  // what `$eval` returns.
  BinaryenExpressionRef children[2] = {
      EmitCelResetCall(mod, layout.arena_base, opts.mem_size_bytes),
      EmitStorageLoad(mod, ann->storage),
  };
  BinaryenExpressionRef body =
      BinaryenBlock(mod.raw(), /*name=*/nullptr, children, /*numChildren=*/2,
                    BinaryenTypeInt32());

  // Every wasm local `$eval` carries is a u32 memory offset — one
  // per referenced variable (m2-ident-select-unknowns.md §2.6 /
  // Slice M2.B).  Build the per-local type vector at emission
  // time; the layout only carries the count via `variables.size()`.
  const std::string func_name_c(func_name);
  const std::vector<BinaryenType> local_types(layout.variables.size(),
                                              BinaryenTypeInt32());
  mod.AddFunction(func_name, /*params=*/{}, BinaryenTypeInt32(), local_types,
                  body);

  BinaryenFunctionRef func =
      BinaryenGetFunction(mod.raw(), func_name_c.c_str());
  ABSL_CHECK(func != nullptr)
      << "expr_lower: Binaryen did not register function `" << func_name
      << "` after AddFunction";
  return LoweredFunction{func};
}

}  // namespace celwasm
