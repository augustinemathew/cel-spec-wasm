#include "compiler/codegen/expr_lower.h"

#include <string>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "binaryen-c.h"
#include "compiler/codegen/module.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/annotations.h"
#include "compiler/ir/typed_ast.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// End-to-end helper: parse + check `expr`, lower it as `eval`, export
// it, and hand back the module so callers can inspect the result.
struct Lowered {
  WasmModule mod;
  LoweredFunction fn;
};

Lowered LowerOk(absl::string_view expr) {
  auto typed = ParseAndCheck(expr, CheckOptions{});
  CHECK_OK(typed.status()) << "ParseAndCheck failed for: " << expr;
  Lowered out;
  out.mod = WasmModule();
  auto fn_or = LowerToEvalFunction(*typed, "eval", out.mod);
  CHECK_OK(fn_or.status()) << "LowerToEvalFunction failed for: " << expr;
  out.fn = *fn_or;
  out.mod.ExportFunction("eval", "eval");
  return out;
}

// Positive cases: each returns OK and the validator accepts.

TEST(ExprLowerTest, IntConstantReturnsI64) {
  auto L = LowerOk("42");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt64());
  EXPECT_EQ(L.fn.result_repr, Repr::kInt);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, UintConstantReturnsI64) {
  auto L = LowerOk("7u");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt64());
  EXPECT_EQ(L.fn.result_repr, Repr::kUint);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, DoubleConstantReturnsF64) {
  auto L = LowerOk("3.14");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeFloat64());
  EXPECT_EQ(L.fn.result_repr, Repr::kDouble);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, BoolConstantReturnsI32) {
  auto L = LowerOk("true");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt32());
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, IntArithmetic) {
  EXPECT_THAT(LowerOk("1 + 2 * 3 - 4").mod.Validate(), IsOk());
  EXPECT_THAT(LowerOk("10 / 3").mod.Validate(), IsOk());
  EXPECT_THAT(LowerOk("10 % 3").mod.Validate(), IsOk());
}

TEST(ExprLowerTest, UintArithmeticUsesUnsignedOpcodes) {
  auto L = LowerOk("10u / 3u");
  EXPECT_THAT(L.mod.Validate(), IsOk());
  // The outermost body op must be i64.div_u, not div_s.
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBinaryId());
  EXPECT_EQ(BinaryenBinaryGetOp(body), BinaryenDivUInt64());
}

TEST(ExprLowerTest, DoubleArithmetic) {
  auto L = LowerOk("1.0 + 2.0 * 3.0 - 4.0 / 5.0");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeFloat64());
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, UnaryNegateInt) {
  // A bare `-5` literal may be folded by the parser into a constant.
  // `-(1 + 2)` forces a real NEGATE call on a non-constant.
  auto L = LowerOk("-(1 + 2)");
  EXPECT_EQ(L.fn.result_repr, Repr::kInt);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  // Our lowering is `0 - (1+2)`, i.e. a Binary node with i64.sub.
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBinaryId());
  EXPECT_EQ(BinaryenBinaryGetOp(body), BinaryenSubInt64());
}

TEST(ExprLowerTest, UnaryNegateDouble) {
  auto L = LowerOk("-(1.0 + 2.0)");
  EXPECT_EQ(L.fn.result_repr, Repr::kDouble);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenUnaryId());
  EXPECT_EQ(BinaryenUnaryGetOp(body), BinaryenNegFloat64());
}

TEST(ExprLowerTest, LogicalNot) {
  auto L = LowerOk("!true");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenUnaryId());
  EXPECT_EQ(BinaryenUnaryGetOp(body), BinaryenEqZInt32());
}

TEST(ExprLowerTest, IntComparisonsAreSigned) {
  auto L = LowerOk("1 < 2");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBinaryId());
  EXPECT_EQ(BinaryenBinaryGetOp(body), BinaryenLtSInt64());
}

