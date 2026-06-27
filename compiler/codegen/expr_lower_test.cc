#include "compiler/codegen/expr_lower.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "binaryen-c.h"
#include "compiler/codegen/layout_pass.h"
#include "compiler/codegen/module.h"
#include "compiler/codegen/overload_table.h"
#include "compiler/codegen/resolve_pass.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/ir/typed_ast.h"
#include "gtest/gtest.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Forward decl — body lives below at the first call site that
// originally needed it (`ExprLowerMapTest`'s indexing tests).
bool BodyContainsCallTo(BinaryenExpressionRef expr, const char* name);
bool CallTargetMatches(BinaryenExpressionRef expr, const char* name);

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

// Per-test OverloadTable.  Tests that don't need customs reuse the
// default-built table (built-in seeds only).
OverloadTable DefaultOverloadTable() {
  return OverloadTable::Build().value();
}

// Convenience: lower the pipeline through `LowerToEvalFunction` with
// a default-seeded OverloadTable.  Most tests don't care about the
// table contents past "the built-ins resolve".
absl::StatusOr<LoweredFunction> LowerWithDefaultOverloads(
    const TypedAst& ast, const StaticLayout& layout,
    absl::string_view func_name, WasmModule& mod,
    const LoweringOptions& opts = {}) {
  static const auto* const kTable = new OverloadTable(DefaultOverloadTable());
  return LowerToEvalFunction(ast, layout, func_name, mod, *kTable, opts);
}

// Install one wasm import per built-in `cel_*` helper in the
// default OverloadTable that ships a runtime export today (mirror
// of `compile.cc::InstallOverloadImports`).  The unit-test
// fixtures reach codegen without going through Compile(), so we
// have to redo the install or `BinaryenValidate` will reject.
bool IsPendingOverloadImpl(absl::string_view n) {
  static constexpr absl::string_view kPending[] = {
      "cel_list_size", "cel_list_in", "cel_list_eq", "cel_list_concat",
      "cel_map_size",  "cel_map_in",  "cel_map_eq",
  };
  return std::any_of(std::begin(kPending), std::end(kPending),
                     [&](absl::string_view p) {
                       return p == n;
                     });
}

void InstallOneOverloadImport(WasmModule& m, const std::string& name) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType vv_params[3] = {i32, i32, i32};
  const BinaryenType v_params[2] = {i32, i32};
  const BinaryenType nullary_params[1] = {i32};
  const bool is_vv =
      name.size() >= 6 && name.substr(name.size() - 6) == "_at_vv";
  const bool is_v =
      !is_vv && name.size() >= 5 && name.substr(name.size() - 5) == "_at_v";
  const bool is_three_arg = name == "cel_and" || name == "cel_or";
  const bool is_two_arg = name == "cel_not";
  // `cel_optional_none_at` is the only constructor whose name ends in
  // bare `_at` (zero value-slots in addition to the out_slot); the
  // suffix heuristic above would miss it.
  const bool is_nullary = name == "cel_optional_none_at";
  if (is_vv || is_three_arg) {
    m.AddFunctionImport(name, "cel", name, vv_params, BinaryenTypeNone());
  } else if (is_v || is_two_arg) {
    m.AddFunctionImport(name, "cel", name, v_params, BinaryenTypeNone());
  } else if (is_nullary) {
    m.AddFunctionImport(name, "cel", name, nullary_params, BinaryenTypeNone());
  }
}

void InstallOverloadImportsForTest(WasmModule& m) {
  static const auto* const kTable = new OverloadTable(DefaultOverloadTable());
  std::vector<std::string> seen;
  for (const OverloadDef& impl : kTable->impls()) {
    if (impl.wasm_import_module_type != ImportModuleSource::kCel) continue;
    const std::string name(impl.wasm_import_function_name);
    if (std::any_of(seen.begin(), seen.end(), [&](const std::string& s) {
          return s == name;
        })) {
      continue;
    }
    if (IsPendingOverloadImpl(impl.wasm_import_function_name)) continue;
    InstallOneOverloadImport(m, name);
    seen.push_back(name);
  }
  const BinaryenType v_params[2] = {BinaryenTypeInt32(), BinaryenTypeInt32()};
  m.AddFunctionImport("cel_copy_slot", "cel", "cel_copy_slot", v_params,
                      BinaryenTypeNone());
}

// Installs the memory + `cel.arena_reset` import shape every lowered
// `$eval` body relies on.  One wasm page and a `()->()` import under
// the internal name `kArenaResetInternalName`.
void InstallMapImports(WasmModule& m) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType map2[2] = {i32, i32};
  const BinaryenType map3[3] = {i32, i32, i32};
  const BinaryenType map4[4] = {i32, i32, i32, i32};
  m.AddFunctionImport(std::string(kCelMapCreateInternalName), "cel",
                      "cel_map_create", map2, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelMapInsertInternalName), "cel",
                      "cel_map_insert", map3, BinaryenTypeNone());
  const BinaryenType map1[1] = {i32};
  m.AddFunctionImport(std::string(kCelMapIndexBuildInternalName), "cel",
                      "cel_map_index_build", map1, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelMapLookupArenaInternalName), "cel",
                      "cel_map_lookup_arena", map3, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelMapLookupInternalName), "cel",
                      "cel_map_lookup", map3, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelHostMapLookupInternalName), "cel_host",
                      "cel_map_lookup", map3, BinaryenTypeNone());
  m.AddFunctionImport("cel_map_insert_at", "cel", "cel_map_insert_at", map3,
                      BinaryenTypeNone());
  m.AddFunctionImport("cel_map_insert_at_if_bool", "cel",
                      "cel_map_insert_at_if_bool", map4, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelMapInsertAtIfPresentInternalName), "cel",
                      "cel_map_insert_at_if_present", map3, BinaryenTypeNone());
  const BinaryenType iter1[1] = {i32};
  m.AddFunctionImport("cel_map_iter_init", "cel", "cel_map_iter_init", iter1,
                      BinaryenTypeInt32());
  m.AddFunctionImport("cel_map_iter_next", "cel", "cel_map_iter_next", iter1,
                      BinaryenTypeInt32());
  m.AddFunctionImport("cel_map_iter_key_at", "cel", "cel_map_iter_key_at", map2,
                      BinaryenTypeNone());
  m.AddFunctionImport("cel_map_iter_value_at", "cel", "cel_map_iter_value_at",
                      map2, BinaryenTypeNone());
  m.AddFunctionImport("cel_map_count", "cel", "cel_map_count", iter1,
                      BinaryenTypeInt32());
}

