// Small CLI for previewing what the full grammar generates.
// Run with `bazel run //e2e/fuzz:dump_samples -- <target> <depth> <count>`.
// `<target>` is any mineable target (see `targets.h` / `ParseTarget`).

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <string>

#include "absl/strings/string_view.h"
#include "e2e/fuzz/generator.h"
#include "e2e/fuzz/grammar.h"
#include "e2e/fuzz/grammar_aggregates.h"
#include "e2e/fuzz/targets.h"
#include "shared/type.h"

using celwasm::CelType;
using celwasm::fuzz::BuildFullGrammar;
using celwasm::fuzz::GenCtx;
using celwasm::fuzz::GenerateExpr;
using celwasm::fuzz::Grammar;
using celwasm::fuzz::NewGenCtx;

namespace {

CelType ParseTargetOrDie(absl::string_view s) {
  std::optional<CelType> t = celwasm::fuzz::ParseTarget(s);
  if (!t.has_value()) {
    std::cerr << "unknown target `" << s << "`\n";
    std::exit(2);
  }
  return *t;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  if (argc != 4) {
    std::cerr << "usage: " << argv[0] << " <target> <depth> <count>\n";
    return 1;
  }
  const CelType target = ParseTargetOrDie(argv[1]);
  const int depth = static_cast<int>(std::strtol(argv[2], nullptr, 10));
  const int count = static_cast<int>(std::strtol(argv[3], nullptr, 10));

  const Grammar g = BuildFullGrammar();
  for (int seed = 1; seed <= count; ++seed) {
    std::mt19937_64 rng(static_cast<uint64_t>(seed));
    GenCtx ctx = NewGenCtx(depth, rng);
    std::cout << "seed=" << seed << "  " << GenerateExpr(g, target, ctx)
              << "\n";
  }
  return 0;
}
