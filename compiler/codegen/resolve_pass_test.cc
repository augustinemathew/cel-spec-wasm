// Tests for ResolvePass — the first codegen pass.  Organised into:
//
//   1. Helpers — parse + check + resolve wrappers, ident collector,
//      variable lookup.
//   2. ResolvePassReprTest — parameterised over every kConst literal
//      kind; locks ReprOf mapping at the kConst layer.
//   3. ResolvePassTest — whole-pass structural invariants (non-repr
//      fields zero on kConst, empty local_types for literal programs,
//      every typed node gets a repr).
//   4. ResolvePassIdentReprTest — parameterised over every declarable
//      variable type (scalars, duration/timestamp, list, map,
//      message); locks the Repr-from-declared-type mapping at the
//      kIdent layer.
//   5. ResolvePassIdentTest — ident behaviour: dense local_index
//      assignment, unreferenced decls excluded, two-distinct-idents.
//   6. ResolvePassSameSlotShapeTest — parameterised over expression
//      shapes that reference the same variable at different AST
//      depths; every occurrence must share one slot (the nastiest
//      case being a message root that also appears inside its own
//      index expression).
//   7. ResolvePassMixedShapeTest — multi-variable, multi-kind
//      expressions; locks first-seen dense indexing and per-node
//      annotation correctness.

#include "compiler/codegen/resolve_pass.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "common/ast_traverse.h"
#include "common/ast_visitor_base.h"
#include "common/expr.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"
#include "gtest/gtest.h"
#include "testdata/e2e_fixture.pb.h"
#include "testdata/host_fixture_proto3.pb.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Force generated-pool registration of descriptors referenced by
// tests below.  Runs once at static init per test binary.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      google::protobuf::LinkMessageReflection<celwasm::testdata::Address>();
      google::protobuf::LinkMessageReflection<celwasm::testdata::HostMsg3>();
      return 0;
    }();

// ============================================================
// 1. Helpers
// ============================================================

// Parse + check + resolve an expression whose free variables are
// supplied as "name:Type" specs.
absl::StatusOr<ResolveOutput> ResolveWithVars(
    absl::string_view expression, std::vector<std::string> variable_specs) {
  CheckOptions opts;
  opts.variable_specs = std::move(variable_specs);
  auto ta = ParseAndCheck(expression, opts);
  if (!ta.ok()) return ta.status();
  return ResolvePass(*ta);
}

// Returns the `Repr` ResolvePass assigned to the root expression.
// Used by the kConst parameterised test.
Repr RootRepr(absl::string_view expression) {
  auto ta = ParseAndCheck(expression, {});
  EXPECT_THAT(ta, IsOk()) << "ParseAndCheck failed for: " << expression;
  if (!ta.ok()) return Repr::kUnknown;
  auto r = ResolvePass(*ta);
  EXPECT_THAT(r, IsOk()) << "ResolvePass failed for: " << expression;
  if (!r.ok()) return Repr::kUnknown;
  const int64_t root_id = ta->ast().root_expr().id();
  const NodeAnnotation* ann = r->annotations.Find(root_id);
  EXPECT_NE(ann, nullptr);
  return ann ? ann->repr : Repr::kUnknown;
}

// First-seen linear search for a variable by name.  O(n) — fine for
// test sizes.
const ResolvedVariable* FindVar(const ResolveOutput& r,
                                absl::string_view name) {
  for (const auto& v : r.variables) {
    if (v.name == name) return &v;
  }
  return nullptr;
}

// One kIdent occurrence in the AST.
struct IdentRef {
  std::string name;
  int64_t expr_id;
};

// Visitor that collects every kIdent node's (name, expr_id).
class IdentCollector : public cel::AstVisitorBase {
 public:
  void PreVisitExpr(const cel::Expr&) override {}
  void PostVisitExpr(const cel::Expr&) override {}
  void PostVisitIdent(const cel::Expr& e, const cel::IdentExpr& i) override {
    refs_.push_back({i.name(), e.id()});
  }
  std::vector<IdentRef> take() {
    return std::move(refs_);
  }

 private:
  std::vector<IdentRef> refs_;
};

// Walks the AST, returning every kIdent occurrence.
std::vector<IdentRef> CollectIdents(const TypedAst& ta) {
  IdentCollector c;
  cel::AstTraverse(ta.ast().root_expr(), c);
  return c.take();
}

