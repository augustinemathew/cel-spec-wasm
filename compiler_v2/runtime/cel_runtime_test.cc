#include "compiler_v2/runtime/cel_runtime.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "gtest/gtest.h"

namespace celwasm {
namespace {

// Fresh-per-test setup.  `cel_reset(arena_base, arena_limit)` writes the
// cursor slot at bytes 8/12; every other test starts from a known state.
class RuntimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Leave [0..16) reserved: 0..7 null sentinel, 8..11 bump, 12..15 limit.
    cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
  }
};

TEST_F(RuntimeTest, CelValueIs24Bytes) {
  EXPECT_EQ(sizeof(CelValue), 24u);
}

TEST_F(RuntimeTest, ResetWritesCursorSlot) {
  cel_reset(/*arena_base=*/128u, /*arena_limit=*/4096u);
  uint32_t bump = 0;
  uint32_t limit = 0;
  std::memcpy(&bump, cel_mem_base() + 8, sizeof(bump));
  std::memcpy(&limit, cel_mem_base() + 12, sizeof(limit));
  EXPECT_EQ(bump, 128u);
  EXPECT_EQ(limit, 4096u);
}

TEST_F(RuntimeTest, AllocBumpsCursorAndReturnsPreBumpOffset) {
  uint32_t a = cel_alloc(24);
  uint32_t b = cel_alloc(24);
  EXPECT_EQ(a, 16u);  // arena_base
  EXPECT_EQ(b, 16u + 24u);
  uint32_t bump = 0;
  std::memcpy(&bump, cel_mem_base() + 8, sizeof(bump));
  EXPECT_EQ(bump, 16u + 48u);
}

TEST_F(RuntimeTest, AllocAlignsToEightBytes) {
  uint32_t a = cel_alloc(1);  // rounds up to 8
  uint32_t b = cel_alloc(0);  // zero rounds up to 8
  EXPECT_EQ(a, 16u);
  EXPECT_EQ(b, 24u);
}

TEST_F(RuntimeTest, AllocReturnsZeroWhenOutOfSpace) {
  cel_reset(/*arena_base=*/16u, /*arena_limit=*/24u);  // 8 usable bytes
  uint32_t a = cel_alloc(8);
  uint32_t b = cel_alloc(8);
  EXPECT_EQ(a, 16u);
  EXPECT_EQ(b, 0u);
}

TEST_F(RuntimeTest, ResetRewindsCursor) {
  (void)cel_alloc(24);
  (void)cel_alloc(24);
  cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
  uint32_t a = cel_alloc(24);
  EXPECT_EQ(a, 16u);
}

TEST_F(RuntimeTest, MakeNull) {
  uint32_t off = cel_make_null();
  ASSERT_NE(off, 0u);
  const CelValue* v = cel_value_at(off);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_NULL));
}

TEST_F(RuntimeTest, MakeBoolBothValues) {
  uint32_t t = cel_make_bool(1);
  uint32_t f = cel_make_bool(0);
  ASSERT_NE(t, 0u);
  ASSERT_NE(f, 0u);
  EXPECT_EQ(cel_value_at(t)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(cel_value_at(t)->payload.b, 1);
  EXPECT_EQ(cel_value_at(f)->payload.b, 0);
}

TEST_F(RuntimeTest, MakeIntRoundTrip) {
  uint32_t a = cel_make_int(INT64_MIN);
  uint32_t b = cel_make_int(INT64_MAX);
  uint32_t c = cel_make_int(-1);
  EXPECT_EQ(cel_value_at(a)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(cel_value_at(a)->payload.i, INT64_MIN);
  EXPECT_EQ(cel_value_at(b)->payload.i, INT64_MAX);
  EXPECT_EQ(cel_value_at(c)->payload.i, -1);
}

TEST_F(RuntimeTest, MakeUintRoundTrip) {
  uint32_t a = cel_make_uint(0);
  uint32_t b = cel_make_uint(UINT64_MAX);
  EXPECT_EQ(cel_value_at(a)->kind, static_cast<uint32_t>(CEL_UINT));
  EXPECT_EQ(cel_value_at(a)->payload.u, 0u);
  EXPECT_EQ(cel_value_at(b)->payload.u, UINT64_MAX);
}

TEST_F(RuntimeTest, MakeDoubleRoundTrip) {
  uint32_t a = cel_make_double(3.14159);
  uint32_t b = cel_make_double(-0.0);
  EXPECT_EQ(cel_value_at(a)->kind, static_cast<uint32_t>(CEL_DOUBLE));
  EXPECT_EQ(cel_value_at(a)->payload.d, 3.14159);
  EXPECT_EQ(cel_value_at(b)->payload.d, -0.0);
}

TEST_F(RuntimeTest, MakeStringCopiesBytes) {
  const char kSrc[] = "hello";
  uint32_t off = cel_make_string(kSrc, sizeof(kSrc) - 1);
  const CelValue* v = cel_value_at(off);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(v->payload.s.len, 5u);
  std::string got(
      reinterpret_cast<const char*>(cel_mem_base()) + v->payload.s.ptr,
      v->payload.s.len);
  EXPECT_EQ(got, "hello");
}

TEST_F(RuntimeTest, MakeBytesCopiesBytes) {
  const uint8_t kSrc[] = {0x00, 0xff, 0x42};
  uint32_t off = cel_make_bytes(kSrc, sizeof(kSrc));
  const CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(v->payload.s.len, 3u);
  const uint8_t* data = cel_mem_base() + v->payload.s.ptr;
  EXPECT_EQ(data[0], 0x00);
  EXPECT_EQ(data[1], 0xff);
  EXPECT_EQ(data[2], 0x42);
}

TEST_F(RuntimeTest, MakeStringViewReusesMemory) {
  const char kSrc[] = "prev";
  uint32_t ptr = cel_alloc(sizeof(kSrc) - 1);
  std::memcpy(cel_mem_base() + ptr, kSrc, sizeof(kSrc) - 1);
  uint32_t off = cel_make_string_view(ptr, sizeof(kSrc) - 1);
  const CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(v->payload.s.ptr, ptr);
  EXPECT_EQ(v->payload.s.len, 4u);
}

TEST_F(RuntimeTest, MakeEmptyStringHasZeroPtr) {
  uint32_t off = cel_make_string(nullptr, 0);
  const CelValue* v = cel_value_at(off);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(v->payload.s.len, 0u);
  EXPECT_EQ(v->payload.s.ptr, 0u);  // no backing alloc for zero-length
}

TEST_F(RuntimeTest, ValueAtZeroReturnsNull) {
  EXPECT_EQ(cel_value_at(0), nullptr);
}

}  // namespace
}  // namespace celwasm
