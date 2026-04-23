#include "compiler_v2/api/attribute.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// ————————— AttributeQualifier —————————

TEST(AttributeQualifierTest, KindDispatch) {
  EXPECT_EQ(AttributeQualifier::OfInt(3).kind(),
            AttributeQualifier::Kind::kInt);
  EXPECT_EQ(AttributeQualifier::OfUint(3u).kind(),
            AttributeQualifier::Kind::kUint);
  EXPECT_EQ(AttributeQualifier::OfString("k").kind(),
            AttributeQualifier::Kind::kString);
  EXPECT_EQ(AttributeQualifier::OfBool(true).kind(),
            AttributeQualifier::Kind::kBool);
}

TEST(AttributeQualifierTest, TypedAccessorsReturnValueOrNullopt) {
  auto i = AttributeQualifier::OfInt(42);
  EXPECT_EQ(i.AsInt(), 42);
  EXPECT_EQ(i.AsUint(), std::nullopt);
  EXPECT_EQ(i.AsString(), std::nullopt);
  EXPECT_EQ(i.AsBool(), std::nullopt);

  auto s = AttributeQualifier::OfString("hello");
  EXPECT_EQ(s.AsString(), "hello");
  EXPECT_EQ(s.AsInt(), std::nullopt);
}

TEST(AttributeQualifierTest, MatchesStringKey) {
  EXPECT_TRUE(AttributeQualifier::OfString("x").MatchesStringKey("x"));
  EXPECT_FALSE(AttributeQualifier::OfString("x").MatchesStringKey("y"));
  // Non-string qualifier never matches a string key.
  EXPECT_FALSE(AttributeQualifier::OfInt(42).MatchesStringKey("42"));
}

TEST(AttributeQualifierTest, EqualityByValue) {
  EXPECT_EQ(AttributeQualifier::OfInt(3), AttributeQualifier::OfInt(3));
  EXPECT_NE(AttributeQualifier::OfInt(3), AttributeQualifier::OfInt(4));
  EXPECT_NE(AttributeQualifier::OfInt(3), AttributeQualifier::OfUint(3u));
}

TEST(AttributeQualifierTest, OrderingByKindThenValue) {
  EXPECT_LT(AttributeQualifier::OfInt(1), AttributeQualifier::OfInt(2));
  // Cross-kind ordering follows Kind enum order.
  EXPECT_LT(AttributeQualifier::OfInt(9), AttributeQualifier::OfUint(0u));
}

TEST(AttributeQualifierTest, CanonicalStringFormats) {
  EXPECT_THAT(AttributeQualifier::OfInt(3).AsCanonicalString(),
              IsOkAndHolds("[3]"));
  EXPECT_THAT(AttributeQualifier::OfUint(3u).AsCanonicalString(),
              IsOkAndHolds("[3u]"));
  EXPECT_THAT(AttributeQualifier::OfString("k").AsCanonicalString(),
              IsOkAndHolds(R"(["k"])"));
  EXPECT_THAT(AttributeQualifier::OfBool(true).AsCanonicalString(),
              IsOkAndHolds("[true]"));
  EXPECT_THAT(AttributeQualifier::OfBool(false).AsCanonicalString(),
              IsOkAndHolds("[false]"));
}

TEST(AttributeQualifierTest, CanonicalStringRejectsUnprintable) {
  EXPECT_THAT(
      AttributeQualifier::OfString(std::string("\x01", 1)).AsCanonicalString(),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("unprintable")));
  EXPECT_THAT(AttributeQualifier::OfString("has\"quote").AsCanonicalString(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("unprintable or quote")));
}

// ————————— AttributeQualifierPattern —————————

TEST(AttributeQualifierPatternTest, WildcardMatchesAnything) {
  auto p = AttributeQualifierPattern::Wildcard();
  EXPECT_TRUE(p.IsWildcard());
  EXPECT_TRUE(p.IsMatch(AttributeQualifier::OfInt(1)));
  EXPECT_TRUE(p.IsMatch(AttributeQualifier::OfString("anything")));
  EXPECT_TRUE(p.IsMatch("anykey"));
}

TEST(AttributeQualifierPatternTest, ConcretePatternMatchesExact) {
  auto p = AttributeQualifierPattern::OfString("email");
  EXPECT_FALSE(p.IsWildcard());
  EXPECT_TRUE(p.IsMatch(AttributeQualifier::OfString("email")));
  EXPECT_FALSE(p.IsMatch(AttributeQualifier::OfString("name")));
  EXPECT_TRUE(p.IsMatch("email"));
  EXPECT_FALSE(p.IsMatch("name"));
}

TEST(AttributeQualifierPatternTest, CrossKindDoesNotMatch) {
  EXPECT_FALSE(AttributeQualifierPattern::OfInt(3).IsMatch(
      AttributeQualifier::OfString("3")));
  EXPECT_FALSE(AttributeQualifierPattern::OfString("3").IsMatch(
      AttributeQualifier::OfInt(3)));
}

// ————————— Attribute —————————

TEST(AttributeTest, BareVariable) {
  Attribute a("x");
  EXPECT_EQ(a.variable_name(), "x");
  EXPECT_TRUE(a.has_variable_name());
  EXPECT_TRUE(a.qualifier_path().empty());
}

TEST(AttributeTest, WithQualifiers) {
  Attribute a("request",
              {AttributeQualifier::OfString("auth"),
               AttributeQualifier::OfString("claims")});
  EXPECT_EQ(a.variable_name(), "request");
  ASSERT_EQ(a.qualifier_path().size(), 2u);
  EXPECT_EQ(a.qualifier_path()[0].AsString(), "auth");
  EXPECT_EQ(a.qualifier_path()[1].AsString(), "claims");
}

