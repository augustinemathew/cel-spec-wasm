#include "compiler/host/attribute.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using MatchType = AttributePattern::MatchType;

AttributeQualifier Q(std::string s) { return AttributeQualifier(std::move(s)); }

Attribute Attr(std::string var, std::vector<std::string> path) {
  std::vector<AttributeQualifier> qs;
  qs.reserve(path.size());
  for (auto& p : path) qs.push_back(Q(std::move(p)));
  return Attribute(std::move(var), std::move(qs));
}

TEST(AttributePatternTest, FullMatchSamePath) {
  auto pat = ParseUnknownAttributePattern("request.user");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user"})), MatchType::FULL);
}

TEST(AttributePatternTest, FullMatchAttributeDeeperThanPattern) {
  // Pattern `request` covers the whole attribute `request.user.name`.
  auto pat = ParseUnknownAttributePattern("request");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user", "name"})), MatchType::FULL);
}

TEST(AttributePatternTest, PartialMatchPatternLongerThanAttribute) {
  // Pattern talks about `request.user.name` but the caller only has
  // `request.user` resolved so far — PARTIAL.
  auto pat = ParseUnknownAttributePattern("request.user.name");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user"})), MatchType::PARTIAL);
}

TEST(AttributePatternTest, NoneOnVariableMismatch) {
  auto pat = ParseUnknownAttributePattern("request.user");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("resource", {"user"})), MatchType::NONE);
}

TEST(AttributePatternTest, NoneOnQualifierMismatch) {
  auto pat = ParseUnknownAttributePattern("request.user.email");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user", "name"})), MatchType::NONE);
}

TEST(AttributePatternTest, WildcardMatchesAnyQualifierAtPosition) {
  auto pat = ParseUnknownAttributePattern("request.user.*");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user", "email"})),
            MatchType::FULL);
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user", "name"})), MatchType::FULL);
  // Different second qualifier -> fails to match even with a later wildcard.
  EXPECT_EQ(pat->IsMatch(Attr("request", {"admin", "name"})),
            MatchType::NONE);
}

TEST(AttributePatternTest, WildcardCoversDeeperAttribute) {
  auto pat = ParseUnknownAttributePattern("request.*");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user", "name"})), MatchType::FULL);
}

TEST(AttributePatternTest, EmptyAttributePathFullMatchesBareVariable) {
  auto pat = ParseUnknownAttributePattern("request");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {})), MatchType::FULL);
}

TEST(AttributePatternTest, ParseRejectsEmpty) {
  EXPECT_THAT(ParseUnknownAttributePattern(""),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternTest, ParseRejectsLeadingDot) {
  EXPECT_THAT(ParseUnknownAttributePattern(".user"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternTest, ParseRejectsTrailingDot) {
  EXPECT_THAT(ParseUnknownAttributePattern("request."),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternTest, ParseRejectsConsecutiveDots) {
  EXPECT_THAT(ParseUnknownAttributePattern("request..user"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternTest, ParseRejectsBadChars) {
  EXPECT_THAT(ParseUnknownAttributePattern("request.user!"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AttributePatternTest, ParseAcceptsUnderscoresAndDigits) {
  auto pat = ParseUnknownAttributePattern("req_1.user_2.id42");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("req_1", {"user_2", "id42"})), MatchType::FULL);
}

}  // namespace
}  // namespace celwasm
