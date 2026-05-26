#include "eval/attribute.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// ————————— AttributeQualifier —————————

// Only string-keyed qualifiers are constructible — int / uint / bool
// factories were removed because the resolver never interns them
// (index / key access breaks the attribute chain).
TEST(AttributeQualifierTest, KindDispatch) {
  EXPECT_EQ(AttributeQualifier::OfString("k").kind(),
            AttributeQualifier::Kind::kString);
}

TEST(AttributeQualifierTest, TypedAccessorsReturnValueOrNullopt) {
  auto s = AttributeQualifier::OfString("hello");
  EXPECT_EQ(s.AsString(), "hello");
  EXPECT_EQ(s.AsInt(), std::nullopt);
  EXPECT_EQ(s.AsUint(), std::nullopt);
  EXPECT_EQ(s.AsBool(), std::nullopt);
}

TEST(AttributeQualifierTest, MatchesStringKey) {
  EXPECT_TRUE(AttributeQualifier::OfString("x").MatchesStringKey("x"));
  EXPECT_FALSE(AttributeQualifier::OfString("x").MatchesStringKey("y"));
}

TEST(AttributeQualifierTest, EqualityByValue) {
  EXPECT_EQ(AttributeQualifier::OfString("a"),
            AttributeQualifier::OfString("a"));
  EXPECT_NE(AttributeQualifier::OfString("a"),
            AttributeQualifier::OfString("b"));
}

TEST(AttributeQualifierTest, OrderingByValue) {
  EXPECT_LT(AttributeQualifier::OfString("a"),
            AttributeQualifier::OfString("b"));
}

TEST(AttributeQualifierTest, CanonicalStringFormats) {
  EXPECT_THAT(AttributeQualifier::OfString("k").AsCanonicalString(),
              IsOkAndHolds(R"(["k"])"));
}

TEST(AttributeQualifierTest, CanonicalStringRejectsUnprintable) {
  EXPECT_THAT(
      AttributeQualifier::OfString(std::string("\x01", 1)).AsCanonicalString(),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("unprintable")));
  EXPECT_THAT(AttributeQualifier::OfString("has\"quote").AsCanonicalString(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("unprintable or quote")));
}

// ————————— AttributeQualifierPattern —————————

TEST(AttributeQualifierPatternTest, WildcardMatchesAnything) {
  auto p = AttributeQualifierPattern::Wildcard();
  EXPECT_TRUE(p.IsWildcard());
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

TEST(AttributeQualifierPatternTest, ConcreteStringMatchIsExact) {
  EXPECT_TRUE(AttributeQualifierPattern::OfString("a").IsMatch(
      AttributeQualifier::OfString("a")));
  EXPECT_FALSE(AttributeQualifierPattern::OfString("a").IsMatch(
      AttributeQualifier::OfString("b")));
}

// ————————— Attribute —————————

TEST(AttributeTest, BareVariable) {
  Attribute a("x");
  EXPECT_EQ(a.variable_name(), "x");
  EXPECT_TRUE(a.has_variable_name());
  EXPECT_TRUE(a.qualifier_path().empty());
}

TEST(AttributeTest, WithQualifiers) {
  Attribute a("request", {AttributeQualifier::OfString("auth"),
                          AttributeQualifier::OfString("claims")});
  EXPECT_EQ(a.variable_name(), "request");
  ASSERT_EQ(a.qualifier_path().size(), 2u);
  EXPECT_EQ(a.qualifier_path()[0].AsString(), "auth");
  EXPECT_EQ(a.qualifier_path()[1].AsString(), "claims");
}

TEST(AttributeTest, EqualityComparesVariableAndPath) {
  Attribute a("x", {AttributeQualifier::OfString("f")});
  Attribute b("x", {AttributeQualifier::OfString("f")});
  Attribute c("x", {AttributeQualifier::OfString("g")});
  Attribute d("y", {AttributeQualifier::OfString("f")});
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(a, d);
}

TEST(AttributeTest, OrderByVariableThenPath) {
  Attribute a("a");
  Attribute b("b");
  Attribute a_ext("a", {AttributeQualifier::OfString("f")});
  EXPECT_LT(a, b);
  EXPECT_LT(a, a_ext);  // shorter path < longer path with same prefix
}

TEST(AttributeTest, CanonicalStringDottedForStringQualifiers) {
  Attribute a("request", {AttributeQualifier::OfString("auth"),
                          AttributeQualifier::OfString("claims")});
  EXPECT_THAT(a.AsString(), IsOkAndHolds("request.auth.claims"));
}

// ————————— AttributePattern (IsMatch) —————————

TEST(AttributePatternTest, VariableMismatchReturnsNone) {
  AttributePattern p("x", {AttributeQualifierPattern::Wildcard()});
  Attribute a("y", {AttributeQualifier::OfString("f")});
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kNone);
}

