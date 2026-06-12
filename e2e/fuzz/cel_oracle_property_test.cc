// Value-oracle property tests.  Each property generates a CEL
// source string of its target type from the typed-attribute
// grammar, evaluates it through BOTH our pipeline and cel-cpp's,
// and asserts the judged outcome is not a failure — all judging
// (classification, payload comparison, rendering) lives in
// `verdict.cc::RunOne`, shared with `mine_divergences`, so this
// test and the miner cannot disagree about what a failure is.
//
// Skips (not failures): the static-window capacity rejection at
// depths 7-8 (`kOurCapacityReject` — the known limitation pinned by
// `KnownBugs.LiteralIntListInScan*`), over-large sources
// (`kSourceTooLarge`), and both-engines-error agreement
// (`kBothErrored`).
//
// **This target is tagged `manual`** because the property mines
// for real bugs and WILL fail when it finds them — that's the
// discovery role m27 §"Oracle-divergence policy" expects.
// Excluded from `bazel test //...` to keep the green-baseline
// CI clean; run explicitly:
//
//   - `bazel test //e2e/fuzz:cel_oracle_property_test`  — unit-
//     test mode (~1000 randomised iterations).
//   - `bazel run --config=fuzztest //e2e/fuzz:cel_oracle_property_test
//      -- --fuzz=CelOracleProperty.BoolEvalAgreesWithOracle`  —
//     long-running coverage-guided fuzzer.
//
// Discovered divergences become known_bugs_test.cc entries pinning
// the exact source + activation; PBT is the discovery tool,
// known_bugs is the regression-pinning tool.

#include <cstdint>

#include "e2e/fuzz/verdict.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm::fuzz {
namespace {

void BoolEvalAgreesWithOracle(uint64_t seed, int depth) {
  const Verdict v = RunOne(CelType::Bool(), seed, depth);
  EXPECT_FALSE(v.IsFailure()) << v.Report("Bool");
}

void IntEvalAgreesWithOracle(uint64_t seed, int depth) {
  const Verdict v = RunOne(CelType::Int(), seed, depth);
  EXPECT_FALSE(v.IsFailure()) << v.Report("Int");
}

void UintEvalAgreesWithOracle(uint64_t seed, int depth) {
  const Verdict v = RunOne(CelType::Uint(), seed, depth);
  EXPECT_FALSE(v.IsFailure()) << v.Report("Uint");
}

void DoubleEvalAgreesWithOracle(uint64_t seed, int depth) {
  const Verdict v = RunOne(CelType::Double(), seed, depth);
  EXPECT_FALSE(v.IsFailure()) << v.Report("Double");
}

void StringEvalAgreesWithOracle(uint64_t seed, int depth) {
  const Verdict v = RunOne(CelType::String(), seed, depth);
  EXPECT_FALSE(v.IsFailure()) << v.Report("String");
}

void BytesEvalAgreesWithOracle(uint64_t seed, int depth) {
  const Verdict v = RunOne(CelType::Bytes(), seed, depth);
  EXPECT_FALSE(v.IsFailure()) << v.Report("Bytes");
}

// ── FUZZ_TEST registrations ──────────────────────────────────────
//
// Six properties, one per scalar target type.  Each runs ~1000
// random iterations in unit-test mode; under `--config=fuzztest`
// the same registration becomes a coverage-guided fuzzer over
// that target.  Depth 0..8: 7-8 are the deep-nesting band
// (capacity rejects there are skips, see `VerdictKind`).

// NOLINTBEGIN(bugprone-throwing-static-initialization) — the
// FUZZ_TEST registration macro expands to a static registrar whose
// constructor allocates; same accepted pattern as the kLinked
// registrars in testdata/cel_cpp_oracle.cc.
FUZZ_TEST(CelOracleProperty, BoolEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 8));
FUZZ_TEST(CelOracleProperty, IntEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 8));
FUZZ_TEST(CelOracleProperty, UintEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 8));
FUZZ_TEST(CelOracleProperty, DoubleEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 8));
FUZZ_TEST(CelOracleProperty, StringEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 8));
FUZZ_TEST(CelOracleProperty, BytesEvalAgreesWithOracle)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange<int>(0, 8));
// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace
}  // namespace celwasm::fuzz
