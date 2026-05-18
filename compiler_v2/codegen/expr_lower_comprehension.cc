// expr_lower_comprehension.cc — wasm codegen for kComprehensionExpr.
//
// Covers every comprehension cel-cpp can emit: exists / all /
// exists_one / map / filter (standard macros), transformList /
// transformMap / transformMapEntry (comprehensions_v2), and
// cel.bind (bindings_ext).  See `m5-comprehensions-followon.md`
// for the design; `m5b-comprehensions-simplification.md` tracks
// pending simplifications.
//
// ── Emission shape ────────────────────────────────────────────
//
// Every comprehension lowers to a single (block (result i32))
// holding three regions:
//
//   1. Prologue (runs once)
//      - Eval iter_range → workspace slot, drop value.
//      - Either pre-size the accu (collection-accu shapes) via
//        cel_list_create / cel_map_create with capacity =
//        iter_range.count × per-iter-entry-count, OR copy the
//        evaluated accu_init via cel_copy_slot.
//      - Bind accu_var.local to accu_slot.
//      - List source: load elements_ptr → iter_local, compute
//        end_local = elements_ptr + count * sizeof(CelValue).
//      - Map source: cel_map_iter_init(map_slot) → handle local;
//        bind iter_var.local (+iter_var2.local for v2) to their
//        workspace slots so cel_map_iter_{key,value}_at writes
//        find them.
//
//   2. (block exit (loop continue ...)) — runs per iter
//      - Loop-cond peephole: one of {const true → omit; const
//        false → immediate exit; @not_strictly_false(@result) →
//        br_if exit when accu's bool payload is 0;
//        @not_strictly_false(!@result) → br_if exit when bool
//        payload is non-0}.  No runtime helper call.
//      - Iter-step:
//          List: br_if exit when iter == end; advance pointer.
//          Map: br_if exit when cel_map_iter_next == 0; key_at +
//          (v2) value_at refresh the iter_var slot(s).
//      - Loop-step dispatched by AST shape via `EmitCompLoopStep`:
//          map(v, t)              → cel_list_append_at
//          filter(v, p)           → cel_list_append_at_if_bool
//          transformMap(k,v, t)   → cel_map_insert_at
//          transformMap(k,v, p,t) → cel_map_insert_at_if_bool
//          transformMapEntry      → N inserts per entry literal
//          exists / all / etc.    → eval loop_step, copy to accu
//
//   3. Result expression — evaluated as the block's i32 value
//      (the slot offset of comp.result()'s CelValue).
//
// ── Per-comp state ───────────────────────────────────────────
//
// `CompContext` (populated by ResolveCompContext) carries:
//   - accu_v / iter_v / iter_v2: LaidOutVariable pointers from
//     ResolvePass — `slot_offset` is the workspace slot,
//     `local_index` is the wasm local that holds it.
//   - source_slot: where iter_range's CelValue lives.
//   - aux0_local / aux1_local: end-pointer (list), iter-handle
//     (map), or index-counter (list two-iter); usage depends on
//     `map_source` + `two_iter`.
//   - exit_label / continue_label: per-comp unique (suffixed by
//     expr_id) so Binaryen's nested-label validator accepts
//     same-name comprehensions inside each other.
//
// ── Pre-sizing invariant ─────────────────────────────────────
//
// Collection-accu comprehensions never grow the accu at runtime:
// codegen sizes capacity = iter_range.count × per_iter.  The
// runtime's cel_list_append_at / cel_map_insert_at trap via
// __builtin_trap if count >= capacity (regression tripwire).
// Per-iter multiplier is 1 for map/filter/transformMap and
// entry.size() for transformMapEntry literal entries.  See
// `IsPresizableCollectionAccu` + `PerIterEntryCount` below.
//
// ── Shared primitives ────────────────────────────────────────
//
// `EmitCtx`, `Emit` dispatcher, `I32Const`, `EmitCelCopySlot`,
// `LoadSlotI32{Eq,Ne}`, `EmitCelListAppendCall` come in via
// `expr_lower_internal.h`.  The entry point exported back to
// expr_lower.cc is `LowerComprehension`, called from the
// kComprehensionExpr arm of `Emit`.

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

