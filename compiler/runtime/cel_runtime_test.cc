#include "compiler/runtime/cel_runtime.h"

#include <cmath>
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
  void SetUp() override {
    cel_reset();
  }
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
  struct Case {
    int64_t in;
  };
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
  for (uint64_t u :
       {uint64_t{0}, uint64_t{1}, uint64_t{0xFFFFFFFFu}, UINT64_MAX}) {
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
  EXPECT_EQ(std::memcmp(cel_mem_base() + v->payload.bytes.ptr, src, 3), 0);
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
  const auto* set =
      reinterpret_cast<const uint32_t*>(cel_mem_base() + v->payload.unk);
  uint32_t ids_ptr = set[0];
  uint32_t ids_len = set[1];
  EXPECT_EQ(ids_len, 1u);
  ASSERT_NE(ids_ptr, 0u);
  EXPECT_EQ(*reinterpret_cast<const uint32_t*>(cel_mem_base() + ids_ptr), 42u);
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
  const auto* err =
      reinterpret_cast<const uint32_t*>(cel_mem_base() + v->payload.err);
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

// ---- cel_string_concat -----------------------------------------------------

TEST_F(RuntimeTest, StringConcatJoinsPayloads) {
  uint32_t a = cel_make_string("foo", 3);
  uint32_t b = cel_make_string("bar", 3);
  uint32_t r = cel_string_concat(a, b);
  ASSERT_NE(r, 0u);
  CelValue* v = cel_value_at(r);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, (uint32_t)CEL_STRING);
  EXPECT_EQ(v->payload.s.len, 6u);
  EXPECT_EQ(std::memcmp(cel_mem_base() + v->payload.s.ptr, "foobar", 6), 0);
}

TEST_F(RuntimeTest, StringConcatLeftEmpty) {
  uint32_t a = cel_make_string("", 0);
  uint32_t b = cel_make_string("abc", 3);
  uint32_t r = cel_string_concat(a, b);
  CelValue* v = cel_value_at(r);
  EXPECT_EQ(v->payload.s.len, 3u);
  EXPECT_EQ(std::memcmp(cel_mem_base() + v->payload.s.ptr, "abc", 3), 0);
}

TEST_F(RuntimeTest, StringConcatRightEmpty) {
  uint32_t a = cel_make_string("abc", 3);
  uint32_t b = cel_make_string("", 0);
  uint32_t r = cel_string_concat(a, b);
  CelValue* v = cel_value_at(r);
  EXPECT_EQ(v->payload.s.len, 3u);
  EXPECT_EQ(std::memcmp(cel_mem_base() + v->payload.s.ptr, "abc", 3), 0);
}

TEST_F(RuntimeTest, StringConcatBothEmpty) {
  uint32_t a = cel_make_string("", 0);
  uint32_t b = cel_make_string("", 0);
  uint32_t r = cel_string_concat(a, b);
  ASSERT_NE(r, 0u);
  CelValue* v = cel_value_at(r);
  EXPECT_EQ(v->payload.s.len, 0u);
  EXPECT_EQ(v->payload.s.ptr, 0u);
}

TEST_F(RuntimeTest, StringConcatResultIndependentOfInputs) {
  // Concat must copy into a fresh region so resetting after a subsequent
  // allocation that overwrote the inputs' data would not invalidate the
  // result.  We simulate that by checking that the result's data pointer
  // differs from either input's data pointer.
  uint32_t a = cel_make_string("aaa", 3);
  uint32_t b = cel_make_string("bbb", 3);
  uint32_t r = cel_string_concat(a, b);
  const CelValue* va = cel_value_at(a);
  const CelValue* vb = cel_value_at(b);
  const CelValue* vr = cel_value_at(r);
  EXPECT_NE(vr->payload.s.ptr, va->payload.s.ptr);
  EXPECT_NE(vr->payload.s.ptr, vb->payload.s.ptr);
}

TEST_F(RuntimeTest, StringConcatRejectsZeroOffsets) {
  uint32_t a = cel_make_string("x", 1);
  EXPECT_EQ(cel_string_concat(0, a), 0u);
  EXPECT_EQ(cel_string_concat(a, 0), 0u);
  EXPECT_EQ(cel_string_concat(0, 0), 0u);
}

TEST_F(RuntimeTest, StringConcatRejectsNonStringOperands) {
  // Concat must be strict about type to catch a codegen bug where an int or
  // bytes operand sneaks past the checker's type rules.
  uint32_t s = cel_make_string("x", 1);
  uint32_t bts = cel_make_bytes("y", 1);
  uint32_t i = cel_make_int(7);
  EXPECT_EQ(cel_string_concat(s, bts), 0u);
  EXPECT_EQ(cel_string_concat(bts, s), 0u);
  EXPECT_EQ(cel_string_concat(s, i), 0u);
}

// ---- cel_string_size -------------------------------------------------------

TEST_F(RuntimeTest, StringSizeAscii) {
  uint32_t s = cel_make_string("hello", 5);
  EXPECT_EQ(cel_string_size(s), 5);
}

TEST_F(RuntimeTest, StringSizeEmpty) {
  uint32_t s = cel_make_string("", 0);
  EXPECT_EQ(cel_string_size(s), 0);
}

TEST_F(RuntimeTest, StringSizeCountsCodepointsNotBytes) {
  // "héllo" in UTF-8: h (1) é (0xC3 0xA9, 2 bytes) l l o — 6 bytes, 5 cps.
  const char* src = "h\xC3\xA9llo";
  uint32_t s = cel_make_string(src, 6);
  EXPECT_EQ(cel_string_size(s), 5);
}

TEST_F(RuntimeTest, StringSizeCountsSurrogatePair) {
  // U+1F600 (grinning face) encodes as 4 UTF-8 bytes: F0 9F 98 80. That's
  // one code point even though it occupies four bytes.
  const char* src = "\xF0\x9F\x98\x80";
  uint32_t s = cel_make_string(src, 4);
  EXPECT_EQ(cel_string_size(s), 1);
}

TEST_F(RuntimeTest, StringSizeRejectsZeroOffset) {
  EXPECT_EQ(cel_string_size(0), -1);
}

TEST_F(RuntimeTest, StringSizeRejectsNonString) {
  uint32_t b = cel_make_bytes("x", 1);
  EXPECT_EQ(cel_string_size(b), -1);
  uint32_t i = cel_make_int(9);
  EXPECT_EQ(cel_string_size(i), -1);
}

// ---- cel_string_starts_with / ends_with / contains -------------------------

TEST_F(RuntimeTest, StartsWithTrue) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t p = cel_make_string("he", 2);
  EXPECT_EQ(cel_string_starts_with(s, p), 1);
}

