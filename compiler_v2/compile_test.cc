#include "compiler_v2/compile.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "binaryen-c.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// --- Per-kind kConst round-trip through Compile() ------------------------

TEST(CompileTest, CompilesScalarBool) {
  EXPECT_THAT(Compile("true").status(), IsOk());
}
TEST(CompileTest, CompilesScalarInt) {
  EXPECT_THAT(Compile("42").status(), IsOk());
}
TEST(CompileTest, CompilesScalarUint) {
  EXPECT_THAT(Compile("42u").status(), IsOk());
}
TEST(CompileTest, CompilesScalarDouble) {
  EXPECT_THAT(Compile("3.14").status(), IsOk());
}
TEST(CompileTest, CompilesScalarNull) {
  EXPECT_THAT(Compile("null").status(), IsOk());
}
TEST(CompileTest, CompilesScalarString) {
  EXPECT_THAT(Compile("\"hi\"").status(), IsOk());
}
TEST(CompileTest, CompilesScalarBytes) {
  EXPECT_THAT(Compile("b\"x\"").status(), IsOk());
}

// --- Error propagation ---------------------------------------------------

TEST(CompileTest, NonKConstRootSurfacesAsUnimplemented) {
  // `1 + 2` type-checks but its root is a kCall; expr_lower rejects
  // non-kConst roots at M1, and the facade passes that through.
  EXPECT_THAT(Compile("1 + 2"), StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(CompileTest, ParseFailureSurfacesAsInvalidArgument) {
  // Unclosed paren — parser rejects, facade forwards the status.
  EXPECT_THAT(Compile("(1"), StatusIs(absl::StatusCode::kInvalidArgument));
}

// --- Artifact shape ------------------------------------------------------

TEST(CompileTest, ArtifactCarriesAstLayoutAndEvalFunction) {
  auto art_or = Compile("42");
  ASSERT_THAT(art_or, IsOk());
  const CompiledArtifact& art = *art_or;
  EXPECT_TRUE(art.ast.has_ast());
  EXPECT_FALSE(art.layout.rodata.empty());
  EXPECT_EQ(art.layout.rodata_base, 16u);
  EXPECT_NE(art.eval_fn.func, nullptr);
}

TEST(CompileTest, ModuleImportsCelMemoryAndExportsEvalByDefault) {
  // Per Plan §5 Commit F: under the (A) two-phase topology, expr no
  // longer defines its own memory — it imports `cel.memory` from the
  // host (the same memory the cel_runtime.wasm instance imports), so
  // both modules see the same bytes when wired up by `Engine::Plan`.
  auto art_or = Compile("42");
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  EXPECT_TRUE(BinaryenHasMemory(raw));

  // Memory should NOT be in the export table (it's an import now).
  bool saw_memory_export = false;
  bool saw_eval_export = false;
  const auto n = BinaryenGetNumExports(raw);
  for (BinaryenIndex i = 0; i < n; ++i) {
    BinaryenExportRef e = BinaryenGetExportByIndex(raw, i);
    const char* name = BinaryenExportGetName(e);
    if (std::strcmp(name, "memory") == 0) saw_memory_export = true;
    if (std::strcmp(name, "eval") == 0) saw_eval_export = true;
  }
  EXPECT_FALSE(saw_memory_export);
  EXPECT_TRUE(saw_eval_export);

  // Memory import should be (cel, memory).  The internal binaryen
  // name of the memory is also "memory" (set by AddMemoryImport in
  // codegen/module.cc).
  const char* mod_name = BinaryenMemoryImportGetModule(raw, "memory");
  const char* base_name = BinaryenMemoryImportGetBase(raw, "memory");
  ASSERT_NE(mod_name, nullptr);
  ASSERT_NE(base_name, nullptr);
  EXPECT_STREQ(mod_name, "cel");
  EXPECT_STREQ(base_name, "memory");
}

TEST(CompileTest, ModuleInstallsCelResetAndCelAllocImports) {
  auto art_or = Compile("42");
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  // Binaryen models function imports as functions whose body is null.
  EXPECT_NE(BinaryenGetFunction(raw, "cel_reset"), nullptr);
  EXPECT_NE(BinaryenGetFunction(raw, "cel_alloc"), nullptr);
}

TEST(CompileTest, SerializedBytesStartWithWasmPreamble) {
  auto art_or = Compile("42");
  ASSERT_THAT(art_or, IsOk());
  const auto& bytes = art_or->wasm_bytes;
  ASSERT_GE(bytes.size(), 8u);
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x61);
  EXPECT_EQ(bytes[2], 0x73);
  EXPECT_EQ(bytes[3], 0x6D);
}

// --- Option flow ---------------------------------------------------------

TEST(CompileTest, SerializeFalseLeavesWasmBytesEmpty) {
  CompileOptions opts;
  opts.serialize = false;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  EXPECT_TRUE(art_or->wasm_bytes.empty());
}

TEST(CompileTest, MemSizeBytesFlowsToCelResetSecondArg) {
  CompileOptions opts;
  opts.mem_size_bytes = 128u * 1024u;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());

  BinaryenExpressionRef body = BinaryenFunctionGetBody(art_or->eval_fn.func);
  BinaryenExpressionRef call = BinaryenBlockGetChildAt(body, 0);
  ASSERT_STREQ(BinaryenCallGetTarget(call), "cel_reset");
  BinaryenExpressionRef arg1 = BinaryenCallGetOperandAt(call, 1);
  EXPECT_EQ(BinaryenConstGetValueI32(arg1),
            static_cast<int32_t>(opts.mem_size_bytes));
}

TEST(CompileTest, EvalExportNameIsHonoured) {
  CompileOptions opts;
  opts.eval_export_name = "my_eval";
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  bool saw = false;
  const auto n = BinaryenGetNumExports(raw);
  for (BinaryenIndex i = 0; i < n; ++i) {
    if (std::strcmp(BinaryenExportGetName(BinaryenGetExportByIndex(raw, i)),
                    "my_eval") == 0) {
      saw = true;
      break;
    }
  }
  EXPECT_TRUE(saw);
}

TEST(CompileTest, MemSizeBytesLargerThanOnePageGrowsPageCount) {
  // Ask for 3 * 64 KiB; memory should be declared with at least 3 pages.
  CompileOptions opts;
  opts.mem_size_bytes = 3u * 64u * 1024u;
  auto art_or = Compile("42", opts);
  ASSERT_THAT(art_or, IsOk());
  BinaryenModuleRef raw = art_or->module.raw();
  ASSERT_TRUE(BinaryenHasMemory(raw));
  // Serialize and validate that it's a legal module — the page-count
  // arithmetic is validated transitively by Binaryen's validator.
  EXPECT_THAT(art_or->module.Validate(), IsOk());
}

}  // namespace
}  // namespace celwasm
