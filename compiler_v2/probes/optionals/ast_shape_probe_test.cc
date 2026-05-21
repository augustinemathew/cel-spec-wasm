// M13 optionals — AST-shape probe.
//
// Parses + type-checks a battery of representative optional
// expressions against cel-cpp's OptionalCheckerLibrary + the
// OptMapMacro / OptFlatMapMacro / enable_optional_syntax parser
// surface.  Dumps the resulting CheckedExpr and asserts the
// AST-shape facts the M13 plan needs to commit to:
//
//   - `.?field` reaches as `Call("_?._", [obj, "field"])`.
//   - `[?key]` reaches as `Call("_[?_]", [obj, key])`.
//   - `{?key: val}` carries `optional_entry: true` per-entry.
//   - `[?elem]` populates `CreateList.optional_indices`.
//   - `optMap` / `optFlatMap` desugar into a Conditional + Comprehension
//     (cel.bind-style: iter_var="#unused", iter_range=[], cond=false).
//   - The checker assigns each optional call a specific overload_id
//     (`optional_of`, `optional_value`, `select_optional_field`, etc.).
//   - `type(optional.none())` resolves `optional_type` as an ident of
//     type `type(optional(V))`.
//
// Each test asserts the structural fact AND prints the relevant
// AST slice on failure for human inspection.

#include <iostream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/syntax.pb.h"
#include "checker/optional.h"
#include "checker/standard_library.h"
#include "checker/type_check_issue.h"
#include "checker/type_checker.h"
#include "checker/type_checker_builder_factory.h"
#include "checker/validation_result.h"
#include "common/ast.h"
#include "common/ast_proto.h"
#include "common/source.h"
#include "extensions/bindings_ext.h"
#include "extensions/strings.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "parser/macro.h"
#include "parser/macro_registry.h"
#include "parser/options.h"
#include "parser/parser.h"
#include "parser/standard_macros.h"
#include "google/protobuf/descriptor.h"

