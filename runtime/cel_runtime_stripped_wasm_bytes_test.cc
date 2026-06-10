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
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "abi/runtime_catalogue.h"
#include "binaryen-c.h"
#include "gtest/gtest.h"
#include "runtime/cel_runtime_wasm_bytes.h"

namespace celwasm {
namespace {

constexpr std::string_view kSuffix = ".command_export";

// Parses the embedded stripped bytes into a Binaryen module.  Copies
// into a mutable buffer because `BinaryenModuleRead` takes `char*`
// (it does not retain the input).  Caller disposes the module.
BinaryenModuleRef ReadStrippedModule() {
  std::vector<char> buf(
      reinterpret_cast<const char*>(kCelRuntimeStrippedWasmBytes),
      reinterpret_cast<const char*>(kCelRuntimeStrippedWasmBytes) +
          kCelRuntimeStrippedWasmBytesSize);
  return BinaryenModuleRead(buf.data(), buf.size());
}

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
  BinaryenModuleRef m = ReadStrippedModule();
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

TEST(CelRuntimeStrippedWasmBytes, CatalogueExportsTargetDistinctFunctions) {
  // Pins the strip tool's no-merge invariant: the tool deliberately runs
  // ONLY `remove-unused-module-elements` because a full
  // `BinaryenModuleOptimize` would run `merge-similar-functions`, which
  // collapses semantically-identical helpers (e.g. `cel_dur_to_int_at_v`
  // and `cel_dur_seconds_at_v`) into one body and silently retargets the
  // dead twin's export.  Every helper in the runtime catalogue must
  // (a) still be exported, and (b) resolve to a DISTINCT internal
  // function — no two catalogue exports may share a target.
  BinaryenModuleRef m = ReadStrippedModule();
  ASSERT_NE(m, nullptr) << "BinaryenModuleRead failed on stripped bytes";

  // Export name -> internal target function name, function exports only.
  std::map<std::string, std::string> export_to_target;
  BinaryenIndex num_exports = BinaryenGetNumExports(m);
  for (BinaryenIndex i = 0; i < num_exports; ++i) {
    BinaryenExportRef e = BinaryenGetExportByIndex(m, i);
    if (BinaryenExportGetKind(e) != BinaryenExternalFunction()) continue;
    export_to_target[BinaryenExportGetName(e)] = BinaryenExportGetValue(e);
  }

  ASSERT_FALSE(abi::CelRuntimeHelpers().empty());
  // Internal target name -> first catalogue export seen targeting it.
  std::map<std::string, std::string> target_to_export;
  for (const auto& helper : abi::CelRuntimeHelpers()) {
    auto it = export_to_target.find(helper.name());
    ASSERT_NE(it, export_to_target.end())
        << "catalogue helper missing from stripped runtime exports: "
        << helper.name();
    auto [pos, inserted] = target_to_export.emplace(it->second, helper.name());
    EXPECT_TRUE(inserted)
        << "exports `" << helper.name() << "` and `" << pos->second
        << "` both target internal function `" << it->second
        << "` — a function-merging pass (merge-similar-functions) "
           "collapsed two catalogue helpers";
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
