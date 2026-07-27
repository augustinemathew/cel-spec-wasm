// M16 e2e acceptance spec — the `math` extension functions
// (cel-cpp's `extensions/math_ext*`) end-to-end through
// Compiler::Compile → Engine::Plan → Instance::Eval.  Source
// expressions and expected results mirror conformance rows from
// `tests/simple/testdata/math_ext.textproto` (199 rows, 17
// functions) so a regression in any family surfaces before the
// conformance run.
//
// MANUAL-TAGGED until the pipeline is wired.  This file COMPILES
// today, but its CEL expressions do NOT yet evaluate: the runtime
// kernels (bitwise / min-max), the `MathCheckerLibrary()` checker
// registration, the overload-table seeds, and the wasm exports are
// being completed in parallel (M16 Slices B/C/D).  Until those land
// the checker rejects every `math.*` reference, so the target is
// `tags = ["manual"]` in BUILD to keep it out of
// the project-package test sweep / CI.  When Slice D wires the
// pipeline, remove the `tags = ["manual"]` line from the `math_ext_test`
// target in `e2e/BUILD.bazel` and this suite becomes a
// live acceptance gate.
//
// Sliced by function family per `rewrite/m16-math-ext.md` §5.2:
//
//   - MinMaxE2ETest          min/max binary + cross-type (the @min /
//                            @max binary overload family)
//   - ListE2ETest            math.greatest/least over list literals
//   - MacroExpansionE2ETest  every macro arg-shape (unary scalar,
//                            list literal, 2-arg, 3+-arg collapse,
//                            mixed-type)
//   - ScalarE2ETest          ceil / floor / round / trunc / abs /
//                            sign / sqrt
//   - PredicateE2ETest       isNaN / isInf / isFinite
//   - BitwiseE2ETest         bitAnd / bitOr / bitXor / bitNot /
//                            bitShiftLeft / bitShiftRight
//   - ErrorE2ETest           spec-pinned runtime-error rows
//                            (abs(INT64_MIN) overflow, negative shift)
//
// The corpus is entirely literal-driven — no proto-message inputs and
// no wrapper-type (google.protobuf.Int32Value/…) arguments appear in
// any `math_ext` row, so this suite needs no proto fixtures or bound
// activations (mirrors the m12 string_ext harness exactly).  See the
// closing report for the (empty) wrapper-coverage note.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "eval/activation.h"
#include "compiler/compiler.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "compiler/program.h"
#include "eval/value.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

using ::celwasm::e2e::GlobalEngine;

Compiler MathCompiler() {
  Compiler::Builder b;
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler);
  return *std::move(compiler);
}

using ::celwasm::e2e::CompilePlan;

Value EvalOk(absl::string_view source) {
  auto compiler = MathCompiler();
  auto instance = CompilePlan(compiler, source);
  Activation a;
  auto v = instance.Eval(a);
  ABSL_CHECK_OK(v) << source;
  return *std::move(v);
}

bool EvalBool(absl::string_view source) {
  Value v = EvalOk(source);
  ABSL_CHECK(v.kind() == Value::Kind::kBool)
      << source << " kind=" << static_cast<int>(v.kind());
  return *v.AsBool();
}

int64_t EvalInt(absl::string_view source) {
  Value v = EvalOk(source);
  ABSL_CHECK(v.kind() == Value::Kind::kInt)
      << source << " kind=" << static_cast<int>(v.kind());
  return *v.AsInt();
}

uint64_t EvalUint(absl::string_view source) {
  Value v = EvalOk(source);
  ABSL_CHECK(v.kind() == Value::Kind::kUint)
      << source << " kind=" << static_cast<int>(v.kind());
  return *v.AsUint();
}

double EvalDouble(absl::string_view source) {
  Value v = EvalOk(source);
  ABSL_CHECK(v.kind() == Value::Kind::kDouble)
      << source << " kind=" << static_cast<int>(v.kind());
  return *v.AsDouble();
}

// Spec-pinned runtime-error rows: the expression type-checks but
// evaluates to a CEL error Value (overflow / negative shift offset).
void ExpectEvalError(absl::string_view source, absl::string_view why) {
  auto compiler = MathCompiler();
  auto instance = CompilePlan(compiler, source);
  Activation a;
  auto v = instance.Eval(a);
  ASSERT_THAT(v, IsOk()) << source;
  EXPECT_TRUE(v->IsError())
      << "expected `" << source << "` to surface a CEL error Value (" << why
      << "); got kind=" << static_cast<int>(v->kind());
}

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kInt64Min = std::numeric_limits<int64_t>::min();
constexpr uint64_t kUint64Max = std::numeric_limits<uint64_t>::max();

