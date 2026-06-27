// m31 e2e — compile-time materialization of constant list literals.
//
// A list literal whose elements are all constants (recursively) is
// packed into rodata at compile time as the byte-identical arena
// representation and lowers to a single i32.const; the read-only
// kernels cannot tell it from an arena-built list (m31 §2).  This suite
// is the end-to-end "done" proof: materialized lists evaluate
// identically to arena-built ones across index / size / `in` / equality
// / iteration, and the per-Eval build path still works for non-const
// lists.
//
// Runs in both link modes via `link_mode_e2e_cc_test`.  Whether a given
// list actually materializes (lowers to i32.const, no cel_list_create)
// is pinned at the codegen-IR level in
// `compiler/codegen/expr_lower_test.cc`; here we assert the *behavior*
// is unchanged by materialization.

#include <cstdint>
#include <string>

#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/internal/cel_host.h"  // HostListBacking definition (Size()).
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "testdata/host_fixture_proto3.pb.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::e2e::CompilePlan;
using ::celwasm::e2e::EvalOk;

// Link the proto3 fixture descriptor so message literals resolve by FQN.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::HostMsg3>();
      return 0;
    }();

absl::StatusOr<Compiler> CompilerEmpty() {
  Compiler::Builder b;
  return std::move(b).Build();
}

// `[1, 2, …, n]` as source.
std::string IntListLiteral(int n) {
  std::string s = "[1";
  for (int i = 2; i <= n; ++i) {
    absl::StrAppend(&s, ", ", i);
  }
  absl::StrAppend(&s, "]");
  return s;
}

// ── Correctness: materialized const lists evaluate identically ───────
class MaterializedListEvalTest : public ::testing::Test {};

TEST_F(MaterializedListEvalTest, IntListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[10, 20, 30][1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 20);
}

TEST_F(MaterializedListEvalTest, ConstListSize) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "size([1, 2, 3, 4, 5])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 5);
}

TEST_F(MaterializedListEvalTest, StringListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"(["alpha", "beta", "gamma"][2])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "gamma");
}

TEST_F(MaterializedListEvalTest, UintListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[10u, 20u, 30u][1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsUint(), 20u);
}

TEST_F(MaterializedListEvalTest, BoolListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[true, false, true][1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(MaterializedListEvalTest, DoubleListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1.5, 2.5, 3.5][2]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsDouble(), 3.5);
}

TEST_F(MaterializedListEvalTest, BytesListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"([b"\x01", b"\x02", b"\x03"][1])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBytes(), absl::string_view("\x02", 1));
}

TEST_F(MaterializedListEvalTest, NullListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[null, null, null][0]");
  Activation a;
  EXPECT_TRUE(EvalOk(instance, a).IsNull());
}

// Nested const list: inner lists materialize innermost-first and embed
// in the outer run; `[[1,2],[3,4]][1][0]` walks both levels.
TEST_F(MaterializedListEvalTest, NestedListIndexed) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[[1, 2], [3, 4]][1][0]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

// A bare `[]` types as `list(dyn)` and is rejected by the static subset
// (RejectDyn) before materialization, so empty list literals only occur
// in typed contexts (comprehension accu/range); the empty-materialization
// byte layout is pinned at the builder unit level
// (StaticMemoryBuilderTest.MaterializeListEmpty).

// The materialized list as the whole result decodes to a 3-element list.
TEST_F(MaterializedListEvalTest, ListAsRootValue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[10, 20, 30]");
  Activation a;
  Value v = EvalOk(instance, a);
  ASSERT_EQ(v.kind(), Value::Kind::kList);
  EXPECT_EQ((*v.ListBacking())->Size(), 3u);
}

TEST_F(MaterializedListEvalTest, InMembershipPresent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "2 in [1, 2, 3]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(MaterializedListEvalTest, InMembershipAbsent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "5 in [1, 2, 3]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// ── Equivalence: materialized list compares equal to an arena-built
// one (the byte-identity premise observed through `==`) ──────────────
class MaterializedListEquivalenceTest : public ::testing::Test {};

TEST_F(MaterializedListEquivalenceTest, SelfEqual) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3] == [1, 2, 3]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// Left side materializes; the right side is built in the arena by a
// comprehension (`.map(x, x)` produces an arena list).  Equal proves a
// materialized list and an arena-built list compare identically.
TEST_F(MaterializedListEquivalenceTest, MaterializedEqualsArenaBuilt) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3] == [1, 2, 3].map(x, x)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(MaterializedListEquivalenceTest, UnequalDiffers) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3] == [1, 2, 4]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

// ── Comprehensions over a materialized iter_range (read-only) ────────
class MaterializedRangeComprehensionTest : public ::testing::Test {};

TEST_F(MaterializedRangeComprehensionTest, MapOverMaterializedRange) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].map(x, x * 2)[1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 4);
}

