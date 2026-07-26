#ifndef CELWASM_E2E_FUZZ_VERDICT_H_
#define CELWASM_E2E_FUZZ_VERDICT_H_

// The ONE verdict path of the differential harness: classify a
// generate→eval-both-sides round-trip, compare payloads, and render
// the outcome.  Every driver (the `mine_divergences` CLI, the
// fuzztest property, future repro tools) judges through `RunOne` /
// `Judge` so they cannot disagree about what a failure is — the
// hand-rolled per-driver verdict switches this replaces did (an
// unexpected our-side rejection failed the property test but never
// the miner's exit code).

#include <cstdint>
#include <string>

#include "absl/strings/string_view.h"
#include "e2e/fuzz/oracle_harness.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// Outcome of one differential round-trip.  Refines
// `GenAndEvalStatus` in two ways: the compared `kOk` splits into
// agreed/diverged, and `kOurPipelineRejected` splits into the known
// static-window capacity rejection (`ResourceExhausted` — a skip)
// vs anything else (a finding).
enum class VerdictKind : std::uint8_t {
  kAgreed,               // both produced values; payloads compare equal
  kBothErrored,          // both evaluated to a CEL error — agreement
  kValueDiverged,        // both produced values; payloads differ
  kOracleErrorOnly,      // oracle errored, ours is a value — divergence
  kOurCapacityReject,    // ResourceExhausted static-window reject — skip
  kOurUnexpectedReject,  // any other our-side rejection — a finding
  kOracleRejected,       // cel-cpp harness rejected the source
  kSourceTooLarge,       // generated source exceeded kMaxSourceBytes
};

// One judged round-trip.  `detail` carries the outcome's diagnostic
// text: the want/got mismatch diff on kValueDiverged, the
// rejecting/erroring status text on the reject/error outcomes.
struct Verdict {
  VerdictKind kind = VerdictKind::kAgreed;
  uint64_t seed = 0;
  int depth = 0;
  std::string source;
  std::string detail;

  // The two engines disagree about the result of a valid expression
  // — the bug class this rig exists to find.
  bool IsDivergence() const {
    return kind == VerdictKind::kValueDiverged ||
           kind == VerdictKind::kOracleErrorOnly;
  }

  // Anything a driver should fail on: a divergence, an unexpected
  // our-side rejection, or an oracle rejection of grammar-emitted
  // (type-checked-by-construction) source.
  bool IsFailure() const {
    return IsDivergence() || kind == VerdictKind::kOurUnexpectedReject ||
           kind == VerdictKind::kOracleRejected;
  }

  // Multi-line human render, tagged with `label` (the CLI target
  // name / property name).  Format matches the historical miner
  // output (`DIVERGE [...]`, `ERROR-DIVERGE (...)`, `OUR-REJECT`,
  // `ORACLE-REJECT`) — scripts grep these prefixes.
  std::string Report(absl::string_view label) const;
};

// Pure classification: fold a `GenAndEvalFull` outcome into a
// Verdict (comparing payloads via the conformance gate's
// `CompareValue` when both sides produced one).  Exposed separately
// from `RunOne` so the classification matrix is unit-testable with
// synthetic results.
Verdict Judge(uint64_t seed, int depth, GenAndEvalStatus status,
              const GenAndEvalResult& result, const std::string& error);

// Generate one expression of `target` at (seed, depth), evaluate it
// through both pipelines, and judge the outcome.
Verdict RunOne(const CelType& target, uint64_t seed, int depth);

}  // namespace celwasm::fuzz

#endif  // CELWASM_E2E_FUZZ_VERDICT_H_