// ── Loop-cond peephole ──────────────────────────────────────
//
// cel-cpp's macro expansions only ever emit one of these four
// loop_cond shapes; the peephole reads `i32.load offset=8` of
// accu_slot directly, avoiding a `cel_not_strictly_false`
// runtime helper.  Anything else returns UnimplementedError.
//
//   kConst(true)                              → no cond check
//   kConst(false)                             → immediate exit
//   @not_strictly_false(@result)              → exit when accu's
//                                               bool payload is 0
//                                               (`all`)
//   @not_strictly_false(!@result)             → exit when accu's
//                                               bool payload is
//                                               non-0 (`exists`)

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

// Per-comprehension binding context.  Slots and locals come from
// ResolvePass + LayoutPass; codegen reads this to emit the prologue
// and loop scaffold.  Field roles by source:
//
//                       single iter      two iter (v2 macros)
//   list source         iter_v = moving  iter_v = index counter
//                       elements pointer iter_v2 = moving pointer
//   map source          iter_v = key     iter_v = key
//                       slot             iter_v2 = value slot
//                                        (both refreshed per iter
//                                         by cel_map_iter_{key,
//                                         value}_at)
//
// aux0_local: end_off for list source (one-past-end pointer),
//             handle for map source (returned by cel_map_iter_init).
// aux1_local: raw-int index counter for list two-iter case;
//             unused otherwise.
struct CompContext {
  const LaidOutVariable* iter_v = nullptr;
  const LaidOutVariable* iter_v2 = nullptr;
  const LaidOutVariable* accu_v = nullptr;
  uint32_t aux0_local = 0;
  uint32_t aux1_local = 0;
  uint32_t accu_slot = 0;
  uint32_t source_slot = 0;
  uint32_t init_src_slot = 0;
  bool map_source = false;
  bool two_iter = false;
  // Per-comp unique labels (suffixed by expr_id).  Nested same-name
  // comprehensions would otherwise trip Binaryen's
  // visitLoop "iter != breakTypes.end()" check.
  std::string exit_label;
  std::string continue_label;
};

// Resolve iter / iter2 / accu LaidOutVariables via the comp node's
// stamped local indices.  Indexing by NAME would conflate nested
// same-name accu_vars (cel-cpp uses "@result" at every nesting
// depth); the indices stamped by ResolvePass are per-comp unique.
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

// Collection-accu shapes (map/filter/transformList/transformMap/
// transformMapEntry) take the pre-sized prologue.  cel.bind with
// an empty-literal value also lands here harmlessly — iter_range is
// `[]` too, so capacity = 0 and the loop body never runs.
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
        "); only list and map sources are handled"));
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
// Which wasm local holds the moving elements pointer.  Single-iter:
// iter_v itself.  Two-iter (v2): iter_v2 — iter_v is the synthesized
// index counter slot.
uint32_t ListIterPointerLocal(const CompContext& c) {
  return c.two_iter ? c.iter_v2->local_index : c.iter_v->local_index;
}

// Map-source prologue.  cel_map_iter_init returns a handle stored
// in aux0_local; iter_var (+ iter_var2 for v2) are bound to fixed
// workspace slots that cel_map_iter_{key,value}_at refresh each
// iteration.
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

// List-source prologue.  Loads the arena-list header pointer
// (CelValue.payload offset 8), then derives:
//   ptr_local = elements pointer (header offset 8)
//   aux0_local = ptr_local + count * sizeof(CelValue) = one-past-end
// For v2 two-iter: also seeds iter_v's index-counter slot + aux1.
// CelValue size is 24 bytes — hardcoded here; if the runtime layout
// shifts the per-iter advance in EmitListLoopTail must change too.
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

