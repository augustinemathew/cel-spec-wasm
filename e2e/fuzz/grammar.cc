#include "e2e/fuzz/grammar.h"

#include <cstddef>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "shared/type.h"

namespace celwasm::fuzz {

namespace {

// Canonical CEL-spec form for primitives, parametric containers
// (`list<T>` / `map<K,V>` / `optional<T>`), and proto messages
// (their fully-qualified name).  Both `TypeKey` and `TypeSpec`
// route through this — they're identical in the current type
// vocabulary; the two entry points exist so a future divergence
// (e.g. a hash-friendly key vs. the human-facing spec string)
// can be done without churning callers.
std::string TypeCanonical(const CelType& t) {
  switch (t.kind()) {
    case CelType::Kind::kBool:
      return "bool";
    case CelType::Kind::kInt:
      return "int";
    case CelType::Kind::kUint:
      return "uint";
    case CelType::Kind::kDouble:
      return "double";
    case CelType::Kind::kString:
      return "string";
    case CelType::Kind::kBytes:
      return "bytes";
    case CelType::Kind::kDuration:
      return "duration";
    case CelType::Kind::kTimestamp:
      return "timestamp";
    case CelType::Kind::kType:
      return "type";
    case CelType::Kind::kMessage:
      return std::string(t.message_fully_qualified_name());
    case CelType::Kind::kList:
      return absl::StrCat("list<", TypeCanonical(t.list_element()), ">");
    case CelType::Kind::kMap:
      return absl::StrCat("map<", TypeCanonical(t.map_key()), ",",
                          TypeCanonical(t.map_value()), ">");
    case CelType::Kind::kUnknown:
      return "<unknown>";
    case CelType::Kind::kNull:
    case CelType::Kind::kOptional:
      // Signature-only kinds — the fuzz grammar's variable-type
      // generator never produces them.
      ABSL_CHECK(false) << "TypeCanonical: non-declarable CelType kind `"
                        << CelTypeKindName(t.kind()) << "`";
  }
  return "<unhandled-CelType-kind>";
}

}  // namespace

std::string TypeKey(const CelType& t) {
  return TypeCanonical(t);
}

std::string TypeSpec(const CelType& t) {
  return TypeCanonical(t);
}

// ── Grammar accessors ────────────────────────────────────────────

const std::vector<Production>& Grammar::Rules(const CelType& target) const {
  static const std::vector<Production>* kEmpty = new std::vector<Production>();
  auto it = rules_.find(TypeKey(target));
  return it == rules_.end() ? *kEmpty : it->second;
}

bool Grammar::HasType(const CelType& t) const {
  return rules_.contains(TypeKey(t));
}

std::size_t Grammar::TotalProductions() const {
  std::size_t n = 0;
  for (const auto& [_, ps] : rules_) {
    n += ps.size();
  }
  return n;
}

// ── L1 — static structural validation ────────────────────────────

namespace {

// Per-production structural checks (a)-(e); `Validate` adds the
// per-type checks around them.
absl::Status ValidateProduction(const Production& p, const Grammar& g,
                                const std::string& target_key) {
  // (a) Every declared placeholder appears in the format.
  for (std::size_t i = 0; i < p.arg_types.size(); ++i) {
    if (!absl::StrContains(p.format, absl::StrCat("%", i))) {
      return absl::InvalidArgumentError(absl::StrCat(
          "production `", p.name, "` (target `", target_key,
          "`) declares arg #", i, " of type `", TypeKey(p.arg_types[i]),
          "` but format `", p.format, "` does not reference `%", i, "`"));
    }
  }
  // (b) No phantom placeholders past the declared arg count.
  for (std::size_t i = p.arg_types.size(); i < 10; ++i) {
    if (absl::StrContains(p.format, absl::StrCat("%", i))) {
      return absl::InvalidArgumentError(
          absl::StrCat("production `", p.name, "` (target `", target_key,
                       "`) format `", p.format, "` references `%", i,
                       "` but declares only ", p.arg_types.size(), " args"));
    }
  }
  // (c) Every arg type must itself be a registered target so
  //     the recursion can find some production yielding it.
  for (std::size_t i = 0; i < p.arg_types.size(); ++i) {
    if (!g.HasType(p.arg_types[i])) {
      return absl::InvalidArgumentError(absl::StrCat(
          "production `", p.name, "` (target `", target_key, "`) needs arg #",
          i, " of type `", TypeKey(p.arg_types[i]),
          "` but no productions are registered for that type"));
    }
  }
  // (d) extra_scope_for_arg, if non-empty, matches arg count.
  if (!p.extra_scope_for_arg.empty() &&
      p.extra_scope_for_arg.size() != p.arg_types.size()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "production `", p.name,
        "` has extra_scope_for_arg.size() = ", p.extra_scope_for_arg.size(),
        " but arg_types.size() = ", p.arg_types.size(),
        "; must be 0 or equal to arg_types.size()"));
  }
  // (e) Weight non-negative.
  if (p.weight < 0) {
    return absl::InvalidArgumentError(absl::StrCat(
        "production `", p.name, "` has negative weight ", p.weight));
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status Grammar::Validate() const {
  if (rules_.empty()) {
    return absl::InvalidArgumentError(
        "Grammar::Validate: no productions registered");
  }
  for (const CelType& target : types_) {
    const std::string target_key = TypeKey(target);
    const auto it = rules_.find(target_key);
    if (it == rules_.end() || it->second.empty()) {
      return absl::InvalidArgumentError(
          absl::StrCat("type `", target_key,
                       "` is in the types() list but has no productions"));
    }
    bool has_leaf = false;
    for (const Production& p : it->second) {
      if (absl::Status s = ValidateProduction(p, *this, target_key); !s.ok()) {
        return s;
      }
      has_leaf = has_leaf || p.is_leaf;
    }
    // (f) Every type with productions has at least one leaf.
    if (!has_leaf) {
      return absl::InvalidArgumentError(
          absl::StrCat("type `", target_key,
                       "` has no leaf production; depth-0 recursion would "
                       "have no eligible rule"));
    }
  }
  return absl::OkStatus();
}

// ── GrammarBuilder ───────────────────────────────────────────────

void GrammarBuilder::Register(const CelType& target, Production p) {
  // Normalise `extra_scope_for_arg`: an empty vector is shorthand
  // for "no scope extensions on any arg".  Expand to the right
  // shape so the generator and L1 can iterate uniformly.
  if (p.extra_scope_for_arg.empty()) {
    p.extra_scope_for_arg.resize(p.arg_types.size());
  }
  const std::string key = TypeKey(target);
  auto [it, inserted] = grammar_.rules_.try_emplace(key);
  if (inserted) {
    grammar_.types_.push_back(target);
  }
  it->second.push_back(std::move(p));
}

GrammarBuilder& GrammarBuilder::Leaf(const CelType& target, std::string name,
                                     std::string format, int weight) {
  Production p;
  p.name = std::move(name);
  p.format = std::move(format);
  p.is_leaf = true;
  p.weight = weight;
  Register(target, std::move(p));
  return *this;
}

GrammarBuilder& GrammarBuilder::Unary(const CelType& target, std::string name,
                                      std::string format, CelType arg0_type,
                                      int weight) {
  Production p;
  p.name = std::move(name);
  p.format = std::move(format);
  p.arg_types.push_back(std::move(arg0_type));
  p.weight = weight;
  Register(target, std::move(p));
  return *this;
}

GrammarBuilder& GrammarBuilder::Binary(const CelType& target, std::string name,
                                       std::string format, CelType arg0_type,
                                       CelType arg1_type, int weight) {
  Production p;
  p.name = std::move(name);
  p.format = std::move(format);
  p.arg_types.push_back(std::move(arg0_type));
  p.arg_types.push_back(std::move(arg1_type));
  p.weight = weight;
  Register(target, std::move(p));
  return *this;
}

GrammarBuilder& GrammarBuilder::Ternary(const CelType& target, std::string name,
                                        std::string format, CelType arg0_type,
                                        CelType arg1_type, CelType arg2_type) {
  Production p;
  p.name = std::move(name);
  p.format = std::move(format);
  p.arg_types.push_back(std::move(arg0_type));
  p.arg_types.push_back(std::move(arg1_type));
  p.arg_types.push_back(std::move(arg2_type));
  Register(target, std::move(p));
  return *this;
}

GrammarBuilder& GrammarBuilder::Repeated(const CelType& target,
                                         std::string name, std::string format,
                                         const CelType& arg_type, int arity,
                                         int weight) {
  Production p;
  p.name = std::move(name);
  p.format = std::move(format);
  p.arg_types.reserve(static_cast<std::size_t>(arity));
  for (int i = 0; i < arity; ++i) {
    p.arg_types.push_back(arg_type);
  }
  p.weight = weight;
  Register(target, std::move(p));
  return *this;
}

GrammarBuilder& GrammarBuilder::Comprehension(
    const CelType& target, std::string name, std::string format,
    CelType range_type, std::pair<std::string, CelType> iter,
    CelType body_type) {
  Production p;
  p.name = std::move(name);
  p.format = std::move(format);
  p.arg_types.push_back(std::move(range_type));
  p.arg_types.push_back(std::move(body_type));
  p.extra_scope_for_arg.resize(2);
  p.extra_scope_for_arg[1].push_back(std::move(iter));
  Register(target, std::move(p));
  return *this;
}

Grammar GrammarBuilder::Build() && {
  return std::move(grammar_);
}

// ── Generation ───────────────────────────────────────────────────

namespace {

// Pick a production from `rules` honouring `require_leaf` and
// per-production `weight`.  Returns nullptr only when the rule
// set has zero eligible production at the current filter (which
// L1's leaf-coverage check should have prevented for the
// require_leaf branch; callers ABSL_CHECK).
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
      << "GenerateExpr: no eligible production for target `" << TypeKey(target)
      << "` (depth=" << ctx.depth_budget << "); L1 should have caught this";

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

}  // namespace celwasm::fuzz
