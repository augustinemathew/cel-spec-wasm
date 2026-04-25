#include "compiler_v2/codegen/layout_pass.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "common/ast_traverse.h"
#include "common/ast_visitor_base.h"
#include "common/constant.h"
#include "common/expr.h"
#include "compiler_v2/codegen/resolve_pass.h"
#include "compiler_v2/codegen/slot_allocator.h"
#include "compiler_v2/codegen/static_memory_builder.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"
#include "compiler_v2/runtime/cel_data.h"

namespace celwasm {

namespace {

// Round `x` up to the next multiple of 8.  The workspace region and the
// arena base must both be 8-aligned so every CelValue frame (24 bytes,
// 8-byte aligned) lands on a clean boundary.
uint32_t RoundUp8(uint32_t x) {
  return (x + 7u) & ~uint32_t{7u};
}

// Walks every kConst node, packs its CelValue into rodata via the
// StaticMemoryBuilder, and writes `{kStaticRodata, offset}` onto the
// node's annotation.  Non-kConst nodes are left at `kNone` in this
// visitor; `IdentStorageVisitor` (below) handles kIdent.
class ConstLayoutVisitor : public cel::AstVisitorBase {
 public:
  ConstLayoutVisitor(StaticMemoryBuilder& builder, WasmAnnotations& annotations)
      : builder_(builder), annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitConst(const cel::Expr& expr, const cel::Constant& c) override {
    const uint32_t offset = Pack(c);
    annotations_[expr.id()].storage =
        Storage{StorageKind::kStaticRodata, offset};
  }

 private:
  uint32_t Pack(const cel::Constant& c) {
    if (c.has_null_value()) return builder_.AllocateNull();
    if (c.has_bool_value()) return builder_.AllocateBool(c.bool_value());
    if (c.has_int_value()) return builder_.AllocateInt(c.int_value());
    if (c.has_uint_value()) return builder_.AllocateUint(c.uint_value());
    if (c.has_double_value()) return builder_.AllocateDouble(c.double_value());
    if (c.has_string_value()) return builder_.AllocateString(c.string_value());
    if (c.has_bytes_value()) return builder_.AllocateBytes(c.bytes_value());
    // Closed set: the checker rejects any other Constant variant before
    // we get here (duration / timestamp literals are not parseable
    // constants).  A new variant reaching this line is an invariant
    // violation, not a legitimate code path (CLAUDE.md).
    ABSL_CHECK(false) << "LayoutPass: unrecognised cel::Constant variant";
    return 0;
  }

  StaticMemoryBuilder& builder_;
  WasmAnnotations& annotations_;
};

// Walks every kIdent node and writes `{kLocal, local_index}` onto
// its annotation.  Per m2-ident-select-unknowns.md §2.6 / Slice
// M2.B: the kIdent arm lowers to `BinaryenLocalGet(local_index,
// i32)`, matched by a `$eval` prelude that sets each local to the
// compile-time-known slot offset.  The slot offset itself lives on
// `StaticLayout::variables[local_index].slot_offset` and the
// prelude reads it at emission time — the kIdent annotation holds
// only the local index.
class IdentStorageVisitor : public cel::AstVisitorBase {
 public:
  IdentStorageVisitor(WasmAnnotations& annotations, uint32_t num_variables)
      : annotations_(annotations), num_variables_(num_variables) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitIdent(const cel::Expr& expr,
                      const cel::IdentExpr& ident) override {
    NodeAnnotation& ann = annotations_[expr.id()];
    // Sanity: ResolvePass assigned a local_index for every kIdent and
    // appended the matching entry to `variables`.  If this kIdent
    // slipped past ResolvePass with an out-of-range index, the
    // `local.get` we emit would read an undeclared local —
    // invariant violation, crash.
    ABSL_CHECK(ann.local_index < num_variables_)
        << "LayoutPass: kIdent expr_id=" << expr.id() << " name=`"
        << ident.name() << "` has local_index=" << ann.local_index
        << " but only " << num_variables_ << " variables were resolved";
    ann.storage = Storage{StorageKind::kLocal, ann.local_index};
  }

