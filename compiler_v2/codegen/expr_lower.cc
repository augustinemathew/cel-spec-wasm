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

// Shared per-eval emission context — threaded through every `Emit`
// call so kSelect can recurse into its operand and append field-ref
// rows without signature churn.
struct EmitCtx {
  WasmModule& mod;
  const TypedAst& ast;
  const StaticLayout& layout;
  std::vector<FieldRefRow>& field_refs;
};

absl::StatusOr<BinaryenExpressionRef> Emit(EmitCtx& ctx, const cel::Expr& expr);

// Lookup the fully-qualified name of the message type of `expr`'s
// static type, or "" if it isn't a message.  `cel.abi.fields[].
// owner_fqn` uses this at Plan time to resolve the descriptor the
// backing will dispatch through.
std::string MessageTypeFqn(const TypedAst& ast, const cel::Expr& expr) {
  const auto& type_map = ast.ast().type_map();
  auto it = type_map.find(expr.id());
  if (it == type_map.end() || !it->second.has_message_type()) return "";
  return it->second.message_type().type();
}

absl::StatusOr<BinaryenExpressionRef> EmitKSelect(EmitCtx& ctx,
                                                  const cel::Expr& expr,
                                                  const cel::SelectExpr& sel,
                                                  const NodeAnnotation& ann) {
  // Recurse into operand — its block/leaf produces an i32 that is
  // the wasm-memory offset of the operand's CelValue (workspace
  // slot for a nested select, the variable's slot for an ident).
  auto operand_or = Emit(ctx, sel.operand());
  if (!operand_or.ok()) return operand_or.status();

  ABSL_CHECK(ann.storage.kind == StorageKind::kWorkspaceSlot)
      << "expr_lower: kSelect expr_id=" << expr.id()
      << " has non-workspace storage (LayoutPass didn't allocate a slot)";
  const uint32_t out_slot = ann.storage.payload;

  // Append a new field-ref row.  `field_refs.size()` at this point
  // is also the dense id for the new row (the sentinel at index 0
  // is pushed once in LowerToEvalFunction before the walk starts).
  const auto field_ref_id = static_cast<uint32_t>(ctx.field_refs.size());
  ctx.field_refs.push_back(FieldRefRow{
      /*field_number=*/ann.field_number,
      /*name=*/sel.field(),
      /*owner_fqn=*/MessageTypeFqn(ctx.ast, sel.operand()),
  });

  // attribute_id is the OPERAND's attribute id — the trampoline
  // looks up that path and appends `sel.field()` at runtime.  0
  // when the operand isn't path-bearing (literal, kCall, …); the
  // trampoline treats 0 as "no unknown-pattern match possible".
  const NodeAnnotation* op_ann =
      ctx.layout.annotations.Find(sel.operand().id());
  const uint32_t attribute_id = op_ann != nullptr ? op_ann->attribute_id : 0;
  BinaryenExpressionRef args[4] = {
      I32Const(ctx.mod, out_slot),
      *operand_or,
      I32Const(ctx.mod, field_ref_id),
      I32Const(ctx.mod, attribute_id),
  };
  // test_only=true routes to cel_has_field (returns Bool);
  // otherwise cel_get_field (returns the field's CelValue).
  const absl::string_view target_view = sel.test_only()
                                            ? kCelHostHasFieldInternalName
                                            : kCelHostGetFieldInternalName;
  const std::string internal_name(target_view);
  BinaryenExpressionRef call = BinaryenCall(
      ctx.mod.raw(), internal_name.c_str(), args, 4, BinaryenTypeNone());

  // Wrap (call, i32.const out_slot) in a block whose value is the
  // out_slot i32 — usable as msg_slot of a parent kSelect.
  BinaryenExpressionRef block_items[2] = {call, I32Const(ctx.mod, out_slot)};
  return BinaryenBlock(ctx.mod.raw(), /*name=*/nullptr, block_items, 2,
                       BinaryenTypeInt32());
}

// Emits `(call $cel.cel_map_create (i32.const out_slot) (i32.const N))`.
// out_slot is the workspace offset LayoutPass assigned to the
// kMapExpr; N is the entry count, used to size the arena allocation.
BinaryenExpressionRef EmitCelMapCreateCall(WasmModule& mod, uint32_t out_slot,
                                           uint32_t capacity) {
  BinaryenExpressionRef args[2] = {I32Const(mod, out_slot),
                                   I32Const(mod, capacity)};
  const std::string name(kCelMapCreateInternalName);
  return BinaryenCall(mod.raw(), name.c_str(), args, 2, BinaryenTypeNone());
}

