#include "compiler_v2/codegen/resolve_pass.h"

#include <cstdint>

#include "absl/status/status_matchers.h"
#include "compiler_v2/frontend/parse_and_check.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Returns the `Repr` ResolvePass assigned to the root expression.
Repr RootRepr(absl::string_view expression) {
  auto ta = ParseAndCheck(expression, {});
  EXPECT_THAT(ta, IsOk()) << "ParseAndCheck failed for: " << expression;
  if (!ta.ok()) return Repr::kUnknown;
  auto resolved = ResolvePass(*ta);
  EXPECT_THAT(resolved, IsOk()) << "ResolvePass failed for: " << expression;
  if (!resolved.ok()) return Repr::kUnknown;
  const int64_t root_id = ta->ast().root_expr().id();
  const NodeAnnotation* ann = resolved->annotations.Find(root_id);
  EXPECT_NE(ann, nullptr) << "no resolved annotation for root id " << root_id;
  return ann ? ann->repr : Repr::kUnknown;
}

// --- Repr population per kConst kind -------------------------------------

TEST(ResolvePassTest, ReprBool) {
  EXPECT_EQ(RootRepr("true"), Repr::kBool);
}
TEST(ResolvePassTest, ReprInt) {
  EXPECT_EQ(RootRepr("42"), Repr::kInt);
}
TEST(ResolvePassTest, ReprUint) {
  EXPECT_EQ(RootRepr("42u"), Repr::kUint);
}
TEST(ResolvePassTest, ReprDouble) {
  EXPECT_EQ(RootRepr("3.14"), Repr::kDouble);
}
TEST(ResolvePassTest, ReprNull) {
  EXPECT_EQ(RootRepr("null"), Repr::kNull);
}
TEST(ResolvePassTest, ReprString) {
  EXPECT_EQ(RootRepr("\"hi\""), Repr::kString);
}
TEST(ResolvePassTest, ReprBytes) {
  EXPECT_EQ(RootRepr("b\"x\""), Repr::kBytes);
}

// --- Non-repr fields stay at zero sentinels for M1 -----------------------

TEST(ResolvePassTest, KConstLeavesNonReprFieldsAtZero) {
  auto ta = ParseAndCheck("42", {});
  ASSERT_THAT(ta, IsOk());
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk());
  const int64_t root_id = ta->ast().root_expr().id();
  const NodeAnnotation* ann = r->annotations.Find(root_id);
  ASSERT_NE(ann, nullptr);
  EXPECT_EQ(ann->field_number, 0u);
  EXPECT_EQ(ann->overload_id, 0u);
  EXPECT_EQ(ann->local_index, 0u);
  EXPECT_EQ(ann->scope_id, 0u);
  EXPECT_EQ(ann->storage.kind, StorageKind::kNone);
  EXPECT_EQ(ann->storage.payload, 0u);
}

// --- ResolveOutput top-level fields --------------------------------------

TEST(ResolvePassTest, EmptyLocalTypesForLiteralOnlyProgram) {
  auto ta = ParseAndCheck("1", {});
  ASSERT_THAT(ta, IsOk());
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk());
  EXPECT_TRUE(r->local_types.empty());
  EXPECT_EQ(r->max_scope_id, 0u);
}

// --- Multi-node AST: every typed node gets a repr ------------------------
// `1 + 2` type-checks: root is a kCall of `_+_` over two kConst operands.
// Even though M1's expr_lower will later reject kCall, ResolvePass doesn't
// gate on expression kind — its job is only to seed repr from type_map for
// every typed node.  This test pins that contract.
TEST(ResolvePassTest, AnnotatesEveryTypedNodeNotJustTheRoot) {
  auto ta = ParseAndCheck("1 + 2", {});
  ASSERT_THAT(ta, IsOk());
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk());
  for (const auto& [id, type] : ta->ast().type_map()) {
    const NodeAnnotation* ann = r->annotations.Find(id);
    ASSERT_NE(ann, nullptr) << "missing annotation for expr id=" << id;
    EXPECT_EQ(ann->repr, ReprOf(type)) << "repr mismatch for expr id=" << id;
  }
}

}  // namespace
}  // namespace celwasm