// Load iter_range.count at runtime.  Arena-list and arena-map
// headers both store count at header offset 0; both CelValue payloads
// store the header pointer at slot offset 8.  Uniform two-load shape.
BinaryenExpressionRef EmitLoadSourceCount(EmitCtx& ctx, const CompContext& c) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef hdr_ptr =
      BinaryenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/8,
                   /*align=*/4, BinaryenTypeInt32(),
                   I32Const(ctx.mod, c.source_slot), "memory");
  return BinaryenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/0,
                      /*align=*/4, BinaryenTypeInt32(), hdr_ptr, "memory");
}

// Forward decls — the matchers live below (loop_step section); the
// prologue's PerIterEntryCount needs to peek at their shape.
bool TryMatchAccuMapInsertEntries(const cel::Expr& expr,
                                  absl::string_view accu_name,
                                  const cel::Expr** entry_out);
bool TryMatchAccuConditionalMapInsertEntries(const cel::Expr& expr,
                                             absl::string_view accu_name,
                                             const cel::Expr** pred_out,
                                             const cel::Expr** entry_out);

// Per-iter entries the accu may receive — drives the pre-size
// capacity multiplier.  1 for map/filter/transformMap (single
// element/entry per iter); entry.size() for transformMapEntry with
// a literal map entry (empty=0 no-op, N>1 means N inserts per iter).
// Unrecognised shapes fall through to 1.
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

// Pre-size the collection accu in lieu of copying an empty literal:
// capacity = iter_range.count * per_iter, loaded at runtime.  Append /
// insert helpers trap if count exceeds this — the pre-size and the
// trap together form the codegen-runtime invariant pair.
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

// Materialise {kind=CEL_INT, payload.i=value_local} in `slot`.
// Used per-iter for the v2 two-iter list-source index binding.
// CelValue layout: kind:u32 at 0, _pad at 4, payload (int64) at 8.
void EmitWriteIntCelValueToSlot(EmitCtx& ctx, uint32_t slot,
                                uint32_t value_local,
                                std::vector<BinaryenExpressionRef>* out) {
  auto* mod = ctx.mod.raw();
  // kind = CEL_INT
  out->push_back(BinaryenStore(mod, /*bytes=*/4, /*offset=*/0, /*align=*/4,
                               I32Const(ctx.mod, slot),
                               BinaryenConst(mod, BinaryenLiteralInt32(2)),
                               BinaryenTypeInt32(), "memory"));
  // _pad: workspace slot's prior contents are unknown, zero
  // defensively even though rodata starts zeroed.
  out->push_back(BinaryenStore(mod, /*bytes=*/4, /*offset=*/4, /*align=*/4,
                               I32Const(ctx.mod, slot),
                               BinaryenConst(mod, BinaryenLiteralInt32(0)),
                               BinaryenTypeInt32(), "memory"));
  // payload.i — sign-extend the i32 local into the int64 slot.
  out->push_back(BinaryenStore(
      mod, /*bytes=*/8, /*offset=*/8, /*align=*/8, I32Const(ctx.mod, slot),
      BinaryenUnary(mod, BinaryenExtendSInt32(),
                    BinaryenLocalGet(mod, value_local, BinaryenTypeInt32())),
      BinaryenTypeInt64(), "memory"));
}

// Returns the br_if-exit expression for the recognised loop_cond
// shapes (see file header).  nullptr means "no check" (kConst true).
// Unrecognised shapes return UnimplementedError — cel-cpp's macros
// don't currently emit any other shape, but a user-authored
// comprehension via the v2 API could.
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
      ") — only kConst bool, @not_strictly_false(@result), and "
      "@not_strictly_false(!@result) peephole shapes are handled"));
}

// `map(v, t)` loop_step shape: `_+_(@result, [t])`.  cel-cpp
// resolves this to the add_list overload; rewriting at codegen
// to a direct append avoids the O(N) list-concat per iter.
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

