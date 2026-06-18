// Compilation-limit e2e tests.
//
// These pin the *exact boundaries* of what the compiler accepts — for
// each fixed budget, the largest expression that still compiles and the
// smallest one that does not.  A limit is a contract a caller can hit,
// so the boundary is asserted as an executable spec, not folklore: when
// a limit moves (e.g. the rodata window is raised — see
// `doc/implementation-plan/rewrite/m31-static-aggregates.md` §10) the
// matching pair here moves with it in the same commit.
//
// Two distinct limits, reached by two distinct expression *shapes*:
//
//   • Rodata window  — BREADTH.  Every scalar literal is a 24-byte
//     CelValue frame in the window `[16, --global-base=262144)`;
//     identical literals are not deduped.  A flat list of N int
//     literals costs `16 + 24*N + 40` rodata (header + run + outer
//     frame); with the 256 B guard the boundary is
//     `16 + 24*N + 40 + 256 ≤ 262144` ⇒ N ≤ 10909 fit, 10910 overflow.
//
//   • Parse nesting depth — DEPTH.  An expression whose AST nests deeper
//     than `kMaxExpressionNestingDepth` (2048) is rejected at parse,
//     independent of rodata.  Reached only when operands cost no rodata
//     (variables); a literal chain hits the 10909-frame wall first.

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/instance.h"
#include "eval/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// `[1, 2, …, n]` — one flat list literal with `n` element frames.
// Breadth, not depth: stresses the rodata window, not parse recursion.
std::string IntListLiteral(int n) {
  std::string s = "[1";
  for (int i = 2; i <= n; ++i) absl::StrAppend(&s, ", ", i);
  absl::StrAppend(&s, "]");
  return s;
}

// `a + a + … + a` with `n` terms — an `n`-deep nested binary tree whose
// operands are a bound variable (zero rodata), so depth is what binds.
std::string VarAddChain(int n) {
  std::string s = "a";
  for (int i = 1; i < n; ++i) absl::StrAppend(&s, " + a");
  return s;
}

class LimitsTest : public ::testing::Test {
 protected:
  // Compile a pure-literal expression (no declared variables).
  absl::StatusOr<Program> Compile(absl::string_view source) {
    Compiler::Builder b;
    auto compiler = std::move(b).Build();
    if (!compiler.ok()) return compiler.status();
    return compiler->Compile(source, e2e::DefaultOpts());
  }

  // Compile with a single `int` variable `a` declared.
  absl::StatusOr<Program> CompileWithVarA(absl::string_view source) {
    Compiler::Builder b;
    b.DeclareVariable("a", CelType::Int());
    auto compiler = std::move(b).Build();
    if (!compiler.ok()) return compiler.status();
    return compiler->Compile(source, e2e::DefaultOpts());
  }

  // Compile + Plan + Eval a literal expression to its int result.
  absl::StatusOr<int64_t> EvalInt(absl::string_view source) {
    auto program = Compile(source);
    if (!program.ok()) return program.status();
    auto instance = e2e::GlobalEngine().Plan(*program);
    if (!instance.ok()) return instance.status();
    auto v = instance->Eval();
    if (!v.ok()) return v.status();
    return *v->AsInt();
  }
};

// End-to-end sanity floor: a trivial literal expression compiles and runs.
TEST_F(LimitsTest, SmallLiteralExprCompilesAndEvaluates) {
  EXPECT_THAT(EvalInt("10 + 1"), IsOkAndHolds(11));
}

// ── Rodata window: exact boundary at 10909 / 10910 literal frames ────
//
// A const list literal materializes into rodata (m31): the window is
// `[16, --global-base=262144)` minus a 256 B guard, and the list costs
// `24*N + 40` bytes (N×24-byte element run + 16-byte header + 24-byte
// outer frame).  Boundary: `16 + 24*N + 40 + 256 ≤ 262144` ⇒ N ≤ 10909.
// If this pair flips, --global-base or the per-frame size changed —
// recompute and move BOTH cases here.

// Well inside the window: a small list compiles and evaluates.
TEST_F(LimitsTest, RodataWindow_SmallListCompiles) {
  EXPECT_THAT(EvalInt(absl::StrCat("size(", IntListLiteral(10), ")")),
              IsOkAndHolds(10));
}

// 10K-element const list (~240 KiB rodata) now fits the 256 KiB window —
// previously impossible under the 8 KiB window (m31 §10 raise).
TEST_F(LimitsTest, RodataWindow_TenThousandFlatLiteralsCompile) {
  EXPECT_THAT(Compile(IntListLiteral(10000)), IsOk());
}

TEST_F(LimitsTest, RodataWindow_10909FlatLiteralsCompile) {
  EXPECT_THAT(Compile(IntListLiteral(10909)), IsOk());
}

TEST_F(LimitsTest, RodataWindow_10910FlatLiteralsExceedWindow) {
  auto program = Compile(IntListLiteral(10910));
  EXPECT_THAT(program, StatusIs(absl::StatusCode::kResourceExhausted));
  EXPECT_THAT(program.status().message(), HasSubstr("rodata"));
}

// ── Parse nesting depth: exact boundary at kMaxExpressionNestingDepth ──
//
// 2048-deep compiles; 2049-deep is rejected at parse.  Variable operands
// keep rodata out of the way so depth is the binding constraint.

TEST_F(LimitsTest, ParseNesting_2048DeepCompiles) {
  EXPECT_THAT(CompileWithVarA(VarAddChain(2048)), IsOk());
}

TEST_F(LimitsTest, ParseNesting_2049DeepExceedsMax) {
  auto program = CompileWithVarA(VarAddChain(2049));
  EXPECT_THAT(program, StatusIs(absl::StatusCode::kResourceExhausted));
  EXPECT_THAT(program.status().message(), HasSubstr("nesting depth"));
}

}  // namespace
}  // namespace celwasm
