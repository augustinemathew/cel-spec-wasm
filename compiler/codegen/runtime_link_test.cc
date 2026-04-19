// Proves the cross-compiled runtime wasm is a valid, Binaryen-readable
// module and that every `cel_*` export the codegen will eventually call
// is present in it.
//
// This is the seam between the runtime (`compiler/runtime/cel_runtime.c`
// cross-compiled to wasm32 via brew's clang) and the codegen path that
// will `BinaryenModuleRead` the bytes and append the per-expression
// `eval` function on top.  M2 doesn't yet wire that merge — the MVP
// scalar expressions don't need any runtime call — but ensuring the
// artefact is sound here keeps M3 from starting from a red build.
//
// The test walks `BinaryenGetExportByIndex` rather than string-matching
// on the raw bytes so a Binaryen version bump that changes the wire
// format still exercises the export shape.

#include <cstddef>
#include <set>
#include <string>

#include "binaryen-c.h"
#include "compiler/runtime/cel_runtime_wasm_bytes.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

BinaryenModuleRef ReadRuntime() {
  // Copy into a mutable buffer — BinaryenModuleRead takes `char*` and
  // may modify it in place.  `kCelRuntimeWasmBytes` is `const`.
  std::string buf(reinterpret_cast<const char*>(kCelRuntimeWasmBytes),
                  kCelRuntimeWasmBytesSize);
  BinaryenFeatures features =
      BinaryenFeatureReferenceTypes() |
      BinaryenFeatureBulkMemory() |
      BinaryenFeatureSignExt() |
      BinaryenFeatureMutableGlobals() |
      BinaryenFeatureMultivalue();
  return BinaryenModuleReadWithFeatures(buf.data(), buf.size(), features);
}

class RuntimeLinkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    module_ = ReadRuntime();
    ASSERT_NE(module_, nullptr);
  }
  void TearDown() override {
    if (module_ != nullptr) {
      BinaryenModuleDispose(module_);
    }
  }
  BinaryenModuleRef module_ = nullptr;
};

TEST_F(RuntimeLinkTest, BytesAreNonEmpty) {
  EXPECT_GT(kCelRuntimeWasmBytesSize, 0u);
  // Wasm preamble: "\0asm" + version 1.
  ASSERT_GE(kCelRuntimeWasmBytesSize, 8u);
  EXPECT_EQ(kCelRuntimeWasmBytes[0], 0x00);
  EXPECT_EQ(kCelRuntimeWasmBytes[1], 0x61);  // 'a'
  EXPECT_EQ(kCelRuntimeWasmBytes[2], 0x73);  // 's'
  EXPECT_EQ(kCelRuntimeWasmBytes[3], 0x6d);  // 'm'
  EXPECT_EQ(kCelRuntimeWasmBytes[4], 0x01);
}

TEST_F(RuntimeLinkTest, ValidatorAcceptsRuntime) {
  EXPECT_TRUE(BinaryenModuleValidate(module_));
}

TEST_F(RuntimeLinkTest, ExportsEveryCelConstructor) {
  std::set<std::string> exports;
  BinaryenIndex n = BinaryenGetNumExports(module_);
  for (BinaryenIndex i = 0; i < n; ++i) {
    BinaryenExportRef e = BinaryenGetExportByIndex(module_, i);
    exports.insert(BinaryenExportGetName(e));
  }

  // Every symbol codegen + the host will reach for.  Omissions here mean
  // the cross-compile dropped a function (dead-stripped, wrong visibility,
  // typoed source, …).  The set is deliberately exhaustive — losing any
  // one of these silently breaks a downstream lowering.
  for (const char* name : {
           "cel_mem_base",
           "cel_mem_size",
           "cel_alloc",
           "cel_reset",
           "cel_value_at",
           "cel_make_null",
           "cel_make_bool",
           "cel_make_int",
           "cel_make_uint",
           "cel_make_double",
           "cel_make_string",
           "cel_make_bytes",
           "cel_make_string_view",
           "cel_make_bytes_view",
           "cel_make_message",
           "cel_make_type",
           "cel_make_duration",
           "cel_make_timestamp",
           "cel_make_optional_some",
           "cel_make_optional_none",
           "cel_make_unknown",
           "cel_make_error",
           "cel_string_eq",
           "cel_bytes_eq",
           "cel_string_concat",
           "cel_string_size",
           "cel_bool_from_value",
           "memory",
       }) {
    EXPECT_EQ(exports.count(name), 1u)
        << "missing export: " << name
        << " (cross-compiled runtime lost a symbol)";
  }
}

TEST_F(RuntimeLinkTest, MemoryExportIsPresentAndNonZeroSize) {
  BinaryenExportRef mem = BinaryenGetExport(module_, "memory");
  ASSERT_NE(mem, nullptr);
  EXPECT_EQ(BinaryenExportGetKind(mem), BinaryenExternalMemory());
}

TEST_F(RuntimeLinkTest, RoundTripsThroughBinaryen) {
  // Serialize the Binaryen-read module and re-read it.  Catches any
  // feature mismatch where BinaryenModuleRead accepts a module the
  // subsequent write cannot reproduce.
  BinaryenModuleAllocateAndWriteResult out =
      BinaryenModuleAllocateAndWrite(module_, /*sourceMapUrl=*/nullptr);
  ASSERT_NE(out.binary, nullptr);
  ASSERT_GT(out.binaryBytes, 0u);

  BinaryenFeatures features =
      BinaryenFeatureReferenceTypes() |
      BinaryenFeatureBulkMemory() |
      BinaryenFeatureSignExt() |
      BinaryenFeatureMutableGlobals() |
      BinaryenFeatureMultivalue();
  BinaryenModuleRef roundtrip = BinaryenModuleReadWithFeatures(
      reinterpret_cast<char*>(out.binary), out.binaryBytes, features);
  EXPECT_NE(roundtrip, nullptr);
  if (roundtrip != nullptr) {
    EXPECT_TRUE(BinaryenModuleValidate(roundtrip));
    BinaryenModuleDispose(roundtrip);
  }
  free(out.binary);
}

}  // namespace
}  // namespace celwasm
