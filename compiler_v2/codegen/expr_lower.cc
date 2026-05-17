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
    // M5.B Slice B: comprehension-scope locals (iter / accu /
    // index) are set by the comprehension's loop prologue, not the
    // function prelude.  Skipping them here also keeps free-variable
    // slot offsets stable when comprehensions are present — the
    // host marshal addresses by `cel.abi.variables[].slot_offset`.
    if (v.kind != ResolvedVariableKind::kFreeVariable) continue;
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
  instrs.reserve(2u + (3u * N));
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

// Emits `(call $cel.cel_list_append_at <list_slot> <elem_eval>)`.
// `elem_eval` is an i32-valued sub-expression whose value at runtime
// is the linear-memory offset of the element's CelValue.  Universal
// write for arena lists — shared between kListExpr literal codegen
// and comprehension accu codegen.
BinaryenExpressionRef EmitCelListAppendCall(EmitCtx& ctx, uint32_t list_slot,
                                            BinaryenExpressionRef elem_eval) {
  BinaryenExpressionRef args[2] = {I32Const(ctx.mod, list_slot), elem_eval};
  return BinaryenCall(ctx.mod.raw(), "cel_list_append_at", args, 2,
                      BinaryenTypeNone());
}

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
        << " element index=" << i << " is optional — stub until M5";
    auto elem_or = Emit(ctx, e.expr());
    if (!elem_or.ok()) return elem_or.status();
    instrs.push_back(EmitCelListAppendCall(ctx, out_slot, *elem_or));
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
BinaryenExpressionRef EmitCelMakeMessageCall(WasmModule& mod, uint32_t type_id,
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
// Maps a wrapper FQN to the matching `CelKind` (1..6) the M8.C
// tail-unwrap trampoline expects as its third argument.  Returns
// 0 (not a valid CelKind for the unwrap path) for non-wrapper
// FQNs — caller falls through.  Mirrors `IsWrapperFqn` in
// `compiler_v2/api/internal/cel_host.cc` but maps to the inner
// scalar kind in a single call; Int32/Int64 collapse onto CEL_INT,
// UInt32/UInt64 onto CEL_UINT, Float/Double onto CEL_DOUBLE per
// CEL's value algebra (no 32-vs-64 distinction; see
// `wat-traces.md` §56 for the rationale + ABI lock).
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

// M7B polish + M8.C: well-known proto-literal tail-unwrap.  At
// kStructExpr lowering, after the recursive build sets the message
// fields, emit a host trampoline call that overwrites the message
// slot IN PLACE with the equivalent scalar / Timestamp / Duration
// CelValue.  Necessary because `compiler_v2/ir/typed_ast.cc:56`
// maps WKT-typed expressions to scalar Repr — the downstream
// pipeline expects a scalar at this slot, not a `CEL_MESSAGE`.
//   - Timestamp / Duration (m7b polish): 2-arg trampoline
//     `cel_wkt_unwrap_time(out_slot, msg_slot)`.
//   - 9 WKT wrappers (M8.C): 3-arg trampoline
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

// `Emit` is forward-declared earlier in this TU (line ~161); the
// definition appears below.  EmitConditional recurses via `Emit`
// for cond / then / else, so it relies on that earlier declaration.

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

// Builds `cel_copy_slot(out, <eval>)` for one ternary arm.  The
// eval expression returns the slot offset of the arm's CelValue at
// runtime — whatever the arm's storage kind (rodata / workspace /
// local).  Using the eval value directly avoids the trap of
// hard-coding `storage.payload`, which for `kLocal` (kIdent arms)
// is a local index, NOT a slot offset.  M5.B Slice C surfaced the
// kIdent-arm case via `exists_one`'s loop_step `p ? accu+1 : accu`
// (else-arm is a bare `kIdent(@result)`).
BinaryenExpressionRef BuildConditionalArm(EmitCtx& ctx,
                                          BinaryenExpressionRef eval_expr,
                                          uint32_t out_slot) {
  BinaryenExpressionRef args[2] = {I32Const(ctx.mod, out_slot), eval_expr};
  return BinaryenCall(ctx.mod.raw(), "cel_copy_slot", args, 2,
                      BinaryenTypeNone());
}

// `(i32.eq (i32.load offset=N <slot>) <expected>)` — the CelValue
// kind / payload probe used by the ternary's nested-if shape.
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

// ============================================================
// M5.B Slice C — kComprehensionExpr lowering (Shape A: list iter,
// single-iter-var; exists / all / exists_one accumulators).
// ============================================================
//
// Memory & local layout (per WAT 60 / 61):
//   - iter_range evaluated in OUTER scope → list_slot (workspace,
//     filled by kCreateList or any list-producing expression).
//   - accu_init evaluated in OUTER scope → CelValue copied via
//     cel_copy_slot into accu_slot (the accu_var's workspace cell).
//   - iter_var.local_index doubles as iter_off (the moving pointer
//     into the list payload).  kIdent(iter_var) inside the body
//     reads it via `local.get` — the existing kIdent arm needs no
//     comprehension awareness (uniform load doctrine, WAT 60 §1).
//   - accu_var.local_index is set ONCE to accu_slot offset.
//     kIdent(accu_var) reads it the same way.
//   - comp_aux_local_base + 0 holds end_off (one-past-end iter
//     pointer).  Slot 1 reserved for Slice E (map cursor) / Slice
//     F (two-iter-var index counter).
//
// Loop-cond peephole (cel-cpp probe 2026-05-17): cel-cpp's macro
// expansions only ever emit one of:
//   - `kConst true`  (exists_one)             → omit cond check
//   - `kConst false` (cel.bind — Slice I)     → immediate exit
//   - `@not_strictly_false(kIdent(@result))`  (all)     → br_if exit
//                                                          when accu's
//                                                          bool payload
//                                                          byte == 0
//   - `@not_strictly_false(!_(kIdent(@result)))` (exists) → br_if exit
//                                                            when accu's
//                                                            bool payload
//                                                            byte != 0
// The peephole reads `i32.load offset=8` of accu_slot directly,
// bypassing `cel_not_strictly_false` (which therefore stays
// deferred).  Any unrecognised loop_cond shape returns
// UnimplementedError — Slice I adds the general path for
// cel.bind / arbitrary user shapes.

bool TryMatchBoolConst(const cel::Expr& expr, bool* out) {
  if (expr.kind_case() != cel::ExprKindCase::kConstant) return false;
  if (!expr.const_expr().has_bool_value()) return false;
  *out = expr.const_expr().bool_value();
  return true;
}

bool IsIdentNamed(const cel::Expr& expr, absl::string_view name) {
  return expr.kind_case() == cel::ExprKindCase::kIdentExpr &&
         expr.ident_expr().name() == name;
}

// `kCall(@not_strictly_false, kIdent(name))` — `all` loop_cond.
bool IsNotStrictlyFalseOfIdent(const cel::Expr& expr,
                               absl::string_view accu_name) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& call = expr.call_expr();
  if (call.function() != "@not_strictly_false" || call.args().size() != 1) {
    return false;
  }
  return IsIdentNamed(call.args()[0], accu_name);
}

