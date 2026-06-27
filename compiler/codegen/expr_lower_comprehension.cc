// expr_lower_comprehension.cc — wasm codegen for kComprehensionExpr.
//
// Covers every comprehension cel-cpp can emit: exists / all /
// exists_one / map / filter (standard macros), transformList /
// transformMap / transformMapEntry (comprehensions_v2), and
// cel.bind (bindings_ext).  Design: `rewrite/m5-comprehensions-design.md`
// + `rewrite/m5-comprehensions-followon.md`; pending simplifications
// tracked in `rewrite/m5b-comprehensions-simplification.md`.
//
// ── Emission shape ────────────────────────────────────────────
//
// Every comprehension lowers to a single (block (result i32))
// holding three regions:
//
//   1. Prologue (runs once)
//      - Eval iter_range → workspace slot, drop value.
//      - Range 3VL absorption guard: if the range CelValue's kind
//        is CEL_UNKNOWN / CEL_ERROR, copy it into accu_slot and
//        branch past the prologue+loop region — the comprehension
//        result IS the poison, no iteration (mirrors cel-cpp
//        comprehension_step.cc's `result = std::move(range)`).
//        Prologue + loop live in a named `comp_absorb_<expr_id>`
//        block for this branch; the result expression still runs.
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
//   - aux0_local: end-pointer (list source) or iter-handle (map
//     source).  The v2 two-iter list index counter occupies
//     `aux0_local + 1`, exposed as `c.index_local()`.
//   - map_source: switches the prologue + loop scaffold between
//     the list and map shapes.
//   - exit_label / continue_label: per-comp unique (suffixed by
//     expr_id) so Binaryen's nested-label validator accepts
//     same-name comprehensions inside each other.
//
// Several values that used to live on this struct are now derived
// at the read site: `accu_slot()` from `accu_v->slot_offset`,
// `two_iter()` from `iter_v2 != nullptr`, `index_local()` from
// `aux0_local + 1`, and the init-source slot inline from
// `accu_init`'s NodeAnnotation in EmitCompPrologue.
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
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/expr_lower_internal.h"
#include "compiler/codegen/layout_pass.h"
#include "compiler/codegen/module.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"

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
  return CodegenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/8,
                     /*align=*/4, BinaryenTypeInt32(),
                     I32Const(ctx.mod, accu_slot));
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
// The list two-iter index counter occupies `aux0_local + 1`
// (a second wasm local ComprehensionLocalsVisitor reserves
// adjacent to aux0).  two_iter and accu_slot are derived from
// other fields rather than cached, so the source-of-truth is
// always iter_v2 / accu_v.
struct CompContext {
  const LaidOutVariable* iter_v = nullptr;
  const LaidOutVariable* iter_v2 = nullptr;
  const LaidOutVariable* accu_v = nullptr;
  uint32_t aux0_local = 0;
  // Where iter_range's CelValue lives at runtime.  Two shapes:
  //   - workspace slot (`source_via_local == false`): `source_slot`
  //     is the literal byte offset of the CelValue.  Set when
  //     iter_range materialises into a layout-assigned slot
  //     (literal `[..]`/`{..}`, proto field read, etc.).
  //   - wasm local (`source_via_local == true`): `source_local`
  //     is the index of the wasm local that holds the byte offset
  //     at runtime.  Set when iter_range is a kIdent — the local
  //     was populated by the `$eval` variable prelude (or an
  //     enclosing comprehension's loop header for nested comp).
  // The two shapes both resolve through `SourceAddrExpr` which
  // returns the right Binaryen i32 expression.  See m5b §CCF-8.
  uint32_t source_slot = 0;
  uint32_t source_local = 0;
  bool source_via_local = false;
  bool map_source = false;
  // Per-comp unique labels (suffixed by expr_id).  Nested same-name
  // comprehensions would otherwise trip Binaryen's visitLoop
  // "iter != breakTypes.end()" check.  `absorb_label` names the
  // block wrapping prologue + loop; the range-absorption guard
  // branches to it when iter_range is CEL_UNKNOWN / CEL_ERROR.
  std::string exit_label;
  std::string continue_label;
  std::string absorb_label;