TEST_F(RuntimeTest, StartsWithFalse) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t p = cel_make_string("xy", 2);
  EXPECT_EQ(cel_string_starts_with(s, p), 0);
}

TEST_F(RuntimeTest, StartsWithEmptyPrefixIsTrue) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t p = cel_make_string("", 0);
  EXPECT_EQ(cel_string_starts_with(s, p), 1);
}

TEST_F(RuntimeTest, StartsWithLongerPrefixIsFalse) {
  uint32_t s = cel_make_string("hi", 2);
  uint32_t p = cel_make_string("hello", 5);
  EXPECT_EQ(cel_string_starts_with(s, p), 0);
}

TEST_F(RuntimeTest, StartsWithFullMatch) {
  uint32_t s = cel_make_string("hi", 2);
  uint32_t p = cel_make_string("hi", 2);
  EXPECT_EQ(cel_string_starts_with(s, p), 1);
}

TEST_F(RuntimeTest, StartsWithRejectsZeroOffsets) {
  uint32_t s = cel_make_string("x", 1);
  EXPECT_EQ(cel_string_starts_with(0, s), 0);
  EXPECT_EQ(cel_string_starts_with(s, 0), 0);
}

TEST_F(RuntimeTest, StartsWithRejectsNonString) {
  uint32_t s = cel_make_string("x", 1);
  uint32_t b = cel_make_bytes("x", 1);
  EXPECT_EQ(cel_string_starts_with(s, b), 0);
  EXPECT_EQ(cel_string_starts_with(b, s), 0);
}

TEST_F(RuntimeTest, EndsWithTrue) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t x = cel_make_string("lo", 2);
  EXPECT_EQ(cel_string_ends_with(s, x), 1);
}

TEST_F(RuntimeTest, EndsWithFalse) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t x = cel_make_string("lx", 2);
  EXPECT_EQ(cel_string_ends_with(s, x), 0);
}

TEST_F(RuntimeTest, EndsWithEmptySuffixIsTrue) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t x = cel_make_string("", 0);
  EXPECT_EQ(cel_string_ends_with(s, x), 1);
}

TEST_F(RuntimeTest, EndsWithLongerSuffixIsFalse) {
  uint32_t s = cel_make_string("hi", 2);
  uint32_t x = cel_make_string("hello", 5);
  EXPECT_EQ(cel_string_ends_with(s, x), 0);
}

TEST_F(RuntimeTest, EndsWithFullMatch) {
  uint32_t s = cel_make_string("hi", 2);
  uint32_t x = cel_make_string("hi", 2);
  EXPECT_EQ(cel_string_ends_with(s, x), 1);
}

TEST_F(RuntimeTest, EndsWithRejectsZeroOffsets) {
  uint32_t s = cel_make_string("x", 1);
  EXPECT_EQ(cel_string_ends_with(0, s), 0);
  EXPECT_EQ(cel_string_ends_with(s, 0), 0);
}

TEST_F(RuntimeTest, ContainsTrueMiddle) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t n = cel_make_string("ell", 3);
  EXPECT_EQ(cel_string_contains(s, n), 1);
}

TEST_F(RuntimeTest, ContainsTruePrefix) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t n = cel_make_string("he", 2);
  EXPECT_EQ(cel_string_contains(s, n), 1);
}

TEST_F(RuntimeTest, ContainsTrueSuffix) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t n = cel_make_string("lo", 2);
  EXPECT_EQ(cel_string_contains(s, n), 1);
}

TEST_F(RuntimeTest, ContainsFalse) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t n = cel_make_string("xyz", 3);
  EXPECT_EQ(cel_string_contains(s, n), 0);
}

TEST_F(RuntimeTest, ContainsEmptyNeedleIsTrue) {
  uint32_t s = cel_make_string("hello", 5);
  uint32_t n = cel_make_string("", 0);
  EXPECT_EQ(cel_string_contains(s, n), 1);
}

TEST_F(RuntimeTest, ContainsLongerNeedleIsFalse) {
  uint32_t s = cel_make_string("hi", 2);
  uint32_t n = cel_make_string("hello", 5);
  EXPECT_EQ(cel_string_contains(s, n), 0);
}

TEST_F(RuntimeTest, ContainsFullMatch) {
  uint32_t s = cel_make_string("hi", 2);
  uint32_t n = cel_make_string("hi", 2);
  EXPECT_EQ(cel_string_contains(s, n), 1);
}

TEST_F(RuntimeTest, ContainsRejectsZeroOffsets) {
  uint32_t s = cel_make_string("x", 1);
  EXPECT_EQ(cel_string_contains(0, s), 0);
  EXPECT_EQ(cel_string_contains(s, 0), 0);
}

TEST_F(RuntimeTest, ContainsRejectsNonString) {
  uint32_t s = cel_make_string("x", 1);
  uint32_t b = cel_make_bytes("x", 1);
  EXPECT_EQ(cel_string_contains(s, b), 0);
}

// ---- cel_bytes_concat ------------------------------------------------------

TEST_F(RuntimeTest, BytesConcatJoinsPayloads) {
  uint32_t a = cel_make_bytes("\x01\x02", 2);
  uint32_t b = cel_make_bytes("\x03\xff", 2);
  uint32_t r = cel_bytes_concat(a, b);
  ASSERT_NE(r, 0u);
  CelValue* v = cel_value_at(r);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, (uint32_t)CEL_BYTES);
  EXPECT_EQ(v->payload.s.len, 4u);
  EXPECT_EQ(
      std::memcmp(cel_mem_base() + v->payload.s.ptr, "\x01\x02\x03\xff", 4), 0);
}

TEST_F(RuntimeTest, BytesConcatLeftEmpty) {
  uint32_t a = cel_make_bytes("", 0);
  uint32_t b = cel_make_bytes("\xaa\xbb", 2);
  uint32_t r = cel_bytes_concat(a, b);
  CelValue* v = cel_value_at(r);
  EXPECT_EQ(v->payload.s.len, 2u);
  EXPECT_EQ(std::memcmp(cel_mem_base() + v->payload.s.ptr, "\xaa\xbb", 2), 0);
}

TEST_F(RuntimeTest, BytesConcatRightEmpty) {
  uint32_t a = cel_make_bytes("\xaa\xbb", 2);
  uint32_t b = cel_make_bytes("", 0);
  uint32_t r = cel_bytes_concat(a, b);
  CelValue* v = cel_value_at(r);
  EXPECT_EQ(v->payload.s.len, 2u);
  EXPECT_EQ(std::memcmp(cel_mem_base() + v->payload.s.ptr, "\xaa\xbb", 2), 0);
}

