#include "compiler/codegen/static_memory_builder.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"
#include "gtest/gtest.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_make.h"
#include "runtime/cel_map.h"
#include "runtime/cel_map_hash.h"
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
  EXPECT_EQ(ReadU32LE(buf, 0), 3u);  // count
  EXPECT_EQ(ReadU32LE(buf, 4), 3u);  // capacity
  EXPECT_EQ(ReadU32LE(buf, 8),
            32u);  // elements_offset = base(16) + run_local(16)
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
  const uint32_t payload_ptr = str_frame + 24;  // bytes follow frame
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
  EXPECT_EQ(ReadU32LE(buf, 8), 256u + 16u);  // elements_offset
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

// MaterializeMap -------------------------------------------------------
//
// Layout under test (header-first, then 48-byte {key,val} run, then — for
// N >= kCelMapIndexThreshold — the baked SwissTable index, then frame):
//   ArenaMapHeader is 16 B { count@0, capacity@4, entries_offset@8,
//   index_offset@12 }; the run is N × 48-B {key,val} pairs; the index is
//   [ctrl: num_slots+7][pad to 4][slot: num_slots × u32]; the outer frame
//   is a CelValue { CEL_MAP_ARENA@0, _pad@4, arena_map.header_ptr@8 }.

using MapEntry = StaticMemoryBuilder::MapEntry;
using MaterializedMap = StaticMemoryBuilder::MaterializedMap;

// Deref a MaterializeMap result through a CHECK the optional-access checker
// models as a guard, so the dependent EXPECT_EQs read the value cleanly.
// (A gtest ASSERT_TRUE(has_value()) is not recognized by the checker.)
const MaterializedMap& Unwrap(const std::optional<MaterializedMap>& r) {
  ABSL_CHECK(r.has_value());
  return *r;
}

// Allocate a string into the builder and return a CEL_STRING key whose
// span points at the payload bytes (mirrors ConstToCelValue's string arm).
CelValue MakeStringKey(StaticMemoryBuilder& b, absl::string_view s) {
  const uint32_t frame = b.AllocateString(s);
  CelValue c{};
  c.kind = CEL_STRING;
  c.payload.s.ptr = frame + static_cast<uint32_t>(sizeof(CelValue));
  c.payload.s.len = static_cast<uint32_t>(s.size());
  return c;
}

// `{}` → header { count=0, capacity=0, entries_offset=0, index_offset=0 }
// + frame; no run, no index — matching cel_map_create(out, 0).
TEST(StaticMemoryBuilderTest, MaterializeMapEmpty) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const auto r = builder.MaterializeMap({});
  ASSERT_TRUE(r.has_value());
  const std::vector<uint8_t> buf = std::move(builder).Finalize();

  EXPECT_EQ(ReadU32LE(buf, 0), 0u);   // count
  EXPECT_EQ(ReadU32LE(buf, 4), 0u);   // capacity
  EXPECT_EQ(ReadU32LE(buf, 8), 0u);   // entries_offset (empty → 0)
  EXPECT_EQ(ReadU32LE(buf, 12), 0u);  // index_offset (none)
  // Frame at local 16 (after the 16-B header, no run, no index).
  EXPECT_EQ(ReadU32LE(buf, 16), static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(ReadU32LE(buf, 24), 16u);  // header_ptr = base + 0
  EXPECT_EQ(buf.size(), 40u);          // 16 header + 24 frame

  const MaterializedMap& m = Unwrap(r);
  EXPECT_EQ(m.frame_offset, 32u);  // base(16) + frame_local(16)
  EXPECT_EQ(m.entries_offset, 0u);
  EXPECT_EQ(m.index_offset, 0u);
  EXPECT_EQ(m.frame.kind, static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(m.frame.payload.arena_map.header_ptr, 16u);
}

// Single int-keyed entry: header + one 48-B {key,val} + frame, NO index
// (N=1 < threshold), index_offset=0.
TEST(StaticMemoryBuilderTest, MaterializeMapSingleIntEntryNoIndex) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const std::vector<MapEntry> entries = {MapEntry{MakeInt(7), MakeInt(42)}};
  const auto r = builder.MaterializeMap(entries);
  ASSERT_TRUE(r.has_value());
  const std::vector<uint8_t> buf = std::move(builder).Finalize();

  EXPECT_EQ(ReadU32LE(buf, 0), 1u);   // count
  EXPECT_EQ(ReadU32LE(buf, 4), 1u);   // capacity
  EXPECT_EQ(ReadU32LE(buf, 8), 32u);  // entries_offset = base(16)+run_local(16)
  EXPECT_EQ(ReadU32LE(buf, 12), 0u);  // index_offset (N<8)
  // Entry: key INT(7) @16, val INT(42) @40 (24-B apart within the 48-B pair).
  EXPECT_EQ(ReadU32LE(buf, 16), static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 24)), 7);
  EXPECT_EQ(ReadU32LE(buf, 40), static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, 48)), 42);
  // Frame at local 64 (16 header + 48 entry); header_ptr = base + 0.
  EXPECT_EQ(ReadU32LE(buf, 64), static_cast<uint32_t>(CEL_MAP_ARENA));
  EXPECT_EQ(ReadU32LE(buf, 72), 16u);
  const MaterializedMap& m = Unwrap(r);
  EXPECT_EQ(m.entries_offset, 32u);
  EXPECT_EQ(m.index_offset, 0u);
}

