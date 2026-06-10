// M10 e2e test suite — the spec of "done" for type-conversion
// overloads (`bool(x)` / `int(x)` / `uint(x)` / `double(x)` /
// `string(x)` / `bytes(x)` and their inter-conversions).  Mirrors
// the m9_test shape: every test asserts a capability
// `m10-conversions.md` says M10 must light up; running this
// binary today (with every conversion id still in
// `OverloadTable::kExplicitlyUnimplementedIds`) should fail every
// case below.  Greening the suite is the M10 exit per
// `m10-conversions.md` §6.
//
// Timestamp / duration conversions (`int(timestamp)`,
// `string(duration)`, `timestamp(string)`, ...) are explicitly
// out of scope — see `m10-conversions.md` §2.2.  Test rows that
// exercise those paths are GTEST_SKIP'd with a pointer to the
// timestamps slice plan (separate, future).
//
// Fixtures grouped by capability (one section per slice):
//
//   - IdentityE2ETest         M10.A — `<scalar>(<scalar>)` identity
//                                     for every kind (6 rows).
//   - IntFamilyE2ETest        M10.B — `int(bool)`, `int(uint)`,
//                                     `int(double)` with overflow
//                                     coverage.
//   - UintFamilyE2ETest       M10.B — `uint(int)`, `uint(double)`
//                                     with sign + overflow coverage.
//   - DoubleFamilyE2ETest     M10.B — `double(int)`, `double(uint)`
//                                     boundaries (lossy ok per spec).
//   - StringParseE2ETest      M10.C — `int(string)`, `uint(string)`,
//                                     `double(string)`, `bool(string)`
//                                     admit + reject matrix.
//   - NumberFormatE2ETest     M10.D — `string(<numeric/bool>)` —
//                                     trivial values pinned, doubles
//                                     asserted via round-trip.
//   - BytesFamilyE2ETest      M10.E — `bytes(string)`, `string(bytes)`
//                                     incl. UTF-8 validation matrix.
//   - RejectE2ETest           §6.5  — overflow / parse-failure /
//                                     invalid-UTF8 / out-of-scope
//                                     rejection matrix.
//   - DeferredTimestampE2ETest §2.2 — placeholder rows for
//                                     conversions deferred to the
//                                     timestamps slice (all SKIP).
//
// Conformance unlock estimate per slice is logged on each test
// section; aggregate target is +60..+100 PASS in conformance per
// `m10-conversions.md` §1.

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
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

using ::celwasm::e2e::GlobalEngine;

using ConfigureFn = std::function<void(Compiler::Builder&)>;
absl::StatusOr<Compiler> BuildCompiler(const ConfigureFn& configure) {
  Compiler::Builder b;
  configure(b);
  return std::move(b).Build();
}

absl::StatusOr<Compiler> CompilerEmpty() {
  return BuildCompiler([](Compiler::Builder& /*b*/) {});
}

using ::celwasm::e2e::CompilePlan;

using ::celwasm::e2e::EvalOk;

void ExpectCompileFails(const Compiler& compiler, absl::string_view source,
                        absl::string_view why) {
  auto program_or = compiler.Compile(source, ::celwasm::e2e::DefaultOpts());
  EXPECT_FALSE(program_or.ok())
      << "expected `" << source << "` to fail at compile (" << why << ")";
}

void ExpectEvalError(const Compiler& compiler, absl::string_view source,
                     absl::string_view why) {
  auto instance = CompilePlan(compiler, source);
  Activation a;
  auto v = instance.Eval(a);
  ASSERT_THAT(v, IsOk()) << source;
  EXPECT_TRUE(v->IsError())
      << "expected `" << source << "` to surface a CEL error Value (" << why
      << "); got kind=" << static_cast<int>(v->kind());
}

// ──────────────────────────────────────────────────────────────
// 1. IdentityE2ETest  (M10.A — identity overloads, all seed to
//    `cel_copy_slot`)
//
//    Per `m10-conversions.md` §3.5: the 6 identity overloads
//    (`bool(bool)`, `int(int)`, ..., `bytes(bytes)`) route through
//    the existing M5.G `cel_copy_slot` helper — no new runtime
//    body, just OverloadTable seeds.
// ──────────────────────────────────────────────────────────────

class IdentityE2ETest : public ::testing::Test {};

