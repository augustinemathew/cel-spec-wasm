#include "compiler/codegen/cel_refs.h"

#include <cstring>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "binaryen-c.h"
#include "compiler/codegen/module.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

TEST(CelRefsTest, AddCelRefsTableAndHelpersValidates) {
  WasmModule m;
  ASSERT_THAT(AddCelRefsTableAndHelpers(m, "$cel_refs", /*initial_slots=*/8),
              IsOk());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(CelRefsTest, InitialSlotsBelowTwoIsRejected) {
  WasmModule m;
  EXPECT_THAT(AddCelRefsTableAndHelpers(m, "$cel_refs", /*initial_slots=*/1),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(AddCelRefsTableAndHelpers(m, "$cel_refs", /*initial_slots=*/0),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(CelRefsTest, EmitsExternrefTable) {
  WasmModule m;
  ASSERT_THAT(AddCelRefsTableAndHelpers(m, "$cel_refs", 16), IsOk());
  BinaryenTableRef tab = BinaryenGetTable(m.raw(), "$cel_refs");
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(BinaryenTableGetInitial(tab), 16u);
  EXPECT_EQ(BinaryenTableGetType(tab), BinaryenTypeExternref());
}

TEST(CelRefsTest, EmitsMutableI32NextGlobalInitToOne) {
  WasmModule m;
  ASSERT_THAT(AddCelRefsTableAndHelpers(m, "$cel_refs", 2), IsOk());
  BinaryenGlobalRef g = BinaryenGetGlobal(m.raw(), "cel_refs_next");
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(BinaryenGlobalGetType(g), BinaryenTypeInt32());
  EXPECT_TRUE(BinaryenGlobalIsMutable(g));
  // Initializer is `i32.const 1`.
  BinaryenExpressionRef init = BinaryenGlobalGetInitExpr(g);
  ASSERT_NE(init, nullptr);
  EXPECT_EQ(BinaryenExpressionGetId(init), BinaryenConstId());
  EXPECT_EQ(BinaryenConstGetValueI32(init), 1);
}

TEST(CelRefsTest, EmitsInternGetAndResetFunctions) {
  WasmModule m;
  ASSERT_THAT(AddCelRefsTableAndHelpers(m, "$cel_refs", 4), IsOk());
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType exref = BinaryenTypeExternref();

  BinaryenFunctionRef intern_fn =
      BinaryenGetFunction(m.raw(), "cel_ref_intern");
  ASSERT_NE(intern_fn, nullptr);
  EXPECT_EQ(BinaryenFunctionGetParams(intern_fn), exref);
  EXPECT_EQ(BinaryenFunctionGetResults(intern_fn), i32);

  BinaryenFunctionRef get_fn = BinaryenGetFunction(m.raw(), "cel_ref_get");
  ASSERT_NE(get_fn, nullptr);
  EXPECT_EQ(BinaryenFunctionGetParams(get_fn), i32);
  EXPECT_EQ(BinaryenFunctionGetResults(get_fn), exref);

  BinaryenFunctionRef reset_fn = BinaryenGetFunction(m.raw(), "cel_refs_reset");
  ASSERT_NE(reset_fn, nullptr);
  EXPECT_EQ(BinaryenFunctionGetParams(reset_fn), BinaryenTypeNone());
  EXPECT_EQ(BinaryenFunctionGetResults(reset_fn), BinaryenTypeNone());
}

TEST(CelRefsTest, ExportsTableAndAllThreeFunctions) {
  WasmModule m;
  ASSERT_THAT(AddCelRefsTableAndHelpers(m, "$cel_refs", 4), IsOk());
  // Exports: $cel_refs (table), cel_ref_intern, cel_ref_get, cel_refs_reset.
  ASSERT_EQ(BinaryenGetNumExports(m.raw()), 4u);

  bool saw_table = false, saw_intern = false, saw_get = false,
       saw_reset = false;
  for (BinaryenIndex i = 0; i < BinaryenGetNumExports(m.raw()); ++i) {
    BinaryenExportRef exp = BinaryenGetExportByIndex(m.raw(), i);
    const char* name = BinaryenExportGetName(exp);
    const BinaryenExternalKind kind = BinaryenExportGetKind(exp);
    if (std::strcmp(name, "$cel_refs") == 0 &&
        kind == BinaryenExternalTable()) {
      saw_table = true;
    } else if (std::strcmp(name, "cel_ref_intern") == 0 &&
               kind == BinaryenExternalFunction()) {
      saw_intern = true;
    } else if (std::strcmp(name, "cel_ref_get") == 0 &&
               kind == BinaryenExternalFunction()) {
      saw_get = true;
    } else if (std::strcmp(name, "cel_refs_reset") == 0 &&
               kind == BinaryenExternalFunction()) {
      saw_reset = true;
    }
  }
  EXPECT_TRUE(saw_table);
  EXPECT_TRUE(saw_intern);
  EXPECT_TRUE(saw_get);
  EXPECT_TRUE(saw_reset);
}

TEST(CelRefsTest, ModuleSerializesToNonEmptyBinary) {
  WasmModule m;
  ASSERT_THAT(AddCelRefsTableAndHelpers(m, "$cel_refs", 4), IsOk());
  auto bytes = m.Serialize();
  ASSERT_THAT(bytes, IsOk());
  EXPECT_GT(bytes->size(), 8u);
}

}  // namespace
}  // namespace celwasm
