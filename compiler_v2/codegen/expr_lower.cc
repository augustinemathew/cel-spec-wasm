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
#include "compiler_v2/codegen/overload_table.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

namespace {

// M5.D step 2 (shipped) — every aggregate-op dispatcher
// (`cel_list_size` / `cel_list_in` / `cel_list_eq` / `cel_list_concat`
// / `cel_map_size` / `cel_map_in` / `cel_map_eq`) now has a runtime
// export AND a kHost trampoline, so codegen can emit calls into them
// freely.  The `kPendingRuntimeExports` guard that previously
// gated those names was deleted at M5.D step 2 ship; if a future
// dispatcher needs a similar gate, reinstate the pattern here.

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
  // M5.F: looked up at every general-arm `kCallExpr` to map the
  // resolved cel-cpp `overload_id` (e.g. `add_int64`) onto the
  // wasm helper this codegen emits a `BinaryenCall` to.
  const OverloadTable& overload_table;
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

// M7.A: emits `(call $cel_host.cel_make_message (i32.const type_id)
// (i32.const out_slot))`.  Two i32 args, void result.  The trampoline
// resolves type_id → Descriptor* against the per-Instance lookup
// table (populated from `cel.abi.types[]` at Plan time), allocates a
// default-constructed proto via MessageFactory::GetPrototype()->New(),
// wraps in an owning HostMessageBacking, interns into the
// ExternrefTable, and writes a CEL_MESSAGE CelValue with the interned
// msg_slot to the out_slot cell.
BinaryenExpressionRef EmitCelMakeMessageCall(WasmModule& mod,
                                             uint32_t type_id,
                                             uint32_t out_slot) {
  BinaryenExpressionRef args[2] = {I32Const(mod, type_id),
                                   I32Const(mod, out_slot)};
  const std::string name(kCelHostMakeMessageInternalName);
  return BinaryenCall(mod.raw(), name.c_str(), args, 2, BinaryenTypeNone());
}

// M7.B: emits `(call $cel_host.cel_set_field (i32.const msg_slot)
// (i32.const field_ref_id) <value_expr>)`.  `value_expr` is an
// i32-valued sub-expression whose value at runtime is the linear-
// memory offset of the entry's value CelValue (rodata for kConst,
// workspace slot for everything else).  The trampoline reads
// value_slot's CelValue, resolves the FieldDescriptor by
// (field_ref_id → name + owner_fqn), and dispatches on cpp_type to
// pick the Reflection setter; no out_slot since the mutation
// happens in-place on the OwnedProtoBacking carried by msg_slot.
BinaryenExpressionRef EmitCelSetFieldCall(WasmModule& mod, uint32_t msg_slot,
                                          uint32_t field_ref_id,
                                          BinaryenExpressionRef value) {
  BinaryenExpressionRef args[3] = {I32Const(mod, msg_slot),
                                   I32Const(mod, field_ref_id), value};
  const std::string name(kCelHostSetFieldInternalName);
  return BinaryenCall(mod.raw(), name.c_str(), args, 3, BinaryenTypeNone());
}

// Lowers a kStructExpr to:
//   (call $cel_host.cel_make_message (i32.const type_id) (i32.const out_slot))
//   for each entry e:
//     <eval e.value>           -> i32 value_slot
//     (call $cel_host.cel_set_field (i32.const out_slot)
//                                   (i32.const field_ref_id)
//                                   <value_slot>)
//   (i32.const out_slot)
// wrapped in a (block (result i32)) whose value is `out_slot`.
//
// M7.A: empty literal — entry loop is a no-op.
// M7.B: scalar entry-set — Layer-2 dispatches per-cpp_type;
//       repeated/map/message singular fields trap at the
//       trampoline (M7.C/E will fill them in).  No codegen-time
//       gating per field type — codegen here doesn't know the
//       descriptor's shape; it trusts the checker for type
//       compatibility and the trampoline to surface unsupported
//       shapes as clean traps.
absl::StatusOr<BinaryenExpressionRef> EmitKStructExpr(
    EmitCtx& ctx, const cel::Expr& expr, const cel::StructExpr& s,
    const NodeAnnotation& ann) {
  ABSL_CHECK(!s.name().empty())
      << "expr_lower: kStructExpr expr_id=" << expr.id()
      << " has empty name() — should have lowered as kMapExpr at parse "
         "time per design.md §4.7.4";
  ABSL_CHECK(ann.storage.kind == StorageKind::kWorkspaceSlot)
      << "expr_lower: kStructExpr expr_id=" << expr.id()
      << " has non-workspace storage (LayoutPass didn't allocate a slot)";
  ABSL_CHECK(ann.message_type_id != 0)
      << "expr_lower: kStructExpr expr_id=" << expr.id()
      << " has message_type_id=0 — ResolvePass MessageTypeIdVisitor "
         "didn't intern the FQN (m7-proto-literals.md §4.2)";

  const uint32_t out_slot = ann.storage.payload;
  std::vector<BinaryenExpressionRef> instrs;
  instrs.reserve(2u + s.fields().size());
  instrs.push_back(
      EmitCelMakeMessageCall(ctx.mod, ann.message_type_id, out_slot));

  for (const cel::StructExprField& f : s.fields()) {
    ABSL_CHECK(!f.optional())
        << "expr_lower: kStructExpr expr_id=" << expr.id() << " field `"
        << f.name() << "` is optional — stub until optionals slice";
    auto value_or = Emit(ctx, f.value());
    if (!value_or.ok()) return value_or.status();
    // Append a fresh field-ref row.  M7.B emits field_number=0
    // so the host resolves the FieldDescriptor by name against
    // the bound message — matches the read-side fallback path
    // ProtoBacking::ResolveFieldDescriptor already uses for
    // non-proto backings (m2-ident-select-unknowns.md §2.4).
    const auto field_ref_id = static_cast<uint32_t>(ctx.field_refs.size());
    ctx.field_refs.push_back(FieldRefRow{
        /*field_number=*/0,
        /*name=*/f.name(),
        /*owner_fqn=*/s.name(),
    });
    instrs.push_back(EmitCelSetFieldCall(ctx.mod, out_slot, field_ref_id,
                                         *value_or));
  }

  instrs.push_back(I32Const(ctx.mod, out_slot));
  return BinaryenBlock(ctx.mod.raw(), /*name=*/nullptr, instrs.data(),
                       static_cast<BinaryenIndex>(instrs.size()),
                       BinaryenTypeInt32());
}

absl::StatusOr<BinaryenExpressionRef> EmitKIndexCall(
    EmitCtx& ctx, const cel::Expr& expr, const cel::CallExpr& call,
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
  BinaryenExpressionRef call_expr =
      BinaryenCall(ctx.mod.raw(), target.c_str(), args, 3, BinaryenTypeNone());

  BinaryenExpressionRef block_items[2] = {call_expr,
                                          I32Const(ctx.mod, out_slot)};
  return BinaryenBlock(ctx.mod.raw(), /*name=*/nullptr, block_items, 2,
                       BinaryenTypeInt32());
}

// M5.F: lowers a general kCallExpr — every call that isn't `_[_]`
// (M3/M4 indexing, special-cased above) and isn't control flow
// (`_&&_` / `_||_` / `_?_:_`, M5.G — branch-style, slot-out has
// 3VL semantics that don't fit the uniform vv/v ABI).
//
// Shape mirrors `EmitKIndexCall`: emit each arg sub-expression
// (returns its CelValue offset), prepend the out_slot constant,
// emit a single `BinaryenCall` to the helper named by
// `OverloadTable::Lookup(ann.overload_id)->name`, then wrap in a
// `(block (call …) (i32.const out_slot))` so the whole call
// expression value-types as the i32 offset of its result CelValue.
//
// Receiver-form (`s.contains("foo")`) is parsed by cel-cpp into a
// `CallExpr{target: s, function: "contains", args: ["foo"]}`; we
// flatten that to `[target, ...args]` so the wasm helper sees a
// uniform argument list — the `contains_string` helper signature
// is `(out_slot, s_slot, sub_slot) → void`, identical to
// `add_int64`'s `(out, a, b) → void`.
// Slice 1.6: cross-numeric ordering re-pick.  cel-cpp's reference
// map for a comparison call site whose operands span numeric kinds
// (e.g. `dyn(int) < uint`) lists exactly one candidate — the same-
// kind overload of the non-dyn operand's kind (`less_uint64` for
// `dyn(int) < uint`).  That id routes to the per-kind helper
// (`cel_uint_lt_at_vv`) which `require_kinds(..., CEL_UINT)` rejects
// the int operand and poisons.  The fix: at codegen time, inspect
// each operand's annotated `Repr`.  When the function is `_<_` /
// `_<=_` / `_>_` / `_>=_` AND the operand Reprs span a cross-
// numeric pair, override the overload id with the cross-numeric
// id (`less_int64_uint64`, etc.) which routes to the polymorphic
// `cel_numeric_<op>_at_vv` kernel.
//
// Why codegen and not resolve_pass: cel-cpp emits exactly ONE
// candidate per call (probe spike, 2026-04-25) — there is no
// candidate list to choose from.  ResolvePass can't synthesise an
// id that cel-cpp didn't list; codegen has the operand Reprs in
// hand (Slice 1.5's `DynPassthroughVisitor` forwards them onto
// dyn calls) and a static table of cross-numeric ids
// (`kBuiltinSeeds` rows 142–189).
//
// Same-kind operands pass through untouched (the per-kind helpers
// are still preferred — one less branch per call).
//
// `@in` membership is handled via runtime element-equality
// (`cel_value_eq_polymorphic` in `cel_runtime.c`), not via this
// codegen re-pick — the `in_list` / `in_map` ids already route to
// the polymorphic dispatcher; only the element-equality matcher
// needed widening.
bool IsCrossNumericOrderingFunction(absl::string_view fn) {
  return fn == "_<_" || fn == "_<=_" || fn == "_>_" || fn == "_>=_";
}

// True iff `r` is one of the three CEL numeric Reprs.
bool IsNumericRepr(Repr r) {
  return r == Repr::kInt || r == Repr::kUint || r == Repr::kDouble;
}

// Pack a (Repr, Repr) cross-numeric pair into a small dense key.
// Caller has already proven both Reprs are numeric (kInt / kUint /
// kDouble) and a != b — so the six cross-pair shapes map onto six
// distinct switch arms.  Mirrors `numeric_kind_pair` in
// `cel_runtime.c::numeric_compare_kernel`.
constexpr uint16_t PackReprPair(Repr a, Repr b) {
  return static_cast<uint16_t>((static_cast<uint16_t>(a) << 8) |
                               static_cast<uint16_t>(b));
}

// Maps a numeric (Repr, Repr) cross-pair to one of the six
// cross-numeric overload-id stems for the `_<_` ladder.  The
// other ordering ops (`_<=_`, `_>_`, `_>=_`) reuse the same key
// space via per-op tables below.  Returns empty for same-kind
// or non-cross-numeric pairs.
absl::string_view CrossNumericLtId(Repr a, Repr b) {
  switch (PackReprPair(a, b)) {
    case PackReprPair(Repr::kInt, Repr::kUint):
      return "less_int64_uint64";
    case PackReprPair(Repr::kUint, Repr::kInt):
      return "less_uint64_int64";
    case PackReprPair(Repr::kInt, Repr::kDouble):
      return "less_int64_double";
    case PackReprPair(Repr::kDouble, Repr::kInt):
      return "less_double_int64";
    case PackReprPair(Repr::kUint, Repr::kDouble):
      return "less_uint64_double";
    case PackReprPair(Repr::kDouble, Repr::kUint):
      return "less_double_uint64";
    default:
      return {};
  }
}

absl::string_view CrossNumericLeId(Repr a, Repr b) {
  switch (PackReprPair(a, b)) {
    case PackReprPair(Repr::kInt, Repr::kUint):
      return "less_equals_int64_uint64";
    case PackReprPair(Repr::kUint, Repr::kInt):
      return "less_equals_uint64_int64";
    case PackReprPair(Repr::kInt, Repr::kDouble):
      return "less_equals_int64_double";
    case PackReprPair(Repr::kDouble, Repr::kInt):
      return "less_equals_double_int64";
    case PackReprPair(Repr::kUint, Repr::kDouble):
      return "less_equals_uint64_double";
    case PackReprPair(Repr::kDouble, Repr::kUint):
      return "less_equals_double_uint64";
    default:
      return {};
  }
}

absl::string_view CrossNumericGtId(Repr a, Repr b) {
  switch (PackReprPair(a, b)) {
    case PackReprPair(Repr::kInt, Repr::kUint):
      return "greater_int64_uint64";
    case PackReprPair(Repr::kUint, Repr::kInt):
      return "greater_uint64_int64";
    case PackReprPair(Repr::kInt, Repr::kDouble):
      return "greater_int64_double";
    case PackReprPair(Repr::kDouble, Repr::kInt):
      return "greater_double_int64";
    case PackReprPair(Repr::kUint, Repr::kDouble):
      return "greater_uint64_double";
    case PackReprPair(Repr::kDouble, Repr::kUint):
      return "greater_double_uint64";
    default:
      return {};
  }
}

// Note: cel-cpp's `greater_equals_uint_double` is spelled `_uint`
// (no `64`) — see `overload_table.cc:188`.  Mirror that asymmetry.
absl::string_view CrossNumericGeId(Repr a, Repr b) {
  switch (PackReprPair(a, b)) {
    case PackReprPair(Repr::kInt, Repr::kUint):
      return "greater_equals_int64_uint64";
    case PackReprPair(Repr::kUint, Repr::kInt):
      return "greater_equals_uint64_int64";
    case PackReprPair(Repr::kInt, Repr::kDouble):
      return "greater_equals_int64_double";
    case PackReprPair(Repr::kDouble, Repr::kInt):
      return "greater_equals_double_int64";
    case PackReprPair(Repr::kUint, Repr::kDouble):
      return "greater_equals_uint_double";
    case PackReprPair(Repr::kDouble, Repr::kUint):
      return "greater_equals_double_uint64";
    default:
      return {};
  }
}

// Maps a (function, operand_a_repr, operand_b_repr) tuple to the
// matching cross-numeric overload id from `kBuiltinSeeds`.  Returns
// empty string_view when the operands are same-kind or one is
// non-numeric — the caller should fall back to the cel-cpp-picked
// id in that case.
absl::string_view CrossNumericOverloadId(absl::string_view fn, Repr a, Repr b) {
  if (!IsNumericRepr(a) || !IsNumericRepr(b)) return {};
  if (a == b) return {};  // Same-kind: per-kind helper is preferred.
  if (fn == "_<_") return CrossNumericLtId(a, b);
  if (fn == "_<=_") return CrossNumericLeId(a, b);
  if (fn == "_>_") return CrossNumericGtId(a, b);
  if (fn == "_>=_") return CrossNumericGeId(a, b);
  return {};
}

// If `call` is a cross-numeric ordering call whose operand Reprs
// span numeric kinds, return the cross-numeric overload id; else
// empty.  Caller (`EmitGeneralCall`) substitutes the cel-cpp-picked
// id when this returns non-empty.
absl::string_view MaybeRepickCrossNumericOverload(
    const WasmAnnotations& annotations, const cel::CallExpr& call) {
  if (call.has_target() || call.args().size() != 2) return {};
  if (!IsCrossNumericOrderingFunction(call.function())) return {};
  const NodeAnnotation* a_ann = annotations.Find(call.args()[0].id());
  const NodeAnnotation* b_ann = annotations.Find(call.args()[1].id());
  if (a_ann == nullptr || b_ann == nullptr) return {};
  return CrossNumericOverloadId(call.function(), a_ann->repr, b_ann->repr);
}

// Resolves `ann.overload_id` to a runtime helper name, returning an
// `Unimplemented` Status if (a) the annotation is empty
// (ResolvePass didn't stamp it — codegen invariant), (b) no entry
// in OverloadTable matches, or (c) the resolved helper is one of
// the M5.D-step-2 pending dispatchers.  The returned string_view
// is the wasm import name codegen emits a `BinaryenCall` to.
absl::StatusOr<absl::string_view> ResolveCallHelper(const OverloadTable& table,
                                                    const cel::Expr& expr,
                                                    const cel::CallExpr& call,
                                                    const NodeAnnotation& ann) {
  if (ann.overload_id.empty()) {
    return absl::UnimplementedError(absl::StrCat(
        "expr_lower: kCallExpr expr_id=", expr.id(), " function=`",
        call.function(),
        "` has empty overload_id (ResolvePass left the annotation unstamped)"));
  }
  const OverloadImpl* impl = table.Lookup(ann.overload_id);
  if (impl == nullptr) {
    return absl::UnimplementedError(
        absl::StrCat("expr_lower: kCallExpr expr_id=", expr.id(), " function=`",
                     call.function(), "` overload_id=`", ann.overload_id,
                     "` not registered in OverloadTable"));
  }
  return impl->name;
}

// Emits the operand slot offsets a kCallExpr's helper consumes —
// `[out_slot, target?, args...]`.  Receiver-form flattens `target`
// to `args[0]`; wasm helpers see a uniform `(out, in0, in1, ...)`
// signature.
absl::StatusOr<std::vector<BinaryenExpressionRef>> EmitCallOperands(
    EmitCtx& ctx, const cel::CallExpr& call, uint32_t out_slot) {
  std::vector<BinaryenExpressionRef> out;
  out.reserve(1u + (call.has_target() ? 1u : 0u) + call.args().size());
  out.push_back(I32Const(ctx.mod, out_slot));
  if (call.has_target()) {
    auto t_or = Emit(ctx, call.target());
    if (!t_or.ok()) return t_or.status();
    out.push_back(*t_or);
  }
  for (const cel::Expr& arg : call.args()) {
    auto a_or = Emit(ctx, arg);
    if (!a_or.ok()) return a_or.status();
    out.push_back(*a_or);
  }
  return out;
}

// Forward decl — `Emit` calls `EmitConditional`, `EmitConditional`
// recurses through `Emit` for cond / then / else.
absl::StatusOr<BinaryenExpressionRef> Emit(EmitCtx& ctx, const cel::Expr& expr);

// M5.G (Slice 2) — `_?_:_` lowering.  Per langdef §"Conditional
// expression", only the chosen arm is evaluated; ERROR / UNKNOWN
// on the cond propagate verbatim without dispatching to either
// branch.  We materialise this with a nested BinaryenIf:
//
//   (block (result i32)
//     (drop <eval cond>)
//     (if (i32.eq (i32.load cond_slot) CEL_BOOL)
//       (then
//         (if (i32.ne (i32.load offset=8 cond_slot) 0)
//           (then (drop <eval then>) (cel_copy_slot out then_slot))
//           (else (drop <eval else>) (cel_copy_slot out else_slot))))
//       (else (cel_copy_slot out cond_slot)))
//     (i32.const out_slot))
//
// Sub-expressions write into their own pre-allocated annotated
// slots; we drop the offset their `Emit` returns and copy from the
// slot we already know.  Slot aliasing between out_slot and the
// arms is harmless — `cel_copy_slot` self-copies safely.
// Builds `cel_copy_slot(dst_slot, src_slot)` as a Binaryen call.
BinaryenExpressionRef EmitCelCopySlot(EmitCtx& ctx, uint32_t dst_slot,
                                      uint32_t src_slot) {
  BinaryenExpressionRef args[2] = {I32Const(ctx.mod, dst_slot),
                                   I32Const(ctx.mod, src_slot)};
  return BinaryenCall(ctx.mod.raw(), "cel_copy_slot", args, 2,
                      BinaryenTypeNone());
}

// Builds `(eval; cel_copy_slot(out, arm_slot))` for one ternary arm.
// Drops the i32 offset that `eval`'s `Emit` puts on the stack — the
// arm's CelValue is already in `arm_slot` after `eval` runs.
BinaryenExpressionRef BuildConditionalArm(EmitCtx& ctx,
                                          BinaryenExpressionRef eval_expr,
                                          uint32_t out_slot,
                                          uint32_t arm_slot) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef items[2] = {BinaryenDrop(mod, eval_expr),
                                    EmitCelCopySlot(ctx, out_slot, arm_slot)};
  return BinaryenBlock(mod, /*name=*/nullptr, items, 2, BinaryenTypeNone());
}

