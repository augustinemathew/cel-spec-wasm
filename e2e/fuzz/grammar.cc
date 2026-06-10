#include "e2e/fuzz/grammar.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
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
          return absl::InvalidArgumentError(absl::StrCat(
              "production `", p.name, "` (target `", target_key, "`) format `",
              p.format, "` references `%", i, "` but declares only ",
              p.arg_types.size(), " args"));
        }
      }
      // (c) Every arg type must itself be a registered target so
      //     the recursion can find some production yielding it.
      for (std::size_t i = 0; i < p.arg_types.size(); ++i) {
        if (!HasType(p.arg_types[i])) {
          return absl::InvalidArgumentError(absl::StrCat(
              "production `", p.name, "` (target `", target_key,
              "`) needs arg #", i, " of type `", TypeKey(p.arg_types[i]),
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
      if (p.is_leaf) {
        has_leaf = true;
      }
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

GrammarBuilder& GrammarBuilder::Leaf(CelType target, std::string name,
                                     std::string format, int weight) {
  Production p;
  p.name = std::move(name);
  p.format = std::move(format);
  p.is_leaf = true;
  p.weight = weight;
  Register(target, std::move(p));
  return *this;
}

GrammarBuilder& GrammarBuilder::Unary(CelType target, std::string name,
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

GrammarBuilder& GrammarBuilder::Binary(CelType target, std::string name,
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

GrammarBuilder& GrammarBuilder::Ternary(CelType target, std::string name,
                                        std::string format, CelType arg0_type,
                                        CelType arg1_type, CelType arg2_type,
                                        int weight) {
  Production p;
  p.name = std::move(name);
  p.format = std::move(format);
  p.arg_types.push_back(std::move(arg0_type));
  p.arg_types.push_back(std::move(arg1_type));
  p.arg_types.push_back(std::move(arg2_type));
  p.weight = weight;
  Register(target, std::move(p));
  return *this;
}

GrammarBuilder& GrammarBuilder::Repeated(CelType target, std::string name,
                                         std::string format, CelType arg_type,
                                         int arity, int weight) {
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
    CelType target, std::string name, std::string format, CelType range_type,
    std::pair<std::string, CelType> iter, CelType body_type, int weight) {
  Production p;
  p.name = std::move(name);
  p.format = std::move(format);
  p.arg_types.push_back(std::move(range_type));
  p.arg_types.push_back(std::move(body_type));
  p.extra_scope_for_arg.resize(2);
  p.extra_scope_for_arg[1].push_back(std::move(iter));
  p.weight = weight;
  Register(target, std::move(p));
  return *this;
}

Grammar GrammarBuilder::Build() && {
  return std::move(grammar_);
}

}  // namespace celwasm::fuzz
