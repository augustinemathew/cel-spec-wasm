// Tests for abi/plugin_validate — the validation logic shared by
// `Plugin::Load` and `cel embed-decls`.  The callers' pinned message
// texts are asserted in abi/plugin_test.cc and
// tools/cel/run_embed_decls_test.cc; here we pin the helpers'
// contract: which input shape selects which caller-supplied message,
// and the exact composed shape of the all-@plugin violation.

#include "abi/plugin_validate.h"

#include <cstdint>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// `\0asm` + version/layer word 0x0001000d — a minimal CM component.
const std::vector<uint8_t> kComponentPreamble = {0x00, 0x61, 0x73, 0x6d,
                                                 0x0d, 0x00, 0x01, 0x00};
// `\0asm` + version 0x00000001 — a minimal (empty) core module.
const std::vector<uint8_t> kCorePreamble = {0x00, 0x61, 0x73, 0x6d,
                                            0x01, 0x00, 0x00, 0x00};

// --- RequireComponentLayer -----------------------------------------

TEST(RequireComponentLayerTest, ComponentPreamblePasses) {
  EXPECT_THAT(RequireComponentLayer(kComponentPreamble, "core", "preamble"),
              IsOk());
}

TEST(RequireComponentLayerTest, CoreModuleSelectsCoreMessage) {
  EXPECT_THAT(RequireComponentLayer(kCorePreamble, "core-msg", "preamble-msg"),
              StatusIs(absl::StatusCode::kInvalidArgument, "core-msg"));
}

TEST(RequireComponentLayerTest, GarbageSelectsPreambleMessage) {
  const std::vector<uint8_t> garbage = {0xde, 0xad, 0xbe, 0xef};
  EXPECT_THAT(RequireComponentLayer(garbage, "core-msg", "preamble-msg"),
              StatusIs(absl::StatusCode::kInvalidArgument, "preamble-msg"));
}

TEST(RequireComponentLayerTest, EmptyAndTruncatedSelectPreambleMessage) {
  EXPECT_THAT(RequireComponentLayer({}, "core-msg", "preamble-msg"),
              StatusIs(absl::StatusCode::kInvalidArgument, "preamble-msg"));
  // Shorter than the 8-byte preamble.
  const std::vector<uint8_t> truncated = {0x00, 0x61, 0x73, 0x6d};
  EXPECT_THAT(RequireComponentLayer(truncated, "core-msg", "preamble-msg"),
              StatusIs(absl::StatusCode::kInvalidArgument, "preamble-msg"));
}

// --- RequireAllPluginBacked ----------------------------------------

CelType Int() {
  return CelType::Int();
}

TEST(RequireAllPluginBackedTest, AllPluginDeclsPass) {
  auto lib = FunctionLibrary::Builder()
                 .AddPlugin("score", Int(), {})
                 .AddPlugin("rank", Int(), {})
                 .Build();
  ASSERT_THAT(lib, IsOk());
  EXPECT_THAT(RequireAllPluginBacked(*lib, "P: ", "somewhere"), IsOk());
}

TEST(RequireAllPluginBackedTest, EmptyLibraryPassesHere) {
  // The at-least-one-decl policy is per-phase and stays with the
  // caller (Plugin::Load enforces it; embed-decls does not).
  EXPECT_THAT(RequireAllPluginBacked(FunctionLibrary(), "P: ", "somewhere"),
              IsOk());
}

TEST(RequireAllPluginBackedTest, HostDeclRejectedWithComposedMessage) {
  auto lib = FunctionLibrary::Builder().AddHost("upper", Int(), {}).Build();
  ASSERT_THAT(lib, IsOk());
  EXPECT_THAT(RequireAllPluginBacked(*lib, "ctx: ", "embedded here"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "ctx: decl `upper` is @host.-backed — every declaration "
                       "embedded here must be @plugin."));
}

TEST(RequireAllPluginBackedTest, NativeDeclNamesNativeBackend) {
  auto lib = FunctionLibrary::Builder()
                 .SetModuleName("m")
                 .AddCelDefined("twice", Int(), {}, "1 + 1")
                 .Build();
  ASSERT_THAT(lib, IsOk());
  EXPECT_THAT(RequireAllPluginBacked(*lib, "ctx: ", "here"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("`twice` is @native.-backed")));
}

}  // namespace
}  // namespace celwasm
