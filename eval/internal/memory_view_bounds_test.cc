// Bounds-check regression for `MemoryView`'s `Read/Write` family.
// Closes cleanup-backlog #36 — `WasmtimeMemoryView::ReadSpan` used
// to return `Data() + ptr` with NO bounds check; a malicious or
// buggy wasm module that passed `ptr=0xFFFFFFFF` through a
// `@host` trampoline could leak host memory (or SIGSEGV on a
// guard page).
//
// These tests exercise `FakeMemoryView`, which shares the same
// `MemoryView` interface + default `IsInBounds` helper — the
// bounds-check contract is on the interface, so the regression
// behaviour is the same for the wasmtime-backed impl.  An
// equivalent end-to-end test against the wasmtime impl needs a
// crafted-bad-ptr wasm fixture and is filed as follow-up under
// the same #36 entry.

#include <cstdint>
#include <cstring>
#include <limits>

#include "absl/strings/string_view.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"

namespace celwasm {
namespace {

using ::celwasm::test::FakeMemoryView;

constexpr uint32_t kMemSize = 1024;

// `IsInBounds` is the centralised predicate; every Read/Write
// shares it.  Covering it on the interface covers all four
// methods' shared contract.

TEST(MemoryViewBoundsTest, IsInBoundsEmptyRangeAlwaysOk) {
  FakeMemoryView mem(kMemSize);
  EXPECT_TRUE(mem.IsInBounds(0, 0));
  EXPECT_TRUE(mem.IsInBounds(kMemSize, 0));      // at end is OK for len=0
  EXPECT_TRUE(mem.IsInBounds(kMemSize - 1, 0));
  EXPECT_TRUE(mem.IsInBounds(0xFFFFFFFFu, 0));   // even past end, len=0 is OK
}

TEST(MemoryViewBoundsTest, IsInBoundsAtBoundary) {
  FakeMemoryView mem(kMemSize);
  EXPECT_TRUE(mem.IsInBounds(0, kMemSize));               // exact fit
  EXPECT_TRUE(mem.IsInBounds(kMemSize - 1, 1));           // last byte
  EXPECT_FALSE(mem.IsInBounds(kMemSize - 1, 2));          // 1 past
  EXPECT_FALSE(mem.IsInBounds(0, kMemSize + 1));          // 1 too long
  EXPECT_FALSE(mem.IsInBounds(kMemSize, 1));              // start at end
}

TEST(MemoryViewBoundsTest, IsInBoundsRejectsOverflowingLen) {
  FakeMemoryView mem(kMemSize);
  // ptr + len would wrap u32; the helper rearranges the arithmetic
  // (`len <= Size() - ptr`) so wrap is impossible.  Concretely:
  // ptr=0x80000000, len=0x80000000 would wrap to 0; the check
  // catches it.
  EXPECT_FALSE(mem.IsInBounds(0x80000000u, 0x80000000u));
  EXPECT_FALSE(mem.IsInBounds(0xFFFFFFFFu, 1));
  EXPECT_FALSE(mem.IsInBounds(0xFFFFFFFFu, 0xFFFFFFFFu));
}

// ── ReadSpan ───────────────────────────────────────────────────

TEST(MemoryViewBoundsTest, ReadSpanInBoundsReturnsView) {
  FakeMemoryView mem(kMemSize);
  // Seed the buffer with 'A' at offset 100 for 16 bytes.
  std::memset(mem.data() + 100, 'A', 16);
  absl::string_view sv = mem.ReadSpan(100, 16);
  EXPECT_EQ(sv.size(), 16u);
  EXPECT_EQ(sv[0], 'A');
  EXPECT_EQ(sv[15], 'A');
}

TEST(MemoryViewBoundsTest, ReadSpanAtEndOfMemoryOk) {
  FakeMemoryView mem(kMemSize);
  std::memset(mem.data() + kMemSize - 4, 'Z', 4);
  absl::string_view sv = mem.ReadSpan(kMemSize - 4, 4);
  EXPECT_EQ(sv.size(), 4u);
  EXPECT_EQ(sv[3], 'Z');
}

TEST(MemoryViewBoundsTest, ReadSpanPastEndReturnsEmpty) {
  FakeMemoryView mem(kMemSize);
  EXPECT_EQ(mem.ReadSpan(kMemSize - 1, 2), absl::string_view{});
  EXPECT_EQ(mem.ReadSpan(kMemSize, 1), absl::string_view{});
  EXPECT_EQ(mem.ReadSpan(kMemSize + 100, 1), absl::string_view{});
}

TEST(MemoryViewBoundsTest, ReadSpanAdversarialMaxPtr) {
  FakeMemoryView mem(kMemSize);
  // The bug #36 was designed to exploit: ptr=0xFFFFFFFF
  // arithmetics to a host-adjacent pointer pre-fix.  Post-fix:
  // bounded.
  EXPECT_EQ(mem.ReadSpan(0xFFFFFFFFu, 1), absl::string_view{});
  EXPECT_EQ(mem.ReadSpan(0xFFFFFFFFu, 0x80000000u), absl::string_view{});
  EXPECT_EQ(mem.ReadSpan(std::numeric_limits<uint32_t>::max(), 1),
            absl::string_view{});
}

// ── ReadCelValue ───────────────────────────────────────────────

TEST(MemoryViewBoundsTest, ReadCelValueInBoundsReturnsValue) {
  FakeMemoryView mem(kMemSize);
  CelValue cv{};
  cv.kind = 2;             // CEL_INT
  cv.payload.i = 42;
  mem.WriteCelValue(64, cv);
  CelValue got = mem.ReadCelValue(64);
  EXPECT_EQ(got.kind, 2u);
  EXPECT_EQ(got.payload.i, 42);
}

TEST(MemoryViewBoundsTest, ReadCelValueAtEndOk) {
  FakeMemoryView mem(kMemSize);
  CelValue cv{};
  cv.kind = 1;
  cv.payload.i = 1;
  mem.WriteCelValue(kMemSize - sizeof(CelValue), cv);
  CelValue got = mem.ReadCelValue(kMemSize - sizeof(CelValue));
  EXPECT_EQ(got.kind, 1u);
}

TEST(MemoryViewBoundsTest, ReadCelValuePastEndReturnsZeroed) {
  FakeMemoryView mem(kMemSize);
  CelValue got = mem.ReadCelValue(kMemSize - 8);  // would read past
  EXPECT_EQ(got.kind, 0u);
  EXPECT_EQ(got.payload.i, 0);
  CelValue past = mem.ReadCelValue(kMemSize + 100);
  EXPECT_EQ(past.kind, 0u);
  CelValue max = mem.ReadCelValue(0xFFFFFFFFu);
  EXPECT_EQ(max.kind, 0u);
}

// ── WriteCelValue + WriteU32 ───────────────────────────────────

TEST(MemoryViewBoundsTest, WriteCelValuePastEndIsNoop) {
  FakeMemoryView mem(kMemSize);
  // Seed a sentinel at the last in-bounds slot.
  CelValue sentinel{};
  sentinel.kind = 0xDEAD;
  mem.WriteCelValue(kMemSize - sizeof(CelValue), sentinel);

  // OOB write must NOT change the sentinel.
  CelValue evil{};
  evil.kind = 0xBEEF;
  mem.WriteCelValue(kMemSize - 8, evil);
  mem.WriteCelValue(kMemSize + 100, evil);
  mem.WriteCelValue(0xFFFFFFFFu, evil);

  CelValue got = mem.ReadCelValue(kMemSize - sizeof(CelValue));
  EXPECT_EQ(got.kind, 0xDEADu) << "OOB write corrupted sentinel";
}

TEST(MemoryViewBoundsTest, WriteU32PastEndIsNoop) {
  FakeMemoryView mem(kMemSize);
  mem.WriteU32(kMemSize - 4, 0xAABBCCDDu);
  // OOB writes must NOT change the bytes.
  mem.WriteU32(kMemSize - 3, 0x11111111u);     // straddles boundary
  mem.WriteU32(kMemSize, 0x22222222u);
  mem.WriteU32(0xFFFFFFFFu, 0x33333333u);

  uint32_t got = 0;
  std::memcpy(&got, mem.data() + kMemSize - 4, sizeof(got));
  EXPECT_EQ(got, 0xAABBCCDDu);
}

// ── Size ───────────────────────────────────────────────────────

TEST(MemoryViewBoundsTest, SizeReportsConfiguredCapacity) {
  FakeMemoryView mem(2048);
  EXPECT_EQ(mem.Size(), 2048u);
  FakeMemoryView tiny(64);
  EXPECT_EQ(tiny.Size(), 64u);
}

}  // namespace
}  // namespace celwasm
