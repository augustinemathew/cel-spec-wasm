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
#include "compiler_v2/codegen/expr_lower_internal.h"
#include "compiler_v2/codegen/layout_pass.h"
#include "compiler_v2/codegen/module.h"
#include "compiler_v2/codegen/overload_table.h"
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

}  // namespace

// Wrap an i32 literal offset as a Binaryen `(i32.const <n>)`.
// Hoisted out of the anonymous namespace post-split (commit
// fb55b7f) so expr_lower_comprehension.cc can reach it via the
// shared internal header.
BinaryenExpressionRef I32Const(WasmModule& mod, uint32_t value) {
  return BinaryenConst(mod.raw(),
                       BinaryenLiteralInt32(static_cast<int32_t>(value)));
}

namespace {

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

// Emits `(call $arena_reset)`.  First instruction of every `$eval`
// body after the variable prelude: rewinds the runtime's bump-arena
// cursor to 0 so each eval gets a fresh arena.  Takes no arguments
// (the bump cursor lives in BSS, not linear memory, post-WASI
// migration — see doc/implementation-plan/rewrite/wasi/DESIGN.md §4).
BinaryenExpressionRef EmitArenaResetCall(WasmModule& mod) {
  const std::string name(kArenaResetInternalName);
  return BinaryenCall(mod.raw(), name.c_str(), nullptr, 0, BinaryenTypeNone());
}

// Emits one `local.set local_index (i32.const slot_offset)` per
// referenced variable, populating each ident's wasm local with its
// compile-time-known workspace slot offset before the body run.
// Per `rewrite/m2-ident-select-unknowns.md` §2.6: every kIdent
// lowering is `local.get local_index`, so the prelude is the one
// place where the "which slot?" question gets answered for free
// variables.
//
// Prelude goes at the top of `$eval`, before `arena_reset`, purely
// for readability of the generated WAT — arena_reset doesn't touch
// the workspace region, so the relative order is semantically
// irrelevant.
std::vector<BinaryenExpressionRef> EmitVariablePrelude(
    WasmModule& mod, absl::Span<const LaidOutVariable> variables) {
  std::vector<BinaryenExpressionRef> out;
  out.reserve(variables.size());
  for (const LaidOutVariable& v : variables) {
    // Comprehension-scope locals (iter / accu / index) are set by
    // the comprehension's loop prologue, not the function prelude.
    // Skipping them here also keeps free-variable slot offsets
    // stable when comprehensions are present — the host marshal
    // addresses by `cel.abi.variables[].slot_offset`.
    if (v.kind != ResolvedVariableKind::kFreeVariable) continue;
    out.push_back(BinaryenLocalSet(mod.raw(), v.local_index,
                                   I32Const(mod, v.slot_offset)));
  }
  return out;
}

// EmitCtx + the `Emit` dispatcher forward decl live in
// expr_lower_internal.h so expr_lower_comprehension.cc shares them.

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
  instrs.reserve(2u + (3u * N));
  instrs.push_back(EmitCelMapCreateCall(ctx.mod, out_slot, N));

  for (const cel::MapExprEntry& e : m.entries()) {
    ABSL_CHECK(!e.optional())
        << "expr_lower: kMapExpr expr_id=" << expr.id()
        << " entry id=" << e.id() << " is optional — stub";
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
// on the operand's `map_origin`.  Three-path dispatch (per
// `rewrite/map-list-dispatch.md` §2.6):
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

// Same shape as MapLookupCallTarget but for list indexing.
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

// Emits `(call $cel.cel_list_append_at <list_slot> <elem_eval>)`.
// `elem_eval` is an i32-valued sub-expression whose value at runtime
// is the linear-memory offset of the element's CelValue.  Universal
// write for arena lists — shared between kListExpr literal codegen
// and comprehension accu codegen.
}  // namespace

BinaryenExpressionRef EmitCelListAppendCall(EmitCtx& ctx, uint32_t list_slot,
                                            BinaryenExpressionRef elem_eval) {
  BinaryenExpressionRef args[2] = {I32Const(ctx.mod, list_slot), elem_eval};
  return BinaryenCall(ctx.mod.raw(), "cel_list_append_at", args, 2,
                      BinaryenTypeNone());
}

namespace {

// Lowers a kListExpr to:
//   (call $cel.cel_list_create out_slot N)   ;; capacity=N, count=0
//   for i in [0, N):
//     <eval element>      -> i32 elem_offset
//     (call $cel.cel_list_append_at out_slot elem_offset)
//   (i32.const out_slot)
// wrapped in a (block (result i32)) whose value is `out_slot`.
// Final `count == capacity == N`.  Append is the universal write
// for arena lists — shared with comprehension accu codegen.
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
        << " element index=" << i << " is optional — stub";
    auto elem_or = Emit(ctx, e.expr());
    if (!elem_or.ok()) return elem_or.status();
    instrs.push_back(EmitCelListAppendCall(ctx, out_slot, *elem_or));
  }