void InstallListImports(WasmModule& m) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType list2[2] = {i32, i32};
  const BinaryenType list3[3] = {i32, i32, i32};
  m.AddFunctionImport(std::string(kCelListCreateInternalName), "cel",
                      "cel_list_create", list2, BinaryenTypeNone());
  m.AddFunctionImport("cel_list_append_at", "cel", "cel_list_append_at", list2,
                      BinaryenTypeNone());
  m.AddFunctionImport("cel_list_append_at_if_bool", "cel",
                      "cel_list_append_at_if_bool", list3, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelListAppendAtIfPresentInternalName), "cel",
                      "cel_list_append_at_if_present", list2,
                      BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelListAtArenaInternalName), "cel",
                      "cel_list_at_arena", list3, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelListAtInternalName), "cel", "cel_list_at",
                      list3, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelHostListAtInternalName), "cel_host",
                      "cel_list_at", list3, BinaryenTypeNone());
  const BinaryenType list1[1] = {i32};
  m.AddFunctionImport("cel_list_arena_view", "cel", "cel_list_arena_view",
                      list1, BinaryenTypeInt32());
}

void PrepareHostModule(WasmModule& m, const StaticLayout& layout) {
  std::vector<uint8_t> rodata_copy(layout.rodata);
  WasmModule::DataSegment seg{layout.rodata_base, rodata_copy};
  ASSERT_THAT(
      m.SetMemory(1, std::nullopt, "memory", absl::MakeConstSpan(&seg, 1)),
      IsOk());
  const BinaryenType i32 = BinaryenTypeInt32();
  m.AddFunctionImport(kArenaResetInternalName, "cel", "arena_reset",
                      absl::Span<const BinaryenType>{}, BinaryenTypeNone());
  const BinaryenType host_params[4] = {i32, i32, i32, i32};
  m.AddFunctionImport(std::string(kCelHostGetFieldInternalName), "cel_host",
                      "cel_get_field", host_params, BinaryenTypeNone());
  m.AddFunctionImport(std::string(kCelHostHasFieldInternalName), "cel_host",
                      "cel_has_field", host_params, BinaryenTypeNone());
  const BinaryenType set3[3] = {i32, i32, i32};
  m.AddFunctionImport(std::string(kCelSetFieldAtIfPresentInternalName), "cel",
                      "cel_set_field_at_if_present", set3, BinaryenTypeNone());
  InstallMapImports(m);
  InstallListImports(m);
  // M5.F: every kCelRuntime helper in the OverloadTable that
  // ships a runtime export today.  Mirrors compile.cc.
  InstallOverloadImportsForTest(m);
}

// --- kConst lowering per scalar kind ------------------------------------

TEST(ExprLowerTest, KConstBoolLowers) {
  Pipeline p = RunPipeline("true");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstIntLowers) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstUintLowers) {
  Pipeline p = RunPipeline("42u");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstDoubleLowers) {
  Pipeline p = RunPipeline("3.14");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstNullLowers) {
  Pipeline p = RunPipeline("null");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstStringLowers) {
  Pipeline p = RunPipeline("\"hi\"");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerTest, KConstBytesLowers) {
  Pipeline p = RunPipeline("b\"x\"");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m), IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

// --- Function shape: nullary, returns i32, named --------------------------

TEST(ExprLowerTest, EvalFunctionIsNullaryReturningI32) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_EQ(BinaryenFunctionGetParams(lowered->func), BinaryenTypeNone());
  EXPECT_EQ(BinaryenFunctionGetResults(lowered->func), BinaryenTypeInt32());
}

TEST(ExprLowerTest, EvalFunctionNameIsHonoured) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerWithDefaultOverloads(p.ast, p.layout, "my_eval", m), IsOk());
  EXPECT_NE(BinaryenGetFunction(m.raw(), "my_eval"), nullptr);
}

// --- Body emits (call $arena_reset) then (i32.const <rodata_off>) -------

TEST(ExprLowerTest, BodyReturnsRodataOffsetOfRootKConst) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  ASSERT_EQ(BinaryenBlockGetNumChildren(body), 2u);

  // First child: call $arena_reset (no args).
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 0);
  EXPECT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(call), "arena_reset");
  EXPECT_EQ(BinaryenCallGetNumOperands(call), 0u);

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
  ASSERT_THAT(LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m), IsOk());
  m.ExportFunction("$eval", "eval");
  ASSERT_THAT(m.Validate(), IsOk());
  auto bytes_or = m.Serialize();
  ASSERT_THAT(bytes_or, IsOk());
  EXPECT_GE(bytes_or->size(), 8u);
}

// Post-M5: the eval prologue is `(call $arena_reset)` with zero
// arguments — LoweringOptions::mem_size_bytes no longer threads
// into codegen (the bump cursor lives in BSS, not linear memory).
// CompileOptions::mem_size_bytes still controls the host's memory
// import; see `MemSizeBytesLargerThanOnePageGrowsPageCount` in
// compile_test.cc.
TEST(ExprLowerTest, EvalPrologueIsZeroArgArenaReset) {
  Pipeline p = RunPipeline("42");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 0);
  ASSERT_STREQ(BinaryenCallGetTarget(call), "arena_reset");
  EXPECT_EQ(BinaryenCallGetNumOperands(call), 0u);
}

// --- Unimplemented kinds return UnimplementedError -----------------------

// M5.G (Slice 2) — `_&&_` lowers to a slot-out call into `cel_and`.
// The runtime owns the 3VL truth table; codegen just routes
// operands and the result slot.  Originally a pending-Unimplemented
// fixture; rewritten as positive coverage at the M5.G enabling
// commit (kept under the same TEST name historically — see git blame).
TEST(ExprLowerTest, KCallLogicalAndLowersToHelper) {
  Pipeline p = RunPipeline("true && false");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_and"));
}

TEST(ExprLowerTest, KCallLogicalNotLowersToHelper) {
  Pipeline p = RunPipeline("!true");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_not"));
}

// Slice 1.5 (dyn-passthrough-plan.md): `dyn(scalar)` is the
// identity function at codegen time.  `dyn(1) == 1u` lowers to a
// single `cel_equals_at_vv` call whose operands are the rodata
// slots of `1` and `1u` directly — no helper function exists for
// `dyn(...)` and none should be emitted.
TEST(ExprLowerTest, KCallDynPassthroughEmitsArgumentSlot) {
  Pipeline p = RunPipeline("dyn(1) == 1u");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_equals_at_vv"));
  // Sanity: there is no runtime helper for `dyn(...)` — the
  // common typo / placeholder names should never appear.
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_to_dyn"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_dyn"));
}