namespace celwasm::probes {
namespace {

using ::cel::expr::CheckedExpr;
using ::cel::expr::Expr;
using ::cel::expr::ParsedExpr;

// One-shot parse + check pipeline mirroring our
// `parse_and_check.cc` config, plus the optional plumbing the M13
// design proposes adding.
absl::StatusOr<CheckedExpr> ParseAndCheck(absl::string_view expression) {
  // Parser side: enable_optional_syntax + the two optional-aware
  // macros (OptMap / OptFlatMap).  Standard macros stay in to keep
  // has() / map() / etc. working in compound expressions.
  cel::ParserOptions parser_opts;
  parser_opts.enable_optional_syntax = true;

  cel::MacroRegistry registry;
  if (auto s = cel::RegisterStandardMacros(registry, parser_opts); !s.ok()) {
    return s;
  }
  if (auto s = cel::extensions::RegisterBindingsMacros(registry, parser_opts);
      !s.ok()) {
    return s;
  }
  // OptMap / OptFlatMap macros are already registered by
  // RegisterStandardMacros when parser_opts.enable_optional_syntax
  // is true.  No additional registration needed — this was an
  // assumption surfaced by the probe.

  auto source = cel::NewSource(expression, "<probe>");
  if (!source.ok()) return source.status();

  auto parsed =
      google::api::expr::parser::Parse(**source, registry, parser_opts);
  if (!parsed.ok()) return parsed.status();

  // Checker side: standard + optional decls.
  auto builder = cel::CreateTypeCheckerBuilder(
      google::protobuf::DescriptorPool::generated_pool());
  if (!builder.ok()) return builder.status();
  if (auto s = (*builder)->AddLibrary(cel::StandardCheckerLibrary()); !s.ok()) {
    return s;
  }
  if (auto s = (*builder)->AddLibrary(cel::OptionalCheckerLibrary()); !s.ok()) {
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

// Dump on assertion failure to make the structural mismatch
// obvious without rerunning by hand.
void DumpOnFailure(const CheckedExpr& ce, absl::string_view expression) {
  if (::testing::Test::HasFailure()) {
    std::cerr << "\n--- expression ---\n"
              << expression << "\n--- CheckedExpr ---\n"
              << ce.DebugString() << "\n";
  }
}

// Find the call expression whose function name matches `fn` by
// walking the entire expression tree (depth-first).  Returns nullptr
// if no such call is found.
const Expr* FindCall(const Expr& root, absl::string_view fn) {
  if (root.expr_kind_case() == Expr::kCallExpr &&
      root.call_expr().function() == fn) {
    return &root;
  }
  switch (root.expr_kind_case()) {
    case Expr::kCallExpr:
      for (const auto& a : root.call_expr().args()) {
        if (const auto* x = FindCall(a, fn); x != nullptr) return x;
      }
      if (root.call_expr().has_target()) {
        if (const auto* x = FindCall(root.call_expr().target(), fn);
            x != nullptr) {
          return x;
        }
      }
      break;
    case Expr::kListExpr:
      for (const auto& e : root.list_expr().elements()) {
        if (const auto* x = FindCall(e, fn); x != nullptr) return x;
      }
      break;
    case Expr::kStructExpr:
      for (const auto& e : root.struct_expr().entries()) {
        if (e.has_map_key()) {
          if (const auto* x = FindCall(e.map_key(), fn); x != nullptr) return x;
        }
        if (const auto* x = FindCall(e.value(), fn); x != nullptr) return x;
      }
      break;
    case Expr::kSelectExpr:
      if (const auto* x = FindCall(root.select_expr().operand(), fn);
          x != nullptr) {
        return x;
      }
      break;
    case Expr::kComprehensionExpr: {
      const auto& c = root.comprehension_expr();
      if (const auto* x = FindCall(c.iter_range(), fn); x != nullptr) return x;
      if (const auto* x = FindCall(c.accu_init(), fn); x != nullptr) return x;
      if (const auto* x = FindCall(c.loop_condition(), fn); x != nullptr) {
        return x;
      }
      if (const auto* x = FindCall(c.loop_step(), fn); x != nullptr) return x;
      if (const auto* x = FindCall(c.result(), fn); x != nullptr) return x;
      break;
    }
    default:
      break;
  }
  return nullptr;
}

// Look up the overload_id(s) in reference_map for the given expr id.
std::vector<std::string> OverloadIds(const CheckedExpr& ce, int64_t expr_id) {
  std::vector<std::string> out;
  auto it = ce.reference_map().find(expr_id);
  if (it == ce.reference_map().end()) return out;
  for (const auto& o : it->second.overload_id()) out.push_back(o);
  return out;
}

// Q1 — does `.?field` reach as Call("_?._", [obj, "field"])?
TEST(OptionalAstShape, OptionalSelectIsCallNode) {
  constexpr absl::string_view expr = R"({'k': 1}.?k)";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  const Expr* call = FindCall(ce.expr(), "_?._");
  ASSERT_NE(call, nullptr) << "expected a Call(_?._) node";

  EXPECT_EQ(call->call_expr().function(), "_?._");
  ASSERT_EQ(call->call_expr().args_size(), 2);
  // arg[0] = the map operand (Struct expr — map literal); arg[1] =
  // ident or const for the field name.
  EXPECT_EQ(call->call_expr().args(0).expr_kind_case(), Expr::kStructExpr);

  auto ids = OverloadIds(ce, call->id());
  EXPECT_THAT(ids, ::testing::ElementsAre("select_optional_field"))
      << "overload_id mismatch on `.?` call";

  DumpOnFailure(ce, expr);
}

// Q2 — does `[?key]` reach as Call("_[?_]", [obj, key])?
TEST(OptionalAstShape, OptionalIndexIsCallNode) {
  constexpr absl::string_view expr = R"({'k': 1}[?'k'])";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  const Expr* call = FindCall(ce.expr(), "_[?_]");
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->call_expr().args_size(), 2);

  auto ids = OverloadIds(ce, call->id());
  EXPECT_FALSE(ids.empty()) << "no overload_id recorded for [?]";
  // map_optindex_optional_value for map source, list_optindex_optional_int
  // for list source.
  EXPECT_EQ(ids.front(), "map_optindex_optional_value");

  DumpOnFailure(ce, expr);
}

// Q3 — does `{?key: val}` carry per-entry optional_entry: true?
TEST(OptionalAstShape, MapLiteralOptionalEntryFlag) {
  // optional.of(1) is present, so the entry materialises; the AST
  // shape is what we care about here, NOT the runtime value.
  constexpr absl::string_view expr = R"({?'k': optional.of(1)})";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  ASSERT_EQ(ce.expr().expr_kind_case(), Expr::kStructExpr);
  const auto& sx = ce.expr().struct_expr();
  ASSERT_EQ(sx.entries_size(), 1);
  EXPECT_TRUE(sx.entries(0).optional_entry())
      << "optional_entry flag missing on `{?` map entry";

  DumpOnFailure(ce, expr);
}

// Q4 — does `[?elem]` populate CreateList.optional_indices?
TEST(OptionalAstShape, ListLiteralOptionalIndices) {
  constexpr absl::string_view expr = R"([0, ?optional.of(1), 2])";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  ASSERT_EQ(ce.expr().expr_kind_case(), Expr::kListExpr);
  const auto& lx = ce.expr().list_expr();
  ASSERT_EQ(lx.elements_size(), 3);
  ASSERT_EQ(lx.optional_indices_size(), 1);
  EXPECT_EQ(lx.optional_indices(0), 1)
      << "expected the second element (index 1) to be optional";

  DumpOnFailure(ce, expr);
}

// Q5 — does optMap expand to Conditional + Comprehension (cel.bind shape)?
TEST(OptionalAstShape, OptMapExpandsToConditionalAndCelBindShape) {
  constexpr absl::string_view expr =
      R"(optional.of(1).optMap(v, v + 1))";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  // Top should be the ternary `_?_:_`.
  ASSERT_EQ(ce.expr().expr_kind_case(), Expr::kCallExpr);
  EXPECT_EQ(ce.expr().call_expr().function(), "_?_:_");
  ASSERT_EQ(ce.expr().call_expr().args_size(), 3);

  // arg[0] = hasValue() on the original target.
  EXPECT_EQ(ce.expr().call_expr().args(0).call_expr().function(), "hasValue");

  // arg[1] = optional.of(<comprehension>).
  const auto& then_arm = ce.expr().call_expr().args(1);
  EXPECT_EQ(then_arm.call_expr().function(), "optional.of");
  ASSERT_EQ(then_arm.call_expr().args_size(), 1);
  const auto& comp = then_arm.call_expr().args(0);
  ASSERT_EQ(comp.expr_kind_case(), Expr::kComprehensionExpr);

  // Shape-C cel.bind invariants.
  EXPECT_EQ(comp.comprehension_expr().iter_var(), "#unused");
  EXPECT_EQ(comp.comprehension_expr().iter_range().expr_kind_case(),
            Expr::kListExpr);
  EXPECT_EQ(comp.comprehension_expr().iter_range().list_expr().elements_size(),
            0)
      << "expected iter_range to be an empty list (Shape-C cel.bind)";
  EXPECT_EQ(comp.comprehension_expr().loop_condition().const_expr().bool_value(),
            false);
  // accu_init = target.value() — the bound payload.
  EXPECT_EQ(comp.comprehension_expr().accu_init().call_expr().function(),
            "value");
  // accu_var is the user-supplied name (v in this expression).
  EXPECT_EQ(comp.comprehension_expr().accu_var(), "v");

  // arg[2] = optional.none().
  EXPECT_EQ(ce.expr().call_expr().args(2).call_expr().function(),
            "optional.none");

  DumpOnFailure(ce, expr);
}

// Q6 — overload IDs for the standalone optional operations.
TEST(OptionalAstShape, ValueAndHasValueOverloadIds) {
  constexpr absl::string_view expr = R"(optional.of(1).hasValue())";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  const Expr* hasv = FindCall(ce.expr(), "hasValue");
  const Expr* of = FindCall(ce.expr(), "optional.of");
  ASSERT_NE(hasv, nullptr);
  ASSERT_NE(of, nullptr);

  EXPECT_THAT(OverloadIds(ce, hasv->id()),
              ::testing::ElementsAre("optional_hasValue"));
  EXPECT_THAT(OverloadIds(ce, of->id()),
              ::testing::ElementsAre("optional_of"));

  DumpOnFailure(ce, expr);
}

TEST(OptionalAstShape, OrAndOrValueOverloadIds) {
  constexpr absl::string_view expr =
      R"(optional.of(1).or(optional.none()).orValue(0))";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  const Expr* or_ = FindCall(ce.expr(), "or");
  const Expr* or_value = FindCall(ce.expr(), "orValue");
  ASSERT_NE(or_, nullptr);
  ASSERT_NE(or_value, nullptr);

  EXPECT_THAT(OverloadIds(ce, or_->id()),
              ::testing::ElementsAre("optional_or_optional"));
  EXPECT_THAT(OverloadIds(ce, or_value->id()),
              ::testing::ElementsAre("optional_orValue_value"));

  DumpOnFailure(ce, expr);
}

// Q7 — what CelType does the checker assign to `optional<int>`?
TEST(OptionalAstShape, OptionalTypeShape) {
  constexpr absl::string_view expr = R"(optional.of(1))";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  const int64_t root_id = ce.expr().id();
  auto it = ce.type_map().find(root_id);
  ASSERT_NE(it, ce.type_map().end()) << "no type for root expr";
  // optional types in the wire format use abstract_type with
  // name="optional_type" + one type parameter.
  EXPECT_EQ(it->second.type_kind_case(), cel::expr::Type::kAbstractType);
  EXPECT_EQ(it->second.abstract_type().name(), "optional_type");
  ASSERT_EQ(it->second.abstract_type().parameter_types_size(), 1);
  EXPECT_EQ(it->second.abstract_type().parameter_types(0).primitive(),
            cel::expr::Type::INT64);

  DumpOnFailure(ce, expr);
}

// Q8 — `type(optional.none()) == optional_type` — the `optional_type`
// ident is registered as a variable holding the meta-type.
TEST(OptionalAstShape, TypeOfOptionalNoneCompares) {
  constexpr absl::string_view expr =
      R"(type(optional.none()) == optional_type)";
  auto ce_or = ParseAndCheck(expr);
  // We just need this to compile.  The runtime side is out-of-scope
  // for the AST probe — but if the checker rejects, we have a
  // bigger problem.
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
}

// Q9 — `has(.?...)` — what does the AST look like for has() on an
// optional-chained select?
TEST(OptionalAstShape, HasOverOptionalChain) {
  constexpr absl::string_view expr = R"(has({'k': 1}.?k.y))";
  auto ce_or = ParseAndCheck(expr);
  // optional<int>.y wouldn't type-check normally — but the corpus
  // has `has({'x': {'y': 'z'}}.?x.y)` which DOES.  Try that shape.
  if (!ce_or.ok()) {
    GTEST_SKIP() << "expression rejected by checker: " << ce_or.status();
  }
}

TEST(OptionalAstShape, HasOverOptionalChainNested) {
  constexpr absl::string_view expr = R"(has({'x': {'y': 'z'}}.?x.y))";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  // has() expands to a test_only Select.  Want to see what the
  // operand looks like — is it Select(Select(.?x, "y")) or
  // Select(_?._, "y") or something else?
  ASSERT_EQ(ce_or->expr().expr_kind_case(), Expr::kSelectExpr);
  EXPECT_TRUE(ce_or->expr().select_expr().test_only());
  const Expr& operand = ce_or->expr().select_expr().operand();
  std::cerr << "has() operand for `.?x.y`: kind="
            << static_cast<int>(operand.expr_kind_case()) << "\n"
            << operand.DebugString();
}

// Q10 — `optional.of({'c': {}}).c.missing` — the chained access on
// an optional value (without `.?`) — does the checker accept it,
// and what's the AST?
TEST(OptionalAstShape, ChainedAccessThroughOptional) {
  constexpr absl::string_view expr =
      R"(optional.of({'c': {'index': 'goodbye'}}).c.index)";
  auto ce_or = ParseAndCheck(expr);
  // Corpus rows `optional_chaining_4` and similar use this shape.
  if (!ce_or.ok()) {
    GTEST_SKIP() << "expression rejected by checker: " << ce_or.status();
  }
  std::cerr << "chained access through optional, CheckedExpr:\n"
            << ce_or->DebugString() << "\n";
}

// Q11 — Select on optional-typed operand: stays as kSelectExpr,
// no reference_map entry for the Select itself, result type
// promoted to optional<inner_field_type>.  Disprove the
// "checker rewrites to _?._" hypothesis.
TEST(OptionalAstShape, SelectOnOptionalStaysAsSelectExpr) {
  constexpr absl::string_view expr = R"(optional.of({'c': 'v'}).c)";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  ASSERT_EQ(ce.expr().expr_kind_case(), Expr::kSelectExpr)
      << "Select on optional must stay a SelectExpr, not be rewritten "
         "to a _?._ call";
  EXPECT_EQ(ce.expr().select_expr().field(), "c");
  // The Select has no reference_map entry (it's not a function call).
  EXPECT_TRUE(OverloadIds(ce, ce.expr().id()).empty());

  // Result type = optional<string>.
  auto it = ce.type_map().find(ce.expr().id());
  ASSERT_NE(it, ce.type_map().end());
  EXPECT_EQ(it->second.type_kind_case(), cel::expr::Type::kAbstractType);
  EXPECT_EQ(it->second.abstract_type().name(), "optional_type");
  ASSERT_EQ(it->second.abstract_type().parameter_types_size(), 1);
  EXPECT_EQ(it->second.abstract_type().parameter_types(0).primitive(),
            cel::expr::Type::STRING);
}

// Q12 — `.hasValue()` / `.value()` on optional: are these
// `kCallExpr(receiver-form)` (matches the checker's MemberOverloadDecl
// shape) or `kSelectExpr`?
TEST(OptionalAstShape, HasValueIsReceiverCall) {
  constexpr absl::string_view expr = R"(optional.of(1).hasValue())";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  ASSERT_EQ(ce.expr().expr_kind_case(), Expr::kCallExpr);
  EXPECT_EQ(ce.expr().call_expr().function(), "hasValue");
  // Receiver lives in `target`, NOT args[0].  Matters for codegen
  // dispatch: kCall with non-empty target == receiver-form.
  EXPECT_TRUE(ce.expr().call_expr().has_target())
      << "hasValue is a receiver-form call (target set); not args[0]";
  EXPECT_EQ(ce.expr().call_expr().args_size(), 0);
}

// Q13 — `has()` on `.?` chain: does the checker collapse the
// outer has(Select(.?x, "y")) into a single optional-aware test?
TEST(OptionalAstShape, HasOnOptionalChainKeepsTestOnlySelect) {
  constexpr absl::string_view expr = R"(has({'x': {'y': 'z'}}.?x.y))";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  const auto& ce = *ce_or;

  // has() expands to test_only Select.  The operand of the
  // test_only Select is the inner `.y` access — but on an optional
  // operand.  Concretely: the outer test_only Select's operand is
  // a NEW Select(field="y", operand=Call("_?._", ...)).
  ASSERT_EQ(ce.expr().expr_kind_case(), Expr::kSelectExpr);
  ASSERT_TRUE(ce.expr().select_expr().test_only());
  EXPECT_EQ(ce.expr().select_expr().field(), "y");
  // Inner operand is a `_?._` Call directly?  Or a Select wrapping it?
  const Expr& inner_operand = ce.expr().select_expr().operand();
  std::cerr << "has-on-optional-chain inner operand kind="
            << static_cast<int>(inner_operand.expr_kind_case())
            << " (kSelectExpr=" << Expr::kSelectExpr << " kCallExpr="
            << Expr::kCallExpr << ")\n"
            << inner_operand.DebugString() << "\n";
}

// Q14 — A direct optional_chaining_3 corpus row:
// `{'c': {}}.c[?'missing-index'].orValue('default value')` —
// observe the AST so codegen can plan the lowering shape.
TEST(OptionalAstShape, CorpusOptionalChaining3) {
  constexpr absl::string_view expr =
      R"({'c': {}}.c[?'missing-index'].orValue('default value'))";
  auto ce_or = ParseAndCheck(expr);
  ASSERT_TRUE(ce_or.ok()) << ce_or.status();
  std::cerr << "optional_chaining_3 AST:\n" << ce_or->DebugString() << "\n";
}

// Q15 — `optional.none().repeated_string` style chained access on
// the explicit None constructor.  Type should stay optional<...>.
TEST(OptionalAstShape, ChainedAccessOnExplicitNone) {
  constexpr absl::string_view expr = R"(optional.none().a)";
  auto ce_or = ParseAndCheck(expr);
  // Note: `optional.none()` has type optional(V) where V is a free
  // type parameter, so `.a` may or may not check depending on
  // whether the checker has enough info.
  if (!ce_or.ok()) {
    GTEST_SKIP() << "checker rejected: " << ce_or.status();
  }
  std::cerr << "optional.none().a AST:\n" << ce_or->DebugString() << "\n";
}

}  // namespace
}  // namespace celwasm::probes
