#include "compiler_v2/codegen/resolve_pass.h"

#include <cstdint>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "common/ast_traverse.h"
#include "common/ast_visitor_base.h"
#include "common/expr.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"

namespace celwasm {

namespace {

// Walks every `kConst` node in the AST and asserts its annotation was
// populated with a non-unknown `Repr`.  A kConst missing a type_map entry
// — or carrying a type that `ReprOf` can't map — would cause codegen to
// emit garbage rodata bytes (every literal is bound straight into
// `.rodata` via its Repr).  Per CLAUDE.md we never `DCHECK` an invariant
// violation: a silently miscompiled release build is strictly worse than
// a crash that names the offending node.
class KConstReprAudit : public cel::AstVisitorBase {
 public:
  explicit KConstReprAudit(const WasmAnnotations& annotations)
      : annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitConst(const cel::Expr& expr,
                      const cel::Constant& /*constant*/) override {
    const NodeAnnotation* ann = annotations_.Find(expr.id());
    ABSL_CHECK(ann != nullptr)
        << "ResolvePass: kConst node id=" << expr.id()
        << " has no NodeAnnotation (type_map missing this id)";
    ABSL_CHECK(ann->repr != Repr::kUnknown)
        << "ResolvePass: kConst node id=" << expr.id()
        << " has Repr::kUnknown (type_map type not mappable by ReprOf)";
  }

 private:
  const WasmAnnotations& annotations_;
};

// Detects any `kComprehensionExpr` in the AST.  Comprehensions are
// M5 work and introduce scope-local idents (cel-cpp's macro
// expansion uses names like `@result` / `@iter` whose Repr varies
// per comprehension form — `exists` yields bool, `map` yields list,
// etc.).  The current `IdentResolver` is scope-flat and would CHECK
// on the cross-form Repr clash, crashing the binary instead of
// reporting a clean Unimplemented.  Until M5 lands scope handling,
// reject comprehension-bearing programs at the front of the resolve
// pass with a Status the conformance runner classifies as SKIP.
class ComprehensionDetector : public cel::AstVisitorBase {
 public:
  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitComprehension(
      const cel::Expr& /*expr*/,
      const cel::ComprehensionExpr& /*comp*/) override {
    found_ = true;
  }

  bool found() const { return found_; }

 private:
  bool found_ = false;
};

// Walks every `kIdentExpr` in the AST, interns the name into a dense
// table (`local_index` 0, 1, 2, ... in first-seen order), and writes
// the index onto the node's `NodeAnnotation::local_index`.
//
// `variables_` accumulates one entry per distinct name, with the
// Repr taken from the checker's type_map (seeded into
// `annotations[id].repr` by the first pass).  The result fuels
// LayoutPass (which assigns a workspace slot per entry) and the
// cel.abi custom section (which names the variables the host marshal
// must populate at Eval time).
class IdentResolver : public cel::AstVisitorBase {
 public:
  IdentResolver(WasmAnnotations& annotations,
                std::vector<ResolvedVariable>& variables)
      : annotations_(annotations), variables_(variables) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitIdent(const cel::Expr& expr,
                      const cel::IdentExpr& ident) override {
    NodeAnnotation& ann = annotations_[expr.id()];
    // Repr should be populated from the checker type_map by the
    // first pass.  If it isn't, the checker accepted an ident whose
    // type we can't map — invariant violation, not a recoverable
    // runtime error.
    ABSL_CHECK(ann.repr != Repr::kUnknown)
        << "ResolvePass: kIdent node id=" << expr.id() << " name=`"
        << ident.name() << "` has Repr::kUnknown "
        << "(checker left the type_map entry absent or non-mappable)";

    auto it = name_to_index_.find(ident.name());
    uint32_t local_index;
    if (it == name_to_index_.end()) {
      local_index = static_cast<uint32_t>(variables_.size());
      variables_.push_back(
          ResolvedVariable{ident.name(), local_index, ann.repr});
      name_to_index_.emplace(ident.name(), local_index);
    } else {
      local_index = it->second;
      // Sanity: a second reference to the same variable must agree
      // with the Repr of the first reference.  A mismatch here
      // would mean the checker assigned two different types to
      // ident nodes with the same name — invariant violation.
      ABSL_CHECK(variables_[local_index].repr == ann.repr)
          << "ResolvePass: kIdent name=`" << ident.name()
          << "` appears with mismatched Repr ("
          << ReprName(variables_[local_index].repr) << " vs "
          << ReprName(ann.repr) << ")";
    }
    ann.local_index = local_index;
  }