// Slice 1.6 — codegen-time cross-numeric ordering re-pick.  cel-cpp's
// reference_map for `dyn(int) < uint` lists exactly one candidate
// (`less_uint64`, the same-kind overload of the non-dyn operand),
// which would route to `cel_uint_lt_at_vv` and reject the int
// operand as TYPE_MISMATCH.  `MaybeRepickCrossNumericOverload` in
// `expr_lower.cc` overrides the cel-cpp pick with the cross-numeric
// id (`less_int64_uint64`), routing the call to
// `cel_numeric_lt_at_vv`.
TEST(ExprLowerTest, KCallCrossNumericOrderingRepicksToNumericKernel) {
  Pipeline p = RunPipeline("dyn(1) < 2u");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  // The re-pick must route through the cross-numeric kernel.
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_numeric_lt_at_vv"));
  // And NOT the same-kind helper cel-cpp's reference_map originally
  // pointed at (`less_uint64` → `cel_uint_lt_at_vv`), which would
  // type-mismatch on the int operand.
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_uint_lt_at_vv"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_int_lt_at_vv"));
}

// Same-kind ordering must still take the per-kind fast path
// (`cel_int_lt_at_vv`) — the re-pick triggers only when operand
// Reprs span numeric kinds.  Pre-Slice-1.6 regression guard.
TEST(ExprLowerTest, KCallSameKindOrderingKeepsPerKindHelper) {
  Pipeline p = RunPipeline("1 < 2");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_int_lt_at_vv"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_numeric_lt_at_vv"));
}

// `dyn(1.0) >= 2u` must re-pick the cross-numeric `_>=_` helper.
// Covers a different op-arm to confirm the re-pick predicate
// generalises across the four ordering ops.
TEST(ExprLowerTest, KCallCrossNumericGeRepicksToNumericKernel) {
  Pipeline p = RunPipeline("dyn(1.0) >= 2u");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_numeric_ge_at_vv"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_uint_ge_at_vv"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_double_ge_at_vv"));
}

// `_?_:_` lowers to a BinaryenIf-based shape (only the chosen arm
// is evaluated, per langdef §"Conditional expression").  The body
// imports `cel_copy_slot` to materialise the chosen arm into the
// expression's out_slot, NOT a `cel_conditional` runtime helper.
TEST(ExprLowerTest, KCallConditionalLowersToBranchedIf) {
  Pipeline p = RunPipeline("true ? 1 : 2");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_copy_slot"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_conditional"));
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
  //     (call $arena_reset ...)
  //     (local.get 0))
  Pipeline p = RunPipelineWithVars("x", {"x:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  ASSERT_EQ(BinaryenBlockGetNumChildren(body), 3u)
      << "prelude (1) + arena_reset (1) + root (1)";

  // Child 0: prelude local.set of x's slot_offset.
  BinaryenExpressionRef set = BinaryenBlockGetChildAt(body, 0);
  ASSERT_EQ(BinaryenExpressionGetId(set), BinaryenLocalSetId());
  EXPECT_EQ(BinaryenLocalSetGetIndex(set), 0u);
  BinaryenExpressionRef slot_const = BinaryenLocalSetGetValue(set);
  ASSERT_EQ(BinaryenExpressionGetId(slot_const), BinaryenConstId());
  ASSERT_EQ(p.layout.variables.size(), 1u);
  EXPECT_EQ(BinaryenConstGetValueI32(slot_const),
            static_cast<int32_t>(p.layout.variables[0].slot_offset));

  // Child 1: call $arena_reset.
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 1);
  EXPECT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(call), "arena_reset");

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
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
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
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_EQ(BinaryenFunctionGetNumVars(lowered->func), 0u)
      << "literal-only program declares no wasm locals";
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_EQ(BinaryenBlockGetNumChildren(body), 2u)
      << "no prelude: arena_reset + const root";
}

