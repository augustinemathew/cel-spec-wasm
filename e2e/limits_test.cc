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
//     CelValue frame in the window `[16, --global-base=8192)`; identical
//     literals are not deduped.  Usable budget = 8192 − 16 reserved −
//     256 guard = 7920 B → 7920 / 24 = 330 frames, minus the list
//     header + outer frame ⇒ 328 literals fit, 329 overflow.
//
//   • Parse nesting depth — DEPTH.  An expression whose AST nests deeper
//     than `kMaxExpressionNestingDepth` (2048) is rejected at parse,
//     independent of rodata.  Reached only when operands cost no rodata
//     (variables); a literal chain hits the 328-frame wall first.

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

// ── Rodata window: exact boundary at 328 / 329 literal frames ─────
//
// If this pair flips, the window or the per-frame size changed —
// recompute as (--global-base − 16 − 256) / 24 minus list overhead and
// move BOTH cases here (m31 §10 raises --global-base to 256 KiB).

// Well inside the window: a small list compiles and evaluates.
TEST_F(LimitsTest, RodataWindow_SmallListCompiles) {
  EXPECT_THAT(EvalInt(absl::StrCat("size(", IntListLiteral(10), ")")),
              IsOkAndHolds(10));
}

TEST_F(LimitsTest, RodataWindow_328FlatLiteralsCompile) {
  EXPECT_THAT(Compile(IntListLiteral(328)), IsOk());
}

TEST_F(LimitsTest, RodataWindow_329FlatLiteralsExceedWindow) {
  auto program = Compile(IntListLiteral(329));
  EXPECT_THAT(program, StatusIs(absl::StatusCode::kResourceExhausted));
  EXPECT_THAT(program.status().message(), HasSubstr("rodata"));
}

// Far past the window (10K frames ≈ 240 KiB vs the 8 KiB budget): still
// a clean, loud rejection — no silent truncation at scale.
TEST_F(LimitsTest, RodataWindow_10KFlatLiteralsExceedWindow) {
  auto program = Compile(IntListLiteral(10000));
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
