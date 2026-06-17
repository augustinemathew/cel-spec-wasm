#include "compiler/codegen/static_memory_builder.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "gtest/gtest.h"
#include "runtime/cel_runtime.h"

namespace celwasm {
namespace {

// Helpers ---------------------------------------------------------------

uint32_t ReadU32LE(const std::vector<uint8_t>& buf, size_t off) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(buf.at(off + i)) << (i * 8);
  }
  return v;
}

uint64_t ReadU64LE(const std::vector<uint8_t>& buf, size_t off) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(buf.at(off + i)) << (i * 8);
  }
  return v;
}

double ReadDoubleLE(const std::vector<uint8_t>& buf, size_t off) {
  const uint64_t bits = ReadU64LE(buf, off);
  double d;
  std::memcpy(&d, &bits, sizeof(d));
  return d;
}

// Tests ---------------------------------------------------------------

TEST(StaticMemoryBuilderTest, EmptyBuilderFinalizesToEmptyBuffer) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  EXPECT_EQ(builder.size_bytes(), 0u);
  EXPECT_EQ(builder.base_offset(), 16u);
  std::vector<uint8_t> out = std::move(builder).Finalize();
  EXPECT_TRUE(out.empty());
}

TEST(StaticMemoryBuilderTest, AllocateNullIsAllZeros) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const uint32_t off = builder.AllocateNull();
  EXPECT_EQ(off, 16u);
  EXPECT_EQ(builder.size_bytes(), 24u);
  std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 24u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_NULL));
  EXPECT_EQ(ReadU32LE(buf, 4), 0u);  // _pad
  for (size_t i = 8; i < 24; ++i) {
    EXPECT_EQ(buf[i], 0u) << "payload byte " << i << " should be zero";
  }
}

TEST(StaticMemoryBuilderTest, AllocateBoolTrue) {
  StaticMemoryBuilder builder(0);
  builder.AllocateBool(true);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 24u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(ReadU32LE(buf, 4), 0u);
  EXPECT_EQ(ReadU32LE(buf, 8), 1u);  // bool at payload offset 8
  for (size_t i = 12; i < 24; ++i) {
    EXPECT_EQ(buf[i], 0u);
  }
}

TEST(StaticMemoryBuilderTest, AllocateBoolFalse) {
  StaticMemoryBuilder builder(0);
  builder.AllocateBool(false);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 24u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(ReadU32LE(buf, 8), 0u);
}

TEST(StaticMemoryBuilderTest, AllocateIntRoundTrips) {
  StaticMemoryBuilder builder(0);
  builder.AllocateInt(-42);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 24u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 8)), int64_t{-42});
  EXPECT_EQ(ReadU64LE(buf, 16), 0u);
}

TEST(StaticMemoryBuilderTest, AllocateIntExtremes) {
  StaticMemoryBuilder builder(0);
  builder.AllocateInt(INT64_MIN);
  builder.AllocateInt(INT64_MAX);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 48u);
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 8)), INT64_MIN);
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 32)), INT64_MAX);
}

TEST(StaticMemoryBuilderTest, AllocateUintRoundTrips) {
  StaticMemoryBuilder builder(0);
  builder.AllocateUint(UINT64_MAX);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_UINT));
  EXPECT_EQ(ReadU64LE(buf, 8), UINT64_MAX);
}

TEST(StaticMemoryBuilderTest, AllocateDoubleRoundTrips) {
  StaticMemoryBuilder builder(0);
  builder.AllocateDouble(3.5);
  builder.AllocateDouble(-0.0);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 48u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_DOUBLE));
  EXPECT_DOUBLE_EQ(ReadDoubleLE(buf, 8), 3.5);
  EXPECT_DOUBLE_EQ(ReadDoubleLE(buf, 32), -0.0);
  // -0.0 has the sign bit set; verify bit-exact encoding (not just
  // value-equal, since 0.0 == -0.0 via operator==).
  const uint64_t neg_zero_bits = ReadU64LE(buf, 32);
  EXPECT_EQ(neg_zero_bits, uint64_t{1} << 63);
}

