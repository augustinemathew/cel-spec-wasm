// M7B e2e test suite — the spec of "done" for timestamp / duration
// surfaces.  Per `doc/implementation-plan/rewrite/m7b-duration-timestamp.md`
// §6 (test matrix) the suite mirrors the m4 / m7 / m10 test shape:
// every test asserts a capability M7B must light up, every test
// today GTEST_SKIPs with a pointer to the slice that will turn it
// on, and the file compiles + builds green TODAY (so the slice-by-
// slice unblock is visible on CI as SKIP → PASS migrations rather
// than as a test-file landing event).
//
// Today (2026-05-16, plan drafted, not yet started):
// `bazel test //compiler_v2/e2e:m7b_test` runs the binary and
// reports every test SKIPPED.  As M7B.A...M7B.E ship, each test
// graduates from GTEST_SKIP → real assertion in the same commit
// that lands the production code, on a row-by-row basis.
//
// Fixtures grouped by capability (one section per slice + cross-
// cutting matrices):
//
//   - RoundTripE2ETest                M7B.A — activation marshalling +
//                                             decoder for kDuration /
//                                             kTimestamp; round-trips
//                                             bound values through Eval.
//   - WellKnownFieldReadE2ETest       M7B.A — singular Timestamp /
//                                             Duration field reads
//                                             normalise to CEL_TIMESTAMP
//                                             / CEL_DURATION (vs the
//                                             current CEL_MESSAGE).
//   - ArithmeticE2ETest               M7B.B — 6 arithmetic helpers ×
//                                             boundary matrix.
//   - OrderingE2ETest                 M7B.B — 8 ordering helpers.
//   - UtcAccessorE2ETest              M7B.C — 10 timestamp UTC
//                                             accessors over the §6.4
//                                             quirk grid + 4 duration
//                                             accessors.
//   - ParseFormatE2ETest              M7B.D — parse / format / int-
//                                             convert / identity
//                                             conversions; admit +
//                                             reject matrix per §6.2.
//   - TzAccessorE2ETest               M7B.E — with-TZ accessor form
//                                             (IANA + fixed-offset +
//                                             reject names).
//   - CrossFormEquivalenceE2ETest     §3.4 — `timestamp("...")` ==
//                                             `Timestamp{seconds: ...}`
//                                             after M7B.A's well-known-
//                                             type normaliser ships.
//   - TypeRegressionE2ETest           §6.6 — `type(timestamp(...))`
//                                             returns the well-known
//                                             type identity.
//   - RejectE2ETest                   §6.8 — checker + runtime
//                                             rejection matrix.
//
// Conformance unlock estimate per slice is logged on each test
// section; aggregate target is +78..+94 PASS in conformance per
// `m7b-duration-timestamp.md` §1.

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/compiler.h"
#include "compiler_v2/api/engine.h"
#include "compiler_v2/api/instance.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

using ::absl_testing::IsOk;

Engine& GlobalEngine() {
  static Engine* engine = [] {
    auto e = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(e);
    return new Engine(*std::move(e));
  }();
  return *engine;
}

using ConfigureFn = std::function<void(Compiler::Builder&)>;
absl::StatusOr<Compiler> BuildCompiler(const ConfigureFn& configure) {
  Compiler::Builder b;
  configure(b);
  return std::move(b).Build();
}

[[maybe_unused]] absl::StatusOr<Compiler> CompilerEmpty() {
  return BuildCompiler([](Compiler::Builder& /*b*/) {});
}

// All e2e helpers below are unused while every test SKIPs.  Once the
// first slice ships and a test body uses them, the
// `[[maybe_unused]]` is dropped.  This file is the spec-of-done; the
// helpers stand ready for slice-by-slice migration.

[[maybe_unused]] Instance CompilePlan(const Compiler& compiler,
                                       absl::string_view source) {
  auto program = compiler.Compile(source);
  ABSL_CHECK_OK(program) << source;
  auto instance = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(instance) << source;
  return *std::move(instance);
}

[[maybe_unused]] Value EvalOk(Instance& instance, const Activation& activation) {
  auto v = instance.Eval(activation);
  ABSL_CHECK_OK(v);
  return *std::move(v);
}

[[maybe_unused]] void ExpectCompileFails(const Compiler& compiler,
                                          absl::string_view source,
                                          absl::string_view why) {
  auto program_or = compiler.Compile(source);
  EXPECT_FALSE(program_or.ok())
      << "expected `" << source << "` to fail at compile (" << why << ")";
}

// ──────────────────────────────────────────────────────────────
// 1. RoundTripE2ETest  (M7B.A — activation marshalling + decoder)
//
//    The first slice's user-facing surface light-up: every path
//    that fails loud on CEL_DURATION / CEL_TIMESTAMP round-trips
//    cleanly.  No constructors yet — `timestamp('...')` still
//    rejects at the static-subset gate until M7B.D ships.  What
//    works after M7B.A: `Activation::Bind("t", Value::Timestamp(...))`
//    round-trips through Eval; equality / inequality already lit
//    via the existing `cel_equals_at_vv` kind-then-payload memcmp.
//
//    Parameterised TEST_P over the §6.1 round-trip matrix:
//    Kind × (seconds boundary) × (nanos boundary) × direction.
// ──────────────────────────────────────────────────────────────

struct RoundTripCase {
  std::string label;
  bool is_timestamp;     // false = duration.
  int64_t seconds;
  int32_t nanos;
};

class RoundTripE2ETest : public ::testing::TestWithParam<RoundTripCase> {};

TEST_P(RoundTripE2ETest, BindReturnDecode) {
  GTEST_SKIP() << "M7B.A not yet shipped — activation marshalling + "
                  "decoder arms for kDuration / kTimestamp are stubs.";
}

