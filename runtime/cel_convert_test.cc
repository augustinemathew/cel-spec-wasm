#include "runtime/cel_convert.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_make.h"
#include "runtime/cel_memory.h"
#include "gtest/gtest.h"

// M10 — conversion-kernel coverage.  Four sections, one per
// sub-milestone (B numeric inter-convert; C string parsing; D
// number/bool → string; E bytes ↔ string).  Each section follows the
// per-component-test-coverage discipline: positive (every overload),
// boundary (INT64_MIN/MAX, UINT64_MAX, NaN, ±Inf, ±0, empty string,
// embedded NUL), and negative (kind mismatch + 3VL absorb).  Each
// helper has its own TEST_F group; cases inside use parameterized
// matrices where structurally identical (string→bool truth table,
// numeric overflow boundary table) and individual TEST_Fs where the
// story differs (specific spec citation, alias-slot semantics).

namespace celwasm {
namespace {

class ConvertFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }

  uint32_t MakeOut() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }

  uint32_t MakeError() {
    uint32_t off = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(off);
    v->kind = CEL_ERROR;
    v->payload.err = CEL_ERR_DIVIDE_BY_ZERO;
    return off;
  }

  uint32_t MakeUnknown() {
    uint32_t off = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
    CelValue* v = cel_value_at(off);
    v->kind = CEL_UNKNOWN;
    v->payload.unk = 0u;
    return off;
  }

  // Assert out is {CEL_ERROR, err}.
  void ExpectError(uint32_t out, uint32_t err) {
    const CelValue* v = cel_value_at(out);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
    EXPECT_EQ(v->payload.err, err);
  }

  std::string StringAt(uint32_t slot) {
    const CelValue* v = cel_value_at(slot);
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
    const char* base = reinterpret_cast<const char*>(cel_mem_base());
    return {base + v->payload.s.ptr, v->payload.s.len};
  }
};

// ─────────────────────────────────────────────────────────────
// M10.B — numeric inter-conversion.
// ─────────────────────────────────────────────────────────────

using ConvertNumericTest = ConvertFixture;

// int(uint) — admits [0, INT64_MAX], rejects (INT64_MAX, UINT64_MAX].

TEST_F(ConvertNumericTest, UintToIntZero) {
  uint32_t out = MakeOut();
  cel_uint_to_int_at_v(out, cel_make_uint(0u));
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(v->payload.i, 0);
}

TEST_F(ConvertNumericTest, UintToIntMax) {
  uint32_t out = MakeOut();
  cel_uint_to_int_at_v(out, cel_make_uint(static_cast<uint64_t>(INT64_MAX)));
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(v->payload.i, INT64_MAX);
}

TEST_F(ConvertNumericTest, UintToIntOverflow) {
  uint32_t out = MakeOut();
  cel_uint_to_int_at_v(out,
                       cel_make_uint(static_cast<uint64_t>(INT64_MAX) + 1ULL));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertNumericTest, UintToIntUintMax) {
  uint32_t out = MakeOut();
  cel_uint_to_int_at_v(out, cel_make_uint(UINT64_MAX));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertNumericTest, UintToIntKindMismatch) {
  uint32_t out = MakeOut();
  cel_uint_to_int_at_v(out, cel_make_int(5));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

// double(int) / double(uint) — never errors, but can lose precision.

TEST_F(ConvertNumericTest, IntToDoubleExact) {
  uint32_t out = MakeOut();
  cel_int_to_double_at_v(out, cel_make_int(42));
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_DOUBLE));
  EXPECT_EQ(v->payload.d, 42.0);
}

TEST_F(ConvertNumericTest, IntToDoubleMin) {
  uint32_t out = MakeOut();
  cel_int_to_double_at_v(out, cel_make_int(INT64_MIN));
  const CelValue* v = cel_value_at(out);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_DOUBLE));
  EXPECT_EQ(v->payload.d, static_cast<double>(INT64_MIN));
}

TEST_F(ConvertNumericTest, UintToDoubleZero) {
  uint32_t out = MakeOut();
  cel_uint_to_double_at_v(out, cel_make_uint(0u));
  EXPECT_EQ(cel_value_at(out)->payload.d, 0.0);
}