// ──────────────────────────────────────────────────────────────
// MinMaxE2ETest — the math.@max / math.@min binary overload family
// (post-macro).  Same-type (int / uint / double) + every cross-type
// pair, with boundary operands.  Mirrors the binary_* rows of the
// greatest_*/least_* corpus sections.
// ──────────────────────────────────────────────────────────────

class MinMaxE2ETest : public ::testing::Test {};

TEST_F(MinMaxE2ETest, GreatestBinaryIntSameType) {
  EXPECT_EQ(EvalInt("math.greatest(1, 1)"), 1);
  EXPECT_EQ(EvalInt("math.greatest(3, -3)"), 3);
  EXPECT_EQ(EvalInt("math.greatest(-7, 5)"), 5);
}

TEST_F(MinMaxE2ETest, LeastBinaryIntSameType) {
  EXPECT_EQ(EvalInt("math.least(1, 1)"), 1);
  EXPECT_EQ(EvalInt("math.least(-3, 3)"), -3);
  EXPECT_EQ(EvalInt("math.least(5, -7)"), -7);
}

TEST_F(MinMaxE2ETest, GreatestBinaryUintSameType) {
  EXPECT_EQ(EvalUint("math.greatest(1u, 1u)"), 1u);
  EXPECT_EQ(EvalUint("math.greatest(10u, 3u)"), 10u);
}

TEST_F(MinMaxE2ETest, LeastBinaryUintSameType) {
  EXPECT_EQ(EvalUint("math.least(1u, 1u)"), 1u);
  EXPECT_EQ(EvalUint("math.least(5u, 2u)"), 2u);
}

TEST_F(MinMaxE2ETest, GreatestBinaryDoubleSameType) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.greatest(1.0, 1.0)"), 1.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.greatest(5.0, -7.0)"), 5.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.greatest(-3.0, 3.0)"), 3.0);
}

TEST_F(MinMaxE2ETest, LeastBinaryDoubleSameType) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.least(1.5, 1.5)"), 1.5);
  EXPECT_DOUBLE_EQ(EvalDouble("math.least(-3.5, 3.5)"), -3.5);
  EXPECT_DOUBLE_EQ(EvalDouble("math.least(5.5, -7.5)"), -7.5);
}

// Cross-type pairwise overloads resolve to a single id but with a
// `dyn` result type (probe surprise #2); the winning operand keeps
// its own runtime kind, hence the `== <typed-literal>` comparisons
// the corpus uses to pin the result kind.
TEST_F(MinMaxE2ETest, GreatestCrossTypeIntDouble) {
  EXPECT_TRUE(EvalBool("math.greatest(1, 1.0) == 1"));
  EXPECT_TRUE(EvalBool("math.greatest(1.0, 1) == 1.0"));
}

TEST_F(MinMaxE2ETest, GreatestCrossTypeIntUint) {
  EXPECT_TRUE(EvalBool("math.greatest(1, 1u) == 1"));
  EXPECT_TRUE(EvalBool("math.greatest(1u, 1) == 1u"));
  EXPECT_EQ(EvalUint("math.greatest(5u, -7)"), 5u);
  EXPECT_EQ(EvalUint("math.greatest(-3, 3u)"), 3u);
}

TEST_F(MinMaxE2ETest, GreatestCrossTypeUintDouble) {
  EXPECT_TRUE(EvalBool("math.greatest(1u, 1.0) == 1"));
  EXPECT_TRUE(EvalBool("math.greatest(1.0, 1u) == 1.0"));
}

TEST_F(MinMaxE2ETest, LeastCrossTypePairs) {
  EXPECT_TRUE(EvalBool("math.least(1, 1.0) == 1"));
  EXPECT_TRUE(EvalBool("math.least(1, 1u) == 1"));
  EXPECT_TRUE(EvalBool("math.least(1u, 1.0) == 1u"));
  EXPECT_TRUE(EvalBool("math.least(1u, 1) == 1u"));
}

