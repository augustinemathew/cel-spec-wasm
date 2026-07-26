// Tests for `Plugin::Load` — the self-describing plugin artifact.
//
// Matrix (m35-plugin-ergonomics.md §3.1): happy path on BOTH a
// hand-framed component (preamble + BuildCustomSection) and the real
// macro-built demo plugin (consumed as built — the macro's
// `cel embed-decls` step embeds the section); empty
// bytes; core-module bytes (message flags the core-module case);
// non-wasm bytes; missing / duplicate `cel.fns`; truncated framing;
// non-UTF-8 payload (invalid lead byte + overlong encoding); `.celfn`
// parse error surfacing line+col; @host. / @native. decls (named in
// the error); zero decls; wit_interface derivation incl. the
// `customfn` fallback; hash stability / divergence / concatenation
// order (bytes ‖ source, pinned against //abi/internal:sha256);
// hash_hex format; accessor round-trips.

#include "abi/plugin.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "abi/internal/sha256.h"
#include "abi/wasm_binary.h"
#include "absl/log/absl_check.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/celfn/function_library.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace celwasm {
namespace {

using ::absl_testing::StatusIs;
using ::bazel::tools::cpp::runfiles::Runfiles;
using ::testing::AllOf;
using ::testing::HasSubstr;

// `\0asm` + version/layer word 0x0001000d — a minimal CM component.
const std::vector<uint8_t> kComponentPreamble = {0x00, 0x61, 0x73, 0x6d,
                                                 0x0d, 0x00, 0x01, 0x00};
// `\0asm` + version 0x00000001 — a minimal (empty) core module.
const std::vector<uint8_t> kCorePreamble = {0x00, 0x61, 0x73, 0x6d,
                                            0x01, 0x00, 0x00, 0x00};

constexpr absl::string_view kScorerIdl =
    "Module scorer;\n"
    "\n"
    "int @plugin.score(string s);\n"
    "bool @plugin.allow(int score, int threshold);\n";

absl::Span<const uint8_t> AsBytes(absl::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// Hand-framed component: preamble + a `cel.fns` section carrying
// `idl` verbatim.
std::vector<uint8_t> MakePluginBytes(absl::string_view idl) {
  std::vector<uint8_t> bytes = kComponentPreamble;
  const std::vector<uint8_t> section =
      BuildCustomSection("cel.fns", AsBytes(idl));
  bytes.insert(bytes.end(), section.begin(), section.end());
  return bytes;
}

// The real `cel_wasm_plugin`-macro-built component, `cel.fns`
// section included (embedded by the macro's `cel embed-decls` step).
std::vector<uint8_t> LoadDemoPluginBytes() {
  std::string error;
  auto runfiles = absl::WrapUnique(Runfiles::CreateForTest(&error));
  ABSL_CHECK(runfiles != nullptr) << "runfiles init failed: " << error;
  const std::string path = runfiles->Rlocation(
      "_main/e2e/plugin_fixtures/cel_wasm_plugin_demo/"
      "demo_plugin.wasm");
  ABSL_CHECK(!path.empty()) << "demo_plugin.wasm not in runfiles";
  std::ifstream f(path, std::ios::binary);
  ABSL_CHECK(f.is_open()) << "failed to open " << path;
  return {(std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>()};
}

// --- Happy paths ---------------------------------------------------

TEST(PluginLoadTest, HandFramedComponentLoads) {
  const std::vector<uint8_t> bytes = MakePluginBytes(kScorerIdl);
  auto plugin = Plugin::Load(bytes);
  ASSERT_TRUE(plugin.ok()) << plugin.status();

  EXPECT_EQ(plugin->celfn_source(), kScorerIdl);
  EXPECT_EQ(plugin->wit_interface(), "cel:scorer/fns@0.1.0");
  ASSERT_EQ(plugin->decls().size(), 2);
  EXPECT_EQ(plugin->decls()[0].fn_name, "score");
  EXPECT_EQ(plugin->decls()[0].overload_id, "score_string");
  EXPECT_EQ(plugin->decls()[0].backend, CelfnDecl::Backend::kPlugin);
  EXPECT_EQ(plugin->decls()[1].fn_name, "allow");
  EXPECT_EQ(plugin->decls()[1].overload_id, "allow_int_int");
}

TEST(PluginLoadTest, RealMacroBuiltComponentLoads) {
  // The macro output is consumed exactly as built: its `cel.fns`
  // section (the verbatim fns.idl bytes, comments included) was
  // embedded by the macro's `cel embed-decls` step — nothing is
  // appended here.  The exact-payload-bytes pin lives in the demo
  // e2e (demo_plugin_e2e_test.cc), which has the idl in runfiles.
  auto plugin = Plugin::Load(LoadDemoPluginBytes());
  ASSERT_TRUE(plugin.ok()) << plugin.status();
  EXPECT_EQ(plugin->wit_interface(), "cel:customfn/fns@0.1.0");
  ASSERT_EQ(plugin->decls().size(), 3);
  EXPECT_EQ(plugin->decls()[0].overload_id, "greet_string_int");
  EXPECT_EQ(plugin->decls()[1].overload_id, "add_int_int");
  EXPECT_EQ(plugin->decls()[2].overload_id, "len_string");
  EXPECT_THAT(plugin->celfn_source(),
              AllOf(HasSubstr("Module customfn;"),
                    HasSubstr("@plugin.greet(string name, int age)"),
                    HasSubstr("@plugin.add(int a, int b)"),
                    HasSubstr("@plugin.len(string s)")));
}

// --- Accessor round-trips ------------------------------------------

TEST(PluginAccessorTest, AccessorsRoundTrip) {
  const std::vector<uint8_t> bytes = MakePluginBytes(kScorerIdl);
  auto plugin = Plugin::Load(bytes);
  ASSERT_TRUE(plugin.ok()) << plugin.status();

  // bytes() is an owned copy of the input, section included.
  ASSERT_EQ(plugin->bytes().size(), bytes.size());
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(),
                         plugin->bytes().begin()));

  // decls() == library().decls().
  ASSERT_EQ(plugin->decls().size(), plugin->library().decls().size());
  EXPECT_EQ(plugin->decls().data(), plugin->library().decls().data());

  // wit_interface() == library().wit_interface().
  EXPECT_EQ(plugin->wit_interface(), plugin->library().wit_interface());
}

// --- Rejections: binary shape --------------------------------------

TEST(PluginLoadTest, EmptyBytesRejected) {
  EXPECT_THAT(Plugin::Load({}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("empty")));
}

TEST(PluginLoadTest, CoreModuleRejectedWithCoreModuleMessage) {
  EXPECT_THAT(Plugin::Load(kCorePreamble),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("core"),
                             HasSubstr("component"))));
}