  uint32_t accu_slot() const {
    return accu_v->slot_offset;
  }
  // Storage of the iter_range's CelValue, reconstructed from the
  // resolved source fields (only the two kinds
  // ResolveCompSourceAddress accepts).  Lets the absorption guard
  // route loads/copies through the storage-dispatching helpers.
  Storage range_storage() const {
    return source_via_local ? Storage{StorageKind::kLocal, source_local}
                            : Storage{StorageKind::kWorkspaceSlot, source_slot};
  }
  bool two_iter() const {
    return iter_v2 != nullptr;
  }
  uint32_t index_local() const {
    return aux0_local + 1;
  }
  // Wasm local holding the kind-resolved source slot offset for the
  // current comprehension's list iter_range.  Set at prologue entry
  // via `cel_list_arena_view(source_addr)` so the inline arena walk
  // sees an arena-shaped CelValue regardless of origin (arena
  // sources pass through; host sources are snapshotted into arena
  // format).  Unused by the map prologue — `cel_map_iter_init`
  // dispatches internally.  See m5b §CCF-8 Slice 2.
  uint32_t list_source_addr_local() const {
    return aux0_local + 2;
  }
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
  if (!comp.iter_var2().empty()) {
    ABSL_CHECK(ann.comp_iter2_local_index < ctx.layout.variables.size())
        << "LowerComprehension: iter2 local index out of range (expr_id="
        << expr.id() << ")";
    c->iter_v2 = &ctx.layout.variables[ann.comp_iter2_local_index];
  }
  ABSL_CHECK(ann.comp_aux_local_base != 0)
      << "LowerComprehension: ComprehensionLocalsVisitor didn't assign aux "
         "locals (expr_id="
      << expr.id() << ")";
  c->aux0_local = ann.comp_aux_local_base;
  ABSL_CHECK(c->accu_v->slot_offset != 0)
      << "LowerComprehension: accu_var `" << comp.accu_var()
      << "` has no workspace slot";
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

// Resolve the comprehension source address into `c`.  Three storage
// shapes are accepted (each only on certain reprs — list/map
// asymmetry handled by the caller):
//   kWorkspaceSlot: literal byte offset of the CelValue.
//   kStaticRodata:  literal byte offset of a const list materialized
//                   into rodata.  Read-only, but a comprehension
//                   only reads its range (absorption-guard kind check +
//                   element iteration; the accu is separate and never
//                   materialized), so a rodata offset is used exactly
//                   like a workspace slot.
//   kLocal:         wasm local holds the byte offset at runtime.
// See CompContext field doc for the runtime mapping.
absl::Status ResolveCompSourceAddress(const NodeAnnotation& range_ann,
                                      const cel::Expr& expr, CompContext* c) {
  switch (range_ann.storage.kind) {
    case StorageKind::kWorkspaceSlot:
    case StorageKind::kStaticRodata:
      c->source_slot = range_ann.storage.payload;
      c->source_via_local = false;
      return absl::OkStatus();
    case StorageKind::kLocal:
      // kLocal sources are kIdent — the local holds the runtime
      // byte offset of the variable's CelValue cell, populated by
      // the `$eval` variable prelude (or an enclosing comp's loop
      // header for nested comp).  Maps: `cel_map_iter_init` is
      // kind-dispatching internally.  Lists: routed through
      // `cel_list_arena_view` in the prologue, which dispatches on
      // kind and snapshots host lists into arena shape so the
      // inline arena walk works uniformly.  See m5b §CCF-8.
      c->source_local = range_ann.storage.payload;
      c->source_via_local = true;
      return absl::OkStatus();
    case StorageKind::kNone:
      return absl::UnimplementedError(
          absl::StrCat("expr_lower: comprehension iter_range storage kind ",
                       static_cast<int>(range_ann.storage.kind),
                       " not yet supported (expr_id=", expr.id(),
                       "); accepted: kWorkspaceSlot, kStaticRodata, kLocal."));
  }
  ABSL_CHECK(false) << "ResolveCompSourceAddress: unknown StorageKind "
                    << static_cast<int>(range_ann.storage.kind);
}

absl::StatusOr<CompContext> ResolveCompContext(
    EmitCtx& ctx, const cel::Expr& expr, const cel::ComprehensionExpr& comp,
    const NodeAnnotation& ann) {
  CompContext c{};
  c.exit_label = absl::StrCat("comp_exit_", expr.id());
  c.continue_label = absl::StrCat("comp_continue_", expr.id());
  c.absorb_label = absl::StrCat("comp_absorb_", expr.id());
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
  if (auto s = ResolveCompSourceAddress(*range_ann, expr, &c); !s.ok()) {
    return s;
  }
  // All origins (arena / host / dynamic) for both list and map
  // sources land here.  Slice 1 (maps) ships via
  // `cel_map_iter_init`'s kind dispatch; Slice 2 (lists) ships via
  // `cel_list_arena_view` (called from the prologue) which
  // dispatches on CelValue.kind and snapshots host lists into
  // arena format.  See m5b §CCF-8.
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
  return c;
}

// Which wasm local holds the moving elements pointer.  Single-iter:
// iter_v itself.  Two-iter (v2): iter_v2 — iter_v is the synthesized
// index counter slot.
uint32_t ListIterPointerLocal(const CompContext& c) {
  return c.two_iter() ? c.iter_v2->local_index : c.iter_v->local_index;
}

// Build a runtime int32 expression that yields the byte offset of
// the iter_range's CelValue.  Workspace-slot sources resolve to a
// literal i32.const; kLocal sources resolve to a local.get of the
// local stamped by the variable prelude.  Used by `EmitMapPrologue`
// to handle both Activation-bound (kLocal-source) and computed
// (kWorkspaceSlot-source) maps uniformly.  See `CompContext` field
// doc + m5b §CCF-8.
BinaryenExpressionRef SourceAddrExpr(EmitCtx& ctx, const CompContext& c) {
  if (c.source_via_local) {
    return BinaryenLocalGet(ctx.mod.raw(), c.source_local, BinaryenTypeInt32());
  }
  return I32Const(ctx.mod, c.source_slot);
}

// Map-source prologue.  cel_map_iter_init returns a handle stored
// in aux0_local; iter_var (+ iter_var2 for v2) are bound to fixed
// workspace slots that cel_map_iter_{key,value}_at refresh each
// iteration.
void EmitMapPrologue(EmitCtx& ctx, const CompContext& c,
                     std::vector<BinaryenExpressionRef>* instrs) {
  auto* mod = ctx.mod.raw();
  BinaryenExpressionRef init_args[1] = {SourceAddrExpr(ctx, c)};
  instrs->push_back(
      BinaryenLocalSet(mod, c.aux0_local,
                       BinaryenCall(mod, "cel_map_iter_init", init_args, 1,
                                    BinaryenTypeInt32())));
  instrs->push_back(BinaryenLocalSet(mod, c.iter_v->local_index,
                                     I32Const(ctx.mod, c.iter_v->slot_offset)));
  if (c.two_iter()) {
    instrs->push_back(
        BinaryenLocalSet(mod, c.iter_v2->local_index,
                         I32Const(ctx.mod, c.iter_v2->slot_offset)));
  }
}

// List-source prologue.  First resolves the iter_range source to an
// arena-shaped slot via `cel_list_arena_view` (arena: passthrough;
// host: snapshot into arena format).  Then loads the arena-list
// header pointer (CelValue.payload offset 8), and derives:
//   ptr_local = elements pointer (header offset 8)
//   aux0_local = ptr_local + count * sizeof(CelValue) = one-past-end
// For v2 two-iter: also seeds iter_v's index-counter slot + aux1.
// CelValue size is 24 bytes — hardcoded here; if the runtime layout
// shifts the per-iter advance in EmitListLoopTail must change too.
void EmitListPrologue(EmitCtx& ctx, const CompContext& c,
                      std::vector<BinaryenExpressionRef>* instrs) {
  auto* mod = ctx.mod.raw();
  // `list_source_addr_local` was populated by EmitCompPrologue via
  // `cel_list_arena_view` (arena passthrough / host snapshot).  All
  // three subsequent loads read through that local — same address
  // pre-sizing already used in `EmitLoadSourceCount`.
  auto src_addr = [&]() {
    return BinaryenLocalGet(mod, c.list_source_addr_local(),
                            BinaryenTypeInt32());
  };
  const uint32_t ptr_local = ListIterPointerLocal(c);
  instrs->push_back(BinaryenLocalSet(
      mod, c.aux0_local,
      CodegenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/8,
                  /*align=*/4, BinaryenTypeInt32(), src_addr())));
  instrs->push_back(BinaryenLocalSet(
      mod, ptr_local,
      CodegenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/8,
                  /*align=*/4, BinaryenTypeInt32(),
                  BinaryenLocalGet(mod, c.aux0_local, BinaryenTypeInt32()))));
  BinaryenExpressionRef count =
      CodegenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/0,
                  /*align=*/4, BinaryenTypeInt32(),
                  BinaryenLocalGet(mod, c.aux0_local, BinaryenTypeInt32()));
  instrs->push_back(BinaryenLocalSet(
      mod, c.aux0_local,
      BinaryenBinary(
          mod, BinaryenAddInt32(),
          BinaryenLocalGet(mod, ptr_local, BinaryenTypeInt32()),
          BinaryenBinary(mod, BinaryenMulInt32(), count,
                         BinaryenConst(mod, BinaryenLiteralInt32(24))))));
  if (c.two_iter()) {
    instrs->push_back(BinaryenLocalSet(
        mod, c.iter_v->local_index, I32Const(ctx.mod, c.iter_v->slot_offset)));
    instrs->push_back(
        BinaryenLocalSet(mod, c.index_local(), I32Const(ctx.mod, 0)));
  }
}

