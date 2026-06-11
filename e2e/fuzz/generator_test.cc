// Unit tests for `GenerateExpr` — the type-directed walker over
// the grammar.  Stronger than L3 in `grammar_test.cc` (which uses
// an inlined walker for its self-check) in that these tests
// exercise the actual `e2e/fuzz/generator.{h,cc}` API the oracle
// property invokes.

#include "e2e/fuzz/generator.h"

#include <cstdint>
#include <random>
#include <string>

#include "absl/status/status_matchers.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"
#include "e2e/fuzz/grammar.h"
#include "e2e/fuzz/grammar_scalars.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm::fuzz {
namespace {

using ::absl_testing::IsOk;

CheckOptions BuildSchemaCheckOpts() {
  CheckOptions opts;
  for (const ActivationBinding& v : ActivationSchema()) {
    opts.variable_specs.push_back(absl::StrCat(v.name, ":", TypeSpec(v.type)));
  }
  return opts;
}

TEST(GenerateExprTest, DepthZeroAlwaysProducesAValidLeafSource) {
  Grammar g = BuildScalarGrammar();
  const CheckOptions opts = BuildSchemaCheckOpts();

  for (uint64_t seed = 0; seed < 64; ++seed) {
    std::mt19937_64 rng(seed);
    GenCtx ctx = NewGenCtx(/*depth=*/0, rng);
    const std::string source = GenerateExpr(g, CelType::Bool(), ctx);

    auto ta = ParseAndCheck(source, opts);
    ASSERT_THAT(ta, IsOk())
        << "depth-0 seed=" << seed << " source=`" << source << "`";

    // Root must annotate as Bool.
    const int64_t root_id = ta->ast().root_expr().id();
    const NodeAnnotation* ann = ta->annotations().Find(root_id);
    ASSERT_NE(ann, nullptr);
    EXPECT_EQ(ann->repr, Repr::kBool) << source;
  }
}

TEST(GenerateExprTest, DeterministicForFixedSeed) {
  Grammar g = BuildScalarGrammar();
  // Constant seeds are the POINT here — the test asserts the
  // generator is a pure function of (seed, depth).
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937_64 rng_a(42);
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937_64 rng_b(42);
  GenCtx ctx_a = NewGenCtx(4, rng_a);
  GenCtx ctx_b = NewGenCtx(4, rng_b);
  EXPECT_EQ(GenerateExpr(g, CelType::Bool(), ctx_a),
            GenerateExpr(g, CelType::Bool(), ctx_b));
}

TEST(GenerateExprTest, DifferentSeedsProduceDifferentSources) {
  // Stochastic — extremely unlikely for two different 64-bit
  // seeds at depth=6 to produce identical sources, but the
  // assertion guards against accidental seed-ignoring.
  Grammar g = BuildScalarGrammar();
  // Distinct constant seeds, deliberately — see comment above.
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937_64 rng_a(1);
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937_64 rng_b(2);
  GenCtx ctx_a = NewGenCtx(6, rng_a);
  GenCtx ctx_b = NewGenCtx(6, rng_b);
  EXPECT_NE(GenerateExpr(g, CelType::Bool(), ctx_a),
            GenerateExpr(g, CelType::Bool(), ctx_b));
}

TEST(GenerateExprTest, EveryScalarTargetTypeCanBeGenerated) {
  Grammar g = BuildScalarGrammar();
  const CheckOptions opts = BuildSchemaCheckOpts();

  struct Case {
    CelType target;
    Repr expected = Repr::kUnknown;
  };
  const Case cases[] = {
      {CelType::Bool(), Repr::kBool},     {CelType::Int(), Repr::kInt},
      {CelType::Uint(), Repr::kUint},     {CelType::Double(), Repr::kDouble},
      {CelType::String(), Repr::kString}, {CelType::Bytes(), Repr::kBytes},
  };

  for (const Case& c : cases) {
    std::mt19937_64 rng(0xC0FFEEull ^ static_cast<uint64_t>(c.target.kind()));
    GenCtx ctx = NewGenCtx(3, rng);
    const std::string source = GenerateExpr(g, c.target, ctx);
    auto ta = ParseAndCheck(source, opts);
    ASSERT_THAT(ta, IsOk())
        << "target=" << TypeKey(c.target) << " source=`" << source << "`";
    const int64_t root_id = ta->ast().root_expr().id();
    const NodeAnnotation* ann = ta->annotations().Find(root_id);
    ASSERT_NE(ann, nullptr);
    EXPECT_EQ(ann->repr, c.expected)
        << "target=" << TypeKey(c.target) << " source=`" << source << "`";
  }
}

TEST(GenerateExprTest, DepthBudgetActuallyBoundsRecursion) {
  // A depth-1 expression must have at most one level of operator
  // wrapping above a leaf — so its source can't be insanely long.
  // This catches the regression where the walker forgets to
  // decrement the budget.
  Grammar g = BuildScalarGrammar();
  for (uint64_t seed = 0; seed < 16; ++seed) {
    std::mt19937_64 rng(seed);
    GenCtx ctx = NewGenCtx(/*depth=*/1, rng);
    const std::string source = GenerateExpr(g, CelType::Bool(), ctx);
    EXPECT_LT(source.size(), 200u)
        << "depth-1 source was " << source.size()
        << " chars, suggesting the depth budget isn't being "
           "honoured. source=`"
        << source << "`";
  }
}

}  // namespace
}  // namespace celwasm::fuzz
