#include "compiler/codegen/attribute_pool.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "common/expr.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {
namespace {

std::string MakeKey(absl::string_view variable,
                    absl::Span<const std::string> qualifiers) {
  // '\0' is not a valid identifier character so it's a safe
  // separator between the variable and each qualifier.
  return absl::StrCat(variable, std::string(1, '\0'),
                      absl::StrJoin(qualifiers, std::string(1, '\0')));
}

void WalkAndIntern(const cel::Expr& expr, AttributePool& pool);

// Recurses into every child of `expr` that can transitively contain a
// SelectExpr.  Extracted to keep `WalkAndIntern` under the lint's
// function-size threshold; only kSelectExpr has non-trivial
// per-kind logic and it lives on the fast path in `WalkAndIntern`.
void WalkChildren(const cel::Expr& expr, AttributePool& pool) {
  switch (expr.kind_case()) {
    case cel::ExprKindCase::kCallExpr: {
      const cel::CallExpr& c = expr.call_expr();
      if (c.has_target()) WalkAndIntern(c.target(), pool);
      for (const cel::Expr& arg : c.args()) {
        WalkAndIntern(arg, pool);
      }
      break;
    }
    case cel::ExprKindCase::kListExpr: {
      for (const cel::ListExprElement& e : expr.list_expr().elements()) {
        WalkAndIntern(e.expr(), pool);
      }
      break;
    }
    case cel::ExprKindCase::kStructExpr: {
      for (const cel::StructExprField& f : expr.struct_expr().fields()) {
        WalkAndIntern(f.value(), pool);
      }
      break;
    }
    case cel::ExprKindCase::kMapExpr: {
      for (const cel::MapExprEntry& e : expr.map_expr().entries()) {
        WalkAndIntern(e.key(), pool);
        WalkAndIntern(e.value(), pool);
      }
      break;
    }
    case cel::ExprKindCase::kComprehensionExpr: {
      const cel::ComprehensionExpr& ce = expr.comprehension_expr();
      WalkAndIntern(ce.iter_range(), pool);
      WalkAndIntern(ce.accu_init(), pool);
      WalkAndIntern(ce.loop_condition(), pool);
      WalkAndIntern(ce.loop_step(), pool);
      WalkAndIntern(ce.result(), pool);
      break;
    }
    case cel::ExprKindCase::kSelectExpr:
    case cel::ExprKindCase::kConstant:
    case cel::ExprKindCase::kIdentExpr:
    case cel::ExprKindCase::kUnspecifiedExpr:
      break;
  }
}

// Pre-order walk: every SelectExpr contributes its full attribute
// path (rooted at the enclosing identifier) to the pool, then the
// walk recurses into child expressions.  The outer-most select is
// interned first so IDs match the codegen's LowerSelectOperand
// order, which interns the outer path before recursing.
void WalkAndIntern(const cel::Expr& expr, AttributePool& pool) {
  if (expr.kind_case() == cel::ExprKindCase::kSelectExpr) {
    auto entry = AttributePool::BuildPath(expr);
    pool.Intern(entry.variable, entry.qualifiers);
    WalkAndIntern(expr.select_expr().operand(), pool);
    return;
  }
  WalkChildren(expr, pool);
}

}  // namespace

uint32_t AttributePool::Intern(absl::string_view variable,
                               absl::Span<const std::string> qualifiers) {
  std::string key = MakeKey(variable, qualifiers);
  auto it = index_.find(key);
  if (it != index_.end()) return it->second;
  const auto id = static_cast<uint32_t>(entries_.size());
  Entry e;
  e.variable = std::string(variable);
  e.qualifiers.assign(qualifiers.begin(), qualifiers.end());
  entries_.push_back(std::move(e));
  index_.emplace(std::move(key), id);
  return id;
}

AttributePool::Entry AttributePool::BuildPath(const cel::Expr& select_expr) {
  Entry out;
  // Walk the operand chain inner-ward, collecting field names in
  // outer-to-inner order; then reverse so the qualifier list reads
  // root-to-leaf.  `a.b.c` starts at the outer select `(.c)` with
  // operand `(.b)` whose operand is `a`.  We push `c`, then `b`,
  // then hit ident `a`; reverse gives `[b, c]`.
  std::vector<std::string> qualifiers;
  qualifiers.emplace_back(select_expr.select_expr().field());
  const cel::Expr* cur = &select_expr.select_expr().operand();
  while (true) {
    if (cur->kind_case() == cel::ExprKindCase::kSelectExpr) {
      qualifiers.emplace_back(cur->select_expr().field());
      cur = &cur->select_expr().operand();
      continue;
    }
    if (cur->kind_case() == cel::ExprKindCase::kIdentExpr) {
      out.variable = cur->ident_expr().name();
    }
    // Any other root (CallExpr, etc.) leaves variable empty: the
    // path is unresolvable, so the host will see NONE for every
    // pattern and the compiler still emits a stable attr_id.
    break;
  }
  std::reverse(qualifiers.begin(), qualifiers.end());
  out.qualifiers = std::move(qualifiers);
  return out;
}

AttributePool AttributePool::FromTypedAst(const TypedAst& ast) {
  AttributePool pool;
  if (!ast.has_ast()) return pool;
  WalkAndIntern(ast.ast().root_expr(), pool);
  return pool;
}

}  // namespace celwasm