  instrs.push_back(I32Const(ctx.mod, out_slot));
  return BinaryenBlock(ctx.mod.raw(), /*name=*/nullptr, instrs.data(),
                       static_cast<BinaryenIndex>(instrs.size()),
                       BinaryenTypeInt32());
}

// Emits `(call $cel_host.cel_make_message (i32.const type_id)
// (i32.const out_slot))`.  Two i32 args, void result.  The trampoline
// resolves type_id → Descriptor* against the per-Instance lookup
// table (populated from `cel.abi.types[]` at Plan time), allocates a
// default-constructed proto via MessageFactory::GetPrototype()->New(),
// wraps in an owning HostMessageBacking, interns into the
// ExternrefTable, and writes a CEL_MESSAGE CelValue with the interned
// msg_slot to the out_slot cell.  See `rewrite/m7-proto-literals.md`.
BinaryenExpressionRef EmitCelMakeMessageCall(WasmModule& mod, uint32_t type_id,
                                             uint32_t out_slot) {
  BinaryenExpressionRef args[2] = {I32Const(mod, type_id),
                                   I32Const(mod, out_slot)};
  const std::string name(kCelHostMakeMessageInternalName);
  return BinaryenCall(mod.raw(), name.c_str(), args, 2, BinaryenTypeNone());
}

// Emits `(call $cel_host.cel_set_field (i32.const msg_slot)
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
// Empty struct literal — entry loop is a no-op.  Scalar entry-set
// — the cel_host Layer-2 dispatches per-cpp_type; repeated/map/
// message singular fields trap at the trampoline.  No codegen-time
// gating per field type — codegen here doesn't know the descriptor's
// shape; it trusts the checker for type compatibility and the
// trampoline to surface unsupported shapes as clean traps.
//
// Maps a wrapper FQN to the matching `CelKind` (1..6) the WKT
// tail-unwrap trampoline expects as its third argument.  Returns
// 0 (not a valid CelKind for the unwrap path) for non-wrapper
// FQNs — caller falls through.  Mirrors `IsWrapperFqn` in
// `compiler_v2/api/internal/cel_host.cc` but maps to the inner
// scalar kind in a single call; Int32/Int64 collapse onto CEL_INT,
// UInt32/UInt64 onto CEL_UINT, Float/Double onto CEL_DOUBLE per
// CEL's value algebra (no 32-vs-64 distinction; see
// `rewrite/m8-wrapper-types.md` and `rewrite/wat-traces.md` §56 for
// the rationale + ABI lock).
uint32_t WrapperKindFromFqn(absl::string_view fqn) {
  if (fqn == "google.protobuf.BoolValue") return 1;    // CEL_BOOL
  if (fqn == "google.protobuf.Int32Value") return 2;   // CEL_INT
  if (fqn == "google.protobuf.Int64Value") return 2;   // CEL_INT
  if (fqn == "google.protobuf.UInt32Value") return 3;  // CEL_UINT
  if (fqn == "google.protobuf.UInt64Value") return 3;  // CEL_UINT
  if (fqn == "google.protobuf.FloatValue") return 4;   // CEL_DOUBLE
  if (fqn == "google.protobuf.DoubleValue") return 4;  // CEL_DOUBLE
  if (fqn == "google.protobuf.StringValue") return 5;  // CEL_STRING
  if (fqn == "google.protobuf.BytesValue") return 6;   // CEL_BYTES
  return 0;
}