INSTANTIATE_TEST_SUITE_P(
    BoundaryGrid, RoundTripE2ETest,
    ::testing::Values(
        RoundTripCase{"DurationZero", false, 0, 0},
        RoundTripCase{"DurationOneSec", false, 1, 0},
        RoundTripCase{"DurationOneNs", false, 0, 1},
        RoundTripCase{"DurationMaxNanos", false, 0, 999'999'999},
        RoundTripCase{"DurationNegOneNs", false, 0, -1},
        RoundTripCase{"DurationIntMaxS", false,
                      std::numeric_limits<int64_t>::max(), 999'999'999},
        RoundTripCase{"DurationIntMinS", false,
                      std::numeric_limits<int64_t>::min(), 0},
        RoundTripCase{"TimestampZero", true, 0, 0},
        RoundTripCase{"TimestampEpochOneSec", true, 1, 0},
        RoundTripCase{"TimestampLangdef", true, 1234567890LL, 0},
        RoundTripCase{"TimestampLangdefMin", true, -62135596800LL, 0},
        RoundTripCase{"TimestampLangdefMax", true, 253402300799LL,
                      999'999'999},
        RoundTripCase{"TimestampMaxNanos", true, 0, 999'999'999}),
    [](const ::testing::TestParamInfo<RoundTripCase>& info) {
      return info.param.label;
    });

// One-off: bind a duration via activation and assert the SDK-side
// decoder produces a matching absl::Duration after Eval returns.
// Distinct from the TEST_P table above which evaluates a CEL
// expression — this exercises the bind + return-value path only.
TEST_F(RoundTripE2ETest, ActivationBindReturnTimestamp) {
  GTEST_SKIP() << "M7B.A not yet shipped — EncodeBoundValue / "
                  "DecodeCelValueAt kTimestamp arms are stubs.";
}

TEST_F(RoundTripE2ETest, ActivationBindReturnDuration) {
  GTEST_SKIP() << "M7B.A not yet shipped — EncodeBoundValue / "
                  "DecodeCelValueAt kDuration arms are stubs.";
}

// Bind declared-but-mismatched: declared kDuration, bound Value::Int
// → encoder must reject.  Pins §6.8 rejection row #1.
TEST_F(RoundTripE2ETest, BindMismatchDurationVsInt) {
  GTEST_SKIP() << "M7B.A not yet shipped — EncodeBoundValue type-"
                  "mismatch arm is a stub.";
}

TEST_F(RoundTripE2ETest, BindMismatchTimestampVsString) {
  GTEST_SKIP() << "M7B.A not yet shipped — EncodeBoundValue type-"
                  "mismatch arm is a stub.";
}

// ──────────────────────────────────────────────────────────────
// 2. WellKnownFieldReadE2ETest  (M7B.A — well-known-type read normaliser)
//
//    Reading a singular `google.protobuf.Timestamp` / `Duration`
//    field returns CEL_TIMESTAMP / CEL_DURATION, NOT CEL_MESSAGE.
//    Pins §4.7 of the plan.  Cross-form equivalence with M7's
//    kStructExpr-constructed messages is checked in
//    CrossFormEquivalenceE2ETest.
// ──────────────────────────────────────────────────────────────

class WellKnownFieldReadE2ETest : public ::testing::Test {};

TEST_F(WellKnownFieldReadE2ETest, ReadTimestampFieldYieldsCelTimestampKind) {
  GTEST_SKIP() << "M7B.A not yet shipped — CelGetFieldImpl well-"
                  "known-type normaliser is a stub.";
}

TEST_F(WellKnownFieldReadE2ETest, ReadDurationFieldYieldsCelDurationKind) {
  GTEST_SKIP() << "M7B.A not yet shipped — CelGetFieldImpl well-"
                  "known-type normaliser is a stub.";
}

TEST_F(WellKnownFieldReadE2ETest, NonWellKnownMessageFieldStillYieldsMessage) {
  // Regression: the normaliser MUST only fire for the two well-known
  // types it knows about.  Other singular-message fields stay
  // CEL_MESSAGE so the M2.C read path keeps working.
  GTEST_SKIP() << "M7B.A not yet shipped — verify normaliser is "
                  "scoped to Timestamp / Duration only.";
}

// ──────────────────────────────────────────────────────────────
// 3. ArithmeticE2ETest  (M7B.B — 6 arithmetic helpers)
//
//    Parameterised TEST_P over the §6.3 arithmetic matrix:
//    6 helpers × 6 boundary scenarios.  Operands are activation-
//    bound (so the test is independent of M7B.D's parse arm).
// ──────────────────────────────────────────────────────────────

enum class ArithOp {
  kDurAddDur,
  kDurSubDur,
  kTsAddDur,
  kDurAddTs,
  kTsSubDur,
  kTsSubTs,
};

struct ArithCase {
  std::string label;
  ArithOp op;
  int64_t a_seconds;
  int32_t a_nanos;
  int64_t b_seconds;
  int32_t b_nanos;
  // Expected result OR expected error flag.
  bool expect_overflow;
  int64_t expected_seconds;
  int32_t expected_nanos;
};

class ArithmeticE2ETest : public ::testing::TestWithParam<ArithCase> {};

TEST_P(ArithmeticE2ETest, BoundaryMatrix) {
  GTEST_SKIP() << "M7B.B not yet shipped — arithmetic helpers "
                  "(cel_dur_add_at_vv etc.) are not registered.";
}

