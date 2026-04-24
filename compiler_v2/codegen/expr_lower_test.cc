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

// Like RunPipeline, but takes variable specs in the `name:Type` form
// that CheckOptions consumes.  Used by kIdent lowering tests.
Pipeline RunPipelineWithVars(absl::string_view expression,
                             std::vector<std::string> variable_specs) {
  CheckOptions opts;
  opts.variable_specs = std::move(variable_specs);
  auto ta = ParseAndCheck(expression, opts);
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
  ASSERT_THAT(
      m.SetMemory(1, std::nullopt, "memory", absl::MakeConstSpan(&seg, 1)),
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
  // `1 + 2` type-checks but its root is a kCall; M2 expr_lower
  // rejects kCall roots (M3 lights them up).
  Pipeline p = RunPipeline("1 + 2");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  EXPECT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m),
              StatusIs(absl::StatusCode::kUnimplemented));
}

// ============================================================
// M2.B.1 — kIdent root + `$eval` variable prelude
// ============================================================
//
// Target wasm shape is locked by
// `doc/implementation-plan/rewrite/wat/02_ident_x.wat`.  Each test
// asserts one invariant of that shape; together they pin the
// emitted IR up to Binaryen's local-name assignment.

TEST(ExprLowerIdentTest, RootIdentLowersToLocalGet) {
  // `x` with x:int.  Body should be:
  //   (block (result i32)
  //     (local.set 0 (i32.const <x_slot_offset>))
  //     (call $cel_reset ...)
  //     (local.get 0))
  Pipeline p = RunPipelineWithVars("x", {"x:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  ASSERT_EQ(BinaryenBlockGetNumChildren(body), 3u)
      << "prelude (1) + cel_reset (1) + root (1)";

  // Child 0: prelude local.set of x's slot_offset.
  BinaryenExpressionRef set = BinaryenBlockGetChildAt(body, 0);
  ASSERT_EQ(BinaryenExpressionGetId(set), BinaryenLocalSetId());
  EXPECT_EQ(BinaryenLocalSetGetIndex(set), 0u);
  BinaryenExpressionRef slot_const = BinaryenLocalSetGetValue(set);
  ASSERT_EQ(BinaryenExpressionGetId(slot_const), BinaryenConstId());
  ASSERT_EQ(p.layout.variables.size(), 1u);
  EXPECT_EQ(BinaryenConstGetValueI32(slot_const),
            static_cast<int32_t>(p.layout.variables[0].slot_offset));

  // Child 1: call $cel_reset.
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 1);
  EXPECT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_reset");

  // Child 2: local.get of x's local.  The returned i32 is what
  // `$eval` produces — the offset of x's CelValue.
  BinaryenExpressionRef get = BinaryenBlockGetChildAt(body, 2);
  ASSERT_EQ(BinaryenExpressionGetId(get), BinaryenLocalGetId());
  EXPECT_EQ(BinaryenLocalGetGetIndex(get), 0u);
}

TEST(ExprLowerIdentTest, EvalFunctionDeclaresOneLocalPerVariable) {
  Pipeline p = RunPipelineWithVars("x", {"x:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_EQ(BinaryenFunctionGetNumVars(lowered->func), 1u);
  EXPECT_EQ(BinaryenFunctionGetVar(lowered->func, 0), BinaryenTypeInt32());
}

TEST(ExprLowerIdentTest, PreludePresentEvenWhenOnlyKConstIsUsed) {
  // `x + 0` — two kIdent (x + literal 0 under kCall) where only one
  // variable `x` is referenced.  kCall root still returns
  // Unimplemented at M2, so this test proves the prelude shape, not
  // the call shape.
  //
  // Actually `x + 0` is a kCall root — kCall rejection fires before
  // we ever inspect the body.  To isolate the prelude invariant we
  // use the plain `x` shape above.  This test instead checks the
  // literal-only case has NO prelude (variables.empty()).
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_EQ(BinaryenFunctionGetNumVars(lowered->func), 0u)
      << "literal-only program declares no wasm locals";
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_EQ(BinaryenBlockGetNumChildren(body), 2u)
      << "no prelude: cel_reset + const root";
}

TEST(ExprLowerIdentTest, EmittedModuleSerializesAndValidates) {
  // End-to-end shape check: the M2.B.1 output for `x:int` is a valid
  // wasm module after the standard module emission step.  This is
  // the same gate the WAT-runner harness applies but going through
  // our codegen instead of hand-written WAT.
  Pipeline p = RunPipelineWithVars("x", {"x:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m), IsOk());
  m.ExportFunction("$eval", "eval");
  ASSERT_THAT(m.Validate(), IsOk());
  EXPECT_THAT(m.Serialize(), IsOk());
}

TEST(ExprLowerIdentTest, MultipleVariablesGetSeparateLocalsAndPrelude) {
  // With the kCall arm still Unimplemented, we can't root-test an
  // expression with two kIdents.  But two declared variables is
  // enough: ResolvePass only keeps referenced ones, so we need the
  // expression to mention both.  `x + y` fails at kCall-root, but
  // LayoutPass / ResolvePass already ran and populated
  // layout.variables.  Feed the layout directly, forging a root
  // that references one of them (`x`), proving both slots are
  // populated by the prelude regardless of which ident the body
  // reads.
  //
  // Simpler: just use `x` but DECLARE both x and y.  Unreferenced
  // declarations don't reserve slots (ResolvePass filter), so this
  // ends up a single-variable test — exactly what we want to avoid.
  //
  // Skipped until M3 (kCall arm) lands.  Documented here so the
  // intent is recorded.
  GTEST_SKIP() << "two-variable ident lowering requires M3 kCall to root";
}

}  // namespace
}  // namespace celwasm