TEST(ExprLowerIdentTest, EmittedModuleSerializesAndValidates) {
  // End-to-end shape check: the M2.B.1 output for `x:int` is a valid
  // wasm module after the standard module emission step.  This is
  // the same gate the WAT-runner harness applies but going through
  // our codegen instead of hand-written WAT.
  Pipeline p = RunPipelineWithVars("x", {"x:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m), IsOk());
  m.ExportFunction("$eval", "eval");
  ASSERT_THAT(m.Validate(), IsOk());
  EXPECT_THAT(m.Serialize(), IsOk());
}

TEST(ExprLowerIdentTest, MultipleVariablesGetSeparateLocalsAndPrelude) {
  // `x + y` — two referenced variables.  The prelude must populate
  // one local per variable, each holding that variable's own slot
  // offset; the slots must be distinct.
  Pipeline p = RunPipelineWithVars("x + y", {"x:int", "y:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  ASSERT_EQ(p.layout.variables.size(), 2u);
  ASSERT_GE(BinaryenFunctionGetNumVars(lowered->func), 2u)
      << "one wasm local per referenced variable";
  EXPECT_EQ(BinaryenFunctionGetVar(lowered->func, 0), BinaryenTypeInt32());
  EXPECT_EQ(BinaryenFunctionGetVar(lowered->func, 1), BinaryenTypeInt32());

  // Prelude: children 0 and 1 are local.set of each variable's own
  // slot offset.
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  ASSERT_EQ(BinaryenExpressionGetId(body), BinaryenBlockId());
  for (uint32_t i = 0; i < 2; ++i) {
    BinaryenExpressionRef set = BinaryenBlockGetChildAt(body, i);
    ASSERT_EQ(BinaryenExpressionGetId(set), BinaryenLocalSetId());
    EXPECT_EQ(BinaryenLocalSetGetIndex(set), i);
    BinaryenExpressionRef slot_const = BinaryenLocalSetGetValue(set);
    ASSERT_EQ(BinaryenExpressionGetId(slot_const), BinaryenConstId());
    EXPECT_EQ(BinaryenConstGetValueI32(slot_const),
              static_cast<int32_t>(p.layout.variables[i].slot_offset));
  }
  EXPECT_NE(p.layout.variables[0].slot_offset,
            p.layout.variables[1].slot_offset);
}

// --- M2.C.3 kSelect arm -------------------------------------------------
// kSelect emits: (block (call $cel_get_field out_slot msg_slot
//                                           field_ref_id attr_id)
//                       (i32.const out_slot)).  Shape locked by
// doc/implementation-plan/rewrite/wat/04_select_c_name.wat.

// The root expression is always $eval body's last child (prelude +
// arena_reset come before).
BinaryenExpressionRef RootExpr(BinaryenFunctionRef func) {
  BinaryenExpressionRef body = BinaryenFunctionGetBody(func);
  return BinaryenBlockGetChildAt(body, BinaryenBlockGetNumChildren(body) - 1);
}

TEST(ExprLowerSelectTest, SingleSelectEmitsCall) {
  Pipeline p = RunPipelineWithVars("c.name", {"c:celwasm.testdata.Customer"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
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
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
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
bool CallTargetMatches(BinaryenExpressionRef expr, const char* name) {
  if (std::string(BinaryenCallGetTarget(expr)) == name) return true;
  for (BinaryenIndex i = 0; i < BinaryenCallGetNumOperands(expr); ++i) {
    if (BodyContainsCallTo(BinaryenCallGetOperandAt(expr, i), name)) {
      return true;
    }
  }
  return false;
}

bool BlockContainsCall(BinaryenExpressionRef expr, const char* name) {
  for (BinaryenIndex i = 0; i < BinaryenBlockGetNumChildren(expr); ++i) {
    if (BodyContainsCallTo(BinaryenBlockGetChildAt(expr, i), name)) return true;
  }
  return false;
}

bool IfContainsCall(BinaryenExpressionRef expr, const char* name) {
  if (BodyContainsCallTo(BinaryenIfGetCondition(expr), name)) return true;
  if (BodyContainsCallTo(BinaryenIfGetIfTrue(expr), name)) return true;
  BinaryenExpressionRef if_false = BinaryenIfGetIfFalse(expr);
  return if_false != nullptr && BodyContainsCallTo(if_false, name);
}

bool BreakContainsCall(BinaryenExpressionRef expr, const char* name) {
  BinaryenExpressionRef v = BinaryenBreakGetValue(expr);
  if (v != nullptr && BodyContainsCallTo(v, name)) return true;
  BinaryenExpressionRef c = BinaryenBreakGetCondition(expr);
  return c != nullptr && BodyContainsCallTo(c, name);
}

bool BodyContainsCallTo(BinaryenExpressionRef expr, const char* name) {
  const BinaryenExpressionId id = BinaryenExpressionGetId(expr);
  if (id == BinaryenCallId()) return CallTargetMatches(expr, name);
  if (id == BinaryenBlockId()) return BlockContainsCall(expr, name);
  if (id == BinaryenIfId()) return IfContainsCall(expr, name);
  if (id == BinaryenDropId()) {
    return BodyContainsCallTo(BinaryenDropGetValue(expr), name);
  }
  if (id == BinaryenLoopId()) {
    return BodyContainsCallTo(BinaryenLoopGetBody(expr), name);
  }
  if (id == BinaryenBreakId()) return BreakContainsCall(expr, name);
  if (id == BinaryenLocalSetId()) {
    return BodyContainsCallTo(BinaryenLocalSetGetValue(expr), name);
  }
  return false;
}

// M5.A removed direct unit coverage of N=0 map codegen because bare
// `{}` is now rejected by the static-subset gate (types as
// `map<dyn, dyn>`).  The N=0 codegen path stays — it's exercised
// indirectly by M5.I comprehension lowering (`accu_init = {}` after
// macro expansion).  When M5.I lands, add an internal-AST test here
// that constructs a `kMapExpr` with zero entries and asserts
// `cel_map_create` is emitted with no `cel_map_insert`.

TEST(ExprLowerMapTest, ScalarMapLiteralEmitsCreateAndInserts) {
  // A map with a non-constant value (variable `a`) is ineligible for
  // const materialization and keeps the per-Eval build path
  // (cel_map_create + inserts + terminal cel_map_index_build).
  Pipeline p = RunPipelineWithVars("{1: a, 2: 20, 3: 30}", {"a:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  // The kMapExpr root materialises as:
  //   (block (call $cel_map_create out N) (call $cel_map_insert ...) ×3
  //          (call $cel_map_index_build out) (i32.const out))
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  // Last child of $eval body = the kMapExpr block (the root).
  BinaryenExpressionRef root =
      BinaryenBlockGetChildAt(body, BinaryenBlockGetNumChildren(body) - 1);
  ASSERT_EQ(BinaryenExpressionGetId(root), BinaryenBlockId())
      << "kMapExpr lowers to a block";
  // 1 create + 3 inserts + 1 index_build + 1 i32.const trailer = 6.
  EXPECT_EQ(BinaryenBlockGetNumChildren(root), 6u);

  BinaryenExpressionRef create = BinaryenBlockGetChildAt(root, 0);
  ASSERT_EQ(BinaryenExpressionGetId(create), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(create), "cel_map_create");
  // Capacity is the second operand to cel_map_create; pin N=3.
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(create, 1)), 3);

  // `++i` increments via the for-clause; clang-tidy mistakes this
  // for an infinite loop because `BinaryenIndex` is an opaque alias
  // for `uint32_t`.
  for (BinaryenIndex i = 1; i <= 3; ++i) {  // NOLINT(bugprone-infinite-loop)
    BinaryenExpressionRef call = BinaryenBlockGetChildAt(root, i);
    ASSERT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
    EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_map_insert");
  }

  // The terminal map-construction step: cel_map_index_build, emitted
  // after the last insert and before the i32.const trailer.  Its sole
  // operand is the map's out_slot (same as cel_map_create's first).
  BinaryenExpressionRef index_build = BinaryenBlockGetChildAt(root, 4);
  ASSERT_EQ(BinaryenExpressionGetId(index_build), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(index_build), "cel_map_index_build");
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(index_build, 0)),
            BinaryenConstGetValueI32(BinaryenCallGetOperandAt(create, 0)))
      << "index_build must target the same slot the map was built into";
}

TEST(ExprLowerMapTest, ConstMapLiteralLowersToI32ConstNoBuild) {
  // An all-constant map is materialized into rodata (header + 48-B entry
  // run + baked index), so the kMapExpr lowers to a single i32.const of
  // its frame offset — no cel_map_create, no per-entry inserts, no
  // terminal cel_map_index_build.
  Pipeline p = RunPipeline("{1: 10, 2: 20, 3: 30}");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_create"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_insert"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_index_build"));
  // The root expression is a bare i32.const (the rodata frame offset).
  BinaryenExpressionRef root =
      BinaryenBlockGetChildAt(body, BinaryenBlockGetNumChildren(body) - 1);
  EXPECT_EQ(BinaryenExpressionGetId(root), BinaryenConstId());
}

TEST(ExprLowerMapTest, ConstMapNestedLowersToI32Const) {
  // Nested all-const map: inner materializes innermost-first and embeds in
  // the outer entry run; the outer also lowers to a single i32.const.
  Pipeline p = RunPipeline("{1: {2: 30}}");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_create"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_index_build"));
  BinaryenExpressionRef root =
      BinaryenBlockGetChildAt(body, BinaryenBlockGetNumChildren(body) - 1);
  EXPECT_EQ(BinaryenExpressionGetId(root), BinaryenConstId());
}

TEST(ExprLowerMapTest, ConstMapInsideNonConstListInnerStillMaterializes) {
  // `[{1: 10}, {2: a}]`: the first map is const (materializes to a frame),
  // the second has an ident value so it builds per-Eval; the outer list is
  // therefore non-const and builds per-Eval too.  The const inner map must
  // NOT emit a cel_map_create for itself — only the non-const inner map
  // does.  So exactly ONE cel_map_create appears (for `{2: a}`), plus the
  // list build.
  Pipeline p = RunPipelineWithVars("[{1: 10}, {2: a}]", {"a:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  // The const inner map is materialized; only the non-const inner map and
  // the outer list build per-Eval.
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_create"));
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_list_create"));
}

TEST(ExprLowerMapTest, MapWithIdentValueKeepsBuildSequence) {
  // A single ident value disqualifies the whole map from materialization;
  // it keeps the cel_map_create + insert + index_build build sequence.
  Pipeline p = RunPipelineWithVars("{1: a}", {"a:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_create"));
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_index_build"));
}

TEST(ExprLowerMapTest, MapWithComprehensionValueKeepsBuildSequence) {
  // A comprehension value (`[1,2,3].map(x, x)`) is not a compile-time
  // constant — IsConstMaterializable rejects comprehension nodes — so the
  // whole map keeps the per-Eval build path: cel_map_create + the terminal
  // cel_map_index_build, NOT a bare i32.const.
  Pipeline p = RunPipeline("{1: [1, 2, 3].map(x, x)}");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_create"));
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_index_build"));
}

TEST(ExprLowerMapTest, MapWithCallValueKeepsBuildSequence) {
  // A call value (`1 + 1`) is syntactically a kCallExpr, which
  // IsConstMaterializable rejects (no compile-time eval), so the map keeps
  // the build path: cel_map_create is emitted, NOT a bare i32.const.
  Pipeline p = RunPipeline("{1: 1 + 1}");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_create"));
}

TEST(ExprLowerMapTest, MapIndexOnLiteralEmitsArenaFastPath) {
  // Materialized map literal indexed with int key — origin is kArena, so
  // codegen routes to cel_map_lookup_arena (the pure-wasm fast path),
  // bypassing the dispatcher.  The map operand is a single i32.const
  // (materialized) rather than a build block.
  Pipeline p = RunPipeline("{1: 10}[1]");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
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
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_host_cel_map_lookup"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_lookup_arena"));
}

// M5.G (Slice 2) — `_||_` symmetric to `_&&_`.  Rewritten from
// pending-Unimplemented at the M5.G enabling commit (kept under the
// same fixture historically).
TEST(ExprLowerMapTest, KCallLogicalOrLowersToHelper) {
  Pipeline p = RunPipeline("true || false");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_or"));
}

// --------------------------------------------------------------
// M4.F — kListExpr + kCallExpr(`_[_]`) on lists
// --------------------------------------------------------------

TEST(ExprLowerListTest, ConstListLiteralLowersToI32ConstNoBuild) {
  // An all-constant list is materialized into rodata, so the kListExpr
  // lowers to a single i32.const of its frame offset — no
  // cel_list_create, no per-element appends.
  Pipeline p = RunPipeline("[10, 20, 30]");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_list_create"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_list_append_at"));
  // The root expression is a bare i32.const (the rodata frame offset).
  BinaryenExpressionRef root =
      BinaryenBlockGetChildAt(body, BinaryenBlockGetNumChildren(body) - 1);
  EXPECT_EQ(BinaryenExpressionGetId(root), BinaryenConstId());
}

TEST(ExprLowerListTest, NonConstListLiteralEmitsCreateAndAppends) {
  // A list with a non-constant element (variable `a`) is ineligible for
  // materialization and keeps the per-Eval build path.
  Pipeline p = RunPipelineWithVars("[a, 20, 30]", {"a:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  // The kListExpr root lowers as:
  //   (block (call $cel_list_create out N)        ;; capacity=N, count=0
  //          (call $cel_list_append_at out e0)
  //          (call $cel_list_append_at out e1)
  //          (call $cel_list_append_at out e2)
  //          (i32.const out))
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  BinaryenExpressionRef root =
      BinaryenBlockGetChildAt(body, BinaryenBlockGetNumChildren(body) - 1);
  ASSERT_EQ(BinaryenExpressionGetId(root), BinaryenBlockId())
      << "kListExpr lowers to a block";
  // 1 create + 3 appends + 1 i32.const trailer = 5 children.
  EXPECT_EQ(BinaryenBlockGetNumChildren(root), 5u);

  BinaryenExpressionRef create = BinaryenBlockGetChildAt(root, 0);
  ASSERT_EQ(BinaryenExpressionGetId(create), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(create), "cel_list_create");
  // capacity is the second arg — pin N=3.
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(create, 1)), 3);

  for (BinaryenIndex i = 1; i <= 3; ++i) {  // NOLINT(bugprone-infinite-loop)
    BinaryenExpressionRef call = BinaryenBlockGetChildAt(root, i);
    ASSERT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
    EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_list_append_at");
  }
}

TEST(ExprLowerListTest, ListIndexOnLiteralEmitsArenaFastPath) {
  // List literal indexed by int — origin is kArena (literal),
  // codegen routes to cel_list_at_arena (pure-wasm fast path).
  Pipeline p = RunPipeline("[1, 2, 3][1]");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_list_at_arena"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_list_at"))
      << "kArena origin must skip the dispatcher";
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_host_cel_list_at"))
      << "kArena origin must skip the host trampoline";
}