// Shorthand for the six-helper × six-boundary matrix.  Each row is
// one cell of the §6.3 grid: zero, carry, overflow, mixed-sign
// normalise, langdef-bound, negative-result correctness.
INSTANTIATE_TEST_SUITE_P(
    BoundaryMatrix, ArithmeticE2ETest,
    ::testing::Values(
        // ── dur + dur ───────────────────────────────────
        ArithCase{"DurAddDur_Zero", ArithOp::kDurAddDur, 0, 0, 0, 0, false, 0,
                  0},
        ArithCase{"DurAddDur_Carry", ArithOp::kDurAddDur, 1, 0, 0, 1, false, 1,
                  1},
        ArithCase{"DurAddDur_NanosCarry", ArithOp::kDurAddDur, 0, 500'000'000,
                  0, 500'000'000, false, 1, 0},
        ArithCase{"DurAddDur_Overflow", ArithOp::kDurAddDur,
                  std::numeric_limits<int64_t>::max(), 999'999'999, 0, 1, true,
                  0, 0},
        ArithCase{"DurAddDur_MixedSign", ArithOp::kDurAddDur, 1, 0, -1, 0,
                  false, 0, 0},
        ArithCase{"DurAddDur_NegResult", ArithOp::kDurAddDur, -10, 0, -20, 0,
                  false, -30, 0},
        // ── dur - dur ───────────────────────────────────
        ArithCase{"DurSubDur_Zero", ArithOp::kDurSubDur, 0, 0, 0, 0, false, 0,
                  0},
        ArithCase{"DurSubDur_Borrow", ArithOp::kDurSubDur, 1, 0, 0, 1, false, 0,
                  999'999'999},
        ArithCase{"DurSubDur_Underflow", ArithOp::kDurSubDur,
                  std::numeric_limits<int64_t>::min(), 0, 0, 1, true, 0, 0},
        ArithCase{"DurSubDur_NegResult", ArithOp::kDurSubDur, 1, 0, 2, 0, false,
                  -1, 0},
        // ── ts + dur ─────────────────────────────────────
        ArithCase{"TsAddDur_Zero", ArithOp::kTsAddDur, 0, 0, 0, 0, false, 0, 0},
        ArithCase{"TsAddDur_Carry", ArithOp::kTsAddDur, 1234567890, 0, 60, 0,
                  false, 1234567950, 0},
        ArithCase{"TsAddDur_LangdefBoundOver", ArithOp::kTsAddDur,
                  253402300799LL, 0, 1, 0, true, 0, 0},
        ArithCase{"TsAddDur_NanosCarry", ArithOp::kTsAddDur, 1, 999'999'999, 0,
                  1, false, 2, 0},
        // ── dur + ts ─────────────────────────────────────
        ArithCase{"DurAddTs_Zero", ArithOp::kDurAddTs, 0, 0, 0, 0, false, 0, 0},
        ArithCase{"DurAddTs_NormalSum", ArithOp::kDurAddTs, 60, 0, 1234567890,
                  0, false, 1234567950, 0},
        // ── ts - dur ─────────────────────────────────────
        ArithCase{"TsSubDur_Zero", ArithOp::kTsSubDur, 0, 0, 0, 0, false, 0, 0},
        ArithCase{"TsSubDur_LangdefBoundUnder", ArithOp::kTsSubDur,
                  -62135596800LL, 0, 1, 0, true, 0, 0},
        ArithCase{"TsSubDur_Borrow", ArithOp::kTsSubDur, 1, 0, 0, 1, false, 0,
                  999'999'999},
        // ── ts - ts → dur ────────────────────────────────
        ArithCase{"TsSubTs_Zero", ArithOp::kTsSubTs, 0, 0, 0, 0, false, 0, 0},
        ArithCase{"TsSubTs_Positive", ArithOp::kTsSubTs, 1234567950, 0,
                  1234567890, 0, false, 60, 0},
        ArithCase{"TsSubTs_Negative", ArithOp::kTsSubTs, 1234567890, 0,
                  1234567950, 0, false, -60, 0},
        ArithCase{"TsSubTs_LangdefSpan", ArithOp::kTsSubTs, 253402300799LL, 0,
                  -62135596800LL, 0, false, 315537897599LL, 0}),
    [](const ::testing::TestParamInfo<ArithCase>& info) {
      return info.param.label;
    });

// Checker-rejection (no overload) — ts + ts must reject at compile.
TEST_F(ArithmeticE2ETest, CheckerRejectsTimestampPlusTimestamp) {
  GTEST_SKIP() << "M7B.B not yet shipped — pin regression that the "
                  "checker (not codegen) rejects ts+ts.";
}

TEST_F(ArithmeticE2ETest, CheckerRejectsDurationMinusTimestamp) {
  GTEST_SKIP() << "M7B.B not yet shipped — pin regression that the "
                  "checker rejects dur-ts (only ts-dur exists).";
}

// ──────────────────────────────────────────────────────────────
// 4. OrderingE2ETest  (M7B.B — 8 ordering helpers)
//
//    `<`, `<=`, `>`, `>=` on (dur,dur) and (ts,ts) pairs.
//    Equality + inequality already work post-M7B.A via the
//    cel_equals_at_vv kind-then-payload memcmp; the §6.5 rows
//    asserting `dur(1s) == dur(1s)` live in
//    CrossFormEquivalenceE2ETest below.
// ──────────────────────────────────────────────────────────────

enum class CmpOp { kLt, kLe, kGt, kGe };

struct CmpCase {
  std::string label;
  bool is_timestamp;
  CmpOp op;
  int64_t a_seconds;
  int32_t a_nanos;
  int64_t b_seconds;
  int32_t b_nanos;
  bool expected;
};

class OrderingE2ETest : public ::testing::TestWithParam<CmpCase> {};

