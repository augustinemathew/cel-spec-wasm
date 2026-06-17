#include "compiler/codegen/layout_pass.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "common/ast_traverse.h"
#include "common/ast_visitor_base.h"
#include "common/constant.h"
#include "common/expr.h"
#include "compiler/codegen/resolve_pass.h"
#include "compiler/codegen/slot_allocator.h"
#include "compiler/codegen/static_memory_builder.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"
#include "compiler/memory_layout.h"
#include "runtime/cel_data.h"

namespace celwasm {

namespace {

// Round `x` up to the next multiple of 8.  Used for the arena base
// — the runtime arena hands out 8-aligned bumps.
uint32_t RoundUp8(uint32_t x) {
  return (x + 7u) & ~uint32_t{7u};
}

// Round `x` up to the next multiple of 16.  Used for the workspace
// base — every workspace slot strides at `SlotAllocator::kSlotStride`
// (32 bytes) and must start 16-aligned so the runtime helpers'
// `memory.atomic.*` ops don't fault.
uint32_t RoundUp16(uint32_t x) {
  return (x + 15u) & ~uint32_t{15u};
}

// Walks every kConst node, packs its CelValue into rodata via the
// StaticMemoryBuilder, and writes `{kStaticRodata, offset}` onto the
// node's annotation.  Non-kConst nodes are left at `kNone` in this
// visitor; `IdentStorageVisitor` (below) handles kIdent.
class ConstLayoutVisitor : public cel::AstVisitorBase {
 public:
  // `consumed` lists kConst node ids that ConstAggregateVisitor already
  // packed into a materialized list's element run; they get no standalone
  // rodata frame here (else a const list would cost ~2x rodata — once for
  // the dead per-element frames, once for the run).
  ConstLayoutVisitor(StaticMemoryBuilder& builder, WasmAnnotations& annotations,
                     const absl::flat_hash_set<int64_t>& consumed)
      : builder_(builder), annotations_(annotations), consumed_(consumed) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitConst(const cel::Expr& expr, const cel::Constant& c) override {
    if (consumed_.contains(expr.id())) return;  // interior to a material list
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
  const absl::flat_hash_set<int64_t>& consumed_;
};

// --- Constant aggregate materialization (m31) --------------------------
//
// A list literal whose elements are all literals (or, recursively,
// const-materializable list literals) is packed into rodata at compile
// time as the byte-identical arena representation, so it lowers to a
// single i32.const instead of a per-Eval cel_list_create + N appends.
// Eligibility is purely syntactic — there is no compile-time evaluation,
// so `[1 + 1]` (a call) is NOT eligible and keeps the build path.

// True iff `e` is a literal or a const-materializable list, recursively.
// Type-valued constants (Repr::kType) and optional (`?x`) list elements
// are excluded — they fall back to the per-Eval build path.
bool IsConstMaterializable(const cel::Expr& e, const WasmAnnotations& ann) {
  switch (e.kind_case()) {
    case cel::ExprKindCase::kConstant: {
      const NodeAnnotation* a = ann.Find(e.id());
      if (a != nullptr && a->repr == Repr::kType) return false;
      const cel::Constant& c = e.const_expr();
      return c.has_null_value() || c.has_bool_value() || c.has_int_value() ||
             c.has_uint_value() || c.has_double_value() ||
             c.has_string_value() || c.has_bytes_value();
    }
    case cel::ExprKindCase::kListExpr: {
      for (const cel::ListExprElement& el : e.list_expr().elements()) {
        if (el.optional()) return false;
        if (!IsConstMaterializable(el.expr(), ann)) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

// Build the CelValue for one materializable literal, allocating any
// string / bytes payload into `b` so the run element's span points into
// rodata.  Precondition: `c` is a kind IsConstMaterializable admits.
CelValue ConstToCelValue(const cel::Constant& c, StaticMemoryBuilder& b) {
  CelValue v{};
  if (c.has_null_value()) {
    v.kind = CEL_NULL;
  } else if (c.has_bool_value()) {
    v.kind = CEL_BOOL;
    v.payload.b = c.bool_value() ? 1 : 0;
  } else if (c.has_int_value()) {
    v.kind = CEL_INT;
    v.payload.i = c.int_value();
  } else if (c.has_uint_value()) {
    v.kind = CEL_UINT;
    v.payload.u = c.uint_value();
  } else if (c.has_double_value()) {
    v.kind = CEL_DOUBLE;
    v.payload.d = c.double_value();
  } else if (c.has_string_value()) {
    // AllocateString writes the payload bytes immediately after a frame
    // (the frame is unused here); the run element's span points at them.
    const uint32_t frame = b.AllocateString(c.string_value());
    v.kind = CEL_STRING;
    v.payload.s.ptr = frame + static_cast<uint32_t>(sizeof(CelValue));
    v.payload.s.len = static_cast<uint32_t>(c.string_value().size());
  } else if (c.has_bytes_value()) {
    const uint32_t frame = b.AllocateBytes(c.bytes_value());
    v.kind = CEL_BYTES;
    v.payload.bytes.ptr = frame + static_cast<uint32_t>(sizeof(CelValue));
    v.payload.bytes.len = static_cast<uint32_t>(c.bytes_value().size());
  } else {
    ABSL_CHECK(false) << "ConstToCelValue: non-materializable constant";
  }
  return v;
}

// Materializes every const-eligible list literal into rodata and stamps
// its node `{kStaticRodata, frame_offset}`.  PostVisit (bottom-up) and
// memoized, so a nested list is materialized once — before its parent
// embeds it — making the inner frame live with no double materialization.
class ConstAggregateVisitor : public cel::AstVisitorBase {
 public:
  // `consumed` is populated with the node id of every literal packed into
  // a materialized run, so ConstLayoutVisitor can skip giving them a
  // redundant standalone rodata frame.
  ConstAggregateVisitor(StaticMemoryBuilder& builder, WasmAnnotations& ann,
                        absl::flat_hash_set<int64_t>& consumed)
      : builder_(builder), annotations_(ann), consumed_(consumed) {}

  void PreVisitExpr(const cel::Expr& expr) override {
    if (expr.kind_case() == cel::ExprKindCase::kComprehensionExpr) {
      // The accumulator is mutated by the loop body (cel_list_append_at);
      // it must be built fresh at a workspace slot per Eval, never
      // materialized into read-only rodata.  iter_range stays eligible —
      // iteration is read-only.  PreVisit fires before the accu_init
      // sub-expr's PostVisitList, so the id is recorded in time.
      excluded_accu_.insert(expr.comprehension_expr().accu_init().id());
    }
  }
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitList(const cel::Expr& expr, const cel::ListExpr&) override {
    if (excluded_accu_.contains(expr.id())) return;
    if (IsConstMaterializable(expr, annotations_)) MaterializeList(expr);
  }

 private:
  // Returns the embeddable frame CelValue for a materialized list,
  // memoized per expr id; stamps the node's storage on first build.
  CelValue MaterializeList(const cel::Expr& expr) {
    auto it = frames_.find(expr.id());
    if (it != frames_.end()) return it->second;
    const cel::ListExpr& l = expr.list_expr();
    std::vector<CelValue> elements;
    elements.reserve(l.elements().size());
    for (const cel::ListExprElement& el : l.elements()) {
      elements.push_back(ElementValue(el.expr()));
    }
    const auto r = builder_.MaterializeList(elements);
    annotations_[expr.id()].storage =
        Storage{StorageKind::kStaticRodata, r.frame_offset};
    // Stamp each element node with its slot in the run.  The element's
    // value already lives there (no standalone frame is packed for it),
    // and this keeps every node's storage populated — the "no kNone"
    // invariant (cleanup-backlog #31) that the histogram tests assert.
    const auto stride = static_cast<uint32_t>(sizeof(CelValue));
    for (uint32_t i = 0; i < l.elements().size(); ++i) {
      annotations_[l.elements()[i].expr().id()].storage =
          Storage{StorageKind::kStaticRodata, r.elements_offset + i * stride};
    }
    frames_.emplace(expr.id(), r.frame);
    return r.frame;
  }

  // Precondition: IsConstMaterializable proved `e` is a literal or a
  // const list, so it is one of those two kinds.
  CelValue ElementValue(const cel::Expr& e) {
    if (e.kind_case() == cel::ExprKindCase::kConstant) {
      // Packed into the run — no standalone rodata frame needed.
      consumed_.insert(e.id());
      return ConstToCelValue(e.const_expr(), builder_);
    }
    return MaterializeList(e);
  }

  StaticMemoryBuilder& builder_;
  WasmAnnotations& annotations_;
  absl::flat_hash_set<int64_t>& consumed_;
  absl::flat_hash_map<int64_t, CelValue> frames_;
  absl::flat_hash_set<int64_t> excluded_accu_;
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
    if (op == nullptr) return;
    // Two operand kinds need a rodata-resident field-name CelValue:
    //   - `kOptional`: `cel_select_optional_field_at_vv` reads the key
    //     from a CEL_STRING CelValue slot.
    //   - `kMap`: `kSelect` on a map operand is sugar for `m[field]`
    //     (CEL spec §"Field selection on maps") — `EmitKSelect` emits
    //     a `cel_map_lookup_*` (or `cel_map_in_*` for `has()`) call
    //     whose key arg is the rodata offset of the field-name string.
    if (op->repr != Repr::kOptional && op->repr != Repr::kMap) return;
    annotations_[expr.id()].select_key_rodata_offset =
        builder_.AllocateString(sel.field());
  }

 private:
  StaticMemoryBuilder& builder_;
  WasmAnnotations& annotations_;
};

// Walks every `kSelect` node post-order and writes
// `{kWorkspaceSlot, offset}` onto its annotation.  kSelect's
// runtime helper (`cel_host.cel_get_field` or the optional-select
// kernel) reads its operand slot before writing its result, so
// aliasing parent↔operand is safe — Release operand first, then
// Acquire the parent.  The LIFO free list hands back the
// operand's just-vacated cell so chains like
// `c.a.b.c.d` peak at one workspace slot.
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

// Allocates one workspace slot per kMapExpr / kListExpr / kStructExpr
// (the aggregate's result) AND one per kCallExpr (control flow,
// `_[_]`, arithmetic, …).  Two visit-time rules apply, picked by
// the codegen pattern each kind emits:
//
// **Aggregate kinds (kListExpr / kMapExpr / kStructExpr).  Acquire
// at PreVisit, NOT PostVisit.**  Codegen for these emits
// `cel_list_create(parent)` / `cel_map_create(parent)` /
// `cel_make_message(type, parent)` BEFORE descending into each
// operand's emit, then comes back to read every operand and
// append/insert/set onto parent.  If parent's slot ever aliased a
// descendant operand's slot, the descendant's later init would
// clobber parent's already-written handle before the parent's read
// — surfaced as the m4 / m7 cross-aggregate aliasing bug.  Holding
// the parent's slot in `Acquire`d state across the entire subtree
// (PreVisit Acquire → PostVisit's children have all finished without
// ever seeing parent's slot in the free list) guarantees no
// descendant can pick parent's cell.  PostVisit releases each
// operand slot so SIBLING subtrees of the aggregate's PARENT can
// reuse them; the aggregate's OWN slot is released by its ancestor
// consumer (a kSelect / kCall that reads it as operand).
//
// **kCallExpr (all of arithmetic, control flow, indexing, generic
// calls).  Acquire at PostVisit, Release operand slots first.**
// Every runtime helper that backs a kCall reads its operands
// BEFORE writing its result slot, so aliasing parent↔operand is
// safe — the helper produces an in-place op.  Releasing operands
// first lets the LIFO free list hand back the just-vacated cell
// for the parent's result, which is what caps long-arithmetic
// chains at peak ≈ tree depth.
//
// Together: aggregate parents never alias any descendant, and
// kCall chains stay tight.  The aggregate's `PreVisit` is dispatched
// from `PreVisitExpr` because cel-cpp's visitor base exposes only
// `PostVisitList/Map/Struct` — not Pre-variants.
//
// Per-entry key/value scratch slots for kMapExpr (and per-element
// scratch slots for kListExpr) are NOT pre-reserved by the layout:
// each sub-expression already gets its own slot via the kSelect /
// kCall visitors, or resolves to a kStaticRodata offset if it's a
// kConst.  expr_lower reads those operand slots and feeds them
// straight into `cel_map_insert` / `cel_list_set`; no extra layout
// work needed.
class AggregateStorageVisitor : public cel::AstVisitorBase {
 public:
  AggregateStorageVisitor(WasmAnnotations& annotations, SlotAllocator& slots)
      : annotations_(annotations), slots_(slots) {}

  // PreVisit dispatcher: for kListExpr / kMapExpr / kStructExpr,
  // Acquire the parent's slot BEFORE descending into operands.
  // See class preamble for why aggregates need their slot pinned
  // across the whole subtree.  Other kinds (kCall, kSelect, leaves)
  // are no-ops here and get their slot in PostVisit.
  void PreVisitExpr(const cel::Expr& expr) override {
    switch (expr.kind_case()) {
      case cel::ExprKindCase::kListExpr:
        // A const list already materialized into rodata
        // (ConstAggregateVisitor stamped {kStaticRodata, ...}) needs no
        // workspace slot and is not built per-Eval.
        if (annotations_[expr.id()].storage.kind ==
            StorageKind::kStaticRodata) {
          break;
        }
        [[fallthrough]];
      case cel::ExprKindCase::kMapExpr:
      case cel::ExprKindCase::kStructExpr:
        annotations_[expr.id()].storage =
            Storage{StorageKind::kWorkspaceSlot, slots_.Acquire()};
        break;
      default:
        break;
    }
  }

  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitMap(const cel::Expr&, const cel::MapExpr& m) override {
    // Slot was acquired in PreVisit.  Release each operand cell
    // (key + value sub-expressions) so SIBLING subtrees of this
    // aggregate's PARENT can reuse them.  The parent aggregate's
    // own slot is released by its consumer (the kSelect / kCall
    // that reads it) — not here.
    for (const cel::MapExprEntry& e : m.entries()) {
      ReleaseIfWorkspaceSlot(e.key().id());
      ReleaseIfWorkspaceSlot(e.value().id());
    }
  }

  void PostVisitList(const cel::Expr&, const cel::ListExpr& l) override {
    for (const cel::ListExprElement& e : l.elements()) {
      ReleaseIfWorkspaceSlot(e.expr().id());
    }
  }

  // kStructExpr — `Foo{...}` lowers to one
  // `cel_host.cel_make_message(type_id, out_slot)` call followed
  // by per-entry `cel_host.cel_set_field(...)` calls.  Same
  // PreVisit-Acquire / PostVisit-Release-operands discipline as
  // kListExpr / kMapExpr.  See `rewrite/m7-proto-literals.md`.
  void PostVisitStruct(const cel::Expr&, const cel::StructExpr& s) override {
    for (const cel::StructExprField& f : s.fields()) {
      ReleaseIfWorkspaceSlot(f.value().id());
    }
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
    // kCallExpr arms are read-before-write at the runtime helper
    // (`cel_int_add_at_vv(out, lhs, rhs)`, `cel_map_lookup`,
    // BinaryenIf for control flow, …) — so aliasing parent↔
    // operand is safe.  Release operand slots first so the LIFO
    // free list hands the just-vacated cell back as this call's
    // result.  Receiver-form `s.f(args)` participates in the
    // same scheme.
    if (!IsIndexCall(call) && call.has_target()) {
      ReleaseIfWorkspaceSlot(call.target().id());
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

  // Stamp the kComprehensionExpr's annotation with the storage of
  // its `result` sub-expression — the sub-expression whose final
  // value the comp block evaluates to.  `expr_lower_comprehension`'s
  // `BuildCompBlock` ends the comp's emitted block with
  // `Emit(comp.result())`, so the block's i32 return value is the
  // slot offset where the result CelValue actually lives; mirroring
  // that into the comp's annotation lets consumers reach the same
  // address through the standard storage-dispatch path.
  //
  // The result sub-expression varies by macro: `.exists` / `.all` /
  // `.filter` / `.map` use `kIdent(@result)` as the result (storage
  // kLocal at the accu's wasm local — `local.get` returns the accu
  // slot offset), so the comp ends up addressable through the accu
  // slot.  `.exists_one` uses `kCallExpr(_==_, @result, 1)` whose
  // result is a fresh workspace slot holding a Bool — distinct from
  // the Int accu slot the count loop wrote — and stamping the accu
  // slot here would give consumers the wrong address (they'd read
  // the count Int and dispatch as if it were a Bool, surfacing as
  // cleanup-backlog #32).
  //
  // This is a post-visit hook (children visited first), so
  // `comp.result()`'s annotation is already populated.
  void PostVisitComprehension(const cel::Expr& expr,
                              const cel::ComprehensionExpr& comp) override {
    const cel::Expr& result_expr = comp.result();
    const NodeAnnotation* result_ann = annotations_.Find(result_expr.id());
    ABSL_CHECK(result_ann != nullptr)
        << "ComprehensionLocalsVisitor: kComprehensionExpr expr_id="
        << expr.id() << " result-sub-expr id=" << result_expr.id()
        << " has no annotation — LayoutPass child traversal didn't "
        << "stamp it before PostVisitComprehension";
    ABSL_CHECK(result_ann->storage.kind != StorageKind::kNone)
        << "ComprehensionLocalsVisitor: kComprehensionExpr expr_id="
        << expr.id() << " result-sub-expr id=" << result_expr.id()
        << " has storage.kind == kNone — children visitor failed to "
        << "stamp the result";
    annotations_[expr.id()].storage = result_ann->storage;
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
  constexpr uint32_t kSlotStride = SlotAllocator::kSlotStride;
  static_assert(kSlotStride >= sizeof(CelValue),
                "slot stride must hold a CelValue");
  static_assert(kSlotStride % 16u == 0u,
                "slot stride must be 16-aligned for memory.atomic ops");

  layout.workspace_base = RoundUp16(
      layout.rodata_base + static_cast<uint32_t>(layout.rodata.size()));
  layout.variables.reserve(variables.size());
  // Comprehension iter vars don't get a workspace slot (their wasm
  // local holds a moving pointer, not a slot address).  Allocate
  // slots only to free vars + accu / index comp vars; pack densely
  // by allocation order so we don't waste cells on iter holes.
  uint32_t slot_count = 0;
  for (const ResolvedVariable& rv : variables) {
    uint32_t slot_offset = 0;
    if (rv.kind != ResolvedVariableKind::kComprehensionIter) {
      slot_offset = layout.workspace_base + (slot_count * kSlotStride);
      ++slot_count;
    }
    layout.variables.push_back(LaidOutVariable{rv.name, rv.local_index, rv.repr,
                                               slot_offset, rv.kind});
  }
  layout.workspace_bytes = slot_count * kSlotStride;
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
  if (opts.rodata_base_override != 0) {
    layout.rodata_base = opts.rodata_base_override;
  }

  // --- Pass A: pack rodata.  First materialize const list literals
  // (m31) — each eligible kListExpr is stamped {kStaticRodata,
  // frame_offset} and its element literals recorded in `consumed` so the
  // ConstLayoutVisitor below skips their redundant standalone frames
  // (a const list would otherwise cost ~2x rodata).  Then pack the
  // remaining kConsts, and lift each kSelect-on-optional field name into
  // rodata for the `cel_select_optional_field_at_vv` kernel.  All three
  // share one builder so the rodata layout is contiguous. ---
  StaticMemoryBuilder builder(layout.rodata_base);
  absl::flat_hash_set<int64_t> consumed_consts;
  ConstAggregateVisitor const_agg_visitor(builder, layout.annotations,
                                          consumed_consts);
  cel::AstTraverse(ast.ast().root_expr(), const_agg_visitor);
  ConstLayoutVisitor const_visitor(builder, layout.annotations,
                                   consumed_consts);
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

  // Slot-exhaustion gate.  rodata + workspace share the
  // `[kRodataBaseMin, kReservedLowMemoryBytes)` window below
  // wasi-libc's static data; anything we write past that line
  // silently corrupts libc's bookkeeping (no wasm trap, just a
  // delayed-death failure inside an unrelated helper).  The cap
  // is dynamic in rodata: an expression with no constants gets
  // ~7.9 KiB of workspace headroom, an expression with 3 KiB of
  // string constants gets ~4.9 KiB.  The `kGuardBytes` band
  // shaved off the top catches the next slot-allocator
  // off-by-one before it spills.
  const auto rodata_size = static_cast<uint32_t>(layout.rodata.size());
  // rodata must itself fit below the reserved line (with the guard band).
  // A materialized const aggregate (m31) carries no workspace slot, so
  // the workspace check below cannot catch oversized rodata — a large
  // const list would otherwise grow the run past the window and silently
  // corrupt wasi-libc's static data.
  const uint32_t rodata_end = layout.rodata_base + rodata_size;
  if (rodata_end + MemoryLayout::kGuardBytes >
      MemoryLayout::kReservedLowMemoryBytes) {
    return absl::ResourceExhaustedError(absl::StrCat(
        kSlotExhaustedMessagePrefix, ": rodata at [", layout.rodata_base, ", ",
        rodata_end, ") plus a ", MemoryLayout::kGuardBytes,
        "-byte guard exceeds the window below wasi-libc's static data (which "
        "starts at ",
        MemoryLayout::kReservedLowMemoryBytes,
        ").  A constant aggregate or string/bytes literal set too large for "
        "the static window; split the expression across multiple Compile() "
        "calls or move large literals into bound variables."));
  }
  const uint32_t max_workspace =
      MemoryLayout::MaxWorkspaceBytes(layout.rodata_base, rodata_size);
  if (layout.workspace_bytes > max_workspace) {
    return absl::ResourceExhaustedError(absl::StrCat(
        kSlotExhaustedMessagePrefix, ": needs ", layout.workspace_bytes,
        " bytes of workspace, but rodata at [", layout.rodata_base, ", ",
        layout.rodata_base + rodata_size, ") plus a ",
        MemoryLayout::kGuardBytes, "-byte guard band leaves only ",
        max_workspace,
        " bytes free below wasi-libc's static data (which starts at ",
        MemoryLayout::kReservedLowMemoryBytes,
        ").  Writing past that line would silently corrupt libc state.  "
        "Split the expression across multiple Compile() calls, or move "
        "literal strings/bytes out of the source into bound variables."));
  }

  return layout;
}

}  // namespace celwasm
