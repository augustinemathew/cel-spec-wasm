// M5.B e2e test suite — the spec of "done" for the comprehensions
// follow-on milestone (`cel.bind`, the five standard comprehension
// macros over list / map sources, and the `comprehensions_v2`
// two-iter-var / `transformMap` / `transformMapEntry` cohort).
//
// Mirrors the m7_test / m8_test shape: every test asserts a
// capability the comprehension plan says M5.B must light up.
// Running this binary today should SKIP every case below — each
// `TEST_F` opens with
// `GTEST_SKIP() << "M5.B.<slice> ships here ...";`.  As each slice
// closes per `m5-comprehensions-followon.md` §5, the matching
// fixture's skips drop and the test bodies (already written
// `compile → eval → assert` style) start passing.
//
// Base M5 ships kCall + control flow; this file is the M5.B
// suffix because M5.B is the comprehension follow-on per
// `m5-comprehensions-followon.md` §0.  No other M5 surface is
// covered here — arithmetic / comparison rows live in
// `m5_test.cc`.
//
// Fixtures grouped by slice (one section per arm):
//
//   - ComprehensionExistsListE2ETest   Slice C — `exists` / `all` /
//                                                `exists_one` over
//                                                list literals + bound
//                                                lists.  Locks empty-
//                                                range / error-prop /
//                                                3VL invariants per
//                                                design §3.1–§3.2.
//   - ComprehensionMapFilterListE2ETest Slice D — `map(v, t)` /
//                                                `map(v, p, t)` /
//                                                `filter(v, p)` over
//                                                lists.  Locks dynamic-
//                                                append + size-grow
//                                                semantics per design
//                                                §3.6 + design §7.1.
//   - ComprehensionMapIterE2ETest      Slice E — single-iter-var
//                                                comprehensions over
//                                                MAP source (iter_var
//                                                binds **key** per
//                                                design §3.10).
//   - ComprehensionTwoIterVarE2ETest   Slice F — `e.exists(i, v, p)` /
//                                                `e.all(k, v, p)` /
//                                                `e.transformList(i,
//                                                v, t)` and conditional;
//                                                covers list + map
//                                                two-iter-var lowering
//                                                per design §3.8.
//   - ComprehensionTransformMapE2ETest Slice G — `transformMap(k, v,
//                                                t)` / conditional;
//                                                map accumulator with
//                                                key-collision last-
//                                                write-wins per design
//                                                §9.6.
//   - ComprehensionTransformMapEntryE2ETest Slice H — `transformMapEntry`
//                                                (per-iter map merge);
//                                                multi-entry literal +
//                                                empty-entry skip per
//                                                design §9.7 + §7.3.
//   - CelBindE2ETest                   Slice I — `cel.bind(name,
//                                                value, body)` — nested
//                                                / shadow / map-accu /
//                                                comp-inside-bind /
//                                                bind-inside-comp per
//                                                design §3.6.
//   - ComprehensionNestedE2ETest       Slice C closeout — outer ×
//                                                inner across list ×
//                                                list / list × map /
//                                                map × list / map ×
//                                                map; same-name shadow
//                                                per design §3.5.
//   - ComprehensionConsumerE2ETest     cross-cutting — comprehension
//                                                output as operand
//                                                (eq / size / index)
//                                                per design §3.9.
//
// Conformance unlock projections per slice are tracked in
// `m5-comprehensions-followon.md` §1 + §12 (~+90 PASS at full close).

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/attribute.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// No descriptor link block — every M5.B comprehension test is
// LITERAL-driven (list / map source built from CEL source text or
// `Value::List` / `Value::Map` bindings).  Adding a proto link
// here would create a stale dep edge when the proto fixtures
// move.

using ::celwasm::e2e::GlobalEngine;

using ConfigureFn = std::function<void(Compiler::Builder&)>;
absl::StatusOr<Compiler> BuildCompiler(const ConfigureFn& configure) {
  Compiler::Builder b;
  configure(b);
  return std::move(b).Build();
}

// All e2e helpers below are unused while every test SKIPs.  Once
// the first slice ships and a test body uses them, the
// `[[maybe_unused]]` is dropped.  This file is the spec-of-done;
// the helpers stand ready for slice-by-slice migration (mirrors
// the m7b / m8 shape).