TEST_P(OrderingE2ETest, LexicographicCompare) {
  GTEST_SKIP() << "M7B.B not yet shipped — ordering helpers "
                  "(cel_dur_lt_at_vv etc.) not registered.";
}

INSTANTIATE_TEST_SUITE_P(
    LexCompareGrid, OrderingE2ETest,
    ::testing::Values(
        // Duration <
        CmpCase{"DurLtTrue", false, CmpOp::kLt, 1, 0, 2, 0, true},
        CmpCase{"DurLtFalse", false, CmpOp::kLt, 2, 0, 1, 0, false},
        CmpCase{"DurLtEqual", false, CmpOp::kLt, 1, 0, 1, 0, false},
        CmpCase{"DurLtNanoTiebreak", false, CmpOp::kLt, 1, 0, 1, 1, true},
        CmpCase{"DurLtNegSign", false, CmpOp::kLt, -1, 0, 1, 0, true},
        // Duration <=
        CmpCase{"DurLeEqual", false, CmpOp::kLe, 1, 0, 1, 0, true},
        CmpCase{"DurLeTrue", false, CmpOp::kLe, 1, 0, 2, 0, true},
        CmpCase{"DurLeFalse", false, CmpOp::kLe, 2, 0, 1, 0, false},
        // Duration >
        CmpCase{"DurGtTrue", false, CmpOp::kGt, 2, 0, 1, 0, true},
        CmpCase{"DurGtFalse", false, CmpOp::kGt, 1, 0, 2, 0, false},
        // Duration >=
        CmpCase{"DurGeTrue", false, CmpOp::kGe, 2, 0, 1, 0, true},
        CmpCase{"DurGeEqual", false, CmpOp::kGe, 1, 0, 1, 0, true},
        // Timestamp <
        CmpCase{"TsLtTrue", true, CmpOp::kLt, 0, 0, 1, 0, true},
        CmpCase{"TsLtFalse", true, CmpOp::kLt, 2, 0, 1, 0, false},
        CmpCase{"TsLtLangdefSpan", true, CmpOp::kLt, -62135596800LL, 0,
                253402300799LL, 0, true},
        // Timestamp <=
        CmpCase{"TsLeEqual", true, CmpOp::kLe, 1, 0, 1, 0, true},
        // Timestamp >
        CmpCase{"TsGtTrue", true, CmpOp::kGt, 1234567950, 0, 1234567890, 0,
                true},
        // Timestamp >=
        CmpCase{"TsGeEqual", true, CmpOp::kGe, 1, 0, 1, 0, true},
        CmpCase{"TsGeFalse", true, CmpOp::kGe, 1, 0, 2, 0, false}),
    [](const ::testing::TestParamInfo<CmpCase>& info) {
      return info.param.label;
    });

// ──────────────────────────────────────────────────────────────
// 5. UtcAccessorE2ETest  (M7B.C — pure-wasm UTC accessors)
//
//    Parameterised over the §6.4 civil-calendar quirk grid (every
//    Gregorian-cycle quirk a naive implementation gets wrong) ×
//    the 10 timestamp accessors + 4 duration accessors.  Probe A
//    in the plan §10 confirmed Hinnant's algorithm matches absl
//    bit-for-bit across this grid; this is the e2e pin of that.
// ──────────────────────────────────────────────────────────────

enum class TsAccessor {
  kYear,
  kFullYear,  // cel-cpp alias for kYear
  kMonth,
  kDate,         // 1-based day-of-month
  kDayOfMonth,   // 0-based day-of-month
  kDayOfYear,
  kDayOfWeek,
  kHours,
  kMinutes,
  kSeconds,
  kMilliseconds,
};

struct TsAccessorCase {
  std::string label;
  TsAccessor accessor;
  int64_t ts_seconds;
  int32_t ts_nanos;
  int64_t expected;
};

class UtcAccessorE2ETest : public ::testing::TestWithParam<TsAccessorCase> {};

TEST_P(UtcAccessorE2ETest, ProjectField) {
  GTEST_SKIP() << "M7B.C not yet shipped — pure-wasm UTC accessor "
                  "helpers + cel_civil_from_seconds are stubs.";
}