// `kCall(@not_strictly_false, kCall(!_, kIdent(name)))` — `exists`.
bool IsNotStrictlyFalseOfNotIdent(const cel::Expr& expr,
                                  absl::string_view accu_name) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& call = expr.call_expr();
  if (call.function() != "@not_strictly_false" || call.args().size() != 1) {
    return false;
  }
  const auto& arg = call.args()[0];
  if (arg.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& inner = arg.call_expr();
  if (inner.function() != "!_" || inner.args().size() != 1) return false;
  return IsIdentNamed(inner.args()[0], accu_name);
}

// `i32.load offset=8 (i32.const accu_slot)` — read the bool payload
// byte (CelValue.payload.b is at byte offset 8 from slot base).
BinaryenExpressionRef LoadAccuBoolPayload(EmitCtx& ctx, uint32_t accu_slot) {
  auto* mod = ctx.mod.raw();
  return BinaryenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/8,
                      /*align=*/4, BinaryenTypeInt32(),
                      I32Const(ctx.mod, accu_slot), "memory");
}

// Per-comprehension binding context populated by Slice C / extended
// in F.  Slots / locals come from ResolvePass + LayoutPass; codegen
// in `LowerComprehension` walks the same data to emit the prologue.
struct CompContext {
  const LaidOutVariable* iter_v = nullptr;
  // Slice F: iter_var2 (null for single-iter-var).  List two-iter:
  // iter_v is the index counter workspace slot; iter_v2 is the
  // moving value pointer.  Map two-iter: both are workspace slots
  // populated by cel_map_iter_{key,value}_at each iter.
  const LaidOutVariable* iter_v2 = nullptr;
  const LaidOutVariable* accu_v = nullptr;
  // For list source: end_off pointer (one-past-end of element run).
  // For map source: iter handle (returned by cel_map_iter_init,
  // passed to cel_map_iter_next / key_at / value_at).
  uint32_t aux0_local = 0;
  // Slice F: list two-iter index counter local (raw int — not a
  // CelValue offset).  Written into iter_v's workspace slot each
  // iter as {kind=CEL_INT, payload.i=index}.  Unused for
  // single-iter and for map source.
  uint32_t aux1_local = 0;
  uint32_t accu_slot = 0;
  uint32_t source_slot = 0;  // list_slot or map_slot
  uint32_t init_src_slot = 0;
  // M5.B Slice E: true if iter_range types as map.  Switches the
  // prologue + loop body to the cel_map_iter_* path.  Iter_var
  // binds to the current key (a workspace slot whose CelValue is
  // rewritten by cel_map_iter_key_at each iteration); iter_var's
  // LaidOutVariable.slot_offset is non-zero here (kComprehensionAccu
  // lifecycle set by ResolvePass when iter_range.repr == kMap).
  bool map_source = false;
  // Slice F: true if iter_var2 is non-empty (comprehensions_v2
  // three-arg form).
  bool two_iter = false;
  // Per-comprehension unique labels.  Nested comprehensions emit
  // their own `(block exit_<id> (loop continue_<id> ...))`; without
  // expr-id-scoped names, Binaryen rejects nested same-name labels
  // with `wasm-validator.cpp visitLoop: iter != breakTypes.end()`.
  std::string exit_label;
  std::string continue_label;
};