// `(i32.eq (i32.load offset=N <slot>) <expected>)` — the CelValue
// kind / payload probe used by the ternary's nested-if shape.
BinaryenExpressionRef LoadSlotI32Eq(EmitCtx& ctx, uint32_t slot,
                                    uint32_t offset, int32_t expected) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef load =
      BinaryenLoad(mod, /*bytes=*/4, /*signed=*/0, offset, /*align=*/4,
                   BinaryenTypeInt32(), I32Const(ctx.mod, slot), "memory");
  return BinaryenBinary(mod, BinaryenEqInt32(), load,
                        BinaryenConst(mod, BinaryenLiteralInt32(expected)));
}

BinaryenExpressionRef LoadSlotI32Ne(EmitCtx& ctx, uint32_t slot,
                                    uint32_t offset, int32_t expected) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef load =
      BinaryenLoad(mod, /*bytes=*/4, /*signed=*/0, offset, /*align=*/4,
                   BinaryenTypeInt32(), I32Const(ctx.mod, slot), "memory");
  return BinaryenBinary(mod, BinaryenNeInt32(), load,
                        BinaryenConst(mod, BinaryenLiteralInt32(expected)));
}

absl::StatusOr<BinaryenExpressionRef> EmitConditional(
    EmitCtx& ctx, const cel::Expr& expr, const cel::CallExpr& call,
    const NodeAnnotation& ann) {
  ABSL_CHECK(ann.storage.kind == StorageKind::kWorkspaceSlot)
      << "expr_lower: ternary expr_id=" << expr.id()
      << " has non-workspace storage (LayoutPass didn't allocate a slot)";
  if (call.args().size() != 3) {
    return absl::InvalidArgumentError(
        absl::StrCat("expr_lower: ternary expr_id=", expr.id(),
                     " expects 3 args, got ", call.args().size()));
  }
  const uint32_t out_slot = ann.storage.payload;

  const cel::Expr& cond = call.args()[0];
  const cel::Expr& then_branch = call.args()[1];
  const cel::Expr& else_branch = call.args()[2];

  auto cond_or = Emit(ctx, cond);
  if (!cond_or.ok()) return cond_or.status();
  auto then_or = Emit(ctx, then_branch);
  if (!then_or.ok()) return then_or.status();
  auto else_or = Emit(ctx, else_branch);
  if (!else_or.ok()) return else_or.status();

  const NodeAnnotation* cond_ann = ctx.layout.annotations.Find(cond.id());
  const NodeAnnotation* then_ann =
      ctx.layout.annotations.Find(then_branch.id());
  const NodeAnnotation* else_ann =
      ctx.layout.annotations.Find(else_branch.id());
  ABSL_CHECK(cond_ann != nullptr && then_ann != nullptr && else_ann != nullptr)
      << "expr_lower: ternary sub-expr missing NodeAnnotation";
  const uint32_t cond_slot = cond_ann->storage.payload;
  const uint32_t then_slot = then_ann->storage.payload;
  const uint32_t else_slot = else_ann->storage.payload;

  auto* mod = ctx.mod.raw();

  // CelValue layout pinned by cel_data.h: kind:u32 at off 0,
  // _pad:u32 at off 4, payload at off 8 (b at off 8 for CEL_BOOL).
  // CEL_BOOL = 1.
  BinaryenExpressionRef inner_if = BinaryenIf(
      mod, LoadSlotI32Ne(ctx, cond_slot, /*offset=*/8, /*expected=*/0),
      BuildConditionalArm(ctx, *then_or, out_slot, then_slot),
      BuildConditionalArm(ctx, *else_or, out_slot, else_slot));
  BinaryenExpressionRef outer_if = BinaryenIf(
      mod, LoadSlotI32Eq(ctx, cond_slot, /*offset=*/0, /*expected=*/1),
      inner_if, EmitCelCopySlot(ctx, out_slot, cond_slot));

  BinaryenExpressionRef block_items[3] = {BinaryenDrop(mod, *cond_or), outer_if,
                                          I32Const(ctx.mod, out_slot)};
  return BinaryenBlock(mod, /*name=*/nullptr, block_items, 3,
                       BinaryenTypeInt32());
}