TEST(ExprLowerListTest, ListIndexOnHostBoundIdentEmitsHostTrampoline) {
  // Bound `xs: list<int>` — kIdent on a list-typed variable
  // stamps `kHost`, so `xs[0]` routes to cel_host_cel_list_at.
  Pipeline p = RunPipelineWithVars("xs[0]", {"xs:list<int>"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_host_cel_list_at"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_list_at_arena"));
}

// ============================================================
// M5.B comprehensions — codegen-IR shape assertions.
// ============================================================
//
// First entries in the §9.5 codegen-IR test surface tracked in
// `m5b-comprehensions-simplification.md` — added as each
// simplification lands.  Pattern mirrors the kList/kMap literal
// tests above: compile source → walk Binaryen IR → assert tree
// shape, no wasm execution.

// Post-IsShapeC-removal (commit TBD): `cel.bind(x, V, body)` no
// longer takes a streamlined no-loop path.  It goes through the
// generic comprehension lowering — iter_range = `[]` produces an
// empty list, the loop scaffold runs zero iterations, and the
// result expression is the body.  This test locks the shape so a
// future regression that accidentally re-introduces ShapeC (or
// that breaks the kLocal-storage handling in
// `EmitCompLoopStep`'s generic fallback) is caught at codegen-IR
// time rather than at e2e or conformance time.
TEST(ExprLowerComprehensionTest, CelBindLowersThroughGenericPath) {
  Pipeline p = RunPipeline("cel.bind(x, 5, x + 1)");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  // The empty iter_range literal `[]` is materialized into rodata, so
  // no cel_list_create is emitted for it; the generic comprehension
  // scaffold still drives the (zero-iteration) loop and the body
  // arithmetic (`x + 1`) lowers normally.
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_list_create"));
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_int_add_at_vv"));
}