// Verifies each kIdent node's `NodeAnnotation::local_index` matches
// the slot its variable was assigned in `variables`.  Returns a map
// name → occurrence count so callers can assert multiplicity.
absl::flat_hash_map<std::string, int> CheckIdentAnnotations(
    const TypedAst& ta, const ResolveOutput& r) {
  absl::flat_hash_map<std::string, int> counts;
  for (const IdentRef& ref : CollectIdents(ta)) {
    ++counts[ref.name];
    const ResolvedVariable* v = FindVar(r, ref.name);
    if (v == nullptr) {
      ADD_FAILURE() << "kIdent `" << ref.name
                    << "` has no entry in ResolveOutput::variables";
      continue;
    }
    const NodeAnnotation* ann = r.annotations.Find(ref.expr_id);
    if (ann == nullptr) {
      ADD_FAILURE() << "no NodeAnnotation for kIdent `" << ref.name
                    << "` (expr_id=" << ref.expr_id << ")";
      continue;
    }
    EXPECT_EQ(ann->local_index, v->local_index)
        << "kIdent `" << ref.name << "` at expr_id=" << ref.expr_id
        << " has local_index=" << ann->local_index
        << " but its variable was assigned slot " << v->local_index;
    EXPECT_EQ(ann->repr, v->repr)
        << "kIdent `" << ref.name << "` at expr_id=" << ref.expr_id
        << " has repr " << ReprName(ann->repr)
        << " but its variable's declared repr is " << ReprName(v->repr);
  }
  return counts;
}

// ============================================================
// 2. ResolvePassReprTest — kConst Repr mapping
// ============================================================

struct ReprCase {
  absl::string_view name;  // used for the test-instance label
  absl::string_view expression;
  Repr expected;
};

class ResolvePassReprTest : public ::testing::TestWithParam<ReprCase> {};

TEST_P(ResolvePassReprTest, RootReprMatches) {
  EXPECT_EQ(RootRepr(GetParam().expression), GetParam().expected);
}

INSTANTIATE_TEST_SUITE_P(
    EveryKConstKind, ResolvePassReprTest,
    ::testing::Values(ReprCase{"null", "null", Repr::kNull},
                      ReprCase{"bool", "true", Repr::kBool},
                      ReprCase{"int", "42", Repr::kInt},
                      ReprCase{"uint", "42u", Repr::kUint},
                      ReprCase{"double", "3.14", Repr::kDouble},
                      ReprCase{"string", "\"hi\"", Repr::kString},
                      ReprCase{"bytes", "b\"x\"", Repr::kBytes}),
    [](const ::testing::TestParamInfo<ReprCase>& info) {
      return std::string(info.param.name);
    });

// ============================================================
// 3. ResolvePassTest — whole-pass structural invariants
// ============================================================

TEST(ResolvePassTest, KConstLeavesNonReprFieldsAtZero) {
  auto ta = ParseAndCheck("42", {});
  ASSERT_THAT(ta, IsOk());
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk());
  const int64_t root_id = ta->ast().root_expr().id();
  const NodeAnnotation* ann = r->annotations.Find(root_id);
  ASSERT_NE(ann, nullptr);
  EXPECT_EQ(ann->field_number, 0u);
  EXPECT_EQ(ann->overload_id, "");
  EXPECT_EQ(ann->local_index, 0u);
  EXPECT_EQ(ann->scope_id, 0u);
  EXPECT_EQ(ann->attribute_id, 0u);
  EXPECT_EQ(ann->storage.kind, StorageKind::kNone);
  EXPECT_EQ(ann->storage.payload, 0u);
}

TEST(ResolvePassTest, NoVariablesForLiteralOnlyProgram) {
  // A literal-only program has no kIdent nodes, so `variables` is
  // empty and `$eval` later declares zero locals (one i32 per
  // referenced variable, per m2-ident-select-unknowns.md §2.6).
  auto ta = ParseAndCheck("1", {});
  ASSERT_THAT(ta, IsOk());
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk());
  EXPECT_EQ(r->max_scope_id, 0u);
  EXPECT_TRUE(r->variables.empty());
}

// `1 + 2` type-checks: root is a kCall of `_+_` over two kConst
// operands.  Even though expr_lower at M2 rejects kCall, ResolvePass
// doesn't gate on expression kind — it seeds repr from type_map for
// every typed node.
TEST(ResolvePassTest, AnnotatesEveryTypedNodeNotJustTheRoot) {
  auto ta = ParseAndCheck("1 + 2", {});
  ASSERT_THAT(ta, IsOk());
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk());
  for (const auto& [id, type] : ta->ast().type_map()) {
    const NodeAnnotation* ann = r->annotations.Find(id);
    ASSERT_NE(ann, nullptr) << "missing annotation for expr id=" << id;
    EXPECT_EQ(ann->repr, ReprOf(type)) << "repr mismatch for expr id=" << id;
  }
}

// ============================================================
// 4. ResolvePassIdentReprTest — every declarable variable type
// ============================================================
//
// Locks the declared-type → ResolvedVariable::repr + annotation.repr
// mapping for every type a caller can declare via CheckOptions.
// Also covers container types (list, map) and message types; for
// maps we vary the key/value to prove the ident's Repr is kMap
// regardless of the parameters.
//
// `Customer` / `HostMsg3` descriptors are touched in the fixture
// setup to force generated-pool registration.