// Resolve the per-comp iter / accu LaidOutVariable entries from the
// comp-node's stamped indices (name-based lookup conflates nested
// same-name accu_vars — see the 2026-05-17 nested probe).  Also pins
// `aux0_local` + `accu_slot` from the annotations.
void BindCompVariables(EmitCtx& ctx, const cel::Expr& expr,
                       const cel::ComprehensionExpr& comp,
                       const NodeAnnotation& ann, CompContext* c) {
  ABSL_CHECK(ann.comp_iter_local_index < ctx.layout.variables.size() &&
             ann.comp_accu_local_index < ctx.layout.variables.size())
      << "LowerComprehension: per-comp local indices out of range (expr_id="
      << expr.id() << " iter=" << ann.comp_iter_local_index
      << " accu=" << ann.comp_accu_local_index
      << " variables.size=" << ctx.layout.variables.size() << ")";
  c->iter_v = &ctx.layout.variables[ann.comp_iter_local_index];
  c->accu_v = &ctx.layout.variables[ann.comp_accu_local_index];
  c->two_iter = !comp.iter_var2().empty();
  if (c->two_iter) {
    ABSL_CHECK(ann.comp_iter2_local_index < ctx.layout.variables.size())
        << "LowerComprehension: iter2 local index out of range (expr_id="
        << expr.id() << ")";
    c->iter_v2 = &ctx.layout.variables[ann.comp_iter2_local_index];
  }
  ABSL_CHECK(ann.comp_aux_local_base != 0)
      << "LowerComprehension: ComprehensionLocalsVisitor didn't assign aux "
         "locals (expr_id="
      << expr.id() << ")";
  c->aux0_local = ann.comp_aux_local_base + 0;
  c->aux1_local = ann.comp_aux_local_base + 1;
  c->accu_slot = c->accu_v->slot_offset;
  ABSL_CHECK(c->accu_slot != 0) << "LowerComprehension: accu_var `"
                                << comp.accu_var() << "` has no workspace slot";
}

// followon §10.A: collection-shaped accu (list or map) with an
// empty-literal accu_init — i.e. one of the standard / v2
// collection-producing macros (`map`, `filter`, `transformList`,
// `transformMap`, `transformMapEntry`).  These get the pre-sized
// prologue (`cel_list_create` / `cel_map_create` with
// capacity=iter_range.count); everything else falls back to the
// generic `Emit(accu_init) + cel_copy_slot` path.  Caller MUST gate
// on `!IsShapeC(comp)` — a bind with `value = []` would otherwise
// match this shape spuriously.
bool IsPresizableCollectionAccu(const cel::ComprehensionExpr& comp,
                                const NodeAnnotation& init_ann) {
  if (init_ann.repr != Repr::kList && init_ann.repr != Repr::kMap) return false;
  const auto& accu_init = comp.accu_init();
  if (accu_init.kind_case() == cel::ExprKindCase::kListExpr) {
    return accu_init.list_expr().elements().empty();
  }
  if (accu_init.kind_case() == cel::ExprKindCase::kMapExpr) {
    return accu_init.map_expr().entries().empty();
  }
  return false;
}

absl::StatusOr<CompContext> ResolveCompContext(
    EmitCtx& ctx, const cel::Expr& expr, const cel::ComprehensionExpr& comp,
    const NodeAnnotation& ann) {
  // Set unique labels up front so any early-return doesn't leave
  // them empty; downstream emitters always have a valid string.
  CompContext c{};
  c.exit_label = absl::StrCat("comp_exit_", expr.id());
  c.continue_label = absl::StrCat("comp_continue_", expr.id());
  BindCompVariables(ctx, expr, comp, ann, &c);

  const auto* range_ann = ctx.layout.annotations.Find(comp.iter_range().id());
  const auto* init_ann = ctx.layout.annotations.Find(comp.accu_init().id());
  ABSL_CHECK(range_ann != nullptr && init_ann != nullptr);
  if (range_ann->repr != Repr::kList && range_ann->repr != Repr::kMap) {
    return absl::UnimplementedError(absl::StrCat(
        "expr_lower: comprehension over iter_range with repr=",
        ReprName(range_ann->repr), " is not supported (expr_id=", expr.id(),
        "); Slice C handles list, Slice E handles map"));
  }
  if (range_ann->storage.kind != StorageKind::kWorkspaceSlot) {
    return absl::UnimplementedError(
        absl::StrCat("expr_lower: comprehension iter_range storage kind ",
                     static_cast<int>(range_ann->storage.kind),
                     " not yet supported (expr_id=", expr.id(),
                     "); literal-source paths are workspace-slot only"));
  }
  c.source_slot = range_ann->storage.payload;
  c.map_source = (range_ann->repr == Repr::kMap);
  if (c.map_source) {
    // Sanity: ResolvePass should have re-tagged the iter_var to get
    // a workspace slot (kComprehensionAccu lifecycle).
    ABSL_CHECK(c.iter_v->slot_offset != 0)
        << "LowerComprehension: map-source iter_var `" << comp.iter_var()
        << "` has no workspace slot for key materialisation "
           "(ResolvePass didn't re-tag for kMap source?)";
  }
  if (init_ann->storage.kind != StorageKind::kStaticRodata &&
      init_ann->storage.kind != StorageKind::kWorkspaceSlot) {
    ABSL_CHECK(false) << "LowerComprehension: accu_init storage kind "
                      << static_cast<int>(init_ann->storage.kind);
  }
  c.init_src_slot = init_ann->storage.payload;
  return c;
}

// Emits the prologue: drop iter_range/accu_init values (their side
// effects already ran), copy accu_init → accu_slot, set accu_var
// local, then source-specific iter setup.  For list source:
// iter_off (= iter_var local) + end_off (= aux0_local).  For map
// source: iter handle (= aux0_local), iter_var local set to a
// fixed workspace slot the loop body rewrites via
// cel_map_iter_key_at.
// Slice F helper: which wasm local holds the moving list pointer.
// Single-iter list: iter_v itself doubles as the pointer.
// Two-iter list: iter_v is the synthesized index workspace slot
// (constant), iter_v2 is the moving pointer.
uint32_t ListIterPointerLocal(const CompContext& c) {
  return c.two_iter ? c.iter_v2->local_index : c.iter_v->local_index;
}

