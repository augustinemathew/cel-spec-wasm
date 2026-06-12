#include "e2e/fuzz/verdict.h"

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "cel/expr/value.pb.h"
#include "e2e/fuzz/oracle_harness.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm::fuzz {
namespace {

// Synthetic GenAndEvalResult for the compared outcomes.
GenAndEvalResult IntResult(int64_t ours, int64_t oracle) {
  GenAndEvalResult r;
  r.source = "1 + 1";
  r.ours = Value::Int(ours);
  r.oracle.set_int64_value(oracle);
  return r;
}

// ── Judge: one case per VerdictKind ──────────────────────────────

TEST(JudgeTest, OkEqualValuesIsAgreed) {
  const Verdict v = Judge(/*seed=*/7, /*depth=*/3, GenAndEvalStatus::kOk,
                          IntResult(2, 2), "");
  EXPECT_EQ(v.kind, VerdictKind::kAgreed);
  EXPECT_FALSE(v.IsDivergence());
  EXPECT_FALSE(v.IsFailure());
  EXPECT_EQ(v.seed, 7u);
  EXPECT_EQ(v.depth, 3);
  EXPECT_EQ(v.source, "1 + 1");
}

TEST(JudgeTest, OkUnequalValuesIsValueDiverged) {
  const Verdict v = Judge(7, 3, GenAndEvalStatus::kOk, IntResult(2, 3), "");
  EXPECT_EQ(v.kind, VerdictKind::kValueDiverged);
  EXPECT_TRUE(v.IsDivergence());
  EXPECT_TRUE(v.IsFailure());
  // The want/got mismatch diff is populated for the divergence log.
  EXPECT_FALSE(v.detail.empty());
}

TEST(JudgeTest, BothErroredIsAgreementNotFailure) {
  const Verdict v =
      Judge(7, 3, GenAndEvalStatus::kBothErrored, IntResult(0, 0), "overflow");
  EXPECT_EQ(v.kind, VerdictKind::kBothErrored);
  EXPECT_FALSE(v.IsDivergence());
  EXPECT_FALSE(v.IsFailure());
}

TEST(JudgeTest, OracleErrorOnlyIsDivergence) {
  const Verdict v = Judge(7, 3, GenAndEvalStatus::kOracleErrorOnly,
                          IntResult(2, 0), "integer overflow");
  EXPECT_EQ(v.kind, VerdictKind::kOracleErrorOnly);
  EXPECT_TRUE(v.IsDivergence());
  EXPECT_TRUE(v.IsFailure());
  EXPECT_EQ(v.detail, "integer overflow");
}

TEST(JudgeTest, ResourceExhaustedRejectIsCapacitySkip) {
  GenAndEvalResult r = IntResult(0, 0);
  r.our_status = absl::ResourceExhaustedError("static window");
  const Verdict v =
      Judge(7, 8, GenAndEvalStatus::kOurPipelineRejected, r, "window");
  EXPECT_EQ(v.kind, VerdictKind::kOurCapacityReject);
  EXPECT_FALSE(v.IsDivergence());
  EXPECT_FALSE(v.IsFailure()) << "capacity rejects are a known skip";
}

TEST(JudgeTest, OtherRejectIsUnexpectedAndFails) {
  GenAndEvalResult r = IntResult(0, 0);
  r.our_status = absl::InternalError("lowering bug");
  const Verdict v =
      Judge(7, 3, GenAndEvalStatus::kOurPipelineRejected, r, "lowering bug");
  EXPECT_EQ(v.kind, VerdictKind::kOurUnexpectedReject);
  EXPECT_FALSE(v.IsDivergence());
  EXPECT_TRUE(v.IsFailure());
}

TEST(JudgeTest, OracleRejectedFails) {
  const Verdict v = Judge(7, 3, GenAndEvalStatus::kOracleRejected,
                          IntResult(0, 0), "no matching overload");
  EXPECT_EQ(v.kind, VerdictKind::kOracleRejected);
  EXPECT_TRUE(v.IsFailure());
}

TEST(JudgeTest, SourceTooLargeIsSkip) {
  const Verdict v =
      Judge(7, 8, GenAndEvalStatus::kSourceTooLarge, IntResult(0, 0), "");
  EXPECT_EQ(v.kind, VerdictKind::kSourceTooLarge);
  EXPECT_FALSE(v.IsFailure());
}

// ── Report: the grep-able prefixes scripts depend on ─────────────

TEST(ReportTest, ValueDivergenceUsesDivergePrefix) {
  const Verdict v = Judge(7, 3, GenAndEvalStatus::kOk, IntResult(2, 3), "");
  const std::string report = v.Report("int");
  EXPECT_NE(report.find("DIVERGE [int seed=7]"), std::string::npos) << report;
  EXPECT_NE(report.find("source = 1 + 1"), std::string::npos) << report;
  // The mismatch line carries CompareValue's want/got diff.
  EXPECT_NE(report.find("mismatch = "), std::string::npos) << report;
  EXPECT_NE(report.find("want"), std::string::npos) << report;
}

TEST(ReportTest, OracleErrorOnlyUsesErrorDivergePrefix) {
  const Verdict v = Judge(7, 3, GenAndEvalStatus::kOracleErrorOnly,
                          IntResult(2, 0), "integer overflow");
  const std::string report = v.Report("int");
  EXPECT_NE(report.find("ERROR-DIVERGE"), std::string::npos) << report;
  EXPECT_NE(report.find("integer overflow"), std::string::npos) << report;
}

TEST(ReportTest, RejectPrefixes) {
  GenAndEvalResult r = IntResult(0, 0);
  r.our_status = absl::InternalError("boom");
  const Verdict our =
      Judge(7, 3, GenAndEvalStatus::kOurPipelineRejected, r, "boom");
  EXPECT_NE(our.Report("int").find("OUR-REJECT"), std::string::npos);

  const Verdict oracle =
      Judge(7, 3, GenAndEvalStatus::kOracleRejected, IntResult(0, 0), "boom");
  EXPECT_NE(oracle.Report("int").find("ORACLE-REJECT"), std::string::npos);
}

// ── RunOne: generative smoke (no outcome pinned — the grammar
// evolves; we assert the plumbing, not the verdict) ───────────────

TEST(RunOneTest, ProducesAJudgedVerdictWithSource) {
  const Verdict v = RunOne(CelType::Int(), /*seed=*/1, /*depth=*/2);
  EXPECT_FALSE(v.source.empty());
  EXPECT_EQ(v.seed, 1u);
  EXPECT_EQ(v.depth, 2);
  // Whatever the outcome, the report renders.
  EXPECT_FALSE(v.Report("int").empty());
}

}  // namespace
}  // namespace celwasm::fuzz
