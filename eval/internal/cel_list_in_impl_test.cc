// Layer-2 trampoline body for `cel_host.cel_list_in`.  Drives
// CelListInImpl through fake MemoryView / ExternrefTable /
// ArenaAllocator impls so the test doesn't depend on wasmtime.
//
// `x in <host list>` scans the backing element-by-element without
// materialising it, comparing each `celwasm::Value` against the
// already-decoded wire query.  The load-bearing surface is that the
// scan answers the same as its arena twin for EVERY element kind:
//
//   - scalar elements (bool / int / uint / double / string / bytes /
//     null / duration / timestamp), including the cross-numeric
//     ladder (`1 in [1u]`, langdef §"Equality") and boundary values
//   - AGGREGATE elements — a list or map needle scanned against a
//     host list of lists / maps.  These compared UNEQUAL
//     unconditionally until the scan learned to encode the element
//     into a wire handle and recurse, so `[1, 2] in xss` was a
//     permanent `false` regardless of contents
//   - MESSAGE elements — `msg in x.repeated_msgs`, which the
//     equality walk had supported for a while but the `in` scan
//     never did
//   - 3VL on the operand pair, non-list operand, missing ref_slot

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"
#include "shared/type.h"
#include "testdata/host_fixture_proto3.pb.h"

namespace celwasm {
namespace {

using ::celwasm::testdata::HostMsg3;
using FakeMemory = test::FakeMemoryView;
using FakeRefs = test::FakeExternrefTable;

constexpr int64_t kInt64Min = -9223372036854775807LL - 1;
constexpr int64_t kInt64Max = 9223372036854775807LL;

// ─── Fixture ───────────────────────────────────────────────────────
struct Fixture {
  FakeMemory mem;
  FakeRefs refs;
  test::FakeArenaAllocator arena{&mem, /*base_offset=*/8192u,
                                 /*capacity=*/8192u};
  CelHostBindings bindings{};
  TrampolineContext ctx{bindings, mem, refs, arena};

  uint32_t value_slot = 24;
  uint32_t list_slot = 48;
  uint32_t out_slot = 72;

