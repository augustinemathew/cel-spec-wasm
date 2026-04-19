#include "compiler/frontend/parse_and_check.h"

#include <string>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;

// Look up the annotation for whichever node the checker assigned to the
// *root* expression. Tests assert against this without needing to know the
// internal id the parser hands out.
Repr RootRepr(const TypedAst& ta) {
  int64_t root_id = ta.ast().root_expr().id();
  const NodeAnnotation* ann = ta.annotations().Find(root_id);
  return ann ? ann->repr : Repr::kUnknown;
}

// ---- Positive expression shapes --------------------------------------------

TEST(ParseAndCheckTest, IntegerConstant) {
  auto r = ParseAndCheck("1", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kInt);
}

TEST(ParseAndCheckTest, BoolConstant) {
  auto r = ParseAndCheck("true", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kBool);
}

TEST(ParseAndCheckTest, StringConstant) {
  auto r = ParseAndCheck("\"abc\"", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kString);
}

TEST(ParseAndCheckTest, DoubleConstant) {
  auto r = ParseAndCheck("1.5", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kDouble);
}

TEST(ParseAndCheckTest, NullTypeConstant) {
  auto r = ParseAndCheck("null", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kNull);
}

TEST(ParseAndCheckTest, BytesConstant) {
  auto r = ParseAndCheck("b\"ok\"", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kBytes);
}

TEST(ParseAndCheckTest, UintConstant) {
  auto r = ParseAndCheck("1u", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kUint);
}

TEST(ParseAndCheckTest, ArithmeticProducesInt) {
  auto r = ParseAndCheck("1 + 2", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kInt);
}

TEST(ParseAndCheckTest, ComparisonProducesBool) {
  auto r = ParseAndCheck("1 < 2", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kBool);
}

// ---- Variable specs: every primitive ---------------------------------------

TEST(ParseAndCheckTest, PrimitiveVariableSpecs) {
  struct Row { const char* spec; const char* expr; Repr expected; };
  const Row rows[] = {
      {"b:bool",      "b",   Repr::kBool},
      {"i:int",       "i",   Repr::kInt},
      {"u:uint",      "u",   Repr::kUint},
      {"d:double",    "d",   Repr::kDouble},
      {"s:string",    "s",   Repr::kString},
      {"by:bytes",    "by",  Repr::kBytes},
      {"n:null_type", "n",   Repr::kNull},
  };
  for (const auto& row : rows) {
    CheckOptions opts;
    opts.variable_specs = {row.spec};
    auto r = ParseAndCheck(row.expr, opts);
    ASSERT_THAT(r, IsOk()) << "spec=" << row.spec;
    EXPECT_EQ(RootRepr(*r), row.expected) << "spec=" << row.spec;
  }
}

// ---- Well-known types ------------------------------------------------------

TEST(ParseAndCheckTest, TimestampVariable) {
  CheckOptions opts;
  opts.variable_specs = {"t:timestamp"};
  auto r = ParseAndCheck("t", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kTimestamp);
}

TEST(ParseAndCheckTest, DurationVariable) {
  CheckOptions opts;
  opts.variable_specs = {"d:duration"};
  auto r = ParseAndCheck("d", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kDuration);
}

TEST(ParseAndCheckTest, AnyVariable) {
  CheckOptions opts;
  opts.variable_specs = {"a:any"};
  auto r = ParseAndCheck("a", opts);
  ASSERT_THAT(r, IsOk());
  // `any` surfaces as a host-owned message at the ABI boundary.
  EXPECT_EQ(RootRepr(*r), Repr::kMessage);
}

// ---- Parameterized types ---------------------------------------------------

TEST(ParseAndCheckTest, ListVariableRoundTrip) {
  CheckOptions opts;
  opts.variable_specs = {"xs:list<int>"};
  auto r = ParseAndCheck("xs", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kList);
}

TEST(ParseAndCheckTest, ListSizeProducesInt) {
  CheckOptions opts;
  opts.variable_specs = {"xs:list<int>"};
  auto r = ParseAndCheck("size(xs)", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kInt);
}

TEST(ParseAndCheckTest, NestedListType) {
  CheckOptions opts;
  opts.variable_specs = {"xs:list<list<int>>"};
  auto r = ParseAndCheck("xs", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kList);
}