// N entries below threshold: 48-B run in source order, no index.
TEST(StaticMemoryBuilderTest, MaterializeMapBelowThresholdNoIndex) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  std::vector<MapEntry> entries;
  entries.reserve(4);
  for (int64_t i = 0; i < 4; ++i) {
    entries.push_back(MapEntry{MakeInt(i), MakeInt(i * 10)});
  }
  const auto r = builder.MaterializeMap(entries);
  ASSERT_TRUE(r.has_value());
  const std::vector<uint8_t> buf = std::move(builder).Finalize();

  EXPECT_EQ(ReadU32LE(buf, 0), 4u);   // count
  EXPECT_EQ(ReadU32LE(buf, 12), 0u);  // index_offset (4 < 8)
  EXPECT_EQ(Unwrap(r).index_offset, 0u);
  // Entries are in source order: key i @ 16+48*i, payload @ +8 of the key.
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t key_local = 16u + (48u * i);
    EXPECT_EQ(ReadU32LE(buf, key_local), static_cast<uint32_t>(CEL_INT));
    EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, key_local + 8)), i);
    EXPECT_EQ(static_cast<int64_t>(ReadU64LE(buf, key_local + 24 + 8)), i * 10);
  }
}

// N >= threshold: the index is baked.  Verify index_offset points just
// past the entry run, ctrl bytes are all valid (full = top-bit-clear H2,
// or kEmpty) with exactly N full slots, and every entry index 0..N-1
// appears once in the slot array of a full slot.

// Verify the well-formedness of a baked index block at `index_local`
// (buffer-local offset) for `num_slots` slots over `n` entries: exactly n
// full control bytes (each a 7-bit H2), the 7 cloned mirror bytes match
// the first 7 primary bytes, and the slot array holds each entry index
// 0..n-1 exactly once.
// Exactly n control bytes are full (each a 7-bit H2, top bit clear); the
// rest are kEmpty (0x80).
void VerifyCtrlBytesFullCount(const std::vector<uint8_t>& buf,
                              uint32_t index_local, uint32_t num_slots,
                              uint32_t n) {
  uint32_t full = 0;
  for (uint32_t s = 0; s < num_slots; ++s) {
    const uint8_t cb = buf.at(index_local + s);
    if (cb == 0x80) continue;  // kEmpty
    EXPECT_EQ(cb & 0x80u, 0u) << "full ctrl byte must be a 7-bit H2";
    ++full;
  }
  EXPECT_EQ(full, n) << "exactly n slots are full";
}