TEST_F(ConvertNumericTest, UintToDoubleMax) {
  uint32_t out = MakeOut();
  cel_uint_to_double_at_v(out, cel_make_uint(UINT64_MAX));
  // 2^64 is the exact rounded value of UINT64_MAX-as-double.
  EXPECT_EQ(cel_value_at(out)->payload.d, static_cast<double>(UINT64_MAX));
}

// int(double) — admits [-2^63, 2^63), rejects NaN, ±Inf, |v| >= 2^63.

TEST_F(ConvertNumericTest, DoubleToIntZero) {
  uint32_t out = MakeOut();
  cel_double_to_int_at_v(out, cel_make_double(0.0));
  EXPECT_EQ(cel_value_at(out)->payload.i, 0);
}

TEST_F(ConvertNumericTest, DoubleToIntNegativeZero) {
  uint32_t out = MakeOut();
  cel_double_to_int_at_v(out, cel_make_double(-0.0));
  EXPECT_EQ(cel_value_at(out)->payload.i, 0);
}

TEST_F(ConvertNumericTest, DoubleToIntTruncatesTowardZero) {
  // Positive truncates down.
  uint32_t out1 = MakeOut();
  cel_double_to_int_at_v(out1, cel_make_double(3.7));
  EXPECT_EQ(cel_value_at(out1)->payload.i, 3);
  // Negative truncates up (toward zero).
  uint32_t out2 = MakeOut();
  cel_double_to_int_at_v(out2, cel_make_double(-3.7));
  EXPECT_EQ(cel_value_at(out2)->payload.i, -3);
}

TEST_F(ConvertNumericTest, DoubleToIntMinRejected) {
  // The double -2^63 is REJECTED as a range error — validated against the
  // real cel-cpp oracle (cel_cpp_oracle_test.cc IntFromDoubleMinIsRangeError)
  // and the conformance corpus (conversions.textproto: int(-9223372036854775808.0)
  // -> "range").  No double lies strictly between -2^63 and the next
  // representable value, so INT64_MIN is unreachable via int(double).
  uint32_t out = MakeOut();
  cel_double_to_int_at_v(out, cel_make_double(static_cast<double>(INT64_MIN)));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertNumericTest, DoubleToIntPositiveBoundaryRejected) {
  // 2^63 is the smallest double greater than INT64_MAX; rejected per
  // cel-cpp's CheckedDoubleToInt64.
  uint32_t out = MakeOut();
  cel_double_to_int_at_v(out, cel_make_double(9.223372036854775808e18));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertNumericTest, DoubleToIntNaNRejected) {
  uint32_t out = MakeOut();
  cel_double_to_int_at_v(out, cel_make_double(std::nan("")));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertNumericTest, DoubleToIntPosInfRejected) {
  uint32_t out = MakeOut();
  cel_double_to_int_at_v(
      out, cel_make_double(std::numeric_limits<double>::infinity()));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertNumericTest, DoubleToIntNegInfRejected) {
  uint32_t out = MakeOut();
  cel_double_to_int_at_v(
      out, cel_make_double(-std::numeric_limits<double>::infinity()));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

// uint(int) — admits [0, INT64_MAX], rejects (-inf, 0).

TEST_F(ConvertNumericTest, IntToUintZero) {
  uint32_t out = MakeOut();
  cel_int_to_uint_at_v(out, cel_make_int(0));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_UINT));
  EXPECT_EQ(cel_value_at(out)->payload.u, 0u);
}

TEST_F(ConvertNumericTest, IntToUintMax) {
  uint32_t out = MakeOut();
  cel_int_to_uint_at_v(out, cel_make_int(INT64_MAX));
  EXPECT_EQ(cel_value_at(out)->payload.u, static_cast<uint64_t>(INT64_MAX));
}

