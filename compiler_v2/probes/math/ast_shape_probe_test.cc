// math extension — AST-shape probe.
//
// Parses + type-checks a representative battery of `math.*`
// expressions against cel-cpp's standard macros + the math macros
// (math.greatest / math.least desugaring) and the standard checker
// library + MathCheckerLibrary().  Dumps the resulting CheckedExpr
// (DebugString) and the reference_map overload ids so a human can
// read, for every distinct shape:
//
//   - whether math.greatest / math.least are expanded by the parser
//     macros into calls of the internal functions math.@max /
//     math.@min (vs. surviving as receiver-style math.greatest calls),
//   - the post-macro arg shape (scalar stays scalar, 3+ args collapse
//     into a single list literal, an explicit list literal stays a
//     single list arg),
//   - the resolved overload id(s) the checker assigns to each call,
//   - the checker-assigned result type.
//
// Every other math function (ceil/floor/round/trunc, abs/sign,
// isNaN/isInf/isFinite, bitAnd/bitOr/bitXor/bitNot/bitShiftLeft/
// bitShiftRight, sqrt) is a plain namespaced function call with no
// macro; the probe records each one's overload id + result type.
//
// Research only — nothing here is production code.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/syntax.pb.h"
#include "checker/standard_library.h"
#include "checker/type_checker.h"
#include "checker/type_checker_builder_factory.h"
#include "checker/validation_result.h"
#include "common/ast.h"
#include "common/ast_proto.h"
#include "common/source.h"
#include "extensions/math_ext_decls.h"
#include "extensions/math_ext_macros.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "parser/macro_registry.h"
#include "parser/options.h"
#include "parser/parser.h"
#include "parser/standard_macros.h"
#include "google/protobuf/descriptor.h"