// Boundary operands: INT64_MAX/MIN, UINT64_MAX, ±DBL_MAX.
TEST_F(MinMaxE2ETest, GreatestIntBoundary) {
  EXPECT_EQ(EvalInt("math.greatest(9223372036854775807, 1)"), kInt64Max);
  EXPECT_EQ(EvalInt("math.greatest(-9223372036854775808, 1)"), 1);
}

TEST_F(MinMaxE2ETest, LeastIntBoundary) {
  EXPECT_EQ(EvalInt("math.least(9223372036854775807, 1)"), 1);
  EXPECT_EQ(EvalInt("math.least(-9223372036854775808, 1)"), kInt64Min);
}

TEST_F(MinMaxE2ETest, MinMaxUintBoundary) {
  EXPECT_EQ(EvalUint("math.greatest(18446744073709551615u, 1u)"), kUint64Max);
  EXPECT_EQ(EvalUint("math.least(18446744073709551615u, 1u)"), 1u);
}

TEST_F(MinMaxE2ETest, MinMaxDoubleBoundary) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.greatest(1.797693e308, 1.5)"),
                   1.797693e308);
  EXPECT_DOUBLE_EQ(EvalDouble("math.least(-1.797693e308, 1.5)"), -1.797693e308);
}

// ──────────────────────────────────────────────────────────────
// ListE2ETest — math.greatest/least over an explicit list literal
// (the unary-list overload family).  Homogeneous int/uint/double
// lists plus mixed-numeric lists (left UNRESOLVED by the checker;
// runtime-kind dispatched — probe surprise #1).
// ──────────────────────────────────────────────────────────────

class ListE2ETest : public ::testing::Test {};

TEST_F(ListE2ETest, GreatestIntList) {
  EXPECT_TRUE(EvalBool("math.greatest([1, 2, 3]) == 3"));
}

TEST_F(ListE2ETest, LeastIntList) {
  EXPECT_TRUE(EvalBool("math.least([1, 2, 3]) == 1"));
}

TEST_F(ListE2ETest, GreatestUintList) {
  EXPECT_TRUE(EvalBool("math.greatest([1u, 2u, 3u]) == 3u"));
}

TEST_F(ListE2ETest, GreatestDoubleList) {
  EXPECT_TRUE(EvalBool("math.greatest([1.0, 2.0]) == 2.0"));
}

TEST_F(ListE2ETest, GreatestMixedListIntResult) {
  EXPECT_TRUE(EvalBool("math.greatest([5.4, 10, 3u, -5.0, 3.5]) == 10"));
}

TEST_F(ListE2ETest, GreatestMixedListDoubleResult) {
  EXPECT_TRUE(EvalBool("math.greatest([5.4, 10.5, 3u, -5.0, 3.5]) == 10.5"));
}

TEST_F(ListE2ETest, GreatestMixedListUintResult) {
  EXPECT_TRUE(EvalBool("math.greatest([5.4, 10u, 3u, -5.0, 3.5]) == 10u"));
}

TEST_F(ListE2ETest, LeastMixedListResult) {
  EXPECT_TRUE(EvalBool("math.least([5.4, 10, 3u, -5.0, 3.5]) == -5.0"));
  EXPECT_TRUE(EvalBool("math.least([5.4, 10u, 3u, 1u, 3.5]) == 1u"));
}

// ──────────────────────────────────────────────────────────────
// MacroExpansionE2ETest — every arg-shape the greatest/least macros
// rewrite (per m16-ast-probe-findings.md): unary scalar → unary
// overload; 2 args → pairwise; 3+ args → collapsed to a single list
// literal; explicit list literal stays a list arg.  These exercise
// the macro rewrite path itself, not just the kernels.
// ──────────────────────────────────────────────────────────────

class MacroExpansionE2ETest : public ::testing::Test {};

TEST_F(MacroExpansionE2ETest, UnaryScalarInt) {
  // math.greatest(5) → math.@max(5) → math_@max_int (identity).
  EXPECT_EQ(EvalInt("math.greatest(5)"), 5);
  EXPECT_EQ(EvalInt("math.greatest(-5)"), -5);
  EXPECT_EQ(EvalInt("math.least(-5)"), -5);
}

TEST_F(MacroExpansionE2ETest, UnaryScalarUint) {
  EXPECT_EQ(EvalUint("math.greatest(5u)"), 5u);
  EXPECT_EQ(EvalUint("math.least(5u)"), 5u);
}

