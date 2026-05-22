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
    const uint32_t offset = Pack(expr, c);
    annotations_[expr.id()].storage =
        Storage{StorageKind::kStaticRodata, offset};
  }

 private:
  uint32_t Pack(const cel::Expr& expr, const cel::Constant& c) {
    // A `kConstant` whose annotation Repr is `kType` is the
    // rewrite target of `InlineTypeIdentifierReferences` — it
    // carries a string_value that names a CEL type.  Pack as a
    // CEL_TYPE-kinded CelValue (same span layout as a string) so
    // the runtime sees `{kind: CEL_TYPE, payload.s: ...}` instead
    // of CEL_STRING.  Dispatch on Repr here (not on the Constant
    // proto's oneof) because the proto has no slot for a
    // type-value — see `rewrite/m9-type-subsystem.md` §3.3 / §4.2.
    auto ann_it = annotations_.Find(expr.id());
    if (ann_it != nullptr && ann_it->repr == Repr::kType) {
      ABSL_CHECK(c.has_string_value())
          << "LayoutPass: kConstant expr_id=" << expr.id()
          << " has Repr::kType but no string_value "
             "(InlineTypeIdentifierReferences invariant violation)";
      return builder_.AllocateType(c.string_value());
    }
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
// its annotation.  Per `rewrite/m2-ident-select-unknowns.md` §2.6:
// the kIdent arm lowers to `BinaryenLocalGet(local_index, i32)`,
// matched by a `$eval` prelude that sets each local to the
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

// Walks every `kSelect` whose operand is annotated `Repr::kOptional`
// and lifts the field name into rodata as a CelValue holding a
// CEL_STRING.  Records the rodata offset on the Select node's
// annotation field `select_key_rodata_offset` so `EmitKSelect` can
// pass it as the `key_slot` arg of `cel_select_optional_field_at_vv`
// (ABI rationale + memory map in
// `wat/m14_optional_select_field.wat`).
//
// Non-optional Selects skip — the regular path uses `field_ref_id`
// instead of a CelValue key.  Must run BEFORE the static-memory
// builder is finalized.
class SelectKeyRodataVisitor : public cel::AstVisitorBase {
 public:
  SelectKeyRodataVisitor(StaticMemoryBuilder& builder,
                         WasmAnnotations& annotations)
      : builder_(builder), annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitSelect(const cel::Expr& expr,
                       const cel::SelectExpr& sel) override {
    const NodeAnnotation* op = annotations_.Find(sel.operand().id());
    if (op == nullptr || op->repr != Repr::kOptional) return;
    annotations_[expr.id()].select_key_rodata_offset =
        builder_.AllocateString(sel.field());
  }

 private:
  StaticMemoryBuilder& builder_;
  WasmAnnotations& annotations_;
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
// CEL-spec function name for `m[k]`.  Layout + codegen dispatch
// through this helper; the function string is stable per langdef
// §"Operator overloads".
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

  void PostVisitMap(const cel::Expr& expr, const cel::MapExpr& m) override {
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

  void PostVisitList(const cel::Expr& expr, const cel::ListExpr& l) override {
    // Each element expression's slot is consumed by `cel_list_set`
    // at codegen; release here so the list's result slot can
    // supersede in the same arena region.
    for (const cel::ListExprElement& e : l.elements()) {
      ReleaseIfWorkspaceSlot(e.expr().id());
    }
    annotations_[expr.id()].storage =
        Storage{StorageKind::kWorkspaceSlot, slots_.Acquire()};
  }

  // kStructExpr — `Foo{...}` lowers to one
  // `cel_host.cel_make_message(type_id, out_slot)` call followed by
  // per-entry `cel_host.cel_set_field(...)` calls.  The result slot
  // stays live through every entry-set call, then is returned to
  // the parent.  Each entry's value-slot is consumed by
  // `cel_set_field` and released here so the kStructExpr's result
  // slot can supersede in the same arena region — same discipline
  // as kMapExpr / kListExpr.  See `rewrite/m7-proto-literals.md`.
  void PostVisitStruct(const cel::Expr& expr,
                       const cel::StructExpr& s) override {
    for (const cel::StructExprField& f : s.fields()) {
      ReleaseIfWorkspaceSlot(f.value().id());
    }
    annotations_[expr.id()].storage =
        Storage{StorageKind::kWorkspaceSlot, slots_.Acquire()};
  }

  void PostVisitCall(const cel::Expr& expr,
                     const cel::CallExpr& call) override {
    // `dyn(scalar)` lowers to its argument's slot directly (per
    // `rewrite/dyn-passthrough-plan.md`, Option A).  ResolvePass has
    // already forwarded the arg's non-storage annotation onto the
    // call node; here we forward storage so consumers reading the
    // call's `ann.storage` (e.g. the ternary's cond_slot lookup)
    // see the arg's slot.  No fresh slot allocation, no release of
    // the arg's slot — its lifetime continues through the dyn call.
    if (call.function() == "dyn" && call.args().size() == 1 &&
        !call.has_target()) {
      const NodeAnnotation* arg_ann = annotations_.Find(call.args()[0].id());
      if (arg_ann != nullptr) {
        annotations_[expr.id()].storage = arg_ann->storage;
        return;
      }
    }
    // Control-flow operators (`_&&_` / `_||_` / `_?_:_` / `!_`),
    // indexing (`_[_]`), and the general arm all follow the same
    // slot-out shape: every arg sub-expression hands its slot up;
    // release before acquiring this call's result slot so the
    // arena region is reused.  The ternary's two branch-arm slots
    // are allocated inside expr_lower (BinaryenIf wraps fresh
    // per-arm slots that don't escape the call's scope), so
    // LayoutPass only owns the call's result.  Receiver-form
    // `s.f(args)` also hands `target`'s slot up — release it too.
    if (!IsIndexCall(call)) {
      // General arm.  Receiver (target) participates in slot reuse.
      if (call.has_target()) {
        ReleaseIfWorkspaceSlot(call.target().id());
      }
    }
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

// Reserve one 24-byte workspace slot per referenced variable and
// fill `layout.variables`.  Workspace sits 8-aligned immediately
// after rodata; each slot is 24 B, and 24 is a multiple of 8 so
// every slot stays aligned.
// M5.B Slice C: walks every `kComprehensionExpr` and stamps
// `comp_aux_local_base` on each — the first of N consecutive wasm
// locals reserved for that comp's per-iter state (end_off, cursor,
// index).  N = `StaticLayout::comprehension_extra_locals_per_comp`.
// `LowerToEvalFunction` reads the total local count via
// `StaticLayout::total_wasm_locals`.
class ComprehensionLocalsVisitor : public cel::AstVisitorBase {
 public:
  ComprehensionLocalsVisitor(WasmAnnotations& annotations,
                             uint32_t locals_per_comp,
                             uint32_t base_local_index)
      : annotations_(annotations),
        locals_per_comp_(locals_per_comp),
        next_local_(base_local_index) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PreVisitComprehension(const cel::Expr& expr,
                             const cel::ComprehensionExpr& /*comp*/) override {
    annotations_[expr.id()].comp_aux_local_base = next_local_;
    next_local_ += locals_per_comp_;
  }

  uint32_t total_locals() const {
    return next_local_;
  }

 private:
  WasmAnnotations& annotations_;
  uint32_t locals_per_comp_;
  uint32_t next_local_;
};

void ReserveVariableSlots(const std::vector<ResolvedVariable>& variables,
                          StaticLayout& layout) {
  constexpr auto kSlotBytes = static_cast<uint32_t>(sizeof(CelValue));
  static_assert(kSlotBytes == 24, "CelValue must remain 24 bytes");
  static_assert(kSlotBytes % 8u == 0u, "CelValue must stay 8-aligned");

  layout.workspace_base = RoundUp8(layout.rodata_base +
                                   static_cast<uint32_t>(layout.rodata.size()));
  layout.variables.reserve(variables.size());
  // Comprehension iter vars don't get a workspace slot (their wasm
  // local holds a moving pointer, not a slot address).  Allocate
  // slots only to free vars + accu / index comp vars; pack densely
  // by allocation order so we don't waste cells on iter holes.
  uint32_t slot_count = 0;
  for (const ResolvedVariable& rv : variables) {
    uint32_t slot_offset = 0;
    if (rv.kind != ResolvedVariableKind::kComprehensionIter) {
      slot_offset = layout.workspace_base + (slot_count * kSlotBytes);
      ++slot_count;
    }
    layout.variables.push_back(LaidOutVariable{rv.name, rv.local_index, rv.repr,
                                               slot_offset, rv.kind});
  }
  layout.workspace_bytes = slot_count * kSlotBytes;
}

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
  layout.message_types = std::move(resolved.message_types);
  layout.debug_mode = opts.debug_layout;

  // --- Pass A: pack every kConst into rodata, then lift the field
  // name of every kSelect-on-optional into rodata too — the
  // `cel_select_optional_field_at_vv` kernel reads its key from a
  // CelValue slot.  Both passes share one builder so the rodata
  // layout is contiguous. ---
  StaticMemoryBuilder builder(layout.rodata_base);
  ConstLayoutVisitor const_visitor(builder, layout.annotations);
  cel::AstTraverse(ast.ast().root_expr(), const_visitor);
  SelectKeyRodataVisitor select_key_visitor(builder, layout.annotations);
  cel::AstTraverse(ast.ast().root_expr(), select_key_visitor);
  layout.rodata = std::move(builder).Finalize();

  // --- Pass B: reserve one 24-byte workspace slot per variable. ---
  ReserveVariableSlots(resolved.variables, layout);

  // --- Pass C: tag every kIdent with `{kLocal, local_index}`. ---
  IdentStorageVisitor ident_visitor(
      layout.annotations, static_cast<uint32_t>(resolved.variables.size()));
  cel::AstTraverse(ast.ast().root_expr(), ident_visitor);

  // --- Pass D: assign workspace slots to every kSelect node and
  // every aggregate-producing / aggregate-indexing node.  All four
  // kinds share the same SlotAllocator so slots recycle correctly. ---
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

  // --- Pass E: allocate per-comprehension auxiliary wasm locals
  // beyond `variables.size()` so `LowerToEvalFunction` can declare
  // the full local set up front.
  const auto base_local = static_cast<uint32_t>(layout.variables.size());
  ComprehensionLocalsVisitor comp_locals_visitor(
      layout.annotations, layout.comprehension_extra_locals_per_comp,
      base_local);
  cel::AstTraverse(ast.ast().root_expr(), comp_locals_visitor,
                   cel::TraversalOptions{.use_comprehension_callbacks = true});
  layout.total_wasm_locals = comp_locals_visitor.total_locals();

  return layout;
}

}  // namespace celwasm