namespace celwasm::probes {
namespace {

using ::cel::expr::CheckedExpr;
using ::cel::expr::Expr;

// Parse + check a single expression against the standard surface plus
// the math macros and math checker library.
absl::StatusOr<CheckedExpr> ParseAndCheck(absl::string_view expression) {
  cel::ParserOptions parser_opts;

  cel::MacroRegistry registry;
  if (auto s = cel::RegisterStandardMacros(registry, parser_opts); !s.ok()) {
    return s;
  }
  if (auto s = cel::extensions::RegisterMathMacros(registry, parser_opts);
      !s.ok()) {
    return s;
  }

  auto source = cel::NewSource(expression, "<probe>");
  if (!source.ok()) return source.status();

  auto parsed =
      google::api::expr::parser::Parse(**source, registry, parser_opts);
  if (!parsed.ok()) return parsed.status();

  auto builder = cel::CreateTypeCheckerBuilder(
      google::protobuf::DescriptorPool::generated_pool());
  if (!builder.ok()) return builder.status();
  if (auto s = (*builder)->AddLibrary(cel::StandardCheckerLibrary()); !s.ok()) {
    return s;
  }
  if (auto s = (*builder)->AddLibrary(cel::extensions::MathCheckerLibrary());
      !s.ok()) {
    return s;
  }

  auto checker = std::move(**builder).Build();
  if (!checker.ok()) return checker.status();

  auto ast = cel::CreateAstFromParsedExpr(*parsed);
  if (!ast.ok()) return ast.status();
  auto result = (*checker)->Check(std::move(*ast));
  if (!result.ok()) return result.status();
  if (!result->IsValid()) {
    return absl::InvalidArgumentError(
        absl::StrCat("type check failed: ", result->FormatError()));
  }

  auto checked_ast = result->ReleaseAst();
  if (!checked_ast.ok()) return checked_ast.status();
  CheckedExpr out;
  if (auto s = cel::AstToCheckedExpr(**checked_ast, &out); !s.ok()) return s;
  return out;
}

// Depth-first search for the first call node with the given function
// name.  Returns nullptr if none found.
const Expr* FindCall(const Expr& root, absl::string_view fn) {
  if (root.expr_kind_case() == Expr::kCallExpr &&
      root.call_expr().function() == fn) {
    return &root;
  }
  switch (root.expr_kind_case()) {
    case Expr::kCallExpr:
      if (root.call_expr().has_target()) {
        if (const auto* x = FindCall(root.call_expr().target(), fn);
            x != nullptr) {
          return x;
        }
      }
      for (const auto& a : root.call_expr().args()) {
        if (const auto* x = FindCall(a, fn); x != nullptr) return x;
      }
      break;
    case Expr::kListExpr:
      for (const auto& e : root.list_expr().elements()) {
        if (const auto* x = FindCall(e, fn); x != nullptr) return x;
      }
      break;
    case Expr::kSelectExpr:
      if (const auto* x = FindCall(root.select_expr().operand(), fn);
          x != nullptr) {
        return x;
      }
      break;
    default:
      break;
  }
  return nullptr;
}

std::vector<std::string> OverloadIds(const CheckedExpr& ce, int64_t expr_id) {
  std::vector<std::string> out;
  auto it = ce.reference_map().find(expr_id);
  if (it == ce.reference_map().end()) return out;
  for (const auto& o : it->second.overload_id()) out.push_back(o);
  return out;
}

// Pretty-print one expression's full result: pass/fail, the
// CheckedExpr DebugString, and a flattened list of (function -> [overload
// ids], result type) for every call node, so the dump is greppable.
void Probe(absl::string_view expr) {
  std::cerr << "\n========================================================\n"
            << "EXPR: " << expr << "\n"
            << "--------------------------------------------------------\n";
  auto ce_or = ParseAndCheck(expr);
  if (!ce_or.ok()) {
    std::cerr << "RESULT: FAILED\n  status: " << ce_or.status() << "\n";
    return;
  }
  const auto& ce = ce_or.value();
  std::cerr << "RESULT: OK\n";

  // Walk the tree, printing every call node's function + overload ids.
  std::vector<const Expr*> stack = {&ce.expr()};
  while (!stack.empty()) {
    const Expr* e = stack.back();
    stack.pop_back();
    if (e->expr_kind_case() == Expr::kCallExpr) {
      const auto& c = e->call_expr();
      auto ids = OverloadIds(ce, e->id());
      std::cerr << "  CALL fn=\"" << c.function() << "\""
                << " has_target=" << (c.has_target() ? "true" : "false")
                << " args=" << c.args_size() << " overload_ids=[";
      for (size_t i = 0; i < ids.size(); ++i) {
        std::cerr << (i ? ", " : "") << ids[i];
      }
      std::cerr << "]";
      // Describe arg kinds (scalar const vs list literal vs other).
      std::cerr << " arg_kinds=[";
      for (int i = 0; i < c.args_size(); ++i) {
        const auto& a = c.args(i);
        std::string k = "other";
        switch (a.expr_kind_case()) {
          case Expr::kConstExpr:
            k = "const";
            break;
          case Expr::kListExpr:
            k = absl::StrCat("list(", a.list_expr().elements_size(), ")");
            break;
          case Expr::kCallExpr:
            k = "call";
            break;
          case Expr::kIdentExpr:
            k = "ident";
            break;
          default:
            break;
        }
        std::cerr << (i ? ", " : "") << k;
      }
      std::cerr << "]\n";
      if (c.has_target()) stack.push_back(&c.target());
      for (const auto& a : c.args()) stack.push_back(&a);
    } else if (e->expr_kind_case() == Expr::kListExpr) {
      for (const auto& el : e->list_expr().elements()) stack.push_back(&el);
    } else if (e->expr_kind_case() == Expr::kSelectExpr) {
      stack.push_back(&e->select_expr().operand());
    }
  }

  // Root result type.
  auto it = ce.type_map().find(ce.expr().id());
  if (it != ce.type_map().end()) {
    std::cerr << "  ROOT TYPE: " << it->second.ShortDebugString() << "\n";
  }
  std::cerr << "--- CheckedExpr ---\n" << ce.DebugString() << "\n";
}

// ---- greatest / least: every distinct arg shape ----

TEST(MathAstShape, GreatestLeastShapes) {
  for (absl::string_view e : {
           // single scalar arg (int / double / uint)
           "math.greatest(5)",
           "math.greatest(-5.0)",
           "math.greatest(5u)",
           "math.least(5)",
           // single list-literal arg (homogeneous + mixed numeric)
           "math.greatest([5.4, 10, 3u, -5.0, 3.5])",
           "math.least([5.4, 10, 3u, -5.0, 3.5])",
           // single list with dyn elements
           "math.greatest([dyn(5.4), dyn(10), dyn(3u), dyn(-5.0), dyn(3.5)])",
           // two args same type
           "math.greatest(1, 1)",
           "math.greatest(1.0, 1.0)",
           "math.greatest(1u, 1u)",
           // two args cross type
           "math.greatest(1, 1.0)",
           "math.greatest(1, 1u)",
           "math.greatest(1.0, 1u)",
           "math.greatest(1u, 1.0)",
           // three args same type
           "math.greatest(10, 1, 3)",
           // three+ args cross type (5 mixed numerics)
           "math.greatest(5.4, 10, 3u, -5.0, 9223372036854775807)",
           "math.least(5.4, 10, 3u, -5.0, 9223372036854775807)",
       }) {
    Probe(e);
  }
}

// ---- single-scalar greatest/least: the open question ----
//
// Does math.least(5) emit a call at all (and which overload), or pass
// through?  Probe(...) above already dumps it; this test asserts the
// fact so the answer is captured as a green/red signal, not just text.
TEST(MathAstShape, SingleScalarLeastEmitsMinUnaryOverload) {
  auto ce_or = ParseAndCheck("math.least(5)");
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = ce_or.value();
  const Expr* call = FindCall(ce.expr(), "math.@min");
  ASSERT_NE(call, nullptr) << "math.least(5) did NOT desugar to math.@min";
  EXPECT_FALSE(call->call_expr().has_target())
      << "expanded math.@min should be a global call, no receiver target";
  EXPECT_EQ(call->call_expr().args_size(), 1);
  EXPECT_THAT(OverloadIds(ce, call->id()),
              ::testing::ElementsAre("math_@min_int"));
}

TEST(MathAstShape, GreatestExpandsToMaxNotReceiverCall) {
  auto ce_or = ParseAndCheck("math.greatest(1, 2)");
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = ce_or.value();
  // The original receiver-style `math.greatest` must NOT survive.
  EXPECT_EQ(FindCall(ce.expr(), "math.greatest"), nullptr)
      << "math.greatest survived as a call — macro did not fire";
  const Expr* call = FindCall(ce.expr(), "math.@max");
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->call_expr().args_size(), 2);
  EXPECT_THAT(OverloadIds(ce, call->id()),
              ::testing::ElementsAre("math_@max_int_int"));
}

TEST(MathAstShape, ThreePlusArgsCollapseIntoSingleListArg) {
  auto ce_or = ParseAndCheck("math.greatest(10, 1, 3)");
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = ce_or.value();
  const Expr* call = FindCall(ce.expr(), "math.@max");
  ASSERT_NE(call, nullptr);
  ASSERT_EQ(call->call_expr().args_size(), 1)
      << "3 args should collapse to a single list-literal arg";
  EXPECT_EQ(call->call_expr().args(0).expr_kind_case(), Expr::kListExpr);
  EXPECT_EQ(call->call_expr().args(0).list_expr().elements_size(), 3);
  EXPECT_THAT(OverloadIds(ce, call->id()),
              ::testing::ElementsAre("math_@max_list_int"));
}

TEST(MathAstShape, ExplicitListLiteralStaysSingleListArg) {
  auto ce_or = ParseAndCheck("math.greatest([1, 2, 3])");
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = ce_or.value();
  const Expr* call = FindCall(ce.expr(), "math.@max");
  ASSERT_NE(call, nullptr);
  ASSERT_EQ(call->call_expr().args_size(), 1);
  EXPECT_EQ(call->call_expr().args(0).expr_kind_case(), Expr::kListExpr);
  EXPECT_THAT(OverloadIds(ce, call->id()),
              ::testing::ElementsAre("math_@max_list_int"));
}

// ---- the plain (non-macro) namespaced functions ----

TEST(MathAstShape, PlainFunctions) {
  for (absl::string_view e : {
           // rounding
           "math.ceil(1.2)",
           "math.floor(-1.2)",
           "math.round(1.5)",
           "math.trunc(-1.2)",
           // signedness
           "math.abs(-11)",
           "math.abs(-11.5)",
           "math.abs(1u)",
           "math.sign(-11)",
           "math.sign(-32.0)",
           "math.sign(0u)",
           // fp helpers
           "math.isNaN(0.0/0.0)",
           "math.isInf(1.0/0.0)",
           "math.isFinite(1.0/1.5)",
           // bitwise
           "math.bitAnd(1, 2)",
           "math.bitAnd(1u, 2u)",
           "math.bitOr(1, 2)",
           "math.bitOr(1u, 4u)",
           "math.bitXor(1, 3)",
           "math.bitXor(1u, 3u)",
           "math.bitNot(1)",
           "math.bitNot(1u)",
           "math.bitShiftLeft(1, 2)",
           "math.bitShiftLeft(1u, 2)",
           "math.bitShiftRight(1024, 2)",
           "math.bitShiftRight(1024u, 2)",
           // sqrt (version 2)
           "math.sqrt(4)",
           "math.sqrt(4.0)",
           "math.sqrt(4u)",
       }) {
    Probe(e);
  }
}

// Assert the overload ids for one representative of each plain
// function family so the seed table is captured as a green signal.
TEST(MathAstShape, PlainFunctionOverloadIds) {
  struct Case {
    absl::string_view expr;
    absl::string_view fn;
    absl::string_view overload;
  };
  for (const Case& c : {
           Case{"math.ceil(1.2)", "math.ceil", "math_ceil_double"},
           Case{"math.floor(1.2)", "math.floor", "math_floor_double"},
           Case{"math.round(1.5)", "math.round", "math_round_double"},
           Case{"math.trunc(1.2)", "math.trunc", "math_trunc_double"},
           Case{"math.abs(-11)", "math.abs", "math_abs_int"},
           Case{"math.abs(1u)", "math.abs", "math_abs_uint"},
           Case{"math.abs(-11.5)", "math.abs", "math_abs_double"},
           Case{"math.sign(-11)", "math.sign", "math_sign_int"},
           Case{"math.isNaN(0.0/0.0)", "math.isNaN", "math_isNaN_double"},
           Case{"math.isInf(1.0/0.0)", "math.isInf", "math_isInf_double"},
           Case{"math.isFinite(1.0/1.5)", "math.isFinite",
                "math_isFinite_double"},
           Case{"math.bitAnd(1, 2)", "math.bitAnd", "math_bitAnd_int_int"},
           Case{"math.bitAnd(1u, 2u)", "math.bitAnd", "math_bitAnd_uint_uint"},
           Case{"math.bitOr(1, 2)", "math.bitOr", "math_bitOr_int_int"},
           Case{"math.bitXor(1, 3)", "math.bitXor", "math_bitXor_int_int"},
           Case{"math.bitNot(1)", "math.bitNot", "math_bitNot_int_int"},
           Case{"math.bitNot(1u)", "math.bitNot", "math_bitNot_uint_uint"},
           Case{"math.bitShiftLeft(1, 2)", "math.bitShiftLeft",
                "math_bitShiftLeft_int_int"},
           Case{"math.bitShiftLeft(1u, 2)", "math.bitShiftLeft",
                "math_bitShiftLeft_uint_int"},
           Case{"math.bitShiftRight(1024, 2)", "math.bitShiftRight",
                "math_bitShiftRight_int_int"},
           Case{"math.bitShiftRight(1024u, 2)", "math.bitShiftRight",
                "math_bitShiftRight_uint_int"},
           Case{"math.sqrt(4.0)", "math.sqrt", "math_sqrt_double"},
           Case{"math.sqrt(4)", "math.sqrt", "math_sqrt_int"},
           Case{"math.sqrt(4u)", "math.sqrt", "math_sqrt_uint"},
       }) {
    auto ce_or = ParseAndCheck(c.expr);
    ASSERT_TRUE(ce_or.ok()) << c.expr << ": " << ce_or.status();
    const Expr* call = FindCall(ce_or->expr(), c.fn);
    ASSERT_NE(call, nullptr) << "no call to " << c.fn << " in " << c.expr;
    EXPECT_THAT(OverloadIds(*ce_or, call->id()),
                ::testing::ElementsAre(c.overload))
        << "expr=" << c.expr;
  }
}

// The full cross-product of min/max numeric overloads the checker can
// emit — the milestone seed table must cover every one.  This test
// exercises each pairwise + unary + list overload and asserts the
// exact id string.
TEST(MathAstShape, MinMaxOverloadCrossProduct) {
  struct Case {
    absl::string_view expr;
    absl::string_view fn;
    absl::string_view overload;
  };
  for (const Case& c : {
           // unary
           Case{"math.greatest(1)", "math.@max", "math_@max_int"},
           Case{"math.greatest(1.0)", "math.@max", "math_@max_double"},
           Case{"math.greatest(1u)", "math.@max", "math_@max_uint"},
           Case{"math.least(1)", "math.@min", "math_@min_int"},
           Case{"math.least(1.0)", "math.@min", "math_@min_double"},
           Case{"math.least(1u)", "math.@min", "math_@min_uint"},
           // pairwise same type
           Case{"math.greatest(1, 2)", "math.@max", "math_@max_int_int"},
           Case{"math.greatest(1.0, 2.0)", "math.@max",
                "math_@max_double_double"},
           Case{"math.greatest(1u, 2u)", "math.@max", "math_@max_uint_uint"},
           // pairwise cross type
           Case{"math.greatest(1, 1.0)", "math.@max", "math_@max_int_double"},
           Case{"math.greatest(1, 1u)", "math.@max", "math_@max_int_uint"},
           Case{"math.greatest(1.0, 1)", "math.@max", "math_@max_double_int"},
           Case{"math.greatest(1.0, 1u)", "math.@max", "math_@max_double_uint"},
           Case{"math.greatest(1u, 1)", "math.@max", "math_@max_uint_int"},
           Case{"math.greatest(1u, 1.0)", "math.@max", "math_@max_uint_double"},
           // list overloads (homogeneous element type)
           Case{"math.greatest([1, 2, 3])", "math.@max", "math_@max_list_int"},
           Case{"math.greatest([1.0, 2.0])", "math.@max",
                "math_@max_list_double"},
           Case{"math.greatest([1u, 2u])", "math.@max", "math_@max_list_uint"},
           Case{"math.least([1, 2, 3])", "math.@min", "math_@min_list_int"},
       }) {
    auto ce_or = ParseAndCheck(c.expr);
    ASSERT_TRUE(ce_or.ok()) << c.expr << ": " << ce_or.status();
    const Expr* call = FindCall(ce_or->expr(), c.fn);
    ASSERT_NE(call, nullptr) << "no call to " << c.fn << " in " << c.expr;
    EXPECT_THAT(OverloadIds(*ce_or, call->id()),
                ::testing::ElementsAre(c.overload))
        << "expr=" << c.expr;
  }
}

}  // namespace
}  // namespace celwasm::probes
