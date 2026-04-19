#include "compiler/codegen/expr_lower.h"

#include <cstdint>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "common/ast.h"
#include "common/constant.h"
#include "common/expr.h"
#include "common/operators.h"
#include "compiler/codegen/module.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {
namespace {

namespace op = ::google::api::expr::common;

// Forward decl: the node dispatcher.
absl::StatusOr<BinaryenExpressionRef> LowerExpr(const TypedAst& ast,
                                                const cel::Expr& expr,
                                                WasmModule& mod);

absl::Status UnimplementedKind(absl::string_view kind, int64_t id) {
  return absl::UnimplementedError(
      absl::StrCat("expr_lower: ", kind,
                   " is not yet supported (expr id ", id, ")"));
}

absl::Status UnimplementedRepr(absl::string_view op_name, Repr r, int64_t id) {
  return absl::UnimplementedError(absl::StrCat(
      "expr_lower: operator `", op_name, "` does not support Repr `",
      ReprName(r), "` (expr id ", id, ")"));
}

// The checker populates annotations() for every typed node.  Failing to
// find one means the AST was not run through PopulateAnnotations (which
// our frontend does) or the node is DYN (which static_subset rejects).
absl::StatusOr<Repr> ReprOf(const TypedAst& ast, const cel::Expr& e) {
  const NodeAnnotation* a = ast.annotations().Find(e.id());
  if (a == nullptr || a->repr == Repr::kUnknown) {
    return absl::FailedPreconditionError(absl::StrCat(
        "expr_lower: missing Repr annotation for expr id ", e.id(),
        " — was PopulateAnnotations run? did RejectDyn pass?"));
  }
  return a->repr;
}

absl::StatusOr<BinaryenExpressionRef> LowerConstant(const TypedAst& ast,
                                                    const cel::Expr& expr,
                                                    WasmModule& mod) {
  const cel::Constant& c = expr.const_expr();
  BinaryenModuleRef m = mod.raw();
  switch (c.kind_case()) {
    case cel::ConstantKindCase::kBool:
      return BinaryenConst(m, BinaryenLiteralInt32(c.bool_value() ? 1 : 0));
    case cel::ConstantKindCase::kInt:
      return BinaryenConst(m, BinaryenLiteralInt64(c.int_value()));
    case cel::ConstantKindCase::kUint:
      return BinaryenConst(m, BinaryenLiteralInt64(
                                  static_cast<int64_t>(c.uint_value())));
    case cel::ConstantKindCase::kDouble:
      return BinaryenConst(m, BinaryenLiteralFloat64(c.double_value()));
    default:
      return absl::UnimplementedError(absl::StrCat(
          "expr_lower: constant kind ", static_cast<int>(c.kind_case()),
          " not yet supported (expr id ", expr.id(), ")"));
  }
  (void)ast;
}

// Binary arithmetic.  Dispatch on the Repr of the first operand (the
// checker guarantees both operands share a type for these overloads).
absl::StatusOr<BinaryenExpressionRef> LowerArithmetic(
    absl::string_view name, Repr r, BinaryenExpressionRef lhs,
    BinaryenExpressionRef rhs, WasmModule& mod) {
  BinaryenModuleRef m = mod.raw();
  BinaryenOp bop;
  if (name == op::CelOperator::ADD) {
    if (r == Repr::kInt || r == Repr::kUint) bop = BinaryenAddInt64();
    else if (r == Repr::kDouble) bop = BinaryenAddFloat64();
    else return UnimplementedRepr(name, r, /*id=*/0);
  } else if (name == op::CelOperator::SUBTRACT) {
    if (r == Repr::kInt || r == Repr::kUint) bop = BinaryenSubInt64();
    else if (r == Repr::kDouble) bop = BinaryenSubFloat64();
    else return UnimplementedRepr(name, r, 0);
  } else if (name == op::CelOperator::MULTIPLY) {
    if (r == Repr::kInt || r == Repr::kUint) bop = BinaryenMulInt64();
    else if (r == Repr::kDouble) bop = BinaryenMulFloat64();
    else return UnimplementedRepr(name, r, 0);
  } else if (name == op::CelOperator::DIVIDE) {
    if (r == Repr::kInt) bop = BinaryenDivSInt64();
    else if (r == Repr::kUint) bop = BinaryenDivUInt64();
    else if (r == Repr::kDouble) bop = BinaryenDivFloat64();
    else return UnimplementedRepr(name, r, 0);
  } else if (name == op::CelOperator::MODULO) {
    // CEL has no % for double; checker rejects it.
    if (r == Repr::kInt) bop = BinaryenRemSInt64();
    else if (r == Repr::kUint) bop = BinaryenRemUInt64();
    else return UnimplementedRepr(name, r, 0);
  } else {
    return absl::InternalError(
        absl::StrCat("LowerArithmetic: unhandled op `", name, "`"));
  }
  return BinaryenBinary(m, bop, lhs, rhs);
}

absl::StatusOr<BinaryenExpressionRef> LowerComparison(
    absl::string_view name, Repr arg_r, BinaryenExpressionRef lhs,
    BinaryenExpressionRef rhs, WasmModule& mod) {
  BinaryenModuleRef m = mod.raw();
  BinaryenOp bop;
  const bool eq = (name == op::CelOperator::EQUALS);
  const bool ne = (name == op::CelOperator::NOT_EQUALS);
  if (eq || ne) {
    switch (arg_r) {
      case Repr::kBool:
        bop = eq ? BinaryenEqInt32() : BinaryenNeInt32();
        break;
      case Repr::kInt:
      case Repr::kUint:
        bop = eq ? BinaryenEqInt64() : BinaryenNeInt64();
        break;
      case Repr::kDouble:
        bop = eq ? BinaryenEqFloat64() : BinaryenNeFloat64();
        break;
      default:
        return UnimplementedRepr(name, arg_r, 0);
    }
    return BinaryenBinary(m, bop, lhs, rhs);
  }
  // Ordered comparisons.  Bool is not ordered in CEL.
  const bool lt = (name == op::CelOperator::LESS);
  const bool le = (name == op::CelOperator::LESS_EQUALS);
  const bool gt = (name == op::CelOperator::GREATER);
  const bool ge = (name == op::CelOperator::GREATER_EQUALS);
  switch (arg_r) {
    case Repr::kInt:
      bop = lt ? BinaryenLtSInt64()
          : le ? BinaryenLeSInt64()
          : gt ? BinaryenGtSInt64()
          : ge ? BinaryenGeSInt64()
               : BinaryenEqInt64();
      break;
    case Repr::kUint:
      bop = lt ? BinaryenLtUInt64()
          : le ? BinaryenLeUInt64()
          : gt ? BinaryenGtUInt64()
          : ge ? BinaryenGeUInt64()
               : BinaryenEqInt64();
      break;
    case Repr::kDouble:
      bop = lt ? BinaryenLtFloat64()
          : le ? BinaryenLeFloat64()
          : gt ? BinaryenGtFloat64()
          : ge ? BinaryenGeFloat64()
               : BinaryenEqFloat64();
      break;
    default:
      return UnimplementedRepr(name, arg_r, 0);
  }
  return BinaryenBinary(m, bop, lhs, rhs);
}

absl::StatusOr<BinaryenExpressionRef> LowerCall(const TypedAst& ast,
                                                const cel::Expr& expr,
                                                WasmModule& mod) {
  const cel::CallExpr& call = expr.call_expr();
  const std::string& fn = call.function();
  BinaryenModuleRef m = mod.raw();

  // Member form (x.f(...)) is only relevant for things like method
  // calls, which the MVP doesn't support.
  if (call.has_target()) {
    return UnimplementedKind(absl::StrCat("method call `", fn, "`"),
                             expr.id());
  }

  // Unary: logical not.
  if (fn == op::CelOperator::LOGICAL_NOT) {
    if (call.args().size() != 1) {
      return absl::InvalidArgumentError(
          absl::StrCat("`!_` takes 1 argument, got ", call.args().size()));
    }
    auto v = LowerExpr(ast, call.args()[0], mod);
    if (!v.ok()) return v.status();
    return BinaryenUnary(m, BinaryenEqZInt32(), *v);
  }

  // Unary: negate.
  if (fn == op::CelOperator::NEGATE) {
    if (call.args().size() != 1) {
      return absl::InvalidArgumentError(
          absl::StrCat("`-_` takes 1 argument, got ", call.args().size()));
    }
    auto v = LowerExpr(ast, call.args()[0], mod);
    if (!v.ok()) return v.status();
    auto arg_r = ReprOf(ast, call.args()[0]);
    if (!arg_r.ok()) return arg_r.status();
    switch (*arg_r) {
      case Repr::kInt:
        // 0 - x preserves i64 signedness; CEL also rejects negating uint.
        return BinaryenBinary(
            m, BinaryenSubInt64(),
            BinaryenConst(m, BinaryenLiteralInt64(0)), *v);
      case Repr::kDouble:
        return BinaryenUnary(m, BinaryenNegFloat64(), *v);
      default:
        return UnimplementedRepr(fn, *arg_r, expr.id());
    }
  }

  // Ternary: _?_:_
  if (fn == op::CelOperator::CONDITIONAL) {
    if (call.args().size() != 3) {
      return absl::InvalidArgumentError(
          absl::StrCat("`_?_:_` takes 3 arguments, got ", call.args().size()));
    }
    auto cond = LowerExpr(ast, call.args()[0], mod);
    if (!cond.ok()) return cond.status();
    auto t = LowerExpr(ast, call.args()[1], mod);
    if (!t.ok()) return t.status();
    auto f = LowerExpr(ast, call.args()[2], mod);
    if (!f.ok()) return f.status();
    return BinaryenIf(m, *cond, *t, *f);
  }

  // Short-circuit logical: _&&_ and _||_.
  if (fn == op::CelOperator::LOGICAL_AND ||
      fn == op::CelOperator::LOGICAL_OR) {
    if (call.args().size() != 2) {
      return absl::InvalidArgumentError(
          absl::StrCat("`", fn, "` takes 2 arguments, got ",
                       call.args().size()));
    }
    auto l = LowerExpr(ast, call.args()[0], mod);
    if (!l.ok()) return l.status();
    auto r = LowerExpr(ast, call.args()[1], mod);
    if (!r.ok()) return r.status();
    // a && b  ->  if (a) b else 0
    // a || b  ->  if (a) 1 else b
    const bool is_and = (fn == op::CelOperator::LOGICAL_AND);
    BinaryenExpressionRef if_true = is_and ? *r
                                           : BinaryenConst(m,
                                                 BinaryenLiteralInt32(1));
    BinaryenExpressionRef if_false = is_and ? BinaryenConst(m,
                                                  BinaryenLiteralInt32(0))
                                            : *r;
    return BinaryenIf(m, *l, if_true, if_false);
  }

  // Binary arithmetic and comparisons: both operands share a Repr;
  // dispatch on the first.
  if (call.args().size() == 2) {
    auto arg_r = ReprOf(ast, call.args()[0]);
    if (!arg_r.ok()) return arg_r.status();
    auto l = LowerExpr(ast, call.args()[0], mod);
    if (!l.ok()) return l.status();
    auto r = LowerExpr(ast, call.args()[1], mod);
    if (!r.ok()) return r.status();
    if (fn == op::CelOperator::ADD || fn == op::CelOperator::SUBTRACT ||
        fn == op::CelOperator::MULTIPLY || fn == op::CelOperator::DIVIDE ||
        fn == op::CelOperator::MODULO) {
      return LowerArithmetic(fn, *arg_r, *l, *r, mod);
    }
    if (fn == op::CelOperator::EQUALS || fn == op::CelOperator::NOT_EQUALS ||
        fn == op::CelOperator::LESS || fn == op::CelOperator::LESS_EQUALS ||
        fn == op::CelOperator::GREATER ||
        fn == op::CelOperator::GREATER_EQUALS) {
      return LowerComparison(fn, *arg_r, *l, *r, mod);
    }
  }

  return UnimplementedKind(absl::StrCat("call `", fn, "`"), expr.id());
}

absl::StatusOr<BinaryenExpressionRef> LowerExpr(const TypedAst& ast,
                                                const cel::Expr& expr,
                                                WasmModule& mod) {
  switch (expr.kind_case()) {
    case cel::ExprKindCase::kConstant:
      return LowerConstant(ast, expr, mod);
    case cel::ExprKindCase::kCallExpr:
      return LowerCall(ast, expr, mod);
    case cel::ExprKindCase::kIdentExpr:
      return UnimplementedKind("IdentExpr", expr.id());
    case cel::ExprKindCase::kSelectExpr:
      return UnimplementedKind("SelectExpr", expr.id());
    case cel::ExprKindCase::kListExpr:
      return UnimplementedKind("ListExpr", expr.id());
    case cel::ExprKindCase::kStructExpr:
      return UnimplementedKind("StructExpr", expr.id());
    case cel::ExprKindCase::kMapExpr:
      return UnimplementedKind("MapExpr", expr.id());
    case cel::ExprKindCase::kComprehensionExpr:
      return UnimplementedKind("ComprehensionExpr", expr.id());
    case cel::ExprKindCase::kUnspecifiedExpr:
      return absl::InvalidArgumentError(
          absl::StrCat("expr_lower: unspecified expr (id ", expr.id(), ")"));
  }
  return absl::InternalError("expr_lower: unreachable kind_case");
}

}  // namespace

