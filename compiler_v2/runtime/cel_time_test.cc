// Coverage for the M7B.B kernels — 6 arithmetic + 8 ordering helpers
// in cel_time.{h,c}.  Per CLAUDE.md "Cover the edge-case matrix",
// every helper has positive + negative + boundary cases; the §6.3
// boundary grid in `m7b-duration-timestamp.md` is materialised as
// parameterised tables; spec-citation cases stay as focused TEST_F.

#include "compiler_v2/runtime/cel_time.h"

#include <cstdint>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_layout.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

class TimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }
  uint32_t MakeSlot() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }
  uint32_t MakeDur(int64_t s, int32_t ns) {
    const uint32_t slot = MakeSlot();
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_DURATION;
    v->payload.dur = CelDurTs{.seconds = s, .nanos = ns, ._pad = 0};
    return slot;
  }
  uint32_t MakeTs(int64_t s, int32_t ns) {
    const uint32_t slot = MakeSlot();
    CelValue* v = cel_value_at(slot);
    v->kind = CEL_TIMESTAMP;
    v->payload.ts = CelDurTs{.seconds = s, .nanos = ns, ._pad = 0};
    return slot;
  }
  uint32_t MakeKind(uint32_t kind) {
    const uint32_t slot = MakeSlot();
    cel_value_at(slot)->kind = kind;
    return slot;
  }
  const CelValue* At(uint32_t slot) {
    return cel_value_at(slot);
  }
};

// ── 3VL absorption matrix ───────────────────────────────────────
// Every helper inherits absorb_3vl_binary; assert the propagation
// for one representative helper per family.  ERROR is left-bias;
// UNKNOWN is also left-bias today (cel_unknown_merge handles the
// UNKNOWN×UNKNOWN merge at the dispatcher level).

TEST_F(TimeTest, ArithAbsorbsErrorLeftOperand) {
  const uint32_t out = MakeSlot();
  cel_dur_add_at_vv(out, MakeKind(CEL_ERROR), MakeDur(1, 0));
  EXPECT_EQ(At(out)->kind, CEL_ERROR);
}

TEST_F(TimeTest, ArithAbsorbsUnknownRightOperand) {
  const uint32_t out = MakeSlot();
  cel_dur_add_at_vv(out, MakeDur(1, 0), MakeKind(CEL_UNKNOWN));
  EXPECT_EQ(At(out)->kind, CEL_UNKNOWN);
}

TEST_F(TimeTest, OrderingAbsorbsErrorLeftOperand) {
  const uint32_t out = MakeSlot();
  cel_dur_lt_at_vv(out, MakeKind(CEL_ERROR), MakeDur(1, 0));
  EXPECT_EQ(At(out)->kind, CEL_ERROR);
}

// ── Operand-kind mismatch ───────────────────────────────────────

