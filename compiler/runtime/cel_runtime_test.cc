#include "compiler/runtime/cel_runtime.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace celwasm {
namespace {

class RuntimeTest : public ::testing::Test {
 protected:
  // Every test starts from a rewound arena so ordering between tests never
  // affects allocator state. Singletons live in the static region and
  // survive the reset.
  void SetUp() override { cel_reset(); }
};

TEST_F(RuntimeTest, CelValueIsTwentyFourBytes) {
  EXPECT_EQ(sizeof(CelValue), 24u);
}

TEST_F(RuntimeTest, AllocEightByteAligns) {
  uint32_t a = cel_alloc(1);
  uint32_t b = cel_alloc(1);
  uint32_t c = cel_alloc(7);
  ASSERT_NE(a, 0u);
  ASSERT_NE(b, 0u);
  ASSERT_NE(c, 0u);
  EXPECT_EQ(a % 8u, 0u);
  EXPECT_EQ(b % 8u, 0u);
  EXPECT_EQ(c % 8u, 0u);
  EXPECT_EQ(b - a, 8u);
  EXPECT_EQ(c - b, 8u);
}

TEST_F(RuntimeTest, AllocReturnsDistinctOffsets) {
  uint32_t a = cel_alloc(24);
  uint32_t b = cel_alloc(24);
  EXPECT_NE(a, b);
  EXPECT_GE(b, a + 24u);
}

TEST_F(RuntimeTest, AllocZeroFillsMemory) {
  uint32_t off = cel_alloc(32);
  ASSERT_NE(off, 0u);
  uint8_t* base = cel_mem_base();
  for (uint32_t i = 0; i < 32; ++i) {
    EXPECT_EQ(base[off + i], 0u) << "byte " << i << " was not zeroed";
  }
}

TEST_F(RuntimeTest, AllocOutOfMemoryReturnsZero) {
  // Drain with decreasing chunk sizes so any tail the big chunks can't fit
  // also gets consumed. Once the arena is truly full both a medium and a
  // minimum-size allocation must fail rather than advancing bump past limit.
  for (uint32_t step : {4096u, 64u, 8u}) {
    while (cel_alloc(step) != 0) {
    }
  }
  EXPECT_EQ(cel_alloc(16), 0u);
  EXPECT_EQ(cel_alloc(1), 0u);
}

TEST_F(RuntimeTest, ResetRewindsAllocator) {
  uint32_t first = cel_alloc(128);
  ASSERT_NE(first, 0u);
  // Pull out a few more allocations, then reset and make sure the next
  // allocation reuses the same byte range.
  (void)cel_alloc(64);
  (void)cel_alloc(64);
  cel_reset();
  uint32_t after = cel_alloc(128);
  EXPECT_EQ(after, first);
}

TEST_F(RuntimeTest, ResetPreservesSingletons) {
  uint32_t null_off_before = cel_make_null();
  uint32_t true_off_before = cel_make_bool(1);
  uint32_t false_off_before = cel_make_bool(0);
  uint32_t none_off_before = cel_make_optional_none();

  cel_reset();

  EXPECT_EQ(cel_make_null(), null_off_before);
  EXPECT_EQ(cel_make_bool(1), true_off_before);
  EXPECT_EQ(cel_make_bool(0), false_off_before);
  EXPECT_EQ(cel_make_optional_none(), none_off_before);
}

TEST_F(RuntimeTest, ValueAtHandlesZero) {
  EXPECT_EQ(cel_value_at(0), nullptr);
  uint32_t off = cel_make_int(42);
  ASSERT_NE(off, 0u);
  CelValue* v = cel_value_at(off);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, (uint32_t)CEL_INT);
  EXPECT_EQ(v->payload.i, 42);
}

TEST_F(RuntimeTest, NullIsSingleton) {
  uint32_t a = cel_make_null();
  uint32_t b = cel_make_null();
  EXPECT_EQ(a, b);
  CelValue* v = cel_value_at(a);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, (uint32_t)CEL_NULL);
}