// Well-known proto-literal tail-unwrap.  At kStructExpr lowering,
// after the recursive build sets the message fields, emit a host
// trampoline call that overwrites the message slot IN PLACE with
// the equivalent scalar / Timestamp / Duration CelValue.  Necessary
// because `compiler_v2/ir/typed_ast.cc:56` maps WKT-typed
// expressions to scalar Repr — the downstream pipeline expects a
// scalar at this slot, not a `CEL_MESSAGE`.  See
// `rewrite/m8-wrapper-types.md` and `rewrite/m7b-duration-timestamp.md`.
//   - Timestamp / Duration: 2-arg trampoline
//     `cel_wkt_unwrap_time(out_slot, msg_slot)`.
//   - 9 WKT wrappers: 3-arg trampoline
//     `cel_wkt_unwrap_wrapper(out_slot, msg_slot, wrapper_kind)`
//     where `wrapper_kind` is the inner CelKind (1..6).
// Returns nullptr for non-WKT struct names — caller falls through.
BinaryenExpressionRef MaybeEmitWktUnwrapTailCall(EmitCtx& ctx,
                                                 absl::string_view type_fqn,
                                                 uint32_t out_slot) {
  if (type_fqn == "google.protobuf.Timestamp" ||
      type_fqn == "google.protobuf.Duration") {
    BinaryenExpressionRef args[2] = {I32Const(ctx.mod, out_slot),
                                     I32Const(ctx.mod, out_slot)};
    return BinaryenCall(ctx.mod.raw(),
                        std::string(kCelHostWktUnwrapTimeInternalName).c_str(),
                        args, 2, BinaryenTypeNone());
  }
  if (const uint32_t wrapper_kind = WrapperKindFromFqn(type_fqn);
      wrapper_kind != 0) {
    BinaryenExpressionRef args[3] = {I32Const(ctx.mod, out_slot),
                                     I32Const(ctx.mod, out_slot),
                                     I32Const(ctx.mod, wrapper_kind)};
    return BinaryenCall(
        ctx.mod.raw(),
        std::string(kCelHostWktUnwrapWrapperInternalName).c_str(), args, 3,
        BinaryenTypeNone());
  }
  return nullptr;
}

absl::StatusOr<BinaryenExpressionRef> EmitKStructExpr(
    EmitCtx& ctx, const cel::Expr& expr, const cel::StructExpr& s,
    const NodeAnnotation& ann) {
  ABSL_CHECK(!s.name().empty())
      << "expr_lower: kStructExpr expr_id=" << expr.id()
      << " has empty name() — should have lowered as kMapExpr at parse "
         "time per rewrite/design.md §4.7.4";
  ABSL_CHECK(ann.storage.kind == StorageKind::kWorkspaceSlot)
      << "expr_lower: kStructExpr expr_id=" << expr.id()
      << " has non-workspace storage (LayoutPass didn't allocate a slot)";
  ABSL_CHECK(ann.message_type_id != 0)
      << "expr_lower: kStructExpr expr_id=" << expr.id()
      << " has message_type_id=0 — ResolvePass MessageTypeIdVisitor "
         "didn't intern the FQN (rewrite/m7-proto-literals.md §4.2)";

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
    // Append a fresh field-ref row.  field_number=0 makes the
    // host resolve the FieldDescriptor by name against the bound
    // message — matches the read-side fallback path
    // ProtoBacking::ResolveFieldDescriptor already uses for
    // non-proto backings (see `rewrite/m2-ident-select-unknowns.md` §2.4).
    const auto field_ref_id = static_cast<uint32_t>(ctx.field_refs.size());
    ctx.field_refs.push_back(FieldRefRow{
        /*field_number=*/0,
        /*name=*/f.name(),
        /*owner_fqn=*/s.name(),
    });
    instrs.push_back(
        EmitCelSetFieldCall(ctx.mod, out_slot, field_ref_id, *value_or));
  }

  if (auto wkt_tail = MaybeEmitWktUnwrapTailCall(ctx, s.name(), out_slot);
      wkt_tail != nullptr) {
    instrs.push_back(wkt_tail);
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

// General kCallExpr lowering — every call that isn't `_[_]`
// (indexing, special-cased above) and isn't control flow (`_&&_` /
// `_||_` / `_?_:_`, branch-style; slot-out has 3VL semantics that
// don't fit the uniform vv/v ABI).
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
//
// Cross-numeric ordering re-pick.  cel-cpp's reference map for a
// comparison call site whose operands span numeric kinds (e.g.
// `dyn(int) < uint`) lists exactly one candidate — the same-kind
// overload of the non-dyn operand's kind (`less_uint64` for
// `dyn(int) < uint`).  That id routes to the per-kind helper
// (`cel_uint_lt_at_vv`) which `require_kinds(..., CEL_UINT)` rejects
// the int operand and poisons.  Fix: at codegen time, inspect each
// operand's annotated `Repr`; when the function is `_<_` / `_<=_` /
// `_>_` / `_>=_` AND the operand Reprs span a cross-numeric pair,
// override the overload id with the cross-numeric id
// (`less_int64_uint64`, etc.) which routes to the polymorphic
// `cel_numeric_<op>_at_vv` kernel.
//
// Why codegen and not resolve_pass: cel-cpp emits exactly ONE
// candidate per call — there is no candidate list to choose from.
// ResolvePass can't synthesise an id that cel-cpp didn't list;
// codegen has the operand Reprs in hand and a static table of
// cross-numeric ids in `kBuiltinSeeds`.  See
// `rewrite/cross-numeric-ordering-plan.md`.
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
// (ResolvePass didn't stamp it — codegen invariant), or (b) no
// entry in OverloadTable matches.  The returned string_view is
// the wasm import name codegen emits a `BinaryenCall` to.
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

// `Emit` is forward-declared earlier in this TU (line ~161); the
// definition appears below.  EmitConditional recurses via `Emit`
// for cond / then / else, so it relies on that earlier declaration.

// `_?_:_` ternary lowering.  Per langdef §"Conditional
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
}  // namespace