TEST_F(MaterializedRangeComprehensionTest, FilterOverMaterializedRange) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "size([1, 2, 3, 4].filter(x, x > 2))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 2);
}

TEST_F(MaterializedRangeComprehensionTest, ExistsOverMaterializedRange) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].exists(x, x == 2)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ── Negative: a non-const list is NOT materialized and still builds
// correctly per-Eval (eligibility is purely syntactic — `1 + 0` is a
// call, so the list is ineligible) ──────────────────────────────────
class NonConstListBuildPathTest : public ::testing::Test {};

TEST_F(NonConstListBuildPathTest, NonConstElementListStillBuilds) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1 + 0, 2, 3][0]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 1);
}

// A const list nested inside a non-const list: the inner materializes,
// the outer builds per-Eval and embeds the materialized inner.
TEST_F(NonConstListBuildPathTest, ConstListNestedInNonConstList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[[1, 2], [3 + 0, 4]][0][1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 2);
}

// ── Element kinds the materializer does NOT handle keep the per-Eval
// build path and still evaluate correctly.  Const maps now materialize
// (so a const list of const maps materializes end-to-end), but proto
// messages are host-side objects (externref handles via
// cel_host.cel_make_message), not bytes in linear memory, so a list of
// them can NEVER materialize into rodata (m31 §6 exclusion).  Both
// correctly evaluate. ──────────────────────────────────────────────
class UnmaterializedElementTypeTest : public ::testing::Test {};

TEST_F(UnmaterializedElementTypeTest, ListOfMapsBuildsAndEvals) {
  // `[{1:10},{2:20}]` is list<map<int,int>>; the maps and the list all
  // materialize into rodata now — and index correctly.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[{1: 10}, {2: 20}][1][2]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 20);
}

TEST_F(UnmaterializedElementTypeTest, ListOfProtoStructsBuildsAndEvals) {
  // A list of proto message literals — host objects, never materialized;
  // builds per-Eval and indexes + field-reads correctly.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "[celwasm.testdata.HostMsg3{i32: 1}, "
                              "celwasm.testdata.HostMsg3{i32: 2}][1].i32");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 2);
}

// ── Nesting cross product: each container kind (list / map / struct)
// holding each container kind, evaluated to a scalar leaf.  List-as-
// outer is covered above (NestedListIndexed, ListOfMaps*, ListOf
// ProtoStructs*); here are the map-as-outer and struct-as-outer rows,
// plus a few triple-nested combos.  All-list and all-map (and mixed
// list/map) const chains materialize; struct-bearing chains build
// per-Eval — all must eval correctly. ──────────────────────────────
class AggregateNestingCrossProductTest : public ::testing::Test {};

// Outer = map.
TEST_F(AggregateNestingCrossProductTest, MapOfList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{1: [10, 20]}[1][1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 20);
}

TEST_F(AggregateNestingCrossProductTest, MapOfMap) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{1: {2: 30}}[1][2]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 30);
}

TEST_F(AggregateNestingCrossProductTest, MapOfStruct) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "{1: celwasm.testdata.HostMsg3{i32: 5}}[1].i32");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 5);
}

// Outer = struct (proto fields: repeated / map / message).
TEST_F(AggregateNestingCrossProductTest, StructOfList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "celwasm.testdata.HostMsg3{rep_i32: [10, 20]}.rep_i32[1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 20);
}

TEST_F(AggregateNestingCrossProductTest, StructOfMap) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"(celwasm.testdata.HostMsg3{str_to_i32: {"a": 5}}.str_to_i32["a"])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 5);
}

TEST_F(AggregateNestingCrossProductTest, StructOfStruct) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler,
                              "celwasm.testdata.HostMsg3{inner: "
                              "celwasm.testdata.HostMsg3{i32: 9}}.inner.i32");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 9);
}

// Triple-nested combos across kinds.
TEST_F(AggregateNestingCrossProductTest, ListOfMapOfList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[{1: [7, 8]}][0][1][0]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 7);
}

TEST_F(AggregateNestingCrossProductTest, StructOfListOfStruct) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler,
                  "celwasm.testdata.HostMsg3{rep_msg: "
                  "[celwasm.testdata.HostMsg3{i32: 4}]}.rep_msg[0].i32");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 4);
}

TEST_F(AggregateNestingCrossProductTest, MapOfListOfStruct) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "{1: [celwasm.testdata.HostMsg3{i32: 6}]}[1][0].i32");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 6);
}

// ── Large const lists: with the 256 KiB rodata window (m31 §10) a
// const list far larger than the old ~328-element cap materializes and
// evaluates.  Each previously failed to compile (rodata overflow). ────
class LargeConstListTest : public ::testing::Test {};

TEST_F(LargeConstListTest, ThousandElementListSizes) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat("size(", IntListLiteral(1000), ")"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 1000);
}