// Load iter_range.count at runtime.
//
// Map sources can be arena OR host (Activation::Bind, proto map
// field).  Routing through the kind-dispatching runtime helper
// `cel_map_count` handles both: arena reads header.count inline;
// host calls `cel_host.cel_map_size` and unboxes the int payload.
//
// List sources: EmitCompPrologue resolved the iter_range to an
// arena-shaped slot via `cel_list_arena_view` and stored the
// resulting offset in `list_source_addr_local`.  Both arena and
// host (post-snapshot) sources now share the inline two-load
// header-walk shape: `payload+8` = arena_list.header_ptr,
// `*header+0` = count.
BinaryenExpressionRef EmitLoadSourceCount(EmitCtx& ctx, const CompContext& c) {
  auto* mod = ctx.mod.raw();
  if (c.map_source) {
    BinaryenExpressionRef args[1] = {SourceAddrExpr(ctx, c)};
    return BinaryenCall(mod, "cel_map_count", args, 1, BinaryenTypeInt32());
  }
  // List path: read through the resolved (arena-shaped) source addr.
  BinaryenExpressionRef src_addr =
      BinaryenLocalGet(mod, c.list_source_addr_local(), BinaryenTypeInt32());
  BinaryenExpressionRef hdr_ptr =
      CodegenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/8,
                  /*align=*/4, BinaryenTypeInt32(), src_addr);
  return CodegenLoad(mod, /*bytes=*/4, /*signed_=*/false, /*offset=*/0,
                     /*align=*/4, BinaryenTypeInt32(), hdr_ptr);
}

