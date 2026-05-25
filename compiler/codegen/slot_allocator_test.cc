#include "compiler_v2/codegen/slot_allocator.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace celwasm {
namespace {

TEST(SlotAllocatorTest, EmptyAtConstruction) {
  SlotAllocator a(/*base_offset=*/0, /*debug_mode=*/true);
  EXPECT_EQ(a.peak_slots(), 0u);
  EXPECT_EQ(a.total_bytes(), 0u);
  EXPECT_EQ(a.base_offset(), 0u);
  EXPECT_TRUE(a.debug_mode());
}

TEST(SlotAllocatorTest, AcquireIsMonotonicFromBase) {
  SlotAllocator a(/*base_offset=*/8, /*debug_mode=*/true);
  EXPECT_EQ(a.Acquire(), 8u);
  EXPECT_EQ(a.Acquire(), 8u + 24u);
  EXPECT_EQ(a.Acquire(), 8u + 48u);
  EXPECT_EQ(a.peak_slots(), 3u);
  EXPECT_EQ(a.total_bytes(), 72u);
}

TEST(SlotAllocatorTest, ReleaseIsNoOpInNaivePath) {
  SlotAllocator a(/*base_offset=*/0, /*debug_mode=*/true);
  const uint32_t s0 = a.Acquire();
  const uint32_t s1 = a.Acquire();
  a.Release(s0);
  a.Release(s1);
  // Next Acquire still hands out a fresh cell — naive path never
  // reuses.
  EXPECT_EQ(a.Acquire(), 48u);
  EXPECT_EQ(a.peak_slots(), 3u);
}

TEST(SlotAllocatorTest, DebugModeFlagIsPreserved) {
  SlotAllocator debug(/*base_offset=*/0, /*debug_mode=*/true);
  SlotAllocator prod(/*base_offset=*/0, /*debug_mode=*/false);
  EXPECT_TRUE(debug.debug_mode());
  EXPECT_FALSE(prod.debug_mode());
  // In M1 both modes behave identically.
  EXPECT_EQ(debug.Acquire(), 0u);
  EXPECT_EQ(prod.Acquire(), 0u);
}

TEST(SlotAllocatorTest, NonZeroBaseOffsetRespected) {
  SlotAllocator a(/*base_offset=*/1024, /*debug_mode=*/true);
  EXPECT_EQ(a.Acquire(), 1024u);
  EXPECT_EQ(a.Acquire(), 1048u);
  EXPECT_EQ(a.base_offset(), 1024u);
}

TEST(SlotAllocatorDeathTest, UnalignedBaseOffsetChecks) {
  EXPECT_DEATH(
      { SlotAllocator a(/*base_offset=*/1, /*debug_mode=*/true); },
      "8-byte aligned");
  EXPECT_DEATH(
      { SlotAllocator a(/*base_offset=*/7, /*debug_mode=*/true); },
      "8-byte aligned");
}

TEST(SlotAllocatorTest, AlignedBaseOffsetsAccepted) {
  // 8-byte alignment is the hard constraint; 16 / 24 / 32 are all fine.
  SlotAllocator a8(/*base_offset=*/8, /*debug_mode=*/true);
  SlotAllocator a16(/*base_offset=*/16, /*debug_mode=*/true);
  SlotAllocator a24(/*base_offset=*/24, /*debug_mode=*/true);
  EXPECT_EQ(a8.Acquire(), 8u);
  EXPECT_EQ(a16.Acquire(), 16u);
  EXPECT_EQ(a24.Acquire(), 24u);
}

}  // namespace
}  // namespace celwasm
