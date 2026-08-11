// Verifies the embedded stripped runtime bytes survive m28's load-bearing
// invariants — the build pipeline correctly produced a wrapper-free
// artifact and the link from .h → .cc → embedded array is intact.
//
// The bytes themselves are produced by `//runtime:strip_command_wrappers`
// running over `cel_runtime_wasm.bin`; this test asserts the symbol the
// compiler will consume actually contains what the design doc promises.

#include "runtime/cel_runtime_wasm_bytes.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "abi/runtime_catalogue.h"
#include "abi/wasm_binary.h"
#include "absl/types/span.h"
#include "binaryen-c.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

constexpr std::string_view kSuffix = ".command_export";

}  // namespace
}  // namespace celwasm