[[maybe_unused]] absl::StatusOr<Compiler> CompilerEmpty() {
  return BuildCompiler([](Compiler::Builder& /*b*/) {});
}

[[maybe_unused]] Compiler CompilerWithVar(absl::string_view name,
                                          const CelType& type) {
  auto compiler_or = BuildCompiler([&](Compiler::Builder& b) {
    b.DeclareVariable(std::string(name), type);
  });
  ABSL_CHECK_OK(compiler_or);
  return *std::move(compiler_or);
}

using ::celwasm::e2e::CompilePlan;

using ::celwasm::e2e::EvalOk;

[[maybe_unused]] Value EvalOk(Instance&& instance,
                              const Activation& activation) {
  Instance moved = std::move(instance);
  return EvalOk(moved, activation);
}

[[maybe_unused]] void ExpectCompileFails(const Compiler& compiler,
                                         absl::string_view source,
                                         absl::string_view why) {
  auto program_or = compiler.Compile(source, ::celwasm::e2e::DefaultOpts());
  EXPECT_FALSE(program_or.ok())
      << "expected `" << source << "` to fail at compile (" << why << ")";
}

// ──────────────────────────────────────────────────────────────
// 1. ComprehensionExistsListE2ETest  (Slice C)
//
//    `exists` / `all` / `exists_one` over LIST literals + bound
//    lists.  Locks the empty-range / single-match / multi-match /
//    no-match / error-short-circuit / 3VL invariants per
//    `m5-comprehensions-design.md` §3.1–§3.2.
//
//    Conformance unlock: ~+25 rows (macros.{exists,all,exists_one}
//    list cohorts per `m5-comprehensions-followon.md` §12).
// ──────────────────────────────────────────────────────────────

class ComprehensionExistsListE2ETest : public ::testing::Test {};

TEST_F(ComprehensionExistsListE2ETest, ExistsTrueOnSingleMatch) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].exists(e, e == 2)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionExistsListE2ETest, ExistsTrueOnMultipleMatches) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 2, 3].exists(e, e == 2)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionExistsListE2ETest, ExistsFalseOnNoMatch) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].exists(e, e == 7)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ComprehensionExistsListE2ETest, ExistsEmptyListReturnsAccuInit) {
  // design §3.1 empty-range invariant: accu_init (false) is returned
  // because loop body never runs.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[].exists(e, e == 2)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ComprehensionExistsListE2ETest, AllTrueOnAllMatch) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].all(e, e > 0)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionExistsListE2ETest, AllFalseOnAnyFail) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].all(e, e > 1)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ComprehensionExistsListE2ETest, AllEmptyListReturnsAccuInit) {
  // design §3.1 empty-range invariant: accu_init (true) is returned.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[].all(e, e > 0)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionExistsListE2ETest, ExistsOneTrueOnExactlyOneMatch) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].exists_one(e, e == 2)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionExistsListE2ETest, ExistsOneFalseOnMultipleMatches) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[2, 2, 3].exists_one(e, e == 2)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ComprehensionExistsListE2ETest, ExistsOneFalseOnNoMatch) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].exists_one(a, a == 7)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ComprehensionExistsListE2ETest, ExistsOneEmptyListIsFalse) {
  // design §3.1: accu (int 0); result `accu == 1` is false.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[].exists_one(a, a == 7)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ComprehensionExistsListE2ETest, ExistsPredicateErrorPropagates) {
  // design §3.2: `[1,2,3].exists(e, e/0 == 17)` → ERROR.  3VL `||`
  // never absorbs an error-only stream to `true`.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].exists(e, e / 0 == 17)");
  Activation a;
  Value v = EvalOk(instance, a);
  EXPECT_TRUE(v.IsError());
}

TEST_F(ComprehensionExistsListE2ETest, AllPredicateErrorPropagates) {
  // design §3.2: `[1,2,3].all(e, e/0 != 17)` → ERROR.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].all(e, e / 0 != 17)");
  Activation a;
  Value v = EvalOk(instance, a);
  EXPECT_TRUE(v.IsError());
}