struct IdentReprCase {
  absl::string_view name;  // used for the test-instance label
  absl::string_view type_spec;
  Repr expected;
};

class ResolvePassIdentReprTest
    : public ::testing::TestWithParam<IdentReprCase> {
 protected:
  static void SetUpTestSuite() {}
};

TEST_P(ResolvePassIdentReprTest, SingleIdentOfGivenTypeCarriesExpectedRepr) {
  const auto& c = GetParam();
  auto r = ResolveWithVars("x", {absl::StrCat("x:", c.type_spec)});
  ASSERT_THAT(r, IsOk()) << c.type_spec;
  ASSERT_EQ(r->variables.size(), 1u);
  EXPECT_EQ(r->variables[0].repr, c.expected) << c.type_spec;
}

INSTANTIATE_TEST_SUITE_P(
    AllScalars, ResolvePassIdentReprTest,
    ::testing::Values(IdentReprCase{"bool", "bool", Repr::kBool},
                      IdentReprCase{"int", "int", Repr::kInt},
                      IdentReprCase{"uint", "uint", Repr::kUint},
                      IdentReprCase{"double", "double", Repr::kDouble},
                      IdentReprCase{"string", "string", Repr::kString},
                      IdentReprCase{"bytes", "bytes", Repr::kBytes},
                      IdentReprCase{"duration", "duration", Repr::kDuration},
                      IdentReprCase{"timestamp", "timestamp",
                                    Repr::kTimestamp}),
    [](const ::testing::TestParamInfo<IdentReprCase>& info) {
      return std::string(info.param.name);
    });

INSTANTIATE_TEST_SUITE_P(
    Containers, ResolvePassIdentReprTest,
    ::testing::Values(
        // list<T> for each legal element kind.
        IdentReprCase{"list_of_bool", "list<bool>", Repr::kList},
        IdentReprCase{"list_of_int", "list<int>", Repr::kList},
        IdentReprCase{"list_of_uint", "list<uint>", Repr::kList},
        IdentReprCase{"list_of_double", "list<double>", Repr::kList},
        IdentReprCase{"list_of_string", "list<string>", Repr::kList},
        IdentReprCase{"list_of_bytes", "list<bytes>", Repr::kList},
        IdentReprCase{"list_of_list", "list<list<int>>", Repr::kList},
        IdentReprCase{"list_of_map", "list<map<string,int>>", Repr::kList},
        // map<K,V> for each legal key kind and varied values.
        IdentReprCase{"map_string_int", "map<string,int>", Repr::kMap},
        IdentReprCase{"map_int_string", "map<int,string>", Repr::kMap},
        IdentReprCase{"map_uint_double", "map<uint,double>", Repr::kMap},
        IdentReprCase{"map_bool_list", "map<bool,list<int>>", Repr::kMap},
        IdentReprCase{"map_string_message",
                      "map<string,celwasm.testdata.Customer>", Repr::kMap}),
    [](const ::testing::TestParamInfo<IdentReprCase>& info) {
      return std::string(info.param.name);
    });

INSTANTIATE_TEST_SUITE_P(
    Messages, ResolvePassIdentReprTest,
    ::testing::Values(
        IdentReprCase{"Customer", "celwasm.testdata.Customer", Repr::kMessage},
        IdentReprCase{"Address", "celwasm.testdata.Address", Repr::kMessage},
        IdentReprCase{"HostMsg3", "celwasm.testdata.HostMsg3", Repr::kMessage}),
    [](const ::testing::TestParamInfo<IdentReprCase>& info) {
      return std::string(info.param.name);
    });

// ============================================================
// 5. ResolvePassIdentTest — ident behaviour
// ============================================================

TEST(ResolvePassIdentTest, SingleIdentAssignsIndexZeroAndTagsNode) {
  auto ta = ParseAndCheck("x", CheckOptions{.variable_specs = {"x:int"}});
  ASSERT_THAT(ta, IsOk());
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk());
  ASSERT_EQ(r->variables.size(), 1u);
  EXPECT_EQ(r->variables[0].name, "x");
  EXPECT_EQ(r->variables[0].local_index, 0u);
  EXPECT_EQ(r->variables[0].repr, Repr::kInt);

  const NodeAnnotation* ann = r->annotations.Find(ta->ast().root_expr().id());
  ASSERT_NE(ann, nullptr);
  EXPECT_EQ(ann->local_index, 0u);
  EXPECT_EQ(ann->repr, Repr::kInt);
}