// One codegen-IR shape assertion per LoopStepShape::Kind.  Each test
// compiles a comprehension that the classifier maps to a specific
// kind and verifies the dispatched emitter's helper-call signature.
// Locks the {classifier kind → emitter → runtime helper} chain so a
// regression in any link bisects to one of these tests.

TEST(ExprLowerComprehensionTest, MapMacroEmitsListAppendAt) {
  // `map(v, t)` → kListAppend → cel_list_append_at.
  Pipeline p = RunPipeline("[1, 2, 3].map(v, v * 2)");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_list_append_at"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_list_append_at_if_bool"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_insert_at"));
  // List-producing comprehensions have no map accu, so no index build.
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_index_build"))
      << "list-producing comprehension must not emit cel_map_index_build";
}

TEST(ExprLowerComprehensionTest, FilterMacroEmitsListAppendAtIfBool) {
  // `filter(v, p)` → kListAppendIf → cel_list_append_at_if_bool.
  Pipeline p = RunPipeline("[1, 2, 3].filter(v, v > 1)");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_list_append_at_if_bool"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_insert_at"));
}

TEST(ExprLowerComprehensionTest, TransformMap3ArgEmitsMapInsertAt) {
  // `transformMap(k, v, t)` → kMapInsert → cel_map_insert_at.
  Pipeline p = RunPipeline(R"({"a": 1, "b": 2}.transformMap(k, v, v * 10))");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_insert_at"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_insert_at_if_bool"));
  // The map accu gets a terminal SwissTable index build after the loop.
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_index_build"))
      << "map-producing comprehension must emit terminal cel_map_index_build";
}

TEST(ExprLowerComprehensionTest, TransformMap4ArgEmitsMapInsertAtIfBool) {
  // `transformMap(k, v, p, t)` → kMapInsertIf → cel_map_insert_at_if_bool.
  Pipeline p =
      RunPipeline(R"({"a": 1, "b": 2}.transformMap(k, v, v > 1, v * 10))");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_insert_at_if_bool"));
}

TEST(ExprLowerComprehensionTest, TransformMapEntryEmitsMapInsertAt) {
  // `transformMapEntry(k, v, {k': t})` → kMapMerge → N×cel_map_insert_at.
  // The single-entry case routes through the same helper as
  // transformMap; the test locks that the merge emitter doesn't
  // accidentally emit an Entries-specific helper that doesn't exist.
  Pipeline p =
      RunPipeline(R"({"foo": "bar"}.transformMapEntry(k, v, {k + v: k}))");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_insert_at"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_merge"));
}

TEST(ExprLowerComprehensionTest, ExistsEmitsGenericCopy) {
  // `exists(v, p)` → kGeneric → eval loop_step + cel_copy_slot.
  // The accu is scalar bool; loop_step doesn't append or insert.
  // (The iter_range literal `[1, 2, 3]` IS built via cel_list_create
  // + cel_list_append_at — those are unrelated to the comprehension
  // loop body itself, so a generic "no append anywhere" assertion
  // would false-fire.  The signal we want is "no map-insert anywhere
  // and no map-iter setup," since exists over a list source uses
  // neither.)
  Pipeline p = RunPipeline("[1, 2, 3].exists(v, v > 1)");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_copy_slot"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_insert_at"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_iter_init"));
}

// Recursively searches the IR for a Block whose name starts with
// `prefix`.  Same descent set as BodyContainsCallTo.
bool BodyContainsBlockNamePrefix(BinaryenExpressionRef expr,
                                 const char* prefix) {
  if (expr == nullptr) return false;
  const BinaryenExpressionId id = BinaryenExpressionGetId(expr);
  if (id == BinaryenBlockId()) {
    const char* name = BinaryenBlockGetName(expr);
    if (name != nullptr && std::strncmp(name, prefix, strlen(prefix)) == 0) {
      return true;
    }
    const BinaryenIndex n = BinaryenBlockGetNumChildren(expr);
    for (BinaryenIndex i = 0; i < n; ++i) {
      if (BodyContainsBlockNamePrefix(BinaryenBlockGetChildAt(expr, i),
                                      prefix)) {
        return true;
      }
    }
    return false;
  }
  if (id == BinaryenIfId()) {
    return BodyContainsBlockNamePrefix(BinaryenIfGetCondition(expr), prefix) ||
           BodyContainsBlockNamePrefix(BinaryenIfGetIfTrue(expr), prefix) ||
           BodyContainsBlockNamePrefix(BinaryenIfGetIfFalse(expr), prefix);
  }
  if (id == BinaryenDropId()) {
    return BodyContainsBlockNamePrefix(BinaryenDropGetValue(expr), prefix);
  }
  if (id == BinaryenLoopId()) {
    return BodyContainsBlockNamePrefix(BinaryenLoopGetBody(expr), prefix);
  }
  if (id == BinaryenLocalSetId()) {
    return BodyContainsBlockNamePrefix(BinaryenLocalSetGetValue(expr), prefix);
  }
  return false;
}

// Range-absorption guard (EmitRangeAbsorptionGuard): every
// comprehension wraps prologue + loop in a named
// `comp_absorb_<expr_id>` block the guard branches past when the
// iter_range CelValue is CEL_UNKNOWN / CEL_ERROR — list AND map
// sources.  Shape locked by
// `rewrite/wat/70_comprehension_unknown_range.wat`; behavior pinned
// e2e in m2_partial_eval_test.cc's range-absorption matrices.
TEST(ExprLowerComprehensionTest, ListSourceEmitsRangeAbsorptionBlock) {
  Pipeline p = RunPipeline("[1, 2, 3].exists(v, v > 1)");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsBlockNamePrefix(body, "comp_absorb_"));
}

TEST(ExprLowerComprehensionTest, MapSourceEmitsRangeAbsorptionBlock) {
  Pipeline p = RunPipeline(R"({"a": 1}.exists(k, k == "a"))");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsBlockNamePrefix(body, "comp_absorb_"));
}

// ============================================================
// M5.F — general kCallExpr arm (OverloadTable wiring).
// ============================================================
//
// Each test asserts one shape invariant of the emitted IR for a
// kCall that goes through `EmitGeneralCall`.  Tests that need the
// helper-name-by-overload bridge (e.g. `add_int64` →
// `cel_int_add_at_vv`) read it via `BodyContainsCallTo`.

TEST(ExprLowerCallTest, KCallIntAddLowersToHelperCall) {
  // `1 + 2` is the cleanest M5.F sanity — same-kind int arithmetic
  // resolves to `add_int64` → `cel_int_add_at_vv`.
  Pipeline p = RunPipeline("1 + 2");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  EXPECT_TRUE(BodyContainsCallTo(BinaryenFunctionGetBody(lowered->func),
                                 "cel_int_add_at_vv"));
}