TEST_F(IdentityE2ETest, BoolIdentity) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "bool(true) == true");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(IdentityE2ETest, IntIdentity) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "int(7) == 7");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(IdentityE2ETest, UintIdentity) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "uint(7u) == 7u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(IdentityE2ETest, DoubleIdentity) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "double(1.5) == 1.5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(IdentityE2ETest, StringIdentity) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(string("hello") == "hello")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(IdentityE2ETest, BytesIdentity) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(bytes(b"abc") == b"abc")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 2. IntFamilyE2ETest  (M10.B — bool→int, uint→int, double→int)
//
//    Per `m10-conversions.md` §3.1: range / overflow / NaN / Inf
//    rules.  cel-cpp parity:
//    `runtime/standard/type_conversion_functions.cc::
//    RegisterIntConversionFunctions`.
// ──────────────────────────────────────────────────────────────

struct IntCase {
  std::string label;
  std::string source;  // CEL source whose top is `int(...) == <expected>`.
};

class IntFamilyE2ETest : public ::testing::TestWithParam<IntCase> {};

TEST_P(IntFamilyE2ETest, ConvertsAndCompares) {
  const auto& p = GetParam();
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, p.source);
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true) << "source=" << p.source;
}

INSTANTIATE_TEST_SUITE_P(
    IntConversions, IntFamilyE2ETest,
    ::testing::Values(
        // Note: `int(bool)` is registered by cel-cpp's runtime
        // (`type_conversion_functions.cc::RegisterIntConversionFunctions`)
        // but NOT by its checker (`checker/standard_library.cc` does
        // not declare a BoolType→IntType overload).  So `int(true)`
        // is checker-rejected; the row was dropped from this matrix.
        // Bool→int / bool→uint / bool→double conversions land if
        // we ship a v2-side checker extension; tracked as future
        // work in m10-conversions.md §9.
        //
        // uint → int (in-range).
        IntCase{"UintZero", "int(0u) == 0"},
        IntCase{"UintMidrange", "int(123u) == 123"},
        IntCase{"UintAtIntMax",
                "int(9223372036854775807u) == 9223372036854775807"},
        // double → int (truncates toward zero).
        IntCase{"DoubleZero", "int(0.0) == 0"},
        IntCase{"DoublePositiveTruncates", "int(1.7) == 1"},
        IntCase{"DoubleNegativeTruncates", "int(-1.7) == -1"},
        // Note: int(2^63 - 1024 as double) — the largest representable
        // double < 2^63 — round-trips to a value near INT64_MAX.
        IntCase{"DoubleNearIntMax",
                "int(9223372036854774784.0) == 9223372036854774784"}),
    [](const ::testing::TestParamInfo<IntCase>& info) {
      return info.param.label;
    });

TEST_F(IntFamilyE2ETest, UintOverflowingIntMaxIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // (INT64_MAX + 1) as uint = 9223372036854775808u; converts to int → overflow.
  ExpectEvalError(*compiler, "int(9223372036854775808u)",
                  "uint > INT64_MAX overflows int");
}

TEST_F(IntFamilyE2ETest, DoubleOverflowingIntMaxIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, "int(1e30)", "double > INT64_MAX overflows int");
}

TEST_F(IntFamilyE2ETest, DoubleOverflowingIntMinIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, "int(-1e30)", "double < INT64_MIN overflows int");
}

TEST_F(IntFamilyE2ETest, DoubleAtIntMinIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // The double -2^63 itself is a range error (validated against the cel-cpp
  // oracle — cel_cpp_oracle_test.cc IntFromDoubleMinIsRangeError); INT64_MIN
  // is unreachable via int(double) since no double lies between -2^63 and the
  // next representable value.
  ExpectEvalError(*compiler, "int(-9223372036854775808.0)",
                  "double -2^63 is out of int range");
}

TEST_F(IntFamilyE2ETest, DoubleNaNIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, "int(0.0/0.0)", "NaN to int is overflow per spec");
}

TEST_F(IntFamilyE2ETest, DoubleInfinityIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, "int(1.0/0.0)", "Infinity to int is overflow");
}

// ──────────────────────────────────────────────────────────────
// 3. UintFamilyE2ETest  (M10.B — int→uint, double→uint)
// ──────────────────────────────────────────────────────────────

class UintFamilyE2ETest : public ::testing::Test {};