// Slice E + F map-source prologue: init handle, point iter_var(s)
// at their workspace slots.  Body's cel_map_iter_{key,value}_at
// calls refresh those slots each iteration.
void EmitMapPrologue(EmitCtx& ctx, const CompContext& c,
                     std::vector<BinaryenExpressionRef>* instrs) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef init_args[1] = {I32Const(ctx.mod, c.source_slot)};
  instrs->push_back(
      BinaryenLocalSet(mod, c.aux0_local,
                       BinaryenCall(mod, "cel_map_iter_init", init_args, 1,
                                    BinaryenTypeInt32())));
  instrs->push_back(BinaryenLocalSet(mod, c.iter_v->local_index,
                                     I32Const(ctx.mod, c.iter_v->slot_offset)));
  if (c.two_iter) {
    instrs->push_back(
        BinaryenLocalSet(mod, c.iter_v2->local_index,
                         I32Const(ctx.mod, c.iter_v2->slot_offset)));
  }
}

// List-source prologue: load list_hdr, derive iter_off + end_off.
// Single-iter: iter_v.local doubles as iter_off (moving pointer).
// Two-iter: iter_v2.local is the pointer; iter_v.local is the
// fixed index-workspace slot offset; aux1_local = index counter.
void EmitListPrologue(EmitCtx& ctx, const CompContext& c,
                      std::vector<BinaryenExpressionRef>* instrs) {
  auto* mod = ctx.mod.raw();
  const uint32_t ptr_local = ListIterPointerLocal(c);
  instrs->push_back(BinaryenLocalSet(
      mod, c.aux0_local,
      BinaryenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/8,
                   /*align=*/4, BinaryenTypeInt32(),
                   I32Const(ctx.mod, c.source_slot), "memory")));
  instrs->push_back(BinaryenLocalSet(
      mod, ptr_local,
      BinaryenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/8,
                   /*align=*/4, BinaryenTypeInt32(),
                   BinaryenLocalGet(mod, c.aux0_local, BinaryenTypeInt32()),
                   "memory")));
  BinaryenExpressionRef count = BinaryenLoad(
      mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/0, /*align=*/4,
      BinaryenTypeInt32(),
      BinaryenLocalGet(mod, c.aux0_local, BinaryenTypeInt32()), "memory");
  instrs->push_back(BinaryenLocalSet(
      mod, c.aux0_local,
      BinaryenBinary(
          mod, BinaryenAddInt32(),
          BinaryenLocalGet(mod, ptr_local, BinaryenTypeInt32()),
          BinaryenBinary(mod, BinaryenMulInt32(), count,
                         BinaryenConst(mod, BinaryenLiteralInt32(24))))));
  if (c.two_iter) {
    instrs->push_back(BinaryenLocalSet(
        mod, c.iter_v->local_index, I32Const(ctx.mod, c.iter_v->slot_offset)));
    instrs->push_back(
        BinaryenLocalSet(mod, c.aux1_local, I32Const(ctx.mod, 0)));
  }
}

// followon §10.A: load `iter_range.count` from the source's
// arena header at runtime.  Both arena-list and arena-map headers
// store the live count at offset 0 of the header, and both CelValue
// payloads place the header pointer at offset 8 of the source slot.
// Returns a fresh i32 expression (the loaded count); caller owns
// further composition.
BinaryenExpressionRef EmitLoadSourceCount(EmitCtx& ctx, const CompContext& c) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef hdr_ptr =
      BinaryenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/8,
                   /*align=*/4, BinaryenTypeInt32(),
                   I32Const(ctx.mod, c.source_slot), "memory");
  return BinaryenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/0,
                      /*align=*/4, BinaryenTypeInt32(), hdr_ptr, "memory");
}

// followon §10.A: emit the pre-sizing call that replaces the
// normal `Emit(accu_init) + cel_copy_slot(accu, init_src)` for
// list/map accumulators.  Calls `cel_list_create_with_capacity` or
// `cel_map_create` with capacity=iter_range.count loaded at
// runtime from the source's arena header.  The downstream runtime
// helpers (`cel_list_append_at`, `cel_map_insert_at`) rely on this
// pre-sizing for their bounded-write invariant.
void EmitPresizeAccu(EmitCtx& ctx, const CompContext& c, bool is_map,
                     std::vector<BinaryenExpressionRef>* instrs) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef count = EmitLoadSourceCount(ctx, c);
  BinaryenExpressionRef args[2] = {I32Const(ctx.mod, c.accu_slot), count};
  const char* helper = is_map ? "cel_map_create" : "cel_list_create";
  instrs->push_back(BinaryenCall(mod, helper, args, 2, BinaryenTypeNone()));
}

void EmitCompPrologue(EmitCtx& ctx, const cel::ComprehensionExpr& comp,
                      const CompContext& c, BinaryenExpressionRef range_value,
                      BinaryenExpressionRef init_value,
                      std::vector<BinaryenExpressionRef>* instrs) {
  auto* mod = ctx.mod.raw();
  instrs->push_back(BinaryenDrop(mod, range_value));
  instrs->push_back(BinaryenDrop(mod, init_value));
  // followon §10.A: collection-shaped accu with empty-literal
  // accu_init → pre-size from iter_range.count.  Predicate
  // inlined here (not stored on CompContext) so the decision
  // lives at the consumer site; caller guarantees we're not on
  // the Shape-C path (LowerComprehension dispatches that earlier).
  const auto* init_ann = ctx.layout.annotations.Find(comp.accu_init().id());
  ABSL_CHECK(init_ann != nullptr);
  if (IsPresizableCollectionAccu(comp, *init_ann)) {
    EmitPresizeAccu(ctx, c, /*is_map=*/init_ann->repr == Repr::kMap, instrs);
  } else {
    instrs->push_back(EmitCelCopySlot(ctx, c.accu_slot, c.init_src_slot));
  }
  instrs->push_back(BinaryenLocalSet(mod, c.accu_v->local_index,
                                     I32Const(ctx.mod, c.accu_slot)));
  if (c.map_source) {
    EmitMapPrologue(ctx, c, instrs);
  } else {
    EmitListPrologue(ctx, c, instrs);
  }
}

