#include "compiler/codegen/expr_lower.h"

#include <set>
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
using ::testing::HasSubstr;

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

// Negative tests assert both the status code *and* that the diagnostic
// mentions the specific Repr or kind that failed.  A regression that
// collapses every Unimplemented path into a single generic string would
// keep the status code but flunk the substring check — which is exactly
// what CLAUDE.md's "rejected with a good message" rule asks for.

TEST(ExprLowerTest, ListExprIsUnimplementedWithListRepr) {
  auto typed = ParseAndCheck("[1, 2, 3]", CheckOptions{});
  ASSERT_THAT(typed.status(), IsOk());
  WasmModule mod;
  auto s = LowerToEvalFunction(*typed, "eval", mod).status();
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kUnimplemented));
  EXPECT_THAT(std::string(s.message()), HasSubstr("list"));
  EXPECT_THAT(std::string(s.message()), HasSubstr("no scalar ABI lowering"));
}

TEST(ExprLowerTest, MapExprIsUnimplementedWithMapRepr) {
  auto typed = ParseAndCheck("{'a': 1}", CheckOptions{});
  ASSERT_THAT(typed.status(), IsOk());
  WasmModule mod;
  auto s = LowerToEvalFunction(*typed, "eval", mod).status();
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kUnimplemented));
  EXPECT_THAT(std::string(s.message()), HasSubstr("map"));
  EXPECT_THAT(std::string(s.message()), HasSubstr("no scalar ABI lowering"));
}

TEST(ExprLowerTest, StringConstantReturnsI32) {
  // A CEL string lowers to the i32 offset of a CelValue the runtime
  // hands back from cel_make_string_view.  The body shape is a block
  // that allocates, stores each byte, and calls the runtime helper.
  auto L = LowerOk("'hello'");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt32());
  EXPECT_EQ(L.fn.result_repr, Repr::kString);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  EXPECT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
}

TEST(ExprLowerTest, EmptyStringLowers) {
  // The store-per-byte loop must be empty-safe; cel_alloc(0) still
  // returns a valid offset and cel_make_string_view accepts len=0.
  auto L = LowerOk("''");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt32());
  EXPECT_EQ(L.fn.result_repr, Repr::kString);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, EvalModuleImportsSharedMemoryFromRuntime) {
  // The eval module shares linear memory with the runtime by importing
  // `cel`/`memory`.  Without this import every `i32.store8` the string
  // path emits would be reading a private uninitialised memory and the
  // offset we hand back would be meaningless to the runtime.
  auto L = LowerOk("'x'");
  BinaryenModuleRef m = L.mod.raw();
  ASSERT_TRUE(BinaryenHasMemory(m));
  EXPECT_STREQ(BinaryenMemoryImportGetModule(m, "memory"), "cel");
  EXPECT_STREQ(BinaryenMemoryImportGetBase(m, "memory"), "memory");
}

TEST(ExprLowerTest, EvalModuleDeclaresRuntimeFunctionImports) {
  // Even for pure-scalar expressions we declare the full cel_* import
  // set up front; the module is always linked against the runtime and
  // unused imports are harmless.  Binaryen has no dedicated function-
  // import iterator, so walk all functions and keep the ones whose
  // import module is set.
  auto L = LowerOk("1 + 2");
  BinaryenModuleRef m = L.mod.raw();
  std::set<std::string> seen;
  for (BinaryenIndex i = 0; i < BinaryenGetNumFunctions(m); ++i) {
    BinaryenFunctionRef f = BinaryenGetFunctionByIndex(m, i);
    if (BinaryenFunctionImportGetModule(f) != nullptr) {
      seen.insert(BinaryenFunctionGetName(f));
    }
  }
  for (const char* name : {
           "cel_alloc",
           "cel_mem_base",
           "cel_make_string_view",
           "cel_make_bytes_view",
           "cel_string_eq",
           "cel_bytes_eq",
           "cel_string_concat",
           "cel_string_size",
           "cel_bool_from_value",
       }) {
    EXPECT_EQ(seen.count(name), 1u) << "missing import: " << name;
  }
}

// Helper: parse + check `expr` under `variable_specs`, lower it as
// `eval`, and hand back the module.  Mirrors `LowerOk` but threads
// variables through so ident-using cases can assert against the
// emitted function signature.
Lowered LowerOkWithVars(absl::string_view expr,
                        std::vector<std::string> specs) {
  CheckOptions opts;
  opts.variable_specs = std::move(specs);
  auto typed = ParseAndCheck(expr, opts);
  CHECK_OK(typed.status()) << "ParseAndCheck failed for: " << expr;
  Lowered out;
  out.mod = WasmModule();
  auto fn_or = LowerToEvalFunction(*typed, "eval", out.mod);
  CHECK_OK(fn_or.status()) << "LowerToEvalFunction failed for: " << expr;
  out.fn = *fn_or;
  out.mod.ExportFunction("eval", "eval");
  return out;
}

// Binaryen surfaces the function's parameter list as a single
// `BinaryenType` — a tuple for >1 params, a single type for 1, or
// `None` for 0.  `ParamTypes` normalises that into the vector shape
// the tests want to assert against.
std::vector<BinaryenType> ParamTypes(BinaryenFunctionRef fn) {
  BinaryenType packed = BinaryenFunctionGetParams(fn);
  uint32_t n = BinaryenTypeArity(packed);
  std::vector<BinaryenType> out(n);
  if (n == 1) {
    out[0] = packed;
  } else if (n > 1) {
    BinaryenTypeExpand(packed, out.data());
  }
  return out;
}

