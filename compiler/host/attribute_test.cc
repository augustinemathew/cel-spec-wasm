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

AttributeQualifier Q(const std::string& s) {
  return AttributeQualifier(s);
}

Attribute Attr(const std::string& var, const std::vector<std::string>& path) {
  std::vector<AttributeQualifier> qs;
  qs.reserve(path.size());
  for (const auto& p : path) {
    qs.push_back(Q(p));
  }
  return {var, std::move(qs)};
}

TEST(AttributePatternTest, FullMatchSamePath) {
  auto pat = ParseUnknownAttributePattern("request.user");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user"})), MatchType::kFull);
}

TEST(AttributePatternTest, FullMatchAttributeDeeperThanPattern) {
  // Pattern `request` covers the whole attribute `request.user.name`.
  auto pat = ParseUnknownAttributePattern("request");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user", "name"})), MatchType::kFull);
}

TEST(AttributePatternTest, PartialMatchPatternLongerThanAttribute) {
  // Pattern talks about `request.user.name` but the caller only has
  // `request.user` resolved so far — PARTIAL.
  auto pat = ParseUnknownAttributePattern("request.user.name");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user"})), MatchType::kPartial);
}

TEST(AttributePatternTest, NoneOnVariableMismatch) {
  auto pat = ParseUnknownAttributePattern("request.user");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("resource", {"user"})), MatchType::kNone);
}

TEST(AttributePatternTest, NoneOnQualifierMismatch) {
  auto pat = ParseUnknownAttributePattern("request.user.email");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user", "name"})), MatchType::kNone);
}

TEST(AttributePatternTest, WildcardMatchesAnyQualifierAtPosition) {
  auto pat = ParseUnknownAttributePattern("request.user.*");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user", "email"})), MatchType::kFull);
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user", "name"})), MatchType::kFull);
  // Different second qualifier -> fails to match even with a later wildcard.
  EXPECT_EQ(pat->IsMatch(Attr("request", {"admin", "name"})), MatchType::kNone);
}

TEST(AttributePatternTest, WildcardCoversDeeperAttribute) {
  auto pat = ParseUnknownAttributePattern("request.*");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {"user", "name"})), MatchType::kFull);
}

TEST(AttributePatternTest, EmptyAttributePathFullMatchesBareVariable) {
  auto pat = ParseUnknownAttributePattern("request");
  ASSERT_THAT(pat, IsOk());
  EXPECT_EQ(pat->IsMatch(Attr("request", {})), MatchType::kFull);
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
  EXPECT_EQ(pat->IsMatch(Attr("req_1", {"user_2", "id42"})), MatchType::kFull);
}

}  // namespace
}  // namespace celwasm