// Slice F helper: write a `{kind=CEL_INT, payload.i=local_value}`
// CelValue into `slot`.  Used per-iter for the list-two-iter
// index binding.  CelValue is 24 bytes: kind:u32 at 0, _pad:u32
// at 4, payload starting at offset 8 (int64 in the first 8 bytes).
void EmitWriteIntCelValueToSlot(EmitCtx& ctx, uint32_t slot,
                                uint32_t value_local,
                                std::vector<BinaryenExpressionRef>* out) {
  auto* mod = ctx.mod.raw();
  // kind = CEL_INT (2)
  out->push_back(BinaryenStore(mod, /*bytes=*/4, /*offset=*/0, /*align=*/4,
                               I32Const(ctx.mod, slot),
                               BinaryenConst(mod, BinaryenLiteralInt32(2)),
                               BinaryenTypeInt32(), "memory"));
  // _pad = 0 (defensive; rodata starts zeroed but we're writing in
  // place into a workspace slot whose prior contents are unknown).
  out->push_back(BinaryenStore(mod, /*bytes=*/4, /*offset=*/4, /*align=*/4,
                               I32Const(ctx.mod, slot),
                               BinaryenConst(mod, BinaryenLiteralInt32(0)),
                               BinaryenTypeInt32(), "memory"));
  // payload.i = sign-extend(value_local : i32) → i64 at offset 8.
  out->push_back(BinaryenStore(
      mod, /*bytes=*/8, /*offset=*/8, /*align=*/8, I32Const(ctx.mod, slot),
      BinaryenUnary(mod, BinaryenExtendSInt32(),
                    BinaryenLocalGet(mod, value_local, BinaryenTypeInt32())),
      BinaryenTypeInt64(), "memory"));
}

// Loop-cond peephole detection.  Returns a br_if exit expression for
// the known shapes (kConst bool / @not_strictly_false(accu) /
// @not_strictly_false(!accu)).  Returns Unimplemented for anything
// else — Slice I adds the general path (recurse into loop_cond,
// br_if on inverted result) when cel.bind is registered.
absl::StatusOr<BinaryenExpressionRef> BuildLoopCondExit(
    EmitCtx& ctx, const cel::Expr& comp_expr, const CompContext& c,
    const cel::Expr& loop_cond, absl::string_view accu_name) {
  auto* mod = ctx.mod.raw();
  bool kconst_bool = false;
  if (TryMatchBoolConst(loop_cond, &kconst_bool)) {
    if (kconst_bool) return BinaryenExpressionRef{nullptr};  // no check
    return BinaryenBreak(mod, c.exit_label.c_str(), nullptr, nullptr);
  }
  if (IsNotStrictlyFalseOfNotIdent(loop_cond, accu_name)) {
    return BinaryenBreak(mod, c.exit_label.c_str(),
                         LoadAccuBoolPayload(ctx, c.accu_slot), nullptr);
  }
  if (IsNotStrictlyFalseOfIdent(loop_cond, accu_name)) {
    return BinaryenBreak(mod, c.exit_label.c_str(),
                         BinaryenUnary(mod, BinaryenEqZInt32(),
                                       LoadAccuBoolPayload(ctx, c.accu_slot)),
                         nullptr);
  }
  return absl::UnimplementedError(absl::StrCat(
      "expr_lower: comprehension loop_cond shape not recognised (expr_id=",
      comp_expr.id(),
      ") — Slice C peephole handles kConst bool, @not_strictly_false(@result), "
      "and @not_strictly_false(!@result) only; general path lands in Slice I"));
}

// Returns true if `expr` matches `kCall(_+_, kIdent(accu_name),
// kCreateList(size=1, [elem]))` — the canonical loop_step shape
// emitted by cel-cpp's `map(v, t)` macro (resolved overload
// `add_list`).  On match, sets `*elem_out` to the single element
// subexpression so the caller can recurse into Emit() for it.
//
// Probe-confirmed (m5b probe 2026-05-17): cel-cpp's map and
// filter macros always emit this exact shape — args[0] is the
// accu_var ident, args[1] is a single-element list literal whose
// element IS the transform / iter_var expression.
bool TryMatchAccuAppendOne(const cel::Expr& expr, absl::string_view accu_name,
                           const cel::Expr** elem_out) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& call = expr.call_expr();
  if (call.function() != "_+_" || call.args().size() != 2) return false;
  if (!IsIdentNamed(call.args()[0], accu_name)) return false;
  const auto& rhs = call.args()[1];
  if (rhs.kind_case() != cel::ExprKindCase::kListExpr) return false;
  if (rhs.list_expr().elements().size() != 1) return false;
  *elem_out = &rhs.list_expr().elements()[0].expr();
  return true;
}

// Returns true if `expr` is `kCall(_?_:_, p, append_then,
// kIdent(accu))` where `append_then` matches the append-one shape —
// the `filter(v, p)` / conditional-map loop_step.  On match sets
// `*pred_out` and `*elem_out`.
bool TryMatchAccuConditionalAppendOne(const cel::Expr& expr,
                                      absl::string_view accu_name,
                                      const cel::Expr** pred_out,
                                      const cel::Expr** elem_out) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& call = expr.call_expr();
  if (call.function() != "_?_:_" || call.args().size() != 3) return false;
  if (!IsIdentNamed(call.args()[2], accu_name)) return false;
  if (!TryMatchAccuAppendOne(call.args()[1], accu_name, elem_out)) {
    return false;
  }
  *pred_out = call.args().data();
  return true;
}