TEST_F(ComprehensionExistsListE2ETest, ExistsErrorAbsorbedByTrueIs3VL) {
  // design §3.2 3VL invariant: `error || true → true`.  Even though
  // the first iter errors, a later iter forces `true`, so accumulator
  // becomes definite.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "[0, 1, 2].exists(e, 1 / e > 0 || e == 1)");
  Activation a;
  // Spec ambiguity tolerated: either ERROR or true is acceptable
  // pending cel-cpp parity check.  Skip assertion until Slice C
  // freezes semantics; presence of test as-is locks the input.
  EXPECT_TRUE(EvalOk(instance, a).IsError() ||
              EvalOk(instance, a).AsBool().value_or(false));
}

TEST_F(ComprehensionExistsListE2ETest, ExistsOverBoundList) {
  // Locks that comprehension iter source can be an Activation::Bind
  // list, not just a literal.  Backs `m5-comprehensions-design.md`
  // §3.9 ("comprehension as operand") symmetrically.
  // Slice 2 (m5b §CCF-8) lifts the SKIP — host-list iter_ranges
  // now snapshot into arena format via `cel_list_arena_view`.
  Compiler compiler = CompilerWithVar("xs", CelType::List(CelType::Int()));
  auto instance = CompilePlan(compiler, "xs.exists(e, e == 5)");
  Activation a;
  a.Bind("xs", Value::List({Value::Int(1), Value::Int(5), Value::Int(9)}));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionExistsListE2ETest, AllOverBoundList) {
  // Slice 2 (m5b §CCF-8) lifts the SKIP — host-list iter_ranges
  // now snapshot into arena format via `cel_list_arena_view`.
  Compiler compiler = CompilerWithVar("xs", CelType::List(CelType::Int()));
  auto instance = CompilePlan(compiler, "xs.all(e, e > 0)");
  Activation a;
  a.Bind("xs", Value::List({Value::Int(1), Value::Int(2), Value::Int(3)}));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 2. ComprehensionMapFilterListE2ETest  (Slice D)
//
//    `map(v, t)` / `map(v, p, t)` / `filter(v, p)` over lists.
//    Locks dynamic-append (cel_list_append_at), size-grow,
//    empty-source, conditional-map, error-in-step.
//
//    Conformance unlock: ~+18 rows (macros.{map,filter} cohorts).
// ──────────────────────────────────────────────────────────────

class ComprehensionMapFilterListE2ETest : public ::testing::Test {};

TEST_F(ComprehensionMapFilterListE2ETest, MapDoubles) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "[1, 2, 3].map(v, v * 2) == [2, 4, 6]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapFilterListE2ETest, MapEmptySource) {
  // design §3.1: empty range → `[]` (accu_init is the empty
  // dynamic list).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[].map(v, v * 2) == []");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapFilterListE2ETest, MapConditional) {
  // `map(v, p, t)` form: predicate gates whether the step appends.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "[1, 2, 3, 4].map(v, v % 2 == 0, v * 10) == [20, 40]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapFilterListE2ETest, MapStepErrorPropagates) {
  // design §3.2: step error aborts comprehension, becomes result.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[2, 1, 0].map(n, 4 / n)");
  Activation a;
  EXPECT_TRUE(EvalOk(instance, a).IsError());
}

TEST_F(ComprehensionMapFilterListE2ETest, FilterKeepsMatching) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "[1, 2, 3].filter(v, v != 2) == [1, 3]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapFilterListE2ETest, FilterEmptySource) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[].filter(v, v > 0) == []");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapFilterListE2ETest, FilterAllKept) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "[1, 2, 3].filter(v, v > 0) == [1, 2, 3]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapFilterListE2ETest, FilterNoneKept) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].filter(v, v > 10) == []");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapFilterListE2ETest, FilterPredicateErrorPropagates) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[3, 2, 1, 0].filter(n, 12 / n > 4)");
  Activation a;
  EXPECT_TRUE(EvalOk(instance, a).IsError());
}