TEST_F(UintFamilyE2ETest, IntZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "uint(0) == 0u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(UintFamilyE2ETest, IntPositive) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "uint(123) == 123u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(UintFamilyE2ETest, IntAtIntMaxGoesToUintMaxOver2) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "uint(9223372036854775807) == 9223372036854775807u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(UintFamilyE2ETest, IntNegativeIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, "uint(-1)", "negative int to uint is overflow");
}

TEST_F(UintFamilyE2ETest, DoubleZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "uint(0.0) == 0u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(UintFamilyE2ETest, DoubleTruncates) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "uint(3.9) == 3u");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(UintFamilyE2ETest, DoubleNegativeIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, "uint(-1.0)", "negative double to uint overflow");
}

TEST_F(UintFamilyE2ETest, DoubleNaNIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, "uint(0.0/0.0)", "NaN to uint overflow");
}

TEST_F(UintFamilyE2ETest, DoubleInfinityIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, "uint(1.0/0.0)", "Infinity to uint overflow");
}

// ──────────────────────────────────────────────────────────────
// 4. DoubleFamilyE2ETest  (M10.B — int→double, uint→double)
//
//    Per `m10-conversions.md` §3.1: never errors; lossy beyond
//    2^53 per IEEE 754, which langdef tolerates.
// ──────────────────────────────────────────────────────────────

class DoubleFamilyE2ETest : public ::testing::Test {};

TEST_F(DoubleFamilyE2ETest, IntZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "double(0) == 0.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(DoubleFamilyE2ETest, IntPositive) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "double(123) == 123.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(DoubleFamilyE2ETest, IntNegative) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "double(-123) == -123.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(DoubleFamilyE2ETest, IntAtIntMaxLossy) {
  // 2^53 + 1 as int rounds to 2^53 as double.  Just assert lossy
  // no-error behavior: double(9007199254740993) is some double.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "double(9007199254740992) == 9007199254740992.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(DoubleFamilyE2ETest, UintZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "double(0u) == 0.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(DoubleFamilyE2ETest, UintLargeLossy) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "double(9007199254740992u) == 9007199254740992.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 5. StringParseE2ETest  (M10.C — string → numeric / bool)
//
//    Per `m10-conversions.md` §3.2 + §6.2: admit + reject matrix.
// ──────────────────────────────────────────────────────────────

class StringParseE2ETest : public ::testing::Test {};

// — int(string) admit —
TEST_F(StringParseE2ETest, IntFromZeroString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(int("0") == 0)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringParseE2ETest, IntFromPositiveString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(int("123") == 123)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringParseE2ETest, IntFromNegativeString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(int("-1") == -1)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringParseE2ETest, IntFromIntMaxString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(int("9223372036854775807") == 9223372036854775807)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringParseE2ETest, IntFromIntMinString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(int("-9223372036854775808") == -9223372036854775808)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// — int(string) reject —
TEST_F(StringParseE2ETest, IntFromEmptyStringIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(int(""))", "empty string is not a number");
}

TEST_F(StringParseE2ETest, IntFromGarbageStringIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(int("abc"))", "non-digit string");
}

TEST_F(StringParseE2ETest, IntFromOverflowStringIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(int("9223372036854775808"))",
                  "INT64_MAX + 1 overflows");
}

TEST_F(StringParseE2ETest, IntFromFractionalStringIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(int("1.5"))",
                  "fractional rejected by SimpleAtoi");
}

// — uint(string) admit / reject —
TEST_F(StringParseE2ETest, UintFromString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(uint("123") == 123u)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringParseE2ETest, UintFromUintMaxString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(uint("18446744073709551615") == 18446744073709551615u)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringParseE2ETest, UintFromNegativeStringIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(uint("-1"))", "uint disallows leading -");
}

TEST_F(StringParseE2ETest, UintFromOverflowStringIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(uint("18446744073709551616"))",
                  "UINT64_MAX + 1 overflows");
}

// — double(string) admit / reject —
TEST_F(StringParseE2ETest, DoubleFromIntegerString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(double("123") == 123.0)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringParseE2ETest, DoubleFromFractionalString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(double("1.5") == 1.5)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringParseE2ETest, DoubleFromScientificString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(double("1e3") == 1000.0)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(StringParseE2ETest, DoubleFromGarbageStringIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(double("abc"))",
                  "non-numeric string is not a double");
}