 private:
  WasmAnnotations& annotations_;
  std::vector<ResolvedVariable>& variables_;
  absl::flat_hash_map<std::string, uint32_t> name_to_index_;
};

// Walks kIdent + kSelect post-order.  Each node gets its attribute
// path computed as `(root_variable, qualifiers)` and interned into
// `attributes_` — duplicate paths share an id.  The node's
// `NodeAnnotation::attribute_id` is stamped with the interned id.
//
// Invariant: id 0 is the sentinel ("no attribute"); callers push it
// at index 0 before running the traversal.  Real entries start at 1.
//
// At `cel_get_field` time, the trampoline reads the OPERAND'S
// attribute_id (via `ctx.layout.annotations.Find(sel.operand().id())
// ->attribute_id`) and appends the select's field name — see
// `BuildAttributeForSelect` in `cel_host.cc`.  So this visitor's
// job is: stamp each path-bearing node with its OWN path id; the
// codegen uses operand's id at emission.
class AttributePathResolver : public cel::AstVisitorBase {
 public:
  AttributePathResolver(WasmAnnotations& annotations,
                        std::vector<AttributeEntryRow>& attributes)
      : annotations_(annotations), attributes_(attributes) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitIdent(const cel::Expr& expr,
                      const cel::IdentExpr& ident) override {
    annotations_[expr.id()].attribute_id = Intern(ident.name(), {});
  }

  void PostVisitSelect(const cel::Expr& expr,
                       const cel::SelectExpr& sel) override {
    const NodeAnnotation* op = annotations_.Find(sel.operand().id());
    if (op == nullptr || op->attribute_id == 0) {
      // Non-path-bearing operand (e.g. a literal, kCall, map) —
      // leave attribute_id at 0; unknown-pattern match is a no-op.
      return;
    }
    const AttributeEntryRow& base = attributes_[op->attribute_id];
    std::vector<std::string> qualifiers = base.qualifiers;
    qualifiers.push_back(sel.field());
    annotations_[expr.id()].attribute_id =
        Intern(base.root_variable, std::move(qualifiers));
  }

 private:
  uint32_t Intern(absl::string_view root_variable,
                  std::vector<std::string> qualifiers) {
    std::string key =
        absl::StrCat(root_variable, "|", absl::StrJoin(qualifiers, "."));
    auto it = key_to_id_.find(key);
    if (it != key_to_id_.end()) return it->second;
    const auto id = static_cast<uint32_t>(attributes_.size());
    attributes_.push_back(
        AttributeEntryRow{std::string(root_variable), std::move(qualifiers)});
    key_to_id_.emplace(std::move(key), id);
    return id;
  }

  WasmAnnotations& annotations_;
  std::vector<AttributeEntryRow>& attributes_;
  absl::flat_hash_map<std::string, uint32_t> key_to_id_;
};

// M3.F: stamps `map_origin` on every map-typed node per the
// `map-list-dispatch.md` §2.6 inference table:
//   kMapExpr        → kArena  (literal in the wasm bump arena)
//   kIdent[map<>]   → kHost   (Activation::Bind hands us a backing)
//   kSelect[map<>]  → kHost   (proto map field via ProtoBacking)
// Codegen reads this annotation at the kCallExpr(_[_]) emission
// site to choose between cel_map_lookup_arena (fast path),
// cel_host.cel_map_lookup (host trampoline), and the kDynamic
// dispatcher.  Branch-coalescing rules (?: / && / ||) over map
// operands stay deferred to M5.
class MapOriginVisitor : public cel::AstVisitorBase {
 public:
  explicit MapOriginVisitor(WasmAnnotations& annotations)
      : annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitMap(const cel::Expr& expr,
                    const cel::MapExpr& /*m*/) override {
    annotations_[expr.id()].map_origin = Origin::kArena;
  }

  void PostVisitIdent(const cel::Expr& expr,
                      const cel::IdentExpr& /*ident*/) override {
    StampHostIfMapTyped(expr);
  }

  void PostVisitSelect(const cel::Expr& expr,
                       const cel::SelectExpr& /*sel*/) override {
    StampHostIfMapTyped(expr);
  }

 private:
  void StampHostIfMapTyped(const cel::Expr& expr) {
    NodeAnnotation* ann = &annotations_[expr.id()];
    if (ann->repr == Repr::kMap) ann->map_origin = Origin::kHost;
  }
  WasmAnnotations& annotations_;
};

// M4.F: stamps `list_origin` on every list-typed node per the
// `map-list-dispatch.md` §2.6 inference table (mirror of
// MapOriginVisitor):
//   kListExpr       → kArena  (literal in the wasm bump arena)
//   kIdent[list<>]  → kHost   (Activation::Bind hands us a backing)
//   kSelect[list<>] → kHost   (proto repeated field via ProtoList)
// Codegen reads this annotation at the kCallExpr(_[_]) emission
// site to choose between cel_list_at_arena (fast path),
// cel_host.cel_list_at (host trampoline), and the kDynamic
// dispatcher.  Branch-coalescing (?: / && / ||) over list operands
// stays deferred to M5.
class ListOriginVisitor : public cel::AstVisitorBase {
 public:
  explicit ListOriginVisitor(WasmAnnotations& annotations)
      : annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitList(const cel::Expr& expr,
                     const cel::ListExpr& /*l*/) override {
    annotations_[expr.id()].list_origin = Origin::kArena;
  }