TEST(PluginLoadTest, NonWasmBytesRejected) {
  const std::vector<uint8_t> garbage = {0xde, 0xad, 0xbe, 0xef,
                                        0x00, 0x00, 0x00, 0x00};
  EXPECT_THAT(Plugin::Load(garbage),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("not a wasm")));
}

// --- Rejections: cel.fns section -----------------------------------

TEST(PluginLoadTest, MissingSectionPointsAtEmbedTooling) {
  EXPECT_THAT(Plugin::Load(kComponentPreamble),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("no cel.fns section"),
                             HasSubstr("cel_wasm_plugin"),
                             HasSubstr("cel embed-decls"))));
}

TEST(PluginLoadTest, DuplicateSectionRejected) {
  std::vector<uint8_t> bytes = MakePluginBytes(kScorerIdl);
  const std::vector<uint8_t> again =
      BuildCustomSection("cel.fns", AsBytes(kScorerIdl));
  bytes.insert(bytes.end(), again.begin(), again.end());
  EXPECT_THAT(Plugin::Load(bytes),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("duplicate")));
}

TEST(PluginLoadTest, TruncatedFramingRejected) {
  // Section id 0x00 + declared size 0x7f, then EOF.
  std::vector<uint8_t> bytes = kComponentPreamble;
  bytes.push_back(0x00);
  bytes.push_back(0x7f);
  EXPECT_THAT(Plugin::Load(bytes),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// --- Rejections: payload text --------------------------------------

TEST(PluginLoadTest, InvalidUtf8LeadByteRejected) {
  const std::vector<uint8_t> payload = {'M', 0xff, ';'};
  std::vector<uint8_t> bytes = kComponentPreamble;
  const std::vector<uint8_t> section = BuildCustomSection("cel.fns", payload);
  bytes.insert(bytes.end(), section.begin(), section.end());
  EXPECT_THAT(Plugin::Load(bytes),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("UTF-8")));
}

TEST(PluginLoadTest, OverlongUtf8Rejected) {
  // 0xC0 0xAF is the classic overlong encoding of '/'.
  const std::vector<uint8_t> payload = {0xc0, 0xaf};
  std::vector<uint8_t> bytes = kComponentPreamble;
  const std::vector<uint8_t> section = BuildCustomSection("cel.fns", payload);
  bytes.insert(bytes.end(), section.begin(), section.end());
  EXPECT_THAT(Plugin::Load(bytes),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("UTF-8")));
}

TEST(PluginLoadTest, ParseErrorSurfacesLineAndColumn) {
  // Truncated decl — the ANTLR error listener reports `line N:C`.
  EXPECT_THAT(Plugin::Load(MakePluginBytes("int @plugin.broken(")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("line 1:")));
}