TEST(ExprLowerCallTest, KCallDoubleSubLowersToHelperCall) {
  Pipeline p = RunPipeline("3.5 - 1.25");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  EXPECT_TRUE(BodyContainsCallTo(BinaryenFunctionGetBody(lowered->func),
                                 "cel_double_sub_at_vv"));
}

TEST(ExprLowerCallTest, KCallIntLessThanLowersToHelperCall) {
  Pipeline p = RunPipeline("1 < 2");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  EXPECT_TRUE(BodyContainsCallTo(BinaryenFunctionGetBody(lowered->func),
                                 "cel_int_lt_at_vv"));
}

TEST(ExprLowerCallTest, KCallStringConcatLowersToHelperCall) {
  Pipeline p = RunPipeline(R"("a" + "b")");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  EXPECT_TRUE(BodyContainsCallTo(BinaryenFunctionGetBody(lowered->func),
                                 "cel_string_concat_at_vv"));
}

TEST(ExprLowerCallTest, KCallReceiverFormStringContainsFlattens) {
  // `s.contains("foo")` parses with target=s, args=["foo"]; the
  // helper signature `cel_string_contains_at_vv(out, s, sub)` takes
  // them flattened.  Verify the emitted call has 3 operands.
  Pipeline p = RunPipelineWithVars(R"(s.contains("foo"))", {"s:string"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_string_contains_at_vv"));

  // Drill into the root (last child of $eval body) and find the
  // helper call inside.  Root shape: (block (call cel_string_contains
  //   <out> <s_local_get> <sub_rodata>) (i32.const out)).
  BinaryenExpressionRef root =
      BinaryenBlockGetChildAt(body, BinaryenBlockGetNumChildren(body) - 1);
  ASSERT_EQ(BinaryenExpressionGetId(root), BinaryenBlockId());
  ASSERT_EQ(BinaryenBlockGetNumChildren(root), 2u);
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(root, 0);
  ASSERT_EQ(BinaryenExpressionGetId(call), BinaryenCallId());
  EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_string_contains_at_vv");
  EXPECT_EQ(BinaryenCallGetNumOperands(call), 3u)
      << "out_slot + receiver(s) + arg(\"foo\") flattened";
}

TEST(ExprLowerCallTest, KCallSizeOnArenaListEmitsDispatcherCall) {
  // `size([1,2,3])` resolves to `size_list` → kDynamic dispatcher
  // `cel_list_size`, which M5.D step 2 ships (the dispatcher
  // tail-calls into the arena fast path on a CEL_LIST_ARENA
  // operand).  EmitGeneralCall emits a `BinaryenCall` to
  // `cel_list_size`.
  Pipeline p = RunPipeline("size([1, 2, 3])");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_TRUE(BodyContainsCallTo(BinaryenFunctionGetBody(lowered->func),
                                 "cel_list_size"));
}

TEST(ExprLowerCallTest, KCallEqualsLowersToPolymorphicHelper) {
  // M5.B step 2b: `_==_` resolves to overload id `equals` →
  // `cel_equals_at_vv`, the polymorphic dispatcher that branches
  // on operand kinds at runtime.  Codegen just emits a single
  // call site; the kind-switch is in cel_runtime.c.
  Pipeline p = RunPipeline("1 == 1");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_TRUE(BodyContainsCallTo(BinaryenFunctionGetBody(lowered->func),
                                 "cel_equals_at_vv"));
}

TEST(ExprLowerCallTest, KCallEvalFunctionValidatesEndToEnd) {
  // Module-level invariant: a complete arithmetic program emits a
  // module BinaryenValidate accepts (eager imports cover the helper).
  Pipeline p = RunPipelineWithVars("x + 1", {"x:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  ASSERT_THAT(LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m), IsOk());
  m.ExportFunction("$eval", "eval");
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(ExprLowerCallTest, KCallNestedArithmetic) {
  // `(1 + 2) * 3` — nested kCall.  Outer call needs both args
  // emitted; the inner call emits its own (block …) yielding the
  // workspace-slot offset for the multiplication's left operand.
  // Verifies recursion through `Emit` for kCall args.
  Pipeline p = RunPipeline("(1 + 2) * 3");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_int_add_at_vv"));
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_int_mul_at_vv"));
}

// ============================================================
// Select / index on optional-typed operand
// ============================================================
//
// EmitKSelect dispatches on the operand's `Repr`:
//   - `kMessage` / `kMap`: emit a `cel_get_field` / `cel_has_field`
//     call against the field_ref intern table.
//   - `kOptional`: emit a `cel_select_optional_field_at_vv` call
//     whose key argument is the rodata offset of the field-name
//     CelValue allocated by `LayoutPass::SelectKeyRodataVisitor`.
//
// `EmitKIndexCall` dispatches on the operand's `Repr` similarly.

TEST(ExprLowerSelectOptionalTest, SelectOnOptionalEmitsOptionalKernelCall) {
  Pipeline p = RunPipeline("optional.of({'c': 'v'}).c");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_select_optional_field_at_vv"))
      << "kSelect-on-optional must route through the optional kernel";
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_get_field"))
      << "the message-field trampoline must NOT be emitted for an "
         "optional-typed Select";
}

TEST(ExprLowerSelectOptionalTest,
     SelectOnOptionalKeyArgIsRodataOffsetOfFieldName) {
  // The select kernel takes (out_slot, src_slot, key_slot) — `key_slot`
  // must be the rodata offset LayoutPass allocated for the field name.
  Pipeline p = RunPipeline("optional.of({'c': 'v'}).c");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());

  const NodeAnnotation* root_ann =
      p.layout.annotations.Find(p.ast.ast().root_expr().id());
  ASSERT_NE(root_ann, nullptr);
  ASSERT_NE(root_ann->select_key_rodata_offset, 0u);

  // Root expression: `(block (call $cel_select_optional_field_at_vv ...)
  //                          (i32.const out_slot))`
  BinaryenExpressionRef root = RootExpr(lowered->func);
  ASSERT_EQ(BinaryenExpressionGetId(root), BinaryenBlockId());
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(root, 0);
  EXPECT_STREQ(BinaryenCallGetTarget(call), "cel_select_optional_field_at_vv");
  ASSERT_EQ(BinaryenCallGetNumOperands(call), 3u);
  // arg0 = out_slot, arg1 = operand_slot, arg2 = key_rodata_offset.
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(call, 0)),
            static_cast<int32_t>(root_ann->storage.payload));
  EXPECT_EQ(BinaryenConstGetValueI32(BinaryenCallGetOperandAt(call, 2)),
            static_cast<int32_t>(root_ann->select_key_rodata_offset));
}

TEST(ExprLowerSelectOptionalTest, TestOnlySelectOnOptionalEmitsHasValueChain) {
  // `has(optional.of(map).c)` — outer test_only Select on an
  // optional-typed Select chain.  We emit the optional-kernel call
  // followed by `cel_optional_has_value_at_v` overwriting the same
  // slot with a Bool.
  Pipeline p = RunPipeline("has(optional.of({'c': 'v'}).c)");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_select_optional_field_at_vv"));
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_optional_has_value_at_v"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_has_field"))
      << "test_only Select on an optional operand must NOT call the "
         "message-field has trampoline";
}

