// Value-oracle property tests.  Each property generates a CEL
// source string of its target type from the typed-attribute
// grammar, evaluates it through BOTH our pipeline and cel-cpp's
// (via `oracle_harness`, the same plumbing `mine_divergences`
// uses), asserts both accept and that the results agree.
//
// The grammar is guarded so every emitted source is guaranteed to
// type-check (L2) and total over its declared input domain (per
// m27 §"Guarded productions").  Any oracle divergence reported by
// this property is therefore necessarily a runtime / codegen bug
// on our side, not a generator misconfiguration.
//
// Depth domain is 0..8.  At depths 7-8 a small percentage of
// generated expressions legitimately exceed the 8192-byte static
// window and are rejected at Compile with `ResourceExhausted`
// (the known capacity limitation pinned by
// `KnownBugs.LiteralIntListInScan*`); the property SKIPS those —
// `mine_divergences` is the tool that tracks rejection rates.
// Every other rejection is a failure.
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
// Discovered divergences become known_bugs_test.cc entries
// pinning the exact source + activation; PBT is the discovery
// tool, known_bugs is the regression-pinning tool.  Past finds
// (since fixed, pinned as live regression guards):
// `KnownBugs.PbtTernaryInsideIntSubtract`,
// `KnownBugs.PbtExistsOne*`.

#include <cmath>
#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "e2e/fuzz/oracle_harness.h"
#include "eval/value.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm::fuzz {
namespace {

// Generate a source of `target`, eval both sides through the
// shared harness, ASSERT both accept, RETURN both values to the
// caller (which compares the kind-specific payload).  Returns
// false (emitting its own failure unless the outcome is a known
// skip) if any pre-condition fails.
bool GenAndEval(const CelType& target, uint64_t seed, int depth,
                absl::string_view kind_label, GenAndEvalResult& out) {
  std::string err;
  switch (GenAndEvalSliceC(target, seed, depth, out, &err)) {
    case GenAndEvalStatus::kOk:
      return true;
    case GenAndEvalStatus::kSourceTooLarge:
      return false;  // capped, not a bug — see kMaxSourceBytes
    case GenAndEvalStatus::kOurPipelineRejected:
      if (absl::IsResourceExhausted(out.our_status)) {
        // Static-window capacity rejection — the known limitation
        // at depths 7-8 (see file header), not a divergence.
        return false;
      }
      ADD_FAILURE() << "our pipeline rejected a grammar-emitted " << kind_label
                    << " expression:\n  seed=" << seed << " depth=" << depth
                    << "\n  source=`" << out.source << "`\n  error=" << err;
      return false;
    case GenAndEvalStatus::kOracleRejected:
      ADD_FAILURE() << "cel-cpp rejected a grammar-emitted " << kind_label
                    << " expression:\n  seed=" << seed << " depth=" << depth
                    << "\n  source=`" << out.source << "`\n  error=" << err;
      return false;
    case GenAndEvalStatus::kBothErrored:
      // Both engines agree the expression errors (an overflow-style
      // guard leak in the grammar, or a future error-producing
      // production).  Agreement — nothing to compare.
      return false;
    case GenAndEvalStatus::kOracleErrorOnly:
      ADD_FAILURE() << "ERROR-VALUE DIVERGENCE (" << kind_label
                    << "): cel-cpp evaluates to a CEL error but our "
                    << "pipeline produced a plain value.\n  seed=" << seed
                    << " depth=" << depth << "\n  source=`" << out.source
                    << "`\n  oracle.error_message=`" << err << "`";
      return false;
  }
  ADD_FAILURE() << "unhandled GenAndEvalStatus";
  return false;
}

// ── Per-target properties ────────────────────────────────────────

void BoolEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::Bool(), seed, depth, "Bool", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kBool)
      << "kind mismatch on `" << r.source << "`";
  EXPECT_EQ(*r.ours.AsBool(), r.oracle.bool_value())
      << "VALUE DIVERGENCE (Bool)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`";
}

void IntEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::Int(), seed, depth, "Int", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kInt)
      << "kind mismatch on `" << r.source << "`";
  EXPECT_EQ(*r.ours.AsInt(), r.oracle.int64_value())
      << "VALUE DIVERGENCE (Int)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`";
}

void UintEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::Uint(), seed, depth, "Uint", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kUint)
      << "kind mismatch on `" << r.source << "`";
  EXPECT_EQ(*r.ours.AsUint(), r.oracle.uint64_value())
      << "VALUE DIVERGENCE (Uint)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`";
}

void DoubleEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::Double(), seed, depth, "Double", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kDouble)
      << "kind mismatch on `" << r.source << "`";
  const double ours_d = *r.ours.AsDouble();
  const double oracle_d = r.oracle.double_value();
  // NaN-equality: NaN != NaN in IEEE 754, so a plain `==` would
  // flag matched NaNs as divergent.  Match cel-cpp's conformance
  // discipline: if both are NaN, they agree; otherwise compare
  // bit-exact (which catches +0/-0 distinctness and +inf/-inf).
  if (std::isnan(ours_d) && std::isnan(oracle_d)) return;
  EXPECT_EQ(ours_d, oracle_d)
      << "VALUE DIVERGENCE (Double)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`"
      << "\n  ours   = " << ours_d << "\n  oracle = " << oracle_d;
}

void StringEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::String(), seed, depth, "String", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kString)
      << "kind mismatch on `" << r.source << "`";
  EXPECT_EQ(*r.ours.AsString(), r.oracle.string_value())
      << "VALUE DIVERGENCE (String)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`";
}

void BytesEvalAgreesWithOracle(uint64_t seed, int depth) {
  GenAndEvalResult r;
  if (!GenAndEval(CelType::Bytes(), seed, depth, "Bytes", r)) return;
  ASSERT_EQ(r.ours.kind(), Value::Kind::kBytes)
      << "kind mismatch on `" << r.source << "`";
  EXPECT_EQ(*r.ours.AsBytes(), r.oracle.bytes_value())
      << "VALUE DIVERGENCE (Bytes)\n  seed=" << seed << " depth=" << depth
      << "\n  source=`" << r.source << "`";
}

// ── FUZZ_TEST registrations ──────────────────────────────────────
//
// Six properties, one per scalar target type.  Each runs ~1000
// random iterations in unit-test mode; under `--config=fuzztest`
// the same registration becomes a coverage-guided fuzzer over
// that target.  Depth 0..8: 7-8 are the deep-nesting band the
// 2026-06-11 mining session opened (capacity rejects there are
// skipped, see `GenAndEval`).

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