INSTANTIATE_TEST_SUITE_P(
    QuirkGrid, UtcAccessorE2ETest,
    ::testing::Values(
        // ── Epoch baseline (1970-01-01T00:00:00Z) ──
        TsAccessorCase{"Epoch_Year", TsAccessor::kYear, 0, 0, 1970},
        TsAccessorCase{"Epoch_Month", TsAccessor::kMonth, 0, 0, 0},
        TsAccessorCase{"Epoch_Date", TsAccessor::kDate, 0, 0, 1},
        TsAccessorCase{"Epoch_DayOfMonth", TsAccessor::kDayOfMonth, 0, 0, 0},
        TsAccessorCase{"Epoch_DayOfYear", TsAccessor::kDayOfYear, 0, 0, 0},
        TsAccessorCase{"Epoch_DayOfWeek", TsAccessor::kDayOfWeek, 0, 0, 4},
        TsAccessorCase{"Epoch_Hours", TsAccessor::kHours, 0, 0, 0},
        TsAccessorCase{"Epoch_Minutes", TsAccessor::kMinutes, 0, 0, 0},
        TsAccessorCase{"Epoch_Seconds", TsAccessor::kSeconds, 0, 0, 0},
        TsAccessorCase{"Epoch_Millis", TsAccessor::kMilliseconds, 0,
                       123'456'789, 123},
        // ── 1969-12-31T23:59:59Z (negative epoch) ──
        TsAccessorCase{"PreEpoch_Year", TsAccessor::kYear, -1, 0, 1969},
        TsAccessorCase{"PreEpoch_Month", TsAccessor::kMonth, -1, 0, 11},
        TsAccessorCase{"PreEpoch_Date", TsAccessor::kDate, -1, 0, 31},
        TsAccessorCase{"PreEpoch_Seconds", TsAccessor::kSeconds, -1, 0, 59},
        // ── 2000-01-01T00:00:00Z (leap-year-div-400) ──
        TsAccessorCase{"Y2K_Year", TsAccessor::kYear, 946684800, 0, 2000},
        TsAccessorCase{"Y2K_FullYear", TsAccessor::kFullYear, 946684800, 0,
                       2000},
        TsAccessorCase{"Y2K_DayOfYear", TsAccessor::kDayOfYear, 946684800, 0,
                       0},
        // ── 2024-02-29T00:00:00Z (Feb 29 leap year) ──
        TsAccessorCase{"LeapFeb29_Date", TsAccessor::kDate, 1709164800, 0, 29},
        TsAccessorCase{"LeapFeb29_Month", TsAccessor::kMonth, 1709164800, 0, 1},
        // ── 2024-12-31T23:59:59Z (last day of leap year) ──
        TsAccessorCase{"LeapEnd_DayOfYear", TsAccessor::kDayOfYear, 1735689599,
                       0, 365},
        // ── 2023-12-31T23:59:59Z (last day of non-leap year) ──
        TsAccessorCase{"NonLeapEnd_DayOfYear", TsAccessor::kDayOfYear,
                       1704067199, 0, 364},
        // ── 2009-02-13T23:31:30Z (langdef worked example) ──
        TsAccessorCase{"Langdef_Year", TsAccessor::kYear, 1234567890, 0, 2009},
        TsAccessorCase{"Langdef_Month", TsAccessor::kMonth, 1234567890, 0, 1},
        TsAccessorCase{"Langdef_Date", TsAccessor::kDate, 1234567890, 0, 13},
        TsAccessorCase{"Langdef_Hours", TsAccessor::kHours, 1234567890, 0, 23},
        TsAccessorCase{"Langdef_Minutes", TsAccessor::kMinutes, 1234567890, 0,
                       31},
        TsAccessorCase{"Langdef_Seconds", TsAccessor::kSeconds, 1234567890, 0,
                       30},
        // ── 9999-12-31T23:59:59Z (langdef upper bound) ──
        TsAccessorCase{"LangdefMax_Year", TsAccessor::kYear, 253402300799LL, 0,
                       9999},
        TsAccessorCase{"LangdefMax_Month", TsAccessor::kMonth, 253402300799LL,
                       0, 11},
        TsAccessorCase{"LangdefMax_Date", TsAccessor::kDate, 253402300799LL, 0,
                       31},
        // ── 0001-01-01T00:00:00Z (langdef lower bound) ──
        TsAccessorCase{"LangdefMin_Year", TsAccessor::kYear, -62135596800LL, 0,
                       1},
        TsAccessorCase{"LangdefMin_Month", TsAccessor::kMonth, -62135596800LL,
                       0, 0},
        TsAccessorCase{"LangdefMin_Date", TsAccessor::kDate, -62135596800LL, 0,
                       1},
        // ── 2038-01-19T03:14:07Z (Y2038 sanity — we're int64) ──
        TsAccessorCase{"Y2038_Year", TsAccessor::kYear, 2147483647, 0, 2038},
        TsAccessorCase{"Y2038_Date", TsAccessor::kDate, 2147483647, 0, 19}),
    [](const ::testing::TestParamInfo<TsAccessorCase>& info) {
      return info.param.label;
    });

// ── Duration accessors (4 helpers; truncating int division) ──

enum class DurAccessor {
  kHours,
  kMinutes,
  kSeconds,
  kMilliseconds,
};

struct DurAccessorCase {
  std::string label;
  DurAccessor accessor;
  int64_t dur_seconds;
  int32_t dur_nanos;
  int64_t expected;
};

class DurationAccessorE2ETest
    : public ::testing::TestWithParam<DurAccessorCase> {};

TEST_P(DurationAccessorE2ETest, ProjectField) {
  GTEST_SKIP() << "M7B.C not yet shipped — duration accessor helpers "
                  "(cel_dur_hours / minutes / seconds / milliseconds) "
                  "are stubs.";
}

INSTANTIATE_TEST_SUITE_P(
    TruncatingDivision, DurationAccessorE2ETest,
    ::testing::Values(
        DurAccessorCase{"Zero_Hours", DurAccessor::kHours, 0, 0, 0},
        DurAccessorCase{"OneHour_Hours", DurAccessor::kHours, 3600, 0, 1},
        DurAccessorCase{"NinetyMin_Hours", DurAccessor::kHours, 5400, 0, 1},
        DurAccessorCase{"NegHour_Hours", DurAccessor::kHours, -3600, 0, -1},
        DurAccessorCase{"Zero_Minutes", DurAccessor::kMinutes, 0, 0, 0},
        DurAccessorCase{"Sixty_Minutes", DurAccessor::kMinutes, 60, 0, 1},
        DurAccessorCase{"Almost_Minutes", DurAccessor::kMinutes, 59, 999'999'999,
                        0},
        DurAccessorCase{"NegSixty_Minutes", DurAccessor::kMinutes, -60, 0, -1},
        DurAccessorCase{"Zero_Seconds", DurAccessor::kSeconds, 0, 0, 0},
        DurAccessorCase{"OneSec_Seconds", DurAccessor::kSeconds, 1, 0, 1},
        DurAccessorCase{"NinetyMs_Seconds", DurAccessor::kSeconds, 0,
                        900'000'000, 0},
        DurAccessorCase{"Zero_Millis", DurAccessor::kMilliseconds, 0, 0, 0},
        DurAccessorCase{"FiveHundredMs_Millis", DurAccessor::kMilliseconds, 0,
                        500'000'000, 500},
        DurAccessorCase{"OneSecFiveHundredMs_Millis", DurAccessor::kMilliseconds,
                        1, 500'000'000, 1500},
        DurAccessorCase{"NegHalf_Millis", DurAccessor::kMilliseconds, 0,
                        -500'000'000, -500}),
    [](const ::testing::TestParamInfo<DurAccessorCase>& info) {
      return info.param.label;
    });