TEST(ExprLowerIndexOptionalTest, IndexOnOptionalEmitsOptionalKernelCall) {
  // `optional.of(map)[k]` — Call(`_[_]`, [opt, key]).  The operand is
  // optional<map>, so codegen routes to the optional kernel (the
  // kernel unwraps internally).
  Pipeline p = RunPipeline("optional.of({'c': 'v'})['c']");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_select_optional_field_at_vv"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_lookup_arena"));
}

TEST(ExprLowerIndexOptionalTest, MapOptIndexCallEmitsOptionalKernelCall) {
  // `m[?k]` — `Call("_[?_]", [m, k])` per probe Q2.  Routes through
  // the general kCall arm using the `map_optindex_optional_value`
  // overload, which the OverloadTable maps to
  // `cel_select_optional_field_at_vv`.  Distinct from the
  // `_[_]`-with-optional-operand path covered above — exercises the
  // seven `*_optindex_*` overload seeds.
  Pipeline p = RunPipeline("{'c': 'v'}[?'c']");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_select_optional_field_at_vv"));
}

TEST(ExprLowerIndexOptionalTest, ListOptIndexCallEmitsOptionalKernelCall) {
  // List variant: `[?i]` on a list literal routes to the
  // `list_optindex_optional_int` overload, also seeded onto the
  // optional kernel.
  Pipeline p = RunPipeline("[10, 20, 30][?1]");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_select_optional_field_at_vv"));
}

TEST(ExprLowerIndexOptionalTest, IndexOnOptionalListEmitsOptionalKernelCall) {
  // `optional.of([1,2,3])[0]` — operand is `optional<list>`, so
  // `EmitKIndexCall` routes Repr::kOptional through the optional
  // kernel for the list variant too.  Distinct from
  // `IndexOnOptionalEmitsOptionalKernelCall` which uses an
  // optional<map> operand.
  Pipeline p = RunPipeline("optional.of([10, 20, 30])[1]");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_select_optional_field_at_vv"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_list_at_arena"));
}

TEST(ExprLowerSelectOptionalTest, SelectOnOptionalNoneEmitsOptionalKernelCall) {
  // `optional.ofNonZeroValue({'a': 'b'}).c` — the value is Some(map)
  // at runtime, but the static type is `optional<map<…>>` so codegen
  // still routes through the optional kernel.  Bare `optional.none()`
  // can't be used here: it types as `optional<dyn>`, which the
  // static-subset gate rejects before codegen runs.
  Pipeline p = RunPipeline("optional.ofNonZeroValue({'a': 'b'}).c");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_select_optional_field_at_vv"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_get_field"));
}

// ============================================================
// Optional entries in map / list literals
// ============================================================
//
// `{?key: opt_v}` and `[?opt_e]` (probes Q3 / Q4) route through new
// predicate-gated kernels.  Mixed literals (one `?` entry + one
// regular entry) must emit one kernel per entry-kind in document
// order.

TEST(ExprLowerOptionalLiteralTest, MapAllOptionalEntriesEmitInsertIfPresent) {
  Pipeline p =
      RunPipeline("{?'k1': optional.of('v1'), ?'k2': optional.none()}");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_insert_at_if_present"));
  // No unconditional `cel_map_insert` — every entry is optional.
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_insert"))
      << "all-optional map literal must not emit any unconditional inserts";
}

TEST(ExprLowerOptionalLiteralTest, MapMixedEntriesEmitsBothInsertKernels) {
  // The first entry is unconditional, the second is `?key:` —
  // codegen must emit `cel_map_insert` then `cel_map_insert_at_if_present`.
  Pipeline p = RunPipeline("{'k1': 'v1', ?'k2': optional.of('v2')}");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_insert"));
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_insert_at_if_present"));
}

TEST(ExprLowerOptionalLiteralTest, ListAllOptionalElementsEmitAppendIfPresent) {
  Pipeline p = RunPipeline("[?optional.of(10), ?optional.of(20)]");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_list_append_at_if_present"));
}

TEST(ExprLowerOptionalLiteralTest, ListMixedElementsEmitsBothAppendKernels) {
  // `[1, ?optional.of(2), 3]` — middle element is optional, the
  // others are not.  Both kernels must appear.
  Pipeline p = RunPipeline("[1, ?optional.of(2), 3]");
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());
  EXPECT_THAT(m.Validate(), IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_list_append_at"));
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_list_append_at_if_present"));
}

TEST(ExprLowerOptionalLiteralTest, NonOptionalMapLiteralEmitsOnlyPlainInsert) {
  // Regression: ordinary map literal must not route through the
  // predicate-gated kernel even after the codegen branch was added.  A
  // non-constant value (`a`) forces the build path; an all-const map would
  // materialize and emit no inserts (see ConstMapLiteralLowersToI32Const).
  Pipeline p = RunPipelineWithVars("{'k': a}", {"a:string"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_map_insert"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_map_insert_at_if_present"));
}

TEST(ExprLowerOptionalLiteralTest, NonOptionalListLiteralEmitsOnlyPlainAppend) {
  // A non-constant element (`a`) forces the build path; the
  // non-optional elements must use the plain append, not the
  // append-if-present variant.  (An all-const list would materialize
  // and emit no appends — see ConstListLiteralLowersToI32ConstNoBuild.)
  Pipeline p = RunPipelineWithVars("[a, 2, 3]", {"a:int"});
  WasmModule m;
  PrepareHostModule(m, p.layout);
  auto lowered = LowerWithDefaultOverloads(p.ast, p.layout, "$eval", m);
  ASSERT_THAT(lowered, IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(lowered->func);
  EXPECT_TRUE(BodyContainsCallTo(body, "cel_list_append_at"));
  EXPECT_FALSE(BodyContainsCallTo(body, "cel_list_append_at_if_present"));
}

// `Foo{?field: opt_value}` proto-literal optional entries route
// through the new `cel_set_field_at_if_present` kernel.  Per-shape
// codegen verification lives at the e2e level (`m14_test.cc`)
// because the codegen-test `RunPipeline` doesn't register the
// conformance proto descriptors needed for `cel.expr.conformance.*`
// names to type-check.  The wat_runner test
// (`WatRunnerM14Test.SetFieldIfPresentSomeCallsHostNoneShortCircuits`)
// locks the kernel ABI byte-exact, and the runtime tests in
// `cel_optional_test.cc` cover every input shape — between them,
// the only remaining surface is "does the codegen branch on
// `f.optional()` correctly", which the e2e tests prove directly.

}  // namespace
}  // namespace celwasm
