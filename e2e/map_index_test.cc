// m32.A e2e — runtime-built SwissTable map index, activated by the
// codegen-emitted terminal `cel_map_index_build` call.
//
// After a map is fully constructed (the last `cel_map_insert` of a map
// literal, or the accumulation loop of a map-producing comprehension),
// codegen emits `cel_map_index_build(map_slot)`.  The runtime builds a
// SwissTable hash index over the dense entries run when
// `count >= kCelMapIndexThreshold` (8), and the keyed kernels
// (`cel_map_lookup_arena`, `cel_map_in_arena`, `cel_map_eq_arena`)
// probe it instead of linear-scanning.  Below the threshold the build
// is a no-op and the kernels linear-scan — identical results either
// way (pure accelerator).
//
// This suite is the end-to-end "the index actually activates and stays
// correct" proof: indexed (>=8) and linear (<8) maps evaluate
// identically across keyed lookup (hit + miss), `in`, and equality,
// over int and string keys, for both literal and comprehension-built
// maps.  Whether the `cel_map_index_build` call is actually emitted is
// pinned at the codegen-IR level in
// `compiler/codegen/expr_lower_test.cc`; here we assert the *behavior*
// is unchanged with the index live.  Runs in both link modes via
// `link_mode_e2e_cc_test`.
//
// See `doc/implementation-plan/rewrite/m32-swisstable-map-index.md` §8
// and the frozen construction sequence in
// `doc/implementation-plan/rewrite/wat/73_map_swisstable_index.wat`.

#include <string>

#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/value.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::e2e::CompilePlan;
using ::celwasm::e2e::EvalOk;

absl::StatusOr<Compiler> CompilerEmpty() {
  Compiler::Builder b;
  return std::move(b).Build();
}

// `{0: 100, 1: 101, …, n-1: 100+n-1}` — an int-keyed map literal of n
// entries.  n >= 8 builds the index; n < 8 stays linear.
std::string IntKeyMapLiteral(int n) {
  std::string s = "{0: 100";
  for (int i = 1; i < n; ++i) {
    absl::StrAppend(&s, ", ", i, ": ", 100 + i);
  }
  absl::StrAppend(&s, "}");
  return s;
}

// `{"k0": 100, "k1": 101, …}` — a string-keyed map literal of n entries.
std::string StringKeyMapLiteral(int n) {
  std::string s = R"({"k0": 100)";
  for (int i = 1; i < n; ++i) {
    absl::StrAppend(&s, ", \"k", i, "\": ", 100 + i);
  }
  absl::StrAppend(&s, "}");
  return s;
}

// ── Indexed path: int keys, >= 8 entries (index built) ───────────────
class IndexedMapEvalTest : public ::testing::Test {};

TEST_F(IndexedMapEvalTest, IntKeyLargeMapLookupHit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // 9 entries (>= threshold 8): cel_map_index_build builds the index;
  // the lookup resolves through cel_map_index_find.
  auto instance =
      CompilePlan(*compiler, absl::StrCat(IntKeyMapLiteral(9), "[5]"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 105);
}

TEST_F(IndexedMapEvalTest, IntKeyLargeMapLookupFirstAndLast) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto first = CompilePlan(*compiler, absl::StrCat(IntKeyMapLiteral(9), "[0]"));
  auto last = CompilePlan(*compiler, absl::StrCat(IntKeyMapLiteral(9), "[8]"));
  Activation a;
  EXPECT_EQ(*EvalOk(first, a).AsInt(), 100);
  EXPECT_EQ(*EvalOk(last, a).AsInt(), 108);
}