TEST_F(ConvertNumericTest, IntToUintNegativeRejected) {
  uint32_t out = MakeOut();
  cel_int_to_uint_at_v(out, cel_make_int(-1));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertNumericTest, IntToUintMinRejected) {
  uint32_t out = MakeOut();
  cel_int_to_uint_at_v(out, cel_make_int(INT64_MIN));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

// uint(double).

TEST_F(ConvertNumericTest, DoubleToUintInRange) {
  uint32_t out = MakeOut();
  cel_double_to_uint_at_v(out, cel_make_double(42.9));
  EXPECT_EQ(cel_value_at(out)->payload.u, 42u);
}

TEST_F(ConvertNumericTest, DoubleToUintNegativeRejected) {
  uint32_t out = MakeOut();
  cel_double_to_uint_at_v(out, cel_make_double(-0.1));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertNumericTest, DoubleToUintOverflowRejected) {
  // 2^64 = UINT64_MAX + 1 (rounded); rejected by the >= boundary.
  uint32_t out = MakeOut();
  cel_double_to_uint_at_v(out, cel_make_double(1.8446744073709552e19));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertNumericTest, DoubleToUintNaNRejected) {
  uint32_t out = MakeOut();
  cel_double_to_uint_at_v(out, cel_make_double(std::nan("")));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

// 3VL absorbs (one per kernel, sampled — every kernel uses the
// shared absorb_3vl_unary inline).

TEST_F(ConvertNumericTest, UintToIntAbsorbsError) {
  uint32_t out = MakeOut();
  cel_uint_to_int_at_v(out, MakeError());
  ExpectError(out, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(ConvertNumericTest, DoubleToIntAbsorbsUnknown) {
  uint32_t out = MakeOut();
  cel_double_to_int_at_v(out, MakeUnknown());
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_UNKNOWN));
}

// ─────────────────────────────────────────────────────────────
// M10.C — string parsing.
// ─────────────────────────────────────────────────────────────

using ConvertParseTest = ConvertFixture;

// int(string) — decimal, optional leading `-`, no whitespace, no
// trailing garbage.

TEST_F(ConvertParseTest, StringToIntZero) {
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string("0", 1));
  EXPECT_EQ(cel_value_at(out)->payload.i, 0);
}

TEST_F(ConvertParseTest, StringToIntPositive) {
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string("42", 2));
  EXPECT_EQ(cel_value_at(out)->payload.i, 42);
}

TEST_F(ConvertParseTest, StringToIntNegative) {
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string("-7", 2));
  EXPECT_EQ(cel_value_at(out)->payload.i, -7);
}

TEST_F(ConvertParseTest, StringToIntMax) {
  const char kMax[] = "9223372036854775807";
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string(kMax, sizeof(kMax) - 1));
  EXPECT_EQ(cel_value_at(out)->payload.i, INT64_MAX);
}

TEST_F(ConvertParseTest, StringToIntMin) {
  const char kMin[] = "-9223372036854775808";
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string(kMin, sizeof(kMin) - 1));
  EXPECT_EQ(cel_value_at(out)->payload.i, INT64_MIN);
}

TEST_F(ConvertParseTest, StringToIntOverflow) {
  const char kOver[] = "9223372036854775808";  // INT64_MAX + 1
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string(kOver, sizeof(kOver) - 1));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertParseTest, StringToIntEmpty) {
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string("", 0));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertParseTest, StringToIntJustMinus) {
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string("-", 1));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertParseTest, StringToIntTrailingGarbage) {
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string("42x", 3));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertParseTest, StringToIntLeadingPlusAccepted) {
  // cel-cpp int(string) uses absl::SimpleAtoi (type_conversion_functions.cc:140),
  // whose safe_strtoi_base accepts an optional leading '+'.  int('+1') == 1.
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string("+1", 2));
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(cel_value_at(out)->payload.i, 1);
}

TEST_F(ConvertParseTest, StringToIntWhitespaceRejected) {
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_string(" 1", 2));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

// uint(string) — decimal only, no sign.

TEST_F(ConvertParseTest, StringToUintZero) {
  uint32_t out = MakeOut();
  cel_string_to_uint_at_v(out, cel_make_string("0", 1));
  EXPECT_EQ(cel_value_at(out)->payload.u, 0u);
}

