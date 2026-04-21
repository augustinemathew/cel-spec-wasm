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

// Since M4 Slice C commit 3 the eval body is wrapped in the sret-root
// epilogue: the actual expression is handed to `cel_box_<repr>_at`
// (or `cel_copy_celvalue_at`) as operand 1.  If the lowering used the
// scratch slot for checked arithmetic, the whole thing sits inside a
// 2-child Block emitting a `local.set $scratch (call $cel_alloc 24)`
// prologue.  Since 3b2 the prologue may also carry per-bool-param
// boxing `local.set`s, which multiplies the block to N+1 children.
// Most tests want the inner expression so they can assert the shape
// the lowering produced — this helper peels those wrappers off, plus
// the outer `cel_make_bool` wrap that every bool-producing helper
// site now emits.
BinaryenExpressionRef ScalarBody(BinaryenExpressionRef body) {
  if (BinaryenExpressionGetId(body) == BinaryenBlockId()) {
    const BinaryenIndex n = BinaryenBlockGetNumChildren(body);
    // The prologue-wrapping block's only non-LocalSet child is the
    // terminal body expression.  Any longer prefix of LocalSets is
    // valid (one per bool param + optional scratch alloc); we walk
    // the prefix and land on the first non-LocalSet child.
    if (n >= 2) {
      bool all_prefix_are_local_set = true;
      for (BinaryenIndex i = 0; i + 1 < n; ++i) {
        if (BinaryenExpressionGetId(BinaryenBlockGetChildAt(body, i)) !=
            BinaryenLocalSetId()) {
          all_prefix_are_local_set = false;
          break;
        }
      }
      if (all_prefix_are_local_set) {
        body = BinaryenBlockGetChildAt(body, n - 1);
      }
    }
  }
  if (BinaryenExpressionGetId(body) == BinaryenCallId() &&
      BinaryenCallGetNumOperands(body) == 2) {
    body = BinaryenCallGetOperandAt(body, 1);
  }
  // For message roots EmitSretStore also wraps the externref through
  // cel_wrap_message; peel that so the "inner" body is the original
  // externref expression.  The same single-operand peel also strips
  // the `cel_make_bool` wrap every bool-producing helper site emits
  // after 3b2 so individual shape assertions can target the
  // underlying i32 opcode or runtime call.
  if (BinaryenExpressionGetId(body) == BinaryenCallId() &&
      BinaryenCallGetNumOperands(body) == 1) {
    const char* target = BinaryenCallGetTarget(body);
    if (target != nullptr && (std::string(target) == "cel_wrap_message" ||
                              std::string(target) == "cel_make_bool")) {
      return BinaryenCallGetOperandAt(body, 0);
    }
  }
  return body;
}

// Positive cases: each returns OK and the validator accepts.

TEST(ExprLowerTest, IntConstantReturnsI64) {
  auto L = LowerOk("42");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
  EXPECT_EQ(L.fn.result_repr, Repr::kInt);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, UintConstantReturnsI64) {
  auto L = LowerOk("7u");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
  EXPECT_EQ(L.fn.result_repr, Repr::kUint);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, DoubleConstantReturnsF64) {
  auto L = LowerOk("3.14");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
  EXPECT_EQ(L.fn.result_repr, Repr::kDouble);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, BoolConstantReturnsI32) {
  auto L = LowerOk("true");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
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
  // After ScalarBody peels the sret epilogue (box call + scratch-slot
  // prologue) the checked-arith block surfaces directly:
  //   Block[ Call(cel_uint_div_at_uu, slot, 10, 3),
  //          If(kind==ERR, unreachable),
  //          i64.load offset=8 ].
  // The unsigned distinction is carried by the helper name, not a
  // wasm opcode.
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  EXPECT_EQ(BinaryenExpressionGetType(body), BinaryenTypeInt64());
  ASSERT_GT(BinaryenBlockGetNumChildren(body), 0u);
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 0);
  ASSERT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_uint_div_at_uu");
}

TEST(ExprLowerTest, DoubleArithmetic) {
  auto L = LowerOk("1.0 + 2.0 * 3.0 - 4.0 / 5.0");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
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
  body = ScalarBody(body);
  // After peeling the sret epilogue the negate is `Binary(Sub, 0,
  // checked_arith_block)` — wasm has no i64 unary negate, so the
  // canonical form is `0 - x`.
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBinaryId());
  EXPECT_EQ(BinaryenBinaryGetOp(body), BinaryenSubInt64());
}

