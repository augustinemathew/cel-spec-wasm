// ABI invariants for cel_data.h.  The compile-time static_asserts
// in the header catch most of these; this file pins the values at
// the test boundary so a renumber fails loudly during `bazel test`.

#include "compiler_v2/runtime/cel_data.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"

namespace celwasm {
namespace {

TEST(CelData, KindAndErrorCodesAreWireStable) {
  EXPECT_EQ(CEL_NULL, 0);
  EXPECT_EQ(CEL_MAP_ARENA, 8);
  EXPECT_EQ(CEL_MAP_HOST, 9);
  EXPECT_EQ(CEL_ERROR, 16);
  EXPECT_EQ(CEL_ERR_NO_SUCH_KEY, 15);
  EXPECT_EQ(CEL_ERR_DUPLICATE_KEY, 16);
}

TEST(CelData, ArenaMapHeaderLayout) {
  EXPECT_EQ(sizeof(ArenaMapHeader), 16u);
  EXPECT_EQ(offsetof(ArenaMapHeader, count), 0u);
  EXPECT_EQ(offsetof(ArenaMapHeader, capacity), 4u);
  EXPECT_EQ(offsetof(ArenaMapHeader, entries_offset), 8u);
  EXPECT_EQ(static_cast<uint32_t>(kCelMapEntryStride), 2u * sizeof(CelValue));
}

TEST(CelData, ArenaMapPayloadAliasesCorrectOffset) {
  CelValue v;
  v.kind = CEL_MAP_ARENA;
  v.payload.arena_map.header_ptr = 0xDEADBEEFu;
  uint32_t round_trip = 0;
  std::memcpy(&round_trip, reinterpret_cast<const char*>(&v) + 8,
              sizeof(round_trip));
  EXPECT_EQ(round_trip, 0xDEADBEEFu);
}

}  // namespace
}  // namespace celwasm
