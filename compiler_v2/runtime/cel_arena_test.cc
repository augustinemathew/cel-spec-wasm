#include "compiler_v2/runtime/cel_arena.h"

#include <cstdint>
#include <cstring>

#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// Fresh-per-test setup.  `cel_reset(arena_base, arena_limit)` writes the
// cursor slot at bytes 8/12; every test starts from a known state.
class ArenaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Leave [0..16) reserved: 0..7 null sentinel, 8..11 bump, 12..15 limit.
    cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
  }
};

// --- CelValue layout asserts (live here since exercising the layout
// requires the arena). ----------------------------------------------------

TEST_F(ArenaTest, CelValueIs24Bytes) {
  EXPECT_EQ(sizeof(CelValue), 24u);
}

// --- cel_reset -----------------------------------------------------------

TEST_F(ArenaTest, ResetWritesCursorSlot) {
  cel_reset(/*arena_base=*/128u, /*arena_limit=*/4096u);
  uint32_t bump = 0;
  uint32_t limit = 0;
  std::memcpy(&bump, cel_mem_base() + 8, sizeof(bump));
  std::memcpy(&limit, cel_mem_base() + 12, sizeof(limit));
  EXPECT_EQ(bump, 128u);
  EXPECT_EQ(limit, 4096u);
}

TEST_F(ArenaTest, ResetRewindsCursor) {
  (void)cel_alloc(24);
  (void)cel_alloc(24);
  cel_reset(/*arena_base=*/16u, /*arena_limit=*/cel_mem_size());
  uint32_t a = cel_alloc(24);
  EXPECT_EQ(a, 16u);
}

// --- cel_alloc -----------------------------------------------------------

TEST_F(ArenaTest, AllocBumpsCursorAndReturnsPreBumpOffset) {
  uint32_t a = cel_alloc(24);
  uint32_t b = cel_alloc(24);
  EXPECT_EQ(a, 16u);  // arena_base
  EXPECT_EQ(b, 16u + 24u);
  uint32_t bump = 0;
  std::memcpy(&bump, cel_mem_base() + 8, sizeof(bump));
  EXPECT_EQ(bump, 16u + 48u);
}

TEST_F(ArenaTest, AllocAlignsToEightBytes) {
  uint32_t a = cel_alloc(1);  // rounds up to 8
  uint32_t b = cel_alloc(0);  // zero rounds up to 8
  EXPECT_EQ(a, 16u);
  EXPECT_EQ(b, 24u);
}

TEST_F(ArenaTest, AllocReturnsZeroWhenOutOfSpace) {
  cel_reset(/*arena_base=*/16u, /*arena_limit=*/24u);  // 8 usable bytes
  uint32_t a = cel_alloc(8);
  uint32_t b = cel_alloc(8);
  EXPECT_EQ(a, 16u);
  EXPECT_EQ(b, 0u);
}

// --- cel_value_at --------------------------------------------------------

TEST_F(ArenaTest, ValueAtZeroReturnsNull) {
  EXPECT_EQ(cel_value_at(0), nullptr);
}

TEST_F(ArenaTest, ValueAtNonZeroReturnsBasePlusOffset) {
  uint32_t off = cel_alloc(24);
  ASSERT_NE(off, 0u);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(cel_value_at(off)),
            cel_mem_base() + off);
}

}  // namespace
}  // namespace celwasm