// ──────────────────────────────────────────────────────────────
// 6. ParseFormatE2ETest  (M7B.D — host trampolines)
//
//    Admit / reject matrix per §6.2.  Parse: 4 host trampolines
//    (cel_timestamp_parse, cel_duration_parse, cel_timestamp_format,
//    cel_duration_format) + 12 overload-id light-ups
//    (string_to_*, int64_to_*, *_to_string, *_to_int64, identity
//    *_to_*).  Probe B/C in the plan §10 found drift between
//    `absl::ParseTime`/`ParseDuration` and the CEL admit-set; the
//    Layer-2 impls post-validate.
// ──────────────────────────────────────────────────────────────

struct ParseCase {
  std::string label;
  std::string source;   // CEL expression to compile.
  bool expect_admit;    // false → parse failure → CEL_ERROR
};

class ParseFormatE2ETest : public ::testing::TestWithParam<ParseCase> {};

TEST_P(ParseFormatE2ETest, AdmitOrReject) {
  GTEST_SKIP() << "M7B.D not yet shipped — cel_timestamp_parse / "
                  "cel_duration_parse host trampolines are stubs.";
}

INSTANTIATE_TEST_SUITE_P(
    TimestampParseAdmit, ParseFormatE2ETest,
    ::testing::Values(
        ParseCase{"TsAdmit_BaseUTC",
                  R"(timestamp("2009-02-13T23:31:30Z") == timestamp("2009-02-13T23:31:30Z"))",
                  true},
        ParseCase{"TsAdmit_FixedOffset",
                  R"(timestamp("2009-02-13T23:31:30+02:00") == timestamp("2009-02-13T21:31:30Z"))",
                  true},
        ParseCase{"TsAdmit_NegOffset",
                  R"(timestamp("2009-02-13T23:31:30-08:00") == timestamp("2009-02-14T07:31:30Z"))",
                  true},
        ParseCase{"TsAdmit_FullNanos",
                  R"(timestamp("2009-02-13T23:31:30.123456789Z").getMilliseconds() == 123)",
                  true},
        ParseCase{"TsAdmit_PartialFractional",
                  R"(timestamp("2009-02-13T23:31:30.5Z").getMilliseconds() == 500)",
                  true},
        ParseCase{"TsReject_MissingTZ",
                  R"(timestamp("2009-02-13T23:31:30"))", false},
        ParseCase{"TsReject_LowercaseZ",
                  R"(timestamp("2009-02-13T23:31:30z"))", false},
        ParseCase{"TsReject_YearOver",
                  R"(timestamp("10000-01-01T00:00:00Z"))", false},
        ParseCase{"TsReject_YearUnder",
                  R"(timestamp("0000-01-01T00:00:00Z"))", false},
        ParseCase{"TsReject_LeapSecond",
                  R"(timestamp("2016-12-31T23:59:60Z"))", false}),
    [](const ::testing::TestParamInfo<ParseCase>& info) {
      return info.param.label;
    });

INSTANTIATE_TEST_SUITE_P(
    DurationParseAdmit, ParseFormatE2ETest,
    ::testing::Values(
        ParseCase{"DurAdmit_IntSec",
                  R"(duration("3600s") == duration("3600s"))", true},
        ParseCase{"DurAdmit_NegSec",
                  R"(duration("-3600s") == duration("-3600s"))", true},
        ParseCase{"DurAdmit_Zero",
                  R"(duration("0s") == duration("0s"))", true},
        ParseCase{"DurAdmit_Million",
                  R"(duration("1000000s") == duration("1000000s"))", true},
        ParseCase{"DurAdmit_Fractional",
                  R"(duration("1.5s").getMilliseconds() == 1500)", true},
        ParseCase{"DurAdmit_NanosPrecision",
                  R"(duration("1.000000001s").getMilliseconds() == 1000)",
                  true},
        ParseCase{"DurAdmit_Compound",
                  R"(duration("1h2m3s") == duration("3723s"))", true},
        ParseCase{"DurAdmit_NegCompound",
                  R"(duration("-1h2m3s") == duration("-3723s"))", true},
        ParseCase{"DurAdmit_Millis",
                  R"(duration("500ms").getMilliseconds() == 500)", true},
        ParseCase{"DurAdmit_Micros",
                  R"(duration("-1us") == duration("-1us"))", true},
        ParseCase{"DurAdmit_Nanos",
                  R"(duration("100ns") == duration("100ns"))", true},
        ParseCase{"DurReject_Empty", R"(duration(""))", false},
        ParseCase{"DurReject_NoUnit", R"(duration("3600"))", false},
        ParseCase{"DurReject_UnknownUnit", R"(duration("3600x"))", false},
        ParseCase{"DurReject_WrongOrder", R"(duration("1s2h"))", false},
        ParseCase{"DurReject_TrailingGarbage", R"(duration("1s "))", false},
        ParseCase{"DurReject_Overflow",
                  R"(duration("9223372036854775808s"))", false}),
    [](const ::testing::TestParamInfo<ParseCase>& info) {
      return info.param.label;
    });

