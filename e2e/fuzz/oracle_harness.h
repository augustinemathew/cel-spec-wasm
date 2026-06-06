// Shared harness for value-oracle differential testing.  Wraps the
// generator → our pipeline → cel-cpp oracle round-trip used by
// `cel_oracle_property_test` (fuzztest-driven) and `mine_divergences`
// (loop-driven diagnostic).  Factored out so divergence-mining
// binaries get the same compiler/engine/activation as the property
// test by construction.

#ifndef CELWASM_E2E_FUZZ_ORACLE_HARNESS_H_
#define CELWASM_E2E_FUZZ_ORACLE_HARNESS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "cel/expr/value.pb.h"
#include "eval/value.h"
#include "shared/type.h"
#include "testdata/cel_cpp_oracle.h"

namespace celwasm::fuzz {

// Cap on per-iteration source size — long sources type-check but
// stress our wasm lowering more than they exercise new shapes.
inline constexpr std::size_t kMaxSourceBytes = 4 * 1024;

// One bound activation entry, in both value representations.
struct BoundActivation {
  std::string name;
  CelType type;
  Value ours;
  cel::expr::Value oracle;
};

// Concrete values bound to the Slice B activation, in our and
// cel-cpp representations.  Built once.
const std::vector<BoundActivation>& SliceBBoundActivation();

// cel-cpp oracle-variable list mirroring `SliceBBoundActivation`.
std::vector<testdata::OracleVar> MakeOracleVars();

// Run `source` through our Compile → Plan → Eval pipeline with the
// Slice B activation bound.
absl::StatusOr<Value> OurEval(absl::string_view source);

// One round-trip result.  `source` is the grammar-emitted CEL
// source; `ours` is our evaluator's value; `oracle` is cel-cpp's.
struct GenAndEvalResult {
  std::string source;
  Value ours;
  cel::expr::Value oracle;
};

// Possible outcomes of `GenAndEvalSliceC`.
enum class GenAndEvalStatus {
  kOk,                  // Both sides accepted and produced a non-error value.
  kSourceTooLarge,      // Source exceeded `kMaxSourceBytes`.
  kOurPipelineRejected, // Our compile/plan/eval returned a non-ok status.
  kOracleRejected,      // cel-cpp returned a non-ok status.
  kOracleErrorValue,    // cel-cpp evaluated but produced a CEL error.
};

// Generate one source of type `target` at the given depth/seed,
// run it through both pipelines.  Returns the status of the
// attempt — only `kOk` yields a populated `out`.  On non-ok
// statuses, `out.source` is still populated so the caller can
// log it.  `error_out`, when non-null, receives a human-readable
// description on the rejecting/erroring statuses.
GenAndEvalStatus GenAndEvalSliceC(const CelType& target, uint64_t seed,
                                  int depth, GenAndEvalResult& out,
                                  std::string* error_out = nullptr);

}  // namespace celwasm::fuzz

#endif  // CELWASM_E2E_FUZZ_ORACLE_HARNESS_H_