TEST(ExprLowerTest, UnaryNegateDouble) {
  auto L = LowerOk("-(1.0 + 2.0)");
  EXPECT_EQ(L.fn.result_repr, Repr::kDouble);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenUnaryId());
  EXPECT_EQ(BinaryenUnaryGetOp(body), BinaryenNegFloat64());
}

TEST(ExprLowerTest, LogicalNot) {
  auto L = LowerOk("!true");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  // 3VL (M4 Slice C / 3b2): `!_` lowers to `cel_not` so CEL_UNKNOWN /
  // CEL_ERROR operands pass through instead of being collapsed to 0/1
  // by i32.eqz.
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_not");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 1u);
}

TEST(ExprLowerTest, IntComparisonDispatchesToBoxedHelper) {
  // Uniform-boxed ABI (Step 3): every scalar comparison lowers to the
  // 3VL-aware `cel_cmp_<kind>_<op>` helper, never to a raw wasm
  // compare opcode.  The helper itself is signed-aware for int
  // (matches the runtime implementation in cel_runtime.c); the shape
  // contract here is "codegen took the boxed dispatch" + the right
  // helper name.
  auto L = LowerOk("1 < 2");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_cmp_int_lt");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
}

TEST(ExprLowerTest, UintComparisonDispatchesToBoxedHelper) {
  auto L = LowerOk("1u < 2u");
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_cmp_uint_lt");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
}

TEST(ExprLowerTest, DoubleComparisonDispatchesToBoxedHelper) {
  // NaN-in-ordered-compare → CEL_ERROR is now a runtime responsibility
  // (`cel_cmp_double_le` in cel_runtime.c); codegen no longer emits a
  // NaN-guard block.  The shape is the same single-helper Call as
  // int / uint.
  auto L = LowerOk("1.0 <= 2.0");
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_cmp_double_le");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
}

TEST(ExprLowerTest, EqualityAcrossScalarReprs) {
  EXPECT_THAT(LowerOk("true == false").mod.Validate(), IsOk());
  EXPECT_THAT(LowerOk("1 == 2").mod.Validate(), IsOk());
  EXPECT_THAT(LowerOk("1u != 2u").mod.Validate(), IsOk());
  EXPECT_THAT(LowerOk("1.5 != 2.5").mod.Validate(), IsOk());
}

TEST(ExprLowerTest, LogicalAndIsThreeValued) {
  // Post-3b2 `&&` delegates to `cel_and` so CEL_UNKNOWN / CEL_ERROR
  // in either operand propagate through the logical result instead of
  // being implicitly coerced.
  auto L = LowerOk("true && false");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_and");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
}

TEST(ExprLowerTest, LogicalOrIsThreeValued) {
  auto L = LowerOk("false || true");
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_or");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, Conditional) {
  auto L = LowerOk("true ? 1 : 2");
  EXPECT_EQ(L.fn.result_repr, Repr::kInt);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  // Since M4 Slice E1 the ternary lowers to a 3-child Block:
  // [cond_set, err_if (non-OK => copy to sret + return), inner_if].
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  ASSERT_EQ(BinaryenBlockGetNumChildren(body), 3u);
  EXPECT_EQ(BinaryenExpressionGetId(BinaryenBlockGetChildAt(body, 0)),
            BinaryenLocalSetId());
  EXPECT_EQ(BinaryenExpressionGetId(BinaryenBlockGetChildAt(body, 1)),
            BinaryenIfId());
  EXPECT_EQ(BinaryenExpressionGetId(BinaryenBlockGetChildAt(body, 2)),
            BinaryenIfId());
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
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
  EXPECT_EQ(L.fn.result_repr, Repr::kString);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  EXPECT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
}