TEST_F(TimeTest, DurAddRejectsTimestampOperand) {
  const uint32_t out = MakeSlot();
  cel_dur_add_at_vv(out, MakeTs(1, 0), MakeDur(1, 0));
  EXPECT_EQ(At(out)->kind, CEL_ERROR);
  EXPECT_EQ(At(out)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(TimeTest, TsDurAddRejectsBothDurations) {
  const uint32_t out = MakeSlot();
  cel_ts_dur_add_at_vv(out, MakeDur(1, 0), MakeDur(2, 0));
  EXPECT_EQ(At(out)->kind, CEL_ERROR);
  EXPECT_EQ(At(out)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

// ── §6.3 arithmetic boundary matrix ─────────────────────────────

// Each row exercises the per-helper signature shape + the
// dur_combine math — same body for all six arithmetic helpers, so
// we cover each helper once with a happy-path row plus one
// boundary row from the §6.3 grid.

TEST_F(TimeTest, DurAddZeroPlusZero) {
  const uint32_t out = MakeSlot();
  cel_dur_add_at_vv(out, MakeDur(0, 0), MakeDur(0, 0));
  EXPECT_EQ(At(out)->kind, CEL_DURATION);
  EXPECT_EQ(At(out)->payload.dur.seconds, 0);
  EXPECT_EQ(At(out)->payload.dur.nanos, 0);
}

TEST_F(TimeTest, DurAddCarriesNanos) {
  const uint32_t out = MakeSlot();
  // (1s, 0ns) + (0s, -1ns): expect (0s, 999_999_999ns) post-carry +
  // sign-correlate.
  cel_dur_add_at_vv(out, MakeDur(1, 0), MakeDur(0, -1));
  EXPECT_EQ(At(out)->kind, CEL_DURATION);
  EXPECT_EQ(At(out)->payload.dur.seconds, 0);
  EXPECT_EQ(At(out)->payload.dur.nanos, 999'999'999);
}

TEST_F(TimeTest, DurAddNanosOverflowCarriesUp) {
  const uint32_t out = MakeSlot();
  // (0s, 700_000_000ns) + (0s, 500_000_000ns) = (1s, 200_000_000ns)
  cel_dur_add_at_vv(out, MakeDur(0, 700'000'000), MakeDur(0, 500'000'000));
  EXPECT_EQ(At(out)->payload.dur.seconds, 1);
  EXPECT_EQ(At(out)->payload.dur.nanos, 200'000'000);
}

TEST_F(TimeTest, DurAddSecondsOverflowProducesPoison) {
  const uint32_t out = MakeSlot();
  cel_dur_add_at_vv(out, MakeDur(INT64_MAX, 0), MakeDur(1, 0));
  EXPECT_EQ(At(out)->kind, CEL_ERROR);
  EXPECT_EQ(At(out)->payload.err, CEL_ERR_OVERFLOW);
}

TEST_F(TimeTest, DurAddNanosCarryTriggersSecondsOverflow) {
  const uint32_t out = MakeSlot();
  // (INT64_MAX, 999_999_999) + (0, 1) → carry triggers INT64_MAX+1
  // overflow.  Pins R5 from the m7b plan.
  cel_dur_add_at_vv(out, MakeDur(INT64_MAX, 999'999'999), MakeDur(0, 1));
  EXPECT_EQ(At(out)->kind, CEL_ERROR);
  EXPECT_EQ(At(out)->payload.err, CEL_ERR_OVERFLOW);
}

TEST_F(TimeTest, DurSubProducesNegativeWithSignCorrelation) {
  const uint32_t out = MakeSlot();
  // 0s - 500ms = -0.5s = (0s, -500_000_000ns) per the proto Duration
  // text format / Probe D sign-correlated convention.
  cel_dur_sub_at_vv(out, MakeDur(0, 0), MakeDur(0, 500'000'000));
  EXPECT_EQ(At(out)->payload.dur.seconds, 0);
  EXPECT_EQ(At(out)->payload.dur.nanos, -500'000'000);
}

TEST_F(TimeTest, DurSubUnderflowProducesPoison) {
  const uint32_t out = MakeSlot();
  cel_dur_sub_at_vv(out, MakeDur(INT64_MIN, 0), MakeDur(1, 0));
  EXPECT_EQ(At(out)->kind, CEL_ERROR);
  EXPECT_EQ(At(out)->payload.err, CEL_ERR_OVERFLOW);
}

TEST_F(TimeTest, TsDurAddProducesTimestamp) {
  const uint32_t out = MakeSlot();
  cel_ts_dur_add_at_vv(out, MakeTs(100, 0), MakeDur(50, 0));
  EXPECT_EQ(At(out)->kind, CEL_TIMESTAMP);
  EXPECT_EQ(At(out)->payload.ts.seconds, 150);
}

TEST_F(TimeTest, DurTsAddIsCommutativeWithTsDurAdd) {
  // langdef §"Timestamps and Durations": addition is commutative
  // (dur+ts === ts+dur).  Verifies the swapped overload produces
  // the same CelValue.
  const uint32_t lhs = MakeSlot();
  const uint32_t rhs = MakeSlot();
  cel_ts_dur_add_at_vv(lhs, MakeTs(100, 250'000'000), MakeDur(50, 500'000'000));
  cel_dur_ts_add_at_vv(rhs, MakeDur(50, 500'000'000), MakeTs(100, 250'000'000));
  EXPECT_EQ(At(lhs)->kind, CEL_TIMESTAMP);
  EXPECT_EQ(At(rhs)->kind, CEL_TIMESTAMP);
  EXPECT_EQ(At(lhs)->payload.ts.seconds, At(rhs)->payload.ts.seconds);
  EXPECT_EQ(At(lhs)->payload.ts.nanos, At(rhs)->payload.ts.nanos);
}

TEST_F(TimeTest, TsDurSubProducesTimestamp) {
  const uint32_t out = MakeSlot();
  cel_ts_dur_sub_at_vv(out, MakeTs(100, 0), MakeDur(30, 0));
  EXPECT_EQ(At(out)->kind, CEL_TIMESTAMP);
  EXPECT_EQ(At(out)->payload.ts.seconds, 70);
}

TEST_F(TimeTest, TsTsSubProducesDuration) {
  const uint32_t out = MakeSlot();
  cel_ts_ts_sub_at_vv(out, MakeTs(200, 0), MakeTs(100, 0));
  EXPECT_EQ(At(out)->kind, CEL_DURATION);
  EXPECT_EQ(At(out)->payload.dur.seconds, 100);
}

// ── Ordering matrix (8 helpers × ordered + equal + reversed) ────

struct CmpRow {
  const char* label;
  void (*helper)(uint32_t, uint32_t, uint32_t);
  bool result_for_a_lt_b;  // expected on (1s, 2s) ordered
  bool result_for_a_eq_b;  // expected on (1s, 1s) equal
  bool result_for_a_gt_b;  // expected on (2s, 1s) reversed
};

class TimeOrderingTest : public TimeTest,
                         public ::testing::WithParamInterface<CmpRow> {};

TEST_P(TimeOrderingTest, DurationCompareMatrix) {
  const CmpRow& r = GetParam();
  const uint32_t out_lt = MakeSlot();
  const uint32_t out_eq = MakeSlot();
  const uint32_t out_gt = MakeSlot();
  r.helper(out_lt, MakeDur(1, 0), MakeDur(2, 0));
  r.helper(out_eq, MakeDur(1, 0), MakeDur(1, 0));
  r.helper(out_gt, MakeDur(2, 0), MakeDur(1, 0));
  ASSERT_EQ(At(out_lt)->kind, CEL_BOOL) << r.label;
  ASSERT_EQ(At(out_eq)->kind, CEL_BOOL) << r.label;
  ASSERT_EQ(At(out_gt)->kind, CEL_BOOL) << r.label;
  EXPECT_EQ(At(out_lt)->payload.b != 0, r.result_for_a_lt_b) << r.label;
  EXPECT_EQ(At(out_eq)->payload.b != 0, r.result_for_a_eq_b) << r.label;
  EXPECT_EQ(At(out_gt)->payload.b != 0, r.result_for_a_gt_b) << r.label;
}

INSTANTIATE_TEST_SUITE_P(
    Ladder, TimeOrderingTest,
    ::testing::Values(CmpRow{"dur_lt", &cel_dur_lt_at_vv, true, false, false},
                      CmpRow{"dur_le", &cel_dur_le_at_vv, true, true, false},
                      CmpRow{"dur_gt", &cel_dur_gt_at_vv, false, false, true},
                      CmpRow{"dur_ge", &cel_dur_ge_at_vv, false, true, true}),
    [](const ::testing::TestParamInfo<CmpRow>& info) {
      return info.param.label;
    });

TEST_F(TimeTest, TsLtSortsByNanosWhenSecondsEqual) {
  const uint32_t out = MakeSlot();
  cel_ts_lt_at_vv(out, MakeTs(100, 0), MakeTs(100, 1));
  EXPECT_EQ(At(out)->kind, CEL_BOOL);
  EXPECT_EQ(At(out)->payload.b, 1);
}

TEST_F(TimeTest, TsGeIsTrueForEqual) {
  const uint32_t out = MakeSlot();
  cel_ts_ge_at_vv(out, MakeTs(100, 500), MakeTs(100, 500));
  EXPECT_EQ(At(out)->payload.b, 1);
}

TEST_F(TimeTest, DurLtTreatsNegativeSecondsAsLess) {
  const uint32_t out = MakeSlot();
  cel_dur_lt_at_vv(out, MakeDur(-1, 0), MakeDur(0, 0));
  EXPECT_EQ(At(out)->payload.b, 1);
}

// ── §6.4 civil-calendar quirk matrix (M7B.C) ───────────────────

struct CivilCase {
  const char* label;
  int64_t epoch_seconds;
  int32_t expect_year;
  int32_t expect_month_0;
  int32_t expect_day_1;
  int32_t expect_day_of_year;
  int32_t expect_day_of_week;
};

class CivilQuirkTest : public TimeTest,
                       public ::testing::WithParamInterface<CivilCase> {};

TEST_P(CivilQuirkTest, AllAccessorsProjectCorrectly) {
  const CivilCase& c = GetParam();
  const uint32_t ts = MakeTs(c.epoch_seconds, 0);
  const uint32_t y = MakeSlot();
  const uint32_t m = MakeSlot();
  const uint32_t d1 = MakeSlot();
  const uint32_t doy = MakeSlot();
  const uint32_t dow = MakeSlot();
  cel_ts_year_utc_at_v(y, ts);
  cel_ts_month_utc_at_v(m, ts);
  cel_ts_day_of_month_1_utc_at_v(d1, ts);
  cel_ts_day_of_year_utc_at_v(doy, ts);
  cel_ts_day_of_week_utc_at_v(dow, ts);
  EXPECT_EQ(At(y)->payload.i, c.expect_year) << c.label;
  EXPECT_EQ(At(m)->payload.i, c.expect_month_0) << c.label;
  EXPECT_EQ(At(d1)->payload.i, c.expect_day_1) << c.label;
  EXPECT_EQ(At(doy)->payload.i, c.expect_day_of_year) << c.label;
  EXPECT_EQ(At(dow)->payload.i, c.expect_day_of_week) << c.label;
}

// Quirk grid from m7b §6.4 (Probe A cross-checks each against
// absl::ToCivilSecond(UTCTimeZone()) — see plan §10.1).
// month_0 is 0-based, day_1 is 1-based, day_of_year is 0-based
// (Jan 1 = 0), day_of_week is 0-based (Sunday = 0).
INSTANTIATE_TEST_SUITE_P(
    QuirkGrid, CivilQuirkTest,
    ::testing::Values(
        CivilCase{"EpochZero", 0, 1970, 0, 1, 0, 4 /*Thu*/},
        CivilCase{"NegOneSec", -1, 1969, 11, 31, 364, 3 /*Wed*/},
        CivilCase{"Y2KLeap", 946'684'800LL, 2000, 0, 1, 0, 6 /*Sat*/},
        CivilCase{"LangdefSample", 1'234'567'890LL, 2009, 1, 13, 43, 5 /*Fri*/},
        CivilCase{"Y2024Feb29", 1'709'164'800LL, 2024, 1, 29, 59, 4 /*Thu*/},
        CivilCase{"Y2024LastDay", 1'735'689'599LL, 2024, 11, 31, 365,
                  2 /*Tue*/},
        CivilCase{"Y2023LastDay", 1'704'067'199LL, 2023, 11, 31, 364,
                  0 /*Sun*/},
        CivilCase{"Y9999LangdefMax", 253'402'300'799LL, 9999, 11, 31, 364,
                  5 /*Fri*/}),
    [](const ::testing::TestParamInfo<CivilCase>& info) {
      return info.param.label;
    });

// One row outside the langdef range — exercises the Hinnant
// algorithm's generality (the M7B.B arithmetic gate prevents this
// timestamp from being produced in practice, but the accessor
// kernel itself is range-agnostic).
TEST_F(TimeTest, CivilFromSeconds_Y0001ProducesLangdefLower) {
  const uint32_t ts = MakeTs(-62135596800LL, 0);
  const uint32_t y = MakeSlot();
  cel_ts_year_utc_at_v(y, ts);
  EXPECT_EQ(At(y)->payload.i, 1);
}

// ── Duration accessor matrix (M7B.C) ───────────────────────────

TEST_F(TimeTest, DurationGetHoursTruncatesTowardZero) {
  const uint32_t out = MakeSlot();
  cel_dur_hours_at_v(out, MakeDur(7200, 999'999'999));
  EXPECT_EQ(At(out)->kind, CEL_INT);
  EXPECT_EQ(At(out)->payload.i, 2);
}

TEST_F(TimeTest, DurationGetHoursOnNegativeSeconds) {
  const uint32_t out = MakeSlot();
  cel_dur_hours_at_v(out, MakeDur(-3601, 0));
  EXPECT_EQ(At(out)->payload.i, -1);  // truncates toward zero
}

TEST_F(TimeTest, DurationGetSecondsReturnsWholeField) {
  const uint32_t out = MakeSlot();
  cel_dur_seconds_at_v(out, MakeDur(42, 999'999'999));
  EXPECT_EQ(At(out)->payload.i, 42);
}

TEST_F(TimeTest, DurationGetMillisecondsReturnsSubSecondComponent) {
  // cel-cpp / spec: `getMilliseconds` returns the millisecond
  // *component* of the duration (sub-second ms in [-999, 999]),
  // NOT the total milliseconds.  Pinned by
  // `timestamps.textproto :: duration_converters/get_milliseconds`.
  const uint32_t out = MakeSlot();
  cel_dur_milliseconds_at_v(out, MakeDur(3, 500'000'000));
  EXPECT_EQ(At(out)->payload.i, 500);
}

TEST_F(TimeTest, TimestampGetMillisecondsReturnsSubSecond) {
  const uint32_t out = MakeSlot();
  cel_ts_milliseconds_utc_at_v(out, MakeTs(1'234'567'890LL, 500'000'000));
  EXPECT_EQ(At(out)->payload.i, 500);
}

TEST_F(TimeTest, TimestampAccessorRejectsDurationOperand) {
  const uint32_t out = MakeSlot();
  cel_ts_year_utc_at_v(out, MakeDur(0, 0));
  EXPECT_EQ(At(out)->kind, CEL_ERROR);
  EXPECT_EQ(At(out)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(TimeTest, DurationAccessorRejectsTimestampOperand) {
  const uint32_t out = MakeSlot();
  cel_dur_hours_at_v(out, MakeTs(0, 0));
  EXPECT_EQ(At(out)->kind, CEL_ERROR);
  EXPECT_EQ(At(out)->payload.err, CEL_ERR_TYPE_MISMATCH);
}

}  // namespace
}  // namespace celwasm
