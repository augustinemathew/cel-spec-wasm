#include "compiler_v2/runtime/cel_arith.h"

#include <cmath>
#include <cstdint>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_make.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

// M5.B arithmetic helper coverage.  Three parameterized tables
// cover the bulk:
//   - HappyPath: per-kind × per-op happy values.
//   - Overflow:  per-kind overflow boundaries (langdef pins ints
//                to ERROR; doubles to inf/nan via IEEE 754).
//   - DivByZero: int / uint div+mod by zero error codes.
// Spec-citation cases (INT64_MIN×−1, INT64_MIN%−1, IEEE NaN/Inf
// behaviour) stay as focused TEST_F so the spec source-of-truth
// is explicit at each assertion.

namespace celwasm {
namespace {

class ArithTest : public ::testing::Test {
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
};

// ── Parameterized: happy-path arithmetic per kind × per op ────

struct BinaryArithCase {
  const char* name;
  void (*helper)(uint32_t, uint32_t, uint32_t);
  uint32_t (*a)();
  uint32_t (*b)();
  uint32_t result_kind;
  // Result accessor: we read the right field of payload depending
  // on the kind.  Per-row this dispatches the assertion.
  int64_t (*read_int)(const CelValue*);    // populated for CEL_INT
  uint64_t (*read_uint)(const CelValue*);  // populated for CEL_UINT
  double (*read_double)(const CelValue*);  // populated for CEL_DOUBLE
  int64_t expected_int;
  uint64_t expected_uint;
  double expected_double;
};

class ArithHappyTest : public ArithTest,
                       public ::testing::WithParamInterface<BinaryArithCase> {};

TEST_P(ArithHappyTest, ProducesExpectedResult) {
  const BinaryArithCase& c = GetParam();
  uint32_t out = MakeOut();
  c.helper(out, c.a(), c.b());
  ASSERT_EQ(At(out)->kind, c.result_kind) << c.name;
  if (c.read_int) EXPECT_EQ(c.read_int(At(out)), c.expected_int) << c.name;
  if (c.read_uint) EXPECT_EQ(c.read_uint(At(out)), c.expected_uint) << c.name;
  if (c.read_double)
    EXPECT_DOUBLE_EQ(c.read_double(At(out)), c.expected_double) << c.name;
}

namespace {
int64_t I(const CelValue* v) {
  return v->payload.i;
}
uint64_t U(const CelValue* v) {
  return v->payload.u;
}
double D(const CelValue* v) {
  return v->payload.d;
}
}  // namespace

#define INT_CASE(name_, helper_, a_val, b_val, expected_) \
  BinaryArithCase{name_,                                  \
                  helper_,                                \
                  +[]() {                                 \
                    return cel_make_int(a_val);           \
                  },                                      \
                  +[]() {                                 \
                    return cel_make_int(b_val);           \
                  },                                      \
                  CEL_INT,                                \
                  I,                                      \
                  nullptr,                                \
                  nullptr,                                \
                  expected_,                              \
                  0,                                      \
                  0.0}
#define UINT_CASE(name_, helper_, a_val, b_val, expected_) \
  BinaryArithCase{name_,                                   \
                  helper_,                                 \
                  +[]() {                                  \
                    return cel_make_uint(a_val);           \
                  },                                       \
                  +[]() {                                  \
                    return cel_make_uint(b_val);           \
                  },                                       \
                  CEL_UINT,                                \
                  nullptr,                                 \
                  U,                                       \
                  nullptr,                                 \
                  0,                                       \
                  expected_,                               \
                  0.0}
#define DOUBLE_CASE(name_, helper_, a_val, b_val, expected_) \
  BinaryArithCase {                                          \
    name_, helper_,                                          \
        +[]() {                                              \
          return cel_make_double(a_val);                     \
        },                                                   \
        +[]() {                                              \
          return cel_make_double(b_val);                     \
        },                                                   \
        CEL_DOUBLE, nullptr, nullptr, D, 0, 0, expected_     \
  }

INSTANTIATE_TEST_SUITE_P(
    Int, ArithHappyTest,
    ::testing::Values(
        INT_CASE("int_add", cel_int_add_at_vv, 2, 3, 5),
        INT_CASE("int_sub", cel_int_sub_at_vv, 10, 4, 6),
        INT_CASE("int_mul_pos", cel_int_mul_at_vv, 7, 6, 42),
        INT_CASE("int_mul_neg_neg", cel_int_mul_at_vv, -3, -4, 12),
        INT_CASE("int_div_truncating", cel_int_div_at_vv, 20, 3, 6),
        INT_CASE("int_mod", cel_int_mod_at_vv, 20, 3, 2)),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    Uint, ArithHappyTest,
    ::testing::Values(UINT_CASE("uint_add", cel_uint_add_at_vv, 2u, 3u, 5u),
                      UINT_CASE("uint_mul", cel_uint_mul_at_vv, 7u, 6u, 42u),
                      UINT_CASE("uint_mul_max_by_one", cel_uint_mul_at_vv,
                                UINT64_MAX, 1u, UINT64_MAX)),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(Double, ArithHappyTest,
                         ::testing::Values(DOUBLE_CASE("double_add",
                                                       cel_double_add_at_vv,
                                                       1.5, 2.25, 3.75)),
                         [](const auto& info) {
                           return info.param.name;
                         });

// ── Parameterized: overflow / underflow boundary errors ───────

struct OverflowCase {
  const char* name;
  void (*helper)(uint32_t, uint32_t, uint32_t);
  uint32_t (*a)();
  uint32_t (*b)();
  uint32_t expected_err;
};

class ArithErrorTest : public ArithTest,
                       public ::testing::WithParamInterface<OverflowCase> {};

TEST_P(ArithErrorTest, PoisonsWithExpectedErrorCode) {
  const OverflowCase& c = GetParam();
  uint32_t out = MakeOut();
  c.helper(out, c.a(), c.b());
  ASSERT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR)) << c.name;
  EXPECT_EQ(At(out)->payload.err, c.expected_err) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Overflow, ArithErrorTest,
    ::testing::Values(
        OverflowCase{"int_add_max_plus_one", cel_int_add_at_vv,
                     +[]() {
                       return cel_make_int(INT64_MAX);
                     },
                     +[]() {
                       return cel_make_int(1);
                     },
                     CEL_ERR_OVERFLOW},
        OverflowCase{"int_add_min_plus_neg_one", cel_int_add_at_vv,
                     +[]() {
                       return cel_make_int(INT64_MIN);
                     },
                     +[]() {
                       return cel_make_int(-1);
                     },
                     CEL_ERR_OVERFLOW},
        OverflowCase{"int_sub_min_minus_one", cel_int_sub_at_vv,
                     +[]() {
                       return cel_make_int(INT64_MIN);
                     },
                     +[]() {
                       return cel_make_int(1);
                     },
                     CEL_ERR_OVERFLOW},
        OverflowCase{"int_mul_2pow40_squared", cel_int_mul_at_vv,
                     +[]() {
                       return cel_make_int(int64_t{1} << 40);
                     },
                     +[]() {
                       return cel_make_int(int64_t{1} << 40);
                     },
                     CEL_ERR_OVERFLOW},
        OverflowCase{"uint_add_max_plus_one", cel_uint_add_at_vv,
                     +[]() {
                       return cel_make_uint(UINT64_MAX);
                     },
                     +[]() {
                       return cel_make_uint(1);
                     },
                     CEL_ERR_OVERFLOW},
        OverflowCase{"uint_sub_zero_minus_one", cel_uint_sub_at_vv,
                     +[]() {
                       return cel_make_uint(0);
                     },
                     +[]() {
                       return cel_make_uint(1);
                     },
                     CEL_ERR_OVERFLOW},
        OverflowCase{"uint_mul_2pow40_squared", cel_uint_mul_at_vv,
                     +[]() {
                       return cel_make_uint(uint64_t{1} << 40);
                     },
                     +[]() {
                       return cel_make_uint(uint64_t{1} << 40);
                     },
                     CEL_ERR_OVERFLOW}),
    [](const auto& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    DivByZero, ArithErrorTest,
    ::testing::Values(OverflowCase{"int_div_zero", cel_int_div_at_vv,
                                   +[]() {
                                     return cel_make_int(1);
                                   },
                                   +[]() {
                                     return cel_make_int(0);
                                   },
                                   CEL_ERR_DIVIDE_BY_ZERO},
                      OverflowCase{"uint_div_zero", cel_uint_div_at_vv,
                                   +[]() {
                                     return cel_make_uint(1);
                                   },
                                   +[]() {
                                     return cel_make_uint(0);
                                   },
                                   CEL_ERR_DIVIDE_BY_ZERO},
                      OverflowCase{"int_mod_zero", cel_int_mod_at_vv,
                                   +[]() {
                                     return cel_make_int(5);
                                   },
                                   +[]() {
                                     return cel_make_int(0);
                                   },
                                   CEL_ERR_MODULUS_BY_ZERO},
                      OverflowCase{"uint_mod_zero", cel_uint_mod_at_vv,
                                   +[]() {
                                     return cel_make_uint(5);
                                   },
                                   +[]() {
                                     return cel_make_uint(0);
                                   },
                                   CEL_ERR_MODULUS_BY_ZERO}),
    [](const auto& info) {
      return info.param.name;
    });

// ── Spec-citation focused tests (TEST_F) ──────────────────────

TEST_F(ArithTest, IntMulIntMinTimesNegOneOverflows) {
  // INT64_MIN * -1 has no representation in two's complement — the
  // helper detects this special case explicitly because the
  // standard magnitude-bounds-check trick can't represent
  // |INT64_MIN| in i64.
  uint32_t out = MakeOut();
  cel_int_mul_at_vv(out, cel_make_int(INT64_MIN), cel_make_int(-1));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(ArithTest, IntMulIntMinTimesZeroIsZero) {
  // INT64_MIN * 0 = 0 (no overflow despite the |INT64_MIN| edge).
  uint32_t out = MakeOut();
  cel_int_mul_at_vv(out, cel_make_int(INT64_MIN), cel_make_int(0));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(At(out)->payload.i, 0);
}

TEST_F(ArithTest, IntDivIntMinByNegOnePoisons) {
  // INT64_MIN / -1 overflows in two's complement (result would be
  // INT64_MAX+1).  langdef pins overflow → ERROR; cel-cpp's helper
  // returns CEL_ERR_OVERFLOW and we mirror.
  uint32_t out = MakeOut();
  cel_int_div_at_vv(out, cel_make_int(INT64_MIN), cel_make_int(-1));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(ArithTest, IntModIntMinByNegOneIsZero) {
  // C's `%` is undefined for INT64_MIN % -1; cel-cpp returns 0
  // (the mathematically correct result) and we mirror.
  uint32_t out = MakeOut();
  cel_int_mod_at_vv(out, cel_make_int(INT64_MIN), cel_make_int(-1));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(At(out)->payload.i, 0);
}

TEST_F(ArithTest, IntNegHappyPath) {
  uint32_t out = MakeOut();
  cel_int_neg_at_v(out, cel_make_int(42));
  EXPECT_EQ(At(out)->payload.i, -42);
}

TEST_F(ArithTest, IntNegIntMinPoisons) {
  // -INT64_MIN doesn't fit in i64.
  uint32_t out = MakeOut();
  cel_int_neg_at_v(out, cel_make_int(INT64_MIN));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(ArithTest, IntAddTypeMismatchPoisons) {
  // Cross-kind operands reject — cross-type numeric arith routes
  // through the M5.B step 2 `cel_numeric_*` ladder, not these
  // same-kind helpers.
  uint32_t out = MakeOut();
  cel_int_add_at_vv(out, cel_make_int(1), cel_make_uint(1));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST_F(ArithTest, DoubleDivByZeroProducesInf) {
  // langdef: double follows IEEE 754; div-by-zero is NOT an error.
  uint32_t out = MakeOut();
  cel_double_div_at_vv(out, cel_make_double(1.0), cel_make_double(0.0));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_DOUBLE));
  EXPECT_TRUE(std::isinf(At(out)->payload.d));
}

TEST_F(ArithTest, DoubleZeroOverZeroProducesNaN) {
  uint32_t out = MakeOut();
  cel_double_div_at_vv(out, cel_make_double(0.0), cel_make_double(0.0));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_DOUBLE));
  EXPECT_TRUE(std::isnan(At(out)->payload.d));
}

TEST_F(ArithTest, DoubleNegHappyPath) {
  uint32_t out = MakeOut();
  cel_double_neg_at_v(out, cel_make_double(3.14));
  EXPECT_DOUBLE_EQ(At(out)->payload.d, -3.14);
}

// 3VL absorption — the shared envelope is exercised once per
// operand position (left / right) and once for the unary helper.
// Repeating per kind would only re-test the shared shape.

TEST_F(ArithTest, ErrorOnLeftPropagates) {
  uint32_t err_off = arena_alloc(sizeof(CelValue));
  CelValue* err = cel_value_at(err_off);
  err->kind = CEL_ERROR;
  err->payload.err = CEL_ERR_OVERFLOW;
  uint32_t out = MakeOut();
  cel_int_add_at_vv(out, err_off, cel_make_int(1));
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(ArithTest, ErrorOnRightPropagates) {
  uint32_t err_off = arena_alloc(sizeof(CelValue));
  CelValue* err = cel_value_at(err_off);
  err->kind = CEL_ERROR;
  err->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
  uint32_t out = MakeOut();
  cel_int_add_at_vv(out, cel_make_int(1), err_off);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

TEST_F(ArithTest, UnknownOperandPropagates) {
  uint32_t unk_off = arena_alloc(sizeof(CelValue));
  CelValue* unk = cel_value_at(unk_off);
  unk->kind = CEL_UNKNOWN;
  unk->payload.unk = 17;
  uint32_t out = MakeOut();
  cel_int_add_at_vv(out, cel_make_int(1), unk_off);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(At(out)->payload.unk, 17u);
}

TEST_F(ArithTest, UnaryAbsorbsError) {
  uint32_t err_off = arena_alloc(sizeof(CelValue));
  CelValue* err = cel_value_at(err_off);
  err->kind = CEL_ERROR;
  err->payload.err = CEL_ERR_TYPE_MISMATCH;
  uint32_t out = MakeOut();
  cel_int_neg_at_v(out, err_off);
  EXPECT_EQ(At(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(At(out)->payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

}  // namespace
}  // namespace celwasm