TEST(ExprLowerTest, EmptyStringLowers) {
  // The store-per-byte loop must be empty-safe; cel_alloc(0) still
  // returns a valid offset and cel_make_string_view accepts len=0.
  auto L = LowerOk("''");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
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
           "cel_bool_from_value",
           "cel_string_eq_v",
           "cel_bytes_eq_v",
           "cel_string_concat_v",
           "cel_bytes_concat_v",
           "cel_string_starts_with_v",
           "cel_string_ends_with_v",
           "cel_string_contains_v",
           "cel_string_size_v",
           "cel_bytes_size_v",
           // cel_log is declared unconditionally so the runtime's own
           // CEL_LOG calls (and any future codegen-emitted traces) bind
           // against the host's `cel_env.cel_log` trampoline.
           "cel_log",
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
  // Param 0 is the sret out-slot (i32); param 1 is `x` (i64).
  auto params = ParamTypes(fn);
  ASSERT_EQ(params.size(), 2u);
  EXPECT_EQ(params.at(0), BinaryenTypeInt32());
  EXPECT_EQ(params.at(1), BinaryenTypeInt64());
  EXPECT_GE(BinaryenFunctionGetNumVars(fn), 1u);

  // After ScalarBody peels the sret epilogue and the scalar-auto-box
  // prologue, the checked-arith block surfaces directly:
  //   Block {
  //     Call(cel_int_add_at_ii, LocalGet(scratch),
  //          Call(cel_int_from_value, LocalGet(boxed_x)), Const(1));
  //     If(kind==ERR, unreachable);
  //     i64.load offset=8 (cel_mem_base + scratch)
  //   }
  // `x` is auto-boxed at $eval entry (Step 1 of the uniform boxed
  // ABI): the raw i64 param at index 1 is wrapped via `cel_make_int`
  // and stored in the first scratch local (index 2); ident reads
  // load that local and unbox via `cel_int_from_value`, which is a
  // no-op at runtime but moves the source of truth to the boxed form.
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  ASSERT_GT(BinaryenBlockGetNumChildren(body), 0u);
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 0);
  ASSERT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_int_add_at_ii");
  // Arg 0: scratch-slot (LocalGet i32).  Arg 1: the unboxed ident lhs.
  ASSERT_EQ(BinaryenCallGetNumOperands(call), 3u);
  BinaryenExpressionRef slot_arg = BinaryenCallGetOperandAt(call, 0);
  ASSERT_EQ(BinaryenExpressionGetId(slot_arg), BinaryenLocalGetId());
  EXPECT_EQ(BinaryenExpressionGetType(slot_arg), BinaryenTypeInt32());
  BinaryenExpressionRef lhs = BinaryenCallGetOperandAt(call, 1);
  ASSERT_EQ(BinaryenExpressionGetId(lhs), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(lhs), "cel_int_from_value");
  EXPECT_EQ(BinaryenExpressionGetType(lhs), BinaryenTypeInt64());
  ASSERT_EQ(BinaryenCallGetNumOperands(lhs), 1u);
  BinaryenExpressionRef inner = BinaryenCallGetOperandAt(lhs, 0);
  ASSERT_EQ(BinaryenExpressionGetId(inner), BinaryenLocalGetId());
  EXPECT_EQ(BinaryenExpressionGetType(inner), BinaryenTypeInt32());
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
  // Param 0 is the sret slot; user vars start at 1.
  ASSERT_EQ(params.size(), 3u);
  EXPECT_EQ(params.at(0), BinaryenTypeInt32());
  EXPECT_EQ(params.at(1), BinaryenTypeInt64());
  EXPECT_EQ(params.at(2), BinaryenTypeFloat64());
}