TEST_F(RuntimeTest, BytesConcatBothEmpty) {
  uint32_t a = cel_make_bytes("", 0);
  uint32_t b = cel_make_bytes("", 0);
  uint32_t r = cel_bytes_concat(a, b);
  ASSERT_NE(r, 0u);
  CelValue* v = cel_value_at(r);
  EXPECT_EQ(v->payload.s.len, 0u);
  EXPECT_EQ(v->payload.s.ptr, 0u);
}

TEST_F(RuntimeTest, BytesConcatRejectsZeroOffsets) {
  uint32_t a = cel_make_bytes("x", 1);
  EXPECT_EQ(cel_bytes_concat(0, a), 0u);
  EXPECT_EQ(cel_bytes_concat(a, 0), 0u);
  EXPECT_EQ(cel_bytes_concat(0, 0), 0u);
}

TEST_F(RuntimeTest, BytesConcatRejectsNonBytesOperands) {
  // Bytes-specific so a codegen bug that feeds a string through
  // `cel_bytes_concat` never produces a CEL_BYTES result whose payload
  // contains UTF-8 the rest of the pipeline will misinterpret.
  uint32_t bts = cel_make_bytes("y", 1);
  uint32_t s = cel_make_string("x", 1);
  uint32_t i = cel_make_int(7);
  EXPECT_EQ(cel_bytes_concat(s, bts), 0u);
  EXPECT_EQ(cel_bytes_concat(bts, s), 0u);
  EXPECT_EQ(cel_bytes_concat(bts, i), 0u);
}

// ---- cel_bytes_size --------------------------------------------------------

TEST_F(RuntimeTest, BytesSizeCountsBytes) {
  // size(bytes) is byte count per CEL §1110 — the multi-byte UTF-8
  // sequence that counts as one code point for `cel_string_size` must
  // count as four bytes here.
  uint32_t b = cel_make_bytes("\xF0\x9F\x98\x80", 4);
  EXPECT_EQ(cel_bytes_size(b), 4);
}

TEST_F(RuntimeTest, BytesSizeEmpty) {
  uint32_t b = cel_make_bytes("", 0);
  EXPECT_EQ(cel_bytes_size(b), 0);
}

TEST_F(RuntimeTest, BytesSizeRejectsZeroOffset) {
  EXPECT_EQ(cel_bytes_size(0), -1);
}

TEST_F(RuntimeTest, BytesSizeRejectsNonBytes) {
  uint32_t s = cel_make_string("xyz", 3);
  EXPECT_EQ(cel_bytes_size(s), -1);
  uint32_t i = cel_make_int(9);
  EXPECT_EQ(cel_bytes_size(i), -1);
}

// ---- cel_bool_from_value ---------------------------------------------------

TEST_F(RuntimeTest, BoolFromValueTrue) {
  uint32_t v = cel_make_bool(1);
  EXPECT_EQ(cel_bool_from_value(v), 1);
}

TEST_F(RuntimeTest, BoolFromValueFalse) {
  uint32_t v = cel_make_bool(0);
  EXPECT_EQ(cel_bool_from_value(v), 0);
}

TEST_F(RuntimeTest, BoolFromValueRejectsZeroOffset) {
  EXPECT_EQ(cel_bool_from_value(0), 0);
}

TEST_F(RuntimeTest, BoolFromValueRejectsNonBool) {
  uint32_t i = cel_make_int(1);
  // A non-bool must return 0 rather than, say, sign-extending the int
  // payload's low bit — otherwise a codegen bug that forgot to unwrap
  // via has() would silently produce "1 is true" on every integer.
  EXPECT_EQ(cel_bool_from_value(i), 0);
  uint32_t s = cel_make_string("true", 4);
  EXPECT_EQ(cel_bool_from_value(s), 0);
}

// ---- cel_{int,uint,double}_from_value -------------------------------------
//
// Uniform boxed ABI unbox helpers. Round-trip (make → from) must be the
// identity on the expected kind; kind-mismatch and zero-offset must
// return a null-shaped scalar so a codegen bug can't forge a phantom
// value.

TEST_F(RuntimeTest, IntFromValueRoundTrip) {
  EXPECT_EQ(cel_int_from_value(cel_make_int(0)), 0);
  EXPECT_EQ(cel_int_from_value(cel_make_int(42)), 42);
  EXPECT_EQ(cel_int_from_value(cel_make_int(-7)), -7);
  EXPECT_EQ(cel_int_from_value(cel_make_int(INT64_MIN)), INT64_MIN);
  EXPECT_EQ(cel_int_from_value(cel_make_int(INT64_MAX)), INT64_MAX);
}

TEST_F(RuntimeTest, IntFromValueRejectsNonInt) {
  EXPECT_EQ(cel_int_from_value(0), 0);
  EXPECT_EQ(cel_int_from_value(cel_make_bool(1)), 0);
  EXPECT_EQ(cel_int_from_value(cel_make_uint(7)), 0);
  EXPECT_EQ(cel_int_from_value(cel_make_double(1.5)), 0);
}

TEST_F(RuntimeTest, UintFromValueRoundTrip) {
  EXPECT_EQ(cel_uint_from_value(cel_make_uint(0u)), 0u);
  EXPECT_EQ(cel_uint_from_value(cel_make_uint(42u)), 42u);
  EXPECT_EQ(cel_uint_from_value(cel_make_uint(UINT64_MAX)), UINT64_MAX);
}

TEST_F(RuntimeTest, UintFromValueRejectsNonUint) {
  EXPECT_EQ(cel_uint_from_value(0), 0u);
  EXPECT_EQ(cel_uint_from_value(cel_make_int(7)), 0u);
  EXPECT_EQ(cel_uint_from_value(cel_make_double(1.5)), 0u);
}

TEST_F(RuntimeTest, DoubleFromValueRoundTrip) {
  EXPECT_EQ(cel_double_from_value(cel_make_double(0.0)), 0.0);
  EXPECT_EQ(cel_double_from_value(cel_make_double(1.5)), 1.5);
  EXPECT_EQ(cel_double_from_value(cel_make_double(-3.25)), -3.25);
}

TEST_F(RuntimeTest, DoubleFromValueRejectsNonDouble) {
  EXPECT_EQ(cel_double_from_value(0), 0.0);
  EXPECT_EQ(cel_double_from_value(cel_make_int(7)), 0.0);
  EXPECT_EQ(cel_double_from_value(cel_make_uint(7)), 0.0);
}

// ---- Three-valued logic helpers -------------------------------------------

// Helpers that build one of the five 3VL operand classes.  Parametric
// truth-table tests below drive all 25 pairings through `cel_and` /
// `cel_or` and all 5 inputs through `cel_not`.
enum class Cls : std::uint8_t {
  kTrue,
  kFalse,
  kError,
  kUnkA,  // UnknownSet{42}
  kUnkB,  // UnknownSet{100}
};