TEST_F(IndexedMapEvalTest, IntKeyLargeMapMissingKeyIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // Key 99 is absent — the indexed probe must miss and surface a
  // no_such_key error, exactly as the linear scan would.
  auto instance =
      CompilePlan(*compiler, absl::StrCat(IntKeyMapLiteral(9), "[99]"));
  Activation a;
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

TEST_F(IndexedMapEvalTest, IntKeyLargeMapInPresent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat("3 in ", IntKeyMapLiteral(9)));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(IndexedMapEvalTest, IntKeyLargeMapInAbsent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat("42 in ", IntKeyMapLiteral(9)));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(IndexedMapEvalTest, IntKeyLargeMapEquality) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // Both sides build an index; equality walks one and probes the other.
  auto instance = CompilePlan(
      *compiler,
      absl::StrCat(IntKeyMapLiteral(9), " == ", IntKeyMapLiteral(9)));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ── Indexed path: string keys, >= 8 entries ──────────────────────────
TEST_F(IndexedMapEvalTest, StringKeyLargeMapLookupHit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, absl::StrCat(StringKeyMapLiteral(10), R"(["k7"])"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 107);
}

TEST_F(IndexedMapEvalTest, StringKeyLargeMapMissingKeyIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, absl::StrCat(StringKeyMapLiteral(10), R"(["nope"])"));
  Activation a;
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

TEST_F(IndexedMapEvalTest, StringKeyLargeMapInPresent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, absl::StrCat(R"("k2" in )", StringKeyMapLiteral(10)));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ── Linear path: < 8 entries (build no-ops) — parity ─────────────────
class LinearMapEvalTest : public ::testing::Test {};

TEST_F(LinearMapEvalTest, IntKeySmallMapLookupHit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // 4 entries (< threshold 8): cel_map_index_build is a no-op; the
  // lookup linear-scans.  Same result as the indexed path.
  auto instance =
      CompilePlan(*compiler, absl::StrCat(IntKeyMapLiteral(4), "[2]"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 102);
}

TEST_F(LinearMapEvalTest, IntKeySmallMapMissingKeyIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat(IntKeyMapLiteral(4), "[99]"));
  Activation a;
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

TEST_F(LinearMapEvalTest, StringKeySmallMapLookupHit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat(StringKeyMapLiteral(3), R"(["k1"])"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 101);
}

TEST_F(LinearMapEvalTest, IntKeySmallMapInPresentAndAbsent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto present =
      CompilePlan(*compiler, absl::StrCat("1 in ", IntKeyMapLiteral(4)));
  auto absent =
      CompilePlan(*compiler, absl::StrCat("9 in ", IntKeyMapLiteral(4)));
  Activation a;
  EXPECT_EQ(*EvalOk(present, a).AsBool(), true);
  EXPECT_EQ(*EvalOk(absent, a).AsBool(), false);
}

// The exact threshold boundary: 8 entries builds the index, 7 does not.
// Both must look up correctly (the boundary is the whole point of the
// no-op-below / build-at-or-above split).
TEST_F(LinearMapEvalTest, ThresholdBoundaryEntries) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto seven = CompilePlan(*compiler, absl::StrCat(IntKeyMapLiteral(7), "[6]"));
  auto eight = CompilePlan(*compiler, absl::StrCat(IntKeyMapLiteral(8), "[6]"));
  Activation a;
  EXPECT_EQ(*EvalOk(seven, a).AsInt(), 106);  // 7 entries: linear.
  EXPECT_EQ(*EvalOk(eight, a).AsInt(), 106);  // 8 entries: indexed.
}

// ── Comprehension-built maps (transformMap) — index built after loop ─
class ComprehensionMapIndexEvalTest : public ::testing::Test {};

TEST_F(ComprehensionMapIndexEvalTest, TransformMapLargeLookupHit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // The source map has 9 entries; transformMap yields a 9-entry map,
  // so the comprehension-terminal cel_map_index_build builds the index.
  // Look up a key in the result.
  auto instance = CompilePlan(
      *compiler,
      absl::StrCat(IntKeyMapLiteral(9), ".transformMap(k, v, v + 1)[5]"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 106);  // 105 + 1.
}

TEST_F(ComprehensionMapIndexEvalTest, TransformMapLargeMissingKeyIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      absl::StrCat(IntKeyMapLiteral(9), ".transformMap(k, v, v + 1)[99]"));
  Activation a;
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

TEST_F(ComprehensionMapIndexEvalTest, TransformMapStringKeyLargeLookupHit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, absl::StrCat(StringKeyMapLiteral(10),
                              R"(.transformMap(k, v, v * 2)["k3"])"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 206);  // 103 * 2.
}

TEST_F(ComprehensionMapIndexEvalTest, TransformMapSmallLookupHit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  // 4-entry transformMap result: build no-ops, linear scan — parity.
  auto instance = CompilePlan(
      *compiler,
      absl::StrCat(IntKeyMapLiteral(4), ".transformMap(k, v, v + 1)[2]"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 103);  // 102 + 1.
}

// ── Indexed path: key-hash token spaces beyond non-negative ints ─────
//
// The canonicalizing key hash (runtime/cel_map_hash.h) routes keys into
// three disjoint token spaces: non-negative ints [0, INT64_MAX],
// NEGATIVE ints (magnitude tagged into its own code space), and the
// uint tail (INT64_MAX, UINT64_MAX].  The suites above only exercise
// the first; these pin the other two through a built index (>= 8
// entries), both insert-side (literal keys) and lookup-side.

// `{-1: 100, -2: 101, …, -n: 100+n-1}` — all-negative int keys.
std::string NegIntKeyMapLiteral(int n) {
  std::string s = "{-1: 100";
  for (int i = 2; i <= n; ++i) {
    absl::StrAppend(&s, ", -", i, ": ", 100 + i - 1);
  }
  absl::StrAppend(&s, "}");
  return s;
}

TEST_F(IndexedMapEvalTest, NegativeIntKeyLargeMapLookupHit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat(NegIntKeyMapLiteral(9), "[-5]"));
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 104);
}

TEST_F(IndexedMapEvalTest, NegativeIntKeyLargeMapMissingKeyIsError) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, absl::StrCat(NegIntKeyMapLiteral(9), "[-99]"));
  Activation a;
  auto v_or = instance.Eval(a);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_TRUE(v_or->IsError());
}

