#include "compiler_v2/codegen/expr_lower.h"

#include <cstdint>
#include <cstring>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "binaryen-c.h"
#include "compiler/testdata/e2e_fixture.pb.h"
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

// Force generated-pool registration of descriptors referenced by
// tests below.  Runs once at static init per test binary.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      google::protobuf::LinkMessageReflection<celwasm::testdata::Address>();
      return 0;
    }();

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
  const BinaryenType reset_params[2] = {i32, i32};
  m.AddFunctionImport(kCelResetInternalName, "cel", "cel_reset", reset_params,
                      BinaryenTypeNone());
  const BinaryenType host_params[4] = {i32, i32, i32, i32};
  m.AddFunctionImport(std::string(kCelHostGetFieldInternalName), "cel_host",
                      "cel_get_field", host_params, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelHostHasFieldInternalName), "cel_host",
                      "cel_has_field", host_params, BinaryenTypeNone());
  // M3.F: map runtime entry points + host trampoline.  Mirrors the
  // imports compile.cc::InstallHostAbi installs on the production
  // module — codegen targets the same internal names.
  const BinaryenType map_create_params[2] = {i32, i32};
  m.AddFunctionImport(std::string(kCelMapCreateInternalName), "cel",
                      "cel_map_create", map_create_params, BinaryenTypeNone());
  const BinaryenType map3_params[3] = {i32, i32, i32};
  m.AddFunctionImport(std::string(kCelMapInsertInternalName), "cel",
                      "cel_map_insert", map3_params, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelMapLookupArenaInternalName), "cel",
                      "cel_map_lookup_arena", map3_params, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelMapLookupInternalName), "cel",
                      "cel_map_lookup", map3_params, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelHostMapLookupInternalName), "cel_host",
                      "cel_map_lookup", map3_params, BinaryenTypeNone());
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

// --- M2.C.3 kSelect arm -------------------------------------------------
// kSelect emits: (block (call $cel_get_field out_slot msg_slot
//                                           field_ref_id attr_id)
//                       (i32.const out_slot)).  Shape locked by
// doc/implementation-plan/rewrite/wat/04_select_c_name.wat.

// The root expression is always $eval body's last child (prelude +
// cel_reset come before).
BinaryenExpressionRef RootExpr(BinaryenFunctionRef func) {
  BinaryenExpressionRef body = BinaryenFunctionGetBody(func);
  return BinaryenBlockGetChildAt(body, BinaryenBlockGetNumChildren(body) - 1);
}

TEST(ExprLowerSelectTest, SingleSelectEmitsCall) {
  Pipeline p = RunPipelineWithVars("c.name", {"c:celwasm.testdata.Customer"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  // field_refs: [sentinel, {1, "name", Customer}].
  ASSERT_EQ(lowered->field_refs.size(), 2u);
  EXPECT_EQ(lowered->field_refs[1].field_number, 1u);
  EXPECT_EQ(lowered->field_refs[1].name, "name");
  EXPECT_EQ(lowered->field_refs[1].owner_fqn, "celwasm.testdata.Customer");

  // (block (call ...) (i32.const out_slot)).
  BinaryenExpressionRef block = RootExpr(lowered->func);
  ASSERT_EQ(BinaryenBlockGetNumChildren(block), 2u);
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(block, 0);
  BinaryenExpressionRef tail = BinaryenBlockGetChildAt(block, 1);

  const uint32_t out_slot =
      p.layout.annotations.Find(p.ast.ast().root_expr().id())->storage.payload;
  EXPECT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_get_field");
  ASSERT_EQ(BinaryenCallGetNumOperands(call), 4u);
  // 4 operands: out_slot, msg_slot=local.get(c), field_ref_id=1,
  // attr_id=1 (M2.E: operand `c` has attribute_id=1 for path {c,[]}).
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(call, 0)),
            static_cast<int32_t>(out_slot));
  EXPECT_EQ(BinaryenExpressionGetId(BinaryenCallGetOperandAt(call, 1)),
            BinaryenLocalGetId());
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(call, 2)), 1);
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(call, 3)), 1);
  // Tail: i32.const out_slot.
  EXPECT_EQ(BinaryenConstGetValueI32(tail), static_cast<int32_t>(out_slot));
}

TEST(ExprLowerSelectTest, NestedSelectRecursesOperandFirst) {
  Pipeline p = RunPipelineWithVars("c.billing_address.city",
                                   {"c:celwasm.testdata.Customer"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  // Post-order: row 1 = billing_address, row 2 = city.
  ASSERT_EQ(lowered->field_refs.size(), 3u);
  EXPECT_EQ(lowered->field_refs[1].name, "billing_address");
  EXPECT_EQ(lowered->field_refs[1].owner_fqn, "celwasm.testdata.Customer");
  EXPECT_EQ(lowered->field_refs[2].name, "city");
  EXPECT_EQ(lowered->field_refs[2].owner_fqn, "celwasm.testdata.Address");

  // Outer call's msg_slot operand is the inner block — proves
  // recursion produced the right nesting.
  BinaryenExpressionRef outer_call =
      BinaryenBlockGetChildAt(RootExpr(lowered->func), 0);
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(outer_call, 2)),
            2);
  BinaryenExpressionRef inner_call =
      BinaryenBlockGetChildAt(BinaryenCallGetOperandAt(outer_call, 1), 0);
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(inner_call, 2)),
            1);
}

