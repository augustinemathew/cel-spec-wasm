// Verifies the embedded stripped runtime bytes survive m28's load-bearing
// invariants — the build pipeline correctly produced a wrapper-free
// artifact and the link from .h → .cc → embedded array is intact.
//
// The bytes themselves are produced by `//runtime:strip_command_wrappers`
// running over `cel_runtime_wasm.bin`; this test asserts the symbol the
// compiler will consume actually contains what the design doc promises.

#include "runtime/cel_runtime_stripped_wasm_bytes.h"

#include <cstdint>
#include <cstring>
#include <string_view>

#include "binaryen-c.h"
#include "gtest/gtest.h"
#include "runtime/cel_runtime_wasm_bytes.h"

namespace celwasm {
namespace {

constexpr std::string_view kSuffix = ".command_export";

TEST(CelRuntimeStrippedWasmBytes, SymbolLinks) {
  EXPECT_GT(kCelRuntimeStrippedWasmBytesSize, 0u);
  EXPECT_NE(kCelRuntimeStrippedWasmBytes, nullptr);
  // Wasm files start with `\0asm` magic followed by version 0x01000000.
  ASSERT_GE(kCelRuntimeStrippedWasmBytesSize, 8u);
  EXPECT_EQ(kCelRuntimeStrippedWasmBytes[0], 0x00);
  EXPECT_EQ(kCelRuntimeStrippedWasmBytes[1], 0x61);  // 'a'
  EXPECT_EQ(kCelRuntimeStrippedWasmBytes[2], 0x73);  // 's'
  EXPECT_EQ(kCelRuntimeStrippedWasmBytes[3], 0x6d);  // 'm'
}

TEST(CelRuntimeStrippedWasmBytes, NoCommandExportFunctions) {
  // Parse the embedded bytes via Binaryen and walk the function table;
  // the strip tool's invariant is that NO function has the
  // ".command_export" suffix.
  BinaryenModuleRef m = BinaryenModuleRead(
      const_cast<char*>(
          reinterpret_cast<const char*>(kCelRuntimeStrippedWasmBytes)),
      kCelRuntimeStrippedWasmBytesSize);
  ASSERT_NE(m, nullptr) << "BinaryenModuleRead failed on stripped bytes";

  BinaryenIndex num_functions = BinaryenGetNumFunctions(m);
  EXPECT_GT(num_functions, 0u);
  for (BinaryenIndex i = 0; i < num_functions; ++i) {
    BinaryenFunctionRef fn = BinaryenGetFunctionByIndex(m, i);
    std::string_view name(BinaryenFunctionGetName(fn));
    ASSERT_FALSE(name.size() > kSuffix.size() &&
                 name.substr(name.size() - kSuffix.size()) == kSuffix)
        << "Stripped runtime still contains wrapper function: " << name;
  }
  BinaryenModuleDispose(m);
}

TEST(CelRuntimeStrippedWasmBytes, SmallerThanOriginal) {
  // DCE on the stripped runtime drops the wrapper functions + the
  // __wasm_call_ctors / __wasm_call_dtors chain (no live callers after
  // the export retarget); the stripped artifact must be strictly
  // smaller than the original.
  EXPECT_LT(kCelRuntimeStrippedWasmBytesSize, kCelRuntimeWasmBytesSize)
      << "Stripped runtime is not smaller than original — strip tool "
         "may have no-op'd";
}

}  // namespace
}  // namespace celwasm