TEST_F(ConvertParseTest, StringToUintMax) {
  const char kMax[] = "18446744073709551615";
  uint32_t out = MakeOut();
  cel_string_to_uint_at_v(out, cel_make_string(kMax, sizeof(kMax) - 1));
  EXPECT_EQ(cel_value_at(out)->payload.u, UINT64_MAX);
}

TEST_F(ConvertParseTest, StringToUintOverflow) {
  const char kOver[] = "18446744073709551616";
  uint32_t out = MakeOut();
  cel_string_to_uint_at_v(out, cel_make_string(kOver, sizeof(kOver) - 1));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertParseTest, StringToUintNegativeRejected) {
  uint32_t out = MakeOut();
  cel_string_to_uint_at_v(out, cel_make_string("-1", 2));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

// double(string) — admits scientific notation + ±inf / nan.

TEST_F(ConvertParseTest, StringToDoubleSimple) {
  uint32_t out = MakeOut();
  cel_string_to_double_at_v(out, cel_make_string("3.5", 3));
  EXPECT_EQ(cel_value_at(out)->payload.d, 3.5);
}

TEST_F(ConvertParseTest, StringToDoubleNegative) {
  uint32_t out = MakeOut();
  cel_string_to_double_at_v(out, cel_make_string("-2.5", 4));
  EXPECT_EQ(cel_value_at(out)->payload.d, -2.5);
}

TEST_F(ConvertParseTest, StringToDoubleScientific) {
  uint32_t out = MakeOut();
  cel_string_to_double_at_v(out, cel_make_string("1e3", 3));
  EXPECT_EQ(cel_value_at(out)->payload.d, 1000.0);
}

TEST_F(ConvertParseTest, StringToDoubleNegativeExponent) {
  uint32_t out = MakeOut();
  cel_string_to_double_at_v(out, cel_make_string("5e-1", 4));
  EXPECT_EQ(cel_value_at(out)->payload.d, 0.5);
}

TEST_F(ConvertParseTest, StringToDoubleInf) {
  uint32_t out = MakeOut();
  cel_string_to_double_at_v(out, cel_make_string("inf", 3));
  EXPECT_TRUE(std::isinf(cel_value_at(out)->payload.d));
  EXPECT_GT(cel_value_at(out)->payload.d, 0.0);
}

TEST_F(ConvertParseTest, StringToDoubleNegInfinity) {
  uint32_t out = MakeOut();
  cel_string_to_double_at_v(out, cel_make_string("-Infinity", 9));
  EXPECT_TRUE(std::isinf(cel_value_at(out)->payload.d));
  EXPECT_LT(cel_value_at(out)->payload.d, 0.0);
}

TEST_F(ConvertParseTest, StringToDoubleNan) {
  uint32_t out = MakeOut();
  cel_string_to_double_at_v(out, cel_make_string("NaN", 3));
  EXPECT_TRUE(std::isnan(cel_value_at(out)->payload.d));
}

TEST_F(ConvertParseTest, StringToDoubleEmptyRejected) {
  uint32_t out = MakeOut();
  cel_string_to_double_at_v(out, cel_make_string("", 0));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertParseTest, StringToDoubleTrailingGarbageRejected) {
  uint32_t out = MakeOut();
  cel_string_to_double_at_v(out, cel_make_string("1.5x", 4));
  ExpectError(out, CEL_ERR_OVERFLOW);
}

// bool(string) — exact 10-row truth table.

struct BoolParseRow {
  const char* in;
  int expect;  // 1 = true, 0 = false, -1 = error
};

class StringToBoolTable : public ConvertFixture,
                          public ::testing::WithParamInterface<BoolParseRow> {};

TEST_P(StringToBoolTable, AdmitSet) {
  const auto& row = GetParam();
  uint32_t in =
      cel_make_string(row.in, static_cast<uint32_t>(std::strlen(row.in)));
  uint32_t out = MakeOut();
  cel_string_to_bool_at_v(out, in);
  const CelValue* v = cel_value_at(out);
  if (row.expect == -1) {
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_ERROR));
  } else {
    EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_BOOL));
    EXPECT_EQ(v->payload.b, row.expect);
  }
}

