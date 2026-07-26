// Layer-2 trampoline body for `cel_host.cel_list_eq`.  Drives
// CelListEqImpl through fake MemoryView / ExternrefTable /
// ArenaAllocator impls so the test doesn't depend on wasmtime.
//
// The load-bearing surface is CROSS-ORIGIN equality of lists of
// MESSAGES: a CEL_LIST_HOST operand (proto repeated field /
// repeated-extension read, ProtoList-backed) compared against a
// CEL_LIST_ARENA operand (list literal in linear memory whose
// elements are CEL_MESSAGE CelValues from `cel_make_message`).  This
// is the shape behind the conformance rows
// `proto2/extensions_get/{package,message}_scoped_repeated_test_all_types`.
//
// Coverage matrix:
//   - message-element lists × both directions (host==arena,
//     arena==host): equal, element-value mismatch, length mismatch,
//     empty×nonempty, element order (lists are sequence-equality,
//     not set-equality — langdef §"Equality")
//   - both-empty lists cross-origin
//   - scalar (int) elements cross-origin — the pre-existing path
//     must keep working
//   - message vs scalar element kinds compare unequal
//   - non-proto custom message backings (message() == nullptr) are
//     not comparable → unequal, mirroring CelMessageEqImpl's
//     proto-vs-non-proto scope boundary
//   - host+host and arena+arena message lists compare through the
//     same normalized walk
//   - 3VL on the operand pair (unknown / error propagate)
//   - non-list operand poisons TYPE_MISMATCH
//   - missing list ref_slot / missing element msg_slot surface a
//     non-OK Status (infrastructure failure — codegen drift, not
//     user-visible)

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
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

  // Staging cursor for hand-built arena lists, kept below the
  // FakeArenaAllocator's base so EncodeBackingScalar allocations
  // can't collide with them.
  uint32_t list_cursor = 1024;

  // Proto element fixtures, mirroring the conformance rows' literal
  // `[TestAllTypes{single_int64: 1}, TestAllTypes{single_bool: true}]`
  // shape.  Owned here so the non-owning ProtoBacking elements stay
  // valid for the duration of each test.
  HostMsg3 msg_i64;       // i64 = 1
  HostMsg3 msg_b;         // b = true
  HostMsg3 msg_i64_diff;  // i64 = 99 (the value-mismatch twin)

  Fixture() {
    msg_i64.set_i64(1);
    msg_b.set_b(true);
    msg_i64_diff.set_i64(99);
  }
};

// ─── CelValue makers ───────────────────────────────────────────────
CelValue MakeInt(int64_t i) {
  CelValue v{};
  v.kind = CEL_INT;
  v.payload.i = i;
  return v;
}

// A CEL_MESSAGE element as codegen produces it inside a list
// literal: the message backing interned into the externref table,
// the wire CelValue carrying the slot.
CelValue MakeArenaMessageElement(Fixture& f,
                                 const google::protobuf::Message& m) {
  CelValue v{};
  v.kind = CEL_MESSAGE;
  v.payload.msg_slot = f.refs.Intern(std::make_shared<ProtoBacking>(&m));
  return v;
}

// Builds a CEL_LIST_ARENA list in the fake memory at the fixture's
// list cursor: 16-byte ArenaListHeader followed by the count×24-byte
// elements run (the layout `cel_runtime.c` writes).
CelValue MakeArenaList(Fixture& f, const std::vector<CelValue>& elements) {
  const uint32_t header_off = f.list_cursor;
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
  f.list_cursor = off;
  CelValue cv{};
  cv.kind = CEL_LIST_ARENA;
  cv.payload.arena_list.header_ptr = header_off;
  return cv;
}

CelValue MakeHostList(Fixture& f, std::vector<celwasm::Value> elements) {
  CelValue cv{};
  cv.kind = CEL_LIST_HOST;
  cv.payload.ref_slot =
      f.refs.InternList(std::make_shared<HostList>(std::move(elements)));
  return cv;
}

// Host-side message element, as ProtoList::At returns repeated
// message elements (Value::HostMessage over a non-owning
// ProtoBacking).
celwasm::Value HostMessageElement(const google::protobuf::Message& m) {
  return celwasm::Value::HostMessage(std::make_shared<ProtoBacking>(&m));
}

