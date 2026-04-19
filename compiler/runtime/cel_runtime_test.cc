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
  const uint32_t* set =
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
  const uint32_t* err =
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

}  // namespace
}  // namespace celwasm
