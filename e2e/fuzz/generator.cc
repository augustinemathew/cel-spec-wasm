#include "e2e/fuzz/generator.h"

#include <cstddef>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "e2e/fuzz/grammar.h"
#include "e2e/fuzz/grammar_slice_b.h"
#include "shared/type.h"

namespace celwasm::fuzz {

namespace {

// Pick a production from `rules` honouring `require_leaf` and
// per-production `weight`.  Returns nullptr only when the rule
// set has zero eligible production at the current filter (which
// L1's leaf-coverage check should have prevented for the
// require_leaf branch; we still ABSL_CHECK below).
const Production* PickProduction(const std::vector<Production>& rules,
                                  bool require_leaf, std::mt19937_64& rng) {
  int total_weight = 0;
  for (const Production& p : rules) {
    if (require_leaf && !p.is_leaf) continue;
    if (p.weight <= 0) continue;
    total_weight += p.weight;
  }
  if (total_weight == 0) return nullptr;
  std::uniform_int_distribution<int> dist(0, total_weight - 1);
  int pick = dist(rng);
  for (const Production& p : rules) {
    if (require_leaf && !p.is_leaf) continue;
    if (p.weight <= 0) continue;
    pick -= p.weight;
    if (pick < 0) return &p;
  }
  return nullptr;  // unreachable
}

}  // namespace

std::string GenerateExpr(const Grammar& grammar, const CelType& target,
                          GenCtx& ctx) {
  const auto& rules = grammar.Rules(target);
  ABSL_CHECK(!rules.empty())
      << "GenerateExpr: no productions registered for target type `"
      << TypeKey(target) << "`; grammar is missing a rule set";

  // At depth 0 only leaves are eligible.  Otherwise pick from the
  // full set.  If we somehow exhaust eligible options at non-zero
  // depth (zero non-leaf weights), fall back to leaves.
  const bool require_leaf = ctx.depth_budget <= 0;
  const Production* p = PickProduction(rules, require_leaf, *ctx.rng);
  if (p == nullptr && !require_leaf) {
    p = PickProduction(rules, /*require_leaf=*/true, *ctx.rng);
  }
  ABSL_CHECK(p != nullptr)
      << "GenerateExpr: no eligible production for target `"
      << TypeKey(target) << "` (depth=" << ctx.depth_budget
      << "); L1 should have caught this";

  if (p->arg_types.empty()) {
    return p->format;
  }
  // Recursive case: walk each placeholder slot.
  std::string out = p->format;
  for (std::size_t i = 0; i < p->arg_types.size(); ++i) {
    GenCtx sub = ctx;
    sub.depth_budget = ctx.depth_budget - 1;
    if (i < p->extra_scope_for_arg.size()) {
      for (const auto& binding : p->extra_scope_for_arg[i]) {
        sub.in_scope.push_back(binding);
      }
    }
    const std::string arg = GenerateExpr(grammar, p->arg_types[i], sub);
    out = absl::StrReplaceAll(out, {{absl::StrCat("%", i), arg}});
  }
  return out;
}

GenCtx NewGenCtxForSliceB(int depth, std::mt19937_64& rng) {
  GenCtx ctx;
  ctx.depth_budget = depth;
  ctx.rng = &rng;
  for (const ActivationBinding& v : SliceBActivation()) {
    ctx.in_scope.push_back({v.name, v.type});
  }
  return ctx;
}

}  // namespace celwasm::fuzz