TEST_F(RuntimeTest, BoolIsSingleton) {
  uint32_t t1 = cel_make_bool(1);
  uint32_t t2 = cel_make_bool(7);  // any truthy value collapses to true
  uint32_t f1 = cel_make_bool(0);
  uint32_t f2 = cel_make_bool(0);
  EXPECT_EQ(t1, t2);
  EXPECT_EQ(f1, f2);
  EXPECT_NE(t1, f1);
  EXPECT_EQ(cel_value_at(t1)->payload.b, 1);
  EXPECT_EQ(cel_value_at(f1)->payload.b, 0);
  EXPECT_EQ(cel_value_at(t1)->kind, (uint32_t)CEL_BOOL);
  EXPECT_EQ(cel_value_at(f1)->kind, (uint32_t)CEL_BOOL);
}

TEST_F(RuntimeTest, MakeIntRoundTrips) {
  struct Case { int64_t in; };
  std::vector<Case> cases{{0}, {1}, {-1}, {INT64_MAX}, {INT64_MIN}, {12345678}};
  for (const auto& c : cases) {
    uint32_t off = cel_make_int(c.in);
    ASSERT_NE(off, 0u);
    CelValue* v = cel_value_at(off);
    EXPECT_EQ(v->kind, (uint32_t)CEL_INT);
    EXPECT_EQ(v->payload.i, c.in);
  }
}

TEST_F(RuntimeTest, MakeUintRoundTrips) {
  for (uint64_t u : {uint64_t{0}, uint64_t{1}, uint64_t{0xFFFFFFFFu},
                     UINT64_MAX}) {
    uint32_t off = cel_make_uint(u);
    ASSERT_NE(off, 0u);
    CelValue* v = cel_value_at(off);
    EXPECT_EQ(v->kind, (uint32_t)CEL_UINT);
    EXPECT_EQ(v->payload.u, u);
  }
}

TEST_F(RuntimeTest, MakeDoubleRoundTrips) {
  for (double d : {0.0, 1.5, -2.25, 1e100, -1e-100}) {
    uint32_t off = cel_make_double(d);
    ASSERT_NE(off, 0u);
    CelValue* v = cel_value_at(off);
    EXPECT_EQ(v->kind, (uint32_t)CEL_DOUBLE);
    EXPECT_EQ(v->payload.d, d);
  }
}

TEST_F(RuntimeTest, MakeStringCopiesBytes) {
  const char* src = "hello";
  uint32_t off = cel_make_string(src, 5);
  ASSERT_NE(off, 0u);
  CelValue* v = cel_value_at(off);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, (uint32_t)CEL_STRING);
  EXPECT_EQ(v->payload.s.len, 5u);
  ASSERT_NE(v->payload.s.ptr, 0u);
  EXPECT_EQ(std::memcmp(cel_mem_base() + v->payload.s.ptr, src, 5), 0);
}

TEST_F(RuntimeTest, MakeStringEmpty) {
  uint32_t off = cel_make_string("", 0);
  ASSERT_NE(off, 0u);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_STRING);
  EXPECT_EQ(v->payload.s.len, 0u);
  EXPECT_EQ(v->payload.s.ptr, 0u);
}

TEST_F(RuntimeTest, MakeStringViewAliases) {
  // Write some bytes into the arena first, then wrap them via the view
  // constructor. The view must not copy the source bytes.
  uint32_t ptr = cel_alloc(4);
  ASSERT_NE(ptr, 0u);
  std::memcpy(cel_mem_base() + ptr, "\x01\x02\x03\x04", 4);
  uint32_t off = cel_make_string_view(ptr, 4);
  ASSERT_NE(off, 0u);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_STRING);
  EXPECT_EQ(v->payload.s.ptr, ptr);
  EXPECT_EQ(v->payload.s.len, 4u);
}