 private:
  WasmAnnotations& annotations_;
  uint32_t num_variables_;
};

// Walks every `kSelect` node post-order and writes
// `{kWorkspaceSlot, offset}` onto its annotation.  Operand slots
// (nested selects) are released before acquiring the parent's slot,
// matching the post-order rule in slot_allocator.h — once the parent
// emits its load of the operand, the operand's cell is dead.
class SelectStorageVisitor : public cel::AstVisitorBase {
 public:
  SelectStorageVisitor(WasmAnnotations& annotations, SlotAllocator& slots)
      : annotations_(annotations), slots_(slots) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitSelect(const cel::Expr& expr,
                       const cel::SelectExpr& sel) override {
    const NodeAnnotation* op = annotations_.Find(sel.operand().id());
    if (op != nullptr && op->storage.kind == StorageKind::kWorkspaceSlot) {
      slots_.Release(op->storage.payload);
    }
    annotations_[expr.id()].storage =
        Storage{StorageKind::kWorkspaceSlot, slots_.Acquire()};
  }

 private:
  WasmAnnotations& annotations_;
  SlotAllocator& slots_;
};

// Returns true if `call` is the indexing operator `_[_]` — the
// CEL-spec function name for `m[k]`.  M3.F dispatches through this
// helper at both layout and codegen time; the function string is
// stable per langdef §"Operator overloads".
bool IsIndexCall(const cel::CallExpr& call) {
  return call.function() == "_[_]";
}

// Allocates one workspace slot per kMapExpr / kListExpr (the
// aggregate's result) AND one per kCallExpr(_[_]) (the lookup
// result).  Operand slots are released after the parent acquires
// per the kSelect convention.
//
// Per-entry key/value scratch slots for kMapExpr (and per-element
// scratch slots for kListExpr) are NOT pre-reserved by the layout:
// each sub-expression already gets its own slot via the existing
// kSelect / future kCall visitors, or resolves to a kStaticRodata
// offset if it's a kConst.  expr_lower reads those operand slots
// and feeds them straight into `cel_map_insert` / `cel_list_set`;
// no extra layout work needed.
class AggregateStorageVisitor : public cel::AstVisitorBase {
 public:
  AggregateStorageVisitor(WasmAnnotations& annotations, SlotAllocator& slots)
      : annotations_(annotations), slots_(slots) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitMap(const cel::Expr& expr,
                    const cel::MapExpr& m) override {
    // Each entry's key + value exprs were laid out in their own
    // post-visit; those slots are released as their values stop
    // being live (after `cel_map_insert` consumes them at codegen
    // time).  Release them here — the map's result slot supersedes.
    for (const cel::MapExprEntry& e : m.entries()) {
      ReleaseIfWorkspaceSlot(e.key().id());
      ReleaseIfWorkspaceSlot(e.value().id());
    }
    annotations_[expr.id()].storage =
        Storage{StorageKind::kWorkspaceSlot, slots_.Acquire()};
  }

  void PostVisitList(const cel::Expr& expr,
                     const cel::ListExpr& l) override {
    // Each element expression's slot is consumed by `cel_list_set`
    // at codegen; release here so the list's result slot can
    // supersede in the same arena region.
    for (const cel::ListExprElement& e : l.elements()) {
      ReleaseIfWorkspaceSlot(e.expr().id());
    }
    annotations_[expr.id()].storage =
        Storage{StorageKind::kWorkspaceSlot, slots_.Acquire()};
  }

  void PostVisitCall(const cel::Expr& expr,
                     const cel::CallExpr& call) override {
    if (!IsIndexCall(call)) return;  // M5 handles general kCall.
    // Operand + index expressions hand their slots up to us; release
    // them and acquire one for the lookup result.
    for (const auto& arg : call.args()) {
      ReleaseIfWorkspaceSlot(arg.id());
    }
    annotations_[expr.id()].storage =
        Storage{StorageKind::kWorkspaceSlot, slots_.Acquire()};
  }

 private:
  void ReleaseIfWorkspaceSlot(int64_t expr_id) {
    const NodeAnnotation* a = annotations_.Find(expr_id);
    if (a != nullptr && a->storage.kind == StorageKind::kWorkspaceSlot) {
      slots_.Release(a->storage.payload);
    }
  }