// Format / int-convert / identity overloads — separate fixture
// because the expression shape is different.
class FormatConvertE2ETest : public ::testing::Test {};

TEST_F(FormatConvertE2ETest, TimestampToString) {
  GTEST_SKIP() << "M7B.D not yet shipped — cel_timestamp_format "
                  "host trampoline is a stub.";
}

TEST_F(FormatConvertE2ETest, TimestampToStringNanos) {
  GTEST_SKIP() << "M7B.D not yet shipped — cel_timestamp_format "
                  "host trampoline is a stub.";
}

TEST_F(FormatConvertE2ETest, DurationToString) {
  GTEST_SKIP() << "M7B.D not yet shipped — cel_duration_format "
                  "host trampoline is a stub.";
}

TEST_F(FormatConvertE2ETest, TimestampToInt64) {
  GTEST_SKIP() << "M7B.D not yet shipped — cel_ts_to_int_at_v not "
                  "registered.";
}

TEST_F(FormatConvertE2ETest, DurationToInt64) {
  GTEST_SKIP() << "M7B.D not yet shipped — cel_dur_to_int_at_v not "
                  "registered.";
}

TEST_F(FormatConvertE2ETest, Int64ToTimestamp) {
  GTEST_SKIP() << "M7B.D not yet shipped — cel_int_to_ts_at_v not "
                  "registered.";
}

TEST_F(FormatConvertE2ETest, Int64ToDuration) {
  GTEST_SKIP() << "M7B.D not yet shipped — cel_int_to_dur_at_v not "
                  "registered.";
}

TEST_F(FormatConvertE2ETest, TimestampIdentity) {
  GTEST_SKIP() << "M7B.D not yet shipped — timestamp_to_timestamp "
                  "identity not registered.";
}

TEST_F(FormatConvertE2ETest, DurationIdentity) {
  GTEST_SKIP() << "M7B.D not yet shipped — duration_to_duration "
                  "identity not registered.";
}

TEST_F(FormatConvertE2ETest, Int64ToTimestampOverflow) {
  // langdef: timestamp seconds range is bounded; int(INT64_MAX) →
  // timestamp must overflow.
  GTEST_SKIP() << "M7B.D not yet shipped — overflow guard on "
                  "int64_to_timestamp.";
}

// ──────────────────────────────────────────────────────────────
// 7. TzAccessorE2ETest  (M7B.E — with-TZ accessor trampoline)
//
//    Single host trampoline `cel_host.cel_timestamp_tz_accessor(out,
//    ts, tz, accessor_kind)` absorbs all 10 with-TZ overloads.  The
//    `accessor_kind` u32 immediate is the closed enum in §4.3 of
//    the plan.
// ──────────────────────────────────────────────────────────────

struct TzCase {
  std::string label;
  TsAccessor accessor;
  std::string tz;        // CEL string literal contents (no quotes).
  int64_t ts_seconds;
  int64_t expected;      // -1 = expect runtime error
  bool expect_error;
};

class TzAccessorE2ETest : public ::testing::TestWithParam<TzCase> {};

TEST_P(TzAccessorE2ETest, IanaOrFixedOffset) {
  GTEST_SKIP() << "M7B.E not yet shipped — cel_timestamp_tz_accessor "
                  "host trampoline is a stub.";
}

INSTANTIATE_TEST_SUITE_P(
    TzGrid, TzAccessorE2ETest,
    ::testing::Values(
        // IANA name — accessor in that zone.  2009-02-13T23:31:30Z
        // is 2009-02-13T15:31:30 in LA (PST, UTC-8).
        TzCase{"IanaLA_Hours", TsAccessor::kHours, "America/Los_Angeles",
               1234567890, 15, false},
        TzCase{"IanaLA_Date", TsAccessor::kDate, "America/Los_Angeles",
               1234567890, 13, false},
        TzCase{"IanaUTC_Hours", TsAccessor::kHours, "UTC", 1234567890, 23,
               false},
        TzCase{"IanaSydney_Date", TsAccessor::kDate, "Australia/Sydney",
               1234567890, 14, false},
        // Fixed offset
        TzCase{"FixedPlus2_Hours", TsAccessor::kHours, "+02:00", 1234567890, 1,
               false},
        TzCase{"FixedMinus8_Hours", TsAccessor::kHours, "-08:00", 1234567890,
               15, false},
        TzCase{"FixedZero_Hours", TsAccessor::kHours, "+00:00", 1234567890, 23,
               false},
        // Invalid TZ — runtime error
        TzCase{"InvalidName", TsAccessor::kYear, "NotARealZone", 1234567890, 0,
               true},
        TzCase{"InvalidEmpty", TsAccessor::kYear, "", 1234567890, 0, true},
        TzCase{"InvalidOffset", TsAccessor::kYear, "+25:00", 1234567890, 0,
               true}),
    [](const ::testing::TestParamInfo<TzCase>& info) {
      return info.param.label;
    });

// ──────────────────────────────────────────────────────────────
// 8. CrossFormEquivalenceE2ETest  (§3.4 — read normaliser bridge)
//
//    `timestamp("1970-01-01T00:00:01Z") == Timestamp{seconds: 1}`
//    — cross-form equivalence between M7's kStructExpr arm and
//    M7B's parse arm.  Routes through M7B.A's well-known-type
//    read normaliser + the cel_equals_at_vv kind-then-payload
//    memcmp.  The §6.5 NaN-not-equal regression pin also lives
//    here (integers, never NaN).
// ──────────────────────────────────────────────────────────────

class CrossFormEquivalenceE2ETest : public ::testing::Test {};

