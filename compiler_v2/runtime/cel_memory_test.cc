#include "compiler_v2/runtime/cel_memory.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace celwasm {
namespace {

TEST(MemoryTest, BaseIsNonNull) {
  EXPECT_NE(cel_mem_base(), nullptr);
}

TEST(MemoryTest, SizeIsAtLeastOnePage) {
  // Wasm page = 64 KiB; initial module memory is one page.
  EXPECT_GE(cel_mem_size(), 64u * 1024u);
}

TEST(MemoryTest, BaseIsEightByteAligned) {
  // CelValue is 8-byte aligned; the backing buffer must also be, or every
  // CelValue* at an 8-aligned offset would be mis-aligned in host memory.
  const auto addr = reinterpret_cast<uintptr_t>(cel_mem_base());
  EXPECT_EQ(addr % 8u, 0u);
}

}  // namespace
}  // namespace celwasm
