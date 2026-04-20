#include "compiler/codegen/field_name_pool.h"

#include <string>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/typed_ast.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

TypedAst CheckOrDie(const std::string& source,
                    const CheckOptions& options = {}) {
  auto typed = ParseAndCheck(source, options);
  CHECK_OK(typed);
  return *std::move(typed);
}

CheckOptions CustomerOpts() {
  // Pin Customer's descriptor into the default pool — ParseAndCheck
  // resolves `c:celwasm.testdata.Customer` by FQN through that pool.
  // Without the reference the linker drops the generated C++ proto
  // symbols (and their static descriptor registrations) because
  // nothing in this TU mentions the type.
  (void)celwasm::testdata::Customer::descriptor();
  CheckOptions opts;
  opts.variable_specs.push_back("c:celwasm.testdata.Customer");
  return opts;
}

TEST(FieldNamePoolTest, InternReturnsSameIdForSameKey) {
  FieldNamePool pool;
  const uint32_t a = pool.Intern(7, "age");
  const uint32_t b = pool.Intern(7, "age");
  EXPECT_EQ(a, b);
  EXPECT_EQ(pool.entries().size(), 1u);
}

TEST(FieldNamePoolTest, InternDistinguishesByNumberAndName) {
  FieldNamePool pool;
  const uint32_t a = pool.Intern(1, "id");       // Customer.id
  const uint32_t b = pool.Intern(2, "id");       // Address.id (same name, diff num)
  const uint32_t c = pool.Intern(1, "user_id");  // diff name, same num
  EXPECT_NE(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(b, c);
  EXPECT_EQ(pool.entries().size(), 3u);
}

TEST(FieldNamePoolTest, InternAssignsDenseIdsFromZero) {
  FieldNamePool pool;
  EXPECT_EQ(pool.Intern(5, "foo"), 0u);
  EXPECT_EQ(pool.Intern(6, "bar"), 1u);
  EXPECT_EQ(pool.Intern(7, "baz"), 2u);
}

TEST(FieldNamePoolTest, FromTypedAstEmptyForExpressionWithoutSelects) {
  TypedAst typed = CheckOrDie("1 + 2");
  FieldNamePool pool = FieldNamePool::FromTypedAst(typed);
  EXPECT_TRUE(pool.entries().empty());
}

TEST(FieldNamePoolTest, FromTypedAstInternsFlatSelect) {
  TypedAst typed = CheckOrDie("c.age + 1", CustomerOpts());
  FieldNamePool pool = FieldNamePool::FromTypedAst(typed);
  ASSERT_EQ(pool.entries().size(), 1u);
  EXPECT_EQ(pool.entries()[0].name, "age");
  EXPECT_GT(pool.entries()[0].field_number, 0u);
}

TEST(FieldNamePoolTest, FromTypedAstInternsNestedSelectInPreOrder) {
  TypedAst typed =
      CheckOrDie("c.billing_address.city", CustomerOpts());
  FieldNamePool pool = FieldNamePool::FromTypedAst(typed);
  ASSERT_EQ(pool.entries().size(), 2u);
  // Outer select (.city) visited before its operand (.billing_address):
  // pre-order walk.
  EXPECT_EQ(pool.entries()[0].name, "city");
  EXPECT_EQ(pool.entries()[1].name, "billing_address");
}

TEST(FieldNamePoolTest, FromTypedAstDedupsRepeatedField) {
  TypedAst typed = CheckOrDie("c.age + c.age", CustomerOpts());
  FieldNamePool pool = FieldNamePool::FromTypedAst(typed);
  EXPECT_EQ(pool.entries().size(), 1u);
}

}  // namespace
}  // namespace celwasm
