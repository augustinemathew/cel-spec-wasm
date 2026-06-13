// Mine oracle divergences over a target type by sequential seed.
// Prints each divergence with its seed + source + ours-vs-oracle
// values, a `--- summary ---` block, and a machine-readable
// `RESULT ...` line.  **Exits non-zero (= divergence count, capped
// at 125) iff a value/error divergence was found** — so it is
// CI-gateable.  `scripts/fuzz.sh` wraps the common invocations.
//
// Run as:
//   bazel run //e2e/fuzz:mine_divergences -- <target> <max_seeds> <depth> [stop_after]
// `target` is any name in `targets.cc` (`AllTargets()`).
// `stop_after` (default 5) caps how many divergences+our-rejects
// before early exit.
//
// All judging happens in `verdict.cc::RunOne` — this file is only
// the loop, the tallies, and the exit-code contract.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "absl/strings/string_view.h"
#include "e2e/fuzz/targets.h"
#include "e2e/fuzz/verdict.h"
#include "shared/type.h"

using ::celwasm::CelType;
using ::celwasm::fuzz::RunOne;
using ::celwasm::fuzz::Verdict;
using ::celwasm::fuzz::VerdictKind;

namespace {

CelType ParseTargetOrDie(absl::string_view s) {
  std::optional<CelType> t = celwasm::fuzz::ParseTarget(s);
  if (!t.has_value()) {
    std::cerr << "unknown target `" << s << "`\n";
    std::exit(2);
  }
  return *t;
}

// Per-outcome tallies for one mining run.
struct Counters {
  int diverged = 0;
  int our_rejected = 0;
  int oracle_rejected = 0;
  int both_errored = 0;
  int too_large = 0;
  int agreed = 0;
};

void PrintSummary(absl::string_view target_str, int depth, const Counters& c) {
  std::printf("\n--- summary [%s, depth=%d] ---\n",
              std::string(target_str).c_str(), depth);
  std::printf(
      "agreed=%d  diverged=%d  our_rejected=%d  oracle_rejected=%d  "
      "both_errored=%d  too_large=%d\n",
      c.agreed, c.diverged, c.our_rejected, c.oracle_rejected, c.both_errored,
      c.too_large);
  // Machine-readable line (stable prefix `RESULT `) for scripts/CI
  // to grep — see scripts/fuzz.sh.
  std::printf(
      "RESULT target=%s depth=%d agreed=%d diverged=%d our_rejected=%d "
      "oracle_rejected=%d both_errored=%d too_large=%d\n",
      std::string(target_str).c_str(), depth, c.agreed, c.diverged,
      c.our_rejected, c.oracle_rejected, c.both_errored, c.too_large);
  std::fflush(stdout);
}

// Tally one verdict; print the report for every anomalous outcome
// (divergences and rejects — agreed/both-errored/too-large are
// silent).  Returns the updated divergence+reject stop counter.
void Tally(const Verdict& v, absl::string_view target_str, Counters& c) {
  switch (v.kind) {
    case VerdictKind::kAgreed:
      ++c.agreed;
      return;
    case VerdictKind::kValueDiverged:
    case VerdictKind::kOracleErrorOnly:
      ++c.diverged;
      break;
    case VerdictKind::kOurCapacityReject:
    case VerdictKind::kOurUnexpectedReject:
      ++c.our_rejected;
      break;
    case VerdictKind::kOracleRejected:
      ++c.oracle_rejected;
      break;
    case VerdictKind::kBothErrored:
      ++c.both_errored;
      return;
    case VerdictKind::kSourceTooLarge:
      ++c.too_large;
      return;
  }
  std::fputs(v.Report(target_str).c_str(), stdout);
  std::fflush(stdout);
}

int RunMine(absl::string_view target_str, const CelType& target,
            uint64_t max_seeds, int depth, int stop_after) {
  Counters c;
  for (uint64_t seed = 1; seed <= max_seeds; ++seed) {
    Tally(RunOne(target, seed, depth), target_str, c);
    if (c.diverged + c.our_rejected >= stop_after) break;
  }
  PrintSummary(target_str, depth, c);
  // CI-gateable exit code: non-zero iff a value/error divergence was
  // found (our-rejects and both-errored are NOT failures — see
  // README "Reading the summary").  Clamped to 125 so it never
  // collides with the shell's 126/127/128+signal range.
  return c.diverged > 125 ? 125 : c.diverged;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  // `--list-targets` prints the canonical target names, one per
  // line — scripts/fuzz.sh derives its sweep list from this so the
  // shell never carries its own copy.
  if (argc == 2 && absl::string_view(argv[1]) == "--list-targets") {
    for (const celwasm::fuzz::NamedTarget& t : celwasm::fuzz::AllTargets()) {
      std::cout << t.name << "\n";
    }
    return 0;
  }
  if (argc < 4 || argc > 5) {
    std::cerr << "usage: " << argv[0]
              << " <target> <max_seeds> <depth> [stop_after=5]\n"
              << "       " << argv[0] << " --list-targets\n";
    return 1;
  }
  const absl::string_view target_str = argv[1];
  const CelType target = ParseTargetOrDie(target_str);
  const uint64_t max_seeds = std::strtoull(argv[2], nullptr, 10);
  const int depth = static_cast<int>(std::strtol(argv[3], nullptr, 10));
  const int stop_after =
      (argc == 5) ? static_cast<int>(std::strtol(argv[4], nullptr, 10)) : 5;
  return RunMine(target_str, target, max_seeds, depth, stop_after);
}