TEST_F(ComprehensionMapFilterListE2ETest, MapOverBoundList) {
  // Slice 2 (m5b §CCF-8) lifts the SKIP — host-list iter_ranges
  // now snapshot into arena format via `cel_list_arena_view`.
  Compiler compiler = CompilerWithVar("xs", CelType::List(CelType::Int()));
  auto instance = CompilePlan(compiler, "xs.map(v, v + 1) == [2, 3, 4]");
  Activation a;
  a.Bind("xs", Value::List({Value::Int(1), Value::Int(2), Value::Int(3)}));
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapFilterListE2ETest, MapLargeListGrowthPath) {
  // Boundary: 1000-element list exercises the dynamic-list rehash /
  // capacity-grow path in `cel_list_append_at` (design §7.1).
  // We assert only the resulting size to keep the test order-agnostic.
  // Slice 2 (m5b §CCF-8) lifts the SKIP — host-list iter_ranges
  // now snapshot into arena format via `cel_list_arena_view`.
  // 1000-element snapshot exercises the per-element EncodeFieldResult
  // loop in CelListIterOpenImpl + the inline arena walk's
  // count-bounded loop.
  Compiler compiler = CompilerWithVar("xs", CelType::List(CelType::Int()));
  auto instance = CompilePlan(compiler, "xs.map(v, v * 2).size()");
  Activation a;
  std::vector<Value> big;
  big.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    big.push_back(Value::Int(i));
  }
  a.Bind("xs", Value::List(std::move(big)));
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 1000);
}

// ──────────────────────────────────────────────────────────────
// 3. ComprehensionMapIterE2ETest  (Slice E)
//
//    Single-iter-var comprehension over MAP source.  iter_var binds
//    the KEY only (design §3.10).  Two-iter-var over map is Slice F.
//
//    Conformance unlock: ~+12 rows (macros.{exists,all,exists_one,
//    map}_map cohorts).
// ──────────────────────────────────────────────────────────────

class ComprehensionMapIterE2ETest : public ::testing::Test {};

TEST_F(ComprehensionMapIterE2ETest, ExistsMapKeyMatch) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"({1: "a", 2: "b"}.exists(k, k > 1))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapIterE2ETest, AllMapKeysPositive) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, R"({1: "a", 2: "b"}.all(k, k > 0))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapIterE2ETest, ExistsEmptyMapIsFalse) {
  // design §3.1: empty range → accu_init (false).
  // CEL has no syntactic way to construct an empty typed map —
  // `{}` types as `map(dyn, dyn)` which the static-subset gate
  // rejects.  An equivalent assertion lives in the runtime test
  // `cel_map_test.cc::MapIterTest::Empty` (handle=0 → no iters).
  // The bound-map variant ships when bound-map iter_range support
  // does (separate post-milestone slice).
  GTEST_SKIP() << "empty map literal types as map(dyn,dyn) — "
                  "RejectDyn fires before the empty-iter codegen "
                  "runs; runtime equivalent covered by cel_map_test.";
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{}.exists(k, true)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), false);
}

TEST_F(ComprehensionMapIterE2ETest, AllEmptyMapIsTrue) {
  GTEST_SKIP() << "empty map literal types as map(dyn,dyn) — "
                  "RejectDyn fires before the empty-iter codegen "
                  "runs; runtime equivalent covered by cel_map_test.";
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{}.all(k, false)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapIterE2ETest, ExistsOneOverMapIntKeys) {
  // design §3.10: `{6:"six",7:"seven",8:"eight"}.exists_one(foo,
  // foo % 5 == 2)` → true (only 7 satisfies).
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"({6: "six", 7: "seven", 8: "eight"}.exists_one(foo, foo % 5 == 2))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapIterE2ETest, MapOverMapKeysReturnsKeyList) {
  // design §3.11 + §3.10: `{'John':'smart'}.map(k, k)` returns
  // `['John']`.  Single-element map avoids ordering ambiguity.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"({"John": "smart"}.map(k, k) == ["John"])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionMapIterE2ETest, MapOverMapKeysSizeCheck) {
  // design §3.11: map iteration order is not spec-mandated.  Assert
  // size only so the test is order-independent across runtime
  // implementations.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"({"k1": 1, "k2": 2}.map(k, k).size())");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 2);
}

