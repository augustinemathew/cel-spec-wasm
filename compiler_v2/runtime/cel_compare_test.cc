#include "compiler_v2/runtime/cel_compare.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

// M5.B comparison helper coverage.  The bulk of the matrix
// (per-kind eq/ne/lt/le/gt/ge happy/false pairs) consolidates
// into a single TEST_P table; spec-citation cases (IEEE NaN /
// Inf, INT64 / UINT64 boundaries, null semantics, type-mismatch,
// 3VL absorption) stay as focused TEST_F so a future reader sees
// "this asserts THIS spec rule" at a glance.

namespace celwasm {
namespace {

class CompareTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
  }

  uint32_t MakeOut() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }
  const CelValue* At(uint32_t slot) {
    return cel_value_at(slot);
  }
  bool ReadBool(uint32_t slot) {
    EXPECT_EQ(At(slot)->kind, static_cast<uint32_t>(CEL_BOOL));
    return At(slot)->payload.b != 0;
  }
};

// ── Parameterized matrix: per-kind same-kind happy/false pairs ──
//
// Covers the eq/ne/lt/le/gt/ge happy paths for int / uint /
// double / bool.  Each row carries the helper + factories that
// build the operands + expected bool result.  Boundary values
// (INT64_MIN/MAX, UINT64_MAX) and NaN/Inf get their own focused
// TEST_F below — the matrix here is the "no surprises" path.

struct CmpCase {
  const char* name;
  void (*helper)(uint32_t, uint32_t, uint32_t);
  uint32_t (*a)();
  uint32_t (*b)();
  bool expected;
};

class SameKindCmpTest : public CompareTest,
                        public ::testing::WithParamInterface<CmpCase> {};

