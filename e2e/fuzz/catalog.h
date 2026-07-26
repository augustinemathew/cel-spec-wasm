#ifndef CELWASM_E2E_FUZZ_CATALOG_H_
#define CELWASM_E2E_FUZZ_CATALOG_H_

// The production catalog — WHAT generated expressions look like.
// Productions are data rows registered onto a `GrammarBuilder`,
// grouped by family (one `catalog_<family>.cc` per group);
// `BuildGrammar()` composes them into THE grammar the drivers mine.
//
// Per m27 §"Guarded productions": every total rule registered here
// is total over its typed input domain; fallible rules (division,
// parse conversions, range-checked arithmetic) are admitted because
// error-ness is a COMPARED dimension (both-error = agreement).
// L2 (`grammar_test.cc`) verifies each production individually
// type-checks against the real cel-cpp pipeline.
//
// A production deliberately NOT generated because cel-cpp disagrees
// (an over-permissiveness on our side, an oracle gap) is a
// WITHHELD row: delete it, leave `// WITHHELD: <PbtTest>` citing the
// `known_bugs_test.cc` pin.  Grep `WITHHELD:` for the inventory.

#include <random>
#include <string>
#include <vector>

#include "e2e/fuzz/grammar.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// ── Activation ───────────────────────────────────────────────────

// The fixed activation the grammar references via its `*_ident`
// leaf productions.  L2/L3 use the same list to build the
// `CheckOptions::variable_specs` so the cel-cpp checker sees the
// same bindings the generator picks names from.  Values are bound
// in `oracle_harness.cc::MakeEntry` — a schema entry without a
// value CHECK-fails at first use, so the two cannot drift.
struct ActivationBinding {
  std::string name;
  CelType type;
};

// Returns the canonical fuzz activation.  Re-evaluated on each
// call; cheap.
std::vector<ActivationBinding> ActivationSchema();

// Initialise a `GenCtx` with the fuzz activation's bindings
// pre-populated in `in_scope`.
GenCtx NewGenCtx(int depth, std::mt19937_64& rng);

// ── Families ─────────────────────────────────────────────────────
// One registration function per data row group; definitions live in
// `catalog_<family>.cc`.  These exist to be composed by
// `BuildGrammar()` — call them individually only in tests.

// catalog_leaves.cc — adversarial scalar leaf values.
void RegisterNumericLeaves(GrammarBuilder& b);
void RegisterLexicalLeaves(GrammarBuilder& b);

// catalog_ops.cc — operators over scalars.
void RegisterArithmetic(GrammarBuilder& b);
void RegisterFallibleArithmetic(GrammarBuilder& b);
void RegisterMathExt(GrammarBuilder& b);
void RegisterBoolProducers(GrammarBuilder& b);
void RegisterMixedTotalOps(GrammarBuilder& b);
void RegisterConversions(GrammarBuilder& b);

// catalog_strings.cc — string functions, format, split/join.
void RegisterStringFunctions(GrammarBuilder& b);
void RegisterStringFormat(GrammarBuilder& b);
void RegisterStringAggregateFunctions(GrammarBuilder& b);

// catalog_temporal.cc — timestamp/duration leaves + accessors.
void RegisterTemporal(GrammarBuilder& b);

// catalog_aggregates.cc — list/map literals, size, in,
// comprehensions, nested aggregates.
void RegisterListLeaves(GrammarBuilder& b);
void RegisterListConstructors(GrammarBuilder& b);
void RegisterMapConstructors(GrammarBuilder& b);
void RegisterSizeProductions(GrammarBuilder& b);
void RegisterInProductions(GrammarBuilder& b);
void RegisterListComprehensions(GrammarBuilder& b);
void RegisterMapComprehensions(GrammarBuilder& b);
void RegisterNestedAggregates(GrammarBuilder& b);

// ── Composition ──────────────────────────────────────────────────

// Builds THE grammar (every family, fixed order).  ABSL_CHECKs
// internally if `Grammar::Validate` (L1) rejects the catalog.
Grammar BuildGrammar();

}  // namespace celwasm::fuzz

#endif  // CELWASM_E2E_FUZZ_CATALOG_H_