// --- Rejections: decl backends -------------------------------------

TEST(PluginLoadTest, HostBackendDeclRejectedNamingDecl) {
  constexpr absl::string_view kIdl =
      "int @plugin.score(string s);\n"
      "bool @host.check(int a);\n";
  EXPECT_THAT(Plugin::Load(MakePluginBytes(kIdl)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("check"), HasSubstr("@plugin."))));
}

TEST(PluginLoadTest, NativeBackendDeclRejectedNamingDecl) {
  constexpr absl::string_view kIdl =
      "Module m;\n"
      "bool @native.is_empty(string s) = s == \"\";\n";
  EXPECT_THAT(Plugin::Load(MakePluginBytes(kIdl)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("is_empty"), HasSubstr("@plugin."))));
}

TEST(PluginLoadTest, ZeroDeclsRejected) {
  EXPECT_THAT(Plugin::Load(MakePluginBytes("Module m;\n")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("no functions")));
}

// --- wit_interface derivation --------------------------------------

TEST(PluginWitInterfaceTest, DerivedFromModuleDirective) {
  auto plugin = Plugin::Load(MakePluginBytes(kScorerIdl));
  ASSERT_TRUE(plugin.ok()) << plugin.status();
  EXPECT_EQ(plugin->wit_interface(), "cel:scorer/fns@0.1.0");
}

TEST(PluginWitInterfaceTest, FallsBackToCustomfnWithoutModuleDirective) {
  auto plugin = Plugin::Load(MakePluginBytes("int @plugin.f(int a);\n"));
  ASSERT_TRUE(plugin.ok()) << plugin.status();
  EXPECT_EQ(plugin->wit_interface(), "cel:customfn/fns@0.1.0");
}

// --- hash ----------------------------------------------------------

TEST(PluginHashTest, StableAcrossLoadsOfSameInput) {
  const std::vector<uint8_t> bytes = MakePluginBytes(kScorerIdl);
  auto a = Plugin::Load(bytes);
  auto b = Plugin::Load(bytes);
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a->hash(), b->hash());
  EXPECT_EQ(a->hash_hex(), b->hash_hex());
}

TEST(PluginHashTest, DivergesWhenBytesChange) {
  // Same declaration text, different component bytes (an unrelated
  // extra custom section before cel.fns).
  auto with_extra = AppendCustomSection(kComponentPreamble, "other",
                                        AsBytes("x"));
  ASSERT_TRUE(with_extra.ok());
  auto bytes_b =
      AppendCustomSection(*with_extra, "cel.fns", AsBytes(kScorerIdl));
  ASSERT_TRUE(bytes_b.ok());

  auto a = Plugin::Load(MakePluginBytes(kScorerIdl));
  auto b = Plugin::Load(*bytes_b);
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_NE(a->hash(), b->hash());
}

TEST(PluginHashTest, DivergesWhenTextChanges) {
  auto a = Plugin::Load(MakePluginBytes("int @plugin.f(int a);\n"));
  auto b = Plugin::Load(MakePluginBytes("int @plugin.f(int aa);\n"));
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_NE(a->hash(), b->hash());
}

TEST(PluginHashTest, HashIsSha256OfBytesThenSource) {
  // Pins the concatenation order (bytes ‖ celfn_source) against the
  // sha256 primitive directly.
  const std::vector<uint8_t> bytes = MakePluginBytes(kScorerIdl);
  auto plugin = Plugin::Load(bytes);
  ASSERT_TRUE(plugin.ok()) << plugin.status();

  std::vector<uint8_t> concat = bytes;
  const absl::Span<const uint8_t> source = AsBytes(kScorerIdl);
  concat.insert(concat.end(), source.begin(), source.end());
  EXPECT_EQ(plugin->hash(), Sha256(concat));
  EXPECT_EQ(plugin->hash_hex(), Sha256Hex(concat));

  // The reverse order digests differently — the order is load-bearing.
  std::vector<uint8_t> reversed(source.begin(), source.end());
  reversed.insert(reversed.end(), bytes.begin(), bytes.end());
  EXPECT_NE(plugin->hash(), Sha256(reversed));
}

TEST(PluginHashTest, HashHexIs64LowercaseHexChars) {
  auto plugin = Plugin::Load(MakePluginBytes(kScorerIdl));
  ASSERT_TRUE(plugin.ok()) << plugin.status();
  const std::string hex = plugin->hash_hex();
  ASSERT_EQ(hex.size(), 64);
  for (const char c : hex) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
        << "non-lowercase-hex char: " << c;
  }
}

}  // namespace
}  // namespace celwasm