// ── loop_step classification ────────────────────────────────
//
// cel-cpp's macros emit one of seven loop_step shapes.  We
// classify the AST once, then dispatch per kind.  Operand
// pointers populated per kind (others stay null):
//
//   kListAppend / kListAppendIf   → elem  (+pred for If)
//   kMapInsert  / kMapInsertIf    → key, value  (+pred for If)
//   kMapMerge   / kMapMergeIf     → entry  (+pred for If)
//   kGeneric                      → none
//
// Originating macro per kind (for reference; macro names are
// erased by cel-cpp's expander, so we recover from AST shape):
//   kListAppend     map / transformList(3-arg)
//   kListAppendIf   filter / transformList(4-arg)
//   kMapInsert      transformMap(3-arg)
//   kMapInsertIf    transformMap(4-arg)
//   kMapMerge       transformMapEntry(3-arg)
//   kMapMergeIf     transformMapEntry(4-arg)
//   kGeneric        exists / all / exists_one / cel.bind
struct LoopStepShape {
  enum class Kind : uint8_t {
    kListAppend,
    kListAppendIf,
    kMapInsert,
    kMapInsertIf,
    kMapMerge,
    kMapMergeIf,
    kGeneric,
  };
  Kind kind = Kind::kGeneric;
  const cel::Expr* pred = nullptr;
  const cel::Expr* key = nullptr;
  const cel::Expr* value = nullptr;
  const cel::Expr* elem = nullptr;
  const cel::Expr* entry = nullptr;
};

// Defined further down (after the match helpers) — prologue reads
// the result to compute the pre-size multiplier.
LoopStepShape ClassifyLoopStep(const cel::Expr& step, absl::string_view accu);

