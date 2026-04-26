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

  void PostVisitComprehension(const cel::Expr& /*expr*/,
                              const cel::ComprehensionExpr& /*comp*/) override {
    found_ = true;
  }

  bool found() const {
    return found_;
  }

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

  void PostVisitMap(const cel::Expr& expr, const cel::MapExpr& /*m*/) override {
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

// Slice 1.5 (dyn-passthrough-plan.md, Option A): for every `dyn(scalar)`
// call admitted by the static-subset gate, copy the argument's
// non-storage annotation fields onto the call node so downstream
// consumers (operand reads in `==`, comprehension scope walks, the
// attribute-pattern matcher) see the underlying scalar type — the
// call site's checker-assigned `dyn` type would otherwise leave the
// annotation at `Repr::kUnknown` with empty `attribute_id` /
// `overload_id`.  Storage forwarding lives in LayoutPass: ResolvePass
// runs before slots are assigned, so we cannot copy `storage` here.
class DynPassthroughVisitor : public cel::AstVisitorBase {
 public:
  explicit DynPassthroughVisitor(WasmAnnotations& annotations)
      : annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitCall(const cel::Expr& expr,
                     const cel::CallExpr& call) override {
    if (call.function() != "dyn" || call.args().size() != 1 ||
        call.has_target()) {
      return;
    }
    const cel::Expr& arg = call.args()[0];
    const NodeAnnotation* arg_ann = annotations_.Find(arg.id());
    if (arg_ann == nullptr) return;
    NodeAnnotation& self = annotations_[expr.id()];
    self.repr = arg_ann->repr;
    self.field_number = arg_ann->field_number;
    self.overload_id = arg_ann->overload_id;
    self.local_index = arg_ann->local_index;
    self.attribute_id = arg_ann->attribute_id;
    self.map_origin = arg_ann->map_origin;
    self.list_origin = arg_ann->list_origin;
  }

 private:
  WasmAnnotations& annotations_;
};

// M5.F: stamps `overload_id` on every kCallExpr from cel-cpp's
// `Ast::reference_map`.  cel-cpp's checker writes a Reference for
// each call node listing the resolved standard-library overload
// (e.g. "add_int64" for `1+2`); we copy the first entry as a
// string_view pointing into cel-cpp's owned storage (lifetime
// tied to the surrounding TypedAst).  Codegen reads this in
// `EmitGeneralCall` to pick the wasm helper from `OverloadTable`.
//
// Empty `overload_id` is the legitimate default for
// special-cased calls (`_[_]`, `_&&_`, `_||_`, `_?_:_`) which
// don't go through the table — `expr_lower.cc` dispatches on
// `call.function()` for those before consulting the annotation.
class OverloadIdResolver : public cel::AstVisitorBase {
 public:
  OverloadIdResolver(const cel::Ast::ReferenceMap& reference_map,
                     WasmAnnotations& annotations)
      : reference_map_(reference_map), annotations_(annotations) {}

  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}

  void PostVisitCall(const cel::Expr& expr,
                     const cel::CallExpr& /*call*/) override {
    auto it = reference_map_.find(expr.id());
    if (it == reference_map_.end()) return;
    const auto& overloads = it->second.overload_id();
    if (overloads.empty()) return;
    // string_view points into cel-cpp's `Reference::overload_id_`
    // (a `std::vector<std::string>`).  The Reference is owned by
    // `Ast::reference_map_`; lifetime is the surrounding TypedAst,
    // which lives through codegen.
    annotations_[expr.id()].overload_id = absl::string_view(overloads.front());
  }

 private:
  const cel::Ast::ReferenceMap& reference_map_;
  WasmAnnotations& annotations_;
};

// Runs the per-kind annotation-stamping visitors on `output` over
// `root`.  Each visitor is independent — sequencing matters only for
// the dyn-passthrough forwarder, which runs last so every other
// visitor's writes on the argument are visible to copy.
void RunAnnotationVisitors(const cel::Ast& checked, const cel::Expr& root,
                           ResolveOutput& output) {
  KConstReprAudit audit(output.annotations);
  cel::AstTraverse(root, audit);

  IdentResolver ident_resolver(output.annotations, output.variables);
  cel::AstTraverse(root, ident_resolver);

  output.attributes.push_back(AttributeEntryRow{});
  AttributePathResolver attr_resolver(output.annotations, output.attributes);
  cel::AstTraverse(root, attr_resolver);

  MapOriginVisitor map_origin_visitor(output.annotations);
  cel::AstTraverse(root, map_origin_visitor);

  ListOriginVisitor list_origin_visitor(output.annotations);
  cel::AstTraverse(root, list_origin_visitor);

  OverloadIdResolver overload_resolver(checked.reference_map(),
                                       output.annotations);
  cel::AstTraverse(root, overload_resolver);

  DynPassthroughVisitor dyn_visitor(output.annotations);
  cel::AstTraverse(root, dyn_visitor);
}

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
  // `local_index` for kIdent nodes and other per-kind fields.
  for (const auto& [expr_id, ann] : ast.annotations().nodes()) {
    output.annotations[expr_id] = ann;
  }

  RunAnnotationVisitors(ast.ast(), ast.ast().root_expr(), output);
  return output;
}

}  // namespace celwasm