TEST(AttributePatternTest, FullMatchExactPath) {
  AttributePattern p("request",
                     {AttributeQualifierPattern::OfString("auth"),
                      AttributeQualifierPattern::OfString("claims")});
  Attribute a("request", {AttributeQualifier::OfString("auth"),
                          AttributeQualifier::OfString("claims")});
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kFull);
}

TEST(AttributePatternTest, FullMatchWhenAttributeExtendsPattern) {
  // Pattern is a prefix of attribute — FULL: the attribute itself
  // (and every child) matches.
  AttributePattern p("request", {AttributeQualifierPattern::OfString("auth")});
  Attribute a("request", {AttributeQualifier::OfString("auth"),
                          AttributeQualifier::OfString("claims")});
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kFull);
}

TEST(AttributePatternTest, PartialMatchWhenPatternExtendsAttribute) {
  // Pattern is longer than attribute — PARTIAL: the pattern names
  // something nested inside the attribute.
  AttributePattern p("request", {AttributeQualifierPattern::OfString("auth"),
                                 AttributeQualifierPattern::OfString("claims"),
                                 AttributeQualifierPattern::OfString("iss")});
  Attribute a("request", {AttributeQualifier::OfString("auth")});
  EXPECT_EQ(p.IsMatch(a), AttributePattern::MatchType::kPartial);
}

TEST(AttributePatternTest, WildcardsMatchAnyQualifier) {
  AttributePattern p("request", {AttributeQualifierPattern::Wildcard(),
                                 AttributeQualifierPattern::OfString("iss")});
  Attribute a1("request", {AttributeQualifier::OfString("auth"),
                           AttributeQualifier::OfString("iss")});
  Attribute a2("request", {AttributeQualifier::OfString("headers"),
                           AttributeQualifier::OfString("iss")});
  EXPECT_EQ(p.IsMatch(a1), AttributePattern::MatchType::kFull);
  EXPECT_EQ(p.IsMatch(a2), AttributePattern::MatchType::kFull);
}