TEST(ExprLowerTest, IdentsOfAllScalarReprs) {
  // One positive case per WASM-scalar-bearing Repr to guard against a
  // regression where WasmTypeFor and LoweredIdent disagree on the type
  // of a `local.get`.  (Binaryen's validator would catch the mismatch,
  // but it's worth nailing down the intended mapping in a test.)
  //
  // All four scalar params get auto-boxed to a CelValue offset at
  // $eval entry (uniform boxed ABI Step 1): the raw scalar at param
  // index 1 is wrapped via `cel_make_<kind>` and stored in the first
  // scratch local (index 2).  Bool ident reads stop there (Repr::kBool
  // already travels as an offset post-3b2); int / uint / double reads
  // add an inline `cel_<kind>_from_value` unbox so the rest of codegen
  // keeps speaking raw scalars.
  struct Case {
    const char* spec;
    BinaryenType param_t;   // raw param wire type.
    BinaryenType result_t;  // ident-read result type.
    const char* unbox_fn;   // nullptr when no unbox is emitted (bool).
  };
  for (const auto& c : {
           Case{"b:bool", BinaryenTypeInt32(), BinaryenTypeInt32(), nullptr},
           Case{"i:int", BinaryenTypeInt64(), BinaryenTypeInt64(),
                "cel_int_from_value"},
           Case{"u:uint", BinaryenTypeInt64(), BinaryenTypeInt64(),
                "cel_uint_from_value"},
           Case{"d:double", BinaryenTypeFloat64(), BinaryenTypeFloat64(),
                "cel_double_from_value"},
       }) {
    SCOPED_TRACE(c.spec);
    std::string name(c.spec, 1);  // first char is the var name.
    auto L = LowerOkWithVars(name, {c.spec});
    EXPECT_THAT(L.mod.Validate(), IsOk());
    BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
    auto params = ParamTypes(fn);
    // Param 0 is the sret slot; the user's variable lives at 1.
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(params.at(0), BinaryenTypeInt32());
    EXPECT_EQ(params.at(1), c.param_t);
    BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
    body = ScalarBody(body);
    if (c.unbox_fn == nullptr) {
      // Bool: body is `local.get boxed_b` (i32 offset) — no unbox.
      ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenLocalGetId());
      EXPECT_EQ(BinaryenLocalGetGetIndex(body), 2u);
      EXPECT_EQ(BinaryenExpressionGetType(body), c.result_t);
    } else {
      // Scalar: body is `call $unbox_fn (local.get boxed_x)`.
      ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
      EXPECT_STREQ(BinaryenCallGetTarget(body), c.unbox_fn);
      EXPECT_EQ(BinaryenExpressionGetType(body), c.result_t);
      ASSERT_EQ(BinaryenCallGetNumOperands(body), 1u);
      BinaryenExpressionRef inner = BinaryenCallGetOperandAt(body, 0);
      ASSERT_EQ(BinaryenExpressionGetId(inner), BinaryenLocalGetId());
      EXPECT_EQ(BinaryenLocalGetGetIndex(inner), 2u);
      EXPECT_EQ(BinaryenExpressionGetType(inner), BinaryenTypeInt32());
    }
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
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());

  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  auto params = ParamTypes(fn);
  // Param 0 is the sret slot; `m` is at index 1.
  ASSERT_EQ(params.size(), 2u);
  EXPECT_EQ(params.at(0), BinaryenTypeInt32());
  EXPECT_EQ(params.at(1), BinaryenTypeExternref());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenLocalGetId());
  EXPECT_EQ(BinaryenLocalGetGetIndex(body), 1u);
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

// Message equality routes through `LowerMessageEqualityBoxed` (Slice F
// Step 5): the top-level call is a BinaryenIf over the
// `cel_message_eq_prologue_v` result, non-OK branch returning the
// prologue status, OK branch calling `cel_make_bool(message_eq(...))`.
// Pins the shape so a regression that drops the absorption gate would
// fail the test.
TEST(ExprLowerTest, MessageEqualityLowersThroughPrologueAndHostCall) {
  auto L = LowerOkWithVars(
      "a == b", {"a:google.protobuf.Empty", "b:google.protobuf.Empty"});
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  // The eval body for `a == b` must contain the prologue helper, the
  // host compare, and cel_make_bool on the OK branch.  We assert
  // their presence by scanning the function (shape of the block
  // changes with local-set prologue churn across slices).
  EXPECT_NE(BinaryenGetFunction(L.mod.raw(), "cel_message_eq_prologue_v"),
            nullptr);
  EXPECT_NE(BinaryenGetFunction(L.mod.raw(), "message_eq"), nullptr);
}

TEST(ExprLowerTest, MessageInequalityInvertsEqCallOnOkBranch) {
  // `_!=_` wraps the host `message_eq` result in `i32.eqz` before
  // `cel_make_bool`.  Non-OK short-circuit still goes through the
  // prologue-return branch untouched.
  auto L = LowerOkWithVars(
      "a != b", {"a:google.protobuf.Empty", "b:google.protobuf.Empty"});
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  EXPECT_NE(BinaryenGetFunction(L.mod.raw(), "cel_message_eq_prologue_v"),
            nullptr);
}

// Ternary lowering (Slice F step 6).  A ternary consumed by a 3VL
// absorber — here, boxed `==` then `|| true` — must route through
// `LowerConditionalBoxed` so the cond's UNKNOWN / ERROR flows into the
// absorber as a CelValue rather than being swallowed by `$eval`'s sret
// early-return.  We pin the presence of `cel_bool_from_value` (the OK-
// branch unbox) and `cel_mem_base` (the kind-byte probe on the cond
// offset) in the emitted module.  A regression that dropped either
// signal would mean the boxed ternary path was bypassed.
TEST(ExprLowerTest, TernaryUnderAbsorberLowersThroughBoxedForm) {
  auto L = LowerOk("(1 > 0 ? 1 : 2) == 1 || true");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  EXPECT_NE(BinaryenGetFunction(L.mod.raw(), "cel_bool_from_value"), nullptr);
  EXPECT_NE(BinaryenGetFunction(L.mod.raw(), "cel_mem_base"), nullptr);
  // The outer absorber must be `cel_or`, not a raw `i32.or`.
  EXPECT_NE(BinaryenGetFunction(L.mod.raw(), "cel_or"), nullptr);
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
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
  EXPECT_EQ(L.fn.result_repr, Repr::kString);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  // Slice F Step 4: codegen routes concat through the absorbing `_v`
  // helper so an UNKNOWN / ERROR operand surfaces as a CelValue.
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_concat_v");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
}

