// Tests for the malloc-backed bump arena.  See cel_arena.h for the
// API; doc/implementation-plan/wasi/DESIGN.md §4-§5 for the design.
//
// Native-build invariants:
//   - cel_alloc(n) returns an offset such that
//     cel_mem_base() + offset is the allocated bytes.
//   - Allocations are 8-byte aligned and zero-initialized.
//   - arena_reset rewinds the cursor; the next alloc returns the
//     same offset the first alloc did after init.

#include "compiler_v2/runtime/cel_arena.h"

#include <cstdint>
#include <cstring>

#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

class ArenaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Reset is idempotent: the compat shim auto-inits the arena on
    // first call across the whole gtest process; subsequent SetUps
    // just rewind the cursor to 0.
    cel_reset(/*ignored=*/0u, /*ignored=*/0u);
  }
};

TEST_F(ArenaTest, CelValueIs24Bytes) {
  EXPECT_EQ(sizeof(CelValue), 24u);
}

TEST_F(ArenaTest, ResetRewindsCursor) {
  uint32_t a0 = arena_alloc(24);
  (void)arena_alloc(24);
  arena_reset();
  uint32_t a1 = arena_alloc(24);
  EXPECT_EQ(a0, a1);
}

TEST_F(ArenaTest, AllocBumpsCursorMonotonically) {
  uint32_t a = arena_alloc(24);
  uint32_t b = arena_alloc(24);
  uint32_t c = arena_alloc(24);
  ASSERT_NE(a, 0u);
  EXPECT_EQ(b, a + 24u);
  EXPECT_EQ(c, b + 24u);
}

TEST_F(ArenaTest, AllocAlignsToEightBytes) {
  uint32_t a = arena_alloc(1);   // rounds up to 8
  uint32_t b = arena_alloc(0);   // zero rounds up to 8
  uint32_t c = arena_alloc(9);   // rounds up to 16
  EXPECT_EQ(b, a + 8u);
  EXPECT_EQ(c, b + 8u);
}

TEST_F(ArenaTest, AllocReturnsZeroedBytes) {
  uint32_t a = arena_alloc(24);
  ASSERT_NE(a, 0u);
  const uint8_t* p = cel_mem_base() + a;
  for (size_t i = 0; i < 24; ++i) {
    EXPECT_EQ(p[i], 0u) << "byte " << i << " not zero";
  }
}

TEST_F(ArenaTest, AllocReturnsZeroWhenOutOfSpace) {
  // Exhaust the arena.  arena_capacity() returns the configured
  // capacity; we drain it in chunks of 1024 bytes then expect 0.
  const uint32_t cap = arena_capacity();
  for (uint32_t consumed = 0; consumed < cap; consumed += 1024u) {
    EXPECT_NE(arena_alloc(1024), 0u);
  }
  EXPECT_EQ(arena_alloc(1), 0u);  // arena is full
}

TEST_F(ArenaTest, CursorReflectsAllocations) {
  EXPECT_EQ(arena_cursor(), 0u);
  arena_alloc(24);
  EXPECT_EQ(arena_cursor(), 24u);
  arena_alloc(16);
  EXPECT_EQ(arena_cursor(), 40u);
  arena_reset();
  EXPECT_EQ(arena_cursor(), 0u);
}

TEST_F(ArenaTest, ValueAtZeroReturnsNull) {
  EXPECT_EQ(cel_value_at(0), nullptr);
}

TEST_F(ArenaTest, ValueAtNonZeroResolvesViaCelMemBase) {
  uint32_t off = arena_alloc(24);
  ASSERT_NE(off, 0u);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(cel_value_at(off)),
            cel_mem_base() + off);
}

TEST_F(ArenaTest, CompatShimCelAllocMatchesArenaAlloc) {
  arena_reset();
  uint32_t a = cel_alloc(24);
  arena_reset();
  uint32_t b = arena_alloc(24);
  EXPECT_EQ(a, b);
}

}  // namespace
}  // namespace celwasm