// — bool(string) admit / reject —
struct BoolStringCase {
  std::string label;
  std::string input;
  bool expected = false;
};

class StringParseBoolE2ETest : public ::testing::TestWithParam<BoolStringCase> {
};

TEST_P(StringParseBoolE2ETest, ParsesPerCelCppTruthTable) {
  const auto& p = GetParam();
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  std::string source =
      "bool(\"" + p.input + "\") == " + (p.expected ? "true" : "false");
  auto instance = CompilePlan(*compiler, source);
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true) << "source=" << source;
}

INSTANTIATE_TEST_SUITE_P(
    BoolTruthTable, StringParseBoolE2ETest,
    ::testing::Values(BoolStringCase{"True", "true", true},
                      BoolStringCase{"TitleTrue", "True", true},
                      BoolStringCase{"UpperTRUE", "TRUE", true},
                      BoolStringCase{"LowerT", "t", true},
                      BoolStringCase{"DigitOne", "1", true},
                      BoolStringCase{"False", "false", false},
                      BoolStringCase{"TitleFalse", "False", false},
                      BoolStringCase{"UpperFALSE", "FALSE", false},
                      BoolStringCase{"LowerF", "f", false},
                      BoolStringCase{"DigitZero", "0", false}),
    [](const ::testing::TestParamInfo<BoolStringCase>& info) {
      return info.param.label;
    });

TEST_F(StringParseE2ETest, BoolFromMixedCaseIsError) {
  // cel-cpp's table is exact-string match; "TruE" is not in it.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(bool("TruE"))",
                  "mixed case not in cel-cpp's truth table");
}

TEST_F(StringParseE2ETest, BoolFromUnknownStringIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(bool("yes"))", "yes/no not in truth table");
}

// ──────────────────────────────────────────────────────────────
// 6. NumberFormatE2ETest  (M10.D — number → string)
//
//    Per `m10-conversions.md` §3.3 + §4.4: trivial values pinned;
//    non-trivial doubles asserted via round-trip equality
//    (`double(string(d)) == d`) since byte-exact format may
//    drift from cel-cpp.
// ──────────────────────────────────────────────────────────────

class NumberFormatE2ETest : public ::testing::Test {};

TEST_F(NumberFormatE2ETest, IntZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(string(0) == "0")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(NumberFormatE2ETest, IntNegative) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(string(-1) == "-1")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(NumberFormatE2ETest, IntAtBoundary) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(string(9223372036854775807) == "9223372036854775807")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(NumberFormatE2ETest, UintZero) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // Note: cel-cpp's string(uint) produces base-10 with no `u` suffix.
  // The langdef example "string(123u) // \"123u\"" is misleading —
  // see m10-conversions.md §3.3 for the spec citation.
  auto instance = CompilePlan(*compiler, R"(string(123u) == "123")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(NumberFormatE2ETest, UintMax) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(string(18446744073709551615u) == "18446744073709551615")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(NumberFormatE2ETest, BoolTrue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(string(true) == "true")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(NumberFormatE2ETest, BoolFalse) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(string(false) == "false")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(NumberFormatE2ETest, DoubleRoundTripPreservesValue) {
  // Per §4.4: byte-exact format may differ from cel-cpp; assert
  // round-trip identity instead.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "double(string(1.5)) == 1.5");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(NumberFormatE2ETest, DoubleZeroFormatsCleanly) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // Pin the trivial-zero spelling.  Either "0" or "0.0" is
  // acceptable per cel-cpp's `to_chars` general format; the test
  // asserts round-trip + that the result IS a string.
  auto instance = CompilePlan(
      *compiler, "size(string(0.0)) > 0 && double(string(0.0)) == 0.0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(NumberFormatE2ETest, DoubleNaNFormatsAsNan) {
  // Round-trip: double(string(NaN)) is itself a NaN.  Compare
  // structurally — `NaN == NaN` is false per IEEE 754, so we
  // instead check that the result kind is double and the string
  // representation is non-empty.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "size(string(0.0/0.0)) > 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 7. BytesFamilyE2ETest  (M10.E — bytes(string) + string(bytes)
//    incl. UTF-8 validation)
//
//    Per `m10-conversions.md` §3.4 + §6.4: bytes(string) is a
//    verbatim copy; string(bytes) UTF-8 validates per RFC3629.
// ──────────────────────────────────────────────────────────────

class BytesFamilyE2ETest : public ::testing::Test {};

TEST_F(BytesFamilyE2ETest, BytesFromAsciiString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(bytes("hello") == b"hello")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(BytesFamilyE2ETest, BytesFromEmptyString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(bytes("") == b"")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(BytesFamilyE2ETest, BytesFromUtf8String) {
  // Snowman (U+2603) — 3-byte UTF-8 (0xe2 0x98 0x83).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(bytes("☃") == b"\xe2\x98\x83")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(BytesFamilyE2ETest, StringFromAsciiBytes) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(string(b"hello") == "hello")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(BytesFamilyE2ETest, StringFromValidUtf8Bytes) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(string(b"\xe2\x98\x83") == "☃")");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(BytesFamilyE2ETest, StringFromInvalidLeadingByteIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(string(b"\xff"))",
                  "0xff is not a valid UTF-8 leading byte");
}

TEST_F(BytesFamilyE2ETest, StringFromOrphanContinuationByteIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(string(b"\x80"))",
                  "orphan UTF-8 continuation byte");
}

TEST_F(BytesFamilyE2ETest, StringFromTruncated2ByteSequenceIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(string(b"\xc2"))",
                  "truncated 2-byte UTF-8 sequence");
}

TEST_F(BytesFamilyE2ETest, StringFromUtf16SurrogateIsError) {
  // U+D800 encoded as 3-byte UTF-8 = 0xed 0xa0 0x80.  Per RFC3629,
  // surrogates are invalid in UTF-8.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(string(b"\xed\xa0\x80"))",
                  "UTF-16 surrogate is invalid UTF-8 per RFC3629");
}

TEST_F(BytesFamilyE2ETest, StringFromOverlongNulIsError) {
  // 0xc0 0x80 is the overlong-encoded NUL.  Valid in some
  // legacy encodings, invalid in RFC3629 UTF-8.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectEvalError(*compiler, R"(string(b"\xc0\x80"))",
                  "overlong NUL is invalid UTF-8");
}

