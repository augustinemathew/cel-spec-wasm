#include "compiler/codegen/attribute_pool.h"

#include <string>

#include "absl/status/status_matchers.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/typed_ast.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

TypedAst Check(absl::string_view source, CheckOptions opts = {}) {
  auto typed = ParseAndCheck(source, opts);
  EXPECT_THAT(typed, IsOk()) << source;
  return *std::move(typed);
}

TEST(AttributePoolTest, EmptyForConstantExpression) {
  AttributePool pool = AttributePool::FromTypedAst(Check("1 + 2"));
  EXPECT_TRUE(pool.entries().empty());
}

TEST(AttributePoolTest, NestedSelectPreOrderIntern) {
  (void)celwasm::testdata::Customer::descriptor();
  CheckOptions opts;
  opts.variable_specs.emplace_back("c:celwasm.testdata.Customer");
  AttributePool pool = AttributePool::FromTypedAst(
      Check("c.billing_address.city", opts));
  // Outer select `(.city)` interned first (id=0), then inner
  // `(.billing_address)` (id=1) — matches the pre-order walk
  // `LowerSelectOperand` uses.
  ASSERT_EQ(pool.entries().size(), 2);
  EXPECT_EQ(pool.entries()[0].variable, "c");
  EXPECT_EQ(pool.entries()[0].qualifiers,
            (std::vector<std::string>{"billing_address", "city"}));
  EXPECT_EQ(pool.entries()[1].variable, "c");
  EXPECT_EQ(pool.entries()[1].qualifiers,
            (std::vector<std::string>{"billing_address"}));
}

TEST(AttributePoolTest, DedupsIdenticalPaths) {
  (void)celwasm::testdata::Customer::descriptor();
  CheckOptions opts;
  opts.variable_specs.emplace_back("c:celwasm.testdata.Customer");
  AttributePool pool = AttributePool::FromTypedAst(
      Check("c.name == c.name", opts));
  // Two occurrences of the same path merge to one intern entry.
  ASSERT_EQ(pool.entries().size(), 1);
  EXPECT_EQ(pool.entries()[0].variable, "c");
  EXPECT_EQ(pool.entries()[0].qualifiers,
            (std::vector<std::string>{"name"}));
}

TEST(AttributePoolTest, TwoDistinctPathsTwoEntries) {
  (void)celwasm::testdata::Customer::descriptor();
  CheckOptions opts;
  opts.variable_specs.emplace_back("c:celwasm.testdata.Customer");
  AttributePool pool = AttributePool::FromTypedAst(
      Check("c.name == \"x\" || c.age != 0", opts));
  ASSERT_EQ(pool.entries().size(), 2);
}

TEST(AttributePoolTest, HasExprWalksIntoSelect) {
  (void)celwasm::testdata::Customer::descriptor();
  CheckOptions opts;
  opts.variable_specs.emplace_back("c:celwasm.testdata.Customer");
  AttributePool pool = AttributePool::FromTypedAst(
      Check("has(c.billing_address)", opts));
  ASSERT_EQ(pool.entries().size(), 1);
  EXPECT_EQ(pool.entries()[0].variable, "c");
  EXPECT_EQ(pool.entries()[0].qualifiers,
            (std::vector<std::string>{"billing_address"}));
}

}  // namespace
}  // namespace celwasm
