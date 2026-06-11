// L1 unit tests for `Grammar::Validate()`.  These exercise the
// static structural checks in isolation, without involving the
// real cel-cpp parser+checker — L2 will add that in
// `grammar_test.cc` once `grammar_scalars.cc` lands.  L3
// (composition spot-checks against the cel-cpp oracle) lands
// alongside L2.

#include "e2e/fuzz/grammar.h"

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"
#include "e2e/fuzz/grammar_aggregates.h"
#include "e2e/fuzz/grammar_scalars.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm::fuzz {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// Trivial well-formed grammar: every type has a leaf.
GrammarBuilder MinimalBoolGrammar() {
  GrammarBuilder b;
  b.Leaf(CelType::Bool(), "bool_true", "true");
  return b;
}

// ── TypeKey / TypeSpec ──────────────────────────────────────────

TEST(TypeKeyTest, ScalarsRenderAsCelSpecNames) {
  EXPECT_EQ(TypeKey(CelType::Bool()), "bool");
  EXPECT_EQ(TypeKey(CelType::Int()), "int");
  EXPECT_EQ(TypeKey(CelType::Uint()), "uint");
  EXPECT_EQ(TypeKey(CelType::Double()), "double");
  EXPECT_EQ(TypeKey(CelType::String()), "string");
  EXPECT_EQ(TypeKey(CelType::Bytes()), "bytes");
  EXPECT_EQ(TypeKey(CelType::Duration()), "duration");
  EXPECT_EQ(TypeKey(CelType::Timestamp()), "timestamp");
}

TEST(TypeKeyTest, ListAndMapAreParameterised) {
  EXPECT_EQ(TypeKey(CelType::List(CelType::Int())), "list<int>");
  EXPECT_EQ(TypeKey(CelType::Map(CelType::String(), CelType::Int())),
            "map<string,int>");
  // Nested.
  EXPECT_EQ(
      TypeKey(CelType::List(CelType::Map(CelType::String(), CelType::Bool()))),
      "list<map<string,bool>>");
}

TEST(TypeKeyTest, MessageRendersAsFullyQualifiedName) {
  EXPECT_EQ(TypeKey(CelType::Message("celwasm.testdata.Customer")),
            "celwasm.testdata.Customer");
}

TEST(TypeSpecTest, MatchesTypeKeyForSupportedTypes) {
  // Today TypeSpec is identical to TypeKey; the test guards
  // against accidental divergence when one of them is changed.
  EXPECT_EQ(TypeSpec(CelType::Int()), TypeKey(CelType::Int()));
  EXPECT_EQ(TypeSpec(CelType::List(CelType::Bool())),
            TypeKey(CelType::List(CelType::Bool())));
}

// ── Builder semantics ────────────────────────────────────────────

TEST(GrammarBuilderTest, LeafRegistersTargetType) {
  Grammar g = MinimalBoolGrammar().Build();
  EXPECT_TRUE(g.HasType(CelType::Bool()));
  EXPECT_FALSE(g.HasType(CelType::Int()));
  EXPECT_EQ(g.TotalProductions(), 1u);
  EXPECT_EQ(g.Types().size(), 1u);
  EXPECT_EQ(g.Rules(CelType::Bool()).size(), 1u);
  EXPECT_EQ(g.Rules(CelType::Bool())[0].name, "bool_true");
  EXPECT_TRUE(g.Rules(CelType::Bool())[0].is_leaf);
}

TEST(GrammarBuilderTest, UnaryFillsArgTypeAndPreservesScopeShape) {
  GrammarBuilder b;
  b.Leaf(CelType::Int(), "int_zero", "0");
  b.Unary(CelType::Int(), "int_neg", "(-%0)", CelType::Int());
  Grammar g = std::move(b).Build();

  const auto& rules = g.Rules(CelType::Int());
  ASSERT_EQ(rules.size(), 2u);
  // Find the neg rule (order isn't part of the contract).
  const Production* neg = nullptr;
  for (const auto& p : rules) {
    if (p.name == "int_neg") neg = &p;
  }
  ASSERT_NE(neg, nullptr);
  EXPECT_EQ(neg->arg_types.size(), 1u);
  EXPECT_EQ(TypeKey(neg->arg_types[0]), "int");
  // `extra_scope_for_arg` is normalised to match `arg_types.size()`
  // (1) even though we didn't pass any extensions.
  EXPECT_EQ(neg->extra_scope_for_arg.size(), 1u);
  EXPECT_TRUE(neg->extra_scope_for_arg[0].empty());
  EXPECT_FALSE(neg->is_leaf);
}