// Per-iter entries the accu may receive — drives the pre-size
// capacity multiplier.  1 for kListAppend / kMapInsert / kGeneric
// (one element/entry per iter, or scalar accu); entry.size() for
// kMapMerge (transformMapEntry literal — empty = no-op, N>1 = N
// inserts per iter).  Reads the already-classified shape; doesn't
// re-walk the AST.
uint32_t PerIterEntryCount(const LoopStepShape& shape) {
  if ((shape.kind == LoopStepShape::Kind::kMapMerge ||
       shape.kind == LoopStepShape::Kind::kMapMergeIf) &&
      shape.entry->kind_case() == cel::ExprKindCase::kMapExpr) {
    return static_cast<uint32_t>(shape.entry->map_expr().entries().size());
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
  BinaryenExpressionRef args[2] = {I32Const(ctx.mod, c.accu_slot()), count};
  const char* helper = is_map ? "cel_map_create" : "cel_list_create";
  instrs->push_back(BinaryenCall(mod, helper, args, 2, BinaryenTypeNone()));
}

// Range 3VL absorption guard.  A comprehension whose iter_range
// evaluated to CEL_UNKNOWN or CEL_ERROR yields that value — no accu
// init, no loop, no result recomputation from an identity (cel-cpp
// routes kError / kUnknown ranges to `result = std::move(range)`;
// third_party/cel-cpp/eval/eval/comprehension_step.cc:165-169 and
// :350-354).  Emitted BEFORE any range-shape-specific prologue work
// (cel_list_arena_view / cel_map_iter_init / accu pre-sizing), all
// of which assume an iterable range and would otherwise turn the
// poison into a zero-count walk — the empty-range-identity soundness
// gap: exists→false, all→true, exists_one→false, map/filter→[].
//
// The poison is copied into the accu slot (kind + payload, so the
// unknown's attribute id / the error's code survive) and the guard
// branches past the prologue+loop region (`absorb_label`).  The
// result expression still runs and reads it from there:
// `@result`-shaped results (exists / all / map / filter /
// transform*) directly, `exists_one`'s `@result == 1` via `_==_`'s
// own 3VL absorption.  ERROR takes the same branch as UNKNOWN —
// with a single operand each propagates itself, consistent with the
// strict-call ERROR-dominates-UNKNOWN precedence
// (doc/design/03-abi-and-memory.md §8.1).  Shape locked by
// doc/implementation-plan/rewrite/wat/70_comprehension_unknown_range.wat.
void EmitRangeAbsorptionGuard(EmitCtx& ctx, const CompContext& c,
                              std::vector<BinaryenExpressionRef>* instrs) {
  auto* mod = ctx.mod.raw();
  const Storage range = c.range_storage();
  // CelValue kind word at offset 0; CEL_UNKNOWN = 15, CEL_ERROR = 16
  // (wire values pinned append-only by runtime/cel_data.h).
  BinaryenExpressionRef poisoned =
      BinaryenBinary(mod, BinaryenOrInt32(),
                     LoadSlotI32Eq(ctx, range, /*offset=*/0, /*expected=*/15),
                     LoadSlotI32Eq(ctx, range, /*offset=*/0, /*expected=*/16));
  BinaryenExpressionRef absorb_body[2] = {
      EmitCelCopySlot(ctx, Storage{StorageKind::kWorkspaceSlot, c.accu_slot()},
                      range),
      BinaryenBreak(mod, c.absorb_label.c_str(), nullptr, nullptr)};
  instrs->push_back(BinaryenIf(
      mod, poisoned,
      BinaryenBlock(mod, /*name=*/nullptr, absorb_body, 2, BinaryenTypeNone()),
      nullptr));
}

// Emits the prologue: drop the iter_range / accu_init values (their
// side effects already ran), bind accu_var's local to accu_slot, run
// the range 3VL absorption guard, then do source-specific iter setup
// — for list sources resolve the range to an arena view (EmitListPrologue)
// and either pre-size or copy the accu; for map sources cel_map_iter_init
// + bind the iter slots (EmitMapPrologue).
void EmitCompPrologue(EmitCtx& ctx, const cel::ComprehensionExpr& comp,
                      const CompContext& c, BinaryenExpressionRef range_value,
                      BinaryenExpressionRef init_value,
                      std::vector<BinaryenExpressionRef>* instrs) {
  auto* mod = ctx.mod.raw();
  instrs->push_back(BinaryenDrop(mod, range_value));
  instrs->push_back(BinaryenDrop(mod, init_value));
  // The accu local binds BEFORE the absorption guard: `@result`
  // reads in the result expression must resolve on both paths.
  instrs->push_back(BinaryenLocalSet(mod, c.accu_v->local_index,
                                     I32Const(ctx.mod, c.accu_slot())));
  EmitRangeAbsorptionGuard(ctx, c, instrs);
  const auto* init_ann = ctx.layout.annotations.Find(comp.accu_init().id());
  ABSL_CHECK(init_ann != nullptr);
  // For LIST sources, resolve the iter_range to an arena-shaped slot
  // FIRST so pre-sizing's `EmitLoadSourceCount` and the prologue's
  // header reads share the same address.  Arena sources pass
  // through `cel_list_arena_view` as identity; host sources get
  // snapshotted into arena format.  Map sources don't need this —
  // `cel_map_count` and `cel_map_iter_init` are both
  // kind-dispatching internally.  See m5b §CCF-8 Slice 2.
  if (!c.map_source) {
    BinaryenExpressionRef view_args[1] = {SourceAddrExpr(ctx, c)};
    instrs->push_back(
        BinaryenLocalSet(mod, c.list_source_addr_local(),
                         BinaryenCall(mod, "cel_list_arena_view", view_args, 1,
                                      BinaryenTypeInt32())));
  }
  if (IsPresizableCollectionAccu(comp, *init_ann)) {
    const LoopStepShape shape =
        ClassifyLoopStep(comp.loop_step(), comp.accu_var());
    EmitPresizeAccu(ctx, c, /*is_map=*/init_ann->repr == Repr::kMap,
                    /*per_iter=*/PerIterEntryCount(shape), instrs);
  } else {
    // The accu var owns a fixed workspace cell; the init expr's
    // storage can be any kind (rodata for literals, kLocal for a
    // bare ident accu_init like `cel.bind`'s pass-through).  The
    // `EmitCelCopySlot` overload taking `Storage` dispatches the
    // src-side address through `EmitSlotBaseAddress`, so a
    // kIdent init is read via `local.get` (not by treating the
    // local index as a byte offset — the bug that surfaced as
    // `KnownBugs.PbtTernaryInsideIntSubtract`).
    instrs->push_back(EmitCelCopySlot(
        ctx, Storage{StorageKind::kWorkspaceSlot, c.accu_slot()},
        init_ann->storage));
  }
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
  out->push_back(CodegenStore(
      mod, /*bytes=*/4, /*offset=*/0, /*align=*/4, I32Const(ctx.mod, slot),
      BinaryenConst(mod, BinaryenLiteralInt32(2)), BinaryenTypeInt32()));
  // _pad: workspace slot's prior contents are unknown, zero
  // defensively even though rodata starts zeroed.
  out->push_back(CodegenStore(
      mod, /*bytes=*/4, /*offset=*/4, /*align=*/4, I32Const(ctx.mod, slot),
      BinaryenConst(mod, BinaryenLiteralInt32(0)), BinaryenTypeInt32()));
  // payload.i — sign-extend the i32 local into the int64 slot.
  out->push_back(CodegenStore(
      mod, /*bytes=*/8, /*offset=*/8, /*align=*/8, I32Const(ctx.mod, slot),
      BinaryenUnary(mod, BinaryenExtendSInt32(),
                    BinaryenLocalGet(mod, value_local, BinaryenTypeInt32())),
      BinaryenTypeInt64()));
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
                         LoadAccuBoolPayload(ctx, c.accu_slot()), nullptr);
  }
  if (IsNotStrictlyFalseOfIdent(loop_cond, accu_name)) {
    return BinaryenBreak(mod, c.exit_label.c_str(),
                         BinaryenUnary(mod, BinaryenEqZInt32(),
                                       LoadAccuBoolPayload(ctx, c.accu_slot())),
                         nullptr);
  }
  return absl::UnimplementedError(absl::StrCat(
      "expr_lower: comprehension loop_cond shape not recognised (expr_id=",
      comp_expr.id(),
      ") — only kConst bool, @not_strictly_false(@result), and "
      "@not_strictly_false(!@result) peephole shapes are handled"));
}

