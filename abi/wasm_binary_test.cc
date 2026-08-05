// Tests for the wasm binary-format layer.  Covers the matrix from
// feature-pipeline-checklist §2.7: core-module positive; preamble
// negatives (empty / truncated / wrong magic / wrong version /
// component preamble); LEB truncation and overrun; section size past
// EOF; duplicate name; missing section; zero-length payload;
// multi-byte UTF-8 name; the no-recursion pin (complete core module
// nested inside an opaque section payload); Append → Find round-trip;
// BuildCustomSection framing bytes asserted literally.

#include "abi/wasm_binary.h"

#include <cstdint>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// `\0asm` + version 0x00000001 — a minimal (empty) core module.
const std::vector<uint8_t> kCorePreamble = {0x00, 0x61, 0x73, 0x6d,
                                            0x01, 0x00, 0x00, 0x00};
// `\0asm` + version/layer word 0x0001000d — a Component-Model
// component preamble.  Kept as a negative case: components are not
// core modules, and every entry point here rejects them.
const std::vector<uint8_t> kComponentPreamble = {0x00, 0x61, 0x73, 0x6d,
                                                 0x0d, 0x00, 0x01, 0x00};

std::vector<uint8_t> Concat(std::vector<uint8_t> a,
                            absl::Span<const uint8_t> b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// --- IsCoreModule -------------------------------------------------

TEST(IsCoreModuleTest, CoreModulePreamble) {
  EXPECT_TRUE(IsCoreModule(kCorePreamble));
}

TEST(IsCoreModuleTest, ComponentPreambleIsNotACoreModule) {
  EXPECT_FALSE(IsCoreModule(kComponentPreamble));
}

TEST(IsCoreModuleTest, EmptyBytes) {
  EXPECT_FALSE(IsCoreModule({}));
}

TEST(IsCoreModuleTest, TruncatedPreamble) {
  const std::vector<uint8_t> bytes = {0x00, 0x61, 0x73};
  EXPECT_FALSE(IsCoreModule(bytes));
}

TEST(IsCoreModuleTest, SevenBytePreambleIsTruncated) {
  const std::vector<uint8_t> bytes = {0x00, 0x61, 0x73, 0x6d,
                                      0x01, 0x00, 0x00};
  EXPECT_FALSE(IsCoreModule(bytes));
}

TEST(IsCoreModuleTest, WrongMagic) {
  const std::vector<uint8_t> bytes = {0xff, 0xff, 0xff, 0xff,
                                      0x01, 0x00, 0x00, 0x00};
  EXPECT_FALSE(IsCoreModule(bytes));
}

TEST(IsCoreModuleTest, UnknownVersionWord) {
  const std::vector<uint8_t> bytes = {0x00, 0x61, 0x73, 0x6d,
                                      0x02, 0x00, 0x00, 0x00};
  EXPECT_FALSE(IsCoreModule(bytes));
}

// --- ReadLeb128U32 / AppendLeb128U32 ------------------------------

TEST(Leb128Test, ReadsSingleByte) {
  const std::vector<uint8_t> bytes = {0x2a};
  size_t pos = 0;
  uint32_t out = 0;
  ASSERT_TRUE(ReadLeb128U32(bytes, &pos, &out));
  EXPECT_EQ(out, 42u);
  EXPECT_EQ(pos, 1u);
}

TEST(Leb128Test, ReadsMultiByte) {
  // 624485 = 0xE5 0x8E 0x26 (the canonical LEB128 example).
  const std::vector<uint8_t> bytes = {0xe5, 0x8e, 0x26};
  size_t pos = 0;
  uint32_t out = 0;
  ASSERT_TRUE(ReadLeb128U32(bytes, &pos, &out));
  EXPECT_EQ(out, 624485u);
  EXPECT_EQ(pos, 3u);
}

TEST(Leb128Test, ReadsUint32Max) {
  const std::vector<uint8_t> bytes = {0xff, 0xff, 0xff, 0xff, 0x0f};
  size_t pos = 0;
  uint32_t out = 0;
  ASSERT_TRUE(ReadLeb128U32(bytes, &pos, &out));
  EXPECT_EQ(out, 0xffffffffu);
  EXPECT_EQ(pos, 5u);
}

TEST(Leb128Test, FailsOnTruncatedEncoding) {
  // Continuation bit set, then EOF.
  const std::vector<uint8_t> bytes = {0x80};
  size_t pos = 0;
  uint32_t out = 0;
  EXPECT_FALSE(ReadLeb128U32(bytes, &pos, &out));
}

TEST(Leb128Test, FailsOnEmptyInput) {
  size_t pos = 0;
  uint32_t out = 0;
  EXPECT_FALSE(ReadLeb128U32({}, &pos, &out));
}

TEST(Leb128Test, FailsOnOverlongEncoding) {
  // Six continuation bytes — a u32 fits in at most five.
  const std::vector<uint8_t> bytes = {0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
  size_t pos = 0;
  uint32_t out = 0;
  EXPECT_FALSE(ReadLeb128U32(bytes, &pos, &out));
}

TEST(Leb128Test, AppendEncodesKnownBytes) {
  std::vector<uint8_t> out;
  AppendLeb128U32(out, 0);
  EXPECT_EQ(out, (std::vector<uint8_t>{0x00}));
  out.clear();
  AppendLeb128U32(out, 127);
  EXPECT_EQ(out, (std::vector<uint8_t>{0x7f}));
  out.clear();
  AppendLeb128U32(out, 128);
  EXPECT_EQ(out, (std::vector<uint8_t>{0x80, 0x01}));
  out.clear();
  AppendLeb128U32(out, 624485);
  EXPECT_EQ(out, (std::vector<uint8_t>{0xe5, 0x8e, 0x26}));
}

TEST(Leb128Test, AppendReadRoundTrip) {
  for (const uint32_t value : {0u, 1u, 127u, 128u, 16384u, 624485u,
                               0x0fffffffu, 0xffffffffu}) {
    std::vector<uint8_t> bytes;
    AppendLeb128U32(bytes, value);
    size_t pos = 0;
    uint32_t out = 0;
    ASSERT_TRUE(ReadLeb128U32(bytes, &pos, &out)) << value;
    EXPECT_EQ(out, value);
    EXPECT_EQ(pos, bytes.size()) << value;
  }
}

// --- BuildCustomSection -------------------------------------------

TEST(BuildCustomSectionTest, FramesKnownBytesLiterally) {
  // name "cel.fns" (7 bytes), payload {0xAA, 0xBB}:
  //   id 0x00, section size 0x0a (1 name-len byte + 7 name + 2
  //   payload), name length 0x07, name, payload.
  const std::vector<uint8_t> payload = {0xaa, 0xbb};
  EXPECT_EQ(BuildCustomSection("cel.fns", payload),
            (std::vector<uint8_t>{0x00, 0x0a, 0x07, 'c', 'e', 'l', '.', 'f',
                                  'n', 's', 0xaa, 0xbb}));
}

TEST(BuildCustomSectionTest, EmptyNameEmptyPayload) {
  // id 0x00, size 0x01 (just the name-len byte), name len 0x00.
  EXPECT_EQ(BuildCustomSection("", {}),
            (std::vector<uint8_t>{0x00, 0x01, 0x00}));
}

// --- FindCustomSection --------------------------------------------

std::vector<uint8_t> WithSection(absl::Span<const uint8_t> preamble,
                                 absl::string_view name,
                                 absl::Span<const uint8_t> payload) {
  return Concat({preamble.begin(), preamble.end()},
                BuildCustomSection(name, payload));
}

TEST(FindCustomSectionTest, FindsOnCoreModule) {
  const std::vector<uint8_t> payload = {1, 2, 3};
  const auto wasm = WithSection(kCorePreamble, "cel.abi", payload);
  auto found = FindCustomSection(wasm, "cel.abi");
  ASSERT_THAT(found, IsOk());
  EXPECT_EQ(std::vector<uint8_t>(found->begin(), found->end()), payload);
  // Zero-copy: the span points into the input buffer.
  EXPECT_GE(found->data(), wasm.data());
  EXPECT_LE(found->data() + found->size(), wasm.data() + wasm.size());
}

TEST(FindCustomSectionTest, InvalidArgumentOnComponentPreamble) {
  // Components are not core modules; the walker refuses them up
  // front rather than walking a layer it no longer supports.
  const auto wasm = WithSection(kComponentPreamble, "cel.fns", {9, 8});
  EXPECT_THAT(FindCustomSection(wasm, "cel.fns"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FindCustomSectionTest, FindsZeroLengthPayload) {
  const auto wasm = WithSection(kCorePreamble, "empty", {});
  auto found = FindCustomSection(wasm, "empty");
  ASSERT_THAT(found, IsOk());
  EXPECT_TRUE(found->empty());
}

TEST(FindCustomSectionTest, FindsMultiByteUtf8Name) {
  // "célfn" — 'é' is 0xC3 0xA9; name length counts BYTES (6), not
  // code points.
  const absl::string_view name = "c\xc3\xa9lfn";
  const std::vector<uint8_t> payload = {0x01};
  const auto wasm = WithSection(kCorePreamble, name, payload);
  auto found = FindCustomSection(wasm, name);
  ASSERT_THAT(found, IsOk());
  EXPECT_EQ(std::vector<uint8_t>(found->begin(), found->end()), payload);
}

TEST(FindCustomSectionTest, SkipsNonCustomAndOtherNamedSections) {
  // Preamble, a non-custom section (id 5, 3-byte opaque payload), a
  // differently-named custom section, then the target.
  std::vector<uint8_t> wasm = kCorePreamble;
  wasm.push_back(0x05);
  wasm.push_back(0x03);
  wasm.insert(wasm.end(), {0xde, 0xad, 0xbe});
  wasm = Concat(std::move(wasm), BuildCustomSection("other", {0x00}));
  const std::vector<uint8_t> payload = {0x42};
  wasm = Concat(std::move(wasm), BuildCustomSection("cel.abi", payload));
  auto found = FindCustomSection(wasm, "cel.abi");
  ASSERT_THAT(found, IsOk());
  EXPECT_EQ(std::vector<uint8_t>(found->begin(), found->end()), payload);
}

TEST(FindCustomSectionTest, NotFoundOnBarePreamble) {
  EXPECT_THAT(FindCustomSection(kCorePreamble, "cel.abi"),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(FindCustomSectionTest, NotFoundWhenOnlyOtherNamesPresent) {
  const auto wasm = WithSection(kCorePreamble, "name", {0x01});
  EXPECT_THAT(FindCustomSection(wasm, "cel.abi"),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(FindCustomSectionTest, InvalidArgumentOnEmptyBytes) {
  EXPECT_THAT(FindCustomSection({}, "cel.abi"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FindCustomSectionTest, InvalidArgumentOnTruncatedPreamble) {
  const std::vector<uint8_t> bytes = {0x00, 0x61, 0x73};
  EXPECT_THAT(FindCustomSection(bytes, "cel.abi"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FindCustomSectionTest, InvalidArgumentOnWrongMagic) {
  const std::vector<uint8_t> bytes = {0xff, 0xff, 0xff, 0xff,
                                      0x01, 0x00, 0x00, 0x00};
  EXPECT_THAT(FindCustomSection(bytes, "cel.abi"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FindCustomSectionTest, InvalidArgumentOnTruncatedSectionSizeLeb) {
  // Section id then a LEB with its continuation bit set at EOF.
  auto wasm = kCorePreamble;
  wasm.push_back(0x00);
  wasm.push_back(0x80);
  EXPECT_THAT(FindCustomSection(wasm, "cel.abi"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FindCustomSectionTest, InvalidArgumentOnOverlongSectionSizeLeb) {
  auto wasm = kCorePreamble;
  wasm.push_back(0x00);
  wasm.insert(wasm.end(), {0x80, 0x80, 0x80, 0x80, 0x80, 0x00});
  EXPECT_THAT(FindCustomSection(wasm, "cel.abi"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FindCustomSectionTest, InvalidArgumentOnSectionSizePastEof) {
  auto wasm = kCorePreamble;
  wasm.push_back(0x00);
  wasm.push_back(0x64);  // claims 100 bytes follow
  wasm.insert(wasm.end(), {0x00, 0x00, 0x00, 0x00, 0x00});
  EXPECT_THAT(FindCustomSection(wasm, "cel.abi"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FindCustomSectionTest, InvalidArgumentOnNameLengthPastSectionEnd) {
  // Custom section of size 2 whose name-length byte claims 100.
  auto wasm = kCorePreamble;
  wasm.push_back(0x00);
  wasm.push_back(0x02);
  wasm.push_back(0x64);  // name length 100 > 1 remaining byte
  wasm.push_back(0x61);
  EXPECT_THAT(FindCustomSection(wasm, "cel.abi"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FindCustomSectionTest, InvalidArgumentOnDuplicateName) {
  auto wasm = WithSection(kCorePreamble, "cel.abi", {0x01});
  wasm = Concat(std::move(wasm), BuildCustomSection("cel.abi", {0x02}));
  EXPECT_THAT(FindCustomSection(wasm, "cel.abi"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FindCustomSectionTest, DoesNotRecurseIntoSectionPayloads) {
  // A core module whose opaque id-5 section payload is itself a
  // complete core module carrying a "cel.fns" custom section.  The
  // top-level walker must NOT find the nested section.
  const auto nested = WithSection(kCorePreamble, "cel.fns", {0x01});
  std::vector<uint8_t> outer = kCorePreamble;
  outer.push_back(0x05);  // an opaque non-custom section
  AppendLeb128U32(outer, static_cast<uint32_t>(nested.size()));
  outer = Concat(std::move(outer), nested);
  EXPECT_THAT(FindCustomSection(outer, "cel.fns"),
              StatusIs(absl::StatusCode::kNotFound));

  // A top-level section with the same name IS found, and the nested
  // one still isn't (payloads differ).
  const std::vector<uint8_t> top_payload = {0xaa};
  const auto with_top =
      Concat(outer, BuildCustomSection("cel.fns", top_payload));
  auto found = FindCustomSection(with_top, "cel.fns");
  ASSERT_THAT(found, IsOk());
  EXPECT_EQ(std::vector<uint8_t>(found->begin(), found->end()), top_payload);
}

// --- AppendCustomSection ------------------------------------------

TEST(AppendCustomSectionTest, RoundTripsOnCoreModule) {
  const std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
  auto appended = AppendCustomSection(kCorePreamble, "cel.abi", payload);
  ASSERT_THAT(appended, IsOk());
  auto found = FindCustomSection(*appended, "cel.abi");
  ASSERT_THAT(found, IsOk());
  EXPECT_EQ(std::vector<uint8_t>(found->begin(), found->end()), payload);
}

TEST(AppendCustomSectionTest, RejectsComponentPreamble) {
  EXPECT_THAT(AppendCustomSection(kComponentPreamble, "cel.fns", {0x07}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AppendCustomSectionTest, AppendsAfterExistingSections) {
  const auto base = WithSection(kCorePreamble, "other", {0x01});
  auto appended = AppendCustomSection(base, "cel.abi", {0x02});
  ASSERT_THAT(appended, IsOk());
  EXPECT_THAT(FindCustomSection(*appended, "other"), IsOk());
  EXPECT_THAT(FindCustomSection(*appended, "cel.abi"), IsOk());
}

TEST(AppendCustomSectionTest, RejectsBadPreamble) {
  const std::vector<uint8_t> bytes = {0xff, 0xff, 0xff, 0xff,
                                      0x01, 0x00, 0x00, 0x00};
  EXPECT_THAT(AppendCustomSection(bytes, "cel.abi", {}),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(AppendCustomSection({}, "cel.abi", {}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AppendCustomSectionTest, RejectsExistingName) {
  const auto base = WithSection(kCorePreamble, "cel.fns", {0x01});
  EXPECT_THAT(AppendCustomSection(base, "cel.fns", {0x02}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AppendCustomSectionTest, RejectsBrokenFraming) {
  // Preamble plus a truncated section: Append must not blindly
  // append to a binary whose framing it can't walk.
  auto wasm = kCorePreamble;
  wasm.push_back(0x00);
  wasm.push_back(0x64);  // claims 100 bytes; EOF
  EXPECT_THAT(AppendCustomSection(wasm, "cel.abi", {}),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace
}  // namespace celwasm