// Emits `(call $cel.cel_map_insert <out_slot> <key_expr> <value_expr>)`.
// `key_expr` and `value_expr` are i32-valued sub-expressions whose
// values at runtime are linear-memory offsets of the key/value's
// CelValue (rodata for kConst, workspace slot for everything else).
BinaryenExpressionRef EmitCelMapInsertCall(WasmModule& mod, uint32_t out_slot,
                                           BinaryenExpressionRef key,
                                           BinaryenExpressionRef value) {
  BinaryenExpressionRef args[3] = {I32Const(mod, out_slot), key, value};
  const std::string name(kCelMapInsertInternalName);
  return BinaryenCall(mod.raw(), name.c_str(), args, 3, BinaryenTypeNone());
}

// Lowers a kMapExpr to:
//   (call $cel.cel_map_create out_slot N)
//   for each entry:
//     <eval key>           -> i32 key_offset
//     <eval value>         -> i32 value_offset
//     (call $cel.cel_map_insert out_slot key_offset value_offset)
//   (i32.const out_slot)
// wrapped in a (block (result i32)) whose value is `out_slot`.
absl::StatusOr<BinaryenExpressionRef> EmitKMapExpr(EmitCtx& ctx,
                                                   const cel::Expr& expr,
                                                   const cel::MapExpr& m,
                                                   const NodeAnnotation& ann) {
  ABSL_CHECK(ann.storage.kind == StorageKind::kWorkspaceSlot)
      << "expr_lower: kMapExpr expr_id=" << expr.id()
      << " has non-workspace storage (LayoutPass didn't allocate a slot)";
  const uint32_t out_slot = ann.storage.payload;
  const auto N = static_cast<uint32_t>(m.entries().size());

  std::vector<BinaryenExpressionRef> instrs;
  instrs.reserve(2u + 3u * N);
  instrs.push_back(EmitCelMapCreateCall(ctx.mod, out_slot, N));

  for (const cel::MapExprEntry& e : m.entries()) {
    ABSL_CHECK(!e.optional())
        << "expr_lower: kMapExpr expr_id=" << expr.id()
        << " entry id=" << e.id() << " is optional — stub until M5";
    auto key_or = Emit(ctx, e.key());
    if (!key_or.ok()) return key_or.status();
    auto val_or = Emit(ctx, e.value());
    if (!val_or.ok()) return val_or.status();
    instrs.push_back(EmitCelMapInsertCall(ctx.mod, out_slot, *key_or, *val_or));
  }

  // Block-trailer i32 yields the map's slot offset for parent
  // expressions (e.g. an index call or a return).
  instrs.push_back(I32Const(ctx.mod, out_slot));
  return BinaryenBlock(ctx.mod.raw(), /*name=*/nullptr, instrs.data(),
                       static_cast<BinaryenIndex>(instrs.size()),
                       BinaryenTypeInt32());
}

// Selects the runtime entry point for a `_[_]` indexing call based
// on the operand's `map_origin`.  M3.F three-path dispatch (per
// `map-list-dispatch.md` §2.6 / m3-map-literals.md §2.9):
//   kArena    → cel.cel_map_lookup_arena (pure wasm fast path)
//   kHost     → cel_host.cel_map_lookup  (host trampoline)
//   kDynamic  → cel.cel_map_lookup       (the runtime dispatcher)
absl::string_view MapLookupCallTarget(Origin origin) {
  switch (origin) {
    case Origin::kArena:
      return kCelMapLookupArenaInternalName;
    case Origin::kHost:
      return kCelHostMapLookupInternalName;
    case Origin::kDynamic:
      return kCelMapLookupInternalName;
  }
  ABSL_CHECK(false) << "expr_lower: unknown map Origin "
                    << static_cast<int>(origin);
}

// M4.F: same shape as MapLookupCallTarget but for list indexing.
//   kArena    → cel.cel_list_at_arena (pure wasm fast path)
//   kHost     → cel_host.cel_list_at  (host trampoline)
//   kDynamic  → cel.cel_list_at       (the runtime dispatcher)
absl::string_view ListAtCallTarget(Origin origin) {
  switch (origin) {
    case Origin::kArena:
      return kCelListAtArenaInternalName;
    case Origin::kHost:
      return kCelHostListAtInternalName;
    case Origin::kDynamic:
      return kCelListAtInternalName;
  }
  ABSL_CHECK(false) << "expr_lower: unknown list Origin "
                    << static_cast<int>(origin);
}