// Emits `cel_list_append_at(accu_slot, <elem_eval>)`.  The eval is
// an i32-valued expression whose runtime value is the source slot's
// Three lowering modes for loop_step:
//   1. Append-one (`map(v, t)` step) — emit cel_list_append_at
//      directly.  Skip the kCreateList and kCall(_+_) entirely; the
//      runtime helper propagates value-side errors.
//   2. Conditional-append (`filter(v, p)` / conditional-map) —
//      evaluate predicate; if true, append.  Errors in the
//      predicate poison the comprehension via the wrapping if.
//   3. General path — Emit(loop_step) into a temp slot, then
//      cel_copy_slot.  Used by exists / all / exists_one /
//      cel.bind / transformMap accumulators.
// Slice G: `transformMap(k, v, t)` loop_step is
// `cel.@mapInsert(@result, k, t)` — cel-cpp's macro factory
// emits exactly this shape (extensions/comprehensions_v2_macros.cc).
// On match, sets `*key_out` / `*value_out` to the second / third
// args.
bool TryMatchAccuMapInsert(const cel::Expr& expr, absl::string_view accu_name,
                           const cel::Expr** key_out,
                           const cel::Expr** value_out) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& call = expr.call_expr();
  if (call.function() != "cel.@mapInsert") return false;
  if (call.args().size() != 3) return false;
  if (!IsIdentNamed(call.args()[0], accu_name)) return false;
  *key_out = &call.args()[1];
  *value_out = &call.args()[2];
  return true;
}

// `kCall(_?_:_, p, cel.@mapInsert(...), kIdent(accu))` —
// conditional `transformMap(k, v, p, t)` step.
bool TryMatchAccuConditionalMapInsert(const cel::Expr& expr,
                                      absl::string_view accu_name,
                                      const cel::Expr** pred_out,
                                      const cel::Expr** key_out,
                                      const cel::Expr** value_out) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& call = expr.call_expr();
  if (call.function() != "_?_:_" || call.args().size() != 3) return false;
  if (!IsIdentNamed(call.args()[2], accu_name)) return false;
  if (!TryMatchAccuMapInsert(call.args()[1], accu_name, key_out, value_out)) {
    return false;
  }
  *pred_out = call.args().data();
  return true;
}

// Slice D append-shape: `_+_(@result, [t])`.
absl::Status EmitAppendStep(EmitCtx& ctx, const CompContext& c,
                            const cel::Expr& elem,
                            std::vector<BinaryenExpressionRef>* body) {
  auto elem_or = Emit(ctx, elem);
  if (!elem_or.ok()) return elem_or.status();
  body->push_back(EmitCelListAppendCall(ctx, c.accu_slot, *elem_or));
  return absl::OkStatus();
}

// Slice D filter-shape: `p ? _+_(@result, [t]) : @result`.
absl::Status EmitConditionalAppendStep(
    EmitCtx& ctx, const CompContext& c, const cel::Expr& pred,
    const cel::Expr& elem, std::vector<BinaryenExpressionRef>* body) {
  auto pred_or = Emit(ctx, pred);
  if (!pred_or.ok()) return pred_or.status();
  auto elem_or = Emit(ctx, elem);
  if (!elem_or.ok()) return elem_or.status();
  BinaryenExpressionRef args[3] = {I32Const(ctx.mod, c.accu_slot), *pred_or,
                                   *elem_or};
  body->push_back(BinaryenCall(ctx.mod.raw(), "cel_list_append_at_if_bool",
                               args, 3, BinaryenTypeNone()));
  return absl::OkStatus();
}

// Slice G transformMap step: `cel.@mapInsert(@result, k, t)`.
absl::Status EmitMapInsertStep(EmitCtx& ctx, const CompContext& c,
                               const cel::Expr& key, const cel::Expr& value,
                               std::vector<BinaryenExpressionRef>* body) {
  auto key_or = Emit(ctx, key);
  if (!key_or.ok()) return key_or.status();
  auto value_or = Emit(ctx, value);
  if (!value_or.ok()) return value_or.status();
  BinaryenExpressionRef args[3] = {I32Const(ctx.mod, c.accu_slot), *key_or,
                                   *value_or};
  body->push_back(BinaryenCall(ctx.mod.raw(), "cel_map_insert_at", args, 3,
                               BinaryenTypeNone()));
  return absl::OkStatus();
}

// Slice G conditional transformMap step:
// `p ? cel.@mapInsert(@result, k, t) : @result`.
absl::Status EmitConditionalMapInsertStep(
    EmitCtx& ctx, const CompContext& c, const cel::Expr& pred,
    const cel::Expr& key, const cel::Expr& value,
    std::vector<BinaryenExpressionRef>* body) {
  auto pred_or = Emit(ctx, pred);
  if (!pred_or.ok()) return pred_or.status();
  auto key_or = Emit(ctx, key);
  if (!key_or.ok()) return key_or.status();
  auto value_or = Emit(ctx, value);
  if (!value_or.ok()) return value_or.status();
  // Pred 3VL: defer the `cel_map_insert_at_if_bool` parallel to
  // `cel_list_append_at_if_bool` until a failing corpus row
  // surfaces.  See followon §3.8 plan-vs-execution note.
  const auto* pred_ann = ctx.layout.annotations.Find(pred.id());
  ABSL_CHECK(pred_ann != nullptr);
  const uint32_t pred_slot = pred_ann->storage.payload;
  BinaryenExpressionRef insert_args[3] = {I32Const(ctx.mod, c.accu_slot),
                                          *key_or, *value_or};
  body->push_back(BinaryenDrop(ctx.mod.raw(), *pred_or));
  body->push_back(
      BinaryenIf(ctx.mod.raw(),
                 LoadSlotI32Ne(ctx, pred_slot, /*offset=*/8, /*expected=*/0),
                 BinaryenCall(ctx.mod.raw(), "cel_map_insert_at", insert_args,
                              3, BinaryenTypeNone()),
                 /*ifFalse=*/nullptr));
  return absl::OkStatus();
}