// The 7 cloned mirror bytes match the first 7 primary control bytes.
void VerifyCtrlMirrorBytes(const std::vector<uint8_t>& buf,
                           uint32_t index_local, uint32_t num_slots) {
  for (uint32_t c = 0; c < 7u; ++c) {
    EXPECT_EQ(buf.at(index_local + num_slots + c), buf.at(index_local + c))
        << "cloned ctrl byte " << c;
  }
}

// The slot array holds each entry index 0..n-1 exactly once (full slots only).
void VerifySlotArrayCoversAllEntries(const std::vector<uint8_t>& buf,
                                     uint32_t index_local, uint32_t num_slots,
                                     uint32_t n) {
  const uint32_t ctrl_total = num_slots + 7u;
  const uint32_t slot_arr_local = index_local + ((ctrl_total + 3u) & ~3u);
  std::vector<bool> seen(n, false);
  for (uint32_t s = 0; s < num_slots; ++s) {
    if (buf.at(index_local + s) == 0x80) continue;  // empty slot
    const uint32_t entry = ReadU32LE(buf, slot_arr_local + (4u * s));
    ASSERT_LT(entry, n);
    EXPECT_FALSE(seen[entry]) << "entry " << entry << " placed twice";
    seen[entry] = true;
  }
  for (uint32_t i = 0; i < n; ++i) {
    EXPECT_TRUE(seen[i]) << "entry " << i;
  }
}

void VerifyBakedIndexStructure(const std::vector<uint8_t>& buf,
                               uint32_t index_local, uint32_t num_slots,
                               uint32_t n) {
  VerifyCtrlBytesFullCount(buf, index_local, num_slots, n);
  VerifyCtrlMirrorBytes(buf, index_local, num_slots);
  VerifySlotArrayCoversAllEntries(buf, index_local, num_slots, n);
}

TEST(StaticMemoryBuilderTest, MaterializeMapAtThresholdBakesIndex) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  std::vector<MapEntry> entries;
  entries.reserve(static_cast<size_t>(kCelMapIndexThreshold));
  for (int64_t i = 0; i < kCelMapIndexThreshold; ++i) {
    entries.push_back(MapEntry{MakeInt(i), MakeInt(i)});
  }
  const auto N = static_cast<uint32_t>(entries.size());
  const auto r = builder.MaterializeMap(entries);
  ASSERT_TRUE(r.has_value());
  const MaterializedMap& m = Unwrap(r);
  const std::vector<uint8_t> buf = std::move(builder).Finalize();

  EXPECT_EQ(ReadU32LE(buf, 0), N);  // count
  const uint32_t index_off_abs = ReadU32LE(buf, 12);
  ASSERT_NE(index_off_abs, 0u) << "N>=threshold must bake an index";
  EXPECT_EQ(m.index_offset, index_off_abs);
  // index_offset = entries_offset + 48*N (run immediately precedes index).
  EXPECT_EQ(index_off_abs, m.entries_offset + (48u * N));

  // num_slots for N=8 is 16 (ceil(64/7)=10 → 16).
  VerifyBakedIndexStructure(buf, index_off_abs - 16u, /*num_slots=*/16u, N);
}

// String keys: the entry-run key CelValue's span survives verbatim and
// points at the rodata bytes allocated earlier in the same builder.
TEST(StaticMemoryBuilderTest, MaterializeMapStringKeyPayloadAdjacency) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const CelValue key = MakeStringKey(builder, "k");  // frame @16, bytes @40
  const std::vector<MapEntry> entries = {MapEntry{key, MakeInt(9)}};
  const auto r = builder.MaterializeMap(entries);
  ASSERT_TRUE(r.has_value());
  const std::vector<uint8_t> buf = std::move(builder).Finalize();

  // AllocateString("k") consumed 32 bytes (24 frame + 1 + 7 pad); the map
  // header starts at local 32, run at 48.
  const uint32_t key_local = Unwrap(r).entries_offset - 16u;
  EXPECT_EQ(ReadU32LE(buf, key_local), static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(ReadU32LE(buf, key_local + 8), key.payload.s.ptr);  // span.ptr
  EXPECT_EQ(ReadU32LE(buf, key_local + 12), 1u);                // span.len
  EXPECT_EQ(buf.at(key.payload.s.ptr - 16u), 'k');  // the staged byte
}