TEST_F(IndexedMapEvalTest, NegativeIntKeyLargeMapInPresentAndAbsent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto present =
      CompilePlan(*compiler, absl::StrCat("-3 in ", NegIntKeyMapLiteral(9)));
  auto absent =
      CompilePlan(*compiler, absl::StrCat("-42 in ", NegIntKeyMapLiteral(9)));
  Activation a;
  EXPECT_EQ(*EvalOk(present, a).AsBool(), true);
  EXPECT_EQ(*EvalOk(absent, a).AsBool(), false);
}

// Uint keys straddling the int64 boundary: keys <= INT64_MAX take the
// shared int token; keys above it take the uint-high token.  Both
// spaces coexist in one index.
constexpr absl::string_view kUintHighKeyMap =
    "{1u: 101, 2u: 102, 3u: 103, 4u: 104, 5u: 105, 6u: 106, "
    "9223372036854775807u: 107, "   // INT64_MAX — int-token space
    "9223372036854775808u: 108, "   // INT64_MAX+1 — uint-high space
    "18446744073709551615u: 109}";  // UINT64_MAX

TEST_F(IndexedMapEvalTest, UintHighKeyLargeMapLookupHit) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto boundary = CompilePlan(
      *compiler, absl::StrCat(kUintHighKeyMap, "[9223372036854775808u]"));
  auto max = CompilePlan(
      *compiler, absl::StrCat(kUintHighKeyMap, "[18446744073709551615u]"));
  Activation a;
  EXPECT_EQ(*EvalOk(boundary, a).AsInt(), 108);
  EXPECT_EQ(*EvalOk(max, a).AsInt(), 109);
}

TEST_F(IndexedMapEvalTest, UintHighKeyLargeMapInPresentAndAbsent) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto present = CompilePlan(
      *compiler,
      absl::StrCat("18446744073709551615u in ", kUintHighKeyMap));
  auto absent = CompilePlan(
      *compiler,
      absl::StrCat("9223372036854775809u in ", kUintHighKeyMap));
  Activation a;
  EXPECT_EQ(*EvalOk(present, a).AsBool(), true);
  EXPECT_EQ(*EvalOk(absent, a).AsBool(), false);
}

}  // namespace
}  // namespace celwasm
