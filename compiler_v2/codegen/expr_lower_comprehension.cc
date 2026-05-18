// expr_lower_comprehension.cc — codegen for `kComprehensionExpr`.
//
// Split out from expr_lower.cc post-M5.B (commit 90a01cc).  The
// surface here implements all comprehension shapes — exists / all /
// exists_one / map / filter / transformList / transformMap /
// transformMapEntry — over both list and map sources, plus cel.bind
// via the Shape-C fast path.  See
// `doc/implementation-plan/rewrite/m5-comprehensions-followon.md`
// for the slice-by-slice design.
//
// All shared primitives (`EmitCtx`, `I32Const`, `EmitCelCopySlot`,
// `LoadSlotI32{Eq,Ne}`, `EmitCelListAppendCall`, and the top-level
// `Emit` dispatcher) come in via `expr_lower_internal.h`.  The
// entry point exported back to expr_lower.cc is
// `LowerComprehension`, called from the kComprehensionExpr arm of
// `Emit`.
//
// Planned simplifications (analysis-only, not yet executed) are
// tracked in `m5b-comprehensions-simplification.md`.

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
#include "compiler_v2/codegen/expr_lower.h"
#include "compiler_v2/codegen/expr_lower_internal.h"
#include "compiler_v2/codegen/layout_pass.h"
#include "compiler_v2/codegen/module.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {
namespace {

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

// Collection-shaped accu (list or map) with an empty-literal
// accu_init — one of map / filter / transformList / transformMap /
// transformMapEntry.  Matches go through the pre-sized prologue
// (`cel_list_create` / `cel_map_create` with capacity computed
// from iter_range.count); everything else falls back to the
// generic `Emit(accu_init) + cel_copy_slot` path.  cel.bind with
// an empty-literal value (`cel.bind(x, [], body)`) is correctly
// handled by this same path: iter_range is also `[]` so the
// pre-sized capacity is 0, and the loop body never executes.
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

// Forward declarations — the matchers themselves live further down
// (in the loop_step section), but PerIterEntryCount below needs to
// peek at the loop_step shape from the prologue's call site.
bool TryMatchAccuMapInsertEntries(const cel::Expr& expr,
                                  absl::string_view accu_name,
                                  const cel::Expr** entry_out);
bool TryMatchAccuConditionalMapInsertEntries(const cel::Expr& expr,
                                             absl::string_view accu_name,
                                             const cel::Expr** pred_out,
                                             const cel::Expr** entry_out);

// followon §10.A: returns the per-iter capacity multiplier for the
// accu given the loop_step shape.
//   - `map` / `filter` / `transformList` list accus: 1 (each iter
//     contributes ≤1 element; the helper already covers that).
//   - `transformMap` 3-arg / 4-arg (`cel.@mapInsert(@result, k, v)`
//     or its conditional wrap): 1 entry per iter — same as
//     transformMap's design contract.
//   - `transformMapEntry` (`cel.@mapInsert(@result, entry)` where
//     entry is a `kMapExpr` literal): `entry.size()` entries per
//     iter — empty `{}` is 0 (no-op), single-key `{k': t}` is 1,
//     multi-key `{k1: t1, k2: t2}` is 2, etc.  The literal-only
//     case is what cel-cpp's transformMapEntry macro emits in
//     practice; computed (non-literal) entry expressions are
//     deferred to the runtime map-merge follow-up.
// Any unrecognised shape returns 1 — conservative default
// matching the pre-Slice-H behaviour.
uint32_t PerIterEntryCount(const cel::ComprehensionExpr& comp) {
  const cel::Expr* entry = nullptr;
  if (TryMatchAccuMapInsertEntries(comp.loop_step(), comp.accu_var(), &entry) &&
      entry->kind_case() == cel::ExprKindCase::kMapExpr) {
    return static_cast<uint32_t>(entry->map_expr().entries().size());
  }
  const cel::Expr* pred = nullptr;
  if (TryMatchAccuConditionalMapInsertEntries(comp.loop_step(), comp.accu_var(),
                                              &pred, &entry) &&
      entry->kind_case() == cel::ExprKindCase::kMapExpr) {
    return static_cast<uint32_t>(entry->map_expr().entries().size());
  }
  return 1;
}

// followon §10.A: emit the pre-sizing call that replaces the normal
// `Emit(accu_init) + cel_copy_slot(accu, init_src)` for list/map
// accumulators.  Calls `cel_list_create` or `cel_map_create` with
// `capacity = iter_range.count * per_iter` loaded at runtime.  The
// downstream runtime helpers (`cel_list_append_at`,
// `cel_map_insert_at`) rely on this pre-sizing for their bounded-
// write invariant.  `per_iter` defaults to 1 for list accus;
// transformMapEntry passes `entry.size()` so a multi-entry literal
// expands capacity correctly.
void EmitPresizeAccu(EmitCtx& ctx, const CompContext& c, bool is_map,
                     uint32_t per_iter,
                     std::vector<BinaryenExpressionRef>* instrs) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef count = EmitLoadSourceCount(ctx, c);
  if (per_iter != 1) {
    count = BinaryenBinary(mod, BinaryenMulInt32(), count,
                           I32Const(ctx.mod, per_iter));
  }
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
    EmitPresizeAccu(ctx, c, /*is_map=*/init_ann->repr == Repr::kMap,
                    /*per_iter=*/PerIterEntryCount(comp), instrs);
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

// Slice H: `cel.@mapInsert(@result, entry_map)` — the 2-arg map-merge
// form emitted by transformMapEntry (cel-cpp
// extensions/comprehensions_v2_macros.cc:420).  On match sets
// `*entry_out` to the entry expression (typically a kMapExpr).
bool TryMatchAccuMapInsertEntries(const cel::Expr& expr,
                                  absl::string_view accu_name,
                                  const cel::Expr** entry_out) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& call = expr.call_expr();
  if (call.function() != "cel.@mapInsert") return false;
  if (call.args().size() != 2) return false;
  if (!IsIdentNamed(call.args()[0], accu_name)) return false;
  *entry_out = &call.args()[1];
  return true;
}