TEST(ExprLowerTest, IntIdentLowersToLocalGetWithI64Param) {
  auto L = LowerOkWithVars("x + 1", {"x:int"});
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  // One i64 param for `x`; no scratch locals (arithmetic needs none).
  auto params = ParamTypes(fn);
  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params[0], BinaryenTypeInt64());
  EXPECT_EQ(BinaryenFunctionGetNumVars(fn), 0u);

  // Body shape: `x + 1` → Binary(i64.add, LocalGet(0, i64), Const(1)).
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBinaryId());
  BinaryenExpressionRef lhs = BinaryenBinaryGetLeft(body);
  ASSERT_EQ(BinaryenExpressionGetId(lhs), BinaryenLocalGetId());
  EXPECT_EQ(BinaryenLocalGetGetIndex(lhs), 0u);
  EXPECT_EQ(BinaryenExpressionGetType(lhs), BinaryenTypeInt64());
}

TEST(ExprLowerTest, MultipleVarsGetParamsInDeclarationOrder) {
  // Whether or not `y` is referenced, it must claim param slot 1 so
  // the emitted function signature is deterministic for the host.  We
  // therefore use an expression that names only `x` and verify that
  // `y`'s param slot still exists.
  auto L = LowerOkWithVars("x * 2", {"x:int", "y:double"});
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  auto params = ParamTypes(fn);
  ASSERT_EQ(params.size(), 2u);
  EXPECT_EQ(params[0], BinaryenTypeInt64());
  EXPECT_EQ(params[1], BinaryenTypeFloat64());
}

TEST(ExprLowerTest, IdentsOfAllScalarReprs) {
  // One positive case per WASM-scalar-bearing Repr to guard against a
  // regression where WasmTypeFor and LoweredIdent disagree on the type
  // of a `local.get`.  (Binaryen's validator would catch the mismatch,
  // but it's worth nailing down the intended mapping in a test.)
  struct Case { const char* spec; BinaryenType want; };
  for (const auto& c : {
           Case{"b:bool",   BinaryenTypeInt32()},
           Case{"i:int",    BinaryenTypeInt64()},
           Case{"u:uint",   BinaryenTypeInt64()},
           Case{"d:double", BinaryenTypeFloat64()},
       }) {
    SCOPED_TRACE(c.spec);
    std::string name(c.spec, 1);  // first char is the var name.
    auto L = LowerOkWithVars(name, {c.spec});
    EXPECT_THAT(L.mod.Validate(), IsOk());
    BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
    auto params = ParamTypes(fn);
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(params[0], c.want);
    BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
    ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenLocalGetId());
    EXPECT_EQ(BinaryenExpressionGetType(body), c.want);
  }
}

TEST(ExprLowerTest, UnsupportedVariableReprFailsWithSpecName) {
  // `list<int>` has no scalar ABI in M3, so declaring such a variable
  // — even before the body touches it — must fail loudly at
  // LowerToEvalFunction with the variable name in the message.  The
  // alternative (accepting the decl, then choking inside the body if
  // `xs` is referenced) would be silently broken for never-referenced
  // vars and confusing for referenced ones.
  CheckOptions opts;
  opts.variable_specs = {"xs:list<int>"};
  auto typed = ParseAndCheck("1 + 2", opts);
  ASSERT_THAT(typed.status(), IsOk());
  WasmModule mod;
  auto s = LowerToEvalFunction(*typed, "eval", mod).status();
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kUnimplemented));
  EXPECT_THAT(std::string(s.message()), HasSubstr("xs"));
  EXPECT_THAT(std::string(s.message()), HasSubstr("list"));
  EXPECT_THAT(std::string(s.message()), HasSubstr("no scalar ABI"));
}

// String operators (M3 slice D).  Shape-only assertions here: the e2e
// test exercises semantic correctness against the runtime.  We verify
// that each CEL string op routes through the correct runtime import
// because a silent miswire (e.g. dropping into the numeric arithmetic
// path) would still produce a validated module that returns nonsense.

TEST(ExprLowerTest, StringConcatLowersToRuntimeCall) {
  auto L = LowerOk("'hi' + 'there'");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt32());
  EXPECT_EQ(L.fn.result_repr, Repr::kString);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_concat");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
}

TEST(ExprLowerTest, StringEqualityLowersToRuntimeCall) {
  auto L = LowerOk("'a' == 'b'");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_eq");
}

TEST(ExprLowerTest, StringInequalityInvertsEqualityCall) {
  // `_!=_` on strings is `i32.eqz(cel_string_eq(...))`.  We can't
  // reuse the numeric `ne` opcode because the helper returns i32 0/1,
  // so we must invert explicitly.  The validator would accept either
  // shape but only this one is semantically correct.
  auto L = LowerOk("'a' != 'b'");
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenUnaryId());
  EXPECT_EQ(BinaryenUnaryGetOp(body), BinaryenEqZInt32());
  BinaryenExpressionRef inner = BinaryenUnaryGetValue(body);
  ASSERT_EQ(BinaryenExpressionGetId(inner), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(inner), "cel_string_eq");
}

TEST(ExprLowerTest, SizeStringLowersToRuntimeCall) {
  auto L = LowerOk("size('abc')");
  // CEL's `size()` returns int, which is i64 in our ABI.
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt64());
  EXPECT_EQ(L.fn.result_repr, Repr::kInt);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_size");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 1u);
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
  // Strings and bytes both travel as i32 offsets (a CelValue* in the
  // runtime's shared memory).
  EXPECT_EQ(WasmTypeFor(Repr::kString), BinaryenTypeInt32());
  EXPECT_EQ(WasmTypeFor(Repr::kBytes), BinaryenTypeInt32());
  EXPECT_EQ(WasmTypeFor(Repr::kList), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kMap), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kMessage), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kNull), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kUnknown), BinaryenTypeNone());
}

}  // namespace
}  // namespace celwasm
