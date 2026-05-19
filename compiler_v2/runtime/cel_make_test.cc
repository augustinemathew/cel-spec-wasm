#include "compiler_v2/runtime/cel_make.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

class MakeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
  }
};

TEST_F(MakeTest, MakeNull) {
  uint32_t off = cel_make_null();
  ASSERT_NE(off, 0u);
  const CelValue* v = cel_value_at(off);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_NULL));
}

TEST_F(MakeTest, MakeBoolBothValues) {
  uint32_t t = cel_make_bool(1);
  uint32_t f = cel_make_bool(0);
  ASSERT_NE(t, 0u);
  ASSERT_NE(f, 0u);
  EXPECT_EQ(cel_value_at(t)->kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(cel_value_at(t)->payload.b, 1);
  EXPECT_EQ(cel_value_at(f)->payload.b, 0);
}

TEST_F(MakeTest, MakeIntRoundTrip) {
  uint32_t a = cel_make_int(INT64_MIN);
  uint32_t b = cel_make_int(INT64_MAX);
  uint32_t c = cel_make_int(-1);
  EXPECT_EQ(cel_value_at(a)->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(cel_value_at(a)->payload.i, INT64_MIN);
  EXPECT_EQ(cel_value_at(b)->payload.i, INT64_MAX);
  EXPECT_EQ(cel_value_at(c)->payload.i, -1);
}

TEST_F(MakeTest, MakeUintRoundTrip) {
  uint32_t a = cel_make_uint(0);
  uint32_t b = cel_make_uint(UINT64_MAX);
  EXPECT_EQ(cel_value_at(a)->kind, static_cast<uint32_t>(CEL_UINT));
  EXPECT_EQ(cel_value_at(a)->payload.u, 0u);
  EXPECT_EQ(cel_value_at(b)->payload.u, UINT64_MAX);
}

TEST_F(MakeTest, MakeDoubleRoundTrip) {
  uint32_t a = cel_make_double(3.14159);
  uint32_t b = cel_make_double(-0.0);
  EXPECT_EQ(cel_value_at(a)->kind, static_cast<uint32_t>(CEL_DOUBLE));
  EXPECT_EQ(cel_value_at(a)->payload.d, 3.14159);
  EXPECT_EQ(cel_value_at(b)->payload.d, -0.0);
}

TEST_F(MakeTest, MakeStringCopiesBytes) {
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

TEST_F(MakeTest, MakeBytesCopiesBytes) {
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

TEST_F(MakeTest, MakeStringViewReusesMemory) {
  const char kSrc[] = "prev";
  uint32_t ptr = arena_alloc(sizeof(kSrc) - 1);
  std::memcpy(cel_mem_base() + ptr, kSrc, sizeof(kSrc) - 1);
  uint32_t off = cel_make_string_view(ptr, sizeof(kSrc) - 1);
  const CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(v->payload.s.ptr, ptr);
  EXPECT_EQ(v->payload.s.len, 4u);
}

TEST_F(MakeTest, MakeBytesViewReusesMemory) {
  const uint8_t kSrc[] = {0xde, 0xad, 0xbe, 0xef};
  uint32_t ptr = arena_alloc(sizeof(kSrc));
  std::memcpy(cel_mem_base() + ptr, kSrc, sizeof(kSrc));
  uint32_t off = cel_make_bytes_view(ptr, sizeof(kSrc));
  const CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(v->payload.s.ptr, ptr);
  EXPECT_EQ(v->payload.s.len, 4u);
}

TEST_F(MakeTest, MakeEmptyStringHasZeroPtr) {
  uint32_t off = cel_make_string(nullptr, 0);
  const CelValue* v = cel_value_at(off);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(v->payload.s.len, 0u);
  EXPECT_EQ(v->payload.s.ptr, 0u);  // no backing alloc for zero-length
}

}  // namespace
}  // namespace celwasm