TEST(ResolvePassIdentTest, MultipleDistinctIdentsAssignDenseIndicesInOrder) {
  auto ta = ParseAndCheck(
      "x + y + z", CheckOptions{.variable_specs = {"x:int", "y:int", "z:int"}});
  ASSERT_THAT(ta, IsOk());
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk());
  ASSERT_EQ(r->variables.size(), 3u);
  EXPECT_EQ(r->variables[0].name, "x");
  EXPECT_EQ(r->variables[0].local_index, 0u);
  EXPECT_EQ(r->variables[1].name, "y");
  EXPECT_EQ(r->variables[1].local_index, 1u);
  EXPECT_EQ(r->variables[2].name, "z");
  EXPECT_EQ(r->variables[2].local_index, 2u);
  CheckIdentAnnotations(*ta, *r);
}

TEST(ResolvePassIdentTest, UnreferencedDeclaredVariablesAreNotInTable) {
  // Declare three variables, reference only one.  `variables`
  // contains only the referenced variable — unreferenced decls
  // don't get a slot.
  auto r = ResolveWithVars("x", {"x:int", "y:string", "z:bool"});
  ASSERT_THAT(r, IsOk());
  ASSERT_EQ(r->variables.size(), 1u);
  EXPECT_EQ(r->variables[0].name, "x");
}

// ============================================================
// 6. ResolvePassSameSlotShapeTest — one variable, many occurrences
// ============================================================
//
// Parameterised over expression shapes that reference the same
// variable at different AST depths (and different parent kinds:
// kCall, kSelect chain, index kCall, nested index).  Every
// occurrence must share one local_index; `variables` must contain
// exactly one entry with the expected Repr.

struct SameSlotCase {
  absl::string_view name;
  absl::string_view expression;
  std::vector<std::string> var_specs;
  absl::string_view var_name;
  Repr expected_repr;
  int expected_occurrences;
};

class ResolvePassSameSlotShapeTest
    : public ::testing::TestWithParam<SameSlotCase> {
 protected:
  static void SetUpTestSuite() {}
};

TEST_P(ResolvePassSameSlotShapeTest, AllOccurrencesShareOneSlot) {
  const auto& c = GetParam();
  auto ta =
      ParseAndCheck(c.expression, CheckOptions{.variable_specs = c.var_specs});
  ASSERT_THAT(ta, IsOk()) << c.name;
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk()) << c.name;
  ASSERT_EQ(r->variables.size(), 1u) << c.name;
  EXPECT_EQ(r->variables[0].name, c.var_name) << c.name;
  EXPECT_EQ(r->variables[0].local_index, 0u) << c.name;
  EXPECT_EQ(r->variables[0].repr, c.expected_repr) << c.name;

  auto counts = CheckIdentAnnotations(*ta, *r);
  EXPECT_EQ(counts[std::string(c.var_name)], c.expected_occurrences) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, ResolvePassSameSlotShapeTest,
    ::testing::Values(
        // Simplest: used twice as a direct kCall operand.
        SameSlotCase{
            "scalar_twice_under_kcall", "x + x", {"x:int"}, "x", Repr::kInt, 2},
        // Used three times across two nested kCall contexts.
        SameSlotCase{"scalar_thrice_mixed_ops",
                     "x + x * x",
                     {"x:int"},
                     "x",
                     Repr::kInt,
                     3},
        // List variable indexed at two different positions: `l[0] + l[1]`.
        SameSlotCase{"list_indexed_twice",
                     "l[0] + l[1]",
                     {"l:list<int>"},
                     "l",
                     Repr::kList,
                     2},
        // Same list as both the array and the index source.
        SameSlotCase{"list_as_both_array_and_index",
                     "arr[arr[0]]",
                     {"arr:list<int>"},
                     "arr",
                     Repr::kList,
                     2},
        // Message used twice at different select-chain depths.
        SameSlotCase{"message_at_two_select_depths",
                     "c.age == 1 || c.billing_address.city == \"Seattle\"",
                     {"c:celwasm.testdata.Customer"},
                     "c",
                     Repr::kMessage,
                     2},
        // The nastiest: root variable as array AND inside its own
        // index expression — a message fixture with list + scalar
        // fields.  `c.rep_i32[c.i32 + 10]`.
        SameSlotCase{"message_root_in_own_index",
                     "c.rep_i32[c.i32 + 10]",
                     {"c:celwasm.testdata.HostMsg3"},
                     "c",
                     Repr::kMessage,
                     2},
        // Same message reused across has() + select.
        SameSlotCase{"message_in_has_and_select",
                     "has(c.name) && c.age > 0",
                     {"c:celwasm.testdata.Customer"},
                     "c",
                     Repr::kMessage,
                     2}),
    [](const ::testing::TestParamInfo<SameSlotCase>& info) {
      return std::string(info.param.name);
    });

// ============================================================
// 7. ResolvePassMixedShapeTest — multiple distinct variables
// ============================================================
//
// Parameterised over multi-variable shapes.  Each case declares the
// expected first-seen order, per-variable Repr and local_index, and
// per-name occurrence count.  The IdentCollector-based annotation
// check confirms every kIdent node carries the right local_index.

