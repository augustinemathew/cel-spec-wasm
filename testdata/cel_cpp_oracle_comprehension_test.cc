// Oracle pins for comprehension-over-UNKNOWN/ERROR-range semantics
// (cleanup-backlog #14).  cel-cpp's ComprehensionDirectStep evaluates
// the iter_range FIRST and routes ValueKind::kError (fallthrough) and
// ValueKind::kUnknown straight to `result = std::move(range)` — no
// accu_init, no loop body, no result expression
// (third_party/cel-cpp/eval/eval/comprehension_step.cc:165-169
// (Evaluate1) and :350-354 (Evaluate2)).  These tests RUN cel-cpp to
// confirm that verdict empirically for every standard macro, in both
// the UNKNOWN-range (partial eval) and ERROR-range (plain eval)
// directions, plus controls proving a concrete range still iterates
// and a 3VL BODY over a concrete range keeps its existing behavior.
//
// Oracle-only TU — deliberately does NOT link our pipeline (the
// `cel::Value` symbol-clash note on testdata/BUILD.bazel's
// cel_cpp_oracle_test target).  Our pipeline's matching assertions
// live in e2e/partial_eval_test.cc.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "cel/expr/value.pb.h"
#include "gtest/gtest.h"
#include "testdata/cel_cpp_oracle.h"

