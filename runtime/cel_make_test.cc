#include "runtime/cel_make.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

class MakeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
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

// ── Arena OOM behaviour (DESIGN §5 A10) ────────────────────────────
//
// When the arena cannot satisfy an allocation, `arena_alloc` returns
// 0 (the absent sentinel).  Every `cel_make_*` constructor propagates
// that by returning 0 — never a partial / dangling slot.  These tests
// exhaust the arena and then exercise each constructor's failure
// path.

class MakeOomTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }

  // Burn through all but `keep` bytes of arena capacity so subsequent
  // allocations of size > `keep` fail.  Returns the number of bytes
  // still available.
  uint32_t DrainArenaLeaving(uint32_t keep) {
    uint32_t remaining = arena_capacity() - arena_cursor();
    EXPECT_GE(remaining, keep);
    uint32_t to_burn = remaining - keep;
    if (to_burn > 0) {
      EXPECT_NE(arena_alloc(to_burn), 0u);
    }
    return keep;
  }
};

TEST_F(MakeOomTest, MakeNullReturnsZeroWhenArenaIsFull) {
  // Fill exactly to capacity.
  DrainArenaLeaving(0);
  EXPECT_EQ(cel_make_null(), 0u);
}

TEST_F(MakeOomTest, MakeIntReturnsZeroWhenArenaIsFull) {
  DrainArenaLeaving(0);
  EXPECT_EQ(cel_make_int(42), 0u);
}

TEST_F(MakeOomTest, MakeBoolReturnsZeroWhenArenaIsFull) {
  DrainArenaLeaving(0);
  EXPECT_EQ(cel_make_bool(1), 0u);
}

TEST_F(MakeOomTest, MakeDoubleReturnsZeroWhenArenaIsFull) {
  DrainArenaLeaving(0);
  EXPECT_EQ(cel_make_double(3.14), 0u);
}

// String with len=0 still needs space for the CelValue header (24 B);
// fails if the header doesn't fit.
TEST_F(MakeOomTest, MakeEmptyStringReturnsZeroWhenNoHeaderSpace) {
  DrainArenaLeaving(0);
  EXPECT_EQ(cel_make_string(nullptr, 0), 0u);
}

// Non-empty string when the payload bytes fit but the CelValue header
// would push past capacity — make_span_copy allocates payload first,
// then header.  Drain to exactly `payload_len` bytes (after alignment)
// so payload alloc succeeds but the header alloc fails.
TEST_F(MakeOomTest, MakeStringReturnsZeroWhenHeaderDoesntFit) {
  const char kSrc[] = "abcdef";  // 6 bytes
  // 6 rounds up to 8.  Leave exactly 8 bytes → payload fits, header
  // (24 bytes) does not.
  DrainArenaLeaving(8);
  EXPECT_EQ(cel_make_string(kSrc, sizeof(kSrc) - 1), 0u);
}

// When neither payload nor header fit, make_string still returns 0.
TEST_F(MakeOomTest, MakeStringReturnsZeroWhenPayloadDoesntFit) {
  DrainArenaLeaving(0);
  EXPECT_EQ(cel_make_string("hi", 2), 0u);
}

// Bytes constructor mirrors string.
TEST_F(MakeOomTest, MakeBytesReturnsZeroWhenArenaIsFull) {
  DrainArenaLeaving(0);
  const uint8_t kSrc[] = {0xde, 0xad};
  EXPECT_EQ(cel_make_bytes(kSrc, sizeof(kSrc)), 0u);
}

// Boundary: just enough room for ONE more CelValue header (24 bytes,
// rounded to 24 since it's already 8-aligned).  Next allocation
// after that fails.
TEST_F(MakeOomTest, MakeValueChainStopsAtBoundary) {
  // Burn down to exactly 24 bytes free.
  DrainArenaLeaving(24);
  uint32_t a = cel_make_int(1);
  EXPECT_NE(a, 0u);
  // Arena is now full.
  EXPECT_EQ(cel_make_int(2), 0u);
}

// Length-1 string at the boundary: payload (1 → 8) + header (24) =
// 32 bytes.  Leave exactly 32 → succeeds.  Leave 31 → fails (header
// doesn't fit after payload align-up).
TEST_F(MakeOomTest, MakeOneByteStringSucceedsAtExactBoundary) {
  DrainArenaLeaving(32);
  EXPECT_NE(cel_make_string("x", 1), 0u);
  EXPECT_EQ(arena_cursor(), arena_capacity());
}

TEST_F(MakeOomTest, MakeOneByteStringFailsJustBelowBoundary) {
  // 31 bytes free: 8-byte payload alloc succeeds (8 ≤ 31), but the
  // 24-byte header alloc fails (24 > 23 remaining).
  DrainArenaLeaving(31);
  EXPECT_EQ(cel_make_string("x", 1), 0u);
}

}  // namespace
}  // namespace celwasm
