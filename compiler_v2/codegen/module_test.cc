#include "compiler_v2/codegen/module.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "binaryen-c.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// --- Lifecycle -----------------------------------------------------------

TEST(WasmModuleTest, EmptyModuleValidatesAndSerializesPreamble) {
  WasmModule m;
  BinaryenExpressionRef body = BinaryenNop(m.raw());
  m.AddFunction("nop", {}, BinaryenTypeNone(), {}, body);

  EXPECT_THAT(m.Validate(), IsOk());

  auto bytes_or = m.Serialize();
  ASSERT_THAT(bytes_or, IsOk());
  const auto& bytes = *bytes_or;
  ASSERT_GE(bytes.size(), 8u);
  // `\0asm` magic + version 1.
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x61);
  EXPECT_EQ(bytes[2], 0x73);
  EXPECT_EQ(bytes[3], 0x6D);
  EXPECT_EQ(bytes[4], 0x01);
  EXPECT_EQ(bytes[5], 0x00);
  EXPECT_EQ(bytes[6], 0x00);
  EXPECT_EQ(bytes[7], 0x00);
}

TEST(WasmModuleTest, MoveConstructTransfersOwnership) {
  WasmModule src;
  BinaryenExpressionRef body = BinaryenNop(src.raw());
  src.AddFunction("f", {}, BinaryenTypeNone(), {}, body);

  WasmModule dst(std::move(src));
  EXPECT_THAT(dst.Validate(), IsOk());
}

TEST(WasmModuleTest, MoveAssignDisposesPrevious) {
  WasmModule a;
  WasmModule b;
  BinaryenExpressionRef body_a = BinaryenNop(a.raw());
  BinaryenExpressionRef body_b = BinaryenNop(b.raw());
  a.AddFunction("fa", {}, BinaryenTypeNone(), {}, body_a);
  b.AddFunction("fb", {}, BinaryenTypeNone(), {}, body_b);
  a = std::move(b);
  EXPECT_THAT(a.Validate(), IsOk());
}

// --- Memory: define + export --------------------------------------------

TEST(WasmModuleTest, SetMemoryExportsUnderGivenName) {
  WasmModule m;
  ASSERT_THAT(m.SetMemory(1, std::nullopt, "memory"), IsOk());
  EXPECT_TRUE(BinaryenHasMemory(m.raw()));
  EXPECT_THAT(m.Validate(), IsOk());
  ASSERT_EQ(BinaryenGetNumExports(m.raw()), 1u);
  BinaryenExportRef exp = BinaryenGetExportByIndex(m.raw(), 0);
  EXPECT_EQ(BinaryenExportGetKind(exp), BinaryenExternalMemory());
  EXPECT_STREQ(BinaryenExportGetName(exp), "memory");
}

TEST(WasmModuleTest, SetMemoryWithEmptyExportDoesNotExport) {
  WasmModule m;
  ASSERT_THAT(m.SetMemory(1, std::nullopt, ""), IsOk());
  EXPECT_TRUE(BinaryenHasMemory(m.raw()));
  EXPECT_EQ(BinaryenGetNumExports(m.raw()), 0u);
}