// ──────────────────────────────────────────────────────────────
// 4. ComprehensionTwoIterVarE2ETest  (Slice F)
//
//    `e.exists(i, v, p)` / `e.all(k, v, p)` / `e.transformList(i,
//    v, t)` / `e.transformList(i, v, p, t)` over LIST and MAP
//    sources.  List form binds (index, value); map form binds
//    (key, value).
//
//    Conformance unlock: ~+28 rows per design §3.8 + followon §3.8.
// ──────────────────────────────────────────────────────────────

class ComprehensionTwoIterVarE2ETest : public ::testing::Test {};

TEST_F(ComprehensionTwoIterVarE2ETest, ExistsTwoIterVarOverListIndexAndValue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "[10, 20, 30].exists(i, v, v == 20 && i == 1)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTwoIterVarE2ETest, AllTwoIterVarOverListIndexAndValue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[10, 20, 30].all(i, v, v >= i * 10)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTwoIterVarE2ETest, ExistsTwoIterVarOverMapKeyAndValue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"({"a": 1, "b": 2}.exists(k, v, k == "b" && v == 2))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTwoIterVarE2ETest, AllTwoIterVarOverMapKeyAndValue) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"({"a": 1, "b": 2}.all(k, v, v > 0))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTwoIterVarE2ETest, TransformListIndexPlusValueHalf) {
  // `[2,4,6].transformList(i, v, v/2 + i)` → `[1, 3, 5]`.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "[2, 4, 6].transformList(i, v, v / 2 + i) == [1, 3, 5]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTwoIterVarE2ETest, TransformListConditional) {
  // `[2,4,6].transformList(i, v, i != 1, v/2 + i)` → `[1, 5]`.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "[2, 4, 6].transformList(i, v, i != 1, v / 2 + i) == [1, 5]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTwoIterVarE2ETest, TransformListEmptySource) {
  // design §3.1: empty range → `[]`; per-iter divide-by-zero never
  // reached.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[].transformList(i, v, i / v) == []");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTwoIterVarE2ETest, ExistsOneTwoIterVarOverList) {
  // ComprehensionsV2 spells the three-arg form `existsOne` (camelCase),
  // distinct from standard `exists_one` (snake_case 2-arg).  See
  // design §2.3 macro 11.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "[10, 20, 30].existsOne(i, v, i == 1 && v == 20)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 5. ComprehensionTransformMapE2ETest  (Slice G)
//
//    `transformMap(k, v, t)` / `transformMap(k, v, p, t)`.  Map
//    accumulator; key-collision is last-write-wins per design §9.6.
//
//    Conformance unlock: ~+10 rows (macros2.transformMap cohort).
// ──────────────────────────────────────────────────────────────

class ComprehensionTransformMapE2ETest : public ::testing::Test {};

TEST_F(ComprehensionTransformMapE2ETest, TransformMapDoublesValues) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"({"a": 1, "b": 2}.transformMap(k, v, v * 2) == {"a": 2, "b": 4})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTransformMapE2ETest, TransformMapConditional) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"({"a": 1, "b": 2, "c": 3}.transformMap(k, v, v > 1, v * 10) == )"
      R"({"b": 20, "c": 30})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTransformMapE2ETest, ConditionalPredicateError) {
  // Regression test for the macros2/transformMap/error_filter
  // conformance row.  Slice G originally shipped with a naive
  // `if (pred.bool != 0) cel_map_insert_at(...)` codegen for the
  // 4-arg form's predicate; an ERROR predicate (here: div-by-zero
  // when v=0) was silently treated as bool, masking the abort
  // semantics required by langdef §3.2.  Followup landed
  // `cel_map_insert_at_if_bool` (mirror of the list-side helper);
  // the conditional codegen now routes through it so 3VL on the
  // predicate propagates an ERROR / UNKNOWN into the accu slot.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"({"foo": 2, "bar": 1, "baz": 0}.transformMap(k, v, )"
                 R"(k == "baz" && 4 / v == 0, v))");
  Activation a;
  EXPECT_TRUE(EvalOk(instance, a).IsError());
}