TEST(StaticMemoryBuilderTest, ConsecutiveScalarsAreEightByteAligned) {
  StaticMemoryBuilder builder(/*base_offset=*/8);
  const uint32_t off0 = builder.AllocateInt(1);
  const uint32_t off1 = builder.AllocateBool(true);
  const uint32_t off2 = builder.AllocateNull();
  EXPECT_EQ(off0, 8u);
  EXPECT_EQ(off1, 8u + 24u);
  EXPECT_EQ(off2, 8u + 48u);
  EXPECT_EQ(builder.size_bytes(), 72u);
  EXPECT_EQ(off1 % 8u, 0u);
  EXPECT_EQ(off2 % 8u, 0u);
}

TEST(StaticMemoryBuilderTest, AllocateStringHeaderAndPayload) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const uint32_t header_off = builder.AllocateString("hello");
  EXPECT_EQ(header_off, 16u);
  // Header is 24 bytes, span payload is 5 bytes, pad to 8 = 3 bytes → 32 total.
  EXPECT_EQ(builder.size_bytes(), 32u);

  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ASSERT_EQ(buf.size(), 32u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(ReadU32LE(buf, 4), 0u);
  // CelSpan { ptr, len } at payload offset 8.
  EXPECT_EQ(ReadU32LE(buf, 8), 16u + 24u);  // base_offset + header_size
  EXPECT_EQ(ReadU32LE(buf, 12), 5u);
  // Tail of 16-byte payload area zero.
  EXPECT_EQ(ReadU64LE(buf, 16), 0u);
  // Span bytes.
  EXPECT_EQ(buf[24], 'h');
  EXPECT_EQ(buf[25], 'e');
  EXPECT_EQ(buf[26], 'l');
  EXPECT_EQ(buf[27], 'l');
  EXPECT_EQ(buf[28], 'o');
  // 3-byte alignment pad.
  EXPECT_EQ(buf[29], 0u);
  EXPECT_EQ(buf[30], 0u);
  EXPECT_EQ(buf[31], 0u);
}

TEST(StaticMemoryBuilderTest, AllocateBytesUsesBytesKind) {
  StaticMemoryBuilder builder(0);
  const char payload[] = {'\x00', '\x01', '\xfe', '\xff'};
  builder.AllocateBytes(absl::string_view(payload, sizeof(payload)));
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  // Header 24 + payload 4 + pad 4 → 32.
  ASSERT_EQ(buf.size(), 32u);
  EXPECT_EQ(ReadU32LE(buf, 0), static_cast<uint32_t>(CEL_BYTES));
  EXPECT_EQ(ReadU32LE(buf, 8), 24u);
  EXPECT_EQ(ReadU32LE(buf, 12), 4u);
  EXPECT_EQ(buf[24], 0x00);
  EXPECT_EQ(buf[25], 0x01);
  EXPECT_EQ(buf[26], 0xfe);
  EXPECT_EQ(buf[27], 0xff);
}

TEST(StaticMemoryBuilderTest, AllocateStringEmptyOccupies24Bytes) {
  StaticMemoryBuilder builder(0);
  const uint32_t off = builder.AllocateString("");
  EXPECT_EQ(off, 0u);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  // Header 24 + 0 payload + 0 pad → 24.
  ASSERT_EQ(buf.size(), 24u);
  EXPECT_EQ(ReadU32LE(buf, 8), 24u);  // ptr points one past the header
  EXPECT_EQ(ReadU32LE(buf, 12), 0u);
}

TEST(StaticMemoryBuilderTest,
     AllocateStringAlignmentIsMaintainedAcrossMixedAllocs) {
  StaticMemoryBuilder builder(/*base_offset=*/0);
  builder.AllocateString("ab");  // 24 header + 2 bytes + 6 pad = 32
  const uint32_t off_next = builder.AllocateInt(7);
  EXPECT_EQ(off_next, 32u);
  EXPECT_EQ(off_next % 8u, 0u);
  EXPECT_EQ(builder.size_bytes(), 56u);
}

TEST(StaticMemoryBuilderTest, AllocateStringSevenBytesPadsByOne) {
  StaticMemoryBuilder builder(0);
  builder.AllocateString("1234567");
  // Header 24 + 7 payload + 1 pad → 32.
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  EXPECT_EQ(buf.size(), 32u);
  EXPECT_EQ(buf[31], 0u);
}

