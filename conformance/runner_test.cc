// Unit tests for the conformance harness comparison helpers and the
// matcher-kind envelope predicate.  End-to-end coverage of `RunOne`
// lives in `run_conformance` against the upstream fixture corpus.

#include "conformance/runner.h"

#include "gtest/gtest.h"

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "cel/expr/eval.pb.h"
#include "eval/error.h"
#include "eval/value.h"
#include "google/protobuf/text_format.h"

namespace celwasm::conformance {
namespace {

celwasm::api::Value MakeRuntimeError(celwasm::ErrorCode code,
                                     const std::string& message) {
  return celwasm::api::Value::Error(
      celwasm::ErrorPayload{.code = code, .message = message, .expr_id = 0});
}

cel::expr::ErrorSet ParseErrorSet(absl::string_view textproto) {
  cel::expr::ErrorSet out;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      std::string(textproto), &out));
  return out;
}

// CompareEvalError — kind-only matching per cel-cpp upstream
// `conformance/run.cc`.  Any error matches any matcher; only kind
// mismatches fail.

TEST(CompareEvalErrorTest, ErrorMatchesAnyMatcher) {
  // Specific message in the matcher is informational only — kind is
  // the only load-bearing check.
  auto got =
      MakeRuntimeError(celwasm::ErrorCode::kDivideByZero, "divide_by_zero");
  auto want = ParseErrorSet(R"pb(errors { message: "anything" })pb");
  EXPECT_EQ(CompareEvalError(got, want), absl::OkStatus());
}

TEST(CompareEvalErrorTest, EmptyMatcherStillMatches) {
  auto got = MakeRuntimeError(celwasm::ErrorCode::kOverflow, "overflow");
  cel::expr::ErrorSet want;
  EXPECT_EQ(CompareEvalError(got, want), absl::OkStatus());
}

TEST(CompareEvalErrorTest, MismatchValueRatherThanError) {
  auto got = celwasm::api::Value::Int(42);
  auto want = ParseErrorSet(R"pb(errors { message: "any" })pb");
  auto s = CompareEvalError(got, want);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(s.message(), "want-kind=error"));
}

TEST(CompareEvalErrorTest, MismatchUnknownIsNotError) {
  auto got = celwasm::api::Value::Unknown(celwasm::AttributeId{.id = 1});
  auto want = ParseErrorSet(R"pb(errors { message: "any" })pb");
  auto s = CompareEvalError(got, want);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(s.message(), "got-kind=unknown"));
}

TEST(CompareEvalErrorTest, MismatchNullIsNotError) {
  auto got = celwasm::api::Value::Null();
  auto want = ParseErrorSet(R"pb(errors { message: "any" })pb");
  auto s = CompareEvalError(got, want);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(s.message(), "got-kind=null"));
}

// Envelope membership --------------------------------------------

TEST(IsInEnvelopeTest, AdmitsEvalErrorMatcher) {
  cel::expr::conformance::test::SimpleTest t;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        name: "div_by_zero"
        expr: "1/0"
        eval_error { errors { message: "divide_by_zero" } }
      )pb",
      &t));
  EXPECT_TRUE(IsInEnvelope(t));
}

TEST(IsInEnvelopeTest, AdmitsAnyEvalErrorsMatcher) {
  cel::expr::conformance::test::SimpleTest t;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        name: "any_eval_errors_demo"
        expr: "1/0"
        any_eval_errors { errors { errors { message: "divide_by_zero" } } }
      )pb",
      &t));
  EXPECT_TRUE(IsInEnvelope(t));
}

TEST(IsInEnvelopeTest, AdmitsTypedResult) {
  cel::expr::conformance::test::SimpleTest t;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        name: "typed_result_demo"
        expr: "1 + 1"
        typed_result {
          result { int64_value: 2 }
          deduced_type { primitive: INT64 }
        }
      )pb",
      &t));
  EXPECT_TRUE(IsInEnvelope(t));
}

// `IsInEnvelope` is strictly a matcher-kind predicate.  The
// `disable_check` / `check_only` flags are checked separately in
// `RunOne`'s `ScopeReject`; the predicate ignores them.
TEST(IsInEnvelopeTest, MatcherInScopeIgnoresDisableCheckFlag) {
  cel::expr::conformance::test::SimpleTest t;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        name: "matcher_only_check"
        expr: "-(42u)"
        disable_check: true
        eval_error { errors { message: "no_such_overload" } }
      )pb",
      &t));
  EXPECT_TRUE(IsInEnvelope(t));
}

// A SimpleTest with no result_matcher set is an implicit
// bool-true assertion (`expr` is expected to evaluate to true).
// Mirrors upstream cel-cpp's `conformance/run.cc` convention.
TEST(IsInEnvelopeTest, AdmitsUnsetMatcherAsImplicitBoolTrue) {
  cel::expr::conformance::test::SimpleTest t;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        name: "bool_asserting_no_matcher" expr: "'tacocat'.charAt(3) == 'o'"
      )pb",
      &t));
  EXPECT_TRUE(IsInEnvelope(t));
}

TEST(IsInEnvelopeTest, MatcherInScopeIgnoresCheckOnlyFlag) {
  cel::expr::conformance::test::SimpleTest t;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        name: "matcher_only_check"
        expr: "1/0"
        check_only: true
        eval_error { errors { message: "divide_by_zero" } }
      )pb",
      &t));
  EXPECT_TRUE(IsInEnvelope(t));
}

// Skip-category naming -------------------------------------------

TEST(SkipCategoryNameTest, RoundTripsAllValues) {
  // Exhaustive over the closed enum.  Names are part of the public
  // harness contract — README's per-fixture SKIP-by-category table
  // groups by these strings.
  EXPECT_EQ(SkipCategoryName(SkipCategory::kDisableCheck), "disable_check");
  EXPECT_EQ(SkipCategoryName(SkipCategory::kCheckOnly), "check_only");
  EXPECT_EQ(SkipCategoryName(SkipCategory::kEnvelope), "envelope");
  EXPECT_EQ(SkipCategoryName(SkipCategory::kStaticSubset), "static_subset");
  EXPECT_EQ(SkipCategoryName(SkipCategory::kCompileUnimpl), "compile_unimpl");
  EXPECT_EQ(SkipCategoryName(SkipCategory::kEvalUnimpl), "eval_unimpl");
  EXPECT_EQ(SkipCategoryName(SkipCategory::kExtensionUnimpl), "ext_unimpl");
  EXPECT_EQ(SkipCategoryName(SkipCategory::kTypeEnvUnsupported), "type_env");
  EXPECT_EQ(SkipCategoryName(SkipCategory::kBindingUnsupported), "bindings");
}

}  // namespace
}  // namespace celwasm::conformance
