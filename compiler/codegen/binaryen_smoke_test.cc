// Integration smoke test: proves celwasmc can reach Binaryen's C API
// through Bazel + rules_foreign_cc + the vendored tarball.  If this
// test compiles, links, and passes, the rest of the codegen can be
// written against the same API with confidence.
//
// We do three things:
//   1. Build a trivial module that exports a function `answer()`
//      returning `i32.const 42`.
//   2. Call BinaryenModuleValidate to confirm Binaryen itself is
//      happy with what we built.
//   3. Serialize to bytes and confirm the output starts with the
//      canonical WASM preamble ("\0asm" + version 1).

#include "binaryen-c.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"

namespace celwasm {
namespace {

TEST(BinaryenSmokeTest, BuildValidateAndSerializeEmptyModule) {
  BinaryenModuleRef module = BinaryenModuleCreate();
  ASSERT_NE(module, nullptr);

  // Function type: () -> i32.
  BinaryenType params = BinaryenTypeNone();
  BinaryenType results = BinaryenTypeInt32();

  // Body: return i32.const 42.
  BinaryenExpressionRef body =
      BinaryenConst(module, BinaryenLiteralInt32(42));

  BinaryenAddFunction(module, "answer", params, results,
                      /*varTypes=*/nullptr,
                      /*numVarTypes=*/0,
                      body);
  BinaryenAddFunctionExport(module, "answer", "answer");

  EXPECT_TRUE(BinaryenModuleValidate(module))
      << "module failed BinaryenModuleValidate";

  BinaryenModuleAllocateAndWriteResult written =
      BinaryenModuleAllocateAndWrite(module, /*sourceMapUrl=*/nullptr);
  ASSERT_NE(written.binary, nullptr);
  ASSERT_GE(written.binaryBytes, 8u);

  const auto* bytes = static_cast<const uint8_t*>(written.binary);
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x61);  // 'a'
  EXPECT_EQ(bytes[2], 0x73);  // 's'
  EXPECT_EQ(bytes[3], 0x6D);  // 'm'
  EXPECT_EQ(bytes[4], 0x01);  // version 1
  EXPECT_EQ(bytes[5], 0x00);
  EXPECT_EQ(bytes[6], 0x00);
  EXPECT_EQ(bytes[7], 0x00);

  // Free the heap buffer Binaryen handed us.
  std::free(written.binary);
  if (written.sourceMap != nullptr) std::free(written.sourceMap);
  BinaryenModuleDispose(module);
}

}  // namespace
}  // namespace celwasm