  void PostVisitIdent(const cel::Expr& expr,
                      const cel::IdentExpr& /*ident*/) override {
    StampHostIfListTyped(expr);
  }

  void PostVisitSelect(const cel::Expr& expr,
                       const cel::SelectExpr& /*sel*/) override {
    StampHostIfListTyped(expr);
  }

 private:
  void StampHostIfListTyped(const cel::Expr& expr) {
    NodeAnnotation* ann = &annotations_[expr.id()];
    if (ann->repr == Repr::kList) ann->list_origin = Origin::kHost;
  }
  WasmAnnotations& annotations_;
};

}  // namespace

absl::StatusOr<ResolveOutput> ResolvePass(const TypedAst& ast) {
  ABSL_CHECK(ast.has_ast()) << "ResolvePass: TypedAst has no checked cel::Ast";

  // Comprehensions are M5; bail early so the kIdent resolver doesn't
  // see the macro-introduced `@result` / `@iter` idents (whose Reprs
  // legitimately vary per comprehension form and would trip the
  // resolver's per-name Repr-agreement CHECK).
  ComprehensionDetector comprehension_detector;
  cel::AstTraverse(ast.ast().root_expr(), comprehension_detector);
  if (comprehension_detector.found()) {
    return absl::UnimplementedError(
        "ResolvePass: comprehensions are M5 — reject until scope handling "
        "lands");
  }

  ResolveOutput output;

  // Inherit the annotations ParseAndCheck already populated:
  // `repr` from the type_map and `field_number` from descriptor
  // resolution on every kSelect (see frontend/parse_and_check.cc →
  // ir/typed_ast.cc::PopulateAnnotations).  ResolvePass then adds
  // `local_index` for kIdent nodes and — later — `overload_id`
  // (M3), `scope_id` (M5), `attribute_id` (M2.E).
  for (const auto& [expr_id, ann] : ast.annotations().nodes()) {
    output.annotations[expr_id] = ann;
  }

  // Second: audit — every kConst now has a non-kUnknown repr.
  // Failure crashes with a message naming the offending expr id.
  KConstReprAudit audit(output.annotations);
  cel::AstTraverse(ast.ast().root_expr(), audit);

  // Third: intern every kIdent name, assign a dense local_index,
  // populate `NodeAnnotation::local_index`, and fill `variables`.
  // The count of entries in `variables` is also the count of wasm
  // locals the lowered `$eval` carries — one i32 per referenced
  // variable, per the M2.B dispatch (m2-ident-select-unknowns.md
  // §2.6: `BinaryenLocalGet(local_index, i32)` in the kIdent arm,
  // matched by a prelude `BinaryenLocalSet(local_index, <slot>)`).
  IdentResolver ident_resolver(output.annotations, output.variables);
  cel::AstTraverse(ast.ast().root_expr(), ident_resolver);

  // Fourth: intern every kIdent/kSelect's attribute path and stamp
  // `NodeAnnotation::attribute_id`.  Entry 0 is the "no attribute"
  // sentinel; real paths start at 1.  Codegen reads the operand's
  // attribute_id at each kSelect emission site.
  output.attributes.push_back(AttributeEntryRow{});
  AttributePathResolver attr_resolver(output.annotations, output.attributes);
  cel::AstTraverse(ast.ast().root_expr(), attr_resolver);

  // Fifth (M3.F): stamp `map_origin = kArena` on every kMapExpr.
  // M2 already wrote `kHost` on map-typed kSelect / kIdent nodes;
  // kCreateMap is the third source of map values and is always
  // arena-backed.  Branch-coalescing rules for ?: / && / || over
  // map operands stay deferred to M5.
  MapOriginVisitor map_origin_visitor(output.annotations);
  cel::AstTraverse(ast.ast().root_expr(), map_origin_visitor);

  // Sixth (M4.F): mirror MapOriginVisitor for lists — stamp
  // `list_origin = kArena` on every kListExpr; M2 already wrote
  // `kHost` on list-typed kSelect / kIdent nodes.
  ListOriginVisitor list_origin_visitor(output.annotations);
  cel::AstTraverse(ast.ast().root_expr(), list_origin_visitor);

  return output;
}

}  // namespace celwasm