TEST_P(SameKindCmpTest, ProducesExpectedBool) {
  const CmpCase& c = GetParam();
  uint32_t out = MakeOut();
  c.helper(out, c.a(), c.b());
  EXPECT_EQ(ReadBool(out), c.expected) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Int, SameKindCmpTest,
    ::testing::Values(CmpCase{"int_eq_true", cel_int_eq_at_vv,
                              +[]() {
                                return cel_make_int(3);
                              },
                              +[]() {
                                return cel_make_int(3);
                              },
                              true},
                      CmpCase{"int_eq_false", cel_int_eq_at_vv,
                              +[]() {
                                return cel_make_int(3);
                              },
                              +[]() {
                                return cel_make_int(4);
                              },
                              false},
                      CmpCase{"int_ne_true", cel_int_ne_at_vv,
                              +[]() {
                                return cel_make_int(3);
                              },
                              +[]() {
                                return cel_make_int(4);
                              },
                              true},
                      CmpCase{"int_ne_false", cel_int_ne_at_vv,
                              +[]() {
                                return cel_make_int(3);
                              },
                              +[]() {
                                return cel_make_int(3);
                              },
                              false},
                      CmpCase{"int_lt_true", cel_int_lt_at_vv,
                              +[]() {
                                return cel_make_int(1);
                              },
                              +[]() {
                                return cel_make_int(2);
                              },
                              true},
                      CmpCase{"int_le_eq", cel_int_le_at_vv,
                              +[]() {
                                return cel_make_int(2);
                              },
                              +[]() {
                                return cel_make_int(2);
                              },
                              true},
                      CmpCase{"int_gt_true", cel_int_gt_at_vv,
                              +[]() {
                                return cel_make_int(3);
                              },
                              +[]() {
                                return cel_make_int(2);
                              },
                              true},
                      CmpCase{"int_ge_eq", cel_int_ge_at_vv,
                              +[]() {
                                return cel_make_int(2);
                              },
                              +[]() {
                                return cel_make_int(2);
                              },
                              true}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    Uint, SameKindCmpTest,
    ::testing::Values(CmpCase{"uint_lt_true", cel_uint_lt_at_vv,
                              +[]() {
                                return cel_make_uint(1);
                              },
                              +[]() {
                                return cel_make_uint(2);
                              },
                              true},
                      CmpCase{"uint_le_eq", cel_uint_le_at_vv,
                              +[]() {
                                return cel_make_uint(2);
                              },
                              +[]() {
                                return cel_make_uint(2);
                              },
                              true},
                      CmpCase{"uint_gt_true", cel_uint_gt_at_vv,
                              +[]() {
                                return cel_make_uint(3);
                              },
                              +[]() {
                                return cel_make_uint(2);
                              },
                              true},
                      CmpCase{"uint_ge_max", cel_uint_ge_at_vv,
                              +[]() {
                                return cel_make_uint(UINT64_MAX);
                              },
                              +[]() {
                                return cel_make_uint(0);
                              },
                              true},
                      CmpCase{"uint_eq_max", cel_uint_eq_at_vv,
                              +[]() {
                                return cel_make_uint(UINT64_MAX);
                              },
                              +[]() {
                                return cel_make_uint(UINT64_MAX);
                              },
                              true}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    Double, SameKindCmpTest,
    ::testing::Values(CmpCase{"double_eq_true", cel_double_eq_at_vv,
                              +[]() {
                                return cel_make_double(1.5);
                              },
                              +[]() {
                                return cel_make_double(1.5);
                              },
                              true},
                      CmpCase{"double_eq_false", cel_double_eq_at_vv,
                              +[]() {
                                return cel_make_double(1.5);
                              },
                              +[]() {
                                return cel_make_double(2.5);
                              },
                              false}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    BoolFamily, SameKindCmpTest,
    ::testing::Values(CmpCase{"bool_eq_true", cel_bool_eq_at_vv,
                              +[]() {
                                return cel_make_bool(1);
                              },
                              +[]() {
                                return cel_make_bool(1);
                              },
                              true},
                      CmpCase{"bool_eq_false", cel_bool_eq_at_vv,
                              +[]() {
                                return cel_make_bool(1);
                              },
                              +[]() {
                                return cel_make_bool(0);
                              },
                              false},
                      CmpCase{"bool_ne", cel_bool_ne_at_vv,
                              +[]() {
                                return cel_make_bool(0);
                              },
                              +[]() {
                                return cel_make_bool(1);
                              },
                              true},
                      // langdef §"Booleans": false < true.  Spot-check each
                      // ordering op at both directions.
                      CmpCase{"bool_lt_false_true", cel_bool_lt_at_vv,
                              +[]() {
                                return cel_make_bool(0);
                              },
                              +[]() {
                                return cel_make_bool(1);
                              },
                              true},
                      CmpCase{"bool_lt_true_false", cel_bool_lt_at_vv,
                              +[]() {
                                return cel_make_bool(1);
                              },
                              +[]() {
                                return cel_make_bool(0);
                              },
                              false},
                      CmpCase{"bool_le_eq", cel_bool_le_at_vv,
                              +[]() {
                                return cel_make_bool(1);
                              },
                              +[]() {
                                return cel_make_bool(1);
                              },
                              true},
                      CmpCase{"bool_le_strictly_lt", cel_bool_le_at_vv,
                              +[]() {
                                return cel_make_bool(0);
                              },
                              +[]() {
                                return cel_make_bool(1);
                              },
                              true},
                      CmpCase{"bool_gt_true_false", cel_bool_gt_at_vv,
                              +[]() {
                                return cel_make_bool(1);
                              },
                              +[]() {
                                return cel_make_bool(0);
                              },
                              true},
                      CmpCase{"bool_gt_false_true", cel_bool_gt_at_vv,
                              +[]() {
                                return cel_make_bool(0);
                              },
                              +[]() {
                                return cel_make_bool(1);
                              },
                              false},
                      CmpCase{"bool_ge_eq", cel_bool_ge_at_vv,
                              +[]() {
                                return cel_make_bool(0);
                              },
                              +[]() {
                                return cel_make_bool(0);
                              },
                              true},
                      CmpCase{"bool_ge_strictly_gt", cel_bool_ge_at_vv,
                              +[]() {
                                return cel_make_bool(1);
                              },
                              +[]() {
                                return cel_make_bool(0);
                              },
                              true}),
    [](const auto& info) {
      return info.param.name;
    });

// ── Cross-kind numeric ladder (M5.B step 2) ───────────────────
//
// `cel_numeric_<op>_at_vv` accepts any combination of {INT, UINT,
// DOUBLE} on either operand.  The matrix below covers each
// cross-kind pair (int↔uint, int↔double, uint↔double) at every
// op.  Same-kind pairs stay covered by the per-kind helpers above
// — but the kernel handles them too so we still drop one same-
// kind row per op as a tripwire that the dispatch hasn't lost
// the trivial path.

class CrossKindCmpTest : public CompareTest,
                         public ::testing::WithParamInterface<CmpCase> {};

TEST_P(CrossKindCmpTest, ProducesExpectedBool) {
  const CmpCase& c = GetParam();
  uint32_t out = MakeOut();
  c.helper(out, c.a(), c.b());
  EXPECT_EQ(ReadBool(out), c.expected) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Lt, CrossKindCmpTest,
    ::testing::Values(
        // int vs uint.
        CmpCase{"lt_int_neg_lt_uint", cel_numeric_lt_at_vv,
                +[]() {
                  return cel_make_int(-1);
                },
                +[]() {
                  return cel_make_uint(0);
                },
                true},
        CmpCase{"lt_int_pos_eq_uint_false", cel_numeric_lt_at_vv,
                +[]() {
                  return cel_make_int(5);
                },
                +[]() {
                  return cel_make_uint(5);
                },
                false},
        CmpCase{"lt_uint_gt_int_neg_false", cel_numeric_lt_at_vv,
                +[]() {
                  return cel_make_uint(0);
                },
                +[]() {
                  return cel_make_int(-1);
                },
                false},
        // int vs double.
        CmpCase{"lt_int_lt_double_value", cel_numeric_lt_at_vv,
                +[]() {
                  return cel_make_int(2);
                },
                +[]() {
                  return cel_make_double(2.5);
                },
                true},
        CmpCase{"lt_double_lt_int_value", cel_numeric_lt_at_vv,
                +[]() {
                  return cel_make_double(1.5);
                },
                +[]() {
                  return cel_make_int(2);
                },
                true},
        // uint vs double.
        CmpCase{"lt_double_neg_lt_uint", cel_numeric_lt_at_vv,
                +[]() {
                  return cel_make_double(-1.0);
                },
                +[]() {
                  return cel_make_uint(0);
                },
                true},
        CmpCase{"lt_uint_lt_double_above_max", cel_numeric_lt_at_vv,
                +[]() {
                  return cel_make_uint(UINT64_MAX);
                },
                +[]() {
                  return cel_make_double(1e30);
                },
                true},
        // same-kind tripwire: kernel must still answer correctly.
        CmpCase{"lt_int_int_same_kind", cel_numeric_lt_at_vv,
                +[]() {
                  return cel_make_int(1);
                },
                +[]() {
                  return cel_make_int(2);
                },
                true}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    Le, CrossKindCmpTest,
    ::testing::Values(CmpCase{"le_int_neg_le_uint", cel_numeric_le_at_vv,
                              +[]() {
                                return cel_make_int(-1);
                              },
                              +[]() {
                                return cel_make_uint(0);
                              },
                              true},
                      CmpCase{"le_int_pos_eq_uint_true", cel_numeric_le_at_vv,
                              +[]() {
                                return cel_make_int(5);
                              },
                              +[]() {
                                return cel_make_uint(5);
                              },
                              true},
                      CmpCase{"le_int_eq_double", cel_numeric_le_at_vv,
                              +[]() {
                                return cel_make_int(3);
                              },
                              +[]() {
                                return cel_make_double(3.0);
                              },
                              true},
                      CmpCase{"le_double_lt_uint_zero", cel_numeric_le_at_vv,
                              +[]() {
                                return cel_make_double(-0.5);
                              },
                              +[]() {
                                return cel_make_uint(0);
                              },
                              true},
                      CmpCase{"le_uint_lt_double", cel_numeric_le_at_vv,
                              +[]() {
                                return cel_make_uint(2);
                              },
                              +[]() {
                                return cel_make_double(2.5);
                              },
                              true},
                      CmpCase{"le_uint_gt_double_neg_false",
                              cel_numeric_le_at_vv,
                              +[]() {
                                return cel_make_uint(0);
                              },
                              +[]() {
                                return cel_make_double(-1.0);
                              },
                              false},
                      CmpCase{"le_same_kind_tripwire", cel_numeric_le_at_vv,
                              +[]() {
                                return cel_make_double(1.0);
                              },
                              +[]() {
                                return cel_make_double(1.0);
                              },
                              true}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    Gt, CrossKindCmpTest,
    ::testing::Values(CmpCase{"gt_uint_gt_int_neg", cel_numeric_gt_at_vv,
                              +[]() {
                                return cel_make_uint(0);
                              },
                              +[]() {
                                return cel_make_int(-1);
                              },
                              true},
                      CmpCase{"gt_int_neg_gt_uint_false", cel_numeric_gt_at_vv,
                              +[]() {
                                return cel_make_int(-1);
                              },
                              +[]() {
                                return cel_make_uint(0);
                              },
                              false},
                      CmpCase{"gt_double_gt_int", cel_numeric_gt_at_vv,
                              +[]() {
                                return cel_make_double(2.5);
                              },
                              +[]() {
                                return cel_make_int(2);
                              },
                              true},
                      CmpCase{"gt_int_gt_double", cel_numeric_gt_at_vv,
                              +[]() {
                                return cel_make_int(3);
                              },
                              +[]() {
                                return cel_make_double(2.5);
                              },
                              true},
                      CmpCase{"gt_double_above_uintmax_gt_uint",
                              cel_numeric_gt_at_vv,
                              +[]() {
                                return cel_make_double(1e30);
                              },
                              +[]() {
                                return cel_make_uint(UINT64_MAX);
                              },
                              true},
                      CmpCase{"gt_uint_gt_double_neg", cel_numeric_gt_at_vv,
                              +[]() {
                                return cel_make_uint(0);
                              },
                              +[]() {
                                return cel_make_double(-1.0);
                              },
                              true},
                      CmpCase{"gt_same_kind_tripwire", cel_numeric_gt_at_vv,
                              +[]() {
                                return cel_make_uint(2);
                              },
                              +[]() {
                                return cel_make_uint(1);
                              },
                              true}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    Ge, CrossKindCmpTest,
    ::testing::Values(CmpCase{"ge_uint_ge_int_eq", cel_numeric_ge_at_vv,
                              +[]() {
                                return cel_make_uint(5);
                              },
                              +[]() {
                                return cel_make_int(5);
                              },
                              true},
                      CmpCase{"ge_int_neg_ge_uint_false", cel_numeric_ge_at_vv,
                              +[]() {
                                return cel_make_int(-1);
                              },
                              +[]() {
                                return cel_make_uint(0);
                              },
                              false},
                      CmpCase{"ge_int_eq_double", cel_numeric_ge_at_vv,
                              +[]() {
                                return cel_make_int(3);
                              },
                              +[]() {
                                return cel_make_double(3.0);
                              },
                              true},
                      CmpCase{"ge_uint_ge_double_neg", cel_numeric_ge_at_vv,
                              +[]() {
                                return cel_make_uint(0);
                              },
                              +[]() {
                                return cel_make_double(-1.0);
                              },
                              true},
                      CmpCase{"ge_double_lt_uint_false", cel_numeric_ge_at_vv,
                              +[]() {
                                return cel_make_double(-0.5);
                              },
                              +[]() {
                                return cel_make_uint(0);
                              },
                              false},
                      CmpCase{"ge_same_kind_tripwire", cel_numeric_ge_at_vv,
                              +[]() {
                                return cel_make_int(2);
                              },
                              +[]() {
                                return cel_make_int(2);
                              },
                              true}),
    [](const auto& info) {
      return info.param.name;
    });

// Eq / ne wrappers exist (header) but the polymorphic `equals`
// dispatcher seeding lands in M5.B step 2b after M5.D step 2 ships
// aggregate eq.  We still cover the kernel directly so a future
// regression to the same-kind eq path surfaces here, not in
// downstream codegen.
INSTANTIATE_TEST_SUITE_P(
    EqNe, CrossKindCmpTest,
    ::testing::Values(CmpCase{"eq_int_pos_eq_uint", cel_numeric_eq_at_vv,
                              +[]() {
                                return cel_make_int(5);
                              },
                              +[]() {
                                return cel_make_uint(5);
                              },
                              true},
                      CmpCase{"eq_int_neg_eq_uint_false", cel_numeric_eq_at_vv,
                              +[]() {
                                return cel_make_int(-1);
                              },
                              +[]() {
                                return cel_make_uint(0);
                              },
                              false},
                      CmpCase{"eq_int_eq_double", cel_numeric_eq_at_vv,
                              +[]() {
                                return cel_make_int(3);
                              },
                              +[]() {
                                return cel_make_double(3.0);
                              },
                              true},
                      CmpCase{"ne_int_eq_uint_false", cel_numeric_ne_at_vv,
                              +[]() {
                                return cel_make_int(5);
                              },
                              +[]() {
                                return cel_make_uint(5);
                              },
                              false},
                      CmpCase{"ne_int_uneq_double_true", cel_numeric_ne_at_vv,
                              +[]() {
                                return cel_make_int(3);
                              },
                              +[]() {
                                return cel_make_double(2.5);
                              },
                              true}),
    [](const auto& info) {
      return info.param.name;
    });

// ── Spec-citation focused tests (TEST_F) ──────────────────────

TEST_F(CompareTest, IntBoundaryValues) {
  // INT64_MIN / INT64_MAX must compare equal to themselves and
  // strictly ordered against each other — locks the i64 ladder
  // bounds the helpers run on.
  uint32_t out = MakeOut();
  cel_int_eq_at_vv(out, cel_make_int(INT64_MIN), cel_make_int(INT64_MIN));
  EXPECT_TRUE(ReadBool(out));
  cel_int_eq_at_vv(out, cel_make_int(INT64_MAX), cel_make_int(INT64_MAX));
  EXPECT_TRUE(ReadBool(out));
  cel_int_lt_at_vv(out, cel_make_int(INT64_MIN), cel_make_int(INT64_MAX));
  EXPECT_TRUE(ReadBool(out));
}

TEST_F(CompareTest, DoubleNanComparesUnequal) {
  // IEEE 754: NaN != NaN.  Every comparison involving NaN returns
  // false — including `==` — except `!=`, which is true.  cel-cpp
  // inherits this directly from the C operators.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  uint32_t out = MakeOut();
  cel_double_eq_at_vv(out, cel_make_double(nan), cel_make_double(nan));
  EXPECT_FALSE(ReadBool(out));
  cel_double_lt_at_vv(out, cel_make_double(nan), cel_make_double(0.0));
  EXPECT_FALSE(ReadBool(out));
  cel_double_gt_at_vv(out, cel_make_double(nan), cel_make_double(0.0));
  EXPECT_FALSE(ReadBool(out));
  cel_double_ne_at_vv(out, cel_make_double(nan), cel_make_double(nan));
  EXPECT_TRUE(ReadBool(out));
}

TEST_F(CompareTest, DoubleInfOrdering) {
  const double inf = std::numeric_limits<double>::infinity();
  uint32_t out = MakeOut();
  cel_double_lt_at_vv(out, cel_make_double(-inf), cel_make_double(0.0));
  EXPECT_TRUE(ReadBool(out));
  cel_double_gt_at_vv(out, cel_make_double(inf), cel_make_double(1e300));
  EXPECT_TRUE(ReadBool(out));
  cel_double_eq_at_vv(out, cel_make_double(inf), cel_make_double(inf));
  EXPECT_TRUE(ReadBool(out));
}

TEST_F(CompareTest, NullEqAlwaysTrue) {
  // langdef: `null == null → true` is the only operation defined
  // on the null type.
  uint32_t out = MakeOut();
  cel_null_eq_at_vv(out, cel_make_null(), cel_make_null());
  EXPECT_TRUE(ReadBool(out));
}

TEST_F(CompareTest, NullEqWithNonNullPoisons) {
  uint32_t out = MakeOut();
  cel_null_eq_at_vv(out, cel_make_null(), cel_make_int(0));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(CompareTest, IntEqTypeMismatchPoisons) {
  // Cross-kind operands (int vs uint) must reject — the cross-
  // type numeric ladder lives in a separate `cel_numeric_*` set
  // (M5.B step 2), not in the same-kind helpers.
  uint32_t out = MakeOut();
  cel_int_eq_at_vv(out, cel_make_int(1), cel_make_uint(1));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

// 3VL absorption — the shared `absorb_3vl_binary` is used by every
// cmp helper; spot-check on one int_eq path with each operand
// position.

TEST_F(CompareTest, ErrorOperandPropagates) {
  uint32_t err_off = arena_alloc(sizeof(CelValue));
  CelValue* err = cel_value_at(err_off);
  err->kind = CEL_ERROR;
  err->payload.err = CEL_ERR_OVERFLOW;
  uint32_t out = MakeOut();
  cel_int_eq_at_vv(out, err_off, cel_make_int(0));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

// ── Cross-type ladder spec-citation cases (M5.B step 2) ──────

// langdef: "any comparison involving NaN returns false".  Differs
// from IEEE for the `!=` operator (which is true for NaN); cel-cpp
// matches langdef in `runtime/standard/equality_functions.cc`.
// Verifies every wrapper answers false when either operand is NaN,
// regardless of cross-kind dispatch.
TEST_F(CompareTest, NumericNanOrderingComparesFalse) {
  // NaN against any value yields false for every ordering operator
  // (langdef §"Comparisons" — "any [ordering] comparison involving
  // NaN returns false").  Equality / inequality have asymmetric
  // semantics; covered separately in `NumericNanEqualityFollowsIeee`.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  uint32_t out = MakeOut();
  cel_numeric_lt_at_vv(out, cel_make_int(0), cel_make_double(nan));
  EXPECT_FALSE(ReadBool(out));
  cel_numeric_le_at_vv(out, cel_make_uint(0), cel_make_double(nan));
  EXPECT_FALSE(ReadBool(out));
  cel_numeric_gt_at_vv(out, cel_make_double(nan), cel_make_int(0));
  EXPECT_FALSE(ReadBool(out));
  cel_numeric_ge_at_vv(out, cel_make_double(nan), cel_make_uint(0));
  EXPECT_FALSE(ReadBool(out));
}

// Slice 1.55 (2026-04-25): cel-cpp's `Inequal<double>` defaults to
// the IEEE `lhs != rhs` semantic — `NaN != NaN` is TRUE, and every
// NaN-touching inequality is true.  v2 previously returned false on
// `kCmpNanInequal`; this test pins the post-fix behaviour.
TEST_F(CompareTest, NumericNanEqualityFollowsIeee) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  uint32_t out = MakeOut();

  // `==` on any NaN-touching pair returns FALSE — including
  // self-comparison (NaN is never equal to anything).
  cel_numeric_eq_at_vv(out, cel_make_double(nan), cel_make_double(nan));
  EXPECT_FALSE(ReadBool(out));
  cel_numeric_eq_at_vv(out, cel_make_double(nan), cel_make_int(0));
  EXPECT_FALSE(ReadBool(out));

  // `!=` is the complement: every NaN-touching pair returns TRUE,
  // matching IEEE 754 and cel-cpp's
  // `Inequal<double>(double, double)` (the C `!=` operator).
  cel_numeric_ne_at_vv(out, cel_make_double(nan), cel_make_double(nan));
  EXPECT_TRUE(ReadBool(out));
  cel_numeric_ne_at_vv(out, cel_make_double(nan), cel_make_int(0));
  EXPECT_TRUE(ReadBool(out));
  cel_numeric_ne_at_vv(out, cel_make_int(0), cel_make_double(nan));
  EXPECT_TRUE(ReadBool(out));
  cel_numeric_ne_at_vv(out, cel_make_uint(0), cel_make_double(nan));
  EXPECT_TRUE(ReadBool(out));
}

// Regression guard for non-NaN inequalities — the kernel's tri-state
// dispatch must keep cross-numeric `!=` working when neither operand
// is NaN.  `Slice 1.55` flipped to `r != kCmpEqual` which absorbs
// `kCmpNanInequal` AND `kCmpLess` AND `kCmpGreater`; all three must
// still produce true.
TEST_F(CompareTest, NumericNeCrossNumericNonNanIsTrue) {
  uint32_t out = MakeOut();
  cel_numeric_ne_at_vv(out, cel_make_int(1), cel_make_uint(2));
  EXPECT_TRUE(ReadBool(out));
  cel_numeric_ne_at_vv(out, cel_make_int(2), cel_make_double(1.5));
  EXPECT_TRUE(ReadBool(out));
  // And a true-equality must STILL return false.
  cel_numeric_ne_at_vv(out, cel_make_int(1), cel_make_double(1.0));
  EXPECT_FALSE(ReadBool(out));
}

// cel-cpp `internal/number.h:41` pins INT64_MAX / INT64_MIN as the
// boundary doubles for the int-vs-double ladder: a double strictly
// above kInt64Max-as-double is greater than every int64; below
// kInt64Min-as-double is lesser.  (double)INT64_MAX itself is
// representable in double (it's a power of two), so `< (double)max`
// must answer `<` for the strict op.  The ladder MUST consult the
// boundary check before any narrowing cast — otherwise INT64_MAX
// rounds up to (double)INT64_MAX during the cast and we miscompile.
TEST_F(CompareTest, NumericIntDoubleBoundary) {
  uint32_t out = MakeOut();
  // (double)INT64_MAX is exactly representable; INT64_MAX's int
  // value vs the same number as a double compares equal.
  cel_numeric_eq_at_vv(out, cel_make_int(INT64_MAX),
                       cel_make_double((double)INT64_MAX));
  EXPECT_TRUE(ReadBool(out));
  // double strictly > kInt64Max: every int64 is less.
  cel_numeric_lt_at_vv(out, cel_make_int(INT64_MAX), cel_make_double(1e30));
  EXPECT_TRUE(ReadBool(out));
  cel_numeric_gt_at_vv(out, cel_make_double(1e30), cel_make_int(INT64_MAX));
  EXPECT_TRUE(ReadBool(out));
  // double strictly < kInt64Min: every int64 is greater.
  cel_numeric_gt_at_vv(out, cel_make_int(INT64_MIN), cel_make_double(-1e30));
  EXPECT_TRUE(ReadBool(out));
  cel_numeric_lt_at_vv(out, cel_make_double(-1e30), cel_make_int(INT64_MIN));
  EXPECT_TRUE(ReadBool(out));
}

// cel-cpp `internal/number.h:42`: kDoubleToUintMax pins the
// uint↔double upper boundary; values ≥ that double are greater
// than every uint64; negative doubles are less than every uint64.
// The boundary value 1.84e19 ≈ kUint64Max isn't exactly
// representable in double, but a double strictly > UINT64_MAX
// must answer "double greater" without aliasing.
TEST_F(CompareTest, NumericUintDoubleBoundary) {
  uint32_t out = MakeOut();
  cel_numeric_lt_at_vv(out, cel_make_uint(UINT64_MAX), cel_make_double(1e30));
  EXPECT_TRUE(ReadBool(out));
  cel_numeric_gt_at_vv(out, cel_make_double(1e30), cel_make_uint(UINT64_MAX));
  EXPECT_TRUE(ReadBool(out));
  cel_numeric_lt_at_vv(out, cel_make_double(-0.5), cel_make_uint(0));
  EXPECT_TRUE(ReadBool(out));
  cel_numeric_gt_at_vv(out, cel_make_uint(0), cel_make_double(-0.5));
  EXPECT_TRUE(ReadBool(out));
}

// Type-mismatch matrix: any non-numeric kind on either operand
// must poison.  The kernel itself dispatches on kind; we verify
// the prelude rejects strings / bools / null before the kernel
// runs.  This is the cross-type analog of the same-kind helpers'
// `IntEqTypeMismatchPoisons` test.
TEST_F(CompareTest, NumericRejectsNonNumericOperands) {
  uint32_t out = MakeOut();
  cel_numeric_lt_at_vv(out, cel_make_int(0), cel_make_bool(0));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
  cel_numeric_gt_at_vv(out, cel_make_string("a", 1), cel_make_int(0));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
  cel_numeric_eq_at_vv(out, cel_make_null(), cel_make_double(0.0));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(CompareTest, NumericAbsorbs3VL) {
  uint32_t err_off = arena_alloc(sizeof(CelValue));
  CelValue* err = cel_value_at(err_off);
  err->kind = CEL_ERROR;
  err->payload.err = CEL_ERR_OVERFLOW;
  uint32_t out = MakeOut();
  cel_numeric_lt_at_vv(out, err_off, cel_make_int(0));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(CompareTest, UnknownOperandPropagates) {
  uint32_t unk_off = arena_alloc(sizeof(CelValue));
  CelValue* unk = cel_value_at(unk_off);
  unk->kind = CEL_UNKNOWN;
  unk->payload.unk = 99;
  uint32_t out = MakeOut();
  cel_int_eq_at_vv(out, cel_make_int(0), unk_off);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(At(out)->payload.unk, 99u);
}

}  // namespace
}  // namespace celwasm