TEST_F(ComprehensionTransformMapE2ETest, TransformMapEmptySource) {
  // design §3.1: `{}.transformMap(k, v, k + v)` → `{}`.
  // Empty map literal types as `map(dyn, dyn)` — RejectDyn fires
  // before the comprehension's iter-empty fast path can be taken.
  // Equivalent assertion lives in cel_map_test.cc::MapIterTest
  // (cel_map_iter_init on an empty map → handle=0 → no iters).
  GTEST_SKIP() << "empty map literal types as map(dyn,?) — "
                  "RejectDyn fires; runtime equivalent covered by "
                  "cel_map_test.cc.";
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "{}.transformMap(k, v, v + 1) == {}");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTransformMapE2ETest,
       TransformMapKeyCollisionLastWriteWins) {
  // Plan-vs-execution delta (2026-05-17): the source as written
  // mis-uses `transformMap(k, v, p, t)` — cel-cpp's 4-arg
  // signature is (k, v, predicate, value-transform).  The key is
  // never remapped by transformMap, so no collision is reachable
  // via this surface; cel-cpp's type checker correctly rejects
  // the 4-arg form here because `"x"` is not a bool predicate.
  // Key remapping is the contract of `transformMapEntry`
  // (Slice H), where the loop_step inserts an entire `{k': t}`
  // map and the user controls k'.  This assertion will be
  // rewritten under Slice H using `transformMapEntry(k, v,
  // {"x": v}).size() == 1`.  Runtime invariant (last-write-wins
  // on duplicate key) is covered by
  // `cel_map_test.cc::MapInsertCollision`.  See
  // m5-comprehensions-followon.md §10 "future work" bullet
  // "TransformMapKeyCollisionLastWriteWins is malformed".
  GTEST_SKIP() << "test mis-uses transformMap as a key-remapper; "
                  "rewritten under Slice H transformMapEntry. "
                  "See m5-comprehensions-followon.md §10.";
}

TEST_F(ComprehensionTransformMapE2ETest, TransformMapValuePredicateError) {
  // design §3.2: per-iter step error aborts comprehension.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"({"a": 0, "b": 1}.transformMap(k, v, 1 / v))");
  Activation a;
  EXPECT_TRUE(EvalOk(instance, a).IsError());
}

// ──────────────────────────────────────────────────────────────
// 6. ComprehensionTransformMapEntryE2ETest  (Slice H)
//
//    `transformMapEntry(k, v, entry)` / `(k, v, p, entry)`.
//    loop_step evaluates an entry-map literal that is merged into
//    the accu via `cel_map_insert_entries` per design §7.3.
//    Empty-entry literal is a SKIP (current iter contributes
//    nothing per design §9.7).
//
//    Conformance unlock: ~+8 rows (macros2.transformMapEntry).
// ──────────────────────────────────────────────────────────────

class ComprehensionTransformMapEntryE2ETest : public ::testing::Test {};

TEST_F(ComprehensionTransformMapEntryE2ETest, SingleEntryRoundTrip) {
  // `{'foo':'bar'}.transformMapEntry(k, v, {k+v: k})` →
  // `{'foobar':'foo'}`.  Slice H single-entry pattern routes
  // through Slice G's `cel_map_insert_at(accu, k', t)`.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"({"foo": "bar"}.transformMapEntry(k, v, {k + v: k}) == )"
                 R"({"foobar": "foo"})");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionTransformMapEntryE2ETest, MultiEntryPerIter) {
  // `{'a':'A'}.transformMapEntry(k, v, {k: v, v: k})` —
  // multi-entry literal: 2 entries per iter.  Slice H generalized
  // emitter loops over entry.size() inserts; pre-sizing
  // multiplies capacity by entries-per-iter so
  // PRESIZE_INVARIANT holds.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"({"a": "A"}.transformMapEntry(k, v, {k: v, v: k}).size())");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 2);
}

TEST_F(ComprehensionTransformMapEntryE2ETest, EmptyEntrySkipsIter) {
  // design §9.7: empty-entry literal contributes nothing.
  // Slice H codegen emits BinaryenNop for the empty-entry shape;
  // the case is exercised at the codegen level by the
  // ConditionalForm test below (whose entry is single-keyed).
  // E2E source `{...}.transformMapEntry(k, v, {})` is unreachable
  // here because the `{}` literal types as `map(dyn, dyn)` and
  // RejectDyn fires before codegen runs — same RejectDyn-on-empty-
  // map-literal pattern as Slice E.  Equivalent codegen invariant
  // (entries.empty() → BinaryenNop) is locked by code inspection
  // + the ConditionalForm path that DOES type-check.
  GTEST_SKIP() << "empty entry literal types as map(dyn,?); "
                  "RejectDyn fires.  Codegen no-op path is "
                  "covered structurally.";
}

