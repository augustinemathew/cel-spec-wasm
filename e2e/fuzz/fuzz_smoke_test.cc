// Smoke test confirming the FuzzTest bazel-module wiring works.
//
// In unit-test mode (`bazel test //e2e/fuzz:fuzz_smoke_test`) FuzzTest
// runs ~1000 randomised inputs through each `FUZZ_TEST` property and
// reports failures through the usual googletest reporter.  In fuzzing
// mode (`bazel run --config=fuzztest //e2e/fuzz:fuzz_smoke_test --
// --fuzz=FuzzSmokeTest.SizeIsNonNegative`) the same binary turns into
// a coverage-guided fuzzer.
//
// The property here is intentionally trivial — `std::string_view::size()`
// is unsigned, so the inequality always holds.  The point is to prove
// the build/link/run pipeline, not to find a bug.  Replace the body
// with a real CEL-expression generator + round-trip check once the
// wiring is confirmed.

#include <string_view>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace {

void SizeIsNonNegative(std::string_view s) {
  // `size()` returns `std::size_t`, so this is trivially true; the
  // FUZZ_TEST harness still exercises the input-generation + property-
  // dispatch path, which is what we want to smoke-test.
  EXPECT_GE(s.size(), 0u);
}
FUZZ_TEST(FuzzSmokeTest, SizeIsNonNegative);

}  // namespace
