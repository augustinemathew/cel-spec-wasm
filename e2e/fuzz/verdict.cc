#include "e2e/fuzz/verdict.h"

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "conformance/runner.h"
#include "e2e/fuzz/oracle_harness.h"
#include "shared/type.h"

namespace celwasm::fuzz {

std::string Verdict::Report(absl::string_view label) const {
  const std::string at = absl::StrCat("[", label, " seed=", seed, "]");
  switch (kind) {
    case VerdictKind::kValueDiverged:
      return absl::StrCat("DIVERGE ", at, "\n  source = ", source,
                          "\n  mismatch = ", detail, "\n");
    case VerdictKind::kOracleErrorOnly:
      return absl::StrCat("ERROR-DIVERGE (oracle errored, ours is a value) ",
                          at, " ", detail, "\n  source = ", source, "\n");
    case VerdictKind::kOurCapacityReject:
    case VerdictKind::kOurUnexpectedReject:
      return absl::StrCat("OUR-REJECT ", at, " ", detail,
                          "\n  source = ", source, "\n");
    case VerdictKind::kOracleRejected:
      return absl::StrCat("ORACLE-REJECT ", at, " ", detail,
                          "\n  source = ", source, "\n");
    case VerdictKind::kAgreed:
      return absl::StrCat("agreed ", at, " ", source, "\n");
    case VerdictKind::kBothErrored:
      return absl::StrCat("both-errored ", at, " ", source, "\n");
    case VerdictKind::kSourceTooLarge:
      return absl::StrCat("too-large ", at, "\n");
  }
  return absl::StrCat("<bad VerdictKind> ", at, "\n");
}

Verdict Judge(uint64_t seed, int depth, GenAndEvalStatus status,
              const GenAndEvalResult& result, const std::string& error) {
  Verdict v;
  v.seed = seed;
  v.depth = depth;
  v.source = result.source;
  v.detail = error;
  switch (status) {
    case GenAndEvalStatus::kOk: {
      // The conformance gate's own comparator: NaN-matches-NaN,
      // list/map recursion, kind mismatch fails.  Any non-OK status
      // (including InvalidArgument for a kind it has no comparator
      // for) is a divergence — a comparator gap must never
      // masquerade as agreement.  Its message carries the want/got
      // diff, which is the divergence render.
      const absl::Status cmp =
          conformance::CompareValue(result.ours, result.oracle);
      v.kind = cmp.ok() ? VerdictKind::kAgreed : VerdictKind::kValueDiverged;
      if (!cmp.ok()) v.detail = std::string(cmp.message());
      return v;
    }
    case GenAndEvalStatus::kSourceTooLarge:
      v.kind = VerdictKind::kSourceTooLarge;
      return v;
    case GenAndEvalStatus::kOurPipelineRejected:
      // The static-window capacity rejection is a known limitation at
      // high depth (see kMaxSourceBytes / the property-test header),
      // not a finding; anything else rejecting grammar-emitted
      // (type-checked-by-construction) source is.
      v.kind = absl::IsResourceExhausted(result.our_status)
                   ? VerdictKind::kOurCapacityReject
                   : VerdictKind::kOurUnexpectedReject;
      return v;
    case GenAndEvalStatus::kOracleRejected:
      v.kind = VerdictKind::kOracleRejected;
      return v;
    case GenAndEvalStatus::kBothErrored:
      v.kind = VerdictKind::kBothErrored;
      return v;
    case GenAndEvalStatus::kOracleErrorOnly:
      v.kind = VerdictKind::kOracleErrorOnly;
      return v;
  }
  v.kind = VerdictKind::kOurUnexpectedReject;
  v.detail = "unhandled GenAndEvalStatus";
  return v;
}

Verdict RunOne(const CelType& target, uint64_t seed, int depth) {
  GenAndEvalResult result;
  std::string error;
  const GenAndEvalStatus status =
      GenAndEvalFull(target, seed, depth, result, &error);
  return Judge(seed, depth, status, result, error);
}

}  // namespace celwasm::fuzz