// Stages `a` / `b` and runs CelListEqImpl, returning the out CelValue.
CelValue RunEq(Fixture& f, const CelValue& a, const CelValue& b) {
  f.mem.WriteCelValue(f.a_slot, a);
  f.mem.WriteCelValue(f.b_slot, b);
  absl::Status s = CelListEqImpl(f.out_slot, f.a_slot, f.b_slot, f.ctx);
  EXPECT_TRUE(s.ok()) << s;
  return f.mem.ReadCelValue(f.out_slot);
}

void ExpectBoolResult(const CelValue& out, bool expected) {
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_BOOL));
  EXPECT_EQ(out.payload.b, expected ? 1 : 0);
}

// ─── Parameterized direction matrix (message elements) ─────────────

enum class Direction : std::uint8_t { kHostVsArena, kArenaVsHost };

std::string DirectionName(const ::testing::TestParamInfo<Direction>& info) {
  return info.param == Direction::kHostVsArena ? "HostVsArena" : "ArenaVsHost";
}

class CrossOriginMessageListEqTest
    : public ::testing::TestWithParam<Direction> {
 protected:
  // Runs `host_list == arena_list` (or the reverse, per the direction
  // param) and returns the result CelValue.
  CelValue RunDirected(Fixture& f, const CelValue& host,
                       const CelValue& arena) {
    return GetParam() == Direction::kHostVsArena ? RunEq(f, host, arena)
                                                 : RunEq(f, arena, host);
  }
};

TEST_P(CrossOriginMessageListEqTest, EqualMessageListsCompareTrue) {
  // The conformance-row shape: a ProtoList-backed repeated-message
  // read vs the literal `[Msg{i64: 1}, Msg{b: true}]`.  Element
  // messages are distinct C++ objects with equal field values —
  // equality must go through MessageDifferencer, not pointer
  // identity.
  Fixture f;
  HostMsg3 lit_i64;
  lit_i64.set_i64(1);
  HostMsg3 lit_b;
  lit_b.set_b(true);
  CelValue host = MakeHostList(
      f, {HostMessageElement(f.msg_i64), HostMessageElement(f.msg_b)});
  CelValue arena = MakeArenaList(f, {MakeArenaMessageElement(f, lit_i64),
                                     MakeArenaMessageElement(f, lit_b)});
  ExpectBoolResult(RunDirected(f, host, arena), true);
}

TEST_P(CrossOriginMessageListEqTest, ElementValueMismatchComparesFalse) {
  Fixture f;
  CelValue host = MakeHostList(
      f, {HostMessageElement(f.msg_i64), HostMessageElement(f.msg_b)});
  CelValue arena =
      MakeArenaList(f, {MakeArenaMessageElement(f, f.msg_i64),
                        MakeArenaMessageElement(f, f.msg_i64_diff)});
  ExpectBoolResult(RunDirected(f, host, arena), false);
}

TEST_P(CrossOriginMessageListEqTest, LengthMismatchComparesFalse) {
  Fixture f;
  CelValue host = MakeHostList(
      f, {HostMessageElement(f.msg_i64), HostMessageElement(f.msg_b)});
  CelValue arena = MakeArenaList(f, {MakeArenaMessageElement(f, f.msg_i64)});
  ExpectBoolResult(RunDirected(f, host, arena), false);
}

TEST_P(CrossOriginMessageListEqTest, EmptyVsNonEmptyComparesFalse) {
  Fixture f;
  CelValue host = MakeHostList(f, {});
  CelValue arena = MakeArenaList(f, {MakeArenaMessageElement(f, f.msg_i64)});
  ExpectBoolResult(RunDirected(f, host, arena), false);
}

TEST_P(CrossOriginMessageListEqTest, ElementOrderMatters) {
  // List equality is element-wise in sequence order (langdef
  // §"Equality") — same elements reversed are UNEQUAL, unlike maps.
  Fixture f;
  CelValue host = MakeHostList(
      f, {HostMessageElement(f.msg_i64), HostMessageElement(f.msg_b)});
  CelValue arena = MakeArenaList(f, {MakeArenaMessageElement(f, f.msg_b),
                                     MakeArenaMessageElement(f, f.msg_i64)});
  ExpectBoolResult(RunDirected(f, host, arena), false);
}

INSTANTIATE_TEST_SUITE_P(BothDirections, CrossOriginMessageListEqTest,
                         ::testing::Values(Direction::kHostVsArena,
                                           Direction::kArenaVsHost),
                         DirectionName);

// ─── Focused cases ─────────────────────────────────────────────────

