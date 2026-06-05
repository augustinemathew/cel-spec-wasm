#include "compiler/memory_layout.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace celwasm {
namespace {

TEST(MemoryLayoutTest, ReservedLowRegionIsTwoPagesMinusGrowth) {
  // The reserved low region must fit inside the initial-memory
  // declaration; the wasm-ld flag `-Wl,--global-base=N` only pushes
  // wasi-libc's data UP, it doesn't grow memory.
  EXPECT_LT(MemoryLayout::kReservedLowMemoryBytes,
            MemoryLayout::kInitialMemoryPages * MemoryLayout::kWasmPageSize);
}

TEST(MemoryLayoutTest, SlotStrideIsSixteenAlignedAndCelValueSized) {
  EXPECT_EQ(MemoryLayout::kSlotStride % 16u, 0u)
      << "stride must be 16-aligned for `memory.atomic.*` ops";
  EXPECT_GE(MemoryLayout::kSlotStride, 24u)
      << "stride must hold a 24-byte CelValue payload";
}

TEST(MemoryLayoutTest, GuardBandIsAtLeastOneSlot) {
  // Property the `static_assert` in the header pins; mirror it here
  // so a death-test reading the doc sees the intent.
  EXPECT_GE(MemoryLayout::kGuardBytes, MemoryLayout::kSlotStride);
}

TEST(MemoryLayoutTest, MaxWorkspaceBytesShrinksAsRodataGrows) {
  // 1 KiB rodata gives more headroom than 4 KiB.
  const uint32_t a = MemoryLayout::MaxWorkspaceBytes(
      MemoryLayout::kRodataBaseMin, /*rodata_size=*/1024);
  const uint32_t b = MemoryLayout::MaxWorkspaceBytes(
      MemoryLayout::kRodataBaseMin, /*rodata_size=*/4096);
  EXPECT_GT(a, b);
  // Both must stay below the reserved region.
  EXPECT_LT(a, MemoryLayout::kReservedLowMemoryBytes);
  EXPECT_LT(b, MemoryLayout::kReservedLowMemoryBytes);
}

TEST(MemoryLayoutTest, MaxWorkspaceBytesClampsToZeroWhenRodataOverflows) {
  // Rodata alone exceeding the reserved region should give zero
  // headroom (not wrap).
  const uint32_t cap = MemoryLayout::MaxWorkspaceBytes(
      MemoryLayout::kRodataBaseMin,
      /*rodata_size=*/MemoryLayout::kReservedLowMemoryBytes);
  EXPECT_EQ(cap, 0u);
}

TEST(MemoryLayoutTest, MaxWorkspaceBytesAccountsForGuard) {
  // The guard band must be subtracted from the headroom.  With zero
  // rodata, the headroom is `kReservedLowMemoryBytes
  // - kRodataBaseMin - kGuardBytes`.
  const uint32_t cap = MemoryLayout::MaxWorkspaceBytes(
      MemoryLayout::kRodataBaseMin, /*rodata_size=*/0);
  EXPECT_EQ(cap, MemoryLayout::kReservedLowMemoryBytes -
                     MemoryLayout::kRodataBaseMin - MemoryLayout::kGuardBytes);
}

TEST(MemoryLayoutTest, MaxMemoryIsToolchainSixtyFourMiB) {
  // Pinned so a future toolchain bump that changes `--max-memory`
  // forces a deliberate edit here rather than a silent drift.
  EXPECT_EQ(MemoryLayout::kMaxMemoryBytes, 64u * 1024u * 1024u);
}

}  // namespace
}  // namespace celwasm