// Slice H conditional form: `p ? cel.@mapInsert(@result, entry) :
// @result` — the 4-arg `transformMapEntry(k, v, p, entry)` step.
bool TryMatchAccuConditionalMapInsertEntries(const cel::Expr& expr,
                                             absl::string_view accu_name,
                                             const cel::Expr** pred_out,
                                             const cel::Expr** entry_out) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& call = expr.call_expr();
  if (call.function() != "_?_:_" || call.args().size() != 3) return false;
  if (!IsIdentNamed(call.args()[2], accu_name)) return false;
  if (!TryMatchAccuMapInsertEntries(call.args()[1], accu_name, entry_out)) {
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

// Slice G conditional transformMap step: `p ? cel.@mapInsert(@result,
// k, t) : @result`.  Uses the 3VL-aware `cel_map_insert_at_if_bool`
// runtime helper so an ERROR / UNKNOWN predicate propagates into the
// accu slot (aborting the comprehension per design §3.2) rather than
// being silently interpreted as a bool.  Surfaced by the
// macros2/transformMap/error_filter conformance row:
// `{...}.transformMap(k, v, k=='baz' && 4/v==0, v)` where v=0
// produces a div-by-zero predicate error.
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
  BinaryenExpressionRef args[4] = {I32Const(ctx.mod, c.accu_slot), *pred_or,
                                   *key_or, *value_or};
  body->push_back(BinaryenCall(ctx.mod.raw(), "cel_map_insert_at_if_bool", args,
                               4, BinaryenTypeNone()));
  return absl::OkStatus();
}

// Slice H: `cel.@mapInsert(@result, entry)` step, generalized over
// entry size.  Entry must be a `kMapExpr` literal — Slice H scope;
// computed (non-literal) entry expressions need a runtime map-
// merge helper, deferred.  Emits one `cel_map_insert_at(accu, k_i,
// v_i)` per entry: size 0 is a no-op iter (langdef §9.7), size 1
// is the transformMap-equivalent shape, size N>1 is N sequential
// inserts.  Capacity is pre-sized to `iter_range.count *
// entry.size()` (see `PerIterEntryCount` + `EmitPresizeAccu`), so
// PRESIZE_INVARIANT holds across all entry shapes.
absl::Status EmitMapInsertEntriesStep(
    EmitCtx& ctx, const CompContext& c, const cel::Expr& entry,
    std::vector<BinaryenExpressionRef>* body) {
  if (entry.kind_case() != cel::ExprKindCase::kMapExpr) {
    ABSL_CHECK(false)
        << "transformMapEntry with non-literal entry expression is a stub "
           "until Slice H.2 (runtime map-merge helper) — entry kind "
        << static_cast<int>(entry.kind_case());
    return absl::OkStatus();
  }
  const auto& entries = entry.map_expr().entries();
  if (entries.empty()) {
    body->push_back(BinaryenNop(ctx.mod.raw()));
    return absl::OkStatus();
  }
  for (const auto& e : entries) {
    absl::Status s = EmitMapInsertStep(ctx, c, e.key(), e.value(), body);
    if (!s.ok()) return s;
  }
  return absl::OkStatus();
}