// Emits `(call $cel.cel_list_create (i32.const out_slot) (i32.const N))`.
BinaryenExpressionRef EmitCelListCreateCall(WasmModule& mod, uint32_t out_slot,
                                            uint32_t count) {
  BinaryenExpressionRef args[2] = {I32Const(mod, out_slot),
                                   I32Const(mod, count)};
  const std::string name(kCelListCreateInternalName);
  return BinaryenCall(mod.raw(), name.c_str(), args, 2, BinaryenTypeNone());
}

// Emits `(call $cel.cel_list_set <list_slot> (i32.const index) <elem_expr>)`.
// `elem_expr` is an i32-valued sub-expression whose value at runtime
// is the linear-memory offset of the element's CelValue.
BinaryenExpressionRef EmitCelListSetCall(WasmModule& mod, uint32_t list_slot,
                                         uint32_t index,
                                         BinaryenExpressionRef elem) {
  BinaryenExpressionRef args[3] = {I32Const(mod, list_slot),
                                   I32Const(mod, index), elem};
  const std::string name(kCelListSetInternalName);
  return BinaryenCall(mod.raw(), name.c_str(), args, 3, BinaryenTypeNone());
}

// Lowers a kListExpr to:
//   (call $cel.cel_list_create out_slot N)
//   for i in [0, N):
//     <eval element>      -> i32 elem_offset
//     (call $cel.cel_list_set out_slot i elem_offset)
//   (i32.const out_slot)
// wrapped in a (block (result i32)) whose value is `out_slot`.
absl::StatusOr<BinaryenExpressionRef> EmitKListExpr(EmitCtx& ctx,
                                                    const cel::Expr& expr,
                                                    const cel::ListExpr& l,
                                                    const NodeAnnotation& ann) {
  ABSL_CHECK(ann.storage.kind == StorageKind::kWorkspaceSlot)
      << "expr_lower: kListExpr expr_id=" << expr.id()
      << " has non-workspace storage (LayoutPass didn't allocate a slot)";
  const uint32_t out_slot = ann.storage.payload;
  const auto N = static_cast<uint32_t>(l.elements().size());

  std::vector<BinaryenExpressionRef> instrs;
  instrs.reserve(2u + N);
  instrs.push_back(EmitCelListCreateCall(ctx.mod, out_slot, N));

  for (uint32_t i = 0; i < N; ++i) {
    const cel::ListExprElement& e = l.elements()[i];
    ABSL_CHECK(!e.optional())
        << "expr_lower: kListExpr expr_id=" << expr.id()
        << " element index=" << i << " is optional — stub until M5";
    auto elem_or = Emit(ctx, e.expr());
    if (!elem_or.ok()) return elem_or.status();
    instrs.push_back(EmitCelListSetCall(ctx.mod, out_slot, i, *elem_or));
  }

  instrs.push_back(I32Const(ctx.mod, out_slot));
  return BinaryenBlock(ctx.mod.raw(), /*name=*/nullptr, instrs.data(),
                       static_cast<BinaryenIndex>(instrs.size()),
                       BinaryenTypeInt32());
}