struct MixedVar {
  absl::string_view name;
  uint32_t expected_index;
  Repr expected_repr;
  int expected_occurrences;
};

struct MixedShapeCase {
  absl::string_view name;
  absl::string_view expression;
  std::vector<std::string> var_specs;
  std::vector<MixedVar> variables;
};

class ResolvePassMixedShapeTest
    : public ::testing::TestWithParam<MixedShapeCase> {
 protected:
  static void SetUpTestSuite() {}
};

TEST_P(ResolvePassMixedShapeTest, EachVariableGetsItsOwnDenseSlot) {
  const auto& c = GetParam();
  auto ta =
      ParseAndCheck(c.expression, CheckOptions{.variable_specs = c.var_specs});
  ASSERT_THAT(ta, IsOk()) << c.name;
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk()) << c.name;
  ASSERT_EQ(r->variables.size(), c.variables.size()) << c.name;

  for (const MixedVar& v : c.variables) {
    const ResolvedVariable* rv = FindVar(*r, v.name);
    ASSERT_NE(rv, nullptr) << c.name << ": missing `" << v.name << "`";
    EXPECT_EQ(rv->local_index, v.expected_index) << c.name << ": " << v.name;
    EXPECT_EQ(rv->repr, v.expected_repr) << c.name << ": " << v.name;
  }

  auto counts = CheckIdentAnnotations(*ta, *r);
  for (const MixedVar& v : c.variables) {
    EXPECT_EQ(counts[std::string(v.name)], v.expected_occurrences)
        << c.name << ": " << v.name;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, ResolvePassMixedShapeTest,
    ::testing::Values(
        // Scalar alongside message select-chain.
        MixedShapeCase{"scalar_and_message",
                       "c.age == x",
                       {"c:celwasm.testdata.Customer", "x:int"},
                       {{"c", 0, Repr::kMessage, 1}, {"x", 1, Repr::kInt, 1}}},
        // Three variables: scalar, message (twice), scalar.
        MixedShapeCase{"three_vars_message_twice",
                       "n == c.age || c.billing_address.city == s",
                       {"n:int", "c:celwasm.testdata.Customer", "s:string"},
                       {{"n", 0, Repr::kInt, 1},
                        {"c", 1, Repr::kMessage, 2},
                        {"s", 2, Repr::kString, 1}}},
        // Scalar + list + map, three different container kinds at
        // different depths under the root `+`.
        MixedShapeCase{"scalar_list_map_at_different_depths",
                       "n + l[0] + m[\"x\"]",
                       {"n:int", "l:list<int>", "m:map<string,int>"},
                       {{"n", 0, Repr::kInt, 1},
                        {"l", 1, Repr::kList, 1},
                        {"m", 2, Repr::kMap, 1}}},
        // Message used as array root AND inside its own index
        // expression, alongside a separate scalar index variable:
        // `c.rep_i32[i + c.i32]`.
        MixedShapeCase{"message_root_and_index_plus_scalar_index",
                       "c.rep_i32[i + c.i32]",
                       {"c:celwasm.testdata.HostMsg3", "i:int"},
                       {{"c", 0, Repr::kMessage, 2}, {"i", 1, Repr::kInt, 1}}},
        // Nested indexing with two distinct container variables:
        // `m[l[0]]` — `m` at outer kCall, `l` inside inner kCall.
        MixedShapeCase{"nested_indexing_two_containers",
                       "m[l[0]]",
                       {"m:map<int,string>", "l:list<int>"},
                       {{"m", 0, Repr::kMap, 1}, {"l", 1, Repr::kList, 1}}},
        // Map-typed variable indexed by a string-variable key.
        MixedShapeCase{"map_and_key_var",
                       "m[k]",
                       {"m:map<string,int>", "k:string"},
                       {{"m", 0, Repr::kMap, 1}, {"k", 1, Repr::kString, 1}}}),
    [](const ::testing::TestParamInfo<MixedShapeCase>& info) {
      return std::string(info.param.name);
    });

// ============================================================
// 8. ResolvePassSelectTest — kSelect field_number preserved
// ============================================================
//
// Upstream layers lock the field-number resolution rules:
// `PopulateAnnotations` (typed_ast_test.cc) covers the cpp_type
// matrix, and `ParseAndCheck` (parse_and_check_test.cc) covers
// each AST shape.  Here we verify ResolvePass preserves those
// stamps in its output — the M2.C.1 invariant.