BinaryenExpressionRef EmitCelCopySlot(EmitCtx& ctx, uint32_t dst_slot,
                                      uint32_t src_slot) {
  BinaryenExpressionRef args[2] = {I32Const(ctx.mod, dst_slot),
                                   I32Const(ctx.mod, src_slot)};
  return BinaryenCall(ctx.mod.raw(), "cel_copy_slot", args, 2,
                      BinaryenTypeNone());
}

// Builds `cel_copy_slot(out, <eval>)` for one ternary arm.  The
// eval expression returns the slot offset of the arm's CelValue at
// runtime — whatever the arm's storage kind (rodata / workspace /
// local).  Using the eval value directly avoids the trap of
// hard-coding `storage.payload`, which for `kLocal` (kIdent arms)
// is a local index, NOT a slot offset.  The kIdent-arm case
// shows up in comprehensions like `exists_one`'s loop_step
// `p ? accu+1 : accu` (else-arm is a bare `kIdent(@result)`).
BinaryenExpressionRef BuildConditionalArm(EmitCtx& ctx,
                                          BinaryenExpressionRef eval_expr,
                                          uint32_t out_slot) {
  BinaryenExpressionRef args[2] = {I32Const(ctx.mod, out_slot), eval_expr};
  return BinaryenCall(ctx.mod.raw(), "cel_copy_slot", args, 2,
                      BinaryenTypeNone());
}

// `(i32.eq (i32.load offset=N <slot>) <expected>)` — the CelValue
// kind / payload probe used by the ternary's nested-if shape and the
// comprehension loop-cond peephole.
BinaryenExpressionRef LoadSlotI32Eq(EmitCtx& ctx, uint32_t slot,
                                    uint32_t offset, int32_t expected) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef load =
      BinaryenLoad(mod, /*bytes=*/4, /*signed_=*/false, offset, /*align=*/4,
                   BinaryenTypeInt32(), I32Const(ctx.mod, slot), "memory");
  return BinaryenBinary(mod, BinaryenEqInt32(), load,
                        BinaryenConst(mod, BinaryenLiteralInt32(expected)));
}

BinaryenExpressionRef LoadSlotI32Ne(EmitCtx& ctx, uint32_t slot,
                                    uint32_t offset, int32_t expected) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef load =
      BinaryenLoad(mod, /*bytes=*/4, /*signed_=*/false, offset, /*align=*/4,
                   BinaryenTypeInt32(), I32Const(ctx.mod, slot), "memory");
  return BinaryenBinary(mod, BinaryenNeInt32(), load,
                        BinaryenConst(mod, BinaryenLiteralInt32(expected)));
}

namespace {

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
  ABSL_CHECK(cond_ann != nullptr)
      << "expr_lower: ternary cond sub-expr missing NodeAnnotation";
  const uint32_t cond_slot = cond_ann->storage.payload;

  auto* mod = ctx.mod.raw();

  // CelValue layout pinned by cel_data.h: kind:u32 at off 0,
  // _pad:u32 at off 4, payload at off 8 (b at off 8 for CEL_BOOL).
  // CEL_BOOL = 1.  Then / else arm slots come from the arm's eval
  // expression at runtime (correct across kLocal / kStaticRodata /
  // kWorkspaceSlot storage kinds — see BuildConditionalArm).
  BinaryenExpressionRef inner_if = BinaryenIf(
      mod, LoadSlotI32Ne(ctx, cond_slot, /*offset=*/8, /*expected=*/0),
      BuildConditionalArm(ctx, *then_or, out_slot),
      BuildConditionalArm(ctx, *else_or, out_slot));
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