TEST(AttributePatternTest, ConcreteMismatchDominatesWildcard) {
  AttributePattern p("request", {AttributeQualifierPattern::OfString("auth"),
                                 AttributeQualifierPattern::Wildcard()});
  Attribute a("request", {AttributeQualifier::OfString("other"),
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

// ————————— AttributePattern::Parse —————————

TEST(AttributePatternParseTest, SingleSegmentVariable) {
  auto p = AttributePattern::Parse("x");
  ASSERT_THAT(p, absl_testing::IsOk());
  EXPECT_EQ(p->variable(), "x");
  EXPECT_TRUE(p->qualifier_path().empty());
}

TEST(AttributePatternParseTest, DottedPathStringSegments) {
  auto p = AttributePattern::Parse("c.billing_address.city");
  ASSERT_THAT(p, absl_testing::IsOk());
  EXPECT_EQ(p->variable(), "c");
  ASSERT_EQ(p->qualifier_path().size(), 2u);
  EXPECT_TRUE(p->qualifier_path()[0].IsMatch("billing_address"));
  EXPECT_FALSE(p->qualifier_path()[0].IsMatch("other"));
  EXPECT_TRUE(p->qualifier_path()[1].IsMatch("city"));
}

TEST(AttributePatternParseTest, WildcardMidPath) {
  auto p = AttributePattern::Parse("c.*.city");
  ASSERT_THAT(p, absl_testing::IsOk());
  ASSERT_EQ(p->qualifier_path().size(), 2u);
  EXPECT_TRUE(p->qualifier_path()[0].IsWildcard());
  EXPECT_FALSE(p->qualifier_path()[1].IsWildcard());
}

TEST(AttributePatternParseTest, WildcardTrailing) {
  auto p = AttributePattern::Parse("c.billing_address.*");
  ASSERT_THAT(p, absl_testing::IsOk());
  ASSERT_EQ(p->qualifier_path().size(), 2u);
  EXPECT_FALSE(p->qualifier_path()[0].IsWildcard());
  EXPECT_TRUE(p->qualifier_path()[1].IsWildcard());
}

TEST(AttributePatternParseTest, WildcardAtRootMatchesAnyVariable) {
  // "*" as the whole pattern is treated as a bare variable named `*`
  // rather than a wildcard root — roots are named variables.  Locks
  // the intent so a future reader doesn't over-generalise the syntax.
  auto p = AttributePattern::Parse("*");
  ASSERT_THAT(p, absl_testing::IsOk());
  EXPECT_EQ(p->variable(), "*");
}

TEST(AttributePatternParseTest, EmptyInputIsInvalid) {
  EXPECT_THAT(AttributePattern::Parse(""),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, LeadingDotIsInvalid) {
  EXPECT_THAT(AttributePattern::Parse(".x"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, TrailingDotIsInvalid) {
  EXPECT_THAT(AttributePattern::Parse("x."),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, ConsecutiveDotsIsInvalid) {
  EXPECT_THAT(AttributePattern::Parse("x..y"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

// Round-trip: parse → IsMatch behaves the same as the manual
// constructor-built pattern.
TEST(AttributePatternParseTest, ParseRoundTripsThroughIsMatch) {
  auto parsed = AttributePattern::Parse("c.billing_address.city");
  ASSERT_THAT(parsed, absl_testing::IsOk());
  Attribute a("c", {AttributeQualifier::OfString("billing_address"),
                    AttributeQualifier::OfString("city")});
  EXPECT_EQ(parsed->IsMatch(a), AttributePattern::MatchType::kFull);
}

// ——— Bracket / index / key qualifiers are rejected by design ———
//
// The resolver only interns string `.field` qualifiers: index / key
// access (`x[i]`, `m['k']`) breaks the attribute chain (resolve_pass
// PostVisitSelect bails when its operand is an index call), so no
// attribute ever carries an index/key qualifier.  A pattern naming
// one could never match anything, so Parse rejects the whole bracket
// surface rather than accept a pattern it cannot honor.

TEST(AttributePatternParseTest, BracketedIntRejected) {
  EXPECT_THAT(AttributePattern::Parse("xs[3]"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(AttributePattern::Parse("xs[-1]"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, BracketedUintRejected) {
  EXPECT_THAT(AttributePattern::Parse("xs[3u]"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, BracketedBoolRejected) {
  EXPECT_THAT(AttributePattern::Parse("m[true]"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, BracketedStringKeyRejected) {
  EXPECT_THAT(AttributePattern::Parse("m[\"k\"]"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, BracketedWildcardRejected) {
  EXPECT_THAT(AttributePattern::Parse("xs[*]"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, MixedDottedAndBracketedRejected) {
  EXPECT_THAT(AttributePattern::Parse("request.messages[3].text"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternParseTest, BracketedAtRootRejected) {
  EXPECT_THAT(AttributePattern::Parse("[3]"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

// Wildcard parsed-pattern matches the same set as a
// hand-constructed wildcard-pattern.
TEST(AttributePatternParseTest, WildcardParsedMatchesAnyMidQualifier) {
  auto parsed = AttributePattern::Parse("c.*.city");
  ASSERT_THAT(parsed, absl_testing::IsOk());
  Attribute billing("c", {AttributeQualifier::OfString("billing_address"),
                          AttributeQualifier::OfString("city")});
  Attribute shipping("c", {AttributeQualifier::OfString("shipping_address"),
                           AttributeQualifier::OfString("city")});
  EXPECT_EQ(parsed->IsMatch(billing), AttributePattern::MatchType::kFull);
  EXPECT_EQ(parsed->IsMatch(shipping), AttributePattern::MatchType::kFull);
}

// ————————— AttributeId —————————

TEST(AttributeIdTest, EqualityByNumericId) {
  EXPECT_EQ(AttributeId{7}, AttributeId{7});
  EXPECT_NE(AttributeId{7}, AttributeId{8});
  EXPECT_EQ(AttributeId{}, AttributeId{0});
}

// Error tests live in error_test.cc — ErrorCode/ErrorPayload moved
// into eval/error.h to keep attribute/ compilation lean.

}  // namespace
}  // namespace celwasm