uint32_t Build(Cls c) {
  switch (c) {
    case Cls::kTrue:
      return cel_make_bool(1);
    case Cls::kFalse:
      return cel_make_bool(0);
    case Cls::kError:
      return cel_make_error(/*code=*/1, /*msg_ptr=*/0, /*msg_len=*/0);
    case Cls::kUnkA:
      return cel_make_unknown(42);
    case Cls::kUnkB:
      return cel_make_unknown(100);
  }
  return 0;
}

// The attribute ids contained in a CEL_UNKNOWN, as a vector we can
// compare against literals — tests use this to assert merge determinism.
std::vector<uint32_t> UnknownIds(uint32_t cv_off) {
  const CelValue* v = cel_value_at(cv_off);
  if (v == nullptr || v->kind != static_cast<uint32_t>(CEL_UNKNOWN)) return {};
  const auto* desc =
      reinterpret_cast<const uint32_t*>(cel_mem_base() + v->payload.unk);
  uint32_t ids_off = desc[0];
  uint32_t len = desc[1];
  const auto* ids = reinterpret_cast<const uint32_t*>(cel_mem_base() + ids_off);
  return {ids, ids + len};
}

CelKind KindOf(uint32_t cv_off) {
  const CelValue* v = cel_value_at(cv_off);
  return v == nullptr ? CEL_NULL : static_cast<CelKind>(v->kind);
}

// ---- cel_unknown_merge ----------------------------------------------------

TEST_F(RuntimeTest, UnknownMergeTwoSingletons) {
  uint32_t a = cel_make_unknown(42);
  uint32_t b = cel_make_unknown(100);
  uint32_t r = cel_unknown_merge(a, b);
  ASSERT_NE(r, 0u);
  EXPECT_EQ(KindOf(r), CEL_UNKNOWN);
  EXPECT_EQ(UnknownIds(r), (std::vector<uint32_t>{42u, 100u}));
}

TEST_F(RuntimeTest, UnknownMergeIsDeterministic) {
  // Order of arguments must not affect the result's id order — the spec
  // calls for a canonical (sorted-dedup'd) set so a host diffing two
  // unknowns produced from the same attributes sees the same bytes.
  uint32_t a = cel_make_unknown(100);
  uint32_t b = cel_make_unknown(42);
  uint32_t r1 = cel_unknown_merge(a, b);
  uint32_t r2 = cel_unknown_merge(b, a);
  EXPECT_EQ(UnknownIds(r1), UnknownIds(r2));
  EXPECT_EQ(UnknownIds(r1), (std::vector<uint32_t>{42u, 100u}));
}

TEST_F(RuntimeTest, UnknownMergeDedupsOverlap) {
  // Merging {42} with itself must produce {42}, not {42, 42}.
  uint32_t a = cel_make_unknown(42);
  uint32_t b = cel_make_unknown(42);
  uint32_t r = cel_unknown_merge(a, b);
  EXPECT_EQ(UnknownIds(r), (std::vector<uint32_t>{42u}));
}

TEST_F(RuntimeTest, UnknownMergeRejectsNonUnknown) {
  uint32_t unk = cel_make_unknown(1);
  uint32_t err = cel_make_error(0, 0, 0);
  uint32_t boolv = cel_make_bool(1);
  EXPECT_EQ(cel_unknown_merge(unk, err), 0u);
  EXPECT_EQ(cel_unknown_merge(err, unk), 0u);
  EXPECT_EQ(cel_unknown_merge(unk, boolv), 0u);
}

TEST_F(RuntimeTest, UnknownMergeRejectsZeroOffset) {
  uint32_t unk = cel_make_unknown(1);
  EXPECT_EQ(cel_unknown_merge(0, unk), 0u);
  EXPECT_EQ(cel_unknown_merge(unk, 0), 0u);
}

TEST_F(RuntimeTest, UnknownMergeHandlesEmptySet) {
  // The host `get_field` trampoline mints UNKNOWNs with payload.unk == 0
  // on FULL attribute-pattern matches (provenance not surfaced yet).
  // Both-empty must not collapse to 0; it must return a usable UNKNOWN
  // so wrapping 3VL absorbers see the kind.
  uint32_t empty_unk = cel_alloc(sizeof(CelValue));
  CelValue* v = cel_value_at(empty_unk);
  v->kind = CEL_UNKNOWN;
  v->payload.unk = 0;
  uint32_t empty_unk2 = cel_alloc(sizeof(CelValue));
  CelValue* v2 = cel_value_at(empty_unk2);
  v2->kind = CEL_UNKNOWN;
  v2->payload.unk = 0;
  uint32_t real_unk = cel_make_unknown(42);

  // both empty → left-biased return
  EXPECT_EQ(cel_unknown_merge(empty_unk, empty_unk2), empty_unk);
  // empty + real → real side wins (no ids lost)
  EXPECT_EQ(cel_unknown_merge(empty_unk, real_unk), real_unk);
  EXPECT_EQ(cel_unknown_merge(real_unk, empty_unk), real_unk);
}

// ---- cel_not --------------------------------------------------------------

TEST_F(RuntimeTest, NotBoolFlips) {
  EXPECT_EQ(cel_value_at(cel_not(cel_make_bool(1)))->payload.b, 0);
  EXPECT_EQ(cel_value_at(cel_not(cel_make_bool(0)))->payload.b, 1);
}

TEST_F(RuntimeTest, NotErrorPassesThrough) {
  uint32_t e = cel_make_error(5, 0, 0);
  uint32_t r = cel_not(e);
  EXPECT_EQ(r, e);
  EXPECT_EQ(KindOf(r), CEL_ERROR);
}

TEST_F(RuntimeTest, NotUnknownPassesThrough) {
  uint32_t u = cel_make_unknown(7);
  uint32_t r = cel_not(u);
  EXPECT_EQ(r, u);
  EXPECT_EQ(KindOf(r), CEL_UNKNOWN);
}

TEST_F(RuntimeTest, NotRejectsNonBoolean) {
  EXPECT_EQ(cel_not(cel_make_int(1)), 0u);
  EXPECT_EQ(cel_not(cel_make_string("true", 4)), 0u);
  EXPECT_EQ(cel_not(0), 0u);
}

// ---- cel_and / cel_or truth tables ---------------------------------------