absl::StatusOr<BinaryenExpressionRef> EmitKIndexCall(EmitCtx& ctx,
                                                     const cel::Expr& expr,
                                                     const cel::CallExpr& call,
                                                     const NodeAnnotation& ann) {
  // CEL's `_[_]` is a binary operator: args[0] = collection,
  // args[1] = index.  `target` (receiver) form is not used at M3 —
  // the parser materialises the operand as args[0].
  ABSL_CHECK(call.args().size() == 2)
      << "expr_lower: `_[_]` expr_id=" << expr.id() << " has "
      << call.args().size() << " args (expected 2)";
  const cel::Expr& operand_expr = call.args()[0];
  const cel::Expr& key_expr = call.args()[1];

  auto operand_or = Emit(ctx, operand_expr);
  if (!operand_or.ok()) return operand_or.status();
  auto key_or = Emit(ctx, key_expr);
  if (!key_or.ok()) return key_or.status();

  ABSL_CHECK(ann.storage.kind == StorageKind::kWorkspaceSlot)
      << "expr_lower: kCallExpr(_[_]) expr_id=" << expr.id()
      << " has non-workspace storage (LayoutPass didn't allocate a slot)";
  const uint32_t out_slot = ann.storage.payload;

  // Origin comes from the OPERAND, not the call: a kSelect over a
  // proto map field stamps `kHost` on its own annotation, and `m[k]`
  // reads that origin off `m` (operand) — same shape M2 uses for
  // attribute-id propagation.  The operand's repr selects which
  // origin field (map vs list) and which dispatch table to use.
  const NodeAnnotation* op_ann = ctx.layout.annotations.Find(operand_expr.id());
  ABSL_CHECK(op_ann != nullptr)
      << "expr_lower: kCallExpr(_[_]) expr_id=" << expr.id()
      << " operand has no NodeAnnotation";
  ABSL_CHECK(op_ann->repr == Repr::kMap || op_ann->repr == Repr::kList)
      << "expr_lower: kCallExpr(_[_]) expr_id=" << expr.id()
      << " operand repr=" << ReprName(op_ann->repr)
      << " — only map / list operands supported (checker should have rejected)";
  const Origin origin =
      (op_ann->repr == Repr::kList) ? op_ann->list_origin : op_ann->map_origin;
  const std::string target((op_ann->repr == Repr::kList)
                               ? ListAtCallTarget(origin)
                               : MapLookupCallTarget(origin));

  BinaryenExpressionRef args[3] = {I32Const(ctx.mod, out_slot), *operand_or,
                                   *key_or};
  BinaryenExpressionRef call_expr = BinaryenCall(
      ctx.mod.raw(), target.c_str(), args, 3, BinaryenTypeNone());

  BinaryenExpressionRef block_items[2] = {call_expr, I32Const(ctx.mod, out_slot)};
  return BinaryenBlock(ctx.mod.raw(), /*name=*/nullptr, block_items, 2,
                       BinaryenTypeInt32());
}

// Emits an i32-valued expression for any supported kind; the value is
// always the linear-memory offset of that expression's CelValue.
absl::StatusOr<BinaryenExpressionRef> Emit(EmitCtx& ctx,
                                           const cel::Expr& expr) {
  const NodeAnnotation* ann = ctx.layout.annotations.Find(expr.id());
  if (ann == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("expr_lower: expr id ", expr.id(),
                     " has no NodeAnnotation — LayoutPass was not run"));
  }
  switch (expr.kind_case()) {
    case cel::ExprKindCase::kConstant:
      return EmitKConstLoad(ctx.mod, *ann);
    case cel::ExprKindCase::kIdentExpr:
      return EmitKIdentLoad(ctx.mod, *ann);
    case cel::ExprKindCase::kSelectExpr:
      return EmitKSelect(ctx, expr, expr.select_expr(), *ann);
    case cel::ExprKindCase::kCallExpr:
      // M3.F: only the indexing operator `_[_]` is lowered.  Other
      // kCall variants (arithmetic, comparison, logical, custom)
      // land at later milestones; surface as Unimplemented so the
      // pipeline rejects deterministically rather than silently
      // dropping the call.
      if (expr.call_expr().function() == "_[_]") {
        return EmitKIndexCall(ctx, expr, expr.call_expr(), *ann);
      }
      return Unimplemented(expr.kind_case(), expr.id());
    case cel::ExprKindCase::kMapExpr:
      return EmitKMapExpr(ctx, expr, expr.map_expr(), *ann);
    case cel::ExprKindCase::kListExpr:
      return EmitKListExpr(ctx, expr, expr.list_expr(), *ann);
    case cel::ExprKindCase::kStructExpr:
    case cel::ExprKindCase::kComprehensionExpr:
    case cel::ExprKindCase::kUnspecifiedExpr:
      return Unimplemented(expr.kind_case(), expr.id());
  }
  ABSL_CHECK(false) << "Emit: unknown ExprKindCase "
                    << static_cast<int>(expr.kind_case());
}

}  // namespace

absl::StatusOr<LoweredFunction> LowerToEvalFunction(
    const TypedAst& ast, const StaticLayout& layout,
    absl::string_view func_name, WasmModule& mod, const LoweringOptions& opts) {
  ABSL_CHECK(ast.has_ast())
      << "LowerToEvalFunction: TypedAst has no checked cel::Ast";

  // field_refs[0] is the reserved "not proto-resolvable" sentinel;
  // subsequent rows are pushed by EmitKSelect as the walk emits each
  // select.
  std::vector<FieldRefRow> field_refs;
  field_refs.push_back(FieldRefRow{});
  EmitCtx ctx{mod, ast, layout, field_refs};

  auto root_ref = Emit(ctx, ast.ast().root_expr());
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
  return LoweredFunction{func, std::move(field_refs)};
}

}  // namespace celwasm
