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
  LoweredFunction fn{};
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

TEST(ExprLowerTest, UintArithmeticDispatchesToUintHelper) {
  auto L = LowerOk("10u / 3u");
  EXPECT_THAT(L.mod.Validate(), IsOk());
  // Post-Slice-B, int/uint arithmetic lowers to a block that calls the
  // checked-arithmetic helper, traps on CEL_ERROR, and loads the
  // payload.  The unsigned distinction is carried by the helper name
  // (`cel_uint_div_uu`), not by a wasm opcode.
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  EXPECT_EQ(BinaryenExpressionGetType(body), BinaryenTypeInt64());
  ASSERT_GT(BinaryenBlockGetNumChildren(body), 0u);
  BinaryenExpressionRef first = BinaryenBlockGetChildAt(body, 0);
  ASSERT_EQ(BinaryenExpressionGetId(first), BinaryenLocalSetId());
  BinaryenExpressionRef call = BinaryenLocalSetGetValue(first);
  ASSERT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_uint_div_uu");
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
           "cel_bytes_concat",
           "cel_string_size",
           "cel_bytes_size",
           "cel_bool_from_value",
           "cel_string_starts_with",
           "cel_string_ends_with",
           "cel_string_contains",
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
    out.at(0) = packed;
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
  // One i64 param for `x`.  The checked-arithmetic lowering allocates
  // an i32 scratch local for the helper's CelValue-offset return, so
  // num-vars is >= 1 post-Slice-B (not 0).
  auto params = ParamTypes(fn);
  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params.at(0), BinaryenTypeInt64());
  EXPECT_GE(BinaryenFunctionGetNumVars(fn), 1u);

  // Body shape: `x + 1` → Block(... Call(cel_int_add_ii, LocalGet(0,
  // i64), Const(1)) ... i64.load at offset 8).  Walk into the block to
  // confirm the helper sees the LocalGet as its lhs.
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  ASSERT_GT(BinaryenBlockGetNumChildren(body), 0u);
  BinaryenExpressionRef first = BinaryenBlockGetChildAt(body, 0);
  ASSERT_EQ(BinaryenExpressionGetId(first), BinaryenLocalSetId());
  BinaryenExpressionRef call = BinaryenLocalSetGetValue(first);
  ASSERT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_int_add_ii");
  ASSERT_GE(BinaryenCallGetNumOperands(call), 1u);
  BinaryenExpressionRef lhs = BinaryenCallGetOperandAt(call, 0);
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
  EXPECT_EQ(params.at(0), BinaryenTypeInt64());
  EXPECT_EQ(params.at(1), BinaryenTypeFloat64());
}

TEST(ExprLowerTest, IdentsOfAllScalarReprs) {
  // One positive case per WASM-scalar-bearing Repr to guard against a
  // regression where WasmTypeFor and LoweredIdent disagree on the type
  // of a `local.get`.  (Binaryen's validator would catch the mismatch,
  // but it's worth nailing down the intended mapping in a test.)
  struct Case {
    const char* spec;
    BinaryenType want;
  };
  for (const auto& c : {
           Case{"b:bool", BinaryenTypeInt32()},
           Case{"i:int", BinaryenTypeInt64()},
           Case{"u:uint", BinaryenTypeInt64()},
           Case{"d:double", BinaryenTypeFloat64()},
       }) {
    SCOPED_TRACE(c.spec);
    std::string name(c.spec, 1);  // first char is the var name.
    auto L = LowerOkWithVars(name, {c.spec});
    EXPECT_THAT(L.mod.Validate(), IsOk());
    BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
    auto params = ParamTypes(fn);
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(params.at(0), c.want);
    BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
    ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenLocalGetId());
    EXPECT_EQ(BinaryenExpressionGetType(body), c.want);
  }
}

// Message-valued variables travel as externref params and bring along
// the `$cel_refs` table + `cel_wrap_message` / `cel_unwrap_message`
// helpers — the plumbing a later slice will rely on to lower field
// reads.  Without a message-typed variable, none of that machinery
// should appear.
TEST(ExprLowerTest, MessageVariableLowersAsExternrefParam) {
  // `google.protobuf.Empty` is always in the generated pool so no
  // schema file is needed; the body is the bare ident which forces
  // the param layout to be inspected.
  auto L = LowerOkWithVars("m", {"m:google.protobuf.Empty"});
  EXPECT_THAT(L.mod.Validate(), IsOk());
  EXPECT_EQ(L.fn.result_repr, Repr::kMessage);
  EXPECT_EQ(L.fn.result_type, BinaryenTypeExternref());

  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  auto params = ParamTypes(fn);
  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params.at(0), BinaryenTypeExternref());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenLocalGetId());
  EXPECT_EQ(BinaryenExpressionGetType(body), BinaryenTypeExternref());
}

