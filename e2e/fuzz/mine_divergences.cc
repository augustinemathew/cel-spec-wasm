// Mine oracle divergences over a target type by sequential seed.
// Prints each divergence with its seed + source + ours-vs-oracle
// values; intended for triaging what the property test catches
// when fuzztest's unit-test mode buffers the EXPECT_EQ message.
// Run as:
//   bazel run //e2e/fuzz:mine_divergences -- <target> <max_seeds> <depth> [stop_after]
// `target` is one of bool / int / uint / double / string / bytes.
// `stop_after` (default 5) caps how many divergences before exit.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/value.pb.h"
#include "e2e/fuzz/compare.h"
#include "e2e/fuzz/oracle_harness.h"
#include "eval/value.h"
#include "shared/type.h"

using ::celwasm::CelType;
using ::celwasm::fuzz::Compare;
using ::celwasm::fuzz::CompareResult;
using ::celwasm::fuzz::GenAndEvalFull;
using ::celwasm::fuzz::GenAndEvalResult;
using ::celwasm::fuzz::GenAndEvalStatus;

namespace {

CelType ParseTarget(absl::string_view s) {
  if (s == "bool") return CelType::Bool();
  if (s == "int") return CelType::Int();
  if (s == "uint") return CelType::Uint();
  if (s == "double") return CelType::Double();
  if (s == "string") return CelType::String();
  if (s == "bytes") return CelType::Bytes();
  if (s == "list_int") return CelType::List(CelType::Int());
  if (s == "list_bool") return CelType::List(CelType::Bool());
  if (s == "list_double") return CelType::List(CelType::Double());
  if (s == "list_string") return CelType::List(CelType::String());
  if (s == "map_string_int") {
    return CelType::Map(CelType::String(), CelType::Int());
  }
  std::cerr << "unknown target `" << s << "`\n";
  std::exit(2);
}

// Returns true on divergence (printed to stdout); false otherwise.
bool CompareAndReport(absl::string_view kind_label, const CelType& target,
                      uint64_t seed, const GenAndEvalResult& r) {
  const CompareResult c = Compare(r.ours, r.oracle, target);
  if (!c.equal) {
    std::printf("DIVERGE [%s seed=%llu]\n", std::string(kind_label).c_str(),
                static_cast<unsigned long long>(seed));
    std::printf("  source = %s\n", r.source.c_str());
    std::printf("  ours   = %s\n", c.ours.c_str());
    std::printf("  oracle = %s\n", c.oracle.c_str());
    std::fflush(stdout);
  }
  return !c.equal;
}

// One-line anomaly report for the non-compared outcomes.
void PrintAnomaly(absl::string_view tag, absl::string_view target_str,
                  uint64_t seed, const std::string& err,
                  const std::string& source) {
  std::printf("%s [%s seed=%llu] %s\n  source = %s\n", std::string(tag).c_str(),
              std::string(target_str).c_str(),
              static_cast<unsigned long long>(seed), err.c_str(),
              source.c_str());
  std::fflush(stdout);
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
}

// Mining loop: generate seeds 1..max_seeds, run each through the
// differential harness, print every anomaly, stop once
// `stop_after` divergences + our-rejects have been reported.
int RunMine(absl::string_view target_str, const CelType& target,
            uint64_t max_seeds, int depth, int stop_after) {
  Counters c;

  for (uint64_t seed = 1; seed <= max_seeds; ++seed) {
    GenAndEvalResult r;
    std::string err;
    GenAndEvalStatus st = GenAndEvalFull(target, seed, depth, r, &err);
    switch (st) {
      case GenAndEvalStatus::kOk:
        if (CompareAndReport(target_str, target, seed, r)) {
          ++c.diverged;
        } else {
          ++c.agreed;
        }
        break;
      case GenAndEvalStatus::kSourceTooLarge:
        ++c.too_large;
        break;
      case GenAndEvalStatus::kOurPipelineRejected:
        ++c.our_rejected;
        std::printf("OUR-REJECT [%s seed=%llu] %s\n  source = %s\n",
                    std::string(target_str).c_str(),
                    static_cast<unsigned long long>(seed), err.c_str(),
                    r.source.c_str());
        std::fflush(stdout);
        break;
      case GenAndEvalStatus::kOracleRejected:
        ++c.oracle_rejected;
        std::printf("ORACLE-REJECT [%s seed=%llu] %s\n  source = %s\n",
                    std::string(target_str).c_str(),
                    static_cast<unsigned long long>(seed), err.c_str(),
                    r.source.c_str());
        std::fflush(stdout);
        break;
      case GenAndEvalStatus::kBothErrored:
        ++c.both_errored;
        break;
      case GenAndEvalStatus::kOracleErrorOnly:
        ++c.diverged;
        std::printf(
            "ERROR-DIVERGE [%s seed=%llu] oracle errored, ours is a "
            "value: %s\n  source = %s\n",
            std::string(target_str).c_str(),
            static_cast<unsigned long long>(seed), err.c_str(),
            r.source.c_str());
        std::fflush(stdout);
        break;
    }
    if (c.diverged + c.our_rejected >= stop_after) {
      break;
    }
  }

  PrintSummary(target_str, depth, c);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || argc > 5) {
    std::cerr << "usage: " << argv[0]
              << " <target> <max_seeds> <depth> [stop_after=5]\n";
    return 1;
  }
  try {
    const absl::string_view target_str = argv[1];
    const CelType target = ParseTarget(target_str);
    const uint64_t max_seeds = std::strtoull(argv[2], nullptr, 10);
    const int depth = static_cast<int>(std::strtol(argv[3], nullptr, 10));
    const int stop_after =
        (argc == 5) ? static_cast<int>(std::strtol(argv[4], nullptr, 10)) : 5;
    return RunMine(target_str, target, max_seeds, depth, stop_after);
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 3;
  }
}