TEST_F(MacroExpansionE2ETest, UnaryScalarDouble) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.greatest(-5.0)"), -5.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.least(-5.5)"), -5.5);
}

TEST_F(MacroExpansionE2ETest, TernaryCollapsedToListInt) {
  // 3+ args → macro collapses into a single list literal arg.
  EXPECT_TRUE(EvalBool("math.greatest(10, 1, 3) == 10"));
  EXPECT_TRUE(EvalBool("math.greatest(1, 3, 10) == 10"));
  EXPECT_TRUE(EvalBool("math.greatest(-1, -2, -3) == -1"));
  EXPECT_TRUE(EvalBool("math.least(0, 1, 3) == 0"));
  EXPECT_TRUE(EvalBool("math.least(-1, -2, -3) == -3"));
}

TEST_F(MacroExpansionE2ETest, TernaryCollapsedToListUint) {
  EXPECT_TRUE(EvalBool("math.greatest(10u, 1u, 3u) == 10u"));
  EXPECT_TRUE(EvalBool("math.least(10u, 3u, 1u) == 1u"));
}

TEST_F(MacroExpansionE2ETest, TernaryCollapsedToListDouble) {
  EXPECT_TRUE(EvalBool("math.greatest(10.5, 1.5, 3.5) == 10.5"));
  EXPECT_TRUE(EvalBool("math.least(0.5, 1.5, 3.5) == 0.5"));
}

TEST_F(MacroExpansionE2ETest, TernaryMixedTypeCollapse) {
  EXPECT_TRUE(EvalBool("math.greatest(1, 1.0, 1.0) == 1"));
  EXPECT_TRUE(EvalBool("math.greatest(1, 1u, 1u) == 1"));
  EXPECT_TRUE(EvalBool("math.least(1, 1.0, 1.0) == 1"));
}

TEST_F(MacroExpansionE2ETest, QuaternaryMixedCollapse) {
  EXPECT_TRUE(
      EvalBool("math.greatest(5.4, 10, 3u, -5.0, 9223372036854775807) == "
               "9223372036854775807"));
  EXPECT_TRUE(
      EvalBool("math.least(5.4, 10, 3u, -5.0, 9223372036854775807) == -5.0"));
  EXPECT_TRUE(
      EvalBool("math.greatest(5.4, 10, 3u, -5.0, 18446744073709551615u) == "
               "18446744073709551615u"));
}

TEST_F(MacroExpansionE2ETest, DynListLiteral) {
  EXPECT_TRUE(EvalBool(
      "math.greatest([dyn(5.4), dyn(10), dyn(3u), dyn(-5.0), dyn(3.5)]) "
      "== 10"));
  EXPECT_TRUE(
      EvalBool("math.least([dyn(5.4), dyn(10), dyn(3u), dyn(-5.0), dyn(3.5)]) "
               "== -5.0"));
}

// ──────────────────────────────────────────────────────────────
// ScalarE2ETest — ceil / floor / round / trunc (double→double),
// abs / sign (int/uint/double kind-dispatch), sqrt (→ double).
// ──────────────────────────────────────────────────────────────

class ScalarE2ETest : public ::testing::Test {};

TEST_F(ScalarE2ETest, Ceil) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.ceil(-1.2)"), -1.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.ceil(1.2)"), 2.0);
}

TEST_F(ScalarE2ETest, Floor) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.floor(-1.2)"), -2.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.floor(1.2)"), 1.0);
}

TEST_F(ScalarE2ETest, RoundHalfAwayFromZero) {
  // CEL round is half-away-from-zero (not banker's rounding).
  EXPECT_DOUBLE_EQ(EvalDouble("math.round(-1.6)"), -2.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.round(-1.4)"), -1.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.round(-1.5)"), -2.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.round(1.2)"), 1.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.round(1.5)"), 2.0);
}

TEST_F(ScalarE2ETest, RoundNanStaysNan) {
  EXPECT_TRUE(EvalBool("math.isNaN(math.round(0.0/0.0))"));
}

TEST_F(ScalarE2ETest, Trunc) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.trunc(-1.2)"), -1.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.trunc(1.2)"), 1.0);
}

TEST_F(ScalarE2ETest, TruncNanStaysNan) {
  EXPECT_TRUE(EvalBool("math.isNaN(math.trunc(0.0/0.0))"));
}