// Match `_+_(@result, [t])` (kListAppend body).  On match writes
// the single element expr to *elem_out.
bool MatchAppendBody(const cel::Expr& expr, absl::string_view accu,
                     const cel::Expr** elem_out) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& call = expr.call_expr();
  if (call.function() != "_+_" || call.args().size() != 2) return false;
  if (!IsIdentNamed(call.args()[0], accu)) return false;
  const auto& rhs = call.args()[1];
  if (rhs.kind_case() != cel::ExprKindCase::kListExpr) return false;
  if (rhs.list_expr().elements().size() != 1) return false;
  *elem_out = &rhs.list_expr().elements()[0].expr();
  return true;
}

// Match `cel.@mapInsert(@result, ...)`.  Returns the arg count
// (2 = MapMerge form with `entry`; 3 = MapInsert form with k, v).
// Returns 0 if the shape doesn't match.
int MatchMapInsertCall(const cel::Expr& expr, absl::string_view accu) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return 0;
  const auto& call = expr.call_expr();
  if (call.function() != "cel.@mapInsert") return 0;
  if (!IsIdentNamed(call.args()[0], accu)) return 0;
  return static_cast<int>(call.args().size());
}

// Recover the conditional-wrap shape `p ? body : @result` — every
// macro's conditional 4-arg form lowers to this ternary.  On
// match, sets *pred_out + *body_out; caller re-classifies *body_out.
bool MatchConditionalWrap(const cel::Expr& expr, absl::string_view accu,
                          const cel::Expr** pred_out,
                          const cel::Expr** body_out) {
  if (expr.kind_case() != cel::ExprKindCase::kCallExpr) return false;
  const auto& call = expr.call_expr();
  if (call.function() != "_?_:_" || call.args().size() != 3) return false;
  if (!IsIdentNamed(call.args()[2], accu)) return false;
  *pred_out = call.args().data();
  *body_out = &call.args()[1];
  return true;
}

// Classify the loop_step AST into one of the recognised shapes.
// One walk; downstream emitters + PerIterEntryCount read the
// result instead of re-walking.
LoopStepShape ClassifyLoopStep(const cel::Expr& step, absl::string_view accu) {
  LoopStepShape s;
  const cel::Expr* pred = nullptr;
  const cel::Expr* inner = nullptr;
  const bool is_if = MatchConditionalWrap(step, accu, &pred, &inner);
  const cel::Expr& body = is_if ? *inner : step;

  if (const cel::Expr* elem = nullptr; MatchAppendBody(body, accu, &elem)) {
    s.kind = is_if ? LoopStepShape::Kind::kListAppendIf
                   : LoopStepShape::Kind::kListAppend;
    s.elem = elem;
    s.pred = pred;
    return s;
  }
  const int argc = MatchMapInsertCall(body, accu);
  if (argc == 3) {
    s.kind = is_if ? LoopStepShape::Kind::kMapInsertIf
                   : LoopStepShape::Kind::kMapInsert;
    s.key = &body.call_expr().args()[1];
    s.value = &body.call_expr().args()[2];
    s.pred = pred;
    return s;
  }
  if (argc == 2) {
    s.kind = is_if ? LoopStepShape::Kind::kMapMergeIf
                   : LoopStepShape::Kind::kMapMerge;
    s.entry = &body.call_expr().args()[1];
    s.pred = pred;
    return s;
  }
  return s;  // kGeneric, no operands
}

absl::Status EmitListAppend(EmitCtx& ctx, const CompContext& c,
                            const cel::Expr& elem,
                            std::vector<BinaryenExpressionRef>* body) {
  auto elem_or = Emit(ctx, elem);
  if (!elem_or.ok()) return elem_or.status();
  body->push_back(EmitCelListAppendCall(ctx, c.accu_slot(), *elem_or));
  return absl::OkStatus();
}