// Name the expected output for a given (a, b) cell of the 5x5 table.
// Two Unknown classes (UnkA=42, UnkB=100) are needed so the merge case
// exercises a real sorted union and not just identity.
enum class Want : std::uint8_t {
  kTrue,
  kFalse,
  kError,
  kUnkA,      // UnknownSet{42}
  kUnkB,      // UnknownSet{100}
  kUnkMerge,  // UnknownSet{42, 100}
};

void ExpectBool(uint32_t got, int32_t b) {
  EXPECT_EQ(KindOf(got), CEL_BOOL);
  EXPECT_EQ(cel_value_at(got)->payload.b, b);
}

void ExpectUnknownWith(uint32_t got, const std::vector<uint32_t>& ids) {
  EXPECT_EQ(KindOf(got), CEL_UNKNOWN);
  EXPECT_EQ(UnknownIds(got), ids);
}

void ExpectWant(uint32_t got, Want w) {
  ASSERT_NE(got, 0u);
  switch (w) {
    case Want::kTrue:
      ExpectBool(got, 1);
      return;
    case Want::kFalse:
      ExpectBool(got, 0);
      return;
    case Want::kError:
      EXPECT_EQ(KindOf(got), CEL_ERROR);
      return;
    case Want::kUnkA:
      ExpectUnknownWith(got, {42u});
      return;
    case Want::kUnkB:
      ExpectUnknownWith(got, {100u});
      return;
    case Want::kUnkMerge:
      ExpectUnknownWith(got, {42u, 100u});
      return;
  }
}

struct AndOrCase {
  Cls a;
  Cls b;
  Want want;
};

// CEL `&&` truth table.  OK(false) short-circuits past everything; OK(true)
// passes the other operand through; ERROR dominates UNKNOWN when neither
// short-circuits.  UnkA/UnkB are distinguishable so the merge column is a
// real test, not a tautology.
class AndTruthTable : public RuntimeTest,
                      public ::testing::WithParamInterface<AndOrCase> {};

TEST_P(AndTruthTable, Matches) {
  AndOrCase c = GetParam();
  uint32_t a = Build(c.a);
  uint32_t b = Build(c.b);
  uint32_t r = cel_and(a, b);
  ExpectWant(r, c.want);
}

INSTANTIATE_TEST_SUITE_P(
    AllCells, AndTruthTable,
    ::testing::Values(
        // a = true
        AndOrCase{Cls::kTrue, Cls::kTrue, Want::kTrue},
        AndOrCase{Cls::kTrue, Cls::kFalse, Want::kFalse},
        AndOrCase{Cls::kTrue, Cls::kError, Want::kError},
        AndOrCase{Cls::kTrue, Cls::kUnkA, Want::kUnkA},
        AndOrCase{Cls::kTrue, Cls::kUnkB, Want::kUnkB},
        // a = false  (short-circuit — every cell is kFalse)
        AndOrCase{Cls::kFalse, Cls::kTrue, Want::kFalse},
        AndOrCase{Cls::kFalse, Cls::kFalse, Want::kFalse},
        AndOrCase{Cls::kFalse, Cls::kError, Want::kFalse},
        AndOrCase{Cls::kFalse, Cls::kUnkA, Want::kFalse},
        AndOrCase{Cls::kFalse, Cls::kUnkB, Want::kFalse},
        // a = error
        AndOrCase{Cls::kError, Cls::kTrue, Want::kError},
        AndOrCase{Cls::kError, Cls::kFalse, Want::kFalse},
        AndOrCase{Cls::kError, Cls::kError, Want::kError},
        AndOrCase{Cls::kError, Cls::kUnkA, Want::kError},
        AndOrCase{Cls::kError, Cls::kUnkB, Want::kError},
        // a = unknown(A)
        AndOrCase{Cls::kUnkA, Cls::kTrue, Want::kUnkA},
        AndOrCase{Cls::kUnkA, Cls::kFalse, Want::kFalse},
        AndOrCase{Cls::kUnkA, Cls::kError, Want::kError},
        AndOrCase{Cls::kUnkA, Cls::kUnkA, Want::kUnkA},
        AndOrCase{Cls::kUnkA, Cls::kUnkB, Want::kUnkMerge},
        // a = unknown(B)
        AndOrCase{Cls::kUnkB, Cls::kTrue, Want::kUnkB},
        AndOrCase{Cls::kUnkB, Cls::kFalse, Want::kFalse},
        AndOrCase{Cls::kUnkB, Cls::kError, Want::kError},
        AndOrCase{Cls::kUnkB, Cls::kUnkA, Want::kUnkMerge},
        AndOrCase{Cls::kUnkB, Cls::kUnkB, Want::kUnkB}));

// CEL `||` truth table.  Mirror of `&&`: OK(true) short-circuits, OK(false)
// passes through, ERROR dominates UNKNOWN in the non-short-circuit
// quadrant.
class OrTruthTable : public RuntimeTest,
                     public ::testing::WithParamInterface<AndOrCase> {};

TEST_P(OrTruthTable, Matches) {
  AndOrCase c = GetParam();
  uint32_t a = Build(c.a);
  uint32_t b = Build(c.b);
  uint32_t r = cel_or(a, b);
  ExpectWant(r, c.want);
}

INSTANTIATE_TEST_SUITE_P(AllCells, OrTruthTable,
                         ::testing::Values(
                             // a = true  (short-circuit — every cell is kTrue)
                             AndOrCase{Cls::kTrue, Cls::kTrue, Want::kTrue},
                             AndOrCase{Cls::kTrue, Cls::kFalse, Want::kTrue},
                             AndOrCase{Cls::kTrue, Cls::kError, Want::kTrue},
                             AndOrCase{Cls::kTrue, Cls::kUnkA, Want::kTrue},
                             AndOrCase{Cls::kTrue, Cls::kUnkB, Want::kTrue},
                             // a = false
                             AndOrCase{Cls::kFalse, Cls::kTrue, Want::kTrue},
                             AndOrCase{Cls::kFalse, Cls::kFalse, Want::kFalse},
                             AndOrCase{Cls::kFalse, Cls::kError, Want::kError},
                             AndOrCase{Cls::kFalse, Cls::kUnkA, Want::kUnkA},
                             AndOrCase{Cls::kFalse, Cls::kUnkB, Want::kUnkB},
                             // a = error
                             AndOrCase{Cls::kError, Cls::kTrue, Want::kTrue},
                             AndOrCase{Cls::kError, Cls::kFalse, Want::kError},
                             AndOrCase{Cls::kError, Cls::kError, Want::kError},
                             AndOrCase{Cls::kError, Cls::kUnkA, Want::kError},
                             AndOrCase{Cls::kError, Cls::kUnkB, Want::kError},
                             // a = unknown(A)
                             AndOrCase{Cls::kUnkA, Cls::kTrue, Want::kTrue},
                             AndOrCase{Cls::kUnkA, Cls::kFalse, Want::kUnkA},
                             AndOrCase{Cls::kUnkA, Cls::kError, Want::kError},
                             AndOrCase{Cls::kUnkA, Cls::kUnkA, Want::kUnkA},
                             AndOrCase{Cls::kUnkA, Cls::kUnkB, Want::kUnkMerge},
                             // a = unknown(B)
                             AndOrCase{Cls::kUnkB, Cls::kTrue, Want::kTrue},
                             AndOrCase{Cls::kUnkB, Cls::kFalse, Want::kUnkB},
                             AndOrCase{Cls::kUnkB, Cls::kError, Want::kError},
                             AndOrCase{Cls::kUnkB, Cls::kUnkA, Want::kUnkMerge},
                             AndOrCase{Cls::kUnkB, Cls::kUnkB, Want::kUnkB}));