// A const map value that is itself a materialized aggregate (here a const
// list) embeds by its frame; the value slot carries CEL_LIST_ARENA whose
// header_ptr resolves to the inner list header.
TEST(StaticMemoryBuilderTest, MaterializeMapNestedListValue) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const auto inner = builder.MaterializeList({MakeInt(2), MakeInt(3)});
  const std::vector<MapEntry> entries = {MapEntry{MakeInt(1), inner.frame}};
  const auto r = builder.MaterializeMap(entries);
  ASSERT_TRUE(r.has_value());
  const std::vector<uint8_t> buf = std::move(builder).Finalize();

  // The value (second CelValue of the entry pair) is the nested list frame.
  const uint32_t val_local = (Unwrap(r).entries_offset - 16u) + 24u;
  EXPECT_EQ(ReadU32LE(buf, val_local), static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_EQ(ReadU32LE(buf, val_local + 8),
            inner.frame.payload.arena_list.header_ptr);
}

// Duplicate keys → nullopt (NOT materializable): the runtime build path
// must handle it so it poisons CEL_ERR_DUPLICATE_KEY with today's
// semantics.  Covered below threshold (O(n^2) scan) and at threshold
// (index placement dup detection).
TEST(StaticMemoryBuilderTest, MaterializeMapDuplicateKeyBelowThreshold) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const std::vector<MapEntry> entries = {MapEntry{MakeInt(1), MakeInt(10)},
                                         MapEntry{MakeInt(1), MakeInt(20)}};
  EXPECT_FALSE(builder.MaterializeMap(entries).has_value());
}

TEST(StaticMemoryBuilderTest, MaterializeMapDuplicateKeyAtThreshold) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  std::vector<MapEntry> entries;
  entries.reserve(static_cast<size_t>(kCelMapIndexThreshold));
  for (int64_t i = 0; i < kCelMapIndexThreshold - 1; ++i) {
    entries.push_back(MapEntry{MakeInt(i), MakeInt(i)});
  }
  // Re-add key 0 as the threshold-th entry → duplicate.
  entries.push_back(MapEntry{MakeInt(0), MakeInt(999)});
  ASSERT_EQ(entries.size(), static_cast<size_t>(kCelMapIndexThreshold));
  EXPECT_FALSE(builder.MaterializeMap(entries).has_value());
}

// Cross-type numeric duplicate: int 1 and uint 1 are map-equal under
// cel_value_eq, so {1: a, 1u: b} is a duplicate-key map (not materializable).
TEST(StaticMemoryBuilderTest, MaterializeMapCrossTypeNumericDuplicate) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  CelValue uint_one{};
  uint_one.kind = CEL_UINT;
  uint_one.payload.u = 1u;
  const std::vector<MapEntry> entries = {MapEntry{MakeInt(1), MakeInt(10)},
                                         MapEntry{uint_one, MakeInt(20)}};
  EXPECT_FALSE(builder.MaterializeMap(entries).has_value());
}

// Cross-type numeric duplicate AT the index threshold: the at/above-
// threshold dup detection runs through SwissTable index placement (NOT the
// below-threshold O(n^2) cel_value_eq scan).  int 3 and uint 3 hash
// identically (§5 hash-canonicalization: int/uint/double of the same
// integer value share a hash) and cel_value_eq as map-equal, so 7 distinct
// int keys 0..6 plus a uint-3 8th entry (count == kCelMapIndexThreshold) is
// a duplicate-key map the index path must reject → nullopt.  Pins the
// load-bearing canonicalization invariant at the index path, not just the
// linear path.
TEST(StaticMemoryBuilderTest,
     MaterializeMapCrossTypeNumericDuplicateAtThreshold) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  std::vector<MapEntry> entries;
  entries.reserve(static_cast<size_t>(kCelMapIndexThreshold));
  for (int64_t i = 0; i < kCelMapIndexThreshold - 1; ++i) {
    entries.push_back(MapEntry{MakeInt(i), MakeInt(i)});
  }
  CelValue uint_three{};
  uint_three.kind = CEL_UINT;
  uint_three.payload.u = 3u;  // cel_value_eq-equal to int 3 (already present)
  entries.push_back(MapEntry{uint_three, MakeInt(999)});
  ASSERT_EQ(entries.size(), static_cast<size_t>(kCelMapIndexThreshold));
  EXPECT_FALSE(builder.MaterializeMap(entries).has_value());
}