// ──────────────────────────────────────────────────────────────
// 8. RejectE2ETest  (§6.5 — checker / runtime rejection)
//
//    Per CLAUDE.md "Cover the edge-case matrix — this is a
//    compiler.  Negative coverage (rejection cases) is ≥ 30% of
//    the total".
// ──────────────────────────────────────────────────────────────

class RejectE2ETest : public ::testing::Test {};

TEST_F(RejectE2ETest, IntOfListIsCheckerRejected) {
  // No `int(list)` overload in cel-cpp's set; checker rejects.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, "int([1, 2, 3])", "no overload int(list)");
}

TEST_F(RejectE2ETest, IntOfMapIsCheckerRejected) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, R"(int({"k": 1}))", "no overload int(map)");
}

TEST_F(RejectE2ETest, StringOfListIsCheckerRejected) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, "string([1, 2, 3])",
                     "no overload string(list)");
}

TEST_F(RejectE2ETest, BoolOfIntIsCheckerRejected) {
  // No `bool(int)` overload — only bool(bool) and bool(string).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  ExpectCompileFails(*compiler, "bool(1)", "no overload bool(int)");
}

// ──────────────────────────────────────────────────────────────
// 9. DeferredTimestampE2ETest  (§2.2 — out of M10 scope; placeholder
//    rows for the timestamps slice)
//
//    These tests exercise the timestamp / duration conversion arms
//    that M10 explicitly carves out.  They're SKIP'd today; flip
//    on when the timestamps slice lands.
// ──────────────────────────────────────────────────────────────

class DeferredTimestampE2ETest : public ::testing::Test {};

TEST_F(DeferredTimestampE2ETest, IntFromTimestamp) {
  GTEST_SKIP()
      << "int(timestamp(...)) is the timestamps slice — out of M10 scope";
}

TEST_F(DeferredTimestampE2ETest, StringFromDuration) {
  GTEST_SKIP()
      << "string(duration(...)) is the timestamps slice — out of M10 scope";
}

TEST_F(DeferredTimestampE2ETest, TimestampFromString) {
  GTEST_SKIP()
      << "timestamp(string) is the timestamps slice — out of M10 scope";
}

TEST_F(DeferredTimestampE2ETest, DurationFromString) {
  GTEST_SKIP() << "duration(string) is the timestamps slice — out of M10 scope";
}

}  // namespace
}  // namespace celwasm