  // Staging cursor for hand-built arena structures, kept below the
  // FakeArenaAllocator's base so trampoline allocations can't
  // collide with them.
  uint32_t string_cursor = 256;
  uint32_t staging_cursor = 1024;
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
CelValue MakeDouble(double d) {
  CelValue v{};
  v.kind = CEL_DOUBLE;
  v.payload.d = d;
  return v;
}
CelValue MakeNull() {
  CelValue v{};
  v.kind = CEL_NULL;
  return v;
}
CelValue MakeDuration(int64_t seconds, int32_t nanos) {
  CelValue v{};
  v.kind = CEL_DURATION;
  v.payload.dur.seconds = seconds;
  v.payload.dur.nanos = nanos;
  return v;
}
CelValue MakeTimestamp(int64_t seconds, int32_t nanos) {
  CelValue v{};
  v.kind = CEL_TIMESTAMP;
  v.payload.ts.seconds = seconds;
  v.payload.ts.nanos = nanos;
  return v;
}
// String / bytes payload bytes are staged into the fake memory so
// the span read path has real data behind it.
CelValue MakeSpanValue(Fixture& f, uint32_t kind, absl::string_view s) {
  const uint32_t off = f.string_cursor;
  if (!s.empty()) std::memcpy(f.mem.data() + off, s.data(), s.size());
  f.string_cursor += static_cast<uint32_t>((s.size() + 7u) & ~size_t{7u});
  CelValue v{};
  v.kind = kind;
  v.payload.s.ptr = off;
  v.payload.s.len = static_cast<uint32_t>(s.size());
  return v;
}
CelValue MakeString(Fixture& f, absl::string_view s) {
  return MakeSpanValue(f, CEL_STRING, s);
}
CelValue MakeBytes(Fixture& f, absl::string_view s) {
  return MakeSpanValue(f, CEL_BYTES, s);
}

// Builds a CEL_LIST_ARENA list in the fake memory: 16-byte
// ArenaListHeader followed by the count×24-byte elements run (the
// layout `cel_runtime.c` writes).
CelValue MakeArenaList(Fixture& f, const std::vector<CelValue>& elements) {
  const uint32_t header_off = f.staging_cursor;
  ArenaListHeader hdr{};
  hdr.count = static_cast<uint32_t>(elements.size());
  hdr.capacity = hdr.count;
  hdr.elements_offset = header_off + static_cast<uint32_t>(sizeof(hdr));
  std::memcpy(f.mem.data() + header_off, &hdr, sizeof(hdr));
  uint32_t off = hdr.elements_offset;
  for (const CelValue& e : elements) {
    f.mem.WriteCelValue(off, e);
    off += static_cast<uint32_t>(kCelListEntryStride);
  }
  f.staging_cursor = off;
  CelValue cv{};
  cv.kind = CEL_LIST_ARENA;
  cv.payload.arena_list.header_ptr = header_off;
  return cv;
}

CelValue MakeArenaMap(
    Fixture& f, const std::vector<std::pair<CelValue, CelValue>>& entries) {
  const uint32_t header_off = f.staging_cursor;
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
  f.staging_cursor = off;
  CelValue cv{};
  cv.kind = CEL_MAP_ARENA;
  cv.payload.arena_map.header_ptr = header_off;
  return cv;
}

CelValue MakeHostList(Fixture& f, std::vector<celwasm::Value> elements) {
  CelValue cv{};
  cv.kind = CEL_LIST_HOST;
  cv.payload.ref_slot =
      f.refs.InternList(std::make_shared<HostList>(std::move(elements)));
  return cv;
}

celwasm::Value HostIntList(const std::vector<int64_t>& xs) {
  std::vector<celwasm::Value> elems;
  elems.reserve(xs.size());
  for (int64_t x : xs) {
    elems.push_back(celwasm::Value::Int(x));
  }
  return celwasm::Value::List(std::move(elems));
}

// Stages the operands and runs CelListInImpl, returning `out`.
CelValue RunIn(Fixture& f, const CelValue& needle, const CelValue& haystack) {
  f.mem.WriteCelValue(f.value_slot, needle);
  f.mem.WriteCelValue(f.list_slot, haystack);
  absl::Status s = CelListInImpl(f.out_slot, f.value_slot, f.list_slot, f.ctx);
  EXPECT_TRUE(s.ok()) << s;
  return f.mem.ReadCelValue(f.out_slot);
}

void ExpectBoolResult(const CelValue& out, bool expected) {
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(out.payload.b, expected ? 1 : 0);
}

// ─── Scalar elements (the pre-existing fast paths) ─────────────────

TEST(CelListInImplTest, ScalarElementKindsMatchAndMiss) {
  Fixture f;
  ExpectBoolResult(
      RunIn(f, MakeBool(true), MakeHostList(f, {celwasm::Value::Bool(true)})),
      true);
  ExpectBoolResult(
      RunIn(f, MakeBool(true), MakeHostList(f, {celwasm::Value::Bool(false)})),
      false);
  ExpectBoolResult(
      RunIn(f, MakeInt(2),
            MakeHostList(f, {celwasm::Value::Int(1), celwasm::Value::Int(2)})),
      true);
  ExpectBoolResult(
      RunIn(f, MakeUint(2), MakeHostList(f, {celwasm::Value::Uint(2)})), true);
  ExpectBoolResult(
      RunIn(f, MakeDouble(1.5), MakeHostList(f, {celwasm::Value::Double(1.5)})),
      true);
  ExpectBoolResult(
      RunIn(f, MakeNull(), MakeHostList(f, {celwasm::Value::Null()})), true);
  ExpectBoolResult(RunIn(f, MakeString(f, "b"),
                         MakeHostList(f, {celwasm::Value::String("a"),
                                          celwasm::Value::String("b")})),
                   true);
  // Embedded NUL: both sides must compare by LENGTH + bytes, not as
  // C strings.
  const std::string nul_bytes("\x00\x01", 2);
  ExpectBoolResult(RunIn(f, MakeBytes(f, absl::string_view(nul_bytes)),
                         MakeHostList(f, {celwasm::Value::Bytes(nul_bytes)})),
                   true);
  ExpectBoolResult(
      RunIn(
          f, MakeBytes(f, absl::string_view(nul_bytes)),
          MakeHostList(f, {celwasm::Value::Bytes(std::string("\x00\x02", 2))})),
      false);
  ExpectBoolResult(
      RunIn(f, MakeDuration(3, 4),
            MakeHostList(f, {celwasm::Value::Duration(absl::Seconds(3) +
                                                      absl::Nanoseconds(4))})),
      true);
  ExpectBoolResult(
      RunIn(
          f, MakeTimestamp(5, 6),
          MakeHostList(f, {celwasm::Value::Timestamp(absl::FromUnixSeconds(5) +
                                                     absl::Nanoseconds(6))})),
      true);
}

TEST(CelListInImplTest, CrossNumericLadderMatchesByMathematicalValue) {
  // langdef §"Equality": int / uint / double compare across the type
  // ladder by mathematical value.
  Fixture f;
  ExpectBoolResult(
      RunIn(f, MakeInt(1), MakeHostList(f, {celwasm::Value::Uint(1)})), true);
  ExpectBoolResult(
      RunIn(f, MakeDouble(1.0), MakeHostList(f, {celwasm::Value::Int(1)})),
      true);
  ExpectBoolResult(
      RunIn(f, MakeInt(-1), MakeHostList(f, {celwasm::Value::Uint(1)})), false);
}

TEST(CelListInImplTest, BoundaryIntegersMatchExactly) {
  Fixture f;
  ExpectBoolResult(RunIn(f, MakeInt(kInt64Min),
                         MakeHostList(f, {celwasm::Value::Int(kInt64Min)})),
                   true);
  ExpectBoolResult(RunIn(f, MakeInt(kInt64Max),
                         MakeHostList(f, {celwasm::Value::Int(kInt64Min)})),
                   false);
}

TEST(CelListInImplTest, EmptyHostListNeverMatches) {
  Fixture f;
  ExpectBoolResult(RunIn(f, MakeInt(1), MakeHostList(f, {})), false);
}

// ─── Aggregate elements ────────────────────────────────────────────

TEST(CelListInImplTest, ArenaListNeedleMatchesHostListElement) {
  Fixture f;
  CelValue haystack = MakeHostList(f, {HostIntList({1, 2}), HostIntList({3})});
  ExpectBoolResult(
      RunIn(f, MakeArenaList(f, {MakeInt(1), MakeInt(2)}), haystack), true);
  ExpectBoolResult(
      RunIn(f, MakeArenaList(f, {MakeInt(1), MakeInt(3)}), haystack), false);
  ExpectBoolResult(
      RunIn(f, MakeArenaList(f, {MakeInt(1), MakeInt(2), MakeInt(3)}),
            haystack),
      false);
}

TEST(CelListInImplTest, HostListNeedleMatchesHostListElement) {
  Fixture f;
  CelValue haystack = MakeHostList(f, {HostIntList({1, 2})});
  ExpectBoolResult(
      RunIn(f,
            MakeHostList(f, {celwasm::Value::Int(1), celwasm::Value::Int(2)}),
            haystack),
      true);
  ExpectBoolResult(
      RunIn(f, MakeHostList(f, {celwasm::Value::Int(9)}), haystack), false);
}

TEST(CelListInImplTest, EmptyNestedNeedleMatchesEmptyNestedElement) {
  Fixture f;
  ExpectBoolResult(
      RunIn(f, MakeArenaList(f, {}), MakeHostList(f, {HostIntList({})})), true);
  ExpectBoolResult(
      RunIn(f, MakeArenaList(f, {}), MakeHostList(f, {HostIntList({1})})),
      false);
}

TEST(CelListInImplTest, BoundaryElementInsideANestedNeedle) {
  Fixture f;
  CelValue haystack = MakeHostList(f, {HostIntList({kInt64Min})});
  ExpectBoolResult(RunIn(f, MakeArenaList(f, {MakeInt(kInt64Min)}), haystack),
                   true);
  ExpectBoolResult(
      RunIn(f, MakeArenaList(f, {MakeInt(kInt64Min + 1)}), haystack), false);
}

TEST(CelListInImplTest, MapNeedleMatchesHostMapElement) {
  Fixture f;
  auto host_entry = [](int64_t v) {
    return celwasm::Value::Map(
        {{celwasm::Value::String("a"), celwasm::Value::Int(v)}});
  };
  CelValue haystack = MakeHostList(f, {host_entry(1)});
  ExpectBoolResult(
      RunIn(f, MakeArenaMap(f, {{MakeString(f, "a"), MakeInt(1)}}), haystack),
      true);
  ExpectBoolResult(
      RunIn(f, MakeArenaMap(f, {{MakeString(f, "a"), MakeInt(2)}}), haystack),
      false);
  ExpectBoolResult(
      RunIn(f, MakeArenaMap(f, {{MakeString(f, "b"), MakeInt(1)}}), haystack),
      false);
}

TEST(CelListInImplTest, MessageNeedleMatchesHostMessageElement) {
  // `msg in x.repeated_msgs` — the equality walk grew a message arm
  // long before the `in` scan did.
  Fixture f;
  HostMsg3 stored;
  stored.set_i64(1);
  HostMsg3 same;
  same.set_i64(1);
  HostMsg3 different;
  different.set_i64(99);
  CelValue haystack = MakeHostList(
      f,
      {celwasm::Value::HostMessage(std::make_shared<ProtoBacking>(&stored))});

  CelValue needle_same{};
  needle_same.kind = CEL_MESSAGE;
  needle_same.payload.msg_slot =
      f.refs.Intern(std::make_shared<ProtoBacking>(&same));
  ExpectBoolResult(RunIn(f, needle_same, haystack), true);

  CelValue needle_diff{};
  needle_diff.kind = CEL_MESSAGE;
  needle_diff.payload.msg_slot =
      f.refs.Intern(std::make_shared<ProtoBacking>(&different));
  ExpectBoolResult(RunIn(f, needle_diff, haystack), false);
}

TEST(CelListInImplTest, AggregateNeedleAgainstScalarElementsIsFalseNotError) {
  // Cross-kind is langdef `false` ("comparing incompatible types is
  // not an error"), and the reverse direction too.
  Fixture f;
  ExpectBoolResult(RunIn(f, MakeArenaList(f, {MakeInt(1)}),
                         MakeHostList(f, {celwasm::Value::Int(1)})),
                   false);
  ExpectBoolResult(RunIn(f, MakeInt(1), MakeHostList(f, {HostIntList({1})})),
                   false);
}

// ─── Envelope ──────────────────────────────────────────────────────

TEST(CelListInImplTest, ThreeValuedLogicAbsorbsOnEitherOperand) {
  Fixture f;
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = CEL_ERR_DIVIDE_BY_ZERO;
  CelValue out = RunIn(f, err, MakeHostList(f, {celwasm::Value::Int(1)}));
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));

  CelValue unknown{};
  unknown.kind = CEL_UNKNOWN;
  out = RunIn(f, MakeInt(1), unknown);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_UNKNOWN));
}

TEST(CelListInImplTest, NonHostListOperandPoisonsTypeMismatch) {
  // Codegen only routes the kHost arm here; anything else is drift.
  Fixture f;
  CelValue out = RunIn(f, MakeInt(1), MakeInt(7));
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST(CelListInImplTest, MissingListRefSlotReturnsNonOkStatus) {
  Fixture f;
  CelValue bogus{};
  bogus.kind = CEL_LIST_HOST;
  bogus.payload.ref_slot = 9999;
  f.mem.WriteCelValue(f.value_slot, MakeInt(1));
  f.mem.WriteCelValue(f.list_slot, bogus);
  EXPECT_FALSE(
      CelListInImpl(f.out_slot, f.value_slot, f.list_slot, f.ctx).ok());
}

}  // namespace
}  // namespace celwasm
