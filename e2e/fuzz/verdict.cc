#include "e2e/fuzz/verdict.h"

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "e2e/fuzz/compare.h"
#include "e2e/fuzz/oracle_harness.h"
#include "shared/type.h"

namespace celwasm::fuzz {

std::string Verdict::Report(absl::string_view label) const {
  const std::string at = absl::StrCat("[", label, " seed=", seed, "]");
  switch (kind) {
    case VerdictKind::kValueDiverged:
      return absl::StrCat("DIVERGE ", at, "\n  source = ", source,
                          "\n  ours   = ", ours, "\n  oracle = ", oracle, "\n");
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

Verdict Judge(const CelType& target, uint64_t seed, int depth,
              GenAndEvalStatus status, const GenAndEvalResult& result,
              const std::string& error) {
  Verdict v;
  v.seed = seed;
  v.depth = depth;
  v.source = result.source;
  v.detail = error;
  switch (status) {
    case GenAndEvalStatus::kOk: {
      const CompareResult c = Compare(result.ours, result.oracle, target);
      v.kind = c.equal ? VerdictKind::kAgreed : VerdictKind::kValueDiverged;
      v.ours = c.ours;
      v.oracle = c.oracle;
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
  return Judge(target, seed, depth, status, result, error);
}

}  // namespace celwasm::fuzz
