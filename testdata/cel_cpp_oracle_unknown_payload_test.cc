// Oracle pins for the unknown-result PROVENANCE contract (the
// `payload.unk` wire question, doc/design/03-abi-and-memory.md §8.2):
// when an evaluation result depends on attributes marked unknown, does
// cel-cpp's result carry ONE attribute identity or the merged set of
// ALL of them?
//
// Answer pinned here empirically: the merged set, deduplicated.
// cel-cpp's logical ops route both-unknown operands through
// `AttributeUtility::MergeUnknowns` (cel-cpp eval/eval/logic_step.cc:233,
// attribute_utility.cc:107-130), and strict functions do the same after
// the error scan (function_step.cc:219); `AttributeSet::Merge`
// (base/attribute_set.h:84-87) is a sorted-set union.  These pins are
// the reference that any single-id unknown wire (ours stamps ONE
// attribute id in `payload.unk`) provably loses information against.
//
// Lives in its own TU (not cel_cpp_oracle_test.cc) so it can land
// independently of edits to that file; it links ONLY the oracle, not
// our pipeline.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "cel/expr/value.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "testdata/cel_cpp_oracle.h"

namespace celwasm {
namespace {

using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

constexpr absl::string_view kP3 = "cel.expr.conformance.proto3";

// A one-entry string→int map value, so a dotted pattern like "a.x" can
// FULL-match a real select (`a.x`) on a bound operand.
cel::expr::Value OracleMapOfInt(absl::string_view key, int64_t val) {
  cel::expr::Value v;
  auto* entry = v.mutable_map_value()->add_entries();
  entry->mutable_key()->set_string_value(std::string(key));
  entry->mutable_value()->set_int64_value(val);
  return v;
}

testdata::OracleResult PartialOracleOk(
    absl::string_view source, const std::vector<testdata::OracleVar>& vars,
    const std::vector<std::string>& patterns) {
  auto r = testdata::PartialEvalWithCelCpp(source, kP3, vars, patterns);
  ABSL_CHECK_OK(r) << source;
  return *std::move(r);
}

// Vars for the dotted-pattern cases: `a` bound to {"x": 1}, `b` bound
// to {"y": 2}, so the selects themselves evaluate and the unknown is
// minted at the SELECT step (a FULL match of "a.x" / "b.y"), mirroring
// the cel_get_field trampoline path — the exact shape of the §8.2 / V2
// probe recipe.
std::vector<testdata::OracleVar> DottedVars() {
  return {{"a", OracleMapOfInt("x", 1)}, {"b", OracleMapOfInt("y", 2)}};
}

// ── single unknown: the baseline identity ──────────────────────────

TEST(UnknownPayloadOracle, SingleSelectUnknownCarriesItsAttribute) {
  auto r = PartialOracleOk("a.x", DottedVars(), {"a.x"});
  ASSERT_TRUE(r.is_unknown);
  EXPECT_THAT(r.unknown_attributes, ElementsAre("a.x"));
}

// ── both-unknown through the logical ops (the V2 recipe case):
//    `a.x && b.y`, both FULL-matched → ONE unknown carrying BOTH
//    attribute identities ──────────────────────────────────────────

TEST(UnknownPayloadOracle, AndBothUnknownMergesBothAttributes) {
  auto r =
      PartialOracleOk("a.x == 1 && b.y == 2", DottedVars(), {"a.x", "b.y"});
  ASSERT_TRUE(r.is_unknown);
  EXPECT_THAT(r.unknown_attributes, UnorderedElementsAre("a.x", "b.y"))
      << "cel-cpp must report the MERGED set, not one winner";
}

TEST(UnknownPayloadOracle, OrBothUnknownMergesBothAttributes) {
  auto r =
      PartialOracleOk("a.x == 1 || b.y == 2", DottedVars(), {"a.x", "b.y"});
  ASSERT_TRUE(r.is_unknown);
  EXPECT_THAT(r.unknown_attributes, UnorderedElementsAre("a.x", "b.y"));
}

// ── both-unknown through a STRICT function (`_+_`): merged too —
//    cel-cpp function_step.cc:219 merges unknown args after the error
//    scan, so strict ops are NOT first-unknown-wins ─────────────────

TEST(UnknownPayloadOracle, AddBothUnknownMergesBothAttributes) {
  auto r = PartialOracleOk("a.x + b.y", DottedVars(), {"a.x", "b.y"});
  ASSERT_TRUE(r.is_unknown);
  EXPECT_THAT(r.unknown_attributes, UnorderedElementsAre("a.x", "b.y"));
}

// ── bare-variable unknowns (the activation-marshal path: whole
//    variables opaque, no binding needed) merge the same way ────────

TEST(UnknownPayloadOracle, BareVarsAndBothUnknownMergesBothAttributes) {
  auto r = PartialOracleOk("a && b", {{"a", std::nullopt}, {"b", std::nullopt}},
                           {"a", "b"});
  ASSERT_TRUE(r.is_unknown);
  EXPECT_THAT(r.unknown_attributes, UnorderedElementsAre("a", "b"));
}

TEST(UnknownPayloadOracle, BareVarsAddBothUnknownMergesBothAttributes) {
  auto r = PartialOracleOk("a + b", {{"a", std::nullopt}, {"b", std::nullopt}},
                           {"a", "b"});
  ASSERT_TRUE(r.is_unknown);
  EXPECT_THAT(r.unknown_attributes, UnorderedElementsAre("a", "b"));
}

// ── the merge deduplicates: the SAME attribute on both sides yields
//    a single-element set, not a two-element one ────────────────────

TEST(UnknownPayloadOracle, SameAttributeBothSidesDeduplicates) {
  auto r = PartialOracleOk("a.x == 1 && a.x == 2", DottedVars(), {"a.x"});
  ASSERT_TRUE(r.is_unknown);
  EXPECT_THAT(r.unknown_attributes, ElementsAre("a.x"));
}

// ── UNKNOWN-vs-ERROR precedence for the LOGIC ops ───────────────────
//
// cel-cpp's LogicalOpStep::Calculate (eval/eval/logic_step.cc, the
// "logical operation treat Unknowns with higher precedence than error"
// comment block) merges unknowns BEFORE scanning for errors: a
// resolved unknown may later short-circuit the error away, so the
// unknown is the more informative outcome.  This is the OPPOSITE of
// the strict-op rule (function_step.cc::NoOverloadResult scans for an
// ErrorValue first) — the precedence is per-op-class, not global.
// `1/0 == 1` is the error operand; `a.x == 1` with pattern "a.x" is
// the unknown operand.

TEST(UnknownPayloadOracle, AndUnknownLeftErrorRightIsUnknown) {
  auto r = PartialOracleOk("a.x == 1 && 1 / 0 == 1", DottedVars(), {"a.x"});
  ASSERT_TRUE(r.is_unknown) << "logic ops: UNKNOWN outranks ERROR";
  EXPECT_THAT(r.unknown_attributes, ElementsAre("a.x"));
}

TEST(UnknownPayloadOracle, AndErrorLeftUnknownRightIsUnknown) {
  auto r = PartialOracleOk("1 / 0 == 1 && a.x == 1", DottedVars(), {"a.x"});
  ASSERT_TRUE(r.is_unknown);
  EXPECT_THAT(r.unknown_attributes, ElementsAre("a.x"));
}

TEST(UnknownPayloadOracle, OrUnknownLeftErrorRightIsUnknown) {
  auto r = PartialOracleOk("a.x == 1 || 1 / 0 == 1", DottedVars(), {"a.x"});
  ASSERT_TRUE(r.is_unknown);
  EXPECT_THAT(r.unknown_attributes, ElementsAre("a.x"));
}

TEST(UnknownPayloadOracle, OrErrorLeftUnknownRightIsUnknown) {
  auto r = PartialOracleOk("1 / 0 == 1 || a.x == 1", DottedVars(), {"a.x"});
  ASSERT_TRUE(r.is_unknown);
  EXPECT_THAT(r.unknown_attributes, ElementsAre("a.x"));
}

// Controls: the absorbing bool dominates BOTH error and unknown —
// langdef §"Logical Operators" commutative short-circuit ("false &&
// X = false", "true || X = true", for any X).

TEST(UnknownPayloadOracle, FalseAndErrorIsFalse) {
  auto r = PartialOracleOk("false && 1 / 0 == 1", DottedVars(), {"a.x"});
  ASSERT_FALSE(r.is_unknown);
  ASSERT_FALSE(r.is_error);
  EXPECT_FALSE(r.value.bool_value());
}

TEST(UnknownPayloadOracle, TrueOrErrorIsTrue) {
  auto r = PartialOracleOk("true || 1 / 0 == 1", DottedVars(), {"a.x"});
  ASSERT_FALSE(r.is_unknown);
  ASSERT_FALSE(r.is_error);
  EXPECT_TRUE(r.value.bool_value());
}

TEST(UnknownPayloadOracle, UnknownAndFalseIsFalse) {
  auto r = PartialOracleOk("a.x == 1 && false", DottedVars(), {"a.x"});
  ASSERT_FALSE(r.is_unknown) << "the false absorber beats the unknown";
  ASSERT_FALSE(r.is_error);
  EXPECT_FALSE(r.value.bool_value());
}

TEST(UnknownPayloadOracle, UnknownOrTrueIsTrue) {
  auto r = PartialOracleOk("a.x == 1 || true", DottedVars(), {"a.x"});
  ASSERT_FALSE(r.is_unknown);
  ASSERT_FALSE(r.is_error);
  EXPECT_TRUE(r.value.bool_value());
}

}  // namespace
}  // namespace celwasm
