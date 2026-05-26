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

// Round-trip: parse → IsMatch behaves the same as the manual
// constructor-built pattern.
TEST(AttributePatternParseTest, ParseRoundTripsThroughIsMatch) {
  auto parsed = AttributePattern::Parse("c.billing_address.city");
  ASSERT_THAT(parsed, absl_testing::IsOk());
  Attribute a("c", {AttributeQualifier::OfString("billing_address"),
                    AttributeQualifier::OfString("city")});
  EXPECT_EQ(parsed->IsMatch(a), AttributePattern::MatchType::kFull);
}

// Positive edge cases the grammar admits.
TEST(AttributePatternParseTest, MultipleWildcardQualifiers) {
  auto p = AttributePattern::Parse("a.*.*");
  ASSERT_THAT(p, absl_testing::IsOk());
  EXPECT_EQ(p->variable(), "a");
  Attribute attr("a", {AttributeQualifier::OfString("p"),
                       AttributeQualifier::OfString("q")});
  EXPECT_EQ(p->IsMatch(attr), AttributePattern::MatchType::kFull)
      << "both qualifiers are wildcards and match any pair";
}

TEST(AttributePatternParseTest, IdentifierCharsetDigitsAndUnderscore) {
  // Non-leading digits and underscores are valid identifier chars.
  EXPECT_THAT(AttributePattern::Parse("_x._y0.z_9"), absl_testing::IsOk());
  EXPECT_THAT(AttributePattern::Parse("single_int32_wrapper"),
              absl_testing::IsOk());
}

// `*` is only a QUALIFIER wildcard — never a root, never glued to other
// characters.  (Previously `*` was accepted as a literal variable named
// `*`; the grammar now rejects it — the root must be a real ident.)
TEST(AttributePatternParseTest, WildcardAtRootRejected) {
  EXPECT_THAT(AttributePattern::Parse("*"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(AttributePattern::Parse("*.city"),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

// ── The negative matrix.  Every "almost correct" input must reject
//    with InvalidArgument.  A silent accept of a segment the resolver
//    can never match (index/key access breaks the attribute chain;
//    whitespace / punctuation never appear in a field name) is the
//    failure mode the grammar guards — fail loudly, don't match
//    nothing.  See AttributePattern::Parse in attribute.cc. ──
struct BadPattern {
  absl::string_view input;
  absl::string_view why;
};

class AttributePatternParseRejects
    : public ::testing::TestWithParam<BadPattern> {};

TEST_P(AttributePatternParseRejects, Rejected) {
  EXPECT_THAT(AttributePattern::Parse(GetParam().input),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument))
      << "input=`" << GetParam().input << "` — " << GetParam().why;
}

// One representative per rejection class (FSM unit coverage).  The
// exhaustive "almost-correct" matrix is exercised end-to-end through
// the partial-eval entry point in
// e2e/m2_partial_eval_test.cc::MalformedPatternBoundaryTest.
INSTANTIATE_TEST_SUITE_P(
    Malformed, AttributePatternParseRejects,
    ::testing::Values(BadPattern{"", "empty"}, BadPattern{"x.", "trailing dot"},
                      BadPattern{"x..y", "consecutive dots"},
                      BadPattern{"a b", "internal whitespace"},
                      BadPattern{"1x", "digit-leading ident"},
                      BadPattern{"a-b", "punctuation"},
                      BadPattern{"*", "wildcard root"},
                      BadPattern{"c.*x", "wildcard glued to ident"},
                      BadPattern{"xs[3]", "closed bracket index"},
                      BadPattern{"xs[3", "unclosed bracket"},
                      BadPattern{"xs]", "lone close bracket"}));

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
