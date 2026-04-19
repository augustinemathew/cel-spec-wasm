#include "compiler/ir/static_subset.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "common/ast.h"
#include "common/ast/metadata.h"
#include "common/expr.h"

namespace celwasm {

namespace {

struct Violation {
  int64_t expr_id;
  const char* kind;    // "dyn", "error", "unset"
  std::string detail;  // optional additional context (type spec kind)
};

// Reports whether the given TypeSpec is acceptable in the static subset.
// Returns an empty string if the spec is accepted, or a short label otherwise.
const char* UnacceptableLabel(const cel::TypeSpec& type) {
  if (type.has_dyn())   return "dyn";
  if (type.has_error()) return "error";
  // ParamTypeSpec / FunctionTypeSpec / AbstractType only appear as type
  // parameters or macro-only constructs in practice; we don't accept them as
  // runtime-typed expressions in the static subset.
  using cel::TypeSpecKind;
  const TypeSpecKind& k = type.type_kind();
  if (absl::holds_alternative<cel::FunctionTypeSpec>(k)) return "function";
  if (absl::holds_alternative<cel::ParamTypeSpec>(k))    return "type-param";
  if (absl::holds_alternative<cel::UnsetTypeSpec>(k))    return "unset";
  return nullptr;
}

void CheckNode(const cel::Expr& node, const cel::Ast::TypeMap& types,
               std::vector<Violation>& out);

void CheckChildren(const cel::Expr& node, const cel::Ast::TypeMap& types,
                   std::vector<Violation>& out) {
  switch (node.kind_case()) {
    case cel::ExprKindCase::kUnspecifiedExpr:
    case cel::ExprKindCase::kConstant:
    case cel::ExprKindCase::kIdentExpr:
      return;
    case cel::ExprKindCase::kSelectExpr:
      if (node.select_expr().has_operand()) {
        CheckNode(node.select_expr().operand(), types, out);
      }
      return;
    case cel::ExprKindCase::kCallExpr: {
      const auto& call = node.call_expr();
      if (call.has_target()) CheckNode(call.target(), types, out);
      for (const auto& arg : call.args()) CheckNode(arg, types, out);
      return;
    }
    case cel::ExprKindCase::kListExpr:
      for (const auto& elem : node.list_expr().elements()) {
        if (elem.has_expr()) CheckNode(elem.expr(), types, out);
      }
      return;
    case cel::ExprKindCase::kStructExpr:
      for (const auto& field : node.struct_expr().fields()) {
        if (field.has_value()) CheckNode(field.value(), types, out);
      }
      return;
    case cel::ExprKindCase::kMapExpr:
      for (const auto& entry : node.map_expr().entries()) {
        if (entry.has_key())   CheckNode(entry.key(),   types, out);
        if (entry.has_value()) CheckNode(entry.value(), types, out);
      }
      return;
    case cel::ExprKindCase::kComprehensionExpr: {
      const auto& c = node.comprehension_expr();
      if (c.has_iter_range())     CheckNode(c.iter_range(),     types, out);
      if (c.has_accu_init())      CheckNode(c.accu_init(),      types, out);
      if (c.has_loop_condition()) CheckNode(c.loop_condition(), types, out);
      if (c.has_loop_step())      CheckNode(c.loop_step(),      types, out);
      if (c.has_result())         CheckNode(c.result(),         types, out);
      return;
    }
  }
}

void CheckNode(const cel::Expr& node, const cel::Ast::TypeMap& types,
               std::vector<Violation>& out) {
  const int64_t id = node.id();
  if (id != 0) {
    auto it = types.find(id);
    if (it == types.end()) {
      // Checker omits DYN entries to save space.
      out.push_back({id, "dyn", "no type_map entry"});
    } else if (const char* label = UnacceptableLabel(it->second);
               label != nullptr) {
      out.push_back({id, label, cel::FormatTypeSpec(it->second)});
    }
  }
  CheckChildren(node, types, out);
}

}  // namespace

absl::Status RejectDyn(const cel::Ast& ast) {
  if (!ast.is_checked()) {
    return absl::FailedPreconditionError(
        "RejectDyn requires a checked AST (type_map must be populated)");
  }
  std::vector<Violation> violations;
  CheckNode(ast.root_expr(), ast.type_map(), violations);
  if (violations.empty()) return absl::OkStatus();

  std::vector<std::string> lines;
  lines.reserve(violations.size());
  for (const auto& v : violations) {
    lines.push_back(absl::StrCat("  expr id=", v.expr_id, " is ", v.kind,
                                 " (", v.detail, ")"));
  }
  return absl::InvalidArgumentError(
      absl::StrCat("expression is not in the static subset:\n",
                   absl::StrJoin(lines, "\n")));
}

}  // namespace celwasm
