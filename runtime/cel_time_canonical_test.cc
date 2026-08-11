// Byte-exact pins for the shared canonical-form helpers.  These
// strings are conformance-scored against cel-cpp verbatim; the
// e2e mirrors live in `e2e/time_test.cc` (FormatConvertE2ETest) —
// expectations here match those pins character-for-character.

#include "runtime/cel_time_canonical.h"

#include <cstdint>
#include <string>

#include "gtest/gtest.h"

namespace celwasm {
namespace {

struct TsCase {
  const char* label;
  int64_t seconds;
  int32_t nanos;
  const char* expect;
};

class TimestampCanonicalTest : public ::testing::TestWithParam<TsCase> {};

TEST_P(TimestampCanonicalTest, FormatsByteExact) {
  const TsCase& c = GetParam();
  EXPECT_EQ(FormatTimestampRfc3339(c.seconds, c.nanos), c.expect) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    Grid, TimestampCanonicalTest,
    ::testing::Values(
        TsCase{"Epoch", 0, 0, "1970-01-01T00:00:00Z"},
        TsCase{"LangdefSample", 1234567890, 0, "2009-02-13T23:31:30Z"},
        // %E*S is minimal-digits: half a second renders `.5`, not
        // `.500` (e2e pin: `string(timestamp("...30.500Z"))` ==
        // "2009-02-13T23:31:30.5Z").
        TsCase{"FracMinimalDigits", 1234567890, 500'000'000,
               "2009-02-13T23:31:30.5Z"},
        TsCase{"FracFullNanos", 1234567890, 123'456'789,
               "2009-02-13T23:31:30.123456789Z"},
        TsCase{"FracOneNano", 0, 1, "1970-01-01T00:00:00.000000001Z"},
        // The year is UNPADDED — langdef-min renders with a bare
        // `1`, pinned by e2e `TimestampFormatLangdefMin`.
        TsCase{"LangdefMinUnpaddedYear", -62135596800LL, 0,
               "1-01-01T00:00:00Z"},
        TsCase{"LangdefMax", 253402300799LL, 0, "9999-12-31T23:59:59Z"},
        TsCase{"LangdefMaxFullNanos", 253402300799LL, 999'999'999,
               "9999-12-31T23:59:59.999999999Z"},
        // Sign-correlated pre-epoch pair: the instant is formed by
        // exact addition, so (0s, -500ms) is 1969-12-31T23:59:59.5.
        TsCase{"PreEpochSignCorrelated", 0, -500'000'000,
               "1969-12-31T23:59:59.5Z"}),
    [](const ::testing::TestParamInfo<TsCase>& info) {
      return info.param.label;
    });

struct DurCase {
  const char* label;
  int64_t seconds;
  int32_t nanos;
  const char* expect;
};

class DurationCanonicalTest : public ::testing::TestWithParam<DurCase> {};

TEST_P(DurationCanonicalTest, FormatsByteExact) {
  const DurCase& c = GetParam();
  EXPECT_EQ(FormatProtoDuration(c.seconds, c.nanos), c.expect) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    Grid, DurationCanonicalTest,
    ::testing::Values(DurCase{"Zero", 0, 0, "0s"},
                      DurCase{"WholeSeconds", 3723, 0, "3723s"},
                      // Fraction renders at 3 / 6 / 9 digits — trailing zero
                      // TRIPLES are trimmed, zeros within a triple are kept.
                      DurCase{"MillisTriple", 0, 500'000'000, "0.500s"},
                      DurCase{"MillisInnerZeros", 0, 120'000'000, "0.120s"},
                      DurCase{"MicrosTriple", 0, 123'000, "0.000123s"},
                      DurCase{"NanosFull", 0, 1, "0.000000001s"},
                      DurCase{"SecondsAndNanos", 1, 500'000'000, "1.500s"},
                      DurCase{"NegativeBoth", -1, -500'000'000, "-1.500s"},
                      DurCase{"NegativeNanosOnly", 0, -500'000'000, "-0.500s"},
                      DurCase{"ProtoMax", 315576000000LL, 0, "315576000000s"},
                      DurCase{"ProtoMin", -315576000000LL, 0,
                              "-315576000000s"}),
    [](const ::testing::TestParamInfo<DurCase>& info) {
      return info.param.label;
    });

}  // namespace
}  // namespace celwasm
