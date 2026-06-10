// Layer-2 trampoline body for `cel_host.cel_map_eq`.  Drives
// CelMapEqImpl through fake MemoryView / ExternrefTable /
// ArenaAllocator impls so the test doesn't depend on wasmtime.
//
// The load-bearing surface is CROSS-ORIGIN equality: a CEL_MAP_HOST
// operand (proto map field / Activation binding) compared against a
// CEL_MAP_ARENA operand (map literal in linear memory), the shape
// behind the conformance rows
// `proto{2,3} set_null/map_{timestamp,duration}_null_pruned`.
//
// Coverage matrix:
//   - all 4 valid map key kinds (bool / int / uint / string) ×
//     both directions (host==arena, arena==host): equal, value
//     mismatch, missing key, size mismatch
//   - empty maps cross-origin
//   - timestamp / duration values cross-origin (the conformance
//     rows' exact value shape)
//   - string values cross-origin (span reads on both sides)
//   - numeric cross-type key match (int key vs uint key — langdef
//     §"Equality" ladder, mirroring the arena kernel's
//     `map_keys_equal`)
//   - host+host and arena+arena still compare through the
//     normalized walk
//   - 3VL on operand pair (unknown / error propagate)
//   - non-map operand poisons TYPE_MISMATCH
//   - missing ref_slot surfaces a non-OK Status (infrastructure
//     failure — codegen drift, not user-visible)

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"

namespace celwasm {
namespace {

using FakeMemory = test::FakeMemoryView;
using FakeRefs = test::FakeExternrefTable;

// ─── Fixture ───────────────────────────────────────────────────────
struct Fixture {
  FakeMemory mem;
  FakeRefs refs;
  test::FakeArenaAllocator arena{&mem, /*base_offset=*/8192u,
                                 /*capacity=*/8192u};
  CelHostBindings bindings{};
  TrampolineContext ctx{bindings, mem, refs, arena};

  uint32_t a_slot = 24;  // offsets aligned to CelValue size for clarity
  uint32_t b_slot = 48;
  uint32_t out_slot = 72;