absl::Status EmitCompLoopStep(EmitCtx& ctx, const cel::ComprehensionExpr& comp,
                              const CompContext& c,
                              std::vector<BinaryenExpressionRef>* body) {
  const cel::Expr* elem = nullptr;
  if (TryMatchAccuAppendOne(comp.loop_step(), comp.accu_var(), &elem)) {
    return EmitAppendStep(ctx, c, *elem, body);
  }
  const cel::Expr* pred = nullptr;
  if (TryMatchAccuConditionalAppendOne(comp.loop_step(), comp.accu_var(), &pred,
                                       &elem)) {
    return EmitConditionalAppendStep(ctx, c, *pred, *elem, body);
  }
  const cel::Expr* tm_key = nullptr;
  const cel::Expr* tm_value = nullptr;
  if (TryMatchAccuMapInsert(comp.loop_step(), comp.accu_var(), &tm_key,
                            &tm_value)) {
    return EmitMapInsertStep(ctx, c, *tm_key, *tm_value, body);
  }
  if (TryMatchAccuConditionalMapInsert(comp.loop_step(), comp.accu_var(), &pred,
                                       &tm_key, &tm_value)) {
    return EmitConditionalMapInsertStep(ctx, c, *pred, *tm_key, *tm_value,
                                        body);
  }
  auto step_or = Emit(ctx, comp.loop_step());
  if (!step_or.ok()) return step_or.status();
  const auto* step_ann = ctx.layout.annotations.Find(comp.loop_step().id());
  ABSL_CHECK(step_ann != nullptr &&
             step_ann->storage.kind == StorageKind::kWorkspaceSlot)
      << "LowerComprehension: loop_step storage kind mismatch";
  body->push_back(BinaryenDrop(ctx.mod.raw(), *step_or));
  body->push_back(EmitCelCopySlot(ctx, c.accu_slot, step_ann->storage.payload));
  return absl::OkStatus();
}

// Map-source loop-head: exit on iter_next() == 0; key_at +
// (Slice F) value_at to refresh iter_var(s).
void EmitMapLoopHead(EmitCtx& ctx, const CompContext& c,
                     std::vector<BinaryenExpressionRef>* body) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef next_args[1] = {
      BinaryenLocalGet(mod, c.aux0_local, BinaryenTypeInt32())};
  body->push_back(BinaryenBreak(
      mod, c.exit_label.c_str(),
      BinaryenUnary(mod, BinaryenEqZInt32(),
                    BinaryenCall(mod, "cel_map_iter_next", next_args, 1,
                                 BinaryenTypeInt32())),
      nullptr));
  BinaryenExpressionRef key_args[2] = {
      I32Const(ctx.mod, c.iter_v->slot_offset),
      BinaryenLocalGet(mod, c.aux0_local, BinaryenTypeInt32())};
  body->push_back(BinaryenCall(mod, "cel_map_iter_key_at", key_args, 2,
                               BinaryenTypeNone()));
  if (c.two_iter) {
    BinaryenExpressionRef val_args[2] = {
        I32Const(ctx.mod, c.iter_v2->slot_offset),
        BinaryenLocalGet(mod, c.aux0_local, BinaryenTypeInt32())};
    body->push_back(BinaryenCall(mod, "cel_map_iter_value_at", val_args, 2,
                                 BinaryenTypeNone()));
  }
}

// List-source loop-head: exit on iter_off >= end_off; Slice F
// two-iter also materialises {CEL_INT, i=index} in iter_var's slot.
void EmitListLoopHead(EmitCtx& ctx, const CompContext& c,
                      std::vector<BinaryenExpressionRef>* body) {
  auto* mod = ctx.mod.raw();
  const uint32_t ptr_local = ListIterPointerLocal(c);
  body->push_back(BinaryenBreak(
      mod, c.exit_label.c_str(),
      BinaryenBinary(mod, BinaryenGeUInt32(),
                     BinaryenLocalGet(mod, ptr_local, BinaryenTypeInt32()),
                     BinaryenLocalGet(mod, c.aux0_local, BinaryenTypeInt32())),
      nullptr));
  if (c.two_iter) {
    EmitWriteIntCelValueToSlot(ctx, c.iter_v->slot_offset, c.aux1_local, body);
  }
}

// List-source loop-tail: advance moving pointer + bump index for
// Slice F two-iter.  Map source's iter advance happens at the
// head via cel_map_iter_next; nothing to do here.
void EmitListLoopTail(EmitCtx& ctx, const CompContext& c,
                      std::vector<BinaryenExpressionRef>* body) {
  auto* mod = ctx.mod.raw();
  const uint32_t ptr_local = ListIterPointerLocal(c);
  body->push_back(BinaryenLocalSet(
      mod, ptr_local,
      BinaryenBinary(mod, BinaryenAddInt32(),
                     BinaryenLocalGet(mod, ptr_local, BinaryenTypeInt32()),
                     BinaryenConst(mod, BinaryenLiteralInt32(24)))));
  if (c.two_iter) {
    body->push_back(BinaryenLocalSet(
        mod, c.aux1_local,
        BinaryenBinary(mod, BinaryenAddInt32(),
                       BinaryenLocalGet(mod, c.aux1_local, BinaryenTypeInt32()),
                       BinaryenConst(mod, BinaryenLiteralInt32(1)))));
  }
}

