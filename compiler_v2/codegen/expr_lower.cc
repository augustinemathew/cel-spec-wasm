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
                   "` is not supported yet (expr id ", id, ")"));
}

// Helper: wrap an i32 literal offset as a Binaryen `(i32.const <n>)`.
BinaryenExpressionRef I32Const(WasmModule& mod, uint32_t value) {
  return BinaryenConst(mod.raw(),
                       BinaryenLiteralInt32(static_cast<int32_t>(value)));
}

// Emits a rodata-offset `(i32.const <off>)`.  The only storage kind
// for kConst is `kStaticRodata`; anything else on a kConst node is an
// invariant violation (LayoutPass didn't pack the literal).
BinaryenExpressionRef EmitKConstLoad(WasmModule& mod,
                                     const NodeAnnotation& ann) {
  ABSL_CHECK(ann.storage.kind == StorageKind::kStaticRodata)
      << "expr_lower: kConst node has storage kind "
      << static_cast<int>(ann.storage.kind)
      << "; expected kStaticRodata (LayoutPass didn't pack the literal)";
  return I32Const(mod, ann.storage.payload);
}

// Emits `(local.get <local_index>)`.  The local's value at runtime is
// the u32 offset of the variable's CelValue cell in linear memory —
// set once per Eval by the `$eval` prelude (free variable), or per
// iteration by the loop header (comprehension iter var, M5).  The
// kIdent read site is identical across both cases.
BinaryenExpressionRef EmitKIdentLoad(WasmModule& mod,
                                     const NodeAnnotation& ann) {
  ABSL_CHECK(ann.storage.kind == StorageKind::kLocal)
      << "expr_lower: kIdent node has storage kind "
      << static_cast<int>(ann.storage.kind)
      << "; expected kLocal (LayoutPass didn't tag the ident)";
  return BinaryenLocalGet(mod.raw(), ann.storage.payload, BinaryenTypeInt32());
}

// Emits `call $cel_reset(arena_base, arena_limit)`.  First instruction
// of every `$eval` body after the variable prelude: writes the arena
// cursor/limit pair to linear-memory bytes 8/12, giving each eval a
// fresh arena.  Both arguments are compile-time constants.
BinaryenExpressionRef EmitCelResetCall(WasmModule& mod, uint32_t arena_base,
                                       uint32_t arena_limit) {
  BinaryenExpressionRef args[2] = {
      I32Const(mod, arena_base),
      I32Const(mod, arena_limit),
  };
  const std::string name(kCelResetInternalName);
  return BinaryenCall(mod.raw(), name.c_str(), args, 2, BinaryenTypeNone());
}

// Emits one `local.set local_index (i32.const slot_offset)` per
// referenced variable, populating each ident's wasm local with its
// compile-time-known workspace slot offset before `cel_reset` or the
// body run.  Per m2-ident-select-unknowns.md §2.6 / Slice M2.B:
// every kIdent lowering is `local.get local_index`, so the prelude is
// the one place where the "which slot?" question gets answered for
// free variables.
//
// The resulting instructions go at the top of `$eval`, before
// `cel_reset`, because cel_reset only writes bytes [8, 16) (arena
// cursor/limit) and doesn't touch the workspace region — so the order
// prelude-then-reset vs reset-then-prelude is irrelevant at runtime.
// We put prelude first so the generated WAT reads top-down matching
// the milestone plan doc's sketch.
std::vector<BinaryenExpressionRef> EmitVariablePrelude(
    WasmModule& mod, absl::Span<const LaidOutVariable> variables) {
  std::vector<BinaryenExpressionRef> out;
  out.reserve(variables.size());
  for (const LaidOutVariable& v : variables) {
    out.push_back(BinaryenLocalSet(mod.raw(), v.local_index,
                                   I32Const(mod, v.slot_offset)));
  }
  return out;
}

// Emits the instruction that supplies `$eval`'s return i32 — the
// offset of the root expression's CelValue.  Dispatches on the root's
// kind; non-M2 kinds fail with Unimplemented.
absl::StatusOr<BinaryenExpressionRef> EmitRoot(WasmModule& mod,
                                               const cel::Expr& root,
                                               const NodeAnnotation& ann) {
  switch (root.kind_case()) {
    case cel::ExprKindCase::kConstant:
      return EmitKConstLoad(mod, ann);
    case cel::ExprKindCase::kIdentExpr:
      return EmitKIdentLoad(mod, ann);
    case cel::ExprKindCase::kSelectExpr:
    case cel::ExprKindCase::kCallExpr:
    case cel::ExprKindCase::kListExpr:
    case cel::ExprKindCase::kStructExpr:
    case cel::ExprKindCase::kMapExpr:
    case cel::ExprKindCase::kComprehensionExpr:
    case cel::ExprKindCase::kUnspecifiedExpr:
      return Unimplemented(root.kind_case(), root.id());
  }
  ABSL_CHECK(false) << "EmitRoot: unknown ExprKindCase "
                    << static_cast<int>(root.kind_case());
}

}  // namespace

absl::StatusOr<LoweredFunction> LowerToEvalFunction(
    const TypedAst& ast, const StaticLayout& layout,
    absl::string_view func_name, WasmModule& mod, const LoweringOptions& opts) {
  ABSL_CHECK(ast.has_ast())
      << "LowerToEvalFunction: TypedAst has no checked cel::Ast";

  const cel::Expr& root = ast.ast().root_expr();
  const NodeAnnotation* ann = layout.annotations.Find(root.id());
  if (ann == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("expr_lower: root expr id ", root.id(),
                     " has no NodeAnnotation — LayoutPass was not run"));
  }

  auto root_ref = EmitRoot(mod, root, *ann);
  if (!root_ref.ok()) return root_ref.status();

  // `$eval` body shape:
  //   (block (result i32)
  //     <prelude: one local.set per referenced variable>
  //     (call $cel_reset arena_base mem_size)
  //     <root expression>)
  //
  // The block's last expression supplies its return value, so the
  // root expression's i32 is what `$eval` returns.  Prelude +
  // cel_reset have `none` result type and contribute nothing to the
  // block's value.
  std::vector<BinaryenExpressionRef> instrs =
      EmitVariablePrelude(mod, layout.variables);
  instrs.push_back(
      EmitCelResetCall(mod, layout.arena_base, opts.mem_size_bytes));
  instrs.push_back(*root_ref);
  BinaryenExpressionRef body = BinaryenBlock(
      mod.raw(), /*name=*/nullptr, instrs.data(),
      static_cast<BinaryenIndex>(instrs.size()), BinaryenTypeInt32());

  // Every wasm local `$eval` carries is a u32 memory offset — one
  // per referenced variable (m2-ident-select-unknowns.md §2.6 /
  // Slice M2.B).  Build the per-local type vector at emission time;
  // the layout carries only the count via `variables.size()`.
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
