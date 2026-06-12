#include "e2e/fuzz/catalog.h"

#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "e2e/fuzz/grammar.h"
#include "shared/type.h"

namespace celwasm::fuzz {

namespace {

// Register one Leaf production per (name, type) pair in the
// activation — these are the kIdent leaves the generator can
// emit at any depth.
void RegisterIdentLeaves(GrammarBuilder& b,
                         const std::vector<ActivationBinding>& activation) {
  for (const ActivationBinding& v : activation) {
    b.Leaf(v.type, /*name=*/v.name + "_ident", /*format=*/v.name);
  }
}

}  // namespace

// ── Activation ───────────────────────────────────────────────────

std::vector<ActivationBinding> ActivationSchema() {
  // Two int vars so binary arithmetic on idents has a non-trivial
  // shape; one of each other scalar.  No collision-prone single-
  // letter names — every name carries its type tag, which makes
  // the generated CEL source self-describing in failure messages.
  //
  // `xs` / `ms` are the bound-aggregate entries from the m27
  // vocabulary table: an activation-bound list/map reaches the
  // host-origin aggregate paths (`cel_list_in` over bound data,
  // host map lookup) that literal aggregates — arena-built every
  // eval — never touch.  Their ident leaves feed every existing
  // list<int> / map<string,int> production (size, _in_,
  // comprehension ranges) with no new grammar rules.
  return {
      {"i_a", CelType::Int()},
      {"i_b", CelType::Int()},
      {"u_a", CelType::Uint()},
      {"d_a", CelType::Double()},
      {"b_a", CelType::Bool()},
      {"s_a", CelType::String()},
      {"y_a", CelType::Bytes()},
      {"xs", CelType::List(CelType::Int())},
      {"ms", CelType::Map(CelType::String(), CelType::Int())},
  };
}

GenCtx NewGenCtx(int depth, std::mt19937_64& rng) {
  GenCtx ctx;
  ctx.depth_budget = depth;
  ctx.rng = &rng;
  for (const ActivationBinding& v : ActivationSchema()) {
    ctx.in_scope.emplace_back(v.name, v.type);
  }
  return ctx;
}

// ── Composition ──────────────────────────────────────────────────
//
// ORDER IS GENERATION-AFFECTING.  Registration order fixes the
// rules-vector order `PickProduction` samples from, so reordering
// these calls changes every seed's output and invalidates pinned
// repros / fixed-seed baselines.  Append new families at the END;
// never re-sort.  (The sequence below is the historical accretion
// order, preserved verbatim when the catalog was split by family.)
Grammar BuildGrammar() {
  GrammarBuilder b;

  RegisterNumericLeaves(b);
  RegisterLexicalLeaves(b);
  RegisterIdentLeaves(b, ActivationSchema());
  RegisterArithmetic(b);
  RegisterFallibleArithmetic(b);
  RegisterStringFunctions(b);
  RegisterStringFormat(b);
  RegisterMathExt(b);
  RegisterTemporal(b);
  RegisterBoolProducers(b);
  RegisterMixedTotalOps(b);
  RegisterConversions(b);
  RegisterListLeaves(b);
  RegisterListConstructors(b);
  RegisterMapConstructors(b);
  RegisterSizeProductions(b);
  RegisterInProductions(b);
  RegisterListComprehensions(b);
  RegisterMapComprehensions(b);
  RegisterNestedAggregates(b);
  RegisterStringAggregateFunctions(b);

  Grammar g = std::move(b).Build();
  ABSL_CHECK_OK(g.Validate())
      << "the grammar failed L1 validation; a catalog_*.cc file is "
         "malformed";
  return g;
}

}  // namespace celwasm::fuzz
