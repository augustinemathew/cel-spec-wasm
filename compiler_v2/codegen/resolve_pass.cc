#include "compiler_v2/codegen/resolve_pass.h"

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
// emit garbage rodata bytes at M1 (every literal is bound straight into
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

}  // namespace

absl::StatusOr<ResolveOutput> ResolvePass(const TypedAst& ast) {
  ABSL_CHECK(ast.has_ast()) << "ResolvePass: TypedAst has no checked cel::Ast";

  ResolveOutput output;

  // Seed `repr` from the checker's type_map.  Design doc §5.2 moves the
  // `PopulateAnnotations` logic out of the frontend into this pass; for
  // M1 that seeding is the whole pass body.  The other NodeAnnotation
  // fields (overload_id, local_index, scope_id) stay at 0 — M1's
  // expression surface is pure literals, so nothing to bind or intern.
  //
  // `local_types` is empty and `max_scope_id` is 0 for the same reason.
  for (const auto& [expr_id, type] : ast.ast().type_map()) {
    output.annotations[expr_id].repr = ReprOf(type);
  }

  // Audit: every kConst now has a non-kUnknown repr.  Failure crashes
  // with a message naming the offending expr id (see KConstReprAudit).
  KConstReprAudit audit(output.annotations);
  cel::AstTraverse(ast.ast().root_expr(), audit);

  return output;
}

}  // namespace celwasm