TEST(ExprLowerTest, StringEqualityLowersToRuntimeCall) {
  auto L = LowerOk("'a' == 'b'");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_eq_v");
}

TEST(ExprLowerTest, StringInequalityInvertsEqualityCall) {
  // `_!=_` on strings wraps the `_v` equality result in `cel_not`
  // rather than raw `i32.eqz` (Slice F Step 4): `cel_not` is 3VL-aware
  // and propagates UNKNOWN / ERROR unchanged, whereas `i32.eqz` would
  // collapse them to a bogus bool.
  auto L = LowerOk("'a' != 'b'");
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_not");
  ASSERT_EQ(BinaryenCallGetNumOperands(body), 1u);
  BinaryenExpressionRef inner = BinaryenCallGetOperandAt(body, 0);
  ASSERT_EQ(BinaryenExpressionGetId(inner), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(inner), "cel_string_eq_v");
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
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_starts_with_v");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
}

TEST(ExprLowerTest, EndsWithLowersToRuntimeCall) {
  auto L = LowerOk("'hello'.endsWith('lo')");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_ends_with_v");
}

TEST(ExprLowerTest, ContainsLowersToRuntimeCall) {
  auto L = LowerOk("'hello'.contains('ell')");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_string_contains_v");
}

TEST(ExprLowerTest, SizeStringLowersToRuntimeCall) {
  auto L = LowerOk("size('abc')");
  // CEL's `size()` returns int, which is i64 in our ABI.  After Slice F
  // Step 7 the scalar path calls the boxed `_v` helper and unboxes via
  // `cel_int_from_value`, so the body is `cel_int_from_value(cel_string_size_v(...))`.
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
  EXPECT_EQ(L.fn.result_repr, Repr::kInt);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_int_from_value");
  ASSERT_EQ(BinaryenCallGetNumOperands(body), 1u);
  BinaryenExpressionRef inner = BinaryenCallGetOperandAt(body, 0);
  ASSERT_EQ(BinaryenExpressionGetId(inner), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(inner), "cel_string_size_v");
}

// Bytes operators (M3 slice F).  Mirrors the string shape-checks above.
// The bytes literal lowering reuses `LowerSpanLiteral` with the bytes
// constructor, so the body is a block ending in `cel_make_bytes_view`;
// concat and size route through the dedicated bytes runtime helpers.

TEST(ExprLowerTest, BytesConstantReturnsI32) {
  auto L = LowerOk("b'hi'");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
  EXPECT_EQ(L.fn.result_repr, Repr::kBytes);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  ASSERT_NE(fn, nullptr);
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
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
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
  EXPECT_EQ(L.fn.result_repr, Repr::kBytes);
  EXPECT_THAT(L.mod.Validate(), IsOk());
}

TEST(ExprLowerTest, BytesConcatLowersToRuntimeCall) {
  auto L = LowerOk("b'ab' + b'cd'");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
  EXPECT_EQ(L.fn.result_repr, Repr::kBytes);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_bytes_concat_v");
  EXPECT_EQ(BinaryenCallGetNumOperands(body), 2u);
}

TEST(ExprLowerTest, BytesEqualityLowersToRuntimeCall) {
  auto L = LowerOk("b'a' == b'b'");
  EXPECT_EQ(L.fn.result_repr, Repr::kBool);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_bytes_eq_v");
}

TEST(ExprLowerTest, SizeBytesLowersToRuntimeCall) {
  auto L = LowerOk("size(b'abc')");
  EXPECT_EQ(L.fn.result_type, BinaryenTypeNone());
  EXPECT_EQ(L.fn.result_repr, Repr::kInt);
  EXPECT_THAT(L.mod.Validate(), IsOk());
  BinaryenFunctionRef fn = BinaryenGetFunction(L.mod.raw(), "eval");
  BinaryenExpressionRef body = BinaryenFunctionGetBody(fn);
  body = ScalarBody(body);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(body), "cel_int_from_value");
  ASSERT_EQ(BinaryenCallGetNumOperands(body), 1u);
  BinaryenExpressionRef inner = BinaryenCallGetOperandAt(body, 0);
  ASSERT_EQ(BinaryenExpressionGetId(inner), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(inner), "cel_bytes_size_v");
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