absl::StatusOr<BinaryenExpressionRef> BuildCompLoop(
    EmitCtx& ctx, const cel::Expr& expr, const cel::ComprehensionExpr& comp,
    const CompContext& c) {
  auto* mod = ctx.mod.raw();
  std::vector<BinaryenExpressionRef> body;
  if (c.map_source) {
    EmitMapLoopHead(ctx, c, &body);
  } else {
    EmitListLoopHead(ctx, c, &body);
  }
  auto cond_exit_or =
      BuildLoopCondExit(ctx, expr, c, comp.loop_condition(), comp.accu_var());
  if (!cond_exit_or.ok()) return cond_exit_or.status();
  if (*cond_exit_or != nullptr) body.push_back(*cond_exit_or);
  if (auto status = EmitCompLoopStep(ctx, comp, c, &body); !status.ok()) {
    return status;
  }
  if (!c.map_source) EmitListLoopTail(ctx, c, &body);
  body.push_back(
      BinaryenBreak(mod, c.continue_label.c_str(), nullptr, nullptr));
  BinaryenExpressionRef loop =
      BinaryenLoop(mod, c.continue_label.c_str(),
                   BinaryenBlock(mod, /*name=*/nullptr, body.data(),
                                 static_cast<BinaryenIndex>(body.size()),
                                 BinaryenTypeNone()));
  BinaryenExpressionRef loop_arr[1] = {loop};
  return BinaryenBlock(mod, c.exit_label.c_str(), loop_arr, 1,
                       BinaryenTypeNone());
}

// M5.B Slice I — Shape-C detector: `iter_range = kCreateList([])
// AND loop_cond = kConst(false)` (the `cel.bind` macro
// expansion).  Loop body never runs at runtime.  Codegen emits a
// streamlined no-loop sequence: evaluate `value` into accu_var's
// slot, set the local pointer, evaluate `body` (result).
bool IsShapeC(const cel::ComprehensionExpr& comp) {
  if (!comp.has_iter_range() || !comp.has_loop_condition()) return false;
  const cel::Expr& range = comp.iter_range();
  if (range.kind_case() != cel::ExprKindCase::kListExpr) return false;
  if (!range.list_expr().elements().empty()) return false;
  const cel::Expr& cond = comp.loop_condition();
  if (cond.kind_case() != cel::ExprKindCase::kConstant) return false;
  return cond.const_expr().has_bool_value() && !cond.const_expr().bool_value();
}

// Streamlined emission for the `cel.bind(name, value, body)`
// shape: evaluate `value` (which cel-cpp stores as accu_init),
// copy it into accu_var's workspace slot, set accu_var's wasm
// local to the slot offset (so kIdent(accu_var) inside `body`
// reads it via `local.get`), then evaluate `body` (the
// comprehension's `result`).  No loop scaffold.  Per design §5
// Shape C / §6 macro #8.
absl::StatusOr<BinaryenExpressionRef> LowerShapeC(
    EmitCtx& ctx, const cel::ComprehensionExpr& comp, const CompContext& c) {
  auto* mod = ctx.mod.raw();
  auto init_or = Emit(ctx, comp.accu_init());
  if (!init_or.ok()) return init_or.status();
  const auto* init_ann = ctx.layout.annotations.Find(comp.accu_init().id());
  ABSL_CHECK(init_ann != nullptr);
  std::vector<BinaryenExpressionRef> instrs;
  instrs.push_back(BinaryenDrop(mod, *init_or));
  instrs.push_back(
      EmitCelCopySlot(ctx, c.accu_slot, init_ann->storage.payload));
  instrs.push_back(BinaryenLocalSet(mod, c.accu_v->local_index,
                                    I32Const(ctx.mod, c.accu_slot)));
  auto result_or = Emit(ctx, comp.result());
  if (!result_or.ok()) return result_or.status();
  instrs.push_back(*result_or);
  return BinaryenBlock(mod, /*name=*/nullptr, instrs.data(),
                       static_cast<BinaryenIndex>(instrs.size()),
                       BinaryenTypeInt32());
}

absl::StatusOr<BinaryenExpressionRef> LowerComprehension(
    EmitCtx& ctx, const cel::Expr& expr, const cel::ComprehensionExpr& comp,
    const NodeAnnotation& ann) {
  auto cctx_or = ResolveCompContext(ctx, expr, comp, ann);
  if (!cctx_or.ok()) return cctx_or.status();
  const CompContext& c = *cctx_or;
  if (IsShapeC(comp)) return LowerShapeC(ctx, comp, c);
  auto range_or = Emit(ctx, comp.iter_range());
  if (!range_or.ok()) return range_or.status();
  auto init_or = Emit(ctx, comp.accu_init());
  if (!init_or.ok()) return init_or.status();
  std::vector<BinaryenExpressionRef> instrs;
  EmitCompPrologue(ctx, comp, c, *range_or, *init_or, &instrs);
  auto loop_or = BuildCompLoop(ctx, expr, comp, c);
  if (!loop_or.ok()) return loop_or.status();
  instrs.push_back(*loop_or);
  auto result_or = Emit(ctx, comp.result());
  if (!result_or.ok()) return result_or.status();
  instrs.push_back(*result_or);
  return BinaryenBlock(ctx.mod.raw(), /*name=*/nullptr, instrs.data(),
                       static_cast<BinaryenIndex>(instrs.size()),
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
      return LowerComprehension(ctx, expr, expr.comprehension_expr(), *ann);
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
  // Slice M2.B), plus per-comprehension auxiliary locals reserved
  // by LayoutPass's `ComprehensionLocalsVisitor` (M5.B Slice C) for
  // end_off / iter cursor / index counter.
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