TEST_F(RuntimeTest, AndRejectsNonBooleanOperand) {
  uint32_t i = cel_make_int(1);
  uint32_t t = cel_make_bool(1);
  EXPECT_EQ(cel_and(i, t), 0u);
  EXPECT_EQ(cel_and(t, i), 0u);
  EXPECT_EQ(cel_and(0, t), 0u);
  EXPECT_EQ(cel_and(t, 0), 0u);
}

TEST_F(RuntimeTest, OrRejectsNonBooleanOperand) {
  uint32_t i = cel_make_int(1);
  uint32_t t = cel_make_bool(1);
  EXPECT_EQ(cel_or(i, t), 0u);
  EXPECT_EQ(cel_or(t, i), 0u);
  EXPECT_EQ(cel_or(0, t), 0u);
  EXPECT_EQ(cel_or(t, 0), 0u);
}

// ---- cel_status_either ----------------------------------------------------

// `cel_status_either` is how arithmetic ops decide whether to do the op,
// propagate an error, or fold into an unknown.  Distinct from cel_and /
// cel_or because it returns 0 when both operands are OK (meaning "proceed
// with the arithmetic") rather than an OK bool.
TEST_F(RuntimeTest, StatusEitherBothOkReturnsZero) {
  EXPECT_EQ(cel_status_either(cel_make_int(1), cel_make_int(2)), 0u);
  EXPECT_EQ(cel_status_either(cel_make_double(1.0), cel_make_int(2)), 0u);
}

TEST_F(RuntimeTest, StatusEitherErrorDominates) {
  uint32_t e = cel_make_error(5, 0, 0);
  uint32_t u = cel_make_unknown(1);
  uint32_t i = cel_make_int(7);
  EXPECT_EQ(cel_status_either(e, i), e);
  EXPECT_EQ(cel_status_either(i, e), e);
  EXPECT_EQ(cel_status_either(e, u), e);
  EXPECT_EQ(cel_status_either(u, e), e);
}

TEST_F(RuntimeTest, StatusEitherUnknownPassesThrough) {
  uint32_t u = cel_make_unknown(42);
  uint32_t i = cel_make_int(7);
  EXPECT_EQ(cel_status_either(u, i), u);
  EXPECT_EQ(cel_status_either(i, u), u);
}

TEST_F(RuntimeTest, StatusEitherTwoUnknownsMerge) {
  uint32_t a = cel_make_unknown(42);
  uint32_t b = cel_make_unknown(100);
  uint32_t r = cel_status_either(a, b);
  ASSERT_NE(r, 0u);
  EXPECT_EQ(KindOf(r), CEL_UNKNOWN);
  EXPECT_EQ(UnknownIds(r), (std::vector<uint32_t>{42u, 100u}));
}

TEST_F(RuntimeTest, StatusEitherPrefersLeftErrorWhenBothError) {
  uint32_t a = cel_make_error(1, 0, 0);
  uint32_t b = cel_make_error(2, 0, 0);
  // Deterministic: left wins.  Tests rely on this when probing "which
  // error did we keep" after a multi-operand expression.
  EXPECT_EQ(cel_status_either(a, b), a);
}

TEST_F(RuntimeTest, StatusEitherRejectsZero) {
  uint32_t i = cel_make_int(1);
  EXPECT_EQ(cel_status_either(0, i), 0u);
  EXPECT_EQ(cel_status_either(i, 0), 0u);
}

// ---- Checked arithmetic (M4 Slice B) -------------------------------------

void ExpectInt(uint32_t got, int64_t want) {
  ASSERT_NE(got, 0u);
  EXPECT_EQ(KindOf(got), CEL_INT);
  EXPECT_EQ(cel_value_at(got)->payload.i, want);
}

void ExpectUint(uint32_t got, uint64_t want) {
  ASSERT_NE(got, 0u);
  EXPECT_EQ(KindOf(got), CEL_UINT);
  EXPECT_EQ(cel_value_at(got)->payload.u, want);
}

void ExpectErrorWithCode(uint32_t got, uint32_t code) {
  ASSERT_NE(got, 0u);
  ASSERT_EQ(KindOf(got), CEL_ERROR);
  const auto* err = reinterpret_cast<const uint32_t*>(
      cel_mem_base() + cel_value_at(got)->payload.err);
  EXPECT_EQ(err[0], code);
}

// ---- Scratch-slot (sret) ABI (M4 Slice C) --------------------------------
//
// The runtime is agnostic to stack vs arena — any aligned 24-byte slot
// works for the sret helpers — so these tests reuse `cel_alloc` as a
// stand-in for the software-stack frame slots codegen will emit later.

uint32_t AllocSlot() {
  return cel_alloc(static_cast<uint32_t>(sizeof(CelValue)));
}

TEST_F(RuntimeTest, BoxIntWritesSlot) {
  uint32_t out = AllocSlot();
  cel_box_int(out, -42);
  ExpectInt(out, -42);
}

TEST_F(RuntimeTest, BoxUintWritesSlot) {
  uint32_t out = AllocSlot();
  cel_box_uint(out, UINT64_MAX);
  ExpectUint(out, UINT64_MAX);
}

TEST_F(RuntimeTest, BoxDoubleWritesSlot) {
  uint32_t out = AllocSlot();
  cel_box_double(out, 3.14);
  EXPECT_EQ(KindOf(out), CEL_DOUBLE);
  EXPECT_DOUBLE_EQ(cel_value_at(out)->payload.d, 3.14);
}

TEST_F(RuntimeTest, BoxOutZeroIsNoOp) {
  // out == 0 targets the null sentinel; helpers must not corrupt it.
  uint32_t null_off = cel_make_null();
  CelKind prev_kind = KindOf(null_off);
  cel_box_int(0, 42);
  cel_box_double(0, 3.14);
  EXPECT_EQ(KindOf(null_off), prev_kind);
}