// `filter(v, p)` loop_step: `p ? _+_(@result, [t]) : @result`.
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

// `transformMap(k, v, t)` loop_step: `cel.@mapInsert(@result, k, t)`.
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

// `transformMap(k, v, p, t)` loop_step:
// `p ? cel.@mapInsert(@result, k, t) : @result`.
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

// `transformMapEntry(k, v, entry)` loop_step: 2-arg map-merge form
// `cel.@mapInsert(@result, entry)`.  Entry is typically a kMapExpr
// literal; computed entries are not supported (see EmitMapInsertEntriesStep).
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

// `transformMapEntry(k, v, p, entry)` loop_step:
// `p ? cel.@mapInsert(@result, entry) : @result`.
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

absl::Status EmitAppendStep(EmitCtx& ctx, const CompContext& c,
                            const cel::Expr& elem,
                            std::vector<BinaryenExpressionRef>* body) {
  auto elem_or = Emit(ctx, elem);
  if (!elem_or.ok()) return elem_or.status();
  body->push_back(EmitCelListAppendCall(ctx, c.accu_slot, *elem_or));
  return absl::OkStatus();
}

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

// Routes through cel_map_insert_at_if_bool so a 3VL predicate
// (ERROR/UNKNOWN) propagates into the accu slot — aborting the
// comprehension — rather than being silently interpreted as a bool.
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

// transformMapEntry step.  Entry must be a kMapExpr literal —
// computed entries need a runtime map-merge helper, not yet shipped.
// Emits one cel_map_insert_at per entry (size 0 = no-op iter per
// langdef §"Comprehension Macros"; size N = N sequential inserts).
absl::Status EmitMapInsertEntriesStep(
    EmitCtx& ctx, const CompContext& c, const cel::Expr& entry,
    std::vector<BinaryenExpressionRef>* body) {
  if (entry.kind_case() != cel::ExprKindCase::kMapExpr) {
    ABSL_CHECK(false)
        << "transformMapEntry: non-literal entry expression unsupported "
           "(needs runtime map-merge helper); entry kind="
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

// Conditional transformMapEntry.  Size 0: eval pred for side-effects
// + drop.  Size 1: route through cel_map_insert_at_if_bool (3VL).
// Size N>1: would need a 3VL ladder gating N inserts atomically;
// cel_map_insert_at_if_bool only handles one (k,v).  Deferred.
absl::Status EmitConditionalMapInsertEntriesStep(
    EmitCtx& ctx, const CompContext& c, const cel::Expr& pred,
    const cel::Expr& entry, std::vector<BinaryenExpressionRef>* body) {
  if (entry.kind_case() != cel::ExprKindCase::kMapExpr) {
    ABSL_CHECK(false)
        << "conditional transformMapEntry: non-literal entry unsupported; "
           "entry kind="
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
      << "conditional transformMapEntry: multi-key entry (N>1) needs a "
         "3VL ladder gating N inserts atomically; not yet shipped.  "
         "entries.size()="
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
  // kLocal storage: the local already holds the accu's slot offset;
  // copy would be a no-op.  Lands here for cel.bind's
  // `loop_step = kIdent(accu_var)` — but cel.bind's empty iter_range
  // means the loop body never executes at runtime anyway.
  if (step_ann->storage.kind == StorageKind::kWorkspaceSlot) {
    body->push_back(
        EmitCelCopySlot(ctx, c.accu_slot, step_ann->storage.payload));
  }
  return absl::OkStatus();
}

// Map-source loop head: exit when cel_map_iter_next returns 0;
// refresh iter_var (+ iter_var2 for v2) via cel_map_iter_{key,value}_at.
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

// List-source loop head: exit when ptr >= end; v2 two-iter also
// materialises {CEL_INT, i=index} in iter_var's slot.
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

// List-source loop tail: advance pointer by sizeof(CelValue), bump
// the v2 index counter.  (Map source advances at the head via
// cel_map_iter_next, so no tail work there.)
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