TEST_F(ScalarE2ETest, AbsUint) {
  EXPECT_EQ(EvalUint("math.abs(1u)"), 1u);
}

TEST_F(ScalarE2ETest, AbsInt) {
  EXPECT_EQ(EvalInt("math.abs(1)"), 1);
  EXPECT_EQ(EvalInt("math.abs(-11)"), 11);
}

TEST_F(ScalarE2ETest, AbsDouble) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.abs(1.5)"), 1.5);
  EXPECT_DOUBLE_EQ(EvalDouble("math.abs(-11.5)"), 11.5);
}

TEST_F(ScalarE2ETest, SignUint) {
  EXPECT_EQ(EvalUint("math.sign(100u)"), 1u);
  EXPECT_EQ(EvalUint("math.sign(0u)"), 0u);  // uint sign is never -1.
}

TEST_F(ScalarE2ETest, SignInt) {
  EXPECT_EQ(EvalInt("math.sign(100)"), 1);
  EXPECT_EQ(EvalInt("math.sign(-11)"), -1);
  EXPECT_EQ(EvalInt("math.sign(0)"), 0);
}

TEST_F(ScalarE2ETest, SignDouble) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.sign(100.5)"), 1.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.sign(-32.0)"), -1.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.sign(0.0)"), 0.0);
}

// sqrt always returns double regardless of arg kind (probe #3); the
// corpus has zero sqrt rows but the checker library declares it, so
// we pin the happy path here.
TEST_F(ScalarE2ETest, SqrtDouble) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.sqrt(4.0)"), 2.0);
}

TEST_F(ScalarE2ETest, SqrtIntAndUintYieldDouble) {
  EXPECT_DOUBLE_EQ(EvalDouble("math.sqrt(4)"), 2.0);
  EXPECT_DOUBLE_EQ(EvalDouble("math.sqrt(4u)"), 2.0);
}

TEST_F(ScalarE2ETest, SqrtNegativeYieldsNan) {
  // sqrt of a negative operand → NaN (no error), per the ABI header.
  EXPECT_TRUE(EvalBool("math.isNaN(math.sqrt(-1.0))"));
}

// ──────────────────────────────────────────────────────────────
// PredicateE2ETest — isNaN / isInf / isFinite (double → bool).
// ──────────────────────────────────────────────────────────────

class PredicateE2ETest : public ::testing::Test {};

TEST_F(PredicateE2ETest, IsNaN) {
  EXPECT_TRUE(EvalBool("math.isNaN(0.0/0.0)"));
  EXPECT_TRUE(EvalBool("!math.isNaN(1.0/0.0)"));
}

TEST_F(PredicateE2ETest, IsInf) {
  EXPECT_TRUE(EvalBool("math.isInf(1.0/0.0)"));
  EXPECT_TRUE(EvalBool("!math.isInf(0.0/0.0)"));
}

TEST_F(PredicateE2ETest, IsFinite) {
  EXPECT_TRUE(EvalBool("math.isFinite(1.0/1.5)"));
  EXPECT_TRUE(EvalBool("!math.isFinite(0.0/0.0)"));
  EXPECT_TRUE(EvalBool("!math.isFinite(-1.0/0.0)"));
}

// ──────────────────────────────────────────────────────────────
// BitwiseE2ETest — bitAnd / bitOr / bitXor / bitNot /
// bitShiftLeft / bitShiftRight, over int and uint operands (shift
// amount is always int).  Full-width masks, two's-complement, and
// the spec's shift-by-≥64 → 0 contract.
// ──────────────────────────────────────────────────────────────

class BitwiseE2ETest : public ::testing::Test {};

TEST_F(BitwiseE2ETest, BitAndInt) {
  EXPECT_EQ(EvalInt("math.bitAnd(1, 2)"), 0);
  EXPECT_EQ(EvalInt("math.bitAnd(1, 3)"), 1);
  EXPECT_EQ(EvalInt("math.bitAnd(1, -1)"), 1);
}

TEST_F(BitwiseE2ETest, BitAndUint) {
  EXPECT_EQ(EvalUint("math.bitAnd(1u, 2u)"), 0u);
  EXPECT_EQ(EvalUint("math.bitAnd(1u, 3u)"), 1u);
}

TEST_F(BitwiseE2ETest, BitOrInt) {
  EXPECT_EQ(EvalInt("math.bitOr(1, 2)"), 3);
  EXPECT_EQ(EvalInt("math.bitOr(4, -2)"), -2);
}