TEST(AttributeTest, EqualityComparesVariableAndPath) {
  Attribute a("x", {AttributeQualifier::OfInt(3)});
  Attribute b("x", {AttributeQualifier::OfInt(3)});
  Attribute c("x", {AttributeQualifier::OfInt(4)});
  Attribute d("y", {AttributeQualifier::OfInt(3)});
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(a, d);
}

TEST(AttributeTest, OrderByVariableThenPath) {
  Attribute a("a");
  Attribute b("b");
  Attribute a_ext("a", {AttributeQualifier::OfInt(1)});
  EXPECT_LT(a, b);
  EXPECT_LT(a, a_ext);  // shorter path < longer path with same prefix
}

TEST(AttributeTest, CanonicalStringDottedForStringQualifiers) {
  Attribute a("request",
              {AttributeQualifier::OfString("auth"),
               AttributeQualifier::OfString("claims")});
  EXPECT_THAT(a.AsString(), IsOkAndHolds("request.auth.claims"));
}

TEST(AttributeTest, CanonicalStringBracketedForIndexQualifiers) {
  Attribute a("xs",
              {AttributeQualifier::OfInt(0),
               AttributeQualifier::OfString("name")});
  EXPECT_THAT(a.AsString(), IsOkAndHolds("xs[0].name"));
}

// ————————— AttributePattern (IsMatch) —————————

TEST(AttributePatternTest, VariableMismatchReturnsNone) {
  AttributePattern p("x", {AttributeQualifierPattern::Wildcard()});
  Attribute a("y", {AttributeQualifier::OfInt(1)});
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kNone);
}

TEST(AttributePatternTest, FullMatchExactPath) {
  AttributePattern p("request",
                     {AttributeQualifierPattern::OfString("auth"),
                      AttributeQualifierPattern::OfString("claims")});
  Attribute a("request",
              {AttributeQualifier::OfString("auth"),
               AttributeQualifier::OfString("claims")});
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kFull);
}

TEST(AttributePatternTest, FullMatchWhenAttributeExtendsPattern) {
  // Pattern is a prefix of attribute — FULL: the attribute itself
  // (and every child) matches.
  AttributePattern p("request",
                     {AttributeQualifierPattern::OfString("auth")});
  Attribute a("request",
              {AttributeQualifier::OfString("auth"),
               AttributeQualifier::OfString("claims")});
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kFull);
}

TEST(AttributePatternTest, PartialMatchWhenPatternExtendsAttribute) {
  // Pattern is longer than attribute — PARTIAL: the pattern names
  // something nested inside the attribute.
  AttributePattern p("request",
                     {AttributeQualifierPattern::OfString("auth"),
                      AttributeQualifierPattern::OfString("claims"),
                      AttributeQualifierPattern::OfString("iss")});
  Attribute a("request", {AttributeQualifier::OfString("auth")});
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kPartial);
}

TEST(AttributePatternTest, WildcardsMatchAnyQualifier) {
  AttributePattern p(
      "request",
      {AttributeQualifierPattern::Wildcard(),
       AttributeQualifierPattern::OfString("iss")});
  Attribute a1("request",
               {AttributeQualifier::OfString("auth"),
                AttributeQualifier::OfString("iss")});
  Attribute a2("request",
               {AttributeQualifier::OfString("headers"),
                AttributeQualifier::OfString("iss")});
  EXPECT_EQ(p.IsMatch(a1), AttributePattern::MatchType::kFull);
  EXPECT_EQ(p.IsMatch(a2), AttributePattern::MatchType::kFull);
}

TEST(AttributePatternTest, ConcreteMismatchDominatesWildcard) {
  AttributePattern p(
      "request",
      {AttributeQualifierPattern::OfString("auth"),
       AttributeQualifierPattern::Wildcard()});
  Attribute a("request",
              {AttributeQualifier::OfString("other"),
               AttributeQualifier::OfString("anything")});
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kNone);
}

TEST(AttributePatternTest, BareVariablePatternFullMatchesBareAttribute) {
  AttributePattern p("x", {});
  Attribute a("x");
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kFull);
}

TEST(AttributePatternTest, BareVariablePatternFullMatchesExtendedAttribute) {
  // Pattern = `x` (prefix); attribute = `x.foo`.  Every prefix
  // position vacuously matches — pattern is a prefix of attribute.
  AttributePattern p("x", {});
  Attribute a("x", {AttributeQualifier::OfString("foo")});
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kFull);
}

TEST(AttributePatternTest, MatchTypeNames) {
  EXPECT_EQ(AttributePatternMatchTypeName(AttributePattern::MatchType::kNone),
            "none");
  EXPECT_EQ(
      AttributePatternMatchTypeName(AttributePattern::MatchType::kPartial),
      "partial");
  EXPECT_EQ(AttributePatternMatchTypeName(AttributePattern::MatchType::kFull),
            "full");
}

// ————————— AttributeId —————————

TEST(AttributeIdTest, EqualityByNumericId) {
  EXPECT_EQ(AttributeId{7}, AttributeId{7});
  EXPECT_NE(AttributeId{7}, AttributeId{8});
  EXPECT_EQ(AttributeId{}, AttributeId{0});
}

// Error tests live in error_test.cc — ErrorCode/ErrorPayload moved
// into compiler_v2/api/error.h to keep attribute/ compilation lean.

}  // namespace
}  // namespace cel