// Cursor stays 8-aligned after a materialized map.
TEST(StaticMemoryBuilderTest, MaterializeMapLeavesCursorAligned) {
  StaticMemoryBuilder builder(/*base_offset=*/16);
  const std::vector<MapEntry> entries = {MapEntry{MakeInt(1), MakeInt(2)}};
  builder.MaterializeMap(entries);
  EXPECT_EQ(builder.AllocateInt(99) % 8u, 0u);
}

// ── BYTE-IDENTITY KEYSTONE (the gate) ────────────────────────────────
//
// A materialized const map must be byte-identical to the same map built
// at runtime via cel_map_create + N×cel_map_insert (source order) +
// cel_map_index_build.  Any drift between MaterializeMap and the runtime
// builders fails by name here.  This is the m32.B keystone: the baked
// index must reproduce cel_map_index_build's control bytes + slot array
// exactly, so a lookup in a materialized map resolves through the index
// identically to a runtime-built one.

// Total bytes of the index block for `count` entries (mirrors
// cel_map_index.c's index_block_bytes, recomputed from the shared kernel).
uint32_t IndexBlockBytesForCount(uint32_t count) {
  const uint32_t num_slots = cel_map_index_num_slots(count);
  const uint32_t ctrl_total = num_slots + 7u;  // + cloned mirror
  const uint32_t slot_arr_off = (ctrl_total + 3u) & ~3u;
  return slot_arr_off + (num_slots * 4u);
}

// The keystone is parameterized over key KIND × entry COUNT.  For each
// cell it builds the same map at runtime (cel_map_create + N×cel_map_insert
// + cel_map_index_build) and via MaterializeMap, then memcmps the baked
// index block — read from EACH side's own index_offset — between the two.
// The index is fully offset-independent (control bytes are H2 values, slot
// entries are indices; both derive only from key CONTENT), so it must be
// bit-for-bit identical regardless of where the keys' bytes live.
//
// For non-span keys (int / uint / bool) the entry-run bytes carry no
// inter-region pointers, so the 48·N run is ALSO compared byte-for-byte
// (the materialized map is built at base_offset == the runtime header
// offset so the run aligns).  For string keys the run's CelSpan ptrs
// legitimately differ (bytes live at different bases) — only the index is
// compared, which is the load-bearing gate (a materialized string-key
// map's index resolves lookups exactly as the runtime-built one's).
enum class KeyKind : uint8_t { kInt, kUint, kString, kBool };

bool KeyKindIsSpan(KeyKind k) {
  return k == KeyKind::kString;
}

// The i-th distinct key for the runtime build, made in the live arena.
uint32_t MakeRuntimeKey(KeyKind kind, uint32_t i) {
  switch (kind) {
    case KeyKind::kInt:
      return cel_make_int(static_cast<int64_t>(i));
    case KeyKind::kUint:
      return cel_make_uint(static_cast<uint64_t>(i));
    case KeyKind::kBool:
      return cel_make_bool(i != 0 ? 1 : 0);
    case KeyKind::kString: {
      const std::string k = "key" + std::to_string(i);
      return cel_make_string(k.data(), static_cast<uint32_t>(k.size()));
    }
  }
  ABSL_CHECK(false) << "unhandled KeyKind";
}