TEST_F(CrossFormEquivalenceE2ETest, DurationConstructEquality) {
  GTEST_SKIP() << "M7B.D not yet shipped — duration(string) needed "
                  "to construct both sides.";
}

TEST_F(CrossFormEquivalenceE2ETest, TimestampConstructEquality) {
  GTEST_SKIP() << "M7B.D not yet shipped — timestamp(string) needed.";
}

TEST_F(CrossFormEquivalenceE2ETest, DurationVsProtoLiteral) {
  // duration("1s") == Duration{seconds: 1}
  GTEST_SKIP() << "M7B.A+D not yet shipped — needs both well-known-"
                  "type read normaliser + duration parse.";
}

TEST_F(CrossFormEquivalenceE2ETest, TimestampVsProtoLiteral) {
  // timestamp("1970-01-01T00:00:01Z") == Timestamp{seconds: 1}
  GTEST_SKIP() << "M7B.A+D not yet shipped — needs both well-known-"
                  "type read normaliser + timestamp parse.";
}

TEST_F(CrossFormEquivalenceE2ETest, OrderingAcrossForms) {
  // timestamp("1970-01-01T00:00:01Z") < Timestamp{seconds: 2}
  GTEST_SKIP() << "M7B.A+B+D not yet shipped — needs all of "
                  "well-known-type normaliser + ordering + parse.";
}

TEST_F(CrossFormEquivalenceE2ETest, CanonicalisationEqualsAcrossUnits) {
  // duration("1s") == duration("1000ms")
  GTEST_SKIP() << "M7B.D not yet shipped — duration parse needed.";
}

TEST_F(CrossFormEquivalenceE2ETest, NoNaNRegression) {
  // Per §6.5: durations / timestamps are integer pairs.  The IEEE-
  // style NaN-not-equal-to-self pattern does NOT apply.  Pin
  // explicitly: `dur('1s') == dur('1s')` is true.
  GTEST_SKIP() << "M7B.D not yet shipped — duration parse needed.";
}

// ──────────────────────────────────────────────────────────────
// 9. TypeRegressionE2ETest  (§6.6 — type(...) on timestamps / durations)
//
//    `type(timestamp(...))` must return the well-known-type identity
//    (`google.protobuf.Timestamp`).  Already pinned at M9 for shipped
//    kinds; M7B is what makes the inputs constructible.  Three
//    timestamps conformance rows graduate when this lights up.
// ──────────────────────────────────────────────────────────────

class TypeRegressionE2ETest : public ::testing::Test {};

TEST_F(TypeRegressionE2ETest, TypeOfTimestamp) {
  GTEST_SKIP() << "M7B.D not yet shipped — timestamp(string) needed.";
}

TEST_F(TypeRegressionE2ETest, TypeOfDuration) {
  GTEST_SKIP() << "M7B.D not yet shipped — duration(string) needed.";
}

TEST_F(TypeRegressionE2ETest, TypeComparisonTimestamp) {
  // `google.protobuf.Timestamp == type(timestamp("..."))`
  GTEST_SKIP() << "M7B.D not yet shipped — timestamp(string) needed.";
}

TEST_F(TypeRegressionE2ETest, TypeComparisonDuration) {
  GTEST_SKIP() << "M7B.D not yet shipped — duration(string) needed.";
}

// ──────────────────────────────────────────────────────────────
// 10. RejectE2ETest  (§6.8 — comprehensive rejection matrix)
//
//     Cross-cutting rejection coverage.  Some rows are checker-side
//     (compile fails), some are runtime-side (CEL_ERROR in the
//     output).  Distinguishing the two is load-bearing per
//     CLAUDE.md "Cover the edge-case matrix".
// ──────────────────────────────────────────────────────────────

class RejectE2ETest : public ::testing::Test {};

TEST_F(RejectE2ETest, DurationParseError) {
  GTEST_SKIP() << "M7B.D not yet shipped — duration parse error "
                  "path is a stub.";
}

TEST_F(RejectE2ETest, TimestampParseError) {
  GTEST_SKIP() << "M7B.D not yet shipped — timestamp parse error "
                  "path is a stub.";
}

TEST_F(RejectE2ETest, TimestampPlusTimestampCheckerReject) {
  GTEST_SKIP() << "M7B.B not yet shipped — pin checker (not codegen) "
                  "rejection of ts+ts.";
}

TEST_F(RejectE2ETest, TimestampPlusIntCheckerReject) {
  // No overload exists for ts + int.
  GTEST_SKIP() << "M7B.B not yet shipped — pin checker rejection of "
                  "ts+int (no overload).";
}

TEST_F(RejectE2ETest, TzAccessorWrongArityCheckerReject) {
  GTEST_SKIP() << "M7B.E not yet shipped — pin checker rejection of "
                  "ts.getYear(tz, extra).";
}

TEST_F(RejectE2ETest, TzAccessorInvalidNameRuntimeError) {
  GTEST_SKIP() << "M7B.E not yet shipped — runtime CEL_ERROR on "
                  "unloadable IANA name.";
}

TEST_F(RejectE2ETest, Int64ToTimestampOverflow) {
  GTEST_SKIP() << "M7B.D not yet shipped — int(INT64_MAX) → timestamp "
                  "is out of the langdef Timestamp range.";
}

TEST_F(RejectE2ETest, DescriptorMismatchOnFieldRead) {
  // If a field's descriptor is `google.protobuf.Timestamp` but the
  // bound message backing is the wrong type, the normaliser must
  // surface CEL_ERROR (kInvalidArgument), not silently miscompile.
  GTEST_SKIP() << "M7B.A not yet shipped — descriptor-mismatch arm "
                  "of CelGetFieldImpl normaliser is a stub.";
}

}  // namespace
}  // namespace cel