  WasmAnnotations& annotations_;
  SlotAllocator& slots_;
};

}  // namespace

// `resolved` is passed by value — its annotations + variables are
// moved into the StaticLayout below.  clang-tidy's
// performance-unnecessary-value-param sees only the const-access
// reads (the `for (const ResolvedVariable& rv : resolved.variables)`
// loop) and doesn't recognise the std::move(resolved.annotations)
// as a consuming use; suppressed inline.
absl::StatusOr<StaticLayout> LayoutPass(
    const TypedAst& ast,
    ResolveOutput resolved,  // NOLINT(performance-unnecessary-value-param)
    const LayoutOptions& opts) {
  ABSL_CHECK(ast.has_ast()) << "LayoutPass: TypedAst has no checked cel::Ast";

  StaticLayout layout;
  layout.annotations = std::move(resolved.annotations);
  layout.attributes = std::move(resolved.attributes);
  layout.debug_mode = opts.debug_layout;

  // --- Pass A: pack every kConst into rodata. ---
  StaticMemoryBuilder builder(layout.rodata_base);
  ConstLayoutVisitor const_visitor(builder, layout.annotations);
  cel::AstTraverse(ast.ast().root_expr(), const_visitor);
  layout.rodata = std::move(builder).Finalize();

  // --- Pass B: reserve one 24-byte workspace slot per referenced
  // variable.  Workspace sits 8-aligned immediately after rodata; each
  // slot is 24 bytes, and 24 is a multiple of 8 so every slot stays
  // aligned.  Indexed by ResolveOutput::variables order (local_index). ---
  layout.workspace_base = RoundUp8(layout.rodata_base +
                                   static_cast<uint32_t>(layout.rodata.size()));

  constexpr auto kSlotBytes = static_cast<uint32_t>(sizeof(CelValue));
  static_assert(kSlotBytes == 24, "CelValue must remain 24 bytes");
  static_assert(kSlotBytes % 8u == 0u, "CelValue must stay 8-aligned");

  layout.variables.reserve(resolved.variables.size());
  for (const ResolvedVariable& rv : resolved.variables) {
    const uint32_t slot_offset =
        layout.workspace_base + (rv.local_index * kSlotBytes);
    layout.variables.push_back(
        LaidOutVariable{rv.name, rv.local_index, rv.repr, slot_offset});
  }
  layout.workspace_bytes =
      static_cast<uint32_t>(resolved.variables.size()) * kSlotBytes;

  // --- Pass C: tag every kIdent with `{kLocal, local_index}`. ---
  // The wasm-local → slot_offset mapping lives on layout.variables;
  // expr_lower emits the `$eval` prelude that writes each slot
  // offset into its local.
  IdentStorageVisitor ident_visitor(
      layout.annotations, static_cast<uint32_t>(resolved.variables.size()));
  cel::AstTraverse(ast.ast().root_expr(), ident_visitor);

  // --- Pass D: assign workspace slots to every kSelect node and
  // every map-producing / map-indexing node (kMapExpr,
  // kCallExpr(_[_])).  All four kinds share the same SlotAllocator
  // so a kSelect inside a map literal's value, or a map indexing
  // chained off a kSelect, recycles slots correctly.  The slot
  // region sits immediately after the variable slots. ---
  const uint32_t selects_base = layout.workspace_base + layout.workspace_bytes;
  SlotAllocator slots(selects_base, opts.debug_layout);
  SelectStorageVisitor select_visitor(layout.annotations, slots);
  cel::AstTraverse(ast.ast().root_expr(), select_visitor);
  AggregateStorageVisitor aggregate_visitor(layout.annotations, slots);
  cel::AstTraverse(ast.ast().root_expr(), aggregate_visitor);
  layout.workspace_bytes += slots.total_bytes();
  layout.peak_slots = slots.peak_slots();

  // Arena grows forward from the first 8-aligned byte past workspace.
  layout.arena_base = RoundUp8(layout.workspace_base + layout.workspace_bytes);

  return layout;
}

}  // namespace celwasm