absl::Status EmitListAppendIf(EmitCtx& ctx, const CompContext& c,
                              const cel::Expr& pred, const cel::Expr& elem,
                              std::vector<BinaryenExpressionRef>* body) {
  auto pred_or = Emit(ctx, pred);
  if (!pred_or.ok()) return pred_or.status();
  auto elem_or = Emit(ctx, elem);
  if (!elem_or.ok()) return elem_or.status();
  BinaryenExpressionRef args[3] = {I32Const(ctx.mod, c.accu_slot()), *pred_or,
                                   *elem_or};
  body->push_back(BinaryenCall(ctx.mod.raw(), "cel_list_append_at_if_bool",
                               args, 3, BinaryenTypeNone()));
  return absl::OkStatus();
}

absl::Status EmitMapInsert(EmitCtx& ctx, const CompContext& c,
                           const cel::Expr& key, const cel::Expr& value,
                           std::vector<BinaryenExpressionRef>* body) {
  auto key_or = Emit(ctx, key);
  if (!key_or.ok()) return key_or.status();
  auto value_or = Emit(ctx, value);
  if (!value_or.ok()) return value_or.status();
  BinaryenExpressionRef args[3] = {I32Const(ctx.mod, c.accu_slot()), *key_or,
                                   *value_or};
  body->push_back(BinaryenCall(ctx.mod.raw(), "cel_map_insert_at", args, 3,
                               BinaryenTypeNone()));
  return absl::OkStatus();
}

// Routes through cel_map_insert_at_if_bool so a 3VL predicate
// (ERROR/UNKNOWN) propagates into the accu slot — aborting the
// comprehension — rather than being silently interpreted as a bool.
absl::Status EmitMapInsertIf(EmitCtx& ctx, const CompContext& c,
                             const cel::Expr& pred, const cel::Expr& key,
                             const cel::Expr& value,
                             std::vector<BinaryenExpressionRef>* body) {
  auto pred_or = Emit(ctx, pred);
  if (!pred_or.ok()) return pred_or.status();
  auto key_or = Emit(ctx, key);
  if (!key_or.ok()) return key_or.status();
  auto value_or = Emit(ctx, value);
  if (!value_or.ok()) return value_or.status();
  BinaryenExpressionRef args[4] = {I32Const(ctx.mod, c.accu_slot()), *pred_or,
                                   *key_or, *value_or};
  body->push_back(BinaryenCall(ctx.mod.raw(), "cel_map_insert_at_if_bool", args,
                               4, BinaryenTypeNone()));
  return absl::OkStatus();
}

// transformMapEntry step.  Entry must be a kMapExpr literal —
// computed entries need a runtime map-merge helper, not yet shipped.
// Emits one cel_map_insert_at per entry (size 0 = no-op iter per
// langdef §"Comprehension Macros"; size N = N sequential inserts).
absl::Status EmitMapMerge(EmitCtx& ctx, const CompContext& c,
                          const cel::Expr& entry,
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
    absl::Status s = EmitMapInsert(ctx, c, e.key(), e.value(), body);
    if (!s.ok()) return s;
  }
  return absl::OkStatus();
}

// Conditional transformMapEntry.  Size 0: eval pred for side-effects
// + drop.  Size 1: route through cel_map_insert_at_if_bool (3VL).
// Size N>1: would need a 3VL ladder gating N inserts atomically;
// cel_map_insert_at_if_bool only handles one (k,v).  Deferred.
absl::Status EmitMapMergeIf(EmitCtx& ctx, const CompContext& c,
                            const cel::Expr& pred, const cel::Expr& entry,
                            std::vector<BinaryenExpressionRef>* body) {
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
    return EmitMapInsertIf(ctx, c, pred, entries[0].key(), entries[0].value(),
                           body);
  }
  ABSL_CHECK(false)
      << "conditional transformMapEntry: multi-key entry (N>1) needs a "
         "3VL ladder gating N inserts atomically; not yet shipped.  "
         "entries.size()="
      << entries.size();
  return absl::OkStatus();
}

// Generic loop_step path: evaluate, drop, copy result slot into accu.
// Used by exists / all / exists_one (scalar bool accu) and cel.bind
// (degenerate, loop body never runs).  kLocal storage on the step
// means the value already lives in a wasm local — no copy needed;
// this is the cel.bind `loop_step = kIdent(accu_var)` case.
absl::Status EmitGenericStep(EmitCtx& ctx, const cel::Expr& step,
                             const CompContext& c,
                             std::vector<BinaryenExpressionRef>* body) {
  auto step_or = Emit(ctx, step);
  if (!step_or.ok()) return step_or.status();
  const auto* step_ann = ctx.layout.annotations.Find(step.id());
  ABSL_CHECK(step_ann != nullptr);
  body->push_back(BinaryenDrop(ctx.mod.raw(), *step_or));
  // loop_step's result can land in any storage kind — a fresh
  // workspace slot for a normal `_+_` expression, but kLocal for
  // a bare `kIdent(@result)` step (the cel.bind pass-through
  // case the function header notes).  Either is correct as long
  // as we route through the type-aware EmitCelCopySlot/
  // EmitSlotBaseAddress pair — passing `step_ann->storage` here
  // is the fix that closed `KnownBugs.PbtTernaryInsideIntSubtract`
  // for the comprehension loop-step path.  Skip the copy only
  // when the step has no storage (kNone — never happens today,
  // but kept defensive).
  if (step_ann->storage.kind != StorageKind::kNone) {
    body->push_back(EmitCelCopySlot(
        ctx, Storage{StorageKind::kWorkspaceSlot, c.accu_slot()},
        step_ann->storage));
  }
  return absl::OkStatus();
}