TEST(WasmModuleTest, SetMemoryTwiceFailsPrecondition) {
  WasmModule m;
  ASSERT_THAT(m.SetMemory(1, std::nullopt, "memory"), IsOk());
  EXPECT_THAT(m.SetMemory(1, std::nullopt, "memory"),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST(WasmModuleTest, SetMemoryMaxLessThanInitialIsInvalidArgument) {
  WasmModule m;
  EXPECT_THAT(m.SetMemory(4, /*max_pages=*/2, "memory"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// --- Memory: data segments ----------------------------------------------

TEST(WasmModuleTest, SetMemoryInstallsActiveDataSegment) {
  WasmModule m;
  // Use a 4-byte tag unlikely to occur elsewhere in the wasm binary so
  // the round-trip search doesn't collide with Binaryen-emitted bytes.
  const std::vector<uint8_t> rodata = {0xDE, 0xAD, 0xBE, 0xEF};
  WasmModule::DataSegment seg{/*offset=*/16, rodata};
  ASSERT_THAT(
      m.SetMemory(1, std::nullopt, "memory", absl::MakeConstSpan(&seg, 1)),
      IsOk());
  EXPECT_EQ(BinaryenGetNumMemorySegments(m.raw()), 1u);
  EXPECT_THAT(m.Validate(), IsOk());

  // Binaryen's C API doesn't expose segment bytes for active segments,
  // so verify the payload by searching for it in the serialised module.
  auto bytes_or = m.Serialize();
  ASSERT_THAT(bytes_or, IsOk());
  const auto& bytes = *bytes_or;
  bool found = false;
  for (size_t i = 0; i + rodata.size() <= bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, rodata.data(), rodata.size()) == 0) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "rodata payload not found in serialised bytes";
}

TEST(WasmModuleTest, SetMemoryInstallsMultipleDataSegments) {
  WasmModule m;
  const std::vector<uint8_t> seg_a = {0x01, 0x02};
  const std::vector<uint8_t> seg_b = {0x03, 0x04, 0x05};
  WasmModule::DataSegment segs[] = {
      {/*offset=*/16, seg_a},
      {/*offset=*/64, seg_b},
  };
  ASSERT_THAT(m.SetMemory(1, std::nullopt, "memory", segs), IsOk());
  EXPECT_EQ(BinaryenGetNumMemorySegments(m.raw()), 2u);
  EXPECT_THAT(m.Validate(), IsOk());
}

// --- Memory: import -----------------------------------------------------

TEST(WasmModuleTest, AddMemoryImportMarksMemoryAsImport) {
  WasmModule m;
  ASSERT_THAT(m.AddMemoryImport("cel", "memory", 1, std::nullopt), IsOk());
  EXPECT_TRUE(BinaryenHasMemory(m.raw()));
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(WasmModuleTest, AddMemoryImportAfterSetFailsPrecondition) {
  WasmModule m;
  ASSERT_THAT(m.SetMemory(1, std::nullopt, "memory"), IsOk());
  EXPECT_THAT(m.AddMemoryImport("cel", "memory", 1, std::nullopt),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

// --- Functions: imports + defs + exports --------------------------------

TEST(WasmModuleTest, AddFunctionImportValidates) {
  WasmModule m;
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType params[2] = {i32, i32};
  m.AddFunctionImport("arena_reset", "cel", "arena_reset", params,
                      BinaryenTypeNone());
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(WasmModuleTest, AddFunctionAndExport) {
  WasmModule m;
  BinaryenExpressionRef body = BinaryenNop(m.raw());
  m.AddFunction("$eval", {}, BinaryenTypeNone(), {}, body);
  m.ExportFunction("$eval", "eval");
  ASSERT_EQ(BinaryenGetNumExports(m.raw()), 1u);
  BinaryenExportRef exp = BinaryenGetExportByIndex(m.raw(), 0);
  EXPECT_EQ(BinaryenExportGetKind(exp), BinaryenExternalFunction());
  EXPECT_STREQ(BinaryenExportGetName(exp), "eval");
  EXPECT_THAT(m.Validate(), IsOk());
}

// --- Custom sections -----------------------------------------------------

TEST(WasmModuleTest, AddCustomSectionRoundTripsThroughSerialize) {
  WasmModule m;
  BinaryenExpressionRef body = BinaryenNop(m.raw());
  m.AddFunction("f", {}, BinaryenTypeNone(), {}, body);
  const std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
  m.AddCustomSection("cel.abi", payload);
  ASSERT_THAT(m.Validate(), IsOk());

  auto bytes_or = m.Serialize();
  ASSERT_THAT(bytes_or, IsOk());
  const auto& bytes = *bytes_or;
  // Custom section names and bodies round-trip into the binary.  The
  // name string "cel.abi" appears verbatim inside the module bytes
  // (followed by the payload bytes), which is the simplest non-brittle
  // assertion at this layer.
  bool found_name = false;
  for (size_t i = 0; i + 7 <= bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, "cel.abi", 7) == 0) {
      found_name = true;
      break;
    }
  }
  EXPECT_TRUE(found_name) << "cel.abi name not found in serialised bytes";
  bool found_payload = false;
  for (size_t i = 0; i + payload.size() <= bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, payload.data(), payload.size()) == 0) {
      found_payload = true;
      break;
    }
  }
  EXPECT_TRUE(found_payload) << "cel.abi payload not found in serialised bytes";
}

// --- TupleType helper ----------------------------------------------------

TEST(TupleTypeTest, EmptyReturnsNone) {
  EXPECT_EQ(TupleType({}), BinaryenTypeNone());
}
TEST(TupleTypeTest, SingleElementReturnsElement) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType parts[1] = {i32};
  EXPECT_EQ(TupleType(parts), i32);
}
TEST(TupleTypeTest, MultipleElementsReturnsInternedTuple) {
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType i64 = BinaryenTypeInt64();
  const BinaryenType a[2] = {i32, i64};
  const BinaryenType b[2] = {i32, i64};
  // Binaryen interns tuple types; equal tuples yield equal handles.
  EXPECT_EQ(TupleType(a), TupleType(b));
  EXPECT_NE(TupleType(a), i32);
}

// ── WasmModule::Optimize ─────────────────────────────────────────

// Helper: builds a tiny module with a function whose body contains
// an obviously-dead local + a constant-foldable expression Binaryen
// should rewrite at -O2.  Used by the Optimize tests below.
WasmModule BuildOptimizableModule() {
  WasmModule m;
  // `(local.set $x (i32.const 7))   ; dead store — never read
  //  (return (i32.add (i32.const 2) (i32.const 3)))`
  // The dead local should DCE; the constant add should fold to 5.
  BinaryenType i32 = BinaryenTypeInt32();
  BinaryenExpressionRef dead =
      BinaryenLocalSet(m.raw(), 0,
                       BinaryenConst(m.raw(), BinaryenLiteralInt32(7)));
  BinaryenExpressionRef ret = BinaryenReturn(
      m.raw(), BinaryenBinary(m.raw(), BinaryenAddInt32(),
                              BinaryenConst(m.raw(), BinaryenLiteralInt32(2)),
                              BinaryenConst(m.raw(), BinaryenLiteralInt32(3))));
  BinaryenExpressionRef body_parts[2] = {dead, ret};
  BinaryenExpressionRef body =
      BinaryenBlock(m.raw(), nullptr, body_parts, 2, BinaryenTypeNone());
  const BinaryenType locals[1] = {i32};
  m.AddFunction("opt_target", {}, i32, absl::MakeConstSpan(locals), body);
  return m;
}

TEST(WasmModuleOptimizeTest, LevelZeroIsByteIdentical) {
  // The opt=0 contract — invoking Optimize with level 0 must not
  // touch the module.  Locks the default-off invariant so existing
  // codegen golden tests stay green when this method is wired into
  // FinaliseModule.
  WasmModule a = BuildOptimizableModule();
  WasmModule b = BuildOptimizableModule();
  auto baseline_or = a.Serialize();
  ASSERT_THAT(baseline_or, IsOk());
  EXPECT_THAT(b.Optimize(0), IsOk());
  auto after_or = b.Serialize();
  ASSERT_THAT(after_or, IsOk());
  EXPECT_EQ(*baseline_or, *after_or);
}

TEST(WasmModuleOptimizeTest, LevelTwoStillValidatesAndShrinksDeadCode) {
  // -O2 must produce a still-valid module AND must DCE or fold at
  // least one of the dead-local / constant-fold seeds.  Asserting
  // strict-less-than on serialized size is the load-bearing proof
  // that the pass list actually ran.
  WasmModule unopt = BuildOptimizableModule();
  WasmModule opt = BuildOptimizableModule();
  auto unopt_bytes_or = unopt.Serialize();
  ASSERT_THAT(unopt_bytes_or, IsOk());
  EXPECT_THAT(opt.Optimize(2), IsOk());
  EXPECT_THAT(opt.Validate(), IsOk());
  auto opt_bytes_or = opt.Serialize();
  ASSERT_THAT(opt_bytes_or, IsOk());
  EXPECT_LT(opt_bytes_or->size(), unopt_bytes_or->size())
      << "Optimize(2) failed to shrink a module with a dead local + a "
         "foldable constant add; the pass list must not have run.";
}

TEST(WasmModuleOptimizeTest, LevelOutOfRangeIsInvalidArgument) {
  // Closed-range contract: 0..3.  -1 / 4 should error explicitly so
  // a misspelled CLI flag surfaces at the boundary.
  WasmModule m = BuildOptimizableModule();
  EXPECT_THAT(m.Optimize(-1),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(m.Optimize(4),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace
}  // namespace celwasm