TEST_F(BitwiseE2ETest, BitOrUint) {
  EXPECT_EQ(EvalUint("math.bitOr(1u, 4u)"), 5u);
}

TEST_F(BitwiseE2ETest, BitXorInt) {
  EXPECT_EQ(EvalInt("math.bitXor(1, 3)"), 2);
  EXPECT_EQ(EvalInt("math.bitXor(4, -2)"), -6);
}

TEST_F(BitwiseE2ETest, BitXorUint) {
  EXPECT_EQ(EvalUint("math.bitXor(1u, 3u)"), 2u);
}

TEST_F(BitwiseE2ETest, BitNotInt) {
  // Two's-complement: ~x == -x - 1.
  EXPECT_EQ(EvalInt("math.bitNot(1)"), -2);
  EXPECT_EQ(EvalInt("math.bitNot(-1)"), 0);
  EXPECT_EQ(EvalInt("math.bitNot(0)"), -1);
}

TEST_F(BitwiseE2ETest, BitNotUint) {
  EXPECT_EQ(EvalUint("math.bitNot(1u)"), kUint64Max - 1);
  EXPECT_EQ(EvalUint("math.bitNot(0u)"), kUint64Max);
}

TEST_F(BitwiseE2ETest, BitShiftLeftInt) {
  EXPECT_EQ(EvalInt("math.bitShiftLeft(1, 2)"), 4);
}

TEST_F(BitwiseE2ETest, BitShiftLeftUint) {
  EXPECT_EQ(EvalUint("math.bitShiftLeft(1u, 2)"), 4u);
}

TEST_F(BitwiseE2ETest, BitShiftLeftLargeShiftIsZero) {
  // Spec: shift ≥ 64 yields 0 (not UB / wraparound).
  EXPECT_EQ(EvalInt("math.bitShiftLeft(1, 200)"), 0);
  EXPECT_EQ(EvalInt("math.bitShiftLeft(-1, 200)"), 0);
  EXPECT_EQ(EvalUint("math.bitShiftLeft(1u, 200)"), 0u);
}

TEST_F(BitwiseE2ETest, BitShiftRightInt) {
  EXPECT_EQ(EvalInt("math.bitShiftRight(1024, 2)"), 256);
}

TEST_F(BitwiseE2ETest, BitShiftRightUint) {
  EXPECT_EQ(EvalUint("math.bitShiftRight(1024u, 2)"), 256u);
}

TEST_F(BitwiseE2ETest, BitShiftRightLargeShiftIsZero) {
  EXPECT_EQ(EvalInt("math.bitShiftRight(1024, 64)"), 0);
  EXPECT_EQ(EvalInt("math.bitShiftRight(-1024, 64)"), 0);
  EXPECT_EQ(EvalUint("math.bitShiftRight(1024u, 200)"), 0u);
}

TEST_F(BitwiseE2ETest, BitShiftRightIntIsLogicalNotArithmetic) {
  // CEL bitShiftRight on a negative int is a *logical* shift (the
  // two's-complement bit pattern is shifted, no sign extension):
  // -1024 >> 3 == 2305843009213693824.
  EXPECT_EQ(EvalInt("math.bitShiftRight(-1024, 3)"), 2305843009213693824LL);
}

// ──────────────────────────────────────────────────────────────
// ErrorE2ETest — spec-pinned rows that type-check but evaluate to a
// CEL error Value.  `dyn(...)` "no such overload" rows are out of
// scope (RejectDyn rejects them at the frontend), so only the
// well-typed runtime errors appear here.
// ──────────────────────────────────────────────────────────────

class ErrorE2ETest : public ::testing::Test {};

TEST_F(ErrorE2ETest, AbsInt64MinOverflows) {
  // abs(INT64_MIN) has no representable positive → overflow error.
  ExpectEvalError("math.abs(-9223372036854775808)", "abs(INT64_MIN) overflow");
}

TEST_F(ErrorE2ETest, ShiftLeftNegativeOffset) {
  ExpectEvalError("math.bitShiftLeft(1u, -1)", "negative shift offset");
}

TEST_F(ErrorE2ETest, ShiftRightNegativeOffset) {
  ExpectEvalError("math.bitShiftRight(1u, -1)", "negative shift offset");
}

}  // namespace
}  // namespace celwasm