TEST(GrammarBuilderTest, ComprehensionAddsIterToBodyScopeOnly) {
  GrammarBuilder b;
  b.Leaf(CelType::Bool(), "bool_true", "true");
  b.Leaf(CelType::List(CelType::Int()), "list_one", "[1]");
  b.Leaf(CelType::Int(), "int_zero", "0");
  b.Comprehension(CelType::Bool(), "comp_exists_int", "(%0).exists(v, %1)",
                  /*range_type=*/CelType::List(CelType::Int()),
                  /*iter=*/{"v", CelType::Int()},
                  /*body_type=*/CelType::Bool());
  Grammar g = std::move(b).Build();

  const Production* comp = nullptr;
  for (const auto& p : g.Rules(CelType::Bool())) {
    if (p.name == "comp_exists_int") comp = &p;
  }
  ASSERT_NE(comp, nullptr);
  ASSERT_EQ(comp->arg_types.size(), 2u);
  EXPECT_EQ(TypeKey(comp->arg_types[0]), "list<int>");
  EXPECT_EQ(TypeKey(comp->arg_types[1]), "bool");
  // Slot 0 (the iter_range): no scope extensions.
  EXPECT_TRUE(comp->extra_scope_for_arg[0].empty());
  // Slot 1 (the body): `v: int` added.
  ASSERT_EQ(comp->extra_scope_for_arg[1].size(), 1u);
  EXPECT_EQ(comp->extra_scope_for_arg[1][0].first, "v");
  EXPECT_EQ(TypeKey(comp->extra_scope_for_arg[1][0].second), "int");
}

// ── L1 — Grammar::Validate() ─────────────────────────────────────