// The i-th distinct key for the materializer (mirrors MakeRuntimeKey's
// content).  String keys allocate their bytes into `b`.
CelValue MakeBuilderKey(KeyKind kind, uint32_t i, StaticMemoryBuilder& b) {
  switch (kind) {
    case KeyKind::kInt:
      return MakeInt(static_cast<int64_t>(i));
    case KeyKind::kUint: {
      CelValue c{};
      c.kind = CEL_UINT;
      c.payload.u = static_cast<uint64_t>(i);
      return c;
    }
    case KeyKind::kBool:
      return MakeBool(i != 0);
    case KeyKind::kString:
      return MakeStringKey(b, "key" + std::to_string(i));
  }
  ABSL_CHECK(false) << "unhandled KeyKind";
}

// The runtime-built reference: header + snapshots of the entry run and the
// index block (captured before MaterializeMap's arena_reset clobbers them).
struct RuntimeMapRef {
  uint32_t header_off = 0;
  ArenaMapHeader hdr{};
  std::vector<uint8_t> run;    // 48*n bytes
  std::vector<uint8_t> index;  // index block (empty when none built)
};

// Byte length of an N-entry 48-B {key,val} run, in size_t to avoid a
// 32-bit multiplication widening into a size_t parameter.
size_t RunBytes(uint32_t n) {
  return static_cast<size_t>(48u) * n;
}

// Build `{key_0:0, key_1:1, …}` at runtime and snapshot run + index.
RuntimeMapRef BuildRuntimeMap(KeyKind kind, uint32_t n) {
  arena_init(CELWASM_ARENA_CAPACITY_BYTES);
  arena_reset();
  const uint32_t out = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  cel_map_create(out, n);
  for (uint32_t i = 0; i < n; ++i) {
    cel_map_insert(out, MakeRuntimeKey(kind, i),
                   cel_make_int(static_cast<int64_t>(i)));
  }
  cel_map_index_build(out);
  const CelValue* m = cel_value_at(out);
  RuntimeMapRef ref;
  ref.header_off = m->payload.arena_map.header_ptr;
  ref.hdr =
      *reinterpret_cast<const ArenaMapHeader*>(cel_mem_base() + ref.header_off);
  ref.run.resize(RunBytes(n));
  std::memcpy(ref.run.data(), cel_mem_base() + ref.hdr.entries_offset,
              ref.run.size());
  if (ref.hdr.index_offset != 0u) {
    ref.index.resize(IndexBlockBytesForCount(n));
    std::memcpy(ref.index.data(), cel_mem_base() + ref.hdr.index_offset,
                ref.index.size());
  }
  return ref;
}

// Compare the materialized header count / capacity to the runtime ref.
// The header may not sit at buf[0]: span keys allocate their payload
// frames into the builder before the map header, so locate it via the
// frame's header_ptr (absolute), converted to a buffer-local offset.
void ExpectHeaderMatches(uint32_t n, const RuntimeMapRef& ref,
                         const StaticMemoryBuilder::MaterializedMap& r,
                         const std::vector<uint8_t>& buf,
                         uint32_t base_offset) {
  const uint32_t hdr_local = r.frame.payload.arena_map.header_ptr - base_offset;
  EXPECT_EQ(ReadU32LE(buf, hdr_local + 0u), ref.hdr.count) << "count n=" << n;
  EXPECT_EQ(ReadU32LE(buf, hdr_local + 4u), ref.hdr.capacity)
      << "capacity n=" << n;
}

// Compare the materialized baked index block to the runtime ref, read from
// each side's own index_offset (the block is offset-independent).  Below
// threshold neither side has an index.
void ExpectIndexMatches(uint32_t n, const RuntimeMapRef& ref,
                        const StaticMemoryBuilder::MaterializedMap& r,
                        const std::vector<uint8_t>& buf, uint32_t base_offset) {
  if (n < static_cast<uint32_t>(kCelMapIndexThreshold)) {
    EXPECT_EQ(ref.hdr.index_offset, 0u) << "n<threshold runtime index";
    EXPECT_EQ(r.index_offset, 0u) << "n<threshold materialized index";
    return;
  }
  ASSERT_NE(ref.hdr.index_offset, 0u) << "runtime built no index (n=" << n;
  ASSERT_NE(r.index_offset, 0u) << "materializer baked no index (n=" << n;
  const uint32_t blk = IndexBlockBytesForCount(n);
  const uint8_t* mat_idx = buf.data() + (r.index_offset - base_offset);
  EXPECT_EQ(std::memcmp(mat_idx, ref.index.data(), blk), 0)
      << "baked index block differs (n=" << n << ")";
}

