#include "compiler_v2/codegen/resolve_pass.h"

#include <cstdint>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
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
  absl::flat_hash_map<std::string, uint32_t> name_to_index_{};
};

}  // namespace

absl::StatusOr<ResolveOutput> ResolvePass(const TypedAst& ast) {
  ABSL_CHECK(ast.has_ast()) << "ResolvePass: TypedAst has no checked cel::Ast";

  ResolveOutput output;

  // First: seed every annotation's `repr` from the checker's
  // type_map.  Design doc §5.2 moves the `PopulateAnnotations` logic
  // out of the frontend into this pass.
  for (const auto& [expr_id, type] : ast.ast().type_map()) {
    output.annotations[expr_id].repr = ReprOf(type);
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

  return output;
}

}  // namespace celwasm
