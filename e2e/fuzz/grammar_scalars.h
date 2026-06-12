#ifndef CELWASM_E2E_FUZZ_GRAMMAR_SCALARS_H_
#define CELWASM_E2E_FUZZ_GRAMMAR_SCALARS_H_

// Scalar catalog — the typed-attribute grammar core for the
// CEL source generator.  Covers bool / int / uint / double /
// string / bytes with the following AST kinds:
//
//   - kConstant (every scalar sub-kind)
//   - kIdentExpr (against the fixed activation declared below)
//   - kCallExpr — arithmetic (+, -, *; no / or %), comparison,
//     logical, concat, size, ternary, safe type conversions
//
// kListExpr / kMapExpr / kStructExpr / kComprehensionExpr arrive
// in the aggregate catalog.
//
// Per m27 §"Guarded productions": all rules registered here are
// **total over their typed input domain** given the constants the
// leaf productions can emit.  No divisions, no unrestricted
// indexing, no string→int, no bytes→string, no uint underflow.
// L2 (`grammar_test.cc`) verifies each production individually
// type-checks against the real cel-cpp pipeline.

#include <random>
#include <string>
#include <utility>
#include <vector>

#include "e2e/fuzz/grammar.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// The fixed activation the scalar grammar references via its
// `*_ident` leaf productions.  L2/L3 use the same list to build
// the `CheckOptions::variable_specs` so the cel-cpp checker sees
// the same bindings the generator picks names from.
//
// Names are intentionally short but disambiguated by type so the
// emitted source stays readable.  Collisions with single-letter
// activations in other test files (e.g.
// `e2e/slot_aliasing_test.cc`'s `a..h`) are not a concern — each
// test binary has its own activation; the grammar's idents are
// just strings the generator substitutes.
struct ActivationBinding {
  std::string name;
  CelType type;
};

// Returns the canonical fuzz activation.  Re-evaluated on each
// call; cheap.
std::vector<ActivationBinding> ActivationSchema();

// Initialise a `GenCtx` with the fuzz activation's bindings
// pre-populated in `in_scope`.  Lives beside `ActivationSchema`
// (the data it wraps) so the grammar engine stays catalog-agnostic.
GenCtx NewGenCtx(int depth, std::mt19937_64& rng);

// Registers the scalar production catalog onto `b` WITHOUT
// finalising / validating.  Used by `grammar_aggregates.cc` to
// layer aggregate + comprehension rules on top of the scalar
// scalar core in a single coherent grammar.  Callers are
// expected to validate (via `Grammar::Validate`) once they're
// done adding productions.
void RegisterScalarProductions(GrammarBuilder& b);

}  // namespace celwasm::fuzz

#endif  // CELWASM_E2E_FUZZ_GRAMMAR_SCALARS_H_