TEST(ExprLowerTest, UintComparisonsAreUnsigned) {
  auto L = LowerOk("1u < 2u");
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBinaryId());
  EXPECT_EQ(BinaryenBinaryGetOp(body), BinaryenLtUInt64());
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, DoubleComparisons) {
  auto L = LowerOk("1.0 <= 2.0");
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBinaryId());
  EXPECT_EQ(BinaryenBinaryGetOp(body), BinaryenLeFloat64());
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, EqualityAcrossScalarReprs) {
  EXPECT_THAT(LowerOk("true == false").mod.Validate(), IsOk());
  EXPECT_THAT(LowerOk("1 == 2").mod.Validate(), IsOk());
  EXPECT_THAT(LowerOk("1u != 2u").mod.Validate(), IsOk());
  EXPECT_THAT(LowerOk("1.5 != 2.5").mod.Validate(), IsOk());
}

TEST(ExprLowerTest, LogicalAndShortCircuits) {
  auto L = LowerOk("true && false");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  // Lowering shape is `if(lhs) rhs else 0`.
  EXPECT_EQ(BinaryenExpressionGetId(body), BinaryenIfId());
}

TEST(ExprLowerTest, LogicalOrShortCircuits) {
  auto L = LowerOk("false || true");
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  EXPECT_EQ(BinaryenExpressionGetId(body), BinaryenIfId());
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, Conditional) {
  auto L = LowerOk("true ? 1 : 2");
  EXPECT_EQ(L.fn.result_repr, Repr::kInt);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  EXPECT_EQ(BinaryenExpressionGetId(body), BinaryenIfId());
}

TEST(ExprLowerTest, MixedExpressionValidates) {
  // Arithmetic + comparison + ternary + logical.
  auto L = LowerOk("(1 + 2) * 3 == 9 ? (true && !false) : false");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

// Negative cases: outside-MVP kinds return Unimplemented cleanly (no crash,
// no invalid module).

TEST(ExprLowerTest, ListExprIsUnimplemented) {
  auto typed = ParseAndCheck("[1, 2, 3]", CheckOptions{});
  ASSERT_THAT(typed.status(), IsOk());
  WasmModule mod;
  EXPECT_THAT(LowerToEvalFunction(*typed, "eval", mod).status(),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(ExprLowerTest, MapExprIsUnimplemented) {
  auto typed = ParseAndCheck("{'a': 1}", CheckOptions{});
  ASSERT_THAT(typed.status(), IsOk());
  WasmModule mod;
  EXPECT_THAT(LowerToEvalFunction(*typed, "eval", mod).status(),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(ExprLowerTest, StringConstantIsUnimplemented) {
  // Strings travel through linear memory; codegen in M2 MVP does
  // not wire that up yet.
  auto typed = ParseAndCheck("'hello'", CheckOptions{});
  ASSERT_THAT(typed.status(), IsOk());
  WasmModule mod;
  EXPECT_THAT(LowerToEvalFunction(*typed, "eval", mod).status(),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(ExprLowerTest, IdentifierIsUnimplemented) {
  CheckOptions opts;
  opts.variable_specs.push_back("x:int");
  auto typed = ParseAndCheck("x + 1", opts);
  ASSERT_THAT(typed.status(), IsOk());
  WasmModule mod;
  EXPECT_THAT(LowerToEvalFunction(*typed, "eval", mod).status(),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(ExprLowerTest, WasmTypeForScalars) {
  EXPECT_EQ(WasmTypeFor(Repr::kBool), BinaryenTypeInt32());
  EXPECT_EQ(WasmTypeFor(Repr::kInt), BinaryenTypeInt64());
  EXPECT_EQ(WasmTypeFor(Repr::kUint), BinaryenTypeInt64());
  EXPECT_EQ(WasmTypeFor(Repr::kDouble), BinaryenTypeFloat64());
  EXPECT_EQ(WasmTypeFor(Repr::kDuration), BinaryenTypeInt64());
  EXPECT_EQ(WasmTypeFor(Repr::kTimestamp), BinaryenTypeInt64());
  EXPECT_EQ(WasmTypeFor(Repr::kEnum), BinaryenTypeInt64());
  EXPECT_EQ(WasmTypeFor(Repr::kType), BinaryenTypeInt32());
  EXPECT_EQ(WasmTypeFor(Repr::kString), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kBytes), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kList), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kMap), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kMessage), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kNull), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kUnknown), BinaryenTypeNone());
}

}  // namespace
}  // namespace celwasm