// Slice H conditional form: `p ? cel.@mapInsert(@result, entry) :
// @result`.  Size-0 evaluates pred for side-effects and drops.
// Size-1 routes through Slice G's `cel_map_insert_at_if_bool` (3VL
// pred + single insert).  Size-N>1 needs the 3VL ladder to gate
// the whole N-insert sequence atomically — the single-pair
// `_if_bool` helper does not express that — so defer to a runtime
// follow-up; CHECK so the surfacing corpus row is actionable.
absl::Status EmitConditionalMapInsertEntriesStep(
    EmitCtx& ctx, const CompContext& c, const cel::Expr& pred,
    const cel::Expr& entry, std::vector<BinaryenExpressionRef>* body) {
  if (entry.kind_case() != cel::ExprKindCase::kMapExpr) {
    ABSL_CHECK(false)
        << "conditional transformMapEntry with non-literal entry is a stub "
           "until Slice H.2 — entry kind "
        << static_cast<int>(entry.kind_case());
    return absl::OkStatus();
  }
  const auto& entries = entry.map_expr().entries();
  if (entries.empty()) {
    auto pred_or = Emit(ctx, pred);
    if (!pred_or.ok()) return pred_or.status();
    body->push_back(BinaryenDrop(ctx.mod.raw(), *pred_or));
    return absl::OkStatus();
  }
  if (entries.size() == 1) {
    return EmitConditionalMapInsertStep(ctx, c, pred, entries[0].key(),
                                        entries[0].value(), body);
  }
  ABSL_CHECK(false)
      << "conditional transformMapEntry with multi-key entry (N>1) is a stub "
         "until Slice H.2 — the 3VL ladder must gate the whole N-insert "
         "sequence atomically; cel_map_insert_at_if_bool handles only single "
         "(k,v).  entries.size()="
      << entries.size();
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
  // Slice H: transformMapEntry — `cel.@mapInsert(@result, entry)`
  // and the conditional wrapper.  Entry shape is either:
  //   - empty `{}` → no-op for this iter (BinaryenNop);
  //   - single-key `{k': t}` → route through Slice G's
  //     `cel_map_insert_at(accu, k', t)`;
  //   - multi-key `{k1: t1, k2: t2, …}` → stub until Slice H.2,
  //     per followon §Slice H "general path deferred" note.
  const cel::Expr* tme_entry = nullptr;
  if (TryMatchAccuMapInsertEntries(comp.loop_step(), comp.accu_var(),
                                   &tme_entry)) {
    return EmitMapInsertEntriesStep(ctx, c, *tme_entry, body);
  }
  if (TryMatchAccuConditionalMapInsertEntries(comp.loop_step(), comp.accu_var(),
                                              &pred, &tme_entry)) {
    return EmitConditionalMapInsertEntriesStep(ctx, c, *pred, *tme_entry, body);
  }
  auto step_or = Emit(ctx, comp.loop_step());
  if (!step_or.ok()) return step_or.status();
  const auto* step_ann = ctx.layout.annotations.Find(comp.loop_step().id());
  ABSL_CHECK(step_ann != nullptr);
  body->push_back(BinaryenDrop(ctx.mod.raw(), *step_or));
  // kLocal storage (cel.bind's loop_step is `kIdent(accu_var)` whose
  // value lives in a wasm local, not a workspace slot) — the local
  // already holds the accu's slot offset, so no copy is needed.
  // Reaching this arm at runtime would mean re-binding accu to
  // itself; cel.bind's empty iter_range means the loop body never
  // executes anyway, so the no-op is purely a codegen-time placeholder.
  if (step_ann->storage.kind == StorageKind::kWorkspaceSlot) {
    body->push_back(
        EmitCelCopySlot(ctx, c.accu_slot, step_ann->storage.payload));
  }
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

}  // namespace

absl::StatusOr<BinaryenExpressionRef> LowerComprehension(
    EmitCtx& ctx, const cel::Expr& expr, const cel::ComprehensionExpr& comp,
    const NodeAnnotation& ann) {
  auto cctx_or = ResolveCompContext(ctx, expr, comp, ann);
  if (!cctx_or.ok()) return cctx_or.status();
  const CompContext& c = *cctx_or;
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

}  // namespace celwasm