absl::Status EmitCompLoopStep(EmitCtx& ctx, const cel::ComprehensionExpr& comp,
                              const CompContext& c,
                              std::vector<BinaryenExpressionRef>* body) {
  const LoopStepShape s = ClassifyLoopStep(comp.loop_step(), comp.accu_var());
  switch (s.kind) {
    case LoopStepShape::Kind::kListAppend:
      return EmitListAppend(ctx, c, *s.elem, body);
    case LoopStepShape::Kind::kListAppendIf:
      return EmitListAppendIf(ctx, c, *s.pred, *s.elem, body);
    case LoopStepShape::Kind::kMapInsert:
      return EmitMapInsert(ctx, c, *s.key, *s.value, body);
    case LoopStepShape::Kind::kMapInsertIf:
      return EmitMapInsertIf(ctx, c, *s.pred, *s.key, *s.value, body);
    case LoopStepShape::Kind::kMapMerge:
      return EmitMapMerge(ctx, c, *s.entry, body);
    case LoopStepShape::Kind::kMapMergeIf:
      return EmitMapMergeIf(ctx, c, *s.pred, *s.entry, body);
    case LoopStepShape::Kind::kGeneric:
      return EmitGenericStep(ctx, comp.loop_step(), c, body);
  }
  ABSL_CHECK(false) << "EmitCompLoopStep: unknown LoopStepShape::Kind "
                    << static_cast<int>(s.kind);
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
  if (c.two_iter()) {
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
  if (c.two_iter()) {
    EmitWriteIntCelValueToSlot(ctx, c.iter_v->slot_offset, c.index_local(),
                               body);
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
  if (c.two_iter()) {
    body->push_back(BinaryenLocalSet(
        mod, c.index_local(),
        BinaryenBinary(
            mod, BinaryenAddInt32(),
            BinaryenLocalGet(mod, c.index_local(), BinaryenTypeInt32()),
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
  // Prologue + loop form the guarded region the range-absorption
  // guard branches past (see EmitRangeAbsorptionGuard); the result
  // expression runs on both paths, after the named block.
  std::vector<BinaryenExpressionRef> guarded;
  EmitCompPrologue(ctx, comp, c, *range_or, *init_or, &guarded);
  auto loop_or = BuildCompLoop(ctx, expr, comp, c);
  if (!loop_or.ok()) return loop_or.status();
  guarded.push_back(*loop_or);
  // Terminal map-construction step for map-ACCUMULATING comprehensions
  // (transformMap / transformMapEntry): after the accumulation loop,
  // build the SwissTable index over the complete accu map.  Gated on
  // the loop-step shape being a map-insert kind — NOT merely a kMap
  // accu repr — so `cel.bind(x, <map>, body)` (a kGeneric pass-through
  // whose map accu_init already carries its own EmitKMapExpr-built
  // index) is excluded and the map isn't index-built twice.  Emitted
  // inside the guarded block, so on the range-absorption path (accu
  // holds a poison CelValue, not a map) it never runs.  Runtime no-ops
  // below threshold; pure accelerator.  See m32-swisstable-map-index.md
  // §8.
  const LoopStepShape build_shape =
      ClassifyLoopStep(comp.loop_step(), comp.accu_var());
  const bool map_accumulating =
      build_shape.kind == LoopStepShape::Kind::kMapInsert ||
      build_shape.kind == LoopStepShape::Kind::kMapInsertIf ||
      build_shape.kind == LoopStepShape::Kind::kMapMerge ||
      build_shape.kind == LoopStepShape::Kind::kMapMergeIf;
  if (map_accumulating) {
    BinaryenExpressionRef build_args[1] = {I32Const(ctx.mod, c.accu_slot())};
    guarded.push_back(BinaryenCall(
        ctx.mod.raw(), std::string(kCelMapIndexBuildInternalName).c_str(),
        build_args, 1, BinaryenTypeNone()));
  }
  BinaryenExpressionRef guarded_block = BinaryenBlock(
      ctx.mod.raw(), c.absorb_label.c_str(), guarded.data(),
      static_cast<BinaryenIndex>(guarded.size()), BinaryenTypeNone());
  auto result_or = Emit(ctx, comp.result());
  if (!result_or.ok()) return result_or.status();
  BinaryenExpressionRef instrs[2] = {guarded_block, *result_or};
  return BinaryenBlock(ctx.mod.raw(), /*name=*/nullptr, instrs, 2,
                       BinaryenTypeInt32());
}

}  // namespace celwasm