TEST(CelListEqImplTest, EmptyListsCompareEqualBothDirections) {
  Fixture f;
  CelValue host = MakeHostList(f, {});
  CelValue arena = MakeArenaList(f, {});
  ExpectBoolResult(RunEq(f, host, arena), true);
  ExpectBoolResult(RunEq(f, arena, host), true);
}

TEST(CelListEqImplTest, ScalarElementsCrossOriginSanity) {
  // The pre-existing scalar walk must keep working alongside the
  // message arm — int elements both directions, equal and unequal.
  Fixture f;
  CelValue host =
      MakeHostList(f, {celwasm::Value::Int(1), celwasm::Value::Int(2)});
  CelValue arena = MakeArenaList(f, {MakeInt(1), MakeInt(2)});
  ExpectBoolResult(RunEq(f, host, arena), true);
  ExpectBoolResult(RunEq(f, arena, host), true);
  CelValue arena_diff = MakeArenaList(f, {MakeInt(1), MakeInt(99)});
  ExpectBoolResult(RunEq(f, host, arena_diff), false);
  ExpectBoolResult(RunEq(f, arena_diff, host), false);
}

TEST(CelListEqImplTest, MessageVsScalarElementComparesFalse) {
  // Cross-kind element pair (message in one list, int in the other)
  // is unequal, not an error — langdef §"Equality" cross-kind rule.
  Fixture f;
  CelValue host = MakeHostList(f, {HostMessageElement(f.msg_i64)});
  CelValue arena = MakeArenaList(f, {MakeInt(1)});
  ExpectBoolResult(RunEq(f, host, arena), false);
  ExpectBoolResult(RunEq(f, arena, host), false);
}

TEST(CelListEqImplTest, HostHostMessageListsCompare) {
  Fixture f;
  HostMsg3 twin_i64;
  twin_i64.set_i64(1);
  CelValue a = MakeHostList(f, {HostMessageElement(f.msg_i64)});
  CelValue b = MakeHostList(f, {HostMessageElement(twin_i64)});
  ExpectBoolResult(RunEq(f, a, b), true);
  CelValue c = MakeHostList(f, {HostMessageElement(f.msg_i64_diff)});
  ExpectBoolResult(RunEq(f, a, c), false);
}

TEST(CelListEqImplTest, ArenaArenaMessageListsCompare) {
  // The runtime dispatcher's arena+arena fast path routes message
  // element pairs to `cel_host.cel_message_eq` directly (see
  // runtime/cel_runtime.c `deep_values_equal`), so production
  // arena+arena lists normally don't reach this trampoline — but
  // the normalized walk admits the pair too, and must produce the
  // same verdicts if dispatch routing ever changes.
  Fixture f;
  HostMsg3 twin_i64;
  twin_i64.set_i64(1);
  CelValue a = MakeArenaList(f, {MakeArenaMessageElement(f, f.msg_i64)});
  CelValue b = MakeArenaList(f, {MakeArenaMessageElement(f, twin_i64)});
  ExpectBoolResult(RunEq(f, a, b), true);
  CelValue c = MakeArenaList(f, {MakeArenaMessageElement(f, f.msg_i64_diff)});
  ExpectBoolResult(RunEq(f, a, c), false);
}

// Minimal non-proto backing: message() inherits the nullptr default,
// so the element is not comparable via MessageDifferencer.
class NonProtoBacking final : public HostMessageBacking {
 public:
  absl::StatusOr<celwasm::Value> ReadField(
      int /*field_number*/, absl::string_view /*field_name*/,
      const celwasm::CelType& /*expected_type*/) const override {
    return celwasm::Value::Null();
  }
  bool HasField(int /*field_number*/,
                absl::string_view /*field_name*/) const override {
    return false;
  }
};

TEST(CelListEqImplTest, NonProtoMessageBackingComparesFalse) {
  // Mirrors CelMessageEqImpl's scope boundary: a message element
  // without an underlying proto (custom backing) cannot equal a
  // proto-backed element.  Within list equality the uncomparable
  // pair is UNEQUAL (matching the established nested-aggregate
  // contract), not an eval error.
  Fixture f;
  CelValue host = MakeHostList(
      f, {celwasm::Value::HostMessage(std::make_shared<NonProtoBacking>())});
  CelValue arena = MakeArenaList(f, {MakeArenaMessageElement(f, f.msg_i64)});
  ExpectBoolResult(RunEq(f, host, arena), false);
  ExpectBoolResult(RunEq(f, arena, host), false);
}