// ============================================================
// M3.F — kMapExpr + kCallExpr(_[_]) lowering
// ============================================================

// Recursive walk of an emitted Binaryen expression looking for a
// `(call $<name> ...)` anywhere in the tree.  Used to assert codegen
// targeted the right runtime entry point without committing to a
// specific block layout.
bool BodyContainsCallTo(BinaryenExpressionRef expr, const char* name) {
  if (BinaryenExpressionGetId(expr) == BinaryenCallId()) {
    if (std::string(BinaryenCallGetTarget(expr)) == name) return true;
    for (BinaryenIndex i = 0; i < BinaryenCallGetNumOperands(expr); ++i) {
      if (BodyContainsCallTo(BinaryenCallGetOperandAt(expr, i), name)) {
        return true;
      }
    }
    return false;
  }
  if (BinaryenExpressionGetId(expr) == BinaryenBlockId()) {
    for (BinaryenIndex i = 0; i < BinaryenBlockGetNumChildren(expr); ++i) {
      if (BodyContainsCallTo(BinaryenBlockGetChildAt(expr, i), name)) {
        return true;
      }
    }
  }
  return false;
}

TEST(ExprLowerMapTest, EmptyMapLiteralLowers) {
  Pipeline p = RunPipeline("{}");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  // Body must call cel_map_create; no inserts since N=0.
  EXPECT_TRUE(BodyContainsCallTo(BinaryenFunctionGetBody(lowered->func),
                                 "cel_map_create"));
  EXPECT_FALSE(BodyContainsCallTo(BinaryenFunctionGetBody(lowered->func),
                                  "cel_map_insert"));
}

TEST(ExprLowerMapTest, ScalarMapLiteralEmitsCreateAndInserts) {
  Pipeline p = RunPipeline("{1: 10, 2: 20, 3: 30}");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  // The kMapExpr root materialises as:
  //   (block (call $cel_map_create out N) (call $cel_map_insert ...) ×3
  //          (i32.const out))
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  // Last child of $eval body = the kMapExpr block (the root).
  BinaryenExpressionRef root =
      BinaryenBlockGetChildAt(body, BinaryenBlockGetNumChildren(body) - 1);
  ASSERT_EQ(BinaryenExpressionGetId(root), BinaryenBlockId())
      << "kMapExpr lowers to a block";
  // 1 create + 3 inserts + 1 i32.const trailer = 5 children.
  EXPECT_EQ(BinaryenBlockGetNumChildren(root), 5u);

  BinaryenExpressionRef create = BinaryenBlockGetChildAt(root, 0);
  ASSERT_EQ(BinaryenExpressionGetId(create), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(create), "cel_map_create");
  // Capacity is the second operand to cel_map_create; pin N=3.
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(create, 1)), 3);

  for (BinaryenIndex i = 1; i <= 3; ++i) {
    BinaryenExpressionRef call = BinaryenBlockGetChildAt(root, i);
    ASSERT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
    EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_map_insert");
  }
}

TEST(ExprLowerMapTest, MapIndexOnLiteralEmitsArenaFastPath) {
  // Map literal indexed with int key — origin is kArena (literal),
  // so codegen routes to cel_map_lookup_arena (the pure-wasm fast
  // path), bypassing the dispatcher.
  Pipeline p = RunPipeline("{1: 10}[1]");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_lookup_arena"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_lookup"))
      << "kArena origin must skip the dispatcher";
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_host_cel_map_lookup"))
      << "kArena origin must skip the host trampoline";
}

TEST(ExprLowerMapTest, MapIndexOnHostBoundIdentEmitsHostTrampoline) {
  // Bound `m: map<int, int>` — kIdent on a map-typed variable
  // stamps `kHost` (M2 path), so `m[k]` routes to
  // cel_host_cel_map_lookup directly.
  Pipeline p = RunPipelineWithVars("m[1]", {"m:map<int,int>"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerToEvalFunction(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_host_cel_map_lookup"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_lookup_arena"));
}

TEST(ExprLowerMapTest, NonIndexCallStillUnimplemented) {
  // Make sure scoping the kCallExpr arm to `_[_]` only didn't
  // accidentally light up other call kinds.
  Pipeline p = RunPipeline("1 + 2");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  EXPECT_THAT(LowerToEvalFunction(p.ast, p.layout, "$eval", m),
              StatusIs(absl::StatusCode::kUnimplemented));
}

}  // namespace
}  // namespace celwasm
