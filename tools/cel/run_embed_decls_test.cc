// Tests for `cel embed-decls` — EmbedDecls (the pure core) and
// RunEmbedDecls (the file-I/O CLI wrapper).
//
// Matrix (m35-plugin-ergonomics.md §3.4 row 1): happy round-trip
// (output loads via Plugin::Load; celfn_source() == idl bytes);
// deterministic byte-identical output; core-module input (distinct
// message); non-wasm input; idl parse error (line+col preserved);
// @host. / @native. decl (offender named); pre-existing cel.fns
// section; zero-decl idl admitted here but rejected by Plugin::Load
// (the ≥1-decl check is Load's row of the contract); CLI wrapper
// required-flag and file-error paths.

#include "tools/cel/run_embed_decls.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "abi/plugin.h"
#include "abi/wasm_binary.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm::tools::cel {
namespace {

using ::absl_testing::StatusIs;
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

std::string WriteTempFile(absl::string_view name, absl::string_view content) {
  const std::string path = absl::StrCat(::testing::TempDir(), "/", name);
  std::ofstream out(path, std::ios::binary);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return path;
}

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {(std::istreambuf_iterator<char>(in)),
          std::istreambuf_iterator<char>()};
}

// --- EmbedDecls: happy path ----------------------------------------

TEST(EmbedDeclsTest, RoundTripsThroughPluginLoad) {
  auto out = EmbedDecls(kComponentPreamble, kScorerIdl);
  ASSERT_TRUE(out.ok()) << out.status();

  auto plugin = Plugin::Load(*out);
  ASSERT_TRUE(plugin.ok()) << plugin.status();
  EXPECT_EQ(plugin->celfn_source(), kScorerIdl);
  EXPECT_EQ(plugin->wit_interface(), "cel:scorer/fns@0.1.0");
  ASSERT_EQ(plugin->decls().size(), 2);
  EXPECT_EQ(plugin->decls()[0].overload_id, "score_string");
  EXPECT_EQ(plugin->decls()[1].overload_id, "allow_int_int");
}

TEST(EmbedDeclsTest, AppendsVerbatimIdlBytesAsCelFnsSection) {
  auto out = EmbedDecls(kComponentPreamble, kScorerIdl);
  ASSERT_TRUE(out.ok()) << out.status();
  auto payload = FindCustomSection(*out, "cel.fns");
  ASSERT_TRUE(payload.ok()) << payload.status();
  EXPECT_EQ(absl::string_view(reinterpret_cast<const char*>(payload->data()),
                              payload->size()),
            kScorerIdl);
}

TEST(EmbedDeclsTest, DeterministicByteIdenticalOutput) {
  auto a = EmbedDecls(kComponentPreamble, kScorerIdl);
  auto b = EmbedDecls(kComponentPreamble, kScorerIdl);
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(*a, *b);
}

// --- EmbedDecls: input-binary rejections ---------------------------

TEST(EmbedDeclsTest, CoreModuleRejectedWithDistinctMessage) {
  EXPECT_THAT(EmbedDecls(kCorePreamble, kScorerIdl),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("cel embed-decls: "),
                             HasSubstr("core wasm module"),
                             HasSubstr("component"))));
}

TEST(EmbedDeclsTest, NonWasmBytesRejected) {
  const std::vector<uint8_t> garbage = {0xde, 0xad, 0xbe, 0xef,
                                        0x00, 0x00, 0x00, 0x00};
  EXPECT_THAT(EmbedDecls(garbage, kScorerIdl),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("cel embed-decls: "),
                             HasSubstr("not a wasm"))));
}

TEST(EmbedDeclsTest, EmptyBytesRejected) {
  EXPECT_THAT(EmbedDecls({}, kScorerIdl),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("cel embed-decls: ")));
}

// --- EmbedDecls: idl rejections ------------------------------------

TEST(EmbedDeclsTest, IdlParseErrorPreservesLineAndColumn) {
  EXPECT_THAT(EmbedDecls(kComponentPreamble, "int @plugin.broken("),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("cel embed-decls: "),
                             HasSubstr("line 1:"))));
}

TEST(EmbedDeclsTest, HostBackendDeclRejectedNamingDecl) {
  constexpr absl::string_view kIdl =
      "int @plugin.score(string s);\n"
      "bool @host.check(int a);\n";
  EXPECT_THAT(EmbedDecls(kComponentPreamble, kIdl),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("cel embed-decls: "),
                             HasSubstr("check"), HasSubstr("@plugin."))));
}

TEST(EmbedDeclsTest, NativeBackendDeclRejectedNamingDecl) {
  constexpr absl::string_view kIdl =
      "Module m;\n"
      "bool @native.is_empty(string s) = s == \"\";\n";
  EXPECT_THAT(EmbedDecls(kComponentPreamble, kIdl),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("cel embed-decls: "),
                             HasSubstr("is_empty"), HasSubstr("@plugin."))));
}

// --- EmbedDecls: pre-existing section ------------------------------

