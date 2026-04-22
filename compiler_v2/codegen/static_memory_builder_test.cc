#include "compiler_v2/codegen/static_memory_builder.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/runtime/cel_runtime.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::StatusIs;

// Helpers ---------------------------------------------------------------

uint32_t ReadU32LE(const std::vector<uint8_t>& buf, size_t off) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(buf.at(off + i)) << (i * 8);
  }
  return v;
}

uint64_t ReadU64LE(const std::vector<uint8_t>& buf, size_t off) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(buf.at(off + i)) << (i * 8);
  }
  return v;
}

double ReadDoubleLE(const std::vector<uint8_t>& buf, size_t off) {
  const uint64_t bits = ReadU64LE(buf, off);
  double d;
  std::memcpy(&d, &bits, sizeof(d));
  return d;
}

// Tests ---------------------------------------------------------------

TEST(StaticMemoryBuilderTest, EmptyBuilderFinalizesToEmptyBuffer) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  EXPECT_EQ(builder.size_bytes(), 0u);
  EXPECT_EQ(builder.base_offset(), 16u);
  std::vector<uint8_t> out = std::move(builder).Finalize();
  EXPECT_TRUE(out.empty());
}

TEST(StaticMemoryBuilderTest, AllocateNullIsAllZeros) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const uint32_t off = builder.AllocateNull();
  EXPECT_EQ(off, 16u);
  EXPECT_EQ(builder.size_bytes(), 24u);
  std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 24u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_NULL));
  EXPECT_EQ(ReadU32LE(buf, 4), 0u);  // _pad
  for (size_t i = 8; i < 24; ++i) {
    EXPECT_EQ(buf[i], 0u) << "payload byte " << i << " should be zero";
  }
}

TEST(StaticMemoryBuilderTest, AllocateBoolTrue) {
  StaticMemoryBuilder builder(0);
  builder.AllocateBool(true);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 24u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(ReadU32LE(buf, 4), 0u);
  EXPECT_EQ(ReadU32LE(buf, 8), 1u);  // bool at payload offset 8
  for (size_t i = 12; i < 24; ++i) {
    EXPECT_EQ(buf[i], 0u);
  }
}

TEST(StaticMemoryBuilderTest, AllocateBoolFalse) {
  StaticMemoryBuilder builder(0);
  builder.AllocateBool(false);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 24u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(ReadU32LE(buf, 8), 0u);
}

TEST(StaticMemoryBuilderTest, AllocateIntRoundTrips) {
  StaticMemoryBuilder builder(0);
  builder.AllocateInt(-42);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 24u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 8)), int64_t{-42});
  EXPECT_EQ(ReadU64LE(buf, 16), 0u);
}

TEST(StaticMemoryBuilderTest, AllocateIntExtremes) {
  StaticMemoryBuilder builder(0);
  builder.AllocateInt(INT64_MIN);
  builder.AllocateInt(INT64_MAX);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 48u);
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 8)), INT64_MIN);
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 32)), INT64_MAX);
}

TEST(StaticMemoryBuilderTest, AllocateUintRoundTrips) {
  StaticMemoryBuilder builder(0);
  builder.AllocateUint(UINT64_MAX);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_UINT));
  EXPECT_EQ(ReadU64LE(buf, 8), UINT64_MAX);
}

TEST(StaticMemoryBuilderTest, AllocateDoubleRoundTrips) {
  StaticMemoryBuilder builder(0);
  builder.AllocateDouble(3.5);
  builder.AllocateDouble(-0.0);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 48u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_DOUBLE));
  EXPECT_DOUBLE_EQ(ReadDoubleLE(buf, 8), 3.5);
  EXPECT_DOUBLE_EQ(ReadDoubleLE(buf, 32), -0.0);
  // -0.0 has the sign bit set; verify bit-exact encoding (not just
  // value-equal, since 0.0 == -0.0 via operator==).
  const uint64_t neg_zero_bits = ReadU64LE(buf, 32);
  EXPECT_EQ(neg_zero_bits, uint64_t{1} << 63);
}

TEST(StaticMemoryBuilderTest, ConsecutiveScalarsAreEightByteAligned) {
  StaticMemoryBuilder builder(/*base_offset=*/8);
  const uint32_t off0 = builder.AllocateInt(1);
  const uint32_t off1 = builder.AllocateBool(true);
  const uint32_t off2 = builder.AllocateNull();
  EXPECT_EQ(off0, 8u);
  EXPECT_EQ(off1, 8u + 24u);
  EXPECT_EQ(off2, 8u + 48u);
  EXPECT_EQ(builder.size_bytes(), 72u);
  EXPECT_EQ(off1 % 8u, 0u);
  EXPECT_EQ(off2 % 8u, 0u);
}