// ─── 3VL / poison / infrastructure surface ─────────────────────────

TEST(CelListEqImplTest, UnknownOperandPropagates) {
  Fixture f;
  CelValue arena = MakeArenaList(f, {MakeInt(1)});
  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 99;
  CelValue out = RunEq(f, unk, arena);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(out.payload.unk, 99u);
}

TEST(CelListEqImplTest, ErrorOperandPropagates) {
  Fixture f;
  CelValue host = MakeHostList(f, {celwasm::Value::Int(1)});
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = CEL_ERR_NO_SUCH_KEY;
  CelValue out = RunEq(f, host, err);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY));
}

TEST(CelListEqImplTest, NonListOperandPoisonsTypeMismatch) {
  Fixture f;
  CelValue arena = MakeArenaList(f, {MakeInt(1)});
  CelValue out = RunEq(f, MakeInt(7), arena);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST(CelListEqImplTest, MissingListRefSlotReturnsNonOkStatus) {
  Fixture f;
  CelValue bogus{};
  bogus.kind = CEL_LIST_HOST;
  bogus.payload.ref_slot = 9999;
  CelValue arena = MakeArenaList(f, {MakeInt(1)});
  f.mem.WriteCelValue(f.a_slot, bogus);
  f.mem.WriteCelValue(f.b_slot, arena);
  EXPECT_FALSE(CelListEqImpl(f.out_slot, f.a_slot, f.b_slot, f.ctx).ok());
}

// ─── Nested-aggregate elements ─────────────────────────────────────
//
// The element walk must recurse, ORIGIN-AGNOSTICALLY, into list and
// map elements: `WireValueEq` dispatches on wire kind and re-enters
// `ListsEqual` / `NormalizedMapEq`.  Before that, aggregate elements
// on the host side encoded to a `CEL_ERROR{TYPE_MISMATCH}`
// placeholder and two placeholders compared UNEQUAL — so a host list
// of lists was never equal to anything, itself included.

// `[[1, 2]]` with the inner list host-backed.
CelValue MakeHostListOfHostLists(
    Fixture& f, const std::vector<std::vector<int64_t>>& rows) {
  std::vector<celwasm::Value> outer;
  outer.reserve(rows.size());
  for (const std::vector<int64_t>& row : rows) {
    std::vector<celwasm::Value> inner;
    inner.reserve(row.size());
    for (int64_t i : row) {
      inner.push_back(celwasm::Value::Int(i));
    }
    outer.push_back(celwasm::Value::List(std::move(inner)));
  }
  return MakeHostList(f, std::move(outer));
}

// `[[1, 2]]` built entirely in linear memory.
CelValue MakeArenaListOfArenaLists(
    Fixture& f, const std::vector<std::vector<int64_t>>& rows) {
  std::vector<CelValue> outer;
  outer.reserve(rows.size());
  for (const std::vector<int64_t>& row : rows) {
    std::vector<CelValue> inner;
    inner.reserve(row.size());
    for (int64_t i : row) {
      inner.push_back(MakeInt(i));
    }
    outer.push_back(MakeArenaList(f, inner));
  }
  return MakeArenaList(f, outer);
}

TEST(CelListEqImplNestedTest, HostVsArenaNestedListsCompareEqual) {
  Fixture f;
  ExpectBoolResult(RunEq(f, MakeHostListOfHostLists(f, {{1, 2}}),
                         MakeArenaListOfArenaLists(f, {{1, 2}})),
                   true);
}

TEST(CelListEqImplNestedTest, ArenaVsHostNestedListsCompareEqual) {
  Fixture f;
  ExpectBoolResult(RunEq(f, MakeArenaListOfArenaLists(f, {{1, 2}}),
                         MakeHostListOfHostLists(f, {{1, 2}})),
                   true);
}

TEST(CelListEqImplNestedTest, HostVsHostNestedListsCompareEqual) {
  Fixture f;
  ExpectBoolResult(RunEq(f, MakeHostListOfHostLists(f, {{1, 2}}),
                         MakeHostListOfHostLists(f, {{1, 2}})),
                   true);
}

TEST(CelListEqImplNestedTest, ReflexivityHoldsForAHostNestedList) {
  // `xss == xss` — the same wire operand on both sides.  Returning
  // `false` here was the reflexivity violation that made this a P0.
  Fixture f;
  CelValue xss = MakeHostListOfHostLists(f, {{1, 2}});
  ExpectBoolResult(RunEq(f, xss, xss), true);
}

TEST(CelListEqImplNestedTest, DifferingNestedElementComparesFalse) {
  Fixture f;
  ExpectBoolResult(RunEq(f, MakeHostListOfHostLists(f, {{1, 2}}),
                         MakeArenaListOfArenaLists(f, {{1, 3}})),
                   false);
  Fixture g;
  ExpectBoolResult(RunEq(g, MakeHostListOfHostLists(g, {{1, 2}}),
                         MakeHostListOfHostLists(g, {{1, 3}})),
                   false);
}

TEST(CelListEqImplNestedTest, DifferingNestedLengthComparesFalse) {
  Fixture f;
  ExpectBoolResult(RunEq(f, MakeHostListOfHostLists(f, {{1, 2}}),
                         MakeHostListOfHostLists(f, {{1, 2, 3}})),
                   false);
}

TEST(CelListEqImplNestedTest, EmptyNestedElementsCompareEqual) {
  Fixture f;
  ExpectBoolResult(RunEq(f, MakeHostListOfHostLists(f, {{}}),
                         MakeArenaListOfArenaLists(f, {{}})),
                   true);
  // An empty OUTER list is equal cross-origin too.
  ExpectBoolResult(RunEq(f, MakeHostListOfHostLists(f, {}),
                         MakeArenaListOfArenaLists(f, {})),
                   true);
  // Empty inner vs non-empty inner is a length mismatch, not equal.
  ExpectBoolResult(RunEq(f, MakeHostListOfHostLists(f, {{}}),
                         MakeArenaListOfArenaLists(f, {{1}})),
                   false);
}

TEST(CelListEqImplNestedTest, Int64MinNestedElementRoundTrips) {
  // Boundary element: the encode path must carry INT64_MIN exactly,
  // not through a lossy double.
  constexpr int64_t kMin = -9223372036854775807LL - 1;
  Fixture f;
  ExpectBoolResult(RunEq(f, MakeHostListOfHostLists(f, {{kMin}}),
                         MakeArenaListOfArenaLists(f, {{kMin}})),
                   true);
  ExpectBoolResult(RunEq(f, MakeHostListOfHostLists(f, {{kMin}}),
                         MakeArenaListOfArenaLists(f, {{kMin + 1}})),
                   false);
}

TEST(CelListEqImplNestedTest, NestedMapElementsCompareByValue) {
  Fixture f;
  auto host_row = [] {
    return celwasm::Value::Map(
        {{celwasm::Value::String("a"), celwasm::Value::Int(1)}});
  };
  CelValue host_a = MakeHostList(f, {host_row()});
  CelValue host_b = MakeHostList(f, {host_row()});
  ExpectBoolResult(RunEq(f, host_a, host_b), true);
  CelValue host_diff = MakeHostList(
      f, {celwasm::Value::Map(
             {{celwasm::Value::String("a"), celwasm::Value::Int(2)}})});
  ExpectBoolResult(RunEq(f, host_a, host_diff), false);
}

TEST(CelListEqImplNestedTest, AggregateVsScalarElementComparesFalse) {
  // Cross-kind is langdef `false`, not an error.
  Fixture f;
  ExpectBoolResult(RunEq(f, MakeHostListOfHostLists(f, {{1}}),
                         MakeArenaList(f, {MakeInt(1)})),
                   false);
}

TEST(CelListEqImplTest, MissingMessageSlotInArenaElementReturnsNonOkStatus) {
  // A CEL_MESSAGE wire value must reference an interned msg_slot;
  // a dangling slot is codegen/interner drift, surfaced as a non-OK
  // Status rather than a silent `false`.
  Fixture f;
  CelValue dangling{};
  dangling.kind = CEL_MESSAGE;
  dangling.payload.msg_slot = 9999;
  CelValue arena = MakeArenaList(f, {dangling});
  CelValue host = MakeHostList(f, {HostMessageElement(f.msg_i64)});
  f.mem.WriteCelValue(f.a_slot, arena);
  f.mem.WriteCelValue(f.b_slot, host);
  EXPECT_FALSE(CelListEqImpl(f.out_slot, f.a_slot, f.b_slot, f.ctx).ok());
}

}  // namespace
}  // namespace celwasm