  // Staging cursors for hand-built arena structures, kept below the
  // FakeArenaAllocator's base so EncodeBackingScalar allocations
  // can't collide with them.
  uint32_t string_cursor = 256;
  uint32_t map_cursor = 1024;
};

// ─── CelValue makers ───────────────────────────────────────────────
CelValue MakeBool(bool b) {
  CelValue v{};
  v.kind = CEL_BOOL;
  v.payload.b = b ? 1 : 0;
  return v;
}
CelValue MakeInt(int64_t i) {
  CelValue v{};
  v.kind = CEL_INT;
  v.payload.i = i;
  return v;
}
CelValue MakeUint(uint64_t u) {
  CelValue v{};
  v.kind = CEL_UINT;
  v.payload.u = u;
  return v;
}
// String payload bytes are staged into the fake memory at the
// fixture's string cursor so the read path has real data.
CelValue MakeString(Fixture& f, absl::string_view s) {
  const uint32_t off = f.string_cursor;
  std::memcpy(f.mem.data() + off, s.data(), s.size());
  f.string_cursor += static_cast<uint32_t>((s.size() + 7u) & ~size_t{7u});
  CelValue v{};
  v.kind = CEL_STRING;
  v.payload.s.ptr = off;
  v.payload.s.len = static_cast<uint32_t>(s.size());
  return v;
}
CelValue MakeTimestamp(int64_t seconds, int32_t nanos) {
  CelValue v{};
  v.kind = CEL_TIMESTAMP;
  v.payload.ts.seconds = seconds;
  v.payload.ts.nanos = nanos;
  return v;
}
CelValue MakeDuration(int64_t seconds, int32_t nanos) {
  CelValue v{};
  v.kind = CEL_DURATION;
  v.payload.dur.seconds = seconds;
  v.payload.dur.nanos = nanos;
  return v;
}

// Builds a CEL_MAP_ARENA map in the fake memory at the fixture's map
// cursor: 16-byte ArenaMapHeader followed by the count×48-byte
// entries run (key CelValue at +0, value CelValue at +24 — the
// layout `cel_runtime.c` writes).
CelValue MakeArenaMap(
    Fixture& f, const std::vector<std::pair<CelValue, CelValue>>& entries) {
  const uint32_t header_off = f.map_cursor;
  ArenaMapHeader hdr{};
  hdr.count = static_cast<uint32_t>(entries.size());
  hdr.capacity = hdr.count;
  hdr.entries_offset = header_off + static_cast<uint32_t>(sizeof(hdr));
  std::memcpy(f.mem.data() + header_off, &hdr, sizeof(hdr));
  uint32_t off = hdr.entries_offset;
  for (const auto& [k, v] : entries) {
    f.mem.WriteCelValue(off, k);
    f.mem.WriteCelValue(off + static_cast<uint32_t>(sizeof(CelValue)), v);
    off += static_cast<uint32_t>(kCelMapEntryStride);
  }
  f.map_cursor = off;
  CelValue cv{};
  cv.kind = CEL_MAP_ARENA;
  cv.payload.arena_map.header_ptr = header_off;
  return cv;
}

CelValue MakeHostMap(
    Fixture& f,
    std::vector<std::pair<celwasm::Value, celwasm::Value>> entries) {
  CelValue cv{};
  cv.kind = CEL_MAP_HOST;
  cv.payload.ref_slot =
      f.refs.InternMap(std::make_shared<HostMap>(std::move(entries)));
  return cv;
}

// Stages `a` / `b` and runs CelMapEqImpl, returning the out CelValue.
CelValue RunEq(Fixture& f, const CelValue& a, const CelValue& b) {
  f.mem.WriteCelValue(f.a_slot, a);
  f.mem.WriteCelValue(f.b_slot, b);
  absl::Status s = CelMapEqImpl(f.out_slot, f.a_slot, f.b_slot, f.ctx);
  EXPECT_TRUE(s.ok()) << s;
  return f.mem.ReadCelValue(f.out_slot);
}

void ExpectBoolResult(const CelValue& out, bool expected) {
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(out.payload.b, expected ? 1 : 0);
}

// ─── Parameterized key-kind × direction matrix ─────────────────────

enum class KeyKind : std::uint8_t { kBool, kInt, kUint, kString };
enum class Direction : std::uint8_t { kHostVsArena, kArenaVsHost };

std::string MatrixName(
    const ::testing::TestParamInfo<std::tuple<KeyKind, Direction>>& info) {
  std::string name;
  switch (std::get<0>(info.param)) {
    case KeyKind::kBool:
      name = "BoolKey";
      break;
    case KeyKind::kInt:
      name = "IntKey";
      break;
    case KeyKind::kUint:
      name = "UintKey";
      break;
    case KeyKind::kString:
      name = "StringKey";
      break;
  }
  name += std::get<1>(info.param) == Direction::kHostVsArena ? "HostVsArena"
                                                             : "ArenaVsHost";
  return name;
}

// The i-th distinct key of the parameterized kind, in both
// representations (host-side celwasm::Value and wire CelValue).
celwasm::Value HostKey(KeyKind kind, int i) {
  switch (kind) {
    case KeyKind::kBool:
      return celwasm::Value::Bool(i != 0);
    case KeyKind::kInt:
      return celwasm::Value::Int(7 + i);
    case KeyKind::kUint:
      return celwasm::Value::Uint(42 + static_cast<uint64_t>(i));
    case KeyKind::kString:
      return celwasm::Value::String("key" + std::to_string(i));
  }
  return celwasm::Value::Null();
}
CelValue ArenaKey(Fixture& f, KeyKind kind, int i) {
  switch (kind) {
    case KeyKind::kBool:
      return MakeBool(i != 0);
    case KeyKind::kInt:
      return MakeInt(7 + i);
    case KeyKind::kUint:
      return MakeUint(42 + static_cast<uint64_t>(i));
    case KeyKind::kString:
      return MakeString(f, "key" + std::to_string(i));
  }
  return CelValue{};
}

class CrossOriginMapEqTest
    : public ::testing::TestWithParam<std::tuple<KeyKind, Direction>> {
 protected:
  // Runs `host_map == arena_map` (or the reverse, per the direction
  // param) and returns the result CelValue.
  CelValue RunDirected(Fixture& f, const CelValue& host,
                       const CelValue& arena) {
    return std::get<1>(GetParam()) == Direction::kHostVsArena
               ? RunEq(f, host, arena)
               : RunEq(f, arena, host);
  }
};

TEST_P(CrossOriginMapEqTest, EqualMapsCompareTrue) {
  Fixture f;
  const KeyKind kind = std::get<0>(GetParam());
  CelValue host = MakeHostMap(f, {{HostKey(kind, 0), celwasm::Value::Int(10)},
                                  {HostKey(kind, 1), celwasm::Value::Int(20)}});
  // Arena twin lists the entries in the OPPOSITE order — map equality
  // is set-equality (langdef §"Equality": order is irrelevant).
  CelValue arena = MakeArenaMap(f, {{ArenaKey(f, kind, 1), MakeInt(20)},
                                    {ArenaKey(f, kind, 0), MakeInt(10)}});
  ExpectBoolResult(RunDirected(f, host, arena), true);
}

TEST_P(CrossOriginMapEqTest, ValueMismatchComparesFalse) {
  Fixture f;
  const KeyKind kind = std::get<0>(GetParam());
  CelValue host = MakeHostMap(f, {{HostKey(kind, 0), celwasm::Value::Int(10)},
                                  {HostKey(kind, 1), celwasm::Value::Int(20)}});
  CelValue arena = MakeArenaMap(f, {{ArenaKey(f, kind, 0), MakeInt(10)},
                                    {ArenaKey(f, kind, 1), MakeInt(99)}});
  ExpectBoolResult(RunDirected(f, host, arena), false);
}

TEST_P(CrossOriginMapEqTest, MissingKeyComparesFalse) {
  Fixture f;
  const KeyKind kind = std::get<0>(GetParam());
  // Same size, same values, but the single key differs — the walk
  // must report a key miss, not just a value mismatch.
  CelValue host = MakeHostMap(f, {{HostKey(kind, 0), celwasm::Value::Int(10)}});
  CelValue arena = MakeArenaMap(f, {{ArenaKey(f, kind, 1), MakeInt(10)}});
  ExpectBoolResult(RunDirected(f, host, arena), false);
}

TEST_P(CrossOriginMapEqTest, SizeMismatchComparesFalse) {
  Fixture f;
  const KeyKind kind = std::get<0>(GetParam());
  CelValue host = MakeHostMap(f, {{HostKey(kind, 0), celwasm::Value::Int(10)},
                                  {HostKey(kind, 1), celwasm::Value::Int(20)}});
  CelValue arena = MakeArenaMap(f, {{ArenaKey(f, kind, 0), MakeInt(10)}});
  ExpectBoolResult(RunDirected(f, host, arena), false);
}

INSTANTIATE_TEST_SUITE_P(
    KeyKindByDirection, CrossOriginMapEqTest,
    ::testing::Combine(::testing::Values(KeyKind::kBool, KeyKind::kInt,
                                         KeyKind::kUint, KeyKind::kString),
                       ::testing::Values(Direction::kHostVsArena,
                                         Direction::kArenaVsHost)),
    MatrixName);

// ─── Focused cases ─────────────────────────────────────────────────

TEST(CelMapEqImplTest, EmptyMapsCompareEqualBothDirections) {
  Fixture f;
  CelValue host = MakeHostMap(f, {});
  CelValue arena = MakeArenaMap(f, {});
  ExpectBoolResult(RunEq(f, host, arena), true);
  ExpectBoolResult(RunEq(f, arena, host), true);
}

TEST(CelMapEqImplTest, TimestampValuesCrossOrigin) {
  // The conformance-row shape (proto{2,3}
  // set_null/map_timestamp_null_pruned): a bool-keyed proto map
  // field of Timestamp values vs the literal `{false: timestamp(1)}`.
  Fixture f;
  CelValue host =
      MakeHostMap(f, {{celwasm::Value::Bool(false),
                       celwasm::Value::Timestamp(absl::FromUnixSeconds(1))}});
  CelValue arena = MakeArenaMap(f, {{MakeBool(false), MakeTimestamp(1, 0)}});
  ExpectBoolResult(RunEq(f, host, arena), true);
  ExpectBoolResult(RunEq(f, arena, host), true);
  // Mismatched nanos → unequal.
  CelValue arena_off =
      MakeArenaMap(f, {{MakeBool(false), MakeTimestamp(1, 5)}});
  ExpectBoolResult(RunEq(f, host, arena_off), false);
}

TEST(CelMapEqImplTest, DurationValuesCrossOrigin) {
  // proto{2,3} set_null/map_duration_null_pruned shape.
  Fixture f;
  CelValue host =
      MakeHostMap(f, {{celwasm::Value::Bool(false),
                       celwasm::Value::Duration(absl::Seconds(1))}});
  CelValue arena = MakeArenaMap(f, {{MakeBool(false), MakeDuration(1, 0)}});
  ExpectBoolResult(RunEq(f, host, arena), true);
  ExpectBoolResult(RunEq(f, arena, host), true);
  CelValue arena_off = MakeArenaMap(f, {{MakeBool(false), MakeDuration(2, 0)}});
  ExpectBoolResult(RunEq(f, host, arena_off), false);
}

TEST(CelMapEqImplTest, StringValuesCrossOrigin) {
  // Value spans live in different regions: the arena literal's bytes
  // are hand-staged; the host value's bytes are encoded into the
  // FakeArenaAllocator region by EncodeBackingScalar.  Both read back
  // through the same MemoryView.
  Fixture f;
  CelValue host = MakeHostMap(
      f, {{celwasm::Value::Int(1), celwasm::Value::String("hello")}});
  CelValue arena = MakeArenaMap(f, {{MakeInt(1), MakeString(f, "hello")}});
  ExpectBoolResult(RunEq(f, host, arena), true);
  ExpectBoolResult(RunEq(f, arena, host), true);
  CelValue arena_diff = MakeArenaMap(f, {{MakeInt(1), MakeString(f, "world")}});
  ExpectBoolResult(RunEq(f, host, arena_diff), false);
}

TEST(CelMapEqImplTest, NumericCrossTypeKeyMatches) {
  // langdef §"Equality": numeric keys compare by mathematical value
  // across int/uint — `{42u: 1}` equals `{42: 1}`.  Mirrors the
  // arena kernel's polymorphic `map_keys_equal`.
  Fixture f;
  CelValue host =
      MakeHostMap(f, {{celwasm::Value::Uint(42), celwasm::Value::Int(1)}});
  CelValue arena = MakeArenaMap(f, {{MakeInt(42), MakeInt(1)}});
  ExpectBoolResult(RunEq(f, host, arena), true);
  ExpectBoolResult(RunEq(f, arena, host), true);
}

TEST(CelMapEqImplTest, HostHostStillCompares) {
  Fixture f;
  CelValue a =
      MakeHostMap(f, {{celwasm::Value::String("k"), celwasm::Value::Int(1)}});
  CelValue b =
      MakeHostMap(f, {{celwasm::Value::String("k"), celwasm::Value::Int(1)}});
  ExpectBoolResult(RunEq(f, a, b), true);
  CelValue c =
      MakeHostMap(f, {{celwasm::Value::String("k"), celwasm::Value::Int(2)}});
  ExpectBoolResult(RunEq(f, a, c), false);
}

TEST(CelMapEqImplTest, ArenaArenaCompares) {
  // The runtime dispatcher short-circuits arena+arena in its own
  // fast path, but the normalized walk admits the pair too — the
  // trampoline must not regress if dispatch routing changes.
  Fixture f;
  CelValue a = MakeArenaMap(f, {{MakeInt(1), MakeInt(10)}});
  CelValue b = MakeArenaMap(f, {{MakeInt(1), MakeInt(10)}});
  ExpectBoolResult(RunEq(f, a, b), true);
  CelValue c = MakeArenaMap(f, {{MakeInt(2), MakeInt(10)}});
  ExpectBoolResult(RunEq(f, a, c), false);
}

// ─── 3VL / poison / infrastructure surface ─────────────────────────

TEST(CelMapEqImplTest, UnknownOperandPropagates) {
  Fixture f;
  CelValue arena = MakeArenaMap(f, {{MakeInt(1), MakeInt(10)}});
  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 99;
  CelValue out = RunEq(f, unk, arena);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(out.payload.unk, 99u);
}

TEST(CelMapEqImplTest, ErrorOperandPropagates) {
  Fixture f;
  CelValue host =
      MakeHostMap(f, {{celwasm::Value::Int(1), celwasm::Value::Int(10)}});
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = CEL_ERR_NO_SUCH_KEY;
  CelValue out = RunEq(f, host, err);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY));
}

TEST(CelMapEqImplTest, NonMapOperandPoisonsTypeMismatch) {
  Fixture f;
  CelValue arena = MakeArenaMap(f, {{MakeInt(1), MakeInt(10)}});
  CelValue out = RunEq(f, MakeInt(7), arena);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST(CelMapEqImplTest, MissingRefSlotReturnsNonOkStatus) {
  Fixture f;
  CelValue bogus{};
  bogus.kind = CEL_MAP_HOST;
  bogus.payload.ref_slot = 9999;
  CelValue arena = MakeArenaMap(f, {{MakeInt(1), MakeInt(10)}});
  f.mem.WriteCelValue(f.a_slot, bogus);
  f.mem.WriteCelValue(f.b_slot, arena);
  EXPECT_FALSE(CelMapEqImpl(f.out_slot, f.a_slot, f.b_slot, f.ctx).ok());
}

}  // namespace
}  // namespace celwasm
