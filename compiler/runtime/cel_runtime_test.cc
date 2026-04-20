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

// ---- Scalar int helpers ---------------------------------------------------

TEST_F(RuntimeTest, IntAddScalarHappyPath) {
  ExpectInt(cel_int_add_ii(2, 3), 5);
  ExpectInt(cel_int_add_ii(-7, 2), -5);
  ExpectInt(cel_int_add_ii(0, 0), 0);
}

TEST_F(RuntimeTest, IntAddScalarOverflowsAtMax) {
  ExpectErrorWithCode(cel_int_add_ii(INT64_MAX, 1), CEL_ERR_OVERFLOW);
  ExpectErrorWithCode(cel_int_add_ii(1, INT64_MAX), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, IntAddScalarOverflowsAtMin) {
  ExpectErrorWithCode(cel_int_add_ii(INT64_MIN, -1), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, IntSubScalarHappyPath) {
  ExpectInt(cel_int_sub_ii(5, 3), 2);
  ExpectInt(cel_int_sub_ii(-1, -1), 0);
}

TEST_F(RuntimeTest, IntSubScalarOverflowsAtMin) {
  ExpectErrorWithCode(cel_int_sub_ii(INT64_MIN, 1), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, IntMulScalarHappyPath) {
  ExpectInt(cel_int_mul_ii(6, 7), 42);
  ExpectInt(cel_int_mul_ii(-3, 4), -12);
  ExpectInt(cel_int_mul_ii(0, INT64_MAX), 0);
}

TEST_F(RuntimeTest, IntMulScalarOverflows) {
  ExpectErrorWithCode(cel_int_mul_ii(INT64_MAX, 2), CEL_ERR_OVERFLOW);
  ExpectErrorWithCode(cel_int_mul_ii(INT64_MIN, -1), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, IntDivScalarHappyPath) {
  ExpectInt(cel_int_div_ii(20, 4), 5);
  ExpectInt(cel_int_div_ii(-9, 2), -4);  // C rounds toward zero
}

TEST_F(RuntimeTest, IntDivScalarByZero) {
  ExpectErrorWithCode(cel_int_div_ii(7, 0), CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(RuntimeTest, IntDivScalarMinByNegOneOverflows) {
  // |INT64_MIN| > INT64_MAX — representing -INT64_MIN is impossible.
  ExpectErrorWithCode(cel_int_div_ii(INT64_MIN, -1), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, IntModScalarHappyPath) {
  ExpectInt(cel_int_mod_ii(10, 3), 1);
  ExpectInt(cel_int_mod_ii(-10, 3), -1);  // C: result takes sign of dividend
}

TEST_F(RuntimeTest, IntModScalarByZero) {
  ExpectErrorWithCode(cel_int_mod_ii(7, 0), CEL_ERR_MODULUS_BY_ZERO);
}

TEST_F(RuntimeTest, IntModScalarMinByNegOneIsZero) {
  // Mathematically INT64_MIN % -1 = 0; C reserves UB so we special-case.
  ExpectInt(cel_int_mod_ii(INT64_MIN, -1), 0);
}

TEST_F(RuntimeTest, IntNegScalarHappyPath) {
  ExpectInt(cel_int_neg_i(5), -5);
  ExpectInt(cel_int_neg_i(-5), 5);
  ExpectInt(cel_int_neg_i(0), 0);
}

TEST_F(RuntimeTest, IntNegScalarMinOverflows) {
  ExpectErrorWithCode(cel_int_neg_i(INT64_MIN), CEL_ERR_OVERFLOW);
}

// ---- Scalar uint helpers --------------------------------------------------

TEST_F(RuntimeTest, UintAddScalarHappyPath) {
  ExpectUint(cel_uint_add_uu(2, 3), 5);
  ExpectUint(cel_uint_add_uu(0, UINT64_MAX), UINT64_MAX);
}

TEST_F(RuntimeTest, UintAddScalarWrapIsOverflow) {
  // CEL semantics: unsigned addition that wraps is ERROR, not silent
  // wrap.  `a + b < a` is the standard detection.
  ExpectErrorWithCode(cel_uint_add_uu(UINT64_MAX, 1), CEL_ERR_OVERFLOW);
  ExpectErrorWithCode(cel_uint_add_uu(1, UINT64_MAX), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, UintSubScalarHappyPath) {
  ExpectUint(cel_uint_sub_uu(5, 3), 2);
  ExpectUint(cel_uint_sub_uu(0, 0), 0);
}

TEST_F(RuntimeTest, UintSubScalarUnderflowsIsOverflow) {
  ExpectErrorWithCode(cel_uint_sub_uu(0, 1), CEL_ERR_OVERFLOW);
  ExpectErrorWithCode(cel_uint_sub_uu(3, 4), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, UintMulScalarHappyPath) {
  ExpectUint(cel_uint_mul_uu(6, 7), 42u);
  ExpectUint(cel_uint_mul_uu(0, UINT64_MAX), 0u);
}

TEST_F(RuntimeTest, UintMulScalarOverflows) {
  ExpectErrorWithCode(cel_uint_mul_uu(UINT64_MAX, 2), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, UintDivScalarHappyPath) {
  ExpectUint(cel_uint_div_uu(20, 4), 5u);
}

TEST_F(RuntimeTest, UintDivScalarByZero) {
  ExpectErrorWithCode(cel_uint_div_uu(7, 0), CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(RuntimeTest, UintModScalarHappyPath) {
  ExpectUint(cel_uint_mod_uu(10, 3), 1u);
}

TEST_F(RuntimeTest, UintModScalarByZero) {
  ExpectErrorWithCode(cel_uint_mod_uu(7, 0), CEL_ERR_MODULUS_BY_ZERO);
}

// ---- Boxed int helpers ----------------------------------------------------

TEST_F(RuntimeTest, BoxedIntAddHappyPath) {
  uint32_t a = cel_make_int(3);
  uint32_t b = cel_make_int(4);
  ExpectInt(cel_int_add(a, b), 7);
}

TEST_F(RuntimeTest, BoxedIntAddOverflow) {
  uint32_t a = cel_make_int(INT64_MAX);
  uint32_t b = cel_make_int(1);
  ExpectErrorWithCode(cel_int_add(a, b), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, BoxedIntAddErrorPropagates) {
  uint32_t e = cel_make_error(99, 0, 0);
  uint32_t b = cel_make_int(2);
  // Left ERROR wins.
  EXPECT_EQ(cel_int_add(e, b), e);
  // Right ERROR wins when left is OK.
  EXPECT_EQ(cel_int_add(b, e), e);
}

TEST_F(RuntimeTest, BoxedIntAddUnknownPropagates) {
  uint32_t u = cel_make_unknown(42);
  uint32_t b = cel_make_int(2);
  EXPECT_EQ(cel_int_add(u, b), u);
  EXPECT_EQ(cel_int_add(b, u), u);
}

TEST_F(RuntimeTest, BoxedIntAddTwoUnknownsMerge) {
  uint32_t a = cel_make_unknown(42);
  uint32_t b = cel_make_unknown(100);
  uint32_t r = cel_int_add(a, b);
  ASSERT_NE(r, 0u);
  ASSERT_EQ(KindOf(r), CEL_UNKNOWN);
  EXPECT_EQ(UnknownIds(r), (std::vector<uint32_t>{42u, 100u}));
}

TEST_F(RuntimeTest, BoxedIntSubOverflow) {
  uint32_t a = cel_make_int(INT64_MIN);
  uint32_t b = cel_make_int(1);
  ExpectErrorWithCode(cel_int_sub(a, b), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, BoxedIntMulOverflow) {
  uint32_t a = cel_make_int(INT64_MAX);
  uint32_t b = cel_make_int(2);
  ExpectErrorWithCode(cel_int_mul(a, b), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, BoxedIntDivByZero) {
  uint32_t a = cel_make_int(5);
  uint32_t b = cel_make_int(0);
  ExpectErrorWithCode(cel_int_div(a, b), CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(RuntimeTest, BoxedIntDivMinByNegOne) {
  uint32_t a = cel_make_int(INT64_MIN);
  uint32_t b = cel_make_int(-1);
  ExpectErrorWithCode(cel_int_div(a, b), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, BoxedIntModByZero) {
  uint32_t a = cel_make_int(5);
  uint32_t b = cel_make_int(0);
  ExpectErrorWithCode(cel_int_mod(a, b), CEL_ERR_MODULUS_BY_ZERO);
}

TEST_F(RuntimeTest, BoxedIntAddRejectsNonInt) {
  uint32_t i = cel_make_int(1);
  uint32_t d = cel_make_double(1.0);
  uint32_t s = cel_make_string("1", 1);
  // Checker is responsible for type-matching; runtime refuses to guess.
  EXPECT_EQ(cel_int_add(i, d), 0u);
  EXPECT_EQ(cel_int_add(d, i), 0u);
  EXPECT_EQ(cel_int_add(i, s), 0u);
}

TEST_F(RuntimeTest, BoxedIntAddRejectsZero) {
  uint32_t i = cel_make_int(1);
  EXPECT_EQ(cel_int_add(0, i), 0u);
  EXPECT_EQ(cel_int_add(i, 0), 0u);
}

TEST_F(RuntimeTest, BoxedIntNegHappyPath) {
  uint32_t a = cel_make_int(5);
  ExpectInt(cel_int_neg(a), -5);
}

TEST_F(RuntimeTest, BoxedIntNegOverflow) {
  uint32_t a = cel_make_int(INT64_MIN);
  ExpectErrorWithCode(cel_int_neg(a), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, BoxedIntNegPropagatesStatus) {
  uint32_t e = cel_make_error(7, 0, 0);
  EXPECT_EQ(cel_int_neg(e), e);
  uint32_t u = cel_make_unknown(3);
  EXPECT_EQ(cel_int_neg(u), u);
}

TEST_F(RuntimeTest, BoxedIntNegRejectsNonInt) {
  uint32_t d = cel_make_double(1.0);
  EXPECT_EQ(cel_int_neg(d), 0u);
  EXPECT_EQ(cel_int_neg(0), 0u);
}

// ---- Boxed uint helpers ---------------------------------------------------

TEST_F(RuntimeTest, BoxedUintAddHappyPath) {
  uint32_t a = cel_make_uint(3);
  uint32_t b = cel_make_uint(4);
  ExpectUint(cel_uint_add(a, b), 7u);
}

TEST_F(RuntimeTest, BoxedUintAddWrapIsOverflow) {
  uint32_t a = cel_make_uint(UINT64_MAX);
  uint32_t b = cel_make_uint(1);
  ExpectErrorWithCode(cel_uint_add(a, b), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, BoxedUintSubUnderflow) {
  uint32_t a = cel_make_uint(0);
  uint32_t b = cel_make_uint(1);
  ExpectErrorWithCode(cel_uint_sub(a, b), CEL_ERR_OVERFLOW);
}

TEST_F(RuntimeTest, BoxedUintDivByZero) {
  uint32_t a = cel_make_uint(7);
  uint32_t b = cel_make_uint(0);
  ExpectErrorWithCode(cel_uint_div(a, b), CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(RuntimeTest, BoxedUintModByZero) {
  uint32_t a = cel_make_uint(7);
  uint32_t b = cel_make_uint(0);
  ExpectErrorWithCode(cel_uint_mod(a, b), CEL_ERR_MODULUS_BY_ZERO);
}

TEST_F(RuntimeTest, BoxedUintRejectsNonUint) {
  uint32_t i = cel_make_int(1);
  uint32_t u = cel_make_uint(1);
  // Signed int is NOT a uint.  Mixed-sign arithmetic must have been
  // rejected or coerced by the checker.
  EXPECT_EQ(cel_uint_add(i, u), 0u);
  EXPECT_EQ(cel_uint_add(u, i), 0u);
}

TEST_F(RuntimeTest, BoxedUintErrorPropagates) {
  uint32_t e = cel_make_error(99, 0, 0);
  uint32_t u = cel_make_uint(2);
  EXPECT_EQ(cel_uint_add(e, u), e);
  EXPECT_EQ(cel_uint_add(u, e), e);
}

}  // namespace
}  // namespace celwasm