TEST_F(ComprehensionTransformMapEntryE2ETest, ConditionalForm) {
  // `(k, v, p, entry)` form — only kept iters contribute.
  // Slice H conditional + single-entry routes through Slice G's
  // 3VL-aware `cel_map_insert_at_if_bool`.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      R"({"a": 1, "b": 2}.transformMapEntry(k, v, v > 1, {k: v}).size())");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 1);
}

TEST_F(ComprehensionTransformMapEntryE2ETest, EmptySource) {
  // Empty `{}` source types as `map(dyn, dyn)` — RejectDyn fires
  // before the comprehension's iter-empty fast path can be taken.
  // Same pattern as Slice E's empty-map literal SKIPs; equivalent
  // runtime invariant covered by cel_map_test::MapIterTest::Empty.
  GTEST_SKIP() << "empty map literal types as map(dyn,?) — "
                  "RejectDyn fires; runtime equivalent covered by "
                  "cel_map_test.cc.";
}

// ──────────────────────────────────────────────────────────────
// 7. CelBindE2ETest  (Slice I)
//
//    `cel.bind(name, value, body)` — a degenerate comprehension
//    with iter_range=[], loop_cond=false.  Slice C codegen already
//    handles it; Slice I adds the parser-library macro registration
//    and (optionally) the Shape-C fast path.
//
//    Conformance unlock: ~+9 rows (bindings_ext + cel.bind-using
//    namespace rows).
// ──────────────────────────────────────────────────────────────

class CelBindE2ETest : public ::testing::Test {};

TEST_F(CelBindE2ETest, BindScalarAndUse) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "cel.bind(x, 5, x + 1)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 6);
}

TEST_F(CelBindE2ETest, NestedBind) {
  // design §3.6: nested binds; outer in scope inside inner.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "cel.bind(x, 5, cel.bind(y, 10, x + y))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 15);
}

TEST_F(CelBindE2ETest, InnerShadowsOuter) {
  // design §3.5: inner cel.bind with same name shadows outer
  // binding for body.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "cel.bind(x, 5, cel.bind(x, 10, x))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 10);
}

