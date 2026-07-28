#include "compiler/codegen/module.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "abi/wasm_binary.h"
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
  // `\0asm` magic + core-module version word.
  EXPECT_TRUE(IsCoreModule(*bytes_or));
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

TEST(WasmModuleTest, AddMemoryImportTwiceFailsPrecondition) {
  WasmModule m;
  ASSERT_THAT(m.AddMemoryImport("cel", "memory", 1, std::nullopt), IsOk());
  EXPECT_THAT(m.AddMemoryImport("cel", "memory", 1, std::nullopt),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST(WasmModuleTest, AddMemoryImportMaxLessThanInitialIsInvalidArgument) {
  WasmModule m;
  EXPECT_THAT(m.AddMemoryImport("cel", "memory", 4, /*max_pages=*/2),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(WasmModuleTest, AddMemoryImportSharedRequiresMaxPages) {
  WasmModule m;
  EXPECT_THAT(m.AddMemoryImport("cel", "memory", 1, std::nullopt,
                                /*segments=*/{}, /*shared=*/true),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// --- Memory: data segments ----------------------------------------------

TEST(WasmModuleTest, MemoryImportInstallsActiveDataSegment) {
  WasmModule m;
  // Use a 4-byte tag unlikely to occur elsewhere in the wasm binary so
  // the round-trip search doesn't collide with Binaryen-emitted bytes.
  const std::vector<uint8_t> rodata = {0xDE, 0xAD, 0xBE, 0xEF};
  WasmModule::DataSegment seg{/*offset=*/16, rodata};
  ASSERT_THAT(m.AddMemoryImport("cel", "memory", 1, std::nullopt,
                                absl::MakeConstSpan(&seg, 1)),
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

TEST(WasmModuleTest, MemoryImportInstallsMultipleDataSegments) {
  WasmModule m;
  const std::vector<uint8_t> seg_a = {0x01, 0x02};
  const std::vector<uint8_t> seg_b = {0x03, 0x04, 0x05};
  WasmModule::DataSegment segs[] = {
      {/*offset=*/16, seg_a},
      {/*offset=*/64, seg_b},
  };
  ASSERT_THAT(m.AddMemoryImport("cel", "memory", 1, std::nullopt, segs),
              IsOk());
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

// --- Functions: imports + defs + exports --------------------------------

TEST(WasmModuleTest, AddFunctionImportValidates) {
  WasmModule m;
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType params[2] = {i32, i32};
  m.AddFunctionImport("arena_reset", "cel", "arena_reset", params,
                      BinaryenTypeNone());
  EXPECT_THAT(m.Validate(), IsOk());
}

// --- ListFunctionImports -------------------------------------------------

TEST(WasmModuleListFunctionImportsTest, EmptyModuleHasNoImports) {
  WasmModule m;
  EXPECT_TRUE(m.ListFunctionImports().empty());
}

TEST(WasmModuleListFunctionImportsTest, ReturnsExternalNamesInModuleOrder) {
  WasmModule m;
  const BinaryenType i32 = BinaryenTypeInt32();
  const BinaryenType params[2] = {i32, i32};
  m.AddFunctionImport("first", "cel", "arena_reset", params,
                      BinaryenTypeNone());
  m.AddFunctionImport("second", "cel_fn", "allow_string", params,
                      BinaryenTypeNone());
  const auto imports = m.ListFunctionImports();
  ASSERT_EQ(imports.size(), 2u);
  EXPECT_EQ(imports[0].module, "cel");
  EXPECT_EQ(imports[0].base, "arena_reset");
  EXPECT_EQ(imports[1].module, "cel_fn");
  EXPECT_EQ(imports[1].base, "allow_string");
}

TEST(WasmModuleListFunctionImportsTest,
     ExternalBaseReturnedWhenInternalNameDiffers) {
  // The list carries the names the HOST resolves at link time, not
  // codegen's internal handles.
  WasmModule m;
  const BinaryenType params[1] = {BinaryenTypeInt32()};
  m.AddFunctionImport("$internal_alias", "cel_host", "cel_get_field", params,
                      BinaryenTypeNone());
  const auto imports = m.ListFunctionImports();
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].module, "cel_host");
  EXPECT_EQ(imports[0].base, "cel_get_field");
}

TEST(WasmModuleListFunctionImportsTest, ExcludesDefinedFunctions) {
  WasmModule m;
  const BinaryenType params[1] = {BinaryenTypeInt32()};
  m.AddFunctionImport("imp", "cel", "arena_alloc", params, BinaryenTypeInt32());
  BinaryenExpressionRef body = BinaryenNop(m.raw());
  m.AddFunction("defined_fn", {}, BinaryenTypeNone(), {}, body);
  const auto imports = m.ListFunctionImports();
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].base, "arena_alloc");
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
  BinaryenExpressionRef dead = BinaryenLocalSet(
      m.raw(), 0, BinaryenConst(m.raw(), BinaryenLiteralInt32(7)));
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
  EXPECT_THAT(m.Optimize(-1), StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(m.Optimize(4), StatusIs(absl::StatusCode::kInvalidArgument));
}

// ── WasmModule::Adopt ────────────────────────────────────────────

// Mirrors the `DefaultFeatures()` set module.cc installs on every
// WasmModule (it lives in an anonymous namespace, so it can't be
// referenced here).  Duplicated deliberately: the default feature set
// is part of the module contract, and this constant is the tripwire
// that makes changing it a reviewable event.
BinaryenFeatures ExpectedDefaultFeatures() {
  return BinaryenFeatureReferenceTypes() | BinaryenFeatureMultivalue() |
         BinaryenFeatureBulkMemory() | BinaryenFeatureSignExt() |
         BinaryenFeatureMutableGlobals() | BinaryenFeatureGC() |
         BinaryenFeatureAtomics();
}

TEST(WasmModuleAdoptTest, RoundTripThroughSerializeAndReadValidates) {
  // Serialize a small module, read it back with BinaryenModuleRead,
  // and Adopt the result — the kStatic link-mode shape, where the
  // pre-built runtime wasm is loaded as the base module.
  WasmModule src;
  // The static base (cel_runtime.wasm) OWNS its memory — install one
  // directly via the Binaryen C API to keep the adopted shape faithful.
  BinaryenSetMemory(src.raw(), /*initial=*/1, /*maximum=*/1,
                    /*exportName=*/"memory", /*segmentNames=*/nullptr,
                    /*segmentDatas=*/nullptr, /*segmentPassives=*/nullptr,
                    /*segmentOffsets=*/nullptr, /*segmentSizes=*/nullptr,
                    /*numSegments=*/0, /*shared=*/false, /*memory64=*/false,
                    /*name=*/"0");
  BinaryenExpressionRef body = BinaryenNop(src.raw());
  src.AddFunction("f", {}, BinaryenTypeNone(), {}, body);
  ASSERT_THAT(src.Validate(), IsOk());
  auto bytes_or = src.Serialize();
  ASSERT_THAT(bytes_or, IsOk());

  // BinaryenModuleRead takes a mutable `char*`; copy into a scratch
  // buffer rather than const_cast'ing the serialised bytes.
  std::vector<char> buf(bytes_or->begin(), bytes_or->end());
  BinaryenModuleRef read = BinaryenModuleRead(buf.data(), buf.size());
  ASSERT_NE(read, nullptr);

  WasmModule adopted = WasmModule::Adopt(read);
  EXPECT_TRUE(BinaryenHasMemory(adopted.raw()));
  EXPECT_THAT(adopted.Validate(), IsOk());
  const BinaryenFeatures feats = BinaryenModuleGetFeatures(adopted.raw());
  EXPECT_EQ(feats & ExpectedDefaultFeatures(), ExpectedDefaultFeatures())
      << "adopted module is missing default feature bits; got 0x" << std::hex
      << feats;
}

TEST(WasmModuleAdoptTest, BareMvpModuleGainsDefaultFeatures) {
  // A freshly-created Binaryen module declares MVP-only features.
  // Adopt must widen it to (MVP ∪ DefaultFeatures) — a still-MVP
  // adopted module would reject the multi-value / reference-types
  // shapes codegen emits.
  BinaryenModuleRef bare = BinaryenModuleCreate();
  ASSERT_EQ(BinaryenModuleGetFeatures(bare), BinaryenFeatureMVP());

  WasmModule adopted = WasmModule::Adopt(bare);
  const BinaryenFeatures feats = BinaryenModuleGetFeatures(adopted.raw());
  EXPECT_NE(feats & BinaryenFeatureAtomics(), 0u);
  EXPECT_NE(feats & BinaryenFeatureReferenceTypes(), 0u);
  EXPECT_EQ(feats & ExpectedDefaultFeatures(), ExpectedDefaultFeatures());
}

TEST(WasmModuleAdoptTest, UnionPreservesAdoptedExtraFeatures) {
  // NontrappingFPToInt is deliberately OUTSIDE the default set: the
  // runtime wasm is built with clang flags that enable more than our
  // defaults, and Adopt must take the UNION, not overwrite.  Narrowing
  // an adopted module's features trips Binaryen's feature-dependency
  // assertions (e.g. BulkMemoryOpt implies BulkMemory).
  BinaryenModuleRef bare = BinaryenModuleCreate();
  BinaryenModuleSetFeatures(bare, BinaryenFeatureNontrappingFPToInt());

  WasmModule adopted = WasmModule::Adopt(bare);
  const BinaryenFeatures feats = BinaryenModuleGetFeatures(adopted.raw());
  EXPECT_NE(feats & BinaryenFeatureNontrappingFPToInt(), 0u)
      << "Adopt dropped a feature the adopted module declared; the "
         "feature-set union is load-bearing.";
  EXPECT_EQ(feats & ExpectedDefaultFeatures(), ExpectedDefaultFeatures());
}

TEST(WasmModuleAdoptTest, DestructorDisposesAdoptedModule) {
  // Ownership contract: Adopt takes the BinaryenModuleRef; destruction
  // disposes it exactly once.  Passing under the normal test run (and
  // under ASAN/LSAN when enabled) is the assertion — a double-dispose
  // crashes here, a missed dispose leaks the module's arena.
  {
    WasmModule adopted = WasmModule::Adopt(BinaryenModuleCreate());
    EXPECT_THAT(adopted.Validate(), IsOk());
  }  // `adopted` destructs; the adopted ref must be disposed exactly once.
}

// ── WasmModule::AddActiveDataSegment ─────────────────────────────

TEST(WasmModuleDataSegmentTest, LandsOnDefaultMemoryAndRoundTrips) {
  WasmModule m;
  ASSERT_THAT(m.AddMemoryImport("cel", "memory", 1, std::nullopt), IsOk());
  const std::vector<uint8_t> payload = {0xCA, 0xFE, 0xF0, 0x0D};
  m.AddActiveDataSegment(/*offset=*/32, payload);
  EXPECT_THAT(m.Validate(), IsOk());

  ASSERT_EQ(BinaryenGetNumMemorySegments(m.raw()), 1u);
  BinaryenDataSegmentRef seg = BinaryenGetDataSegmentByIndex(m.raw(), 0);
  ASSERT_NE(seg, nullptr);
  EXPECT_FALSE(BinaryenGetMemorySegmentPassive(seg));
  EXPECT_EQ(BinaryenGetMemorySegmentByteOffset(m.raw(), seg), 32u);
  ASSERT_EQ(BinaryenGetMemorySegmentByteLength(seg), payload.size());
  std::vector<char> out(payload.size());
  BinaryenCopyMemorySegmentData(seg, out.data());
  EXPECT_EQ(std::memcmp(out.data(), payload.data(), payload.size()), 0);
}

TEST(WasmModuleDataSegmentTest, AppendsAlongsideExistingSegments) {
  // The kStatic-path shape: the memory's init data was already
  // committed through the memory install's segments array;
  // AddActiveDataSegment appends alongside it rather than replacing it.
  WasmModule m;
  const std::vector<uint8_t> committed = {0x01, 0x02};
  WasmModule::DataSegment seg{/*offset=*/16, committed};
  ASSERT_THAT(m.AddMemoryImport("cel", "memory", 1, std::nullopt,
                                absl::MakeConstSpan(&seg, 1)),
              IsOk());
  ASSERT_EQ(BinaryenGetNumMemorySegments(m.raw()), 1u);

  const std::vector<uint8_t> appended = {0x03, 0x04, 0x05};
  m.AddActiveDataSegment(/*offset=*/64, appended);
  EXPECT_EQ(BinaryenGetNumMemorySegments(m.raw()), 2u);
  EXPECT_THAT(m.Validate(), IsOk());
}

TEST(WasmModuleDataSegmentTest, LandsOnAdoptedMemoryNamedZero) {
  // The adopted wasi-libc runtime's memory is named "0" (Binaryen's
  // default name for memories read from a binary), not "memory".  Pin
  // that the `memory_name` parameter reaches the emitted segment.
  BinaryenModuleRef raw = BinaryenModuleCreate();
  BinaryenSetMemory(raw, /*initial=*/1, /*maximum=*/1, /*exportName=*/nullptr,
                    /*segmentNames=*/nullptr, /*segmentDatas=*/nullptr,
                    /*segmentPassives=*/nullptr, /*segmentOffsets=*/nullptr,
                    /*segmentSizes=*/nullptr, /*numSegments=*/0,
                    /*shared=*/false, /*memory64=*/false, /*name=*/"0");
  WasmModule adopted = WasmModule::Adopt(raw);
  ASSERT_TRUE(BinaryenHasMemory(adopted.raw()));

  const std::vector<uint8_t> payload = {0xAB, 0xCD};
  adopted.AddActiveDataSegment(/*offset=*/8, payload, /*memory_name=*/"0");
  EXPECT_THAT(adopted.Validate(), IsOk());

  ASSERT_EQ(BinaryenGetNumMemorySegments(adopted.raw()), 1u);
  BinaryenDataSegmentRef seg = BinaryenGetDataSegmentByIndex(adopted.raw(), 0);
  ASSERT_NE(seg, nullptr);
  EXPECT_EQ(BinaryenGetMemorySegmentByteOffset(adopted.raw(), seg), 8u);
  ASSERT_EQ(BinaryenGetMemorySegmentByteLength(seg), payload.size());
  std::vector<char> out(payload.size());
  BinaryenCopyMemorySegmentData(seg, out.data());
  EXPECT_EQ(std::memcmp(out.data(), payload.data(), payload.size()), 0);
}

TEST(WasmModuleDataSegmentTest, EmptyBytesSpanProducesEmptyValidSegment) {
  // Pins current behaviour: an empty payload yields a zero-length
  // active segment that Binaryen accepts (the wasm spec permits
  // size-0 active segments; the instantiator writes nothing).
  WasmModule m;
  ASSERT_THAT(m.AddMemoryImport("cel", "memory", 1, std::nullopt), IsOk());
  m.AddActiveDataSegment(/*offset=*/16, {});
  ASSERT_EQ(BinaryenGetNumMemorySegments(m.raw()), 1u);
  BinaryenDataSegmentRef seg = BinaryenGetDataSegmentByIndex(m.raw(), 0);
  ASSERT_NE(seg, nullptr);
  EXPECT_EQ(BinaryenGetMemorySegmentByteLength(seg), 0u);
  EXPECT_THAT(m.Validate(), IsOk());
}

}  // namespace
}  // namespace celwasm