absl::StatusOr<BinaryenExpressionRef> EmitGeneralCall(
    EmitCtx& ctx, const cel::Expr& expr, const cel::CallExpr& call,
    const NodeAnnotation& ann) {
  ABSL_CHECK(ann.storage.kind == StorageKind::kWorkspaceSlot)
      << "expr_lower: kCallExpr expr_id=" << expr.id() << " function=`"
      << call.function()
      << "` has non-workspace storage (LayoutPass didn't allocate a slot)";
  const uint32_t out_slot = ann.storage.payload;

  // Slice 1.6: cross-numeric ordering overload re-pick.  When operand
  // Reprs span a numeric cross-pair, override the cel-cpp-picked id
  // (which was the same-kind overload of one operand) with the
  // cross-numeric id.  See `MaybeRepickCrossNumericOverload` above.
  NodeAnnotation effective_ann = ann;
  absl::string_view repicked =
      MaybeRepickCrossNumericOverload(ctx.layout.annotations, call);
  if (!repicked.empty()) {
    effective_ann.overload_id = repicked;
  }

  auto helper_or =
      ResolveCallHelper(ctx.overload_table, expr, call, effective_ann);
  if (!helper_or.ok()) return helper_or.status();

  auto operands_or = EmitCallOperands(ctx, call, out_slot);
  if (!operands_or.ok()) return operands_or.status();

  const std::string target(*helper_or);
  BinaryenExpressionRef call_expr = BinaryenCall(
      ctx.mod.raw(), target.c_str(), operands_or->data(),
      static_cast<BinaryenIndex>(operands_or->size()), BinaryenTypeNone());

  BinaryenExpressionRef block_items[2] = {call_expr,
                                          I32Const(ctx.mod, out_slot)};
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
    case cel::ExprKindCase::kCallExpr: {
      const cel::CallExpr& call = expr.call_expr();
      // Slice 1.5 (dyn-passthrough-plan.md): `dyn(scalar)` is the
      // identity function at codegen — emit the argument directly.
      // Annotation forwarding (ResolvePass + LayoutPass) ensured
      // any consumer that reads this call's annotation sees the
      // argument's repr / storage; this branch makes sure the call
      // node itself doesn't drop a `cel_to_dyn` helper.
      if (call.function() == "dyn" && call.args().size() == 1 &&
          !call.has_target()) {
        return Emit(ctx, call.args()[0]);
      }
      // M3.F: indexing operator `_[_]` is origin-aware (kArena fast
      // path / kHost trampoline / kDynamic dispatcher) and pre-dates
      // the OverloadTable; keep its bespoke arm.
      if (call.function() == "_[_]") {
        return EmitKIndexCall(ctx, expr, call, *ann);
      }
      // M5.G (Slice 2) ternary `_?_:_`: BinaryenIf-based lowering
      // (only the chosen arm is evaluated, per langdef §"Conditional
      // expression").  `_&&_` / `_||_` / `!_` route through the
      // standard slot-out helper arm — non-strict 3VL semantics
      // live entirely inside `cel_and` / `cel_or` / `cel_not`.
      if (call.function() == "_?_:_") {
        return EmitConditional(ctx, expr, call, *ann);
      }
      // M5.F general arm — arithmetic, comparison, string ops,
      // receiver-style (`s.contains(…)`), and any custom function
      // the embedder registered (M6).  Lookup is by
      // `ann.overload_id` (cel-cpp's resolved overload string,
      // stamped by ResolvePass).
      return EmitGeneralCall(ctx, expr, call, *ann);
    }
    case cel::ExprKindCase::kMapExpr:
      return EmitKMapExpr(ctx, expr, expr.map_expr(), *ann);
    case cel::ExprKindCase::kListExpr:
      return EmitKListExpr(ctx, expr, expr.list_expr(), *ann);
    case cel::ExprKindCase::kStructExpr:
      return EmitKStructExpr(ctx, expr, expr.struct_expr(), *ann);
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
    absl::string_view func_name, WasmModule& mod,
    const OverloadTable& overload_table, const LoweringOptions& opts) {
  ABSL_CHECK(ast.has_ast())
      << "LowerToEvalFunction: TypedAst has no checked cel::Ast";

  // field_refs[0] is the reserved "not proto-resolvable" sentinel;
  // subsequent rows are pushed by EmitKSelect as the walk emits each
  // select.
  std::vector<FieldRefRow> field_refs;
  field_refs.push_back(FieldRefRow{});
  EmitCtx ctx{mod, ast, layout, field_refs, overload_table};

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