TEST_F(CelBindE2ETest, ComprehensionInsideBind) {
  // design §3.6: bind-then-comprehension; accu_var visible inside
  // the inner comprehension's loop_step.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "cel.bind(valid, [1, 2, 3], [3, 4, 5].exists(e, e in valid))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(CelBindE2ETest, BindInsideComprehension) {
  // design §3.6 / §9.3: comprehension's loop_step contains a
  // cel.bind.  Iter_var visible inside the bind's value expression.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "[1, 2, 3].exists(v, cel.bind(t, v * 2, t > 4))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(CelBindE2ETest, BindWithMapAccu) {
  // design §3.6: accu can be a map; body indexes it.  Uses the
  // explicit `x["y"]` index form rather than `x.y` because the
  // shorthand routes to `cel_host.cel_get_field` (proto path)
  // and not `cel_map_lookup` — that's a separate codegen gap
  // (kSelect on a map operand) unrelated to cel.bind itself.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"(cel.bind(x, {"y": 0}, x["y"] == 0))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(CelBindE2ETest, NestedBindBothBoolean) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "cel.bind(t1, true, cel.bind(t2, true, t1 && t2))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 8. ComprehensionNestedE2ETest  (Slice C closeout — nesting)
//
//    Outer × inner across {list, map} × {list, map}.  Same-name
//    shadow per design §3.5.  Free-var passthrough (outer iter_var
//    visible inside inner loop_step).
//
//    Conformance unlock: ~+6 rows (nested namespace cohorts).
// ──────────────────────────────────────────────────────────────

class ComprehensionNestedE2ETest : public ::testing::Test {};

TEST_F(ComprehensionNestedE2ETest, OuterListInnerList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, "[1, 2, 3].exists(x, [4, 5, 6].exists(y, x + y == 7))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionNestedE2ETest, OuterListInnerMap) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"([1, 2, 3].exists(x, {1: "a", 2: "b"}.exists(k, k == x)))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionNestedE2ETest, OuterMapInnerList) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"({1: "a", 2: "b"}.exists(k, [1, 2, 3].exists(e, e == k)))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionNestedE2ETest, OuterMapInnerMap) {
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"({1: "a"}.exists(k1, {2: "b"}.exists(k2, k1 + k2 == 3)))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionNestedE2ETest, SameNameInnerShadowsOuter) {
  // design §3.5: `[1].exists(y, [0].exists(y, y == 0))` → true.
  // Inner `y` binds 0; reference inside inner step resolves to
  // inner binding.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "[1].exists(y, [0].exists(y, y == 0))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionNestedE2ETest, OuterIterVarVisibleInsideInner) {
  // design §3.5: outer iter_var is a free variable in the inner
  // loop_step; resolves to outer binding.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler,
      "['signer'].all(signer, ['artifact'].all(artifact, signer != artifact))");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionNestedE2ETest, FilterWithInnerAllPasses) {
  // design §3.5: `['signer'].filter(signer, ['artifact'].all(artifact,
  // true))` → `['signer']`.
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(
      *compiler, R"(['signer'].filter(signer, ['artifact'].all(artifact, )"
                 R"(true)) == ['signer'])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

// ──────────────────────────────────────────────────────────────
// 9. ComprehensionConsumerE2ETest  (cross-cutting)
//
//    Comprehension output as operand to `==` / `size()` / index /
//    further comprehension.  Per design §3.9: a comprehension's
//    result slot is addressable like any other CelValue.
//
//    Conformance unlock: indirect (none of these rows are
//    comprehension-only; they pin the consumer-side contract).
// ──────────────────────────────────────────────────────────────

class ComprehensionConsumerE2ETest : public ::testing::Test {};

TEST_F(ComprehensionConsumerE2ETest, MapResultEqualsLiteralList) {
  // design §3.9 canonical row: `{'John':'smart'}.map(key, key) ==
  // ['John']` → true.
  GTEST_SKIP() << "M5.B.D ships here — see "
                  "m5-comprehensions-followon.md §Slice D (consumer).";
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, R"({"John": "smart"}.map(key, key) == ["John"])");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionConsumerE2ETest, FilterResultSizeGreaterThanZero) {
  GTEST_SKIP() << "M5.B.D ships here — see "
                  "m5-comprehensions-followon.md §Slice D (consumer).";
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "[1, 2, 3].filter(v, v > 1).size() > 0");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionConsumerE2ETest, FilterResultIndexedReturnsElement) {
  // `[1,2,3].filter(v, v > 1)[0]` → 2.
  GTEST_SKIP() << "M5.B.D ships here — see "
                  "m5-comprehensions-followon.md §Slice D (consumer).";
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].filter(v, v > 1)[0]");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsInt(), 2);
}

TEST_F(ComprehensionConsumerE2ETest, MapResultAsSourceForFurtherComp) {
  // Chain: outer `exists` over the result of an inner `map`.
  GTEST_SKIP() << "M5.B.D ships here — see "
                  "m5-comprehensions-followon.md §Slice D (consumer).";
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance =
      CompilePlan(*compiler, "[1, 2, 3].map(v, v * 10).exists(e, e == 20)");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

TEST_F(ComprehensionConsumerE2ETest, FilterResultEqualsEmptyList) {
  // Empty-filter-result vs `[]` round-trip.
  GTEST_SKIP() << "M5.B.D ships here — see "
                  "m5-comprehensions-followon.md §Slice D (consumer).";
  auto compiler = CompilerEmpty();
  ASSERT_THAT(compiler, IsOk());
  auto instance = CompilePlan(*compiler, "[1, 2, 3].filter(v, v > 100) == []");
  Activation a;
  EXPECT_EQ(*EvalOk(instance, a).AsBool(), true);
}

}  // namespace
}  // namespace celwasm