TEST(ResolvePassSelectTest, PreservesFieldNumberFromAnnotations) {
  auto ta = ParseAndCheck(
      "c.billing_address.city",
      CheckOptions{.variable_specs = {"c:celwasm.testdata.Customer"}});
  ASSERT_THAT(ta, IsOk());
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk());

  // Outer select is the root (`<inner>.city`); inner is its operand.
  const cel::Expr& city_sel = ta->ast().root_expr();
  const cel::Expr& billing_sel = city_sel.select_expr().operand();
  // Customer.billing_address = 9; Address.city = 1.
  EXPECT_EQ(r->annotations.Find(billing_sel.id())->field_number, 9u);
  EXPECT_EQ(r->annotations.Find(city_sel.id())->field_number, 1u);
}

// ============================================================
// 9. ResolvePassMapOriginTest — map-list-dispatch.md §2.6
// ============================================================
//
//   kMapExpr        → kArena
//   kIdent[map<>]   → kHost   (Activation::Bind hands us a backing)
//   kSelect[map<>]  → kHost   (proto map field via ProtoBacking)
//   non-map nodes   → kDynamic (default — never set)

TEST(ResolvePassMapOriginTest, MapLiteralStampedArena) {
  auto r = ResolveWithVars("{1: 10}", {});
  ASSERT_THAT(r, IsOk());
  bool found_arena = false;
  for (const auto& [id, a] : r->annotations.nodes()) {
    if (a.map_origin == Origin::kArena) found_arena = true;
  }
  EXPECT_TRUE(found_arena);
}

TEST(ResolvePassMapOriginTest, MapTypedIdentStampedHost) {
  auto r = ResolveWithVars("m", {"m:map<int,int>"});
  ASSERT_THAT(r, IsOk());
  bool found_host = false;
  for (const auto& [id, a] : r->annotations.nodes()) {
    if (a.repr == Repr::kMap && a.map_origin == Origin::kHost) {
      found_host = true;
    }
  }
  EXPECT_TRUE(found_host);
}

TEST(ResolvePassMapOriginTest, NonMapNodesStayDynamic) {
  // Scalar program — no map_origin should be promoted off the
  // kDynamic default for any node.
  auto r = ResolveWithVars("true", {});
  ASSERT_THAT(r, IsOk());
  for (const auto& [id, a] : r->annotations.nodes()) {
    EXPECT_EQ(a.map_origin, Origin::kDynamic) << "expr id=" << id;
  }
}

// ============================================================
// 10. ResolvePassListOriginTest — map-list-dispatch.md §2.6
// ============================================================
//
//   kListExpr        → kArena
//   kIdent[list<>]   → kHost   (Activation::Bind hands us a backing)
//   kSelect[list<>]  → kHost   (proto repeated field via ProtoList)
//   non-list nodes   → kDynamic (default — never set)

TEST(ResolvePassListOriginTest, ListLiteralStampedArena) {
  auto r = ResolveWithVars("[1, 2, 3]", {});
  ASSERT_THAT(r, IsOk());
  bool found_arena = false;
  for (const auto& [id, a] : r->annotations.nodes()) {
    if (a.list_origin == Origin::kArena) found_arena = true;
  }
  EXPECT_TRUE(found_arena);
}

TEST(ResolvePassListOriginTest, ListTypedIdentStampedHost) {
  auto r = ResolveWithVars("xs", {"xs:list<int>"});
  ASSERT_THAT(r, IsOk());
  bool found_host = false;
  for (const auto& [id, a] : r->annotations.nodes()) {
    if (a.repr == Repr::kList && a.list_origin == Origin::kHost) {
      found_host = true;
    }
  }
  EXPECT_TRUE(found_host);
}

TEST(ResolvePassListOriginTest, NonListNodesStayDynamic) {
  // Scalar program — no list_origin should be promoted off the
  // kDynamic default for any node.
  auto r = ResolveWithVars("true", {});
  ASSERT_THAT(r, IsOk());
  for (const auto& [id, a] : r->annotations.nodes()) {
    EXPECT_EQ(a.list_origin, Origin::kDynamic) << "expr id=" << id;
  }
}

TEST(ResolvePassListOriginTest, ListLiteralIndexOperandStaysArena) {
  // `[1,2,3][1]` — the operand of `_[_]` is the kListExpr; it must
  // carry kArena so codegen routes to cel_list_at_arena.
  auto r = ResolveWithVars("[1, 2, 3][1]", {});
  ASSERT_THAT(r, IsOk());
  bool found_arena_list = false;
  for (const auto& [id, a] : r->annotations.nodes()) {
    if (a.repr == Repr::kList && a.list_origin == Origin::kArena) {
      found_arena_list = true;
    }
  }
  EXPECT_TRUE(found_arena_list);
}

// ============================================================
// 10. ResolvePassComprehensionScopeTest — M5.B Slice A
//
//     Locks: ComprehensionDetector early-reject is GONE; comprehension
//     iter / accu names get their own ResolvedVariable entries (with
//     `kind != kFreeVariable`); kIdent annotations inside the body
//     resolve through the scope stack; `scope_id` is set; nested
//     comprehensions stack scopes correctly with shadowing; the
//     attribute / origin visitors skip scope-bound idents.
// ============================================================