TEST_F(RuntimeTest, CopyCelvalueAtCopiesAllBytes) {
  // Round-trip a string CelValue through the sret-copy helper.  Use
  // cel_make_string which builds a CelValue{kind=STRING, ptr, len} in
  // the arena; the copy helper should reproduce it byte-for-byte at
  // the destination slot.
  const char kStr[] = "hello";
  uint32_t src = cel_make_string(kStr, sizeof(kStr) - 1);
  uint32_t out = AllocSlot();
  cel_copy_celvalue_at(out, src);
  EXPECT_EQ(KindOf(out), CEL_STRING);
  EXPECT_EQ(cel_value_at(out)->payload.s.len, cel_value_at(src)->payload.s.len);
  EXPECT_EQ(cel_value_at(out)->payload.s.ptr, cel_value_at(src)->payload.s.ptr);
}

TEST_F(RuntimeTest, CopyCelvalueAtOutZeroIsNoOp) {
  uint32_t src = cel_make_int(42);
  uint32_t null_off = cel_make_null();
  CelKind prev = KindOf(null_off);
  cel_copy_celvalue_at(0, src);
  EXPECT_EQ(KindOf(null_off), prev);
}

TEST_F(RuntimeTest, CopyCelvalueAtSrcZeroWritesTypeMismatch) {
  // A zero src means "no value was produced" — translate to a
  // proper CEL error so forgetting to allocate a source surfaces as
  // TYPE_MISMATCH rather than a phantom OK read from offset zero.
  uint32_t out = AllocSlot();
  cel_copy_celvalue_at(out, 0);
  ExpectErrorWithCode(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(RuntimeTest, SetErrorAtWritesCodedError) {
  // cel_set_error_at is the codegen escape hatch for NaN-in-ordered-
  // compare (and future non-slot ERROR sources).  Every CEL_ERR_*
  // code round-trips unchanged.
  uint32_t out = AllocSlot();
  cel_set_error_at(out, CEL_ERR_TYPE_MISMATCH);
  ExpectErrorWithCode(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(RuntimeTest, SetErrorAtOutZeroIsNoOp) {
  uint32_t null_off = cel_make_null();
  CelKind prev = KindOf(null_off);
  cel_set_error_at(0, CEL_ERR_OVERFLOW);
  EXPECT_EQ(KindOf(null_off), prev);
}

// ---- Scalar-arg sret helpers (M4 Slice C) --------------------------------
//
// These take raw i64 / u64 operands instead of boxed offsets so there
// is no operand-status plumbing — just overflow / div0 / mod0 checks.
// Codegen uses these as the replacement for the Slice B `_ii` / `_uu`
// variants; happy path + each arithmetic error path must land in the
// slot as a well-formed CelValue.

TEST_F(RuntimeTest, IntAddAtIiHappyPath) {
  uint32_t out = AllocSlot();
  cel_int_add_at_ii(out, 3, 4);
  ExpectInt(out, 7);
}

TEST_F(RuntimeTest, IntAddAtIiOverflow) {
  uint32_t out = AllocSlot();
  cel_int_add_at_ii(out, INT64_MAX, 1);
  ExpectErrorWithCode(out, CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, IntSubAtIiOverflow) {
  uint32_t out = AllocSlot();
  cel_int_sub_at_ii(out, INT64_MIN, 1);
  ExpectErrorWithCode(out, CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, IntMulAtIiOverflow) {
  uint32_t out = AllocSlot();
  cel_int_mul_at_ii(out, INT64_MAX, 2);
  ExpectErrorWithCode(out, CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, IntDivAtIiHappyPath) {
  uint32_t out = AllocSlot();
  cel_int_div_at_ii(out, 10, 3);
  ExpectInt(out, 3);
}

TEST_F(RuntimeTest, IntDivAtIiByZero) {
  uint32_t out = AllocSlot();
  cel_int_div_at_ii(out, 5, 0);
  ExpectErrorWithCode(out, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(RuntimeTest, IntDivAtIiMinByNegOneOverflows) {
  uint32_t out = AllocSlot();
  cel_int_div_at_ii(out, INT64_MIN, -1);
  ExpectErrorWithCode(out, CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, IntModAtIiByZero) {
  uint32_t out = AllocSlot();
  cel_int_mod_at_ii(out, 5, 0);
  ExpectErrorWithCode(out, CEL_ERR_MODULUS_BY_ZERO);
}

TEST_F(RuntimeTest, IntModAtIiMinByNegOneIsZero) {
  uint32_t out = AllocSlot();
  cel_int_mod_at_ii(out, INT64_MIN, -1);
  ExpectInt(out, 0);
}

TEST_F(RuntimeTest, UintAddAtUuHappyPath) {
  uint32_t out = AllocSlot();
  cel_uint_add_at_uu(out, 3u, 4u);
  ExpectUint(out, 7u);
}

TEST_F(RuntimeTest, UintAddAtUuWrapIsOverflow) {
  uint32_t out = AllocSlot();
  cel_uint_add_at_uu(out, UINT64_MAX, 1u);
  ExpectErrorWithCode(out, CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, UintSubAtUuUnderflow) {
  uint32_t out = AllocSlot();
  cel_uint_sub_at_uu(out, 0u, 1u);
  ExpectErrorWithCode(out, CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, UintMulAtUuOverflow) {
  uint32_t out = AllocSlot();
  cel_uint_mul_at_uu(out, UINT64_MAX, 2u);
  ExpectErrorWithCode(out, CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, UintDivAtUuByZero) {
  uint32_t out = AllocSlot();
  cel_uint_div_at_uu(out, 7u, 0u);
  ExpectErrorWithCode(out, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(RuntimeTest, UintModAtUuByZero) {
  uint32_t out = AllocSlot();
  cel_uint_mod_at_uu(out, 7u, 0u);
  ExpectErrorWithCode(out, CEL_ERR_MODULUS_BY_ZERO);
}

TEST_F(RuntimeTest, IntNegAtIHappyPath) {
  uint32_t out = AllocSlot();
  cel_int_neg_at_i(out, 5);
  ExpectInt(out, -5);
}

TEST_F(RuntimeTest, IntNegAtIMinOverflows) {
  uint32_t out = AllocSlot();
  cel_int_neg_at_i(out, INT64_MIN);
  ExpectErrorWithCode(out, CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, ScalarAtZeroOffsetIsNoOp) {
  // All scalar-sret helpers must early-return on out == 0 without
  // trapping; mirrors the BoxOutZeroIsNoOp contract.
  uint32_t null_off = cel_make_null();
  CelKind prev_kind = KindOf(null_off);
  cel_int_add_at_ii(0, 1, 2);
  cel_uint_add_at_uu(0, 1u, 2u);
  cel_int_neg_at_i(0, 1);
  EXPECT_EQ(KindOf(null_off), prev_kind);
}

// ---- 3VL-aware comparison helpers (M4 Slice F1) --------------------------
//
// Each helper must (a) forward dominant non-OK status so absorbing
// operators upstream can see UNKNOWN / ERROR as values, (b) compute the
// normal boolean result when both operands are OK, and (c) return 0
// on kind-mismatch so codegen's zero-check surfaces a bug rather than
// a phantom OK.  The tests below exercise all three cases for int;
// narrower coverage on the uint / double / bool variants since the
// helpers share the same prologue.

TEST_F(RuntimeTest, CmpIntEqHappyPath) {
  ExpectBool(cel_cmp_int_eq(cel_make_int(7), cel_make_int(7)), 1);
  ExpectBool(cel_cmp_int_eq(cel_make_int(7), cel_make_int(8)), 0);
}

TEST_F(RuntimeTest, CmpIntOrderedHappyPath) {
  ExpectBool(cel_cmp_int_lt(cel_make_int(1), cel_make_int(2)), 1);
  ExpectBool(cel_cmp_int_le(cel_make_int(2), cel_make_int(2)), 1);
  ExpectBool(cel_cmp_int_gt(cel_make_int(3), cel_make_int(2)), 1);
  ExpectBool(cel_cmp_int_ge(cel_make_int(2), cel_make_int(3)), 0);
  ExpectBool(cel_cmp_int_ne(cel_make_int(1), cel_make_int(2)), 1);
}

TEST_F(RuntimeTest, CmpIntErrorDominates) {
  uint32_t err = cel_make_error(CEL_ERR_DIVIDE_BY_ZERO, 0, 0);
  uint32_t r = cel_cmp_int_lt(err, cel_make_int(5));
  EXPECT_EQ(KindOf(r), CEL_ERROR);
  ExpectErrorWithCode(r, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(RuntimeTest, CmpIntUnknownPassesThrough) {
  uint32_t unk = cel_make_unknown(42);
  uint32_t r = cel_cmp_int_eq(unk, cel_make_int(5));
  EXPECT_EQ(KindOf(r), CEL_UNKNOWN);
}

TEST_F(RuntimeTest, CmpIntErrorDominatesUnknown) {
  uint32_t err = cel_make_error(CEL_ERR_OVERFLOW, 0, 0);
  uint32_t unk = cel_make_unknown(3);
  uint32_t r = cel_cmp_int_gt(err, unk);
  EXPECT_EQ(KindOf(r), CEL_ERROR);
  ExpectErrorWithCode(r, CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, CmpIntTwoUnknownsMerge) {
  uint32_t r = cel_cmp_int_eq(cel_make_unknown(2), cel_make_unknown(1));
  EXPECT_EQ(KindOf(r), CEL_UNKNOWN);
}

TEST_F(RuntimeTest, CmpIntKindMismatchReturnsZero) {
  // Both operands OK but the right one isn't an int — codegen bug.
  EXPECT_EQ(cel_cmp_int_eq(cel_make_int(1), cel_make_uint(1u)), 0u);
}

TEST_F(RuntimeTest, CmpIntZeroOffsetReturnsZero) {
  EXPECT_EQ(cel_cmp_int_eq(0, cel_make_int(1)), 0u);
  EXPECT_EQ(cel_cmp_int_eq(cel_make_int(1), 0), 0u);
}

TEST_F(RuntimeTest, CmpUintHappyPath) {
  ExpectBool(cel_cmp_uint_eq(cel_make_uint(7u), cel_make_uint(7u)), 1);
  ExpectBool(cel_cmp_uint_lt(cel_make_uint(1u), cel_make_uint(2u)), 1);
  ExpectBool(cel_cmp_uint_ge(cel_make_uint(2u), cel_make_uint(3u)), 0);
}

TEST_F(RuntimeTest, CmpUintUnknownPassesThrough) {
  uint32_t unk = cel_make_unknown(1);
  uint32_t r = cel_cmp_uint_lt(unk, cel_make_uint(5u));
  EXPECT_EQ(KindOf(r), CEL_UNKNOWN);
}

TEST_F(RuntimeTest, CmpDoubleHappyPath) {
  ExpectBool(cel_cmp_double_eq(cel_make_double(1.5), cel_make_double(1.5)), 1);
  ExpectBool(cel_cmp_double_lt(cel_make_double(1.0), cel_make_double(2.0)), 1);
  ExpectBool(cel_cmp_double_ne(cel_make_double(1.0), cel_make_double(2.0)), 1);
}

TEST_F(RuntimeTest, CmpDoubleEqWithNaNIsFalse) {
  // IEEE 754: NaN equality is well-defined (false for ==, true for !=).
  double nan_v = std::nan("");
  ExpectBool(cel_cmp_double_eq(cel_make_double(nan_v), cel_make_double(1.0)),
             0);
  ExpectBool(cel_cmp_double_ne(cel_make_double(nan_v), cel_make_double(1.0)),
             1);
}

TEST_F(RuntimeTest, CmpDoubleOrderedNaNReturnsError) {
  double nan_v = std::nan("");
  uint32_t r = cel_cmp_double_lt(cel_make_double(nan_v), cel_make_double(1.0));
  ExpectErrorWithCode(r, CEL_ERR_TYPE_MISMATCH);
  r = cel_cmp_double_gt(cel_make_double(1.0), cel_make_double(nan_v));
  ExpectErrorWithCode(r, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(RuntimeTest, CmpDoubleUnknownBeatsNaN) {
  // cel_status_either runs first; NaN-check only on the OK path.
  double nan_v = std::nan("");
  uint32_t r = cel_cmp_double_lt(cel_make_unknown(2), cel_make_double(nan_v));
  EXPECT_EQ(KindOf(r), CEL_UNKNOWN);
}

TEST_F(RuntimeTest, CmpBoolEqHappyPath) {
  ExpectBool(cel_cmp_bool_eq(cel_make_bool(1), cel_make_bool(1)), 1);
  ExpectBool(cel_cmp_bool_eq(cel_make_bool(1), cel_make_bool(0)), 0);
  ExpectBool(cel_cmp_bool_ne(cel_make_bool(1), cel_make_bool(0)), 1);
}

TEST_F(RuntimeTest, CmpBoolUnknownPassesThrough) {
  uint32_t r = cel_cmp_bool_eq(cel_make_unknown(7), cel_make_bool(1));
  EXPECT_EQ(KindOf(r), CEL_UNKNOWN);
}

}  // namespace
}  // namespace celwasm