TEST(StaticMemoryBuilderTest, AllocateStringHeaderAndPayload) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const uint32_t header_off = builder.AllocateString("hello");
  EXPECT_EQ(header_off, 16u);
  // Header is 24 bytes, span payload is 5 bytes, pad to 8 = 3 bytes → 32 total.
  EXPECT_EQ(builder.size_bytes(), 32u);

  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 32u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(ReadU32LE(buf, 4), 0u);
  // CelSpan { ptr, len } at payload offset 8.
  EXPECT_EQ(ReadU32LE(buf, 8), 16u + 24u);  // base_offset + header_size
  EXPECT_EQ(ReadU32LE(buf, 12), 5u);
  // Tail of 16-byte payload area zero.
  EXPECT_EQ(ReadU64LE(buf, 16), 0u);
  // Span bytes.
  EXPECT_EQ(buf[24], 'h');
  EXPECT_EQ(buf[25], 'e');
  EXPECT_EQ(buf[26], 'l');
  EXPECT_EQ(buf[27], 'l');
  EXPECT_EQ(buf[28], 'o');
  // 3-byte alignment pad.
  EXPECT_EQ(buf[29], 0u);
  EXPECT_EQ(buf[30], 0u);
  EXPECT_EQ(buf[31], 0u);
}

TEST(StaticMemoryBuilderTest, AllocateBytesUsesBytesKind) {
  StaticMemoryBuilder builder(0);
  const char payload[] = {'\x00', '\x01', '\xfe', '\xff'};
  builder.AllocateBytes(absl::string_view(payload, sizeof(payload)));
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  // Header 24 + payload 4 + pad 4 → 32.
  ASSERT_EQ(buf.size(), 32u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(ReadU32LE(buf, 8), 24u);
  EXPECT_EQ(ReadU32LE(buf, 12), 4u);
  EXPECT_EQ(buf[24], 0x00);
  EXPECT_EQ(buf[25], 0x01);
  EXPECT_EQ(buf[26], 0xfe);
  EXPECT_EQ(buf[27], 0xff);
}

TEST(StaticMemoryBuilderTest, AllocateStringEmptyOccupies24Bytes) {
  StaticMemoryBuilder builder(0);
  const uint32_t off = builder.AllocateString("");
  EXPECT_EQ(off, 0u);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  // Header 24 + 0 payload + 0 pad → 24.
  ASSERT_EQ(buf.size(), 24u);
  EXPECT_EQ(ReadU32LE(buf, 8), 24u);  // ptr points one past the header
  EXPECT_EQ(ReadU32LE(buf, 12), 0u);
}

TEST(StaticMemoryBuilderTest,
     AllocateStringAlignmentIsMaintainedAcrossMixedAllocs) {
  StaticMemoryBuilder builder(/*base_offset=*/0);
  builder.AllocateString("ab");  // 24 header + 2 bytes + 6 pad = 32
  const uint32_t off_next = builder.AllocateInt(7);
  EXPECT_EQ(off_next, 32u);
  EXPECT_EQ(off_next % 8u, 0u);
  EXPECT_EQ(builder.size_bytes(), 56u);
}

TEST(StaticMemoryBuilderTest, AllocateStringSevenBytesPadsByOne) {
  StaticMemoryBuilder builder(0);
  builder.AllocateString("1234567");
  // Header 24 + 7 payload + 1 pad → 32.
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  EXPECT_EQ(buf.size(), 32u);
  EXPECT_EQ(buf[31], 0u);
}

TEST(StaticMemoryBuilderTest, BaseOffsetIsAppliedToReturnedOffsetAndSpanPtr) {
  StaticMemoryBuilder builder(/*base_offset=*/128);
  const uint32_t hoff = builder.AllocateString("x");
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  EXPECT_EQ(hoff, 128u);
  // CelSpan.ptr = base_offset + header_size = 128 + 24 = 152.
  EXPECT_EQ(ReadU32LE(buf, 8), 152u);
}

TEST(StaticMemoryBuilderTest, AllocateListReturnsUnimplemented) {
  StaticMemoryBuilder builder(0);
  const uint32_t offs[] = {24u, 48u};
  EXPECT_THAT(builder.AllocateList(absl::MakeSpan(offs)),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(StaticMemoryBuilderTest, AllocateMapReturnsUnimplemented) {
  StaticMemoryBuilder builder(0);
  const uint32_t keys[] = {24u};
  const uint32_t vals[] = {48u};
  EXPECT_THAT(builder.AllocateMap(absl::MakeSpan(keys), absl::MakeSpan(vals)),
              StatusIs(absl::StatusCode::kUnimplemented));
}

}  // namespace
}  // namespace celwasm