namespace {

// Walks `output.variables` for an entry by name + expected kind.
const ResolvedVariable* FindVarOfKind(const ResolveOutput& r,
                                      absl::string_view name,
                                      ResolvedVariableKind kind) {
  for (const auto& v : r.variables) {
    if (v.name == name && v.kind == kind) return &v;
  }
  return nullptr;
}

}  // namespace

TEST(ResolvePassComprehensionScopeTest, SingleIterListExistsAdmitted) {
  // `[1, 2, 3].exists(v, v > 0)` — was REJECTED by the early-rejecter
  // pre-Slice-A; now resolves to a non-error ResolveOutput.
  auto r = ResolveWithVars("[1, 2, 3].exists(v, v > 0)", {});
  ASSERT_THAT(r, IsOk());

  // iter_var `v` and accu_var `@result` get their own entries with
  // the right kind tags.
  const ResolvedVariable* v_iter =
      FindVarOfKind(*r, "v", ResolvedVariableKind::kComprehensionIter);
  ASSERT_NE(v_iter, nullptr) << "iter_var `v` missing from variables";
  EXPECT_EQ(v_iter->repr, Repr::kInt)
      << "iter_var `v` Repr should be kInt (list element type)";

  const ResolvedVariable* accu =
      FindVarOfKind(*r, "@result", ResolvedVariableKind::kComprehensionAccu);
  ASSERT_NE(accu, nullptr)
      << "accu_var `@result` missing from variables (probe confirmed cel-cpp "
         "uses `@result` not `__result__`)";
  EXPECT_EQ(accu->repr, Repr::kBool)
      << "accu_var Repr should be kBool (exists' accumulator type)";

  // No free variables — the expression has no Activation-bound names.
  for (const auto& var : r->variables) {
    EXPECT_NE(var.kind, ResolvedVariableKind::kFreeVariable)
        << "unexpected free variable `" << var.name
        << "` in literal-only "
           "comprehension";
  }

  // At least one scope was pushed.
  EXPECT_GE(r->max_scope_id, 1u);
}

TEST(ResolvePassComprehensionScopeTest, IdentScopeBindingsStamped) {
  // Every kIdent named `v` or `@result` inside the body must carry
  // `scope_id != 0`.  No kIdent at the top level (there isn't one for
  // this expression).
  auto ta = ParseAndCheck("[1, 2, 3].exists(v, v > 0)", {});
  ASSERT_THAT(ta, IsOk());
  auto r = ResolvePass(*ta);
  ASSERT_THAT(r, IsOk());
  for (const IdentRef& ref : CollectIdents(*ta)) {
    const NodeAnnotation* ann = r->annotations.Find(ref.expr_id);
    ASSERT_NE(ann, nullptr) << "missing annotation for " << ref.name;
    if (ref.name == "v" || ref.name == "@result") {
      EXPECT_NE(ann->scope_id, 0u)
          << "scope-bound ident `" << ref.name << "` missing scope_id stamp";
    }
  }
}

TEST(ResolvePassComprehensionScopeTest, FreeVarVisibleInsideBody) {
  // `[1,2,3].exists(v, v > x)` — `x` is a free int var; `v` shadows
  // nothing.  Both bind correctly; `x` stays in the activation surface.
  auto r = ResolveWithVars("[1, 2, 3].exists(v, v > x)", {"x:int"});
  ASSERT_THAT(r, IsOk());

  const ResolvedVariable* free_x =
      FindVarOfKind(*r, "x", ResolvedVariableKind::kFreeVariable);
  ASSERT_NE(free_x, nullptr) << "free variable `x` not bound";
  EXPECT_EQ(free_x->repr, Repr::kInt);

  const ResolvedVariable* iter_v =
      FindVarOfKind(*r, "v", ResolvedVariableKind::kComprehensionIter);
  ASSERT_NE(iter_v, nullptr);
}

TEST(ResolvePassComprehensionScopeTest, ShadowedFreeVarKeepsBothBindings) {
  // A free `v:int` declaration AND an inner-comprehension `v` iter_var:
  // both must exist as distinct entries in `variables`.  The inner
  // refs resolve to the iter binding (scope_id != 0); refs OUTSIDE the
  // comprehension would resolve to the free binding — but here we
  // construct a CEL expression where `v` only appears inside, so the
  // free `v` is unreferenced and won't appear in variables_.  Use `v`
  // outside too: `v + [1,2,3].exists(v, v > 0) ? 1 : 0` — has both.
  auto r = ResolveWithVars("(v > 0) && [1, 2, 3].exists(v, v > 0)", {"v:int"});
  ASSERT_THAT(r, IsOk());

  // Free `v` exists (referenced at the top level).
  const ResolvedVariable* free_v =
      FindVarOfKind(*r, "v", ResolvedVariableKind::kFreeVariable);
  ASSERT_NE(free_v, nullptr)
      << "free `v` should be in variables (referenced at top level)";

  // Iter `v` exists separately.
  const ResolvedVariable* iter_v =
      FindVarOfKind(*r, "v", ResolvedVariableKind::kComprehensionIter);
  ASSERT_NE(iter_v, nullptr)
      << "iter `v` should be a distinct entry from the free `v`";
  EXPECT_NE(free_v->local_index, iter_v->local_index)
      << "free and iter `v` must have distinct local_index slots";
}

