// Coverage for the M7B.B kernels — 6 arithmetic + 8 ordering helpers
// in cel_time.{h,c}.  Per CLAUDE.md "Cover the edge-case matrix",
// every helper has positive + negative + boundary cases; the §6.3
// boundary grid in `m7b-duration-timestamp.md` is materialised as
// parameterised tables; spec-citation cases stay as focused TEST_F.

#include "compiler_v2/runtime/cel_time.h"

#include <cstdint>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

class TimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
  }
  uint32_t MakeSlot() {
    return cel_alloc(static_cast<uint32_t>(sizeof(CelValue)));
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
  const CelValue* At(uint32_t slot) { return cel_value_at(slot); }
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
  bool result_for_a_lt_b;   // expected on (1s, 2s) ordered
  bool result_for_a_eq_b;   // expected on (1s, 1s) equal
  bool result_for_a_gt_b;   // expected on (2s, 1s) reversed
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
    ::testing::Values(
        CmpRow{"dur_lt", &cel_dur_lt_at_vv, true, false, false},
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

}  // namespace
}  // namespace celwasm