INSTANTIATE_TEST_SUITE_P(
    All, StringToBoolTable,
    ::testing::Values(
        // Admitted true spellings.
        BoolParseRow{"1", 1}, BoolParseRow{"t", 1}, BoolParseRow{"true", 1},
        BoolParseRow{"TRUE", 1}, BoolParseRow{"True", 1},
        // Admitted false spellings.
        BoolParseRow{"0", 0}, BoolParseRow{"f", 0}, BoolParseRow{"false", 0},
        BoolParseRow{"FALSE", 0}, BoolParseRow{"False", 0},
        // Mixed-case beyond Title-case rejects (spec says only the 5
        // spellings per polarity).
        BoolParseRow{"TrUe", -1}, BoolParseRow{"fAlSe", -1},
        BoolParseRow{"T", -1}, BoolParseRow{"F", -1},
        // Whitespace / surrounding garbage rejects.
        BoolParseRow{" true", -1}, BoolParseRow{"true ", -1},
        // Empty rejects.
        BoolParseRow{"", -1}));

// All parse helpers absorb ERROR / UNKNOWN.
TEST_F(ConvertParseTest, ParseAbsorbsError) {
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, MakeError());
  ExpectError(out, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(ConvertParseTest, ParseKindMismatch) {
  uint32_t out = MakeOut();
  cel_string_to_int_at_v(out, cel_make_int(5));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

// ─────────────────────────────────────────────────────────────
// M10.D — number / bool → string formatting.
// ─────────────────────────────────────────────────────────────

using ConvertFormatTest = ConvertFixture;

TEST_F(ConvertFormatTest, IntZero) {
  uint32_t out = MakeOut();
  cel_int_to_string_at_v(out, cel_make_int(0));
  EXPECT_EQ(StringAt(out), "0");
}

TEST_F(ConvertFormatTest, IntPositive) {
  uint32_t out = MakeOut();
  cel_int_to_string_at_v(out, cel_make_int(42));
  EXPECT_EQ(StringAt(out), "42");
}

TEST_F(ConvertFormatTest, IntNegative) {
  uint32_t out = MakeOut();
  cel_int_to_string_at_v(out, cel_make_int(-7));
  EXPECT_EQ(StringAt(out), "-7");
}

TEST_F(ConvertFormatTest, IntMax) {
  uint32_t out = MakeOut();
  cel_int_to_string_at_v(out, cel_make_int(INT64_MAX));
  EXPECT_EQ(StringAt(out), "9223372036854775807");
}

TEST_F(ConvertFormatTest, IntMin) {
  // INT64_MIN is the canonical "negation would UB" case.
  uint32_t out = MakeOut();
  cel_int_to_string_at_v(out, cel_make_int(INT64_MIN));
  EXPECT_EQ(StringAt(out), "-9223372036854775808");
}

TEST_F(ConvertFormatTest, UintZero) {
  uint32_t out = MakeOut();
  cel_uint_to_string_at_v(out, cel_make_uint(0u));
  EXPECT_EQ(StringAt(out), "0");
}

TEST_F(ConvertFormatTest, UintMax) {
  uint32_t out = MakeOut();
  cel_uint_to_string_at_v(out, cel_make_uint(UINT64_MAX));
  EXPECT_EQ(StringAt(out), "18446744073709551615");
}

TEST_F(ConvertFormatTest, BoolTrue) {
  uint32_t out = MakeOut();
  cel_bool_to_string_at_v(out, cel_make_bool(1));
  EXPECT_EQ(StringAt(out), "true");
}

TEST_F(ConvertFormatTest, BoolFalse) {
  uint32_t out = MakeOut();
  cel_bool_to_string_at_v(out, cel_make_bool(0));
  EXPECT_EQ(StringAt(out), "false");
}

TEST_F(ConvertFormatTest, DoubleZero) {
  uint32_t out = MakeOut();
  cel_double_to_string_at_v(out, cel_make_double(0.0));
  EXPECT_EQ(StringAt(out), "0");
}

TEST_F(ConvertFormatTest, DoubleNegativeZero) {
  // -0.0 == 0.0; helper emits the zero special.
  uint32_t out = MakeOut();
  cel_double_to_string_at_v(out, cel_make_double(-0.0));
  EXPECT_EQ(StringAt(out), "0");
}

TEST_F(ConvertFormatTest, DoubleInteger) {
  uint32_t out = MakeOut();
  cel_double_to_string_at_v(out, cel_make_double(42.0));
  EXPECT_EQ(StringAt(out), "42");
}

TEST_F(ConvertFormatTest, DoubleNegativeInteger) {
  uint32_t out = MakeOut();
  cel_double_to_string_at_v(out, cel_make_double(-7.0));
  EXPECT_EQ(StringAt(out), "-7");
}

TEST_F(ConvertFormatTest, DoubleHalf) {
  uint32_t out = MakeOut();
  cel_double_to_string_at_v(out, cel_make_double(0.5));
  EXPECT_EQ(StringAt(out), "0.5");
}

TEST_F(ConvertFormatTest, DoubleNaN) {
  uint32_t out = MakeOut();
  cel_double_to_string_at_v(out, cel_make_double(std::nan("")));
  EXPECT_EQ(StringAt(out), "nan");
}

TEST_F(ConvertFormatTest, DoublePosInf) {
  uint32_t out = MakeOut();
  cel_double_to_string_at_v(
      out, cel_make_double(std::numeric_limits<double>::infinity()));
  EXPECT_EQ(StringAt(out), "+Inf");
}

TEST_F(ConvertFormatTest, DoubleNegInf) {
  uint32_t out = MakeOut();
  cel_double_to_string_at_v(
      out, cel_make_double(-std::numeric_limits<double>::infinity()));
  EXPECT_EQ(StringAt(out), "-Inf");
}

TEST_F(ConvertFormatTest, DoubleScientificSmall) {
  // 1e-10 is below the mixed-path 1e-4 floor; scientific notation.
  uint32_t out = MakeOut();
  cel_double_to_string_at_v(out, cel_make_double(1e-10));
  // Round-trip identity is the conformance contract; specific byte
  // shape is "1e-10" by construction.
  const std::string s = StringAt(out);
  EXPECT_TRUE(s.find('e') != std::string::npos) << s;
}

TEST_F(ConvertFormatTest, DoubleScientificLarge) {
  // 1e20 is above the mixed-path 1e18 ceiling; scientific notation.
  uint32_t out = MakeOut();
  cel_double_to_string_at_v(out, cel_make_double(1e20));
  const std::string s = StringAt(out);
  EXPECT_TRUE(s.find('e') != std::string::npos) << s;
}

TEST_F(ConvertFormatTest, FormatKindMismatch) {
  uint32_t out = MakeOut();
  cel_int_to_string_at_v(out, cel_make_double(1.0));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(ConvertFormatTest, FormatAbsorbsError) {
  uint32_t out = MakeOut();
  cel_int_to_string_at_v(out, MakeError());
  ExpectError(out, CEL_ERR_DIVIDE_BY_ZERO);
}

// ─────────────────────────────────────────────────────────────
// M10.E — bytes ↔ string interconversion.
// ─────────────────────────────────────────────────────────────

using ConvertBytesStringTest = ConvertFixture;

TEST_F(ConvertBytesStringTest, StringToBytesAlias) {
  uint32_t in = cel_make_string("abc", 3);
  uint32_t out = MakeOut();
  cel_string_to_bytes_at_v(out, in);
  const CelValue* iv = cel_value_at(in);
  const CelValue* ov = cel_value_at(out);
  EXPECT_EQ(ov->kind, static_cast<uint32_t>(CEL_BYTES));
  // Span aliased (same ptr + len).
  EXPECT_EQ(ov->payload.s.ptr, iv->payload.s.ptr);
  EXPECT_EQ(ov->payload.s.len, iv->payload.s.len);
}

TEST_F(ConvertBytesStringTest, StringToBytesEmpty) {
  uint32_t in = cel_make_string("", 0);
  uint32_t out = MakeOut();
  cel_string_to_bytes_at_v(out, in);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(cel_value_at(out)->payload.s.len, 0u);
}

TEST_F(ConvertBytesStringTest, BytesToStringValidUtf8) {
  uint32_t in = cel_make_bytes("abc", 3);
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  EXPECT_EQ(StringAt(out), "abc");
}

TEST_F(ConvertBytesStringTest, BytesToStringEmpty) {
  uint32_t in = cel_make_bytes("", 0);
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(cel_value_at(out)->payload.s.len, 0u);
}

// UTF-8 reject matrix (RFC3629) — one row per failure mode.

TEST_F(ConvertBytesStringTest, BytesToStringRejectsOrphanContinuation) {
  // 0x80 is a continuation byte without a leader.
  const uint8_t kBad[] = {0x80};
  uint32_t in = cel_make_bytes(kBad, sizeof(kBad));
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertBytesStringTest, BytesToStringRejectsOverlong2Byte) {
  // 0xC0 / 0xC1 are 2-byte leaders with no admissible payload (would
  // encode <U+0080).
  const uint8_t kBad[] = {0xC0, 0x80};
  uint32_t in = cel_make_bytes(kBad, sizeof(kBad));
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertBytesStringTest, BytesToStringRejectsTruncated) {
  // 2-byte leader without continuation.
  const uint8_t kBad[] = {0xC2};
  uint32_t in = cel_make_bytes(kBad, sizeof(kBad));
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertBytesStringTest, BytesToStringRejectsSurrogate) {
  // 0xED 0xA0..0xBF is the surrogate range U+D800..U+DFFF.
  const uint8_t kBad[] = {0xED, 0xA0, 0x80};
  uint32_t in = cel_make_bytes(kBad, sizeof(kBad));
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertBytesStringTest, BytesToStringRejectsBeyondMax) {
  // 0xF4 0x90.. is above U+10FFFF.
  const uint8_t kBad[] = {0xF4, 0x90, 0x80, 0x80};
  uint32_t in = cel_make_bytes(kBad, sizeof(kBad));
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertBytesStringTest, BytesToStringRejectsInvalidLeader) {
  // 0xF5-0xFF are never valid leaders.
  const uint8_t kBad[] = {0xFF};
  uint32_t in = cel_make_bytes(kBad, sizeof(kBad));
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  ExpectError(out, CEL_ERR_OVERFLOW);
}

TEST_F(ConvertBytesStringTest, BytesToStringAcceptsValid2Byte) {
  // U+00A9 (©) = 0xC2 0xA9.
  const uint8_t kCopy[] = {0xC2, 0xA9};
  uint32_t in = cel_make_bytes(kCopy, sizeof(kCopy));
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_STRING));
}

TEST_F(ConvertBytesStringTest, BytesToStringAcceptsValid3Byte) {
  // U+20AC (€) = 0xE2 0x82 0xAC.
  const uint8_t kEuro[] = {0xE2, 0x82, 0xAC};
  uint32_t in = cel_make_bytes(kEuro, sizeof(kEuro));
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_STRING));
}

TEST_F(ConvertBytesStringTest, BytesToStringAcceptsValid4Byte) {
  // U+1F600 (😀) = 0xF0 0x9F 0x98 0x80.
  const uint8_t kEmoji[] = {0xF0, 0x9F, 0x98, 0x80};
  uint32_t in = cel_make_bytes(kEmoji, sizeof(kEmoji));
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, in);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_STRING));
}

TEST_F(ConvertBytesStringTest, KindMismatchStringToBytes) {
  uint32_t out = MakeOut();
  cel_string_to_bytes_at_v(out, cel_make_int(5));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(ConvertBytesStringTest, KindMismatchBytesToString) {
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, cel_make_string("x", 1));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(ConvertBytesStringTest, AbsorbsError) {
  uint32_t out = MakeOut();
  cel_bytes_to_string_at_v(out, MakeError());
  ExpectError(out, CEL_ERR_DIVIDE_BY_ZERO);
}

}  // namespace
}  // namespace celwasm
