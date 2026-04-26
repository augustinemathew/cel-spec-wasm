// Tests for the M4 `eval_error` / `any_eval_errors` matcher
// branch.  Construct `cel::Value::Error` directly and `ErrorSet`
// matchers as proto literals, then exercise `CompareEvalError`
// through every outcome documented in `runner.h`.
//
// `RunOne` end-to-end coverage lives in the `run_conformance`
// fixture harness — these tests target the comparison helper in
// isolation so the proto-encoding shape and the compare semantics
// are pinned independently.

#include "compiler_v2/conformance/runner.h"

#include "gtest/gtest.h"

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "cel/expr/eval.pb.h"
#include "compiler_v2/api/error.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/text_format.h"

namespace celwasm::conformance {
namespace {

// Helpers --------------------------------------------------------

cel::Value MakeRuntimeError(cel::ErrorCode code, const std::string& message) {
  return cel::Value::Error(
      cel::ErrorPayload{.code = code, .message = message, .expr_id = 0});
}

cel::expr::ErrorSet ParseErrorSet(absl::string_view textproto) {
  cel::expr::ErrorSet out;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      std::string(textproto), &out));
  return out;
}

// kEvalError comparisons -----------------------------------------

TEST(CompareEvalErrorTest, MatchExactMessage) {
  // Got and want messages are byte-identical → loose match
  // succeeds on the first substring check, before any normalise
  // fallback or kind-only fallback fires.
  auto got = MakeRuntimeError(cel::ErrorCode::kDivideByZero, "divide_by_zero");
  auto want = ParseErrorSet(R"pb(errors { message: "divide_by_zero" })pb");
  EXPECT_EQ(CompareEvalError(got, want), absl::OkStatus());
}

TEST(CompareEvalErrorTest, MatchSubstringMessage) {
  // Want is a substring of got — the harness should accept loose
  // either-direction substring matches.
  auto got = MakeRuntimeError(cel::ErrorCode::kOverflow,
                              "integer overflow during addition");
  auto want = ParseErrorSet(R"pb(errors { message: "overflow" })pb");
  EXPECT_EQ(CompareEvalError(got, want), absl::OkStatus());
}

TEST(CompareEvalErrorTest, MatchNormalisedSpaceUnderscore) {
  // Fixture phrasing ("divide by zero") vs runtime payload
  // ("divide_by_zero") differ only in `_` vs space + case — the
  // normalised-substring fallback should catch this.
  auto got = MakeRuntimeError(cel::ErrorCode::kDivideByZero, "divide_by_zero");
  auto want = ParseErrorSet(R"pb(errors { message: "divide by zero" })pb");
  EXPECT_EQ(CompareEvalError(got, want), absl::OkStatus());
}

TEST(CompareEvalErrorTest, MatchEmptyErrorsAcceptsAnyError) {
  // A bare `eval_error: {}` matcher — i.e. errors_size() == 0 — is
  // a wildcard "an error occurred, don't care which".
  auto got = MakeRuntimeError(cel::ErrorCode::kDivideByZero, "divide_by_zero");
  cel::expr::ErrorSet want;  // no `errors` repeated entries
  EXPECT_EQ(CompareEvalError(got, want), absl::OkStatus());
}

TEST(CompareEvalErrorTest, MatchKindOnlyFallback) {
  // Got and want messages have NO substring overlap and don't
  // normalise to one another ("foo" vs "divide_by_zero").  The
  // kind-only fallback (mirrors cel-cpp's harness, which only
  // checks `has_error()`) should still pass — every CEL error
  // matches every non-empty `eval_error` matcher at the kind level.
  auto got = MakeRuntimeError(cel::ErrorCode::kDivideByZero, "divide_by_zero");
  auto want = ParseErrorSet(R"pb(errors { message: "foo" })pb");
  EXPECT_EQ(CompareEvalError(got, want), absl::OkStatus());
}

TEST(CompareEvalErrorTest, MatchAnyOfMultipleWantMessages) {
  // Multiple `errors[]` entries — pass if any one matches.
  auto got = MakeRuntimeError(cel::ErrorCode::kOverflow, "overflow");
  auto want = ParseErrorSet(R"pb(
    errors { message: "no_such_overload" }
    errors { message: "overflow" }
  )pb");
  EXPECT_EQ(CompareEvalError(got, want), absl::OkStatus());
}

TEST(CompareEvalErrorTest, MismatchValueRatherThanError) {
  // Got is a regular int — kind mismatch is the one outcome that
  // MUST fail; otherwise the harness would silently pass any
  // expression that returned a value when the spec required an
  // error (e.g. `1/0` returning 0 instead of an error).
  auto got = cel::Value::Int(42);
  auto want = ParseErrorSet(R"pb(errors { message: "any" })pb");
  auto s = CompareEvalError(got, want);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(s.message(), "want-kind=error"));
}

TEST(CompareEvalErrorTest, MismatchUnknownIsNotError) {
  // A `Value::Unknown` is also kind-distinct from `Value::Error`
  // and must NOT satisfy an `eval_error` matcher (the
  // langdef separates the two).
  auto got = cel::Value::Unknown(cel::AttributeId{.id = 1});
  auto want = ParseErrorSet(R"pb(errors { message: "any" })pb");
  auto s = CompareEvalError(got, want);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(s.message(), "got-kind=unknown"));
}

TEST(CompareEvalErrorTest, MismatchNullIsNotError) {
  auto got = cel::Value::Null();
  auto want = ParseErrorSet(R"pb(errors { message: "any" })pb");
  auto s = CompareEvalError(got, want);
  EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(s.message(), "got-kind=null"));
}

// Envelope membership --------------------------------------------

TEST(IsInM7EnvelopeTest, AdmitsEvalErrorMatcher) {
  cel::expr::conformance::test::SimpleTest t;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        name: "div_by_zero"
        expr: "1/0"
        eval_error { errors { message: "divide_by_zero" } }
      )pb",
      &t));
  EXPECT_TRUE(IsInM7Envelope(t));
}

TEST(IsInM7EnvelopeTest, AdmitsAnyEvalErrorsMatcher) {
  cel::expr::conformance::test::SimpleTest t;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        name: "any_eval_errors_demo"
        expr: "1/0"
        any_eval_errors { errors { errors { message: "divide_by_zero" } } }
      )pb",
      &t));
  EXPECT_TRUE(IsInM7Envelope(t));
}

TEST(IsInM7EnvelopeTest, RejectsEvalErrorWithDisableCheck) {
  // `disable_check:true` short-circuits BEFORE the matcher gate —
  // even a row whose matcher we now accept stays SKIP if the
  // type-checker is disabled (the harness needs a checked AST).
  // Several fixture rows (e.g. `unary_minus_not_uint` in
  // integer_math) carry both `disable_check:true` and
  // `eval_error:` and must remain SKIP.
  cel::expr::conformance::test::SimpleTest t;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        name: "skip_me"
        expr: "-(42u)"
        disable_check: true
        eval_error { errors { message: "no_such_overload" } }
      )pb",
      &t));
  EXPECT_FALSE(IsInM7Envelope(t));
}

TEST(IsInM7EnvelopeTest, RejectsEvalErrorWithCheckOnly) {
  cel::expr::conformance::test::SimpleTest t;
  ABSL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        name: "skip_check_only"
        expr: "1/0"
        check_only: true
        eval_error { errors { message: "divide_by_zero" } }
      )pb",
      &t));
  EXPECT_FALSE(IsInM7Envelope(t));
}

}  // namespace
}  // namespace celwasm::conformance