// Compare the materialized index block (and, for non-span keys, the entry
// run) against the runtime reference.
void ExpectIndexByteIdentical(KeyKind kind, uint32_t n,
                              const RuntimeMapRef& ref,
                              const StaticMemoryBuilder::MaterializedMap& r,
                              const std::vector<uint8_t>& buf,
                              uint32_t base_offset) {
  ExpectHeaderMatches(n, ref, r, buf, base_offset);
  if (!KeyKindIsSpan(kind)) {
    const uint8_t* mat_run = buf.data() + (r.entries_offset - base_offset);
    EXPECT_EQ(std::memcmp(mat_run, ref.run.data(), RunBytes(n)), 0)
        << "entry run differs (n=" << n << ")";
  }
  ExpectIndexMatches(n, ref, r, buf, base_offset);
}

void ExpectMapByteIdentical(KeyKind kind, uint32_t n) {
  const RuntimeMapRef ref = BuildRuntimeMap(kind, n);
  ASSERT_EQ(ref.hdr.count, n) << "runtime map not built (n=" << n << ")";

  // Materialize at the runtime header offset so non-span entry-run bytes
  // align exactly; for span keys the base is irrelevant to the index.
  StaticMemoryBuilder builder(ref.header_off);
  std::vector<MapEntry> entries;
  entries.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    entries.push_back(MapEntry{MakeBuilderKey(kind, i, builder),
                               MakeInt(static_cast<int64_t>(i))});
  }
  const auto r = builder.MaterializeMap(entries);
  ASSERT_TRUE(r.has_value()) << "n=" << n;
  if (!r.has_value()) return;  // guard for bugprone-unchecked-optional-access
  const std::vector<uint8_t> buf = std::move(builder).Finalize();
  ExpectIndexByteIdentical(kind, n, ref, *r, buf, ref.header_off);
}

// Key-kind × count matrix.  Counts straddle the threshold (7 below, 8 at)
// and the num_slots transitions (15 → 32, 56 → 64; cf.
// cel_map_index_num_slots: 8→16, 14→16, 15→32, 56→64, 64→128).  bool keys
// have only two distinct values, so bool is capped at N=2 (below
// threshold, no index) — fabricating an 8-entry bool map is impossible.
struct KeystoneCase {
  KeyKind kind;
  uint32_t n;
  std::string name;
};

class StaticMemoryBuilderKeystoneMatrix
    : public ::testing::TestWithParam<KeystoneCase> {};

TEST_P(StaticMemoryBuilderKeystoneMatrix, MaterializedIndexMatchesRuntime) {
  const KeystoneCase& c = GetParam();
  ExpectMapByteIdentical(c.kind, c.n);
}

std::vector<KeystoneCase> KeystoneCases() {
  std::vector<KeystoneCase> cases;
  const uint32_t ns[] = {7u, 8u, 15u, 16u, 56u, 64u};
  for (const auto [kind, prefix] :
       {std::pair{KeyKind::kInt, "Int"}, std::pair{KeyKind::kUint, "Uint"},
        std::pair{KeyKind::kString, "String"}}) {
    for (uint32_t n : ns) {
      cases.push_back(KeystoneCase{
          kind, n, std::string(prefix) + "_N" + std::to_string(n)});
    }
  }
  // bool: only true/false exist → N=2 (below threshold, no index).
  cases.push_back(KeystoneCase{KeyKind::kBool, 2u, "Bool_N2"});
  return cases;
}

INSTANTIATE_TEST_SUITE_P(
    KeyKindByCount, StaticMemoryBuilderKeystoneMatrix,
    ::testing::ValuesIn(KeystoneCases()),
    [](const ::testing::TestParamInfo<KeystoneCase>& info) {
      return info.param.name;
    });

}  // namespace
}  // namespace celwasm