  // Cross-numeric ordering overload re-pick.  When operand Reprs
  // span a numeric cross-pair, override the cel-cpp-picked id
  // (which was the same-kind overload of one operand) with the
  // cross-numeric id.  See `MaybeRepickCrossNumericOverload` above
  // and `rewrite/cross-numeric-ordering-plan.md`.
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

}  // namespace

// Top-level expression dispatcher.  Returns an i32-valued Binaryen
// expression whose runtime value is the linear-memory offset of the
// emitted node's CelValue (rodata for kConst, workspace slot for
// kIdent + most aggregates, block-returning-i32 for nested ops).
// Comprehension codegen lives in expr_lower_comprehension.cc;
// `LowerComprehension` is called from the kComprehensionExpr arm
// below and shares state via the EmitCtx threaded through here.
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
      // `dyn(scalar)` is the identity function at codegen — emit
      // the argument directly (see `rewrite/dyn-passthrough-plan.md`).
      // Annotation forwarding (ResolvePass + LayoutPass) ensured
      // any consumer that reads this call's annotation sees the
      // argument's repr / storage; this branch makes sure the call
      // node itself doesn't drop a `cel_to_dyn` helper.
      if (call.function() == "dyn" && call.args().size() == 1 &&
          !call.has_target()) {
        return Emit(ctx, call.args()[0]);
      }
      // Indexing operator `_[_]` is origin-aware (kArena fast
      // path / kHost trampoline / kDynamic dispatcher) and pre-dates
      // the OverloadTable; keep its bespoke arm.
      if (call.function() == "_[_]") {
        return EmitKIndexCall(ctx, expr, call, *ann);
      }
      // Ternary `_?_:_` uses BinaryenIf-based lowering — only the
      // chosen arm is evaluated, per langdef §"Conditional expression".
      // `_&&_` / `_||_` / `!_` route through the standard slot-out
      // helper arm — non-strict 3VL semantics live entirely inside
      // `cel_and` / `cel_or` / `cel_not`.
      if (call.function() == "_?_:_") {
        return EmitConditional(ctx, expr, call, *ann);
      }
      // General arm — arithmetic, comparison, string ops,
      // receiver-style (`s.contains(…)`), and any custom function
      // the embedder registered (see `rewrite/m-custom-fns.md`).
      // Lookup is by `ann.overload_id` (cel-cpp's resolved overload
      // string, stamped by ResolvePass).
      return EmitGeneralCall(ctx, expr, call, *ann);
    }
    case cel::ExprKindCase::kMapExpr:
      return EmitKMapExpr(ctx, expr, expr.map_expr(), *ann);
    case cel::ExprKindCase::kListExpr:
      return EmitKListExpr(ctx, expr, expr.list_expr(), *ann);
    case cel::ExprKindCase::kStructExpr:
      return EmitKStructExpr(ctx, expr, expr.struct_expr(), *ann);
    case cel::ExprKindCase::kComprehensionExpr:
      return LowerComprehension(ctx, expr, expr.comprehension_expr(), *ann);
    case cel::ExprKindCase::kUnspecifiedExpr:
      return Unimplemented(expr.kind_case(), expr.id());
  }
  ABSL_CHECK(false) << "Emit: unknown ExprKindCase "
                    << static_cast<int>(expr.kind_case());
}

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
  //     (call $arena_reset)
  //     <root expression>)
  //
  // The block's last expression supplies its return value, so the
  // root expression's i32 is what `$eval` returns.  Prelude +
  // arena_reset have `none` result type and contribute nothing to
  // the block's value.
  std::vector<BinaryenExpressionRef> instrs =
      EmitVariablePrelude(mod, layout.variables);
  instrs.push_back(EmitArenaResetCall(mod));
  instrs.push_back(*root_ref);
  BinaryenExpressionRef body = BinaryenBlock(
      mod.raw(), /*name=*/nullptr, instrs.data(),
      static_cast<BinaryenIndex>(instrs.size()), BinaryenTypeInt32());

  // Every wasm local `$eval` carries is a u32 memory offset — one
  // per referenced variable (see `rewrite/m2-ident-select-unknowns.md` §2.6),
  // plus per-comprehension auxiliary locals reserved by LayoutPass's
  // `ComprehensionLocalsVisitor` for end_off / iter cursor / index
  // counter (see `rewrite/m5-comprehensions-design.md`).
  const std::string func_name_c(func_name);
  const uint32_t locals_count =
      layout.total_wasm_locals != 0
          ? layout.total_wasm_locals
          : static_cast<uint32_t>(layout.variables.size());
  const std::vector<BinaryenType> local_types(locals_count,
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