TEST(ExprLowerTest, MessageVariablePullsInCelRefsTableAndWrappers) {
  auto L = LowerOkWithVars("m", {"m:google.protobuf.Empty"});
  EXPECT_NE(BinaryenGetTable(L.mod.raw(), "$cel_refs"), nullptr);
  EXPECT_NE(BinaryenGetFunction(L.mod.raw(), "cel_ref_intern"), nullptr);
  EXPECT_NE(BinaryenGetFunction(L.mod.raw(), "cel_ref_get"), nullptr);
  EXPECT_NE(BinaryenGetFunction(L.mod.raw(), "cel_wrap_message"), nullptr);
  EXPECT_NE(BinaryenGetFunction(L.mod.raw(), "cel_unwrap_message"), nullptr);
}

TEST(ExprLowerTest, NoMessageVariableMeansNoCelRefsTable) {
  auto L = LowerOk("1 + 2");
  EXPECT_EQ(BinaryenGetTable(L.mod.raw(), "$cel_refs"), nullptr);
  EXPECT_EQ(BinaryenGetFunction(L.mod.raw(), "cel_wrap_message"), nullptr);
  EXPECT_EQ(BinaryenGetFunction(L.mod.raw(), "cel_unwrap_message"), nullptr);
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

// String member calls (M3 slice E).  The checker lowers
// `'x'.startsWith('y')` to a CallExpr with `target='x'`, `function='startsWith'`,
// `args=['y']`; codegen must dispatch on the function-name string and
// emit a call to the matching runtime helper.  Same shape for
// endsWith / contains.

TEST(ExprLowerTest, StartsWithLowersToRuntimeCall) {
  auto L = LowerOk("'hello'.startsWith('he')");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_starts_with");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
}

TEST(ExprLowerTest, EndsWithLowersToRuntimeCall) {
  auto L = LowerOk("'hello'.endsWith('lo')");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_ends_with");
}

TEST(ExprLowerTest, ContainsLowersToRuntimeCall) {
  auto L = LowerOk("'hello'.contains('ell')");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_contains");
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

// Bytes operators (M3 slice F).  Mirrors the string shape-checks above.
// The bytes literal lowering reuses `LowerSpanLiteral` with the bytes
// constructor, so the body is a block ending in `cel_make_bytes_view`;
// concat and size route through the dedicated bytes runtime helpers.

TEST(ExprLowerTest, BytesConstantReturnsI32) {
  auto L = LowerOk("b'hi'");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt32());
  EXPECT_EQ(L.fn.result_repr, Repr::kBytes);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  EXPECT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  // Last child of the block is the ctor call — walk to it and verify
  // it targets the bytes constructor rather than the string one.  A
  // silent miswire would still validate but produce a CEL_STRING at
  // runtime, which the rest of the pipeline would then misinterpret.
  BinaryenIndex n = BinaryenBlockGetNumChildren(body);
  ASSERT_GT(n, 0u);
  BinaryenExpressionRef last = BinaryenBlockGetChildAt(body, n - 1);
  ASSERT_EQ(BinaryenExpressionGetId(last), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(last), "cel_make_bytes_view");
}

TEST(ExprLowerTest, EmptyBytesLowers) {
  auto L = LowerOk("b''");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt32());
  EXPECT_EQ(L.fn.result_repr, Repr::kBytes);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, BytesConcatLowersToRuntimeCall) {
  auto L = LowerOk("b'ab' + b'cd'");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt32());
  EXPECT_EQ(L.fn.result_repr, Repr::kBytes);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_bytes_concat");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
}

TEST(ExprLowerTest, BytesEqualityLowersToRuntimeCall) {
  auto L = LowerOk("b'a' == b'b'");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_bytes_eq");
}

TEST(ExprLowerTest, SizeBytesLowersToRuntimeCall) {
  auto L = LowerOk("size(b'abc')");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeInt64());
  EXPECT_EQ(L.fn.result_repr, Repr::kInt);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_bytes_size");
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
  // Messages travel as externref (host-owned proto handle).
  EXPECT_EQ(WasmTypeFor(Repr::kMessage), BinaryenTypeExternref());
  EXPECT_EQ(WasmTypeFor(Repr::kList), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kMap), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kNull), BinaryenTypeNone());
  EXPECT_EQ(WasmTypeFor(Repr::kUnknown), BinaryenTypeNone());
}

}  // namespace
}  // namespace celwasm
