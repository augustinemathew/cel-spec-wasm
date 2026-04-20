#include "compiler/codegen/field_name_pool.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "common/expr.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {
namespace {

// Pre-order walk: every SelectExpr contributes its `(field_number,
// field())` pair to the pool, then the walk recurses into child
// expressions.  The ordering matters — codegen lowers the same tree
// top-down and calls `Intern` at each SelectExpr in the same order,
// so the two agree on intern IDs by construction.
void WalkAndIntern(const TypedAst& ast, const cel::Expr& expr,
                   FieldNamePool& pool) {
  switch (expr.kind_case()) {
    case cel::ExprKindCase::kSelectExpr: {
      const cel::SelectExpr& s = expr.select_expr();
      const NodeAnnotation* ann = ast.annotations().Find(expr.id());
      const uint32_t field_number =
          ann != nullptr ? static_cast<uint32_t>(ann->field_number) : 0u;
      pool.Intern(field_number, s.field());
      WalkAndIntern(ast, s.operand(), pool);
      break;
    }
    case cel::ExprKindCase::kCallExpr: {
      const cel::CallExpr& c = expr.call_expr();
      if (c.has_target()) WalkAndIntern(ast, c.target(), pool);
      for (const cel::Expr& arg : c.args()) WalkAndIntern(ast, arg, pool);
      break;
    }
    case cel::ExprKindCase::kListExpr: {
      for (const cel::ListExprElement& e : expr.list_expr().elements()) {
        WalkAndIntern(ast, e.expr(), pool);
      }
      break;
    }
    case cel::ExprKindCase::kStructExpr: {
      for (const cel::StructExprField& f : expr.struct_expr().fields()) {
        WalkAndIntern(ast, f.value(), pool);
      }
      break;
    }
    case cel::ExprKindCase::kMapExpr: {
      for (const cel::MapExprEntry& e : expr.map_expr().entries()) {
        WalkAndIntern(ast, e.key(), pool);
        WalkAndIntern(ast, e.value(), pool);
      }
      break;
    }
    case cel::ExprKindCase::kComprehensionExpr: {
      const cel::ComprehensionExpr& ce = expr.comprehension_expr();
      WalkAndIntern(ast, ce.iter_range(), pool);
      WalkAndIntern(ast, ce.accu_init(), pool);
      WalkAndIntern(ast, ce.loop_condition(), pool);
      WalkAndIntern(ast, ce.loop_step(), pool);
      WalkAndIntern(ast, ce.result(), pool);
      break;
    }
    case cel::ExprKindCase::kConstant:
    case cel::ExprKindCase::kIdentExpr:
    case cel::ExprKindCase::kUnspecifiedExpr:
      break;
  }
}

}  // namespace

uint32_t FieldNamePool::Intern(uint32_t field_number,
                               absl::string_view name) {
  std::string key = absl::StrCat(field_number, ":", name);
  auto it = index_.find(key);
  if (it != index_.end()) return it->second;
  const uint32_t id = static_cast<uint32_t>(entries_.size());
  entries_.push_back(Entry{field_number, std::string(name)});
  index_.emplace(std::move(key), id);
  return id;
}

FieldNamePool FieldNamePool::FromTypedAst(const TypedAst& ast) {
  FieldNamePool pool;
  if (!ast.has_ast()) return pool;
  WalkAndIntern(ast, ast.ast().root_expr(), pool);
  return pool;
}

}  // namespace celwasm
