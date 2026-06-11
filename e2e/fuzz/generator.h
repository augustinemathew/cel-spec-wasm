#ifndef CELWASM_E2E_FUZZ_GENERATOR_H_
#define CELWASM_E2E_FUZZ_GENERATOR_H_

// Type-directed CEL source generator.  Walks a `Grammar` post-
// L1-validation, picking productions weighted by `Production::
// weight` and recursing into each declared placeholder's
// `arg_type`.  Termination is by depth-budget: at
// `ctx.depth_budget == 0` only `is_leaf == true` productions are
// eligible.
//
// This is the permanent home for the recursion that landed
// inline in `grammar_test.cc` as the L3 walker; that copy stays
// for L3's gtest-only "does the grammar self-compose?" check.
// This file is what the oracle-property `FUZZ_TEST` invokes.

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "e2e/fuzz/grammar.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// Per-recursion state.  Threaded through `GenerateExpr`'s
// recursive descent.
struct GenCtx {
  // Remaining depth.  Decremented by one each recursion; at zero,
  // only leaf productions are eligible.  Negative values are
  // treated as zero.
  int depth_budget;

  // Variables bound in lexical scope at the current recursion
  // point.  The activation's free variables sit here from the
  // start (see `NewGenCtx`); comprehension productions
  // push their iter_var only while their body subtree is being
  // generated, then the caller pops.
  std::vector<std::pair<std::string, CelType>> in_scope;

  // Deterministic RNG.  Driven by a 64-bit seed the caller picks
  // (the fuzztest property hands in `Arbitrary<uint64_t>`).
  std::mt19937_64* rng;
};

// Generate a CEL source string of type `target` against
// `grammar` using `ctx` for depth/RNG state.  Recursion
// terminates at `ctx.depth_budget == 0` by picking a leaf;
// ABSL_CHECKs if the grammar lacks an eligible production
// (which `Grammar::Validate` should have caught at construction
// time, so this is an internal invariant violation, not a
// runtime error).
std::string GenerateExpr(const Grammar& grammar, const CelType& target,
                         GenCtx& ctx);

// Initialise a `GenCtx` with the fuzz activation's bindings
// pre-populated in `in_scope`.  Convenience for callers that
// want the standard activation vocab without rebuilding it by hand.
GenCtx NewGenCtx(int depth, std::mt19937_64& rng);

}  // namespace celwasm::fuzz

#endif  // CELWASM_E2E_FUZZ_GENERATOR_H_