BinaryenType WasmTypeFor(Repr r) {
  switch (r) {
    case Repr::kBool:
      return BinaryenTypeInt32();
    case Repr::kInt:
    case Repr::kUint:
    case Repr::kEnum:
    case Repr::kDuration:
    case Repr::kTimestamp:
      return BinaryenTypeInt64();
    case Repr::kDouble:
      return BinaryenTypeFloat64();
    case Repr::kType:
      return BinaryenTypeInt32();
    default:
      return BinaryenTypeNone();
  }
}

absl::StatusOr<LoweredFunction> LowerToEvalFunction(const TypedAst& ast,
                                                   absl::string_view func_name,
                                                   WasmModule& mod) {
  if (!ast.has_ast()) {
    return absl::InvalidArgumentError(
        "LowerToEvalFunction: TypedAst has no underlying cel::Ast");
  }
  const cel::Expr& root = ast.ast().root_expr();
  auto root_r = ReprOf(ast, root);
  if (!root_r.ok()) return root_r.status();
  BinaryenType result_type = WasmTypeFor(*root_r);
  if (result_type == BinaryenTypeNone()) {
    return absl::UnimplementedError(absl::StrCat(
        "expr_lower: root Repr `", ReprName(*root_r),
        "` has no scalar ABI lowering in M2"));
  }
  auto body = LowerExpr(ast, root, mod);
  if (!body.ok()) return body.status();
  mod.AddFunction(func_name, /*params=*/{}, result_type, /*local_types=*/{},
                  *body);
  BinaryenFunctionRef fn =
      BinaryenGetFunction(mod.raw(), std::string(func_name).c_str());
  if (fn == nullptr) {
    return absl::InternalError(absl::StrCat(
        "expr_lower: BinaryenGetFunction returned null for `", func_name,
        "` immediately after AddFunction"));
  }
  return LoweredFunction{fn, result_type, *root_r};
}

}  // namespace celwasm