TEST(ResolvePassComprehensionScopeTest, NestedComprehensionMaxScopeIdGrows) {
  // `[1].exists(y, [0].exists(y, y == 0))` — depth-2 nesting; inner y
  // shadows outer y.  Same-name shadowing is the canonical edge case
  // from `m5-comprehensions-design.md §3.5`.
  auto r = ResolveWithVars("[1].exists(y, [0].exists(y, y == 0))", {});
  ASSERT_THAT(r, IsOk());
  EXPECT_GE(r->max_scope_id, 2u)
      << "nested comprehensions must each push a fresh scope";

  // TWO iter-`y` entries (outer y and inner y) — distinct slots.
  int iter_y_count = 0;
  for (const auto& v : r->variables) {
    if (v.name == "y" && v.kind == ResolvedVariableKind::kComprehensionIter) {
      ++iter_y_count;
    }
  }
  EXPECT_EQ(iter_y_count, 2)
      << "outer + inner `y` should be distinct ResolvedVariable entries";
}

TEST(ResolvePassComprehensionScopeTest, IterVarNotAnAttributeRoot) {
  // The AttributePathResolver must not intern comprehension-scope
  // iter / accu vars as attribute roots — they aren't activation
  // attributes and can't appear in unknown-pattern matches.
  auto r = ResolveWithVars("[1, 2, 3].exists(v, v > 0)", {});
  ASSERT_THAT(r, IsOk());
  for (const auto& [id, ann] : r->annotations.nodes()) {
    if (ann.scope_id != 0) {
      EXPECT_EQ(ann.attribute_id, 0u)
          << "scope-bound expr id=" << id
          << " must have attribute_id=0 (not an attribute root)";
    }
  }
  // The attribute table should NOT contain entries with root_variable
  // == "v" or "@result".
  for (const AttributeEntryRow& row : r->attributes) {
    EXPECT_NE(row.root_variable, "v")
        << "iter_var `v` leaked into attribute intern table";
    EXPECT_NE(row.root_variable, "@result")
        << "accu_var `@result` leaked into attribute intern table";
  }
}

TEST(ResolvePassComprehensionScopeTest, AllMacroAccuIsBool) {
  // `[1,2,3].all(v, v > 0)` — accu starts at `true`; loop_step uses
  // `&&` to combine.  Accu Repr is bool just like exists.
  auto r = ResolveWithVars("[1, 2, 3].all(v, v > 0)", {});
  ASSERT_THAT(r, IsOk());
  const ResolvedVariable* accu =
      FindVarOfKind(*r, "@result", ResolvedVariableKind::kComprehensionAccu);
  ASSERT_NE(accu, nullptr);
  EXPECT_EQ(accu->repr, Repr::kBool);
}

TEST(ResolvePassComprehensionScopeTest, MapMacroAccuIsList) {
  // `[1,2,3].map(v, v * 2)` — accu starts at `[]`; loop_step appends.
  // Accu Repr is list.  Confirms scope resolver handles non-bool accu.
  auto r = ResolveWithVars("[1, 2, 3].map(v, v * 2)", {});
  ASSERT_THAT(r, IsOk());
  const ResolvedVariable* accu =
      FindVarOfKind(*r, "@result", ResolvedVariableKind::kComprehensionAccu);
  ASSERT_NE(accu, nullptr);
  EXPECT_EQ(accu->repr, Repr::kList);
}

TEST(ResolvePassComprehensionScopeTest, ExistsOneAccuIsInt) {
  // `[1,2,3].exists_one(v, v > 0)` — accu starts at 0 (int); result
  // is `accu == 1` (bool).  The ACCU's repr is int, even though the
  // comprehension's OUTPUT repr is bool (per design §3.8).
  auto r = ResolveWithVars("[1, 2, 3].exists_one(v, v > 0)", {});
  ASSERT_THAT(r, IsOk());
  const ResolvedVariable* accu =
      FindVarOfKind(*r, "@result", ResolvedVariableKind::kComprehensionAccu);
  ASSERT_NE(accu, nullptr);
  EXPECT_EQ(accu->repr, Repr::kInt);
}

}  // namespace
}  // namespace celwasm
