#ifndef CELWASM_E2E_FUZZ_GRAMMAR_H_
#define CELWASM_E2E_FUZZ_GRAMMAR_H_

// Typed attribute grammar for CEL source generation.  Each
// `Production` describes one way to build an expression of a
// declared target `CelType`: a `format` string template with
// `%0` / `%1` / `%2` placeholders, one declared `arg_type` per
// placeholder, an optional scope extension per argument (for
// comprehension iter-vars), a leaf flag (depth-0 eligible), and
// a sampling weight.
//
// The grammar is the spec for the generator that runs in
// `cel_oracle_property_test.cc`.  Every emitted source string is
// constructed by recursively filling each `%i` with a
// sub-expression of `arg_types[i]`, so every result type-checks
// by construction — provided each Production is internally
// consistent.  That consistency is the job of `Grammar::Validate`
// (L1) and `grammar_test.cc` (L2 + L3).  See
// `doc/implementation-plan/rewrite/m27-pbt-cel-generator.md`
// §"Grammar validation".

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "shared/type.h"

namespace celwasm::fuzz {

// Names + types added to `GenCtx::in_scope` when the matching
// `%i` slot of a Production is generated.  Empty for non-binding
// arms; non-empty for the predicate slot of a comprehension
// macro (the iter_var lives in scope only while generating that
// slot's sub-expression).
using ScopeExtension = std::vector<std::pair<std::string, CelType>>;

struct Production {
  // Human-readable handle, e.g. "int_add", "list_int_size",
  // "comp_exists_int".  Used in test failure messages and in
  // shrinker output; not parsed.
  std::string name;

  // Format template with `%0` / `%1` / … placeholders.  Leaves
  // typically have no placeholders (the format is the literal
  // source itself, e.g. "42" or "a") but may have them for
  // domain-backed leaves in later slices.
  std::string format;

  // One entry per placeholder, in `%0` / `%1` / … order.  The
  // recursion fills each with a sub-expression of the declared
  // type.
  std::vector<CelType> arg_types;

  // Per-arg scope extensions.  Size must equal `arg_types.size()`
  // (or zero, which is treated as "no extensions for any arg").
  // Used by `comp_*` productions to bind the iter_var while
  // generating the predicate slot.
  std::vector<ScopeExtension> extra_scope_for_arg;

  // Eligible at depth 0?  Every target type must have at least
  // one leaf production, else depth-0 recursion gets stuck.
  bool is_leaf = false;

  // Sampling weight.  Must be >= 0; entries with weight 0 are
  // never picked (useful for productions we want to register-and-
  // disable rather than delete).
  int weight = 1;
};

// Container for all `Production`s, keyed by target `CelType`.
// Use `GrammarBuilder` to populate.
class Grammar {
 public:
  // Productions yielding `target`.  Empty if `target` is
  // unregistered.
  const std::vector<Production>& Rules(const CelType& target) const;

  // True iff at least one production with target `t` was
  // registered.
  bool HasType(const CelType& t) const;

  // Every registered target type, in insertion order.  Stable
  // across calls; used by `grammar_test.cc` to iterate the
  // grammar for L2 checks.
  const std::vector<CelType>& Types() const {
    return types_;
  }

  // Total production count, summed across all target types.
  // Used by tests for sanity assertions.
  std::size_t TotalProductions() const;

  // L1 — static structural checks (see m27 §"Grammar
  // validation").  Returns `InvalidArgumentError` with a
  // human-readable message naming the offending production and
  // field.  Cheap; runs in O(rules) without touching the parser
  // or checker.
  absl::Status Validate() const;

 private:
  friend class GrammarBuilder;

  // Internally keyed by `CelType.Kind()` plus a sub-type
  // discriminator string (for List/Map/Message) because
  // `CelType` doesn't expose a hash or stable string key.  See
  // `TypeKey` in grammar.cc.
  absl::flat_hash_map<std::string, std::vector<Production>> rules_;
  // Stable iteration order; mirrors first-insertion-of-target.
  std::vector<CelType> types_;
};

// Fluent builder for `Grammar`.  The signature shorthands
// (`Leaf` / `Unary` / `Binary` / `Ternary` / `Comprehension`)
// guarantee the placeholder count matches `arg_types.size()` at
// the type-system level so L1's "placeholder consistency" check
// is reduced to "format references exactly `%0..%N-1`".
class GrammarBuilder {
 public:
  // Leaf with zero placeholders — `format` is the literal source.
  // Example: `Leaf(Int(), "int_const_zero", "0")`.
  GrammarBuilder& Leaf(CelType target, std::string name,
                       std::string format, int weight = 1);

  // 1-arg recursive rule.  `format` must contain `%0`.
  GrammarBuilder& Unary(CelType target, std::string name,
                        std::string format, CelType arg0_type,
                        int weight = 1);

  // 2-arg recursive rule.  `format` must contain `%0` and `%1`.
  GrammarBuilder& Binary(CelType target, std::string name,
                         std::string format, CelType arg0_type,
                         CelType arg1_type, int weight = 1);

  // 3-arg recursive rule (ternary).  `format` must contain
  // `%0`, `%1`, `%2`.
  GrammarBuilder& Ternary(CelType target, std::string name,
                          std::string format, CelType arg0_type,
                          CelType arg1_type, CelType arg2_type,
                          int weight = 1);

  // Comprehension shape — `range_type` for `%0`, `body_type` for
  // `%1` with `(iter.first : iter.second)` added to scope only
  // while `%1` is generated.  Format example:
  // `"(%0).exists(v, %1)"` for `exists`.
  GrammarBuilder& Comprehension(CelType target, std::string name,
                                std::string format,
                                CelType range_type,
                                std::pair<std::string, CelType> iter,
                                CelType body_type, int weight = 1);

  // Finalises and returns the grammar.  Consumes `*this`.
  Grammar Build() &&;

 private:
  // Appends `p` to the rules vector for `target`, registering
  // `target` as a known type on first sight.  Normalises
  // `p.extra_scope_for_arg` to size match `p.arg_types` if the
  // caller left it empty (the shorthand for "no scope
  // extensions").
  void Register(const CelType& target, Production p);

  Grammar grammar_;
};

// Stable string key for a `CelType` (canonical CEL-spec form,
// e.g. "int", "list<int>", "map<string,celwasm.testdata.Customer>",
// "optional<int>", "google.example.Foo").  Used as the
// `Grammar::rules_` map key.  Exposed for tests + the L2
// implementation that has to surface "type X has no production"
// errors in human-readable form.
std::string TypeKey(const CelType& t);

// The cel-cpp / CheckOptions variable-spec form of a `CelType`,
// e.g. "int", "list<int>", "celwasm.testdata.Customer".  Roughly
// identical to `TypeKey` but does NOT wrap message names in any
// quoting — fed straight into `CheckOptions::variable_specs`.
// Used by L2 to synthesise check-able sources.
std::string TypeSpec(const CelType& t);

}  // namespace celwasm::fuzz

#endif  // CELWASM_E2E_FUZZ_GRAMMAR_H_
