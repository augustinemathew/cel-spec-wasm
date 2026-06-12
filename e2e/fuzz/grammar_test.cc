// The grammar's validation ladder, over THE grammar
// (`BuildGrammar()` — scalars + aggregates, the one the drivers
// mine):
//
//   L1 — `Grammar::Validate()` structural checks (unit-tested in
//        isolation on synthetic mini-grammars below).
//   L2 — every registered production individually parses and
//        type-checks as its declared target against the REAL
//        cel-cpp parser+checker.
//   L3 — sampled compositions through `GenerateExpr` (the same
//        walker the oracle property and the miner drive) parse and
//        type-check at depths {1, 3, 6}.
//
// Plus unit coverage for `GenerateExpr` itself (determinism, depth
// budget, per-target reachability).

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

#include "e2e/fuzz/catalog.h"
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

// ── The catalog: L1 self-consistency ────────────────────────────

TEST(GrammarCatalogTest, BuildsAndPassesL1Validation) {
  // `BuildGrammar()` ABSL_CHECKs internally if L1 fails — so
  // reaching this point is itself the assertion.  We also check
  // structural properties to detect catalog-skew.
  Grammar g = BuildGrammar();
  EXPECT_THAT(g.Validate(), IsOk());
  // Every scalar target, and list<T> for every scalar T, must have
  // at least one production registered.
  for (const CelType& t :
       {CelType::Bool(), CelType::Int(), CelType::Uint(), CelType::Double(),
        CelType::String(), CelType::Bytes()}) {
    EXPECT_TRUE(g.HasType(t)) << "catalog is missing target " << TypeKey(t);
    EXPECT_FALSE(g.Rules(t).empty()) << TypeKey(t);
    EXPECT_TRUE(g.HasType(CelType::List(t)))
        << "catalog is missing list<" << TypeKey(t) << ">";
    EXPECT_FALSE(g.Rules(CelType::List(t)).empty());
  }
  // Catalog size sanity check — catches accidental wholesale
  // deletions.  Bump as the catalog grows; a drop below means we
  // lost a category by accident.
  EXPECT_GT(g.TotalProductions(), 130u);
}

TEST(GrammarCatalogTest, EveryActivationBindingHasIdentLeaf) {
  Grammar g = BuildGrammar();
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

TEST(GrammarL2Test, EveryProductionParsesAndTypesAsDeclared) {
  Grammar g = BuildGrammar();
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
  EXPECT_GT(productions_checked, 130u);
}

// ── L3 — sampled composition check at varying depths ────────────
//
// Walks THE generator (`GenerateExpr` — the same walker the
// oracle property and the miner drive) and asserts every composed
// source parses and types as the declared target against the real
// cel-cpp checker.

TEST(GrammarL3Test, SampledCompositionsParseAndTypeAsBool) {
  Grammar g = BuildGrammar();

  // The activation idents are the only free idents the catalog can
  // emit; declare them once.  No synthesised slot vars needed
  // because L3 generates self-contained source (no `%i` left over).
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
      GenCtx ctx = NewGenCtx(depth, rng);
      const std::string source = GenerateExpr(g, CelType::Bool(), ctx);

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

// ── GenerateExpr unit coverage (determinism, depth, targets) ─────

TEST(GenerateExprTest, DepthZeroAlwaysProducesAValidLeafSource) {
  Grammar g = BuildGrammar();
  CheckOptions opts;
  for (const ActivationBinding& v : ActivationSchema()) {
    opts.variable_specs.push_back(absl::StrCat(v.name, ":", TypeSpec(v.type)));
  }

  for (uint64_t seed = 0; seed < 64; ++seed) {
    std::mt19937_64 rng(seed);
    GenCtx ctx = NewGenCtx(/*depth=*/0, rng);
    const std::string source = GenerateExpr(g, CelType::Bool(), ctx);

    auto ta = ParseAndCheck(source, opts);
    ASSERT_THAT(ta, IsOk())
        << "depth-0 seed=" << seed << " source=`" << source << "`";

    // Root must annotate as Bool.
    const int64_t root_id = ta->ast().root_expr().id();
    const NodeAnnotation* ann = ta->annotations().Find(root_id);
    ASSERT_NE(ann, nullptr);
    EXPECT_EQ(ann->repr, Repr::kBool) << source;
  }
}

TEST(GenerateExprTest, DeterministicForFixedSeed) {
  Grammar g = BuildGrammar();
  // Constant seeds are the POINT here — the test asserts the
  // generator is a pure function of (seed, depth).
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937_64 rng_a(42);
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937_64 rng_b(42);
  GenCtx ctx_a = NewGenCtx(4, rng_a);
  GenCtx ctx_b = NewGenCtx(4, rng_b);
  EXPECT_EQ(GenerateExpr(g, CelType::Bool(), ctx_a),
            GenerateExpr(g, CelType::Bool(), ctx_b));
}

TEST(GenerateExprTest, DifferentSeedsProduceDifferentSources) {
  // Stochastic — extremely unlikely for two different 64-bit
  // seeds at depth=6 to produce identical sources, but the
  // assertion guards against accidental seed-ignoring.
  Grammar g = BuildGrammar();
  // Distinct constant seeds, deliberately — see comment above.
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937_64 rng_a(1);
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937_64 rng_b(2);
  GenCtx ctx_a = NewGenCtx(6, rng_a);
  GenCtx ctx_b = NewGenCtx(6, rng_b);
  EXPECT_NE(GenerateExpr(g, CelType::Bool(), ctx_a),
            GenerateExpr(g, CelType::Bool(), ctx_b));
}

TEST(GenerateExprTest, DepthBudgetActuallyBoundsRecursion) {
  // A depth-1 expression must have at most one level of operator
  // wrapping above a leaf — so its source can't be insanely long.
  // This catches the regression where the walker forgets to
  // decrement the budget.  (Bound sized for the full grammar's
  // widest leaf + one ternary wrap.)
  Grammar g = BuildGrammar();
  for (uint64_t seed = 0; seed < 16; ++seed) {
    std::mt19937_64 rng(seed);
    GenCtx ctx = NewGenCtx(/*depth=*/1, rng);
    const std::string source = GenerateExpr(g, CelType::Bool(), ctx);
    EXPECT_LT(source.size(), 400u)
        << "depth-1 source was " << source.size()
        << " chars, suggesting the depth budget isn't being "
           "honoured. source=`"
        << source << "`";
  }
}

TEST(GrammarCatalogTest, EveryListAndMapHasALeaf) {
  // L1 already checks this generically; this test re-states the
  // invariant for the new aggregate targets so a future catalog
  // edit that drops the literal-only leaf gets a focused
  // failure message.
  Grammar g = BuildGrammar();
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

}  // namespace
}  // namespace celwasm::fuzz