TEST(StaticMemoryBuilderTest, BaseOffsetIsAppliedToReturnedOffsetAndSpanPtr) {
  StaticMemoryBuilder builder(/*base_offset=*/128);
  const uint32_t hoff = builder.AllocateString("x");
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  EXPECT_EQ(hoff, 128u);
  // CelSpan.ptr = base_offset + header_size = 128 + 24 = 152.
  EXPECT_EQ(ReadU32LE(buf, 8), 152u);
}

// MaterializeList ------------------------------------------------------
//
// Layout under test (header-first, then contiguous run, then frame —
// byte-identical to cel_list_create + N×cel_list_append_at):
//   ArenaListHeader is 16 B { count@0, capacity@4, elements_offset@8,
//   _pad@12 }; the run is N × 24-B CelValue frames; the outer frame is a
//   CelValue { CEL_LIST_ARENA@0, _pad@4, arena_list.header_ptr@8 }.

CelValue MakeInt(int64_t v) {
  CelValue c{};
  c.kind = CEL_INT;
  c.payload.i = v;
  return c;
}
CelValue MakeBool(bool v) {
  CelValue c{};
  c.kind = CEL_BOOL;
  c.payload.b = v ? 1 : 0;
  return c;
}
CelValue MakeDouble(double v) {
  CelValue c{};
  c.kind = CEL_DOUBLE;
  c.payload.d = v;
  return c;
}

// `[]` → header { count=0, capacity=0, elements_offset=0 } + frame; no
// run, matching cel_list_create(out, 0).
TEST(StaticMemoryBuilderTest, MaterializeListEmpty) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const auto r = builder.MaterializeList({});
  const std::vector<uint8_t> buf = std::move(builder).Finalize();

  // Header at local 0.
  EXPECT_EQ(ReadU32LE(buf, 0), 0u);   // count
  EXPECT_EQ(ReadU32LE(buf, 4), 0u);   // capacity
  EXPECT_EQ(ReadU32LE(buf, 8), 0u);   // elements_offset (empty → 0)
  EXPECT_EQ(ReadU32LE(buf, 12), 0u);  // _pad
  // Frame at local 16 (immediately after the 16-B header, no run).
  EXPECT_EQ(ReadU32LE(buf, 16), static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_EQ(ReadU32LE(buf, 24), 16u);  // header_ptr = base + 0
  EXPECT_EQ(buf.size(), 40u);          // 16 header + 24 frame

  EXPECT_EQ(r.frame_offset, 32u);  // base(16) + frame_local(16)
  EXPECT_EQ(r.frame.kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_EQ(r.frame.payload.arena_list.header_ptr, 16u);
}

// `[10, 20, 30]` → header { count=3, cap=3 } + 3-frame run + frame.
TEST(StaticMemoryBuilderTest, MaterializeListInts) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const std::vector<CelValue> elems = {MakeInt(10), MakeInt(20), MakeInt(30)};
  const auto r = builder.MaterializeList(elems);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();

  // Header.
  EXPECT_EQ(ReadU32LE(buf, 0), 3u);   // count
  EXPECT_EQ(ReadU32LE(buf, 4), 3u);   // capacity
  EXPECT_EQ(ReadU32LE(buf, 8), 32u);  // elements_offset = base(16) + run_local(16)
  EXPECT_EQ(ReadU32LE(buf, 12), 0u);  // _pad
  // Run: element[i] frame at local 16 + 24*i, INT payload at +8 within.
  EXPECT_EQ(ReadU32LE(buf, 16), static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 24)), 10);
  EXPECT_EQ(ReadU32LE(buf, 40), static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 48)), 20);
  EXPECT_EQ(ReadU32LE(buf, 64), static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 72)), 30);
  // Frame at local 88 (after 16 header + 72 run).
  EXPECT_EQ(ReadU32LE(buf, 88), static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_EQ(ReadU32LE(buf, 96), 16u);  // header_ptr = base + 0
  EXPECT_EQ(buf.size(), 112u);

  EXPECT_EQ(r.frame_offset, 104u);  // base(16) + 88
  EXPECT_EQ(r.frame.payload.arena_list.header_ptr, 16u);
}