TEST_F(RuntimeTest, MakeBytesCopiesBytes) {
  uint8_t src[3] = {0x00, 0xFF, 0x7F};
  uint32_t off = cel_make_bytes(src, 3);
  ASSERT_NE(off, 0u);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_BYTES);
  EXPECT_EQ(v->payload.bytes.len, 3u);
  EXPECT_EQ(
      std::memcmp(cel_mem_base() + v->payload.bytes.ptr, src, 3), 0);
}

TEST_F(RuntimeTest, MakeBytesViewAliases) {
  uint32_t ptr = cel_alloc(2);
  ASSERT_NE(ptr, 0u);
  std::memcpy(cel_mem_base() + ptr, "ok", 2);
  uint32_t off = cel_make_bytes_view(ptr, 2);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_BYTES);
  EXPECT_EQ(v->payload.bytes.ptr, ptr);
  EXPECT_EQ(v->payload.bytes.len, 2u);
}

TEST_F(RuntimeTest, MakeMessageCarriesSlot) {
  uint32_t off = cel_make_message(17);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_MESSAGE);
  EXPECT_EQ(v->payload.msg_slot, 17u);
}

TEST_F(RuntimeTest, MakeType) {
  uint32_t off = cel_make_type(5);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_TYPE);
  EXPECT_EQ(v->payload.type_id, 5u);
}

TEST_F(RuntimeTest, MakeDurationCarriesSecondsAndNanos) {
  uint32_t off = cel_make_duration(-5, 250);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_DURATION);
  EXPECT_EQ(v->payload.dur.seconds, -5);
  EXPECT_EQ(v->payload.dur.nanos, 250);
}

TEST_F(RuntimeTest, MakeTimestampCarriesSecondsAndNanos) {
  uint32_t off = cel_make_timestamp(1'700'000'000, 123);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_TIMESTAMP);
  EXPECT_EQ(v->payload.ts.seconds, 1'700'000'000);
  EXPECT_EQ(v->payload.ts.nanos, 123);
}

TEST_F(RuntimeTest, MakeOptionalSomeWrapsInner) {
  uint32_t inner = cel_make_int(9);
  ASSERT_NE(inner, 0u);
  uint32_t off = cel_make_optional_some(inner);
  ASSERT_NE(off, 0u);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_OPTIONAL);
  EXPECT_EQ(v->payload.opt, inner);
}

TEST_F(RuntimeTest, MakeOptionalSomeRejectsZero) {
  // Passing a zero (null) offset would collide with the "absent" encoding,
  // so the constructor refuses it explicitly.
  EXPECT_EQ(cel_make_optional_some(0), 0u);
}

TEST_F(RuntimeTest, MakeOptionalNoneIsSingleton) {
  uint32_t a = cel_make_optional_none();
  uint32_t b = cel_make_optional_none();
  EXPECT_EQ(a, b);
  CelValue* v = cel_value_at(a);
  EXPECT_EQ(v->kind, (uint32_t)CEL_OPTIONAL);
  EXPECT_EQ(v->payload.opt, 0u);
}

TEST_F(RuntimeTest, MakeUnknownCarriesAttributeId) {
  uint32_t off = cel_make_unknown(42);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_UNKNOWN);
  ASSERT_NE(v->payload.unk, 0u);
  const uint32_t* set = reinterpret_cast<const uint32_t*>(
      cel_mem_base() + v->payload.unk);
  uint32_t ids_ptr = set[0];
  uint32_t ids_len = set[1];
  EXPECT_EQ(ids_len, 1u);
  ASSERT_NE(ids_ptr, 0u);
  EXPECT_EQ(*reinterpret_cast<const uint32_t*>(cel_mem_base() + ids_ptr),
            42u);
}