namespace celwasm {
namespace {

constexpr absl::string_view kP3 = "cel.expr.conformance.proto3";

cel::expr::Value OracleListOfInts(const std::vector<int64_t>& xs) {
  cel::expr::Value v;
  for (int64_t x : xs) {
    v.mutable_list_value()->add_values()->set_int64_value(x);
  }
  return v;
}

testdata::OracleResult PartialOracleOk(
    absl::string_view source, const std::vector<testdata::OracleVar>& vars,
    const std::vector<std::string>& patterns) {
  auto r = testdata::PartialEvalWithCelCpp(source, kP3, vars, patterns);
  ABSL_CHECK_OK(r) << source;
  return *std::move(r);
}

testdata::OracleResult OracleOk(absl::string_view source) {
  auto r = testdata::EvalWithCelCpp(source, kP3);
  ABSL_CHECK_OK(r) << source;
  return *std::move(r);
}

// ── UNKNOWN range: every standard macro must yield UNKNOWN ──
//
// `xs` is declared (dyn) and marked unknown via the whole-variable
// pattern; the macro's identity result (false / true / false / [] /
// []) would be the SILENTLY WRONG answer.

struct UnknownRangeCase {
  std::string name;
  std::string source;
};

class ComprehensionUnknownRangeOracle
    : public ::testing::TestWithParam<UnknownRangeCase> {};

TEST_P(ComprehensionUnknownRangeOracle, IsUnknown) {
  auto r = PartialOracleOk(GetParam().source, {{"xs", std::nullopt}}, {"xs"});
  EXPECT_FALSE(r.is_error) << GetParam().source << " → " << r.error_message;
  EXPECT_TRUE(r.is_unknown)
      << GetParam().source
      << " over an unknown range must be UNKNOWN, not the macro identity";
}

INSTANTIATE_TEST_SUITE_P(
    AllMacros, ComprehensionUnknownRangeOracle,
    ::testing::Values(UnknownRangeCase{"Exists", "xs.exists(e, e > 0)"},
                      UnknownRangeCase{"All", "xs.all(e, e > 0)"},
                      UnknownRangeCase{"ExistsOne", "xs.exists_one(e, e > 0)"},
                      UnknownRangeCase{"Map", "xs.map(e, e + 1)"},
                      UnknownRangeCase{"Filter", "xs.filter(e, e > 0)"}),
    [](const ::testing::TestParamInfo<UnknownRangeCase>& info) {
      return info.param.name;
    });

// Map-typed range (transform-map shape `m.map(k, …)` iterates keys):
// an unknown MAP range absorbs exactly like an unknown list range.
TEST(ComprehensionUnknownRangeOracle, MapRangeExistsIsUnknown) {
  auto r =
      PartialOracleOk("m.exists(k, k == 'a')", {{"m", std::nullopt}}, {"m"});
  EXPECT_FALSE(r.is_error);
  EXPECT_TRUE(r.is_unknown);
}

TEST(ComprehensionUnknownRangeOracle, MapRangeTransformMapIsUnknown) {
  auto r = PartialOracleOk("m.map(k, k)", {{"m", std::nullopt}}, {"m"});
  EXPECT_FALSE(r.is_error);
  EXPECT_TRUE(r.is_unknown);
}

// ── ERROR range: every standard macro must yield the ERROR ──
//
// `[[1]][1]` is a list-typed index-out-of-bounds error; `{'a': {'b':
// 1}}['c']` is a map-typed missing-key error.  cel-cpp propagates the
// range error as the comprehension result (comprehension_step.cc
// kError arm, same `result = std::move(range)`).

struct ErrorRangeCase {
  std::string name;
  std::string source;
};

class ComprehensionErrorRangeOracle
    : public ::testing::TestWithParam<ErrorRangeCase> {};

TEST_P(ComprehensionErrorRangeOracle, IsError) {
  auto r = OracleOk(GetParam().source);
  EXPECT_FALSE(r.is_unknown) << GetParam().source;
  EXPECT_TRUE(r.is_error)
      << GetParam().source
      << " over an error range must propagate the ERROR, not the identity";
}

INSTANTIATE_TEST_SUITE_P(
    AllMacros, ComprehensionErrorRangeOracle,
    ::testing::Values(
        ErrorRangeCase{"Exists", "[[1]][1].exists(e, e > 0)"},
        ErrorRangeCase{"All", "[[1]][1].all(e, e > 0)"},
        ErrorRangeCase{"ExistsOne", "[[1]][1].exists_one(e, e > 0)"},
        ErrorRangeCase{"Map", "[[1]][1].map(e, e + 1)"},
        ErrorRangeCase{"Filter", "[[1]][1].filter(e, e > 0)"},
        ErrorRangeCase{"MapRangeExists",
                       "{'a': {'b': 1}}['c'].exists(k, k == 'b')"},
        ErrorRangeCase{"MapRangeTransformMap",
                       "{'a': {'b': 1}}['c'].map(k, k)"}),
    [](const ::testing::TestParamInfo<ErrorRangeCase>& info) {
      return info.param.name;
    });

// ERROR dominates UNKNOWN when the range itself is the error and an
// unknown only appears in the (never-entered) body — consistent with
// the strict-call precedence pinned in
// doc/design/03-abi-and-memory.md §8.1.
TEST(ComprehensionErrorRangeOracle, ErrorRangeDominatesUnknownBody) {
  auto r = PartialOracleOk("[[1]][1].exists(e, e > x)", {{"x", std::nullopt}},
                           {"x"});
  EXPECT_FALSE(r.is_unknown)
      << "the range error propagates; the unknown body never runs";
  EXPECT_TRUE(r.is_error);
}

// ── Controls: concrete range still iterates ──

TEST(ComprehensionRangeControlOracle, ConcreteRangeExistsIsConcrete) {
  auto r = PartialOracleOk("xs.exists(e, e > 10)",
                           {{"xs", OracleListOfInts({10, 20, 30})}}, {"ys"});
  ASSERT_FALSE(r.is_unknown);
  ASSERT_FALSE(r.is_error);
  EXPECT_TRUE(r.value.bool_value()) << "20 > 10 → exists is true";
}

// Negative control — accumulator 3VL is a DIFFERENT mechanism than
// range absorption and must keep its behavior: a concrete range whose
// BODY references an unknown yields unknown (merged into the accu
// per-iteration), and a body that errors yields the error.
TEST(ComprehensionRangeControlOracle, ConcreteRangeUnknownBodyIsUnknown) {
  auto r = PartialOracleOk("[1, 2, 3].exists(e, e > x)", {{"x", std::nullopt}},
                           {"x"});
  EXPECT_FALSE(r.is_error);
  EXPECT_TRUE(r.is_unknown) << "unknown BODY over a concrete range";
}

TEST(ComprehensionRangeControlOracle, ConcreteRangeErrorBodyIsError) {
  auto r = OracleOk("[1, 2, 3].map(e, e / 0)");
  EXPECT_FALSE(r.is_unknown);
  EXPECT_TRUE(r.is_error) << "error BODY over a concrete range";
}

}  // namespace
}  // namespace celwasm