// Heterogeneous element kinds are copied frame-for-frame in order.
TEST(StaticMemoryBuilderTest, MaterializeListMixedKinds) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const std::vector<CelValue> elems = {MakeInt(7), MakeBool(true),
                                       MakeDouble(3.5)};
  builder.MaterializeList(elems);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  // element0 INT(7) @16, element1 BOOL(true) @40, element2 DOUBLE(3.5) @64.
  EXPECT_EQ(ReadU32LE(buf, 16), static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 24)), 7);
  EXPECT_EQ(ReadU32LE(buf, 40), static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(ReadU32LE(buf, 48), 1u);  // bool payload @ +8
  EXPECT_EQ(ReadU32LE(buf, 64), static_cast<uint32_t>(CEL_DOUBLE));
  EXPECT_EQ(ReadDoubleLE(buf, 72), 3.5);
}

// A string element's CelSpan, pointing at rodata bytes allocated earlier
// in the same builder, survives verbatim into the run.
TEST(StaticMemoryBuilderTest, MaterializeListStringElement) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const uint32_t str_frame = builder.AllocateString("hi");  // → 16
  const uint32_t payload_ptr = str_frame + 24;              // bytes follow frame
  CelValue elem{};
  elem.kind = CEL_STRING;
  elem.payload.s.ptr = payload_ptr;
  elem.payload.s.len = 2;
  const std::vector<CelValue> elems = {elem};
  builder.MaterializeList(elems);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();

  // AllocateString consumed 32 bytes (24 frame + 2 + 6 pad); the list
  // starts at local 32: header @32, run @48.
  EXPECT_EQ(ReadU32LE(buf, 48), static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(ReadU32LE(buf, 56), payload_ptr);  // span.ptr @ frame+8
  EXPECT_EQ(ReadU32LE(buf, 60), 2u);           // span.len @ frame+12
  // The pointed-at bytes are the original "hi".
  EXPECT_EQ(buf[payload_ptr - 16], 'h');  // local = abs - base
  EXPECT_EQ(buf[payload_ptr - 16 + 1], 'i');
}

// `[1, [2, 3]]`: the inner list's returned frame embeds as the outer
// list's element, its header_ptr pointing at the inner header.
TEST(StaticMemoryBuilderTest, MaterializeNestedList) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const std::vector<CelValue> inner_elems = {MakeInt(2), MakeInt(3)};
  const auto inner = builder.MaterializeList(inner_elems);
  // inner: header@0, run@16 (2×24=48), frame@64 → header_ptr = base+0 = 16.
  EXPECT_EQ(inner.frame.payload.arena_list.header_ptr, 16u);

  const std::vector<CelValue> outer_elems = {MakeInt(1), inner.frame};
  const auto outer = builder.MaterializeList(outer_elems);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();

  // Outer header starts after inner (16+48+24 = 88): header@88, run@104.
  EXPECT_EQ(ReadU32LE(buf, 88), 2u);    // count
  EXPECT_EQ(ReadU32LE(buf, 96), 120u);  // elements_offset = base(16)+104
  // Outer run element1 (the nested list) at local 128.
  EXPECT_EQ(ReadU32LE(buf, 128), static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_EQ(ReadU32LE(buf, 136), 16u);  // header_ptr → inner header (base+0)
  EXPECT_EQ(outer.frame.payload.arena_list.header_ptr, 104u);  // base + 88
}

// elements_offset and header_ptr are absolute (include base_offset).
TEST(StaticMemoryBuilderTest, MaterializeListBaseOffsetPropagates) {
  StaticMemoryBuilder builder(/*base_offset=*/256);
  const std::vector<CelValue> elems = {MakeInt(5)};
  const auto r = builder.MaterializeList(elems);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  EXPECT_EQ(ReadU32LE(buf, 8), 256u + 16u);   // elements_offset
  EXPECT_EQ(r.frame.payload.arena_list.header_ptr, 256u);
  EXPECT_EQ(r.frame_offset, 256u + 16u + 24u);  // base + header + 1 run frame
  EXPECT_EQ(r.frame_offset % 8u, 0u);
}

// The cursor stays 8-byte aligned, so a following Allocate is aligned.
TEST(StaticMemoryBuilderTest, MaterializeListLeavesCursorAligned) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const std::vector<CelValue> elems = {MakeInt(1), MakeInt(2)};
  builder.MaterializeList(elems);
  const uint32_t next = builder.AllocateInt(99);
  EXPECT_EQ(next % 8u, 0u);
}

}  // namespace
}  // namespace celwasm