TEST_F(RuntimeTest, MakeErrorCarriesCodeAndMessage) {
  const char* msg = "oops";
  uint32_t msg_ptr = cel_alloc(4);
  ASSERT_NE(msg_ptr, 0u);
  std::memcpy(cel_mem_base() + msg_ptr, msg, 4);

  uint32_t off = cel_make_error(/*code=*/7, msg_ptr, /*msg_len=*/4);
  CelValue* v = cel_value_at(off);
  EXPECT_EQ(v->kind, (uint32_t)CEL_ERROR);
  ASSERT_NE(v->payload.err, 0u);
  const uint32_t* err = reinterpret_cast<const uint32_t*>(
      cel_mem_base() + v->payload.err);
  EXPECT_EQ(err[0], 7u);
  EXPECT_EQ(err[1], msg_ptr);
  EXPECT_EQ(err[2], 4u);
}

TEST_F(RuntimeTest, StringEqEmpty) {
  uint32_t a = cel_make_string("", 0);
  uint32_t b = cel_make_string("", 0);
  EXPECT_EQ(cel_string_eq(a, b), 1);
}

TEST_F(RuntimeTest, StringEqEqual) {
  uint32_t a = cel_make_string("hello", 5);
  uint32_t b = cel_make_string("hello", 5);
  EXPECT_EQ(cel_string_eq(a, b), 1);
}

TEST_F(RuntimeTest, StringEqDifferentLengths) {
  uint32_t a = cel_make_string("hello", 5);
  uint32_t b = cel_make_string("helloo", 6);
  EXPECT_EQ(cel_string_eq(a, b), 0);
}

TEST_F(RuntimeTest, StringEqDifferentContent) {
  uint32_t a = cel_make_string("hello", 5);
  uint32_t b = cel_make_string("world", 5);
  EXPECT_EQ(cel_string_eq(a, b), 0);
}

TEST_F(RuntimeTest, StringEqRejectsNonString) {
  uint32_t s = cel_make_string("x", 1);
  uint32_t b = cel_make_bytes("x", 1);
  EXPECT_EQ(cel_string_eq(s, b), 0);
  EXPECT_EQ(cel_string_eq(b, s), 0);
}

TEST_F(RuntimeTest, StringEqRejectsZeroOffset) {
  uint32_t s = cel_make_string("x", 1);
  EXPECT_EQ(cel_string_eq(0, s), 0);
  EXPECT_EQ(cel_string_eq(s, 0), 0);
}

TEST_F(RuntimeTest, BytesEqEmpty) {
  uint32_t a = cel_make_bytes("", 0);
  uint32_t b = cel_make_bytes("", 0);
  EXPECT_EQ(cel_bytes_eq(a, b), 1);
}

TEST_F(RuntimeTest, BytesEqEqual) {
  uint8_t src[3] = {0, 1, 2};
  uint32_t a = cel_make_bytes(src, 3);
  uint32_t b = cel_make_bytes(src, 3);
  EXPECT_EQ(cel_bytes_eq(a, b), 1);
}

TEST_F(RuntimeTest, BytesEqDifferentContent) {
  uint8_t x[3] = {0, 1, 2};
  uint8_t y[3] = {0, 1, 3};
  uint32_t a = cel_make_bytes(x, 3);
  uint32_t b = cel_make_bytes(y, 3);
  EXPECT_EQ(cel_bytes_eq(a, b), 0);
}

TEST_F(RuntimeTest, BytesEqDifferentLengths) {
  uint8_t x[3] = {0, 1, 2};
  uint8_t y[4] = {0, 1, 2, 3};
  uint32_t a = cel_make_bytes(x, 3);
  uint32_t b = cel_make_bytes(y, 4);
  EXPECT_EQ(cel_bytes_eq(a, b), 0);
}

TEST_F(RuntimeTest, BytesEqRejectsNonBytes) {
  uint32_t a = cel_make_string("x", 1);
  uint32_t b = cel_make_bytes("x", 1);
  EXPECT_EQ(cel_bytes_eq(a, b), 0);
}

}  // namespace
}  // namespace celwasm