TEST(GrammarValidateTest, EmptyGrammarRejected) {
  Grammar g = GrammarBuilder().Build();
  EXPECT_THAT(g.Validate(), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(GrammarValidateTest, MinimalLeafGrammarAccepted) {
  Grammar g = MinimalBoolGrammar().Build();
  EXPECT_THAT(g.Validate(), IsOk());
}

TEST(GrammarValidateTest, MissingLeafForRegisteredTypeRejected) {
  GrammarBuilder b;
  b.Leaf(CelType::Bool(), "bool_true", "true");
  // Int has only a recursive rule (depth-0 has nothing to pick).
  b.Leaf(CelType::Int(), "int_zero", "0");
  b.Unary(CelType::Int(), "int_neg", "(-%0)", CelType::Int());
  Grammar fine = std::move(b).Build();
  EXPECT_THAT(fine.Validate(), IsOk());

  GrammarBuilder bad_builder;
  bad_builder.Leaf(CelType::Bool(), "bool_true", "true");
  bad_builder.Unary(CelType::Int(), "int_neg_no_leaf", "(-%0)", CelType::Int());
  Grammar bad = std::move(bad_builder).Build();
  EXPECT_THAT(bad.Validate(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       ::testing::HasSubstr("has no leaf production")));
}

TEST(GrammarValidateTest, FormatMissingDeclaredPlaceholderRejected) {
  GrammarBuilder b;
  b.Leaf(CelType::Int(), "int_zero", "0");
  // %1 missing.
  b.Binary(CelType::Int(), "int_add_typo", "(%0 +)", CelType::Int(),
           CelType::Int());
  Grammar g = std::move(b).Build();
  EXPECT_THAT(g.Validate(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       ::testing::HasSubstr("does not reference `%1`")));
}

TEST(GrammarValidateTest, PhantomPlaceholderInFormatRejected) {
  GrammarBuilder b;
  b.Leaf(CelType::Int(), "int_zero", "0");
  // Declared arity 1 but format references %0 AND %2.
  b.Unary(CelType::Int(), "int_phantom", "(%0 + %2)", CelType::Int());
  Grammar g = std::move(b).Build();
  EXPECT_THAT(g.Validate(), StatusIs(absl::StatusCode::kInvalidArgument,
                                     ::testing::HasSubstr("references `%2`")));
}

TEST(GrammarValidateTest, UnregisteredArgTypeRejected) {
  GrammarBuilder b;
  b.Leaf(CelType::Bool(), "bool_true", "true");
  // Int isn't registered anywhere, but the bool_to_bool rule
  // requires it as arg.
  b.Unary(CelType::Bool(), "bool_from_int_lt", "(%0 < 1)", CelType::Int());
  Grammar g = std::move(b).Build();
  EXPECT_THAT(g.Validate(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       ::testing::HasSubstr("no productions are registered")));
}

TEST(GrammarValidateTest, NegativeWeightRejected) {
  GrammarBuilder b;
  b.Leaf(CelType::Int(), "int_zero", "0", /*weight=*/-1);
  Grammar g = std::move(b).Build();
  EXPECT_THAT(g.Validate(), StatusIs(absl::StatusCode::kInvalidArgument,
                                     ::testing::HasSubstr("negative weight")));
}

// ── Scalar catalog: L1 self-consistency ─────────────────────────

TEST(ScalarGrammarTest, BuildsAndPassesL1Validation) {
  // `BuildScalarGrammar()` ABSL_CHECKs internally if L1 fails — so
  // reaching this point is itself the assertion.  We also check
  // a couple of structural properties to detect catalog-skew.
  Grammar g = BuildScalarGrammar();
  EXPECT_THAT(g.Validate(), IsOk());
  // Every supported scalar target must have at least one
  // production registered.
  for (const CelType& t :
       {CelType::Bool(), CelType::Int(), CelType::Uint(), CelType::Double(),
        CelType::String(), CelType::Bytes()}) {
    EXPECT_TRUE(g.HasType(t))
        << "scalar catalog is missing target " << TypeKey(t);
    EXPECT_FALSE(g.Rules(t).empty()) << TypeKey(t);
  }
  // Catalog size sanity check — catches accidental wholesale
  // deletions.  Bump as the catalog grows; sub-50 means we
  // dropped a category by accident.
  EXPECT_GE(g.TotalProductions(), 50u);
}

TEST(ScalarGrammarTest, EveryActivationBindingHasIdentLeaf) {
  Grammar g = BuildScalarGrammar();
  for (const ActivationBinding& v : ActivationSchema()) {
    const auto& rules = g.Rules(v.type);
    bool found = false;
    for (const Production& p : rules) {
      // The catalog names ident leaves "<name>_ident".  Stable
      // suffix; we rely on it for the L2 source synthesis below.
      if (p.is_leaf && p.format == v.name) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "no ident-leaf production for activation binding `"
                       << v.name << ": " << TypeKey(v.type) << "`";
  }
}

// ── L2 — every production type-checks against real cel-cpp ──────
//
// For each Production p:
//   1. Synthesise a source string by substituting each `%i` with a
//      uniquely-named ident of the declared `arg_types[i]`.
//   2. Build a `CheckOptions::variable_specs` declaring both the
//      activation idents (so `*_ident` leaves resolve) AND the
//      synthesised `__pbt_v<i>` slots.
//   3. Run `ParseAndCheck`.  Must accept.
//   4. Read the root expression's annotated `Repr` and verify it
//      matches the production's declared target type.
//
// Failures name the production that's wrong, so a catalog bug is
// pinpointed instantly.

namespace l2 {

// Map a target `CelType` to the `Repr` the cel-cpp checker
// would stamp on a root expression of that type.  Used for the
// post-check root-type comparison.  Covers every kind a grammar
// target can be today (scalars + list/map — the annotator stamps
// an aggregate root with the aggregate's own kind).
Repr ExpectedRoot(const CelType& t) {
  switch (t.kind()) {
    case CelType::Kind::kBool:
      return Repr::kBool;
    case CelType::Kind::kInt:
      return Repr::kInt;
    case CelType::Kind::kUint:
      return Repr::kUint;
    case CelType::Kind::kDouble:
      return Repr::kDouble;
    case CelType::Kind::kString:
      return Repr::kString;
    case CelType::Kind::kBytes:
      return Repr::kBytes;
    case CelType::Kind::kList:
      return Repr::kList;
    case CelType::Kind::kMap:
      return Repr::kMap;
    case CelType::Kind::kTimestamp:
      return Repr::kTimestamp;
    case CelType::Kind::kDuration:
      return Repr::kDuration;
    default:
      // No grammar registers these kinds yet; if a future catalog
      // adds one, expand this switch alongside it.
      return Repr::kUnknown;
  }
}

// Per-production source synthesis.  Replaces `%i` with
// `__pbt_v<i>` and returns the list of (var_name, var_type) pairs
// the caller must declare to the checker.
struct Synthesised {
  std::string source;
  std::vector<std::pair<std::string, CelType>> slot_vars;
};
Synthesised Synthesise(const Production& p) {
  Synthesised out;
  out.source = p.format;
  for (std::size_t i = 0; i < p.arg_types.size(); ++i) {
    const std::string slot_name = absl::StrCat("__pbt_v", i);
    out.source =
        absl::StrReplaceAll(out.source, {{absl::StrCat("%", i), slot_name}});
    out.slot_vars.emplace_back(slot_name, p.arg_types[i]);
  }
  return out;
}

// Build the `variable_specs` list that declares every activation
// binding PLUS the per-production synthesised slot vars.  Slot
// vars get the production's arg type; activation bindings get
// their declared type.
std::vector<std::string> BuildVariableSpecs(
    const std::vector<std::pair<std::string, CelType>>& slot_vars) {
  std::vector<std::string> specs;
  for (const ActivationBinding& v : ActivationSchema()) {
    specs.push_back(absl::StrCat(v.name, ":", TypeSpec(v.type)));
  }
  for (const auto& [name, type] : slot_vars) {
    specs.push_back(absl::StrCat(name, ":", TypeSpec(type)));
  }
  return specs;
}

}  // namespace l2

TEST(ScalarGrammarL2Test, EveryProductionParsesAndTypesAsDeclared) {
  Grammar g = BuildScalarGrammar();
  std::size_t productions_checked = 0;
  for (const CelType& target : g.Types()) {
    const Repr expected = l2::ExpectedRoot(target);
    ASSERT_NE(expected, Repr::kUnknown)
        << "L2 has no Repr expectation for target " << TypeKey(target)
        << "; extend l2::ExpectedRoot";

    for (const Production& p : g.Rules(target)) {
      const auto synth = l2::Synthesise(p);
      CheckOptions opts;
      opts.variable_specs = l2::BuildVariableSpecs(synth.slot_vars);

      auto ta = ParseAndCheck(synth.source, opts);
      ASSERT_THAT(ta, IsOk())
          << "production `" << p.name << "` (target " << TypeKey(target)
          << "): source `" << synth.source << "` failed ParseAndCheck";

      // Root expr should have its Repr annotation set to the
      // declared target.
      const int64_t root_id = ta->ast().root_expr().id();
      const NodeAnnotation* root_ann = ta->annotations().Find(root_id);
      ASSERT_NE(root_ann, nullptr)
          << "production `" << p.name << "` (source `" << synth.source
          << "`): no annotation on root";
      EXPECT_EQ(root_ann->repr, expected)
          << "production `" << p.name << "` (source `" << synth.source
          << "`): declared target " << TypeKey(target)
          << " (expects Repr=" << static_cast<int>(expected)
          << "), but cel-cpp stamped Repr=" << static_cast<int>(root_ann->repr);

      ++productions_checked;
    }
  }
  // Sanity: we actually iterated the whole catalog.
  EXPECT_GE(productions_checked, 50u);
}

// ── L3 — sampled composition check at varying depths ────────────
//
// The walker below is a deliberately-minimal implementation of
// the type-directed recursion described in m27 §"The data model".
// The real generator (the seeded grammar walker)
// will live in `generator.{h,cc}` with a richer API
// (fuzztest Domain, weighting hooks, shrinker support, etc.); we
// inline a tiny version here just so L3 can assert composition
// correctness BEFORE step 4 lands.  If this walker disagrees with
// the eventual generator, the production-level invariants L3
// pins still hold because both walk the same grammar.

namespace l3 {

// Per-recursion context.  Threaded through `Walk`; mirrors the
// `GenCtx` the future generator will use.
struct WalkCtx {
  int depth_budget;
  // in_scope: name → type.  the scalar leaves draw idents only
  // from the fixed activation, so the only entries that ever land
  // here are the comprehension-bound iter_vars (which the scalar catalog
  // doesn't have).  Kept for shape-parity with `GenCtx`.
  std::vector<std::pair<std::string, CelType>> in_scope;
  std::mt19937_64* rng;
};

// Pick a production from `rules` honouring `is_leaf` filtering at
// depth 0 and `weight` for sampling.  Returns nullptr only if the
// rule set has no eligible production at the current depth — the
// caller must ASSERT that doesn't happen (it would be a grammar
// bug L1 should have caught).
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

std::string Walk(const Grammar& g, const CelType& target, WalkCtx& ctx);

// Substitute `%i` in `format` with the result of recursively
// walking each arg type, threading `extra_scope_for_arg[i]` into
// scope only for that recursion.
std::string ExpandTemplate(const Grammar& g, const Production& p,
                           WalkCtx& ctx) {
  std::string out = p.format;
  for (std::size_t i = 0; i < p.arg_types.size(); ++i) {
    WalkCtx sub = ctx;
    sub.depth_budget--;
    for (const auto& [n, t] : p.extra_scope_for_arg[i]) {
      sub.in_scope.emplace_back(n, t);
    }
    const std::string arg = Walk(g, p.arg_types[i], sub);
    out = absl::StrReplaceAll(out, {{absl::StrCat("%", i), arg}});
  }
  return out;
}

std::string Walk(const Grammar& g, const CelType& target, WalkCtx& ctx) {
  const auto& rules = g.Rules(target);
  const Production* p =
      PickProduction(rules, /*require_leaf=*/ctx.depth_budget == 0, *ctx.rng);
  // Falls back to any leaf if the depth-0 require_leaf filter found
  // nothing usable — shouldn't happen post-L1, but defensive.
  if (p == nullptr) {
    p = PickProduction(rules, /*require_leaf=*/true, *ctx.rng);
  }
  if (p == nullptr) return "/*l3-walker: no production*/";
  if (p->arg_types.empty()) {
    return p->format;
  }
  return ExpandTemplate(g, *p, ctx);
}

}  // namespace l3

TEST(ScalarGrammarL3Test, SampledCompositionsParseAndTypeAsBool) {
  Grammar g = BuildScalarGrammar();

  // The activation idents are the only idents the scalar catalog can emit;
  // declare them once.  No synthesised slot vars needed because
  // L3 generates self-contained source (no `%i` left over).
  CheckOptions opts;
  for (const ActivationBinding& v : ActivationSchema()) {
    opts.variable_specs.push_back(absl::StrCat(v.name, ":", TypeSpec(v.type)));
  }

  constexpr int kSeedsPerDepth = 200;  // 200 × 3 depths × ~5ms ≈ 3s
  constexpr int kDepths[] = {1, 3, 6};

  int total_sampled = 0;
  for (int depth : kDepths) {
    for (int seed = 0; seed < kSeedsPerDepth; ++seed) {
      std::mt19937_64 rng((static_cast<uint64_t>(seed) * 1000u) +
                          static_cast<uint64_t>(depth));
      l3::WalkCtx ctx{depth, {}, &rng};
      const std::string source = l3::Walk(g, CelType::Bool(), ctx);

      auto ta = ParseAndCheck(source, opts);
      ASSERT_THAT(ta, IsOk()) << "L3 depth=" << depth << " seed=" << seed
                              << ": grammar-composed source `" << source
                              << "` did not ParseAndCheck";

      // Root must be Bool, per the L3 target type.
      const int64_t root_id = ta->ast().root_expr().id();
      const NodeAnnotation* root_ann = ta->annotations().Find(root_id);
      ASSERT_NE(root_ann, nullptr)
          << "L3 depth=" << depth << " seed=" << seed
          << ": no annotation on root of `" << source << "`";
      EXPECT_EQ(root_ann->repr, Repr::kBool)
          << "L3 depth=" << depth << " seed=" << seed
          << ": expected Bool, got Repr=" << static_cast<int>(root_ann->repr)
          << " on source `" << source << "`";

      ++total_sampled;
    }
  }
  EXPECT_EQ(total_sampled, kSeedsPerDepth * 3);
}

// ── Aggregate catalog: L1 / catalog-shape / L2 / L3 ────────────────
//
// The aggregate catalog extends the grammar with aggregates (list / map
// literals) and comprehension macros.  Reuses the L2 / L3
// helpers above by passing the slice-C grammar instead of B.
// Per m27 §"Grammar validation", the three validation layers
// must be green BEFORE any oracle iteration runs against the
// expanded grammar; that's what these tests guard.

TEST(AggregateGrammarTest, BuildsAndPassesL1Validation) {
  Grammar g = BuildFullGrammar();
  EXPECT_THAT(g.Validate(), IsOk());
  // C1 must register list<T> for every scalar T and a sampled
  // K×V map vocab.
  for (const CelType& elt :
       {CelType::Bool(), CelType::Int(), CelType::Uint(), CelType::Double(),
        CelType::String(), CelType::Bytes()}) {
    EXPECT_TRUE(g.HasType(CelType::List(elt)))
        << "aggregate catalog is missing list<" << TypeKey(elt) << ">";
    EXPECT_FALSE(g.Rules(CelType::List(elt)).empty());
  }
  // Aggregates grow the catalog substantially — roughly the scalar
  // B count plus aggregates + comprehensions.  Lower-bound the
  // total to catch accidental wholesale deletions.
  EXPECT_GT(g.TotalProductions(), 130u);
}

TEST(AggregateGrammarTest, EveryListAndMapHasALeaf) {
  // L1 already checks this generically; this test re-states the
  // invariant for the new aggregate targets so a future catalog
  // edit that drops the literal-only leaf gets a focused
  // failure message.
  Grammar g = BuildFullGrammar();
  for (const CelType& elt :
       {CelType::Bool(), CelType::Int(), CelType::Uint(), CelType::Double(),
        CelType::String(), CelType::Bytes()}) {
    bool has_leaf = false;
    for (const Production& p : g.Rules(CelType::List(elt))) {
      if (p.is_leaf) {
        has_leaf = true;
        break;
      }
    }
    EXPECT_TRUE(has_leaf) << "list<" << TypeKey(elt)
                          << "> has no leaf production";
  }
}

TEST(AggregateGrammarL2Test, EveryProductionParsesAndTypesAsDeclared) {
  Grammar g = BuildFullGrammar();
  std::size_t productions_checked = 0;
  for (const CelType& target : g.Types()) {
    // `ExpectedRoot` covers scalar AND aggregate targets (the
    // annotator stamps an aggregate root with Repr::kList /
    // kMap), so every registered target gets the Repr equality
    // assertion.
    const Repr expected_scalar = l2::ExpectedRoot(target);
    for (const Production& p : g.Rules(target)) {
      const auto synth = l2::Synthesise(p);
      CheckOptions opts;
      opts.variable_specs = l2::BuildVariableSpecs(synth.slot_vars);

      auto ta = ParseAndCheck(synth.source, opts);
      ASSERT_THAT(ta, IsOk())
          << "aggregate production `" << p.name << "` (target "
          << TypeKey(target) << "): source `" << synth.source
          << "` failed ParseAndCheck";

      if (expected_scalar != Repr::kUnknown) {
        const int64_t root_id = ta->ast().root_expr().id();
        const NodeAnnotation* root_ann = ta->annotations().Find(root_id);
        ASSERT_NE(root_ann, nullptr);
        EXPECT_EQ(root_ann->repr, expected_scalar)
            << "aggregate production `" << p.name << "` (source `"
            << synth.source << "`): declared target " << TypeKey(target)
            << " (expects scalar Repr=" << static_cast<int>(expected_scalar)
            << "), but cel-cpp stamped Repr="
            << static_cast<int>(root_ann->repr);
      }
      ++productions_checked;
    }
  }
  EXPECT_GT(productions_checked, 130u);
}

TEST(AggregateGrammarL3Test, SampledCompositionsParseAndTypeAsBool) {
  Grammar g = BuildFullGrammar();
  CheckOptions opts;
  for (const ActivationBinding& v : ActivationSchema()) {
    opts.variable_specs.push_back(absl::StrCat(v.name, ":", TypeSpec(v.type)));
  }

  constexpr int kSeedsPerDepth = 200;
  constexpr int kDepths[] = {1, 3, 6};
  int total_sampled = 0;
  for (int depth : kDepths) {
    for (int seed = 0; seed < kSeedsPerDepth; ++seed) {
      std::mt19937_64 rng(0xC0FFEEull ^ (static_cast<uint64_t>(seed) << 8) ^
                          static_cast<uint64_t>(depth));
      l3::WalkCtx ctx{depth, {}, &rng};
      const std::string source = l3::Walk(g, CelType::Bool(), ctx);

      auto ta = ParseAndCheck(source, opts);
      ASSERT_THAT(ta, IsOk())
          << "aggregate L3 depth=" << depth << " seed=" << seed
          << ": composed source `" << source << "` failed ParseAndCheck";

      const int64_t root_id = ta->ast().root_expr().id();
      const NodeAnnotation* root_ann = ta->annotations().Find(root_id);
      ASSERT_NE(root_ann, nullptr);
      EXPECT_EQ(root_ann->repr, Repr::kBool)
          << "aggregate L3 depth=" << depth << " seed=" << seed
          << ": expected Bool, got Repr=" << static_cast<int>(root_ann->repr)
          << " on source `" << source << "`";
      ++total_sampled;
    }
  }
  EXPECT_EQ(total_sampled, kSeedsPerDepth * 3);
}

}  // namespace
}  // namespace celwasm::fuzz