TEST_F(LargeConstListTest, TenThousandElementListIndexes) {
  // ~240 KiB of materialized rodata — fits the 256 KiB window.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat(IntListLiteral(10000), "[9999]"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 10000);
}

// ── Const-map materialization ────────────────────────────────────────
//
// Const map literals are arena-byte representable (ArenaMapHeader +
// 48-byte {key,val} entry run + a baked SwissTable index for N >=
// kCelMapIndexThreshold) exactly like lists, so they materialize into
// rodata and lower to a single i32.const — byte-identical to a
// runtime-built map (pinned at the builder level in
// StaticMemoryBuilderKeystoneTest).  These are the behavioral end-to-end
// proofs: a materialized map evaluates identically across value/key kinds,
// index / size / `in` / `==`, and nesting.  Whether a given map actually
// lowers to i32.const (no cel_map_create) is pinned at the codegen-IR
// level in compiler/codegen/expr_lower_test.cc.

// `{1: 1, 2: 2, …, n: n}` source.
std::string IntMapLiteral(int n) {
  std::string s = "{1: 1";
  for (int i = 2; i <= n; ++i) {
    absl::StrAppend(&s, ", ", i, ": ", i);
  }
  absl::StrAppend(&s, "}");
  return s;
}

class ConstMapMaterializationTest : public ::testing::Test {};

// Value kinds (key = int).
TEST_F(ConstMapMaterializationTest, ValueInt) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{1: 42}[1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 42);
}

TEST_F(ConstMapMaterializationTest, ValueUint) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{1: 42u}[1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsUint(), 42u);
}

TEST_F(ConstMapMaterializationTest, ValueDouble) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{1: 1.5}[1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsDouble(), 1.5);
}

TEST_F(ConstMapMaterializationTest, ValueBool) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{1: true}[1]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ConstMapMaterializationTest, ValueString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"({1: "x"}[1])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsString(), "x");
}

TEST_F(ConstMapMaterializationTest, ValueBytes) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"({1: b"\x02"}[1])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBytes(), absl::string_view("\x02", 1));
}

TEST_F(ConstMapMaterializationTest, ValueNull) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{1: null}[1]");
  Activation a;
  EXPECT_TRUE(EvalOk(instance, a).IsNull());
}

// Key kinds (value = int) — the valid CEL map-key types.
TEST_F(ConstMapMaterializationTest, KeyInt) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{7: 1}[7]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 1);
}

TEST_F(ConstMapMaterializationTest, KeyUint) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{7u: 1}[7u]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 1);
}

TEST_F(ConstMapMaterializationTest, KeyBool) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{true: 1}[true]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 1);
}

TEST_F(ConstMapMaterializationTest, KeyString) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"({"k": 1}["k"])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 1);
}

// Operations.
TEST_F(ConstMapMaterializationTest, Size) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "size({1: 10, 2: 20, 3: 30})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 3);
}

TEST_F(ConstMapMaterializationTest, InKeyPresent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "2 in {1: 10, 2: 20}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ConstMapMaterializationTest, InKeyAbsent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "9 in {1: 10, 2: 20}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ConstMapMaterializationTest, EqualityOrderIndependent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{1: 10, 2: 20} == {2: 20, 1: 10}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// Nesting — both levels materialize once the map slice lands.
TEST_F(ConstMapMaterializationTest, NestedConstMap) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{1: {2: 30}}[1][2]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 30);
}

// A list of const maps materializes end-to-end once both materializers
// exist (the list materializer already handles const-aggregate elements
// — see IsConstMaterializable — so this lights up automatically).
TEST_F(ConstMapMaterializationTest, ListOfConstMaps) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[{1: 10}, {2: 20}][1][2]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 20);
}

// Large const map — the m31 §1 motivating case (`size(map100)` was the
// worst corpus ratio at 0.02×).  Materialization makes it O(1).
TEST_F(ConstMapMaterializationTest, LargeConstMapSizes) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat("size(", IntMapLiteral(100), ")"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 100);
}

// A lookup hit on a large (N >= kCelMapIndexThreshold) materialized map
// resolves through the BAKED SwissTable index — the headline correctness
// claim that an index-baked materialized map resolves lookups identically
// to a runtime-built one, proven end-to-end (the keystone pins byte-
// identity; this pins behavioral lookup through the bake).
TEST_F(ConstMapMaterializationTest, LargeConstMapLookupHit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat(IntMapLiteral(100), "[42]"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 42);
}

// A lookup of an absent key on the same large materialized map surfaces an
// eval error (no_such_key) — the baked index reports the miss exactly as a
// linear scan would.
TEST_F(ConstMapMaterializationTest, LargeConstMapLookupMiss) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat(IntMapLiteral(100), "[9999]"));
  Activation a;
  EXPECT_TRUE(EvalOk(instance, a).IsError());
}

}  // namespace
}  // namespace celwasm