TEST(ParseAndCheckTest, MapVariableRoundTrip) {
  CheckOptions opts;
  opts.variable_specs = {"m:map<string,int>"};
  auto r = ParseAndCheck("m", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kMap);
}

TEST(ParseAndCheckTest, MapIndexProducesValueType) {
  CheckOptions opts;
  opts.variable_specs = {"m:map<string,int>"};
  auto r = ParseAndCheck("m[\"k\"]", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kInt);
}

TEST(ParseAndCheckTest, TypeSpecToleratesWhitespace) {
  // The spec parser should accept `map<K, V>` with spaces around the comma
  // and angle brackets without tripping the "trailing garbage" branch.
  CheckOptions opts;
  opts.variable_specs = {"m: map< string , int > "};
  auto r = ParseAndCheck("m", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kMap);
}

// ---- Message type from the generated pool ----------------------------------

TEST(ParseAndCheckTest, MessageVariableUsesGeneratedDescriptorPool) {
  // `google.protobuf.Empty` is always present in the process-wide generated
  // pool, so no schema file is needed.
  CheckOptions opts;
  opts.variable_specs = {"e:google.protobuf.Empty"};
  auto r = ParseAndCheck("e", opts);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(RootRepr(*r), Repr::kMessage);
}

// ---- Annotations contract --------------------------------------------------

TEST(ParseAndCheckTest, AnnotationsAreSeededForEveryTypedNode) {
  CheckOptions opts;
  opts.variable_specs = {"a:int", "b:int"};
  auto r = ParseAndCheck("a + b", opts);
  ASSERT_THAT(r, IsOk());
  // There must be at least three typed nodes: two operand idents and one
  // call expression for `_+_`.
  EXPECT_GE(r->annotations().nodes().size(), 3u);
  for (const auto& [id, ann] : r->annotations().nodes()) {
    EXPECT_NE(ann.repr, Repr::kUnknown)
        << "expr id " << id << " ended up kUnknown in the annotation map";
  }
}

// ---- Negative: variable-spec parser errors ---------------------------------

TEST(ParseAndCheckTest, RejectsSpecWithoutColon) {
  CheckOptions opts;
  opts.variable_specs = {"bogus"};
  EXPECT_THAT(ParseAndCheck("1", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsSpecWithEmptyName) {
  CheckOptions opts;
  opts.variable_specs = {":int"};
  EXPECT_THAT(ParseAndCheck("1", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsUnknownTypeName) {
  CheckOptions opts;
  opts.variable_specs = {"x:NotAType"};
  auto s = ParseAndCheck("1", opts);
  ASSERT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.status().message().find("unknown type"),
            absl::string_view::npos);
}

TEST(ParseAndCheckTest, RejectsUnbalancedList) {
  CheckOptions opts;
  opts.variable_specs = {"xs:list<int"};
  EXPECT_THAT(ParseAndCheck("1", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsUnbalancedMap) {
  CheckOptions opts;
  opts.variable_specs = {"m:map<string,int"};
  EXPECT_THAT(ParseAndCheck("1", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsTrailingGarbageInSpec) {
  CheckOptions opts;
  opts.variable_specs = {"x:int garbage"};
  auto s = ParseAndCheck("1", opts);
  ASSERT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_NE(s.status().message().find("trailing garbage"),
            absl::string_view::npos);
}

TEST(ParseAndCheckTest, RejectsSchemaNotFound) {
  CheckOptions opts;
  opts.schema_path = "/does/not/exist.fds";
  EXPECT_THAT(ParseAndCheck("1", opts),
              StatusIs(absl::StatusCode::kNotFound));
}

// ---- Negative: expression-level failures -----------------------------------

TEST(ParseAndCheckTest, RejectsSyntaxError) {
  // `parser::Parse` returns InvalidArgument on a malformed source.
  EXPECT_THAT(ParseAndCheck("1 +", {}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsUndeclaredVariable) {
  // No variable_specs and an ident → the checker flags it.
  EXPECT_THAT(ParseAndCheck("undeclared", {}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ParseAndCheckTest, RejectsTypeMismatch) {
  CheckOptions opts;
  opts.variable_specs = {"s:string", "i:int"};
  // The checker must reject string + int.
  EXPECT_THAT(ParseAndCheck("s + i", opts),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace
}  // namespace celwasm
