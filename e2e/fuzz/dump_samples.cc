// Small CLI for previewing what the Slice C grammar generates.
// Run with `bazel run //e2e/fuzz:dump_samples -- <target> <depth> <count>`.
// `<target>` is one of: bool, int, uint, double, string, bytes,
// list_int, list_double, list_string, map_string_int.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

#include "absl/strings/string_view.h"
#include "e2e/fuzz/generator.h"
#include "e2e/fuzz/grammar.h"
#include "e2e/fuzz/grammar_slice_c.h"
#include "shared/type.h"

using celwasm::CelType;
using celwasm::fuzz::BuildSliceCGrammar;
using celwasm::fuzz::Grammar;
using celwasm::fuzz::GenCtx;
using celwasm::fuzz::GenerateExpr;
using celwasm::fuzz::NewGenCtxForSliceB;

CelType ParseTarget(absl::string_view s) {
  if (s == "bool")    return CelType::Bool();
  if (s == "int")     return CelType::Int();
  if (s == "uint")    return CelType::Uint();
  if (s == "double")  return CelType::Double();
  if (s == "string")  return CelType::String();
  if (s == "bytes")   return CelType::Bytes();
  if (s == "list_int")    return CelType::List(CelType::Int());
  if (s == "list_double") return CelType::List(CelType::Double());
  if (s == "list_string") return CelType::List(CelType::String());
  if (s == "list_bool")   return CelType::List(CelType::Bool());
  if (s == "map_string_int")
    return CelType::Map(CelType::String(), CelType::Int());
  std::cerr << "unknown target `" << s << "`\n";
  std::exit(2);
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: " << argv[0] << " <target> <depth> <count>\n";
    return 1;
  }
  const CelType target = ParseTarget(argv[1]);
  const int depth = std::atoi(argv[2]);
  const int count = std::atoi(argv[3]);

  const Grammar g = BuildSliceCGrammar();
  for (int seed = 1; seed <= count; ++seed) {
    std::mt19937_64 rng(static_cast<uint64_t>(seed));
    GenCtx ctx = NewGenCtxForSliceB(depth, rng);
    std::cout << "seed=" << seed << "  "
              << GenerateExpr(g, target, ctx) << "\n";
  }
  return 0;
}
