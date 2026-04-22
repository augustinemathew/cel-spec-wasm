#include "compiler_v2/codegen/expr_lower.h"

#include <cstdint>
#include <cstring>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "binaryen-c.h"
#include "compiler_v2/codegen/layout_pass.h"
#include "compiler_v2/codegen/module.h"
#include "compiler_v2/codegen/resolve_pass.h"
#include "compiler_v2/frontend/parse_and_check.h"
#include "compiler_v2/ir/typed_ast.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// Runs parse → check → resolve → layout over `expr` and returns the
// pair (TypedAst, StaticLayout).  Crashes on any pipeline failure —
// the helper is only used by tests that expect a green pipeline; error
// paths are covered separately with manual pipeline invocations.
struct Pipeline {
  TypedAst ast;
  StaticLayout layout;
};
Pipeline RunPipeline(absl::string_view expression) {
  auto ta = ParseAndCheck(expression, {});
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  return Pipeline{*std::move(ta), *std::move(layout)};
}

// Installs the memory + `cel.cel_reset` import shape every lowered
// `$eval` body relies on.  One wasm page and a `(i32,i32)->()` import
// under the internal name `kCelResetInternalName`.
void PrepareHostModule(WasmModule& m, const StaticLayout& layout) {
  std::vector<uint8_t> rodata_copy(layout.rodata);
  WasmModule::DataSegment seg{layout.rodata_base, rodata_copy};
  ASSERT_THAT(m.SetMemory(1, std::nullopt, "memory",
                          absl::MakeConstSpan(&seg, 1)),
              IsOk());
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType params[2] = {i32, i32};
  m.AddFunctionImport(kCelResetInternalName, "cel", "cel_reset", params,
                      BinaryenTypeNone());
}

// --- kConst lowering per scalar kind ------------------------------------

TEST(ExprLowerTest, KConstBoolLowers) {
  Pipeline p = RunPipeline("true");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstIntLowers) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstUintLowers) {
  Pipeline p = RunPipeline("42u");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstDoubleLowers) {
  Pipeline p = RunPipeline("3.14");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstNullLowers) {
  Pipeline p = RunPipeline("null");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstStringLowers) {
  Pipeline p = RunPipeline("\"hi\"");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstBytesLowers) {
  Pipeline p = RunPipeline("b\"x\"");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

// --- Function shape: nullary, returns i32, named --------------------------

TEST(ExprLowerTest, EvalFunctionIsNullaryReturningI32) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_EQ(BinaryenFunctionGetParams(lowered->func), BinaryenTypeNone());
  EXPECT_EQ(BinaryenFunctionGetResults(lowered->func), BinaryenTypeInt32());
}

TEST(ExprLowerTest, EvalFunctionNameIsHonoured) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerToEvalFunction(p.ast, p.layout, "my_eval", m), IsOk());
  EXPECT_NE(BinaryenGetFunction(m.raw(), "my_eval"), nullptr);
}

// --- Body emits (call $cel_reset ...) then (i32.const <rodata_off>) ------

TEST(ExprLowerTest, BodyReturnsRodataOffsetOfRootKConst) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  ASSERT_EQ(BinaryenBlockGetNumChildren(body), 2u);

  // First child: call $cel_reset with two i32.const args.
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 0);
  EXPECT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_reset");
  EXPECT_EQ(BinaryenCallGetNumOperands(call), 2u);

  // Second child: i32.const <root_storage_offset>.
  BinaryenExpressionRef constExpr = BinaryenBlockGetChildAt(body, 1);
  EXPECT_EQ(BinaryenExpressionGetId(constExpr), BinaryenConstId());
  EXPECT_EQ(BinaryenConstGetValueI32(constExpr),
            static_cast<int32_t>(p.layout.rodata_base));
}

// --- Serialised module round-trips ---------------------------------------

TEST(ExprLowerTest, EmittedModuleSerializesSuccessfully) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m), IsOk());
  m.ExportFunction("$eval", "eval");
  ASSERT_THAT(m.Validate(), IsOk());
  auto bytes_or = m.Serialize();
  ASSERT_THAT(bytes_or, IsOk());
  EXPECT_GE(bytes_or->size(), 8u);
}

// --- LoweringOptions: arena_limit argument reflected in the call ---------

TEST(ExprLowerTest, MemSizeBytesFlowsIntoCelResetSecondArg) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  LoweringOptions opts;
  opts.mem_size_bytes = 128u * 1024u;
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m, opts);
  ASSERT_THAT(lowered, IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 0);
  BinaryenExpressionRef arg1 = BinaryenCallGetOperandAt(call, 1);
  EXPECT_EQ(BinaryenConstGetValueI32(arg1),
            static_cast<int32_t>(opts.mem_size_bytes));
}

// --- Unimplemented kinds return UnimplementedError -----------------------

TEST(ExprLowerTest, KCallReturnsUnimplemented) {
  // `1 + 2` type-checks but its root is a kCall; M1's expr_lower
  // rejects any non-kConst root.
  Pipeline p = RunPipeline("1 + 2");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  EXPECT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m),
              StatusIs(absl::StatusCode::kUnimplemented));
}

}  // namespace
}  // namespace celwasm