TEST(EmbedDeclsTest, PreExistingCelFnsSectionRejected) {
  auto once = EmbedDecls(kComponentPreamble, kScorerIdl);
  ASSERT_TRUE(once.ok()) << once.status();
  EXPECT_THAT(EmbedDecls(*once, kScorerIdl),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       AllOf(HasSubstr("cel embed-decls: "),
                             HasSubstr("cel.fns"))));
}

// --- EmbedDecls: contract seam vs Plugin::Load ---------------------

TEST(EmbedDeclsTest, ZeroDeclIdlEmbedsButFailsPluginLoad) {
  // §3.4 row 1 lists no ≥1-decl check for the embed tool; the
  // zero-decl reject is Plugin::Load's row.  Pin the seam.
  auto out = EmbedDecls(kComponentPreamble, "Module m;\n");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(Plugin::Load(*out),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("no functions")));
}

// --- RunEmbedDecls: CLI wrapper ------------------------------------

TEST(RunEmbedDeclsTest, HappyPathWritesLoadablePlugin) {
  const absl::string_view preamble(
      reinterpret_cast<const char*>(kComponentPreamble.data()),
      kComponentPreamble.size());
  EmbedDeclsOptions opts;
  opts.plugin_path = WriteTempFile("in_component.wasm", preamble);
  opts.idl_path = WriteTempFile("fns.idl", kScorerIdl);
  opts.out_path = absl::StrCat(::testing::TempDir(), "/out_plugin.wasm");

  EXPECT_EQ(RunEmbedDecls(opts), 0);

  const std::vector<uint8_t> written = ReadFileBytes(opts.out_path);
  auto expected = EmbedDecls(kComponentPreamble, kScorerIdl);
  ASSERT_TRUE(expected.ok());
  EXPECT_EQ(written, *expected);  // file wrapper is byte-faithful

  auto plugin = Plugin::Load(written);
  ASSERT_TRUE(plugin.ok()) << plugin.status();
  EXPECT_EQ(plugin->celfn_source(), kScorerIdl);
}

TEST(RunEmbedDeclsTest, MissingPluginFlagIsUsageError) {
  EmbedDeclsOptions opts;
  opts.idl_path = WriteTempFile("fns.idl", kScorerIdl);
  opts.out_path = absl::StrCat(::testing::TempDir(), "/unused.wasm");
  EXPECT_EQ(RunEmbedDecls(opts), 2);
}

TEST(RunEmbedDeclsTest, MissingIdlFlagIsUsageError) {
  const absl::string_view preamble(
      reinterpret_cast<const char*>(kComponentPreamble.data()),
      kComponentPreamble.size());
  EmbedDeclsOptions opts;
  opts.plugin_path = WriteTempFile("in_component.wasm", preamble);
  opts.out_path = absl::StrCat(::testing::TempDir(), "/unused.wasm");
  EXPECT_EQ(RunEmbedDecls(opts), 2);
}

TEST(RunEmbedDeclsTest, MissingOutFlagIsUsageError) {
  const absl::string_view preamble(
      reinterpret_cast<const char*>(kComponentPreamble.data()),
      kComponentPreamble.size());
  EmbedDeclsOptions opts;
  opts.plugin_path = WriteTempFile("in_component.wasm", preamble);
  opts.idl_path = WriteTempFile("fns.idl", kScorerIdl);
  EXPECT_EQ(RunEmbedDecls(opts), 2);
}

TEST(RunEmbedDeclsTest, NonexistentPluginFileFails) {
  EmbedDeclsOptions opts;
  opts.plugin_path = absl::StrCat(::testing::TempDir(), "/no_such.wasm");
  opts.idl_path = WriteTempFile("fns.idl", kScorerIdl);
  opts.out_path = absl::StrCat(::testing::TempDir(), "/unused.wasm");
  EXPECT_EQ(RunEmbedDecls(opts), 1);
}

TEST(RunEmbedDeclsTest, NonexistentIdlFileFails) {
  const absl::string_view preamble(
      reinterpret_cast<const char*>(kComponentPreamble.data()),
      kComponentPreamble.size());
  EmbedDeclsOptions opts;
  opts.plugin_path = WriteTempFile("in_component.wasm", preamble);
  opts.idl_path = absl::StrCat(::testing::TempDir(), "/no_such.idl");
  opts.out_path = absl::StrCat(::testing::TempDir(), "/unused.wasm");
  EXPECT_EQ(RunEmbedDecls(opts), 1);
}

TEST(RunEmbedDeclsTest, ValidationFailureFails) {
  const absl::string_view core(
      reinterpret_cast<const char*>(kCorePreamble.data()),
      kCorePreamble.size());
  EmbedDeclsOptions opts;
  opts.plugin_path = WriteTempFile("in_core.wasm", core);
  opts.idl_path = WriteTempFile("fns.idl", kScorerIdl);
  opts.out_path = absl::StrCat(::testing::TempDir(), "/unused.wasm");
  EXPECT_EQ(RunEmbedDecls(opts), 1);
}

}  // namespace
}  // namespace celwasm::tools::cel
