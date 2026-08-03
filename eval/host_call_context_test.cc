// Layer-1 unit matrix for HostCallContext — the typed accessor surface
// over the raw 24-byte CelValue slot ABI.  Driven over the shared fake
// MemoryView / ExternrefTable / ArenaAllocator (no wasm), so every
// accessor and setter is exercised in isolation.
//
// Per type, both halves are load-bearing:
//   - positive: ArgXxx decodes a correctly-typed slot; ReturnXxx encodes.
//   - negative: ArgXxx on a wrong-kind slot → InvalidArgument (the
//     non-bypassable kind-tag check); OOB arg index → OutOfRange; a
//     dangling externref slot → FailedPrecondition — never UB / a
//     garbage read.
//
// Design: doc/implementation-plan/rewrite/m21-host-call-adapter.md §7.

#include "eval/host_call_context.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "eval/attribute.h"
#include "eval/error.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

using ::absl_testing::StatusIs;
using ::celwasm::test::FakeArenaAllocator;
using ::celwasm::test::FakeExternrefTable;
using ::celwasm::test::FakeMemoryView;
using ::celwasm::testdata::Customer;

// Memory layout for the fakes: CelValue slots live low; argument string
// bytes in a mid region; the arena (for ReturnString / ReturnBytes)
// occupies the high half so a return allocation can never collide with
// a staged slot.
constexpr uint32_t kOutSlot = 0;
constexpr uint32_t kArg0 = 24;
constexpr uint32_t kArg1 = 48;
constexpr uint32_t kArg2 = 72;
constexpr uint32_t kStrArea = 0x1000;    // staged arg string bytes
constexpr uint32_t kArenaBase = 0x8000;  // ReturnString/Bytes allocations
constexpr size_t kArenaCap = 0x8000;

class HostCallContextTest : public testing::Test {
 protected:
  CelValue MakeBool(bool v) {
    CelValue cv{};
    cv.kind = CEL_BOOL;
    cv.payload.b = v ? 1 : 0;
    return cv;
  }
  CelValue MakeInt(int64_t v) {
    CelValue cv{};
    cv.kind = CEL_INT;
    cv.payload.i = v;
    return cv;
  }
  CelValue MakeUint(uint64_t v) {
    CelValue cv{};
    cv.kind = CEL_UINT;
    cv.payload.u = v;
    return cv;
  }
  CelValue MakeDouble(double v) {
    CelValue cv{};
    cv.kind = CEL_DOUBLE;
    cv.payload.d = v;
    return cv;
  }
  CelValue MakeNull() {
    CelValue cv{};
    cv.kind = CEL_NULL;
    return cv;
  }
  // Stage `s` bytes at `ptr` and return a CEL_STRING / CEL_BYTES slot.
  CelValue MakeStr(uint32_t kind, uint32_t ptr, absl::string_view s) {
    if (!s.empty()) std::memcpy(mem_.data() + ptr, s.data(), s.size());
    CelValue cv{};
    cv.kind = kind;
    cv.payload.s.ptr = ptr;
    cv.payload.s.len = static_cast<uint32_t>(s.size());
    return cv;
  }
  CelValue MakeDuration(absl::Duration d) {
    CelValue cv{};
    cv.kind = CEL_DURATION;
    DecomposeAbslDuration(d, &cv.payload.dur);
    return cv;
  }
  CelValue MakeTimestamp(absl::Time t) {
    CelValue cv{};
    cv.kind = CEL_TIMESTAMP;
    DecomposeAbslDuration(t - absl::UnixEpoch(), &cv.payload.ts);
    return cv;
  }
  CelValue MakeMessage(uint32_t msg_slot) {
    CelValue cv{};
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = msg_slot;
    return cv;
  }
  CelValue MakeHostList(uint32_t ref_slot) {
    CelValue cv{};
    cv.kind = CEL_LIST_HOST;
    cv.payload.ref_slot = ref_slot;
    return cv;
  }
  CelValue MakeHostMap(uint32_t ref_slot) {
    CelValue cv{};
    cv.kind = CEL_MAP_HOST;
    cv.payload.ref_slot = ref_slot;
    return cv;
  }
  // Build a CEL_LIST_ARENA over `elems`: header at `kStrArea`, elements
  // run immediately after it.
  CelValue MakeArenaList(const std::vector<CelValue>& elems) {
    const uint32_t header_ptr = kStrArea;
    const uint32_t elems_ptr = kStrArea + sizeof(ArenaListHeader);
    ArenaListHeader h{};
    h.count = static_cast<uint32_t>(elems.size());
    h.capacity = h.count;
    h.elements_offset = elems_ptr;
    std::memcpy(mem_.data() + header_ptr, &h, sizeof(h));
    for (size_t i = 0; i < elems.size(); ++i) {
      mem_.WriteCelValue(
          elems_ptr + (static_cast<uint32_t>(i) * kCelListEntryStride),
          elems[i]);
    }
    CelValue cv{};
    cv.kind = CEL_LIST_ARENA;
    cv.payload.arena_list.header_ptr = header_ptr;
    return cv;
  }
  // Build a CEL_MAP_ARENA over (key,val) pairs: header at kStrArea,
  // 48-byte entries run after it.
  CelValue MakeArenaMap(const std::vector<std::pair<CelValue, CelValue>>& kv) {
    const uint32_t header_ptr = kStrArea;
    const uint32_t entries_ptr = kStrArea + sizeof(ArenaMapHeader);
    ArenaMapHeader h{};
    h.count = static_cast<uint32_t>(kv.size());
    h.capacity = h.count;
    h.entries_offset = entries_ptr;
    std::memcpy(mem_.data() + header_ptr, &h, sizeof(h));
    for (size_t i = 0; i < kv.size(); ++i) {
      const uint32_t off =
          entries_ptr + (static_cast<uint32_t>(i) * kCelMapEntryStride);
      mem_.WriteCelValue(off, kv[i].first);
      mem_.WriteCelValue(off + sizeof(CelValue), kv[i].second);
    }
    CelValue cv{};
    cv.kind = CEL_MAP_ARENA;
    cv.payload.arena_map.header_ptr = header_ptr;
    return cv;
  }

  // Construct a context over args staged at the given slots.  The slot
  // vector is owned by the test fixture so the context's span stays
  // valid for the test's duration.
  HostCallContext Ctx(std::vector<uint32_t> arg_slots) {
    arg_slots_ = std::move(arg_slots);
    return {mem_, refs_, arena_, kOutSlot, arg_slots_};
  }

  CelValue Out() const {
    return mem_.ReadCelValue(kOutSlot);
  }

  FakeMemoryView mem_;
  FakeExternrefTable refs_;
  FakeArenaAllocator arena_{&mem_, kArenaBase, kArenaCap};
  std::vector<uint32_t> arg_slots_;
};

// ════════════════════════ scalar arg decode ════════════════════════

TEST_F(HostCallContextTest, ArgBoolDecodesBothValues) {
  mem_.Place(kArg0, MakeBool(true));
  mem_.Place(kArg1, MakeBool(false));
  auto ctx = Ctx({kArg0, kArg1});
  EXPECT_EQ(ctx.NumArgs(), 2);
  ASSERT_TRUE(ctx.ArgBool(0).ok());
  EXPECT_TRUE(*ctx.ArgBool(0));
  EXPECT_FALSE(*ctx.ArgBool(1));
}

TEST_F(HostCallContextTest, ArgBoolRejectsWrongKind) {
  mem_.Place(kArg0, MakeInt(1));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgBool(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(HostCallContextTest, ArgIntDecodesBoundaries) {
  const int64_t cases[] = {0, -1, 1, std::numeric_limits<int64_t>::min(),
                           std::numeric_limits<int64_t>::max()};
  for (int64_t c : cases) {
    mem_.Place(kArg0, MakeInt(c));
    auto ctx = Ctx({kArg0});
    ASSERT_TRUE(ctx.ArgInt(0).ok()) << c;
    EXPECT_EQ(*ctx.ArgInt(0), c);
  }
}

TEST_F(HostCallContextTest, ArgIntRejectsWrongKind) {
  mem_.Place(kArg0, MakeStr(CEL_STRING, kStrArea, "x"));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgInt(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(HostCallContextTest, ArgUintDecodesBoundaries) {
  const uint64_t cases[] = {0u, 1u, std::numeric_limits<uint64_t>::max()};
  for (uint64_t c : cases) {
    mem_.Place(kArg0, MakeUint(c));
    auto ctx = Ctx({kArg0});
    ASSERT_TRUE(ctx.ArgUint(0).ok()) << c;
    EXPECT_EQ(*ctx.ArgUint(0), c);
  }
}

TEST_F(HostCallContextTest, ArgUintRejectsWrongKind) {
  mem_.Place(kArg0, MakeInt(1));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgUint(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(HostCallContextTest, ArgDoubleDecodesSpecialValues) {
  mem_.Place(kArg0, MakeDouble(3.5));
  mem_.Place(kArg1, MakeDouble(std::numeric_limits<double>::infinity()));
  mem_.Place(kArg2, MakeDouble(-0.0));
  auto ctx = Ctx({kArg0, kArg1, kArg2});
  EXPECT_EQ(*ctx.ArgDouble(0), 3.5);
  EXPECT_TRUE(std::isinf(*ctx.ArgDouble(1)));
  EXPECT_TRUE(std::signbit(*ctx.ArgDouble(2)));
}

TEST_F(HostCallContextTest, ArgDoubleDecodesNan) {
  mem_.Place(kArg0, MakeDouble(std::numeric_limits<double>::quiet_NaN()));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(ctx.ArgDouble(0).ok());
  EXPECT_TRUE(std::isnan(*ctx.ArgDouble(0)));
}

TEST_F(HostCallContextTest, ArgDoubleRejectsWrongKind) {
  mem_.Place(kArg0, MakeInt(1));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgDouble(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ════════════════════════ string / bytes args ═════════════════════

TEST_F(HostCallContextTest, ArgStringDecodesIncludingEdges) {
  // empty, ascii, multi-byte UTF-8.
  std::string nul;
  nul.push_back('a');
  nul.push_back('\0');
  nul.push_back('b');
  const std::pair<std::string, uint32_t> cases[] = {
      {"", kStrArea},
      {"hello", kStrArea + 16},
      {"日本語", kStrArea + 64},
      {nul, kStrArea + 128},
  };
  for (const auto& [s, ptr] : cases) {
    mem_.Place(kArg0, MakeStr(CEL_STRING, ptr, s));
    auto ctx = Ctx({kArg0});
    ASSERT_TRUE(ctx.ArgString(0).ok());
    EXPECT_EQ(std::string(*ctx.ArgString(0)), s);
  }
}

TEST_F(HostCallContextTest, ArgStringRejectsBytesKind) {
  mem_.Place(kArg0, MakeStr(CEL_BYTES, kStrArea, "x"));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgString(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(HostCallContextTest, ArgBytesDecodesEmbeddedNul) {
  std::string b;
  b.push_back('\0');
  b.push_back('\x01');
  b.push_back('\0');
  mem_.Place(kArg0, MakeStr(CEL_BYTES, kStrArea, b));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(ctx.ArgBytes(0).ok());
  EXPECT_EQ(std::string(*ctx.ArgBytes(0)), b);
}

TEST_F(HostCallContextTest, ArgBytesRejectsStringKind) {
  mem_.Place(kArg0, MakeStr(CEL_STRING, kStrArea, "x"));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgBytes(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ════════════════════════ temporal args ════════════════════════════

TEST_F(HostCallContextTest, ArgDurationRoundTrips) {
  const absl::Duration d = absl::Seconds(90) + absl::Nanoseconds(500);
  mem_.Place(kArg0, MakeDuration(d));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(ctx.ArgDuration(0).ok());
  EXPECT_EQ(*ctx.ArgDuration(0), d);
}

TEST_F(HostCallContextTest, ArgDurationRejectsWrongKind) {
  mem_.Place(kArg0, MakeInt(1));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgDuration(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(HostCallContextTest, ArgTimestampRoundTrips) {
  const absl::Time t = absl::FromUnixSeconds(1700000000) + absl::Nanoseconds(7);
  mem_.Place(kArg0, MakeTimestamp(t));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(ctx.ArgTimestamp(0).ok());
  EXPECT_EQ(*ctx.ArgTimestamp(0), t);
}

TEST_F(HostCallContextTest, ArgTimestampRejectsWrongKind) {
  mem_.Place(kArg0, MakeInt(1));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgTimestamp(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ════════════════════════ null ═════════════════════════════════════

TEST_F(HostCallContextTest, ArgIsNullDistinguishesNullFromPresent) {
  mem_.Place(kArg0, MakeNull());
  mem_.Place(kArg1, MakeInt(0));
  auto ctx = Ctx({kArg0, kArg1});
  EXPECT_TRUE(ctx.ArgIsNull(0));
  EXPECT_FALSE(ctx.ArgIsNull(1));
  EXPECT_FALSE(ctx.ArgIsNull(5));  // out of range → false, not a crash
}

// ════════════════════════ bounds ═══════════════════════════════════

TEST_F(HostCallContextTest, OutOfRangeArgIndexErrors) {
  mem_.Place(kArg0, MakeInt(1));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgInt(1).status(), StatusIs(absl::StatusCode::kOutOfRange));
  EXPECT_THAT(ctx.ArgInt(-1).status(), StatusIs(absl::StatusCode::kOutOfRange));
  EXPECT_THAT(ctx.ArgString(99).status(),
              StatusIs(absl::StatusCode::kOutOfRange));
}

// ════════════════════════ proto arg ════════════════════════════════

TEST_F(HostCallContextTest, ArgProtoReturnsLiveMessage) {
  Customer c;
  c.set_name("Ada");
  c.set_age(36);
  const uint32_t slot = refs_.Intern(std::make_shared<ProtoBacking>(&c));
  mem_.Place(kArg0, MakeMessage(slot));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(ctx.ArgProto(0).ok()) << ctx.ArgProto(0).status();
  const auto* down = dynamic_cast<const Customer*>(*ctx.ArgProto(0));
  ASSERT_NE(down, nullptr);
  EXPECT_EQ(down->name(), "Ada");
  EXPECT_EQ(down->age(), 36);
}

TEST_F(HostCallContextTest, ArgProtoRejectsWrongKind) {
  mem_.Place(kArg0, MakeInt(1));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgProto(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(HostCallContextTest, ArgProtoDanglingSlotErrors) {
  mem_.Place(kArg0, MakeMessage(999));  // never interned
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgProto(0).status(),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

// ════════════════════════ list args ════════════════════════════════

TEST_F(HostCallContextTest, ArgListHostScalarElements) {
  std::vector<Value> elems = {Value::Int(10), Value::Int(20), Value::Int(30)};
  const uint32_t slot =
      refs_.InternList(std::make_shared<HostList>(std::move(elems)));
  mem_.Place(kArg0, MakeHostList(slot));
  auto ctx = Ctx({kArg0});
  auto lv_or = ctx.ArgList(0);
  ASSERT_TRUE(lv_or.ok()) << lv_or.status();
  EXPECT_EQ(lv_or->Size(), 3u);
  ASSERT_TRUE(lv_or->At(1).ok());
  EXPECT_EQ(*lv_or->At(1)->AsInt(), 20);
}

TEST_F(HostCallContextTest, ArgListHostMessageElements) {
  Customer a;
  a.set_name("a");
  Customer b;
  b.set_name("b");
  std::vector<Value> elems = {Value::Message(a), Value::Message(b)};
  const uint32_t slot =
      refs_.InternList(std::make_shared<HostList>(std::move(elems)));
  mem_.Place(kArg0, MakeHostList(slot));
  auto ctx = Ctx({kArg0});
  auto lv_or = ctx.ArgList(0);
  ASSERT_TRUE(lv_or.ok());
  EXPECT_EQ(lv_or->Size(), 2u);
  auto elem_or = lv_or->At(0);
  ASSERT_TRUE(elem_or.ok());
  ASSERT_TRUE(elem_or->MessageBacking().ok());
  const auto* c =
      dynamic_cast<const Customer*>((*elem_or->MessageBacking())->message());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->name(), "a");
}

TEST_F(HostCallContextTest, ArgListArenaScalarElements) {
  mem_.Place(kArg0, MakeArenaList({MakeInt(1), MakeInt(2)}));
  auto ctx = Ctx({kArg0});
  auto lv_or = ctx.ArgList(0);
  ASSERT_TRUE(lv_or.ok()) << lv_or.status();
  EXPECT_EQ(lv_or->Size(), 2u);
  EXPECT_EQ(*lv_or->At(0)->AsInt(), 1);
  EXPECT_EQ(*lv_or->At(1)->AsInt(), 2);
}

TEST_F(HostCallContextTest, ArgListArenaOutOfRangeIndexErrors) {
  mem_.Place(kArg0, MakeArenaList({MakeInt(1)}));
  auto ctx = Ctx({kArg0});
  auto lv_or = ctx.ArgList(0);
  ASSERT_TRUE(lv_or.ok());
  EXPECT_THAT(lv_or->At(5).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(HostCallContextTest, ArgListRejectsWrongKind) {
  mem_.Place(kArg0, MakeInt(1));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgList(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(HostCallContextTest, ArgListHostDanglingSlotErrors) {
  mem_.Place(kArg0, MakeHostList(999));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgList(0).status(),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

// ════════════════════════ map args ═════════════════════════════════

TEST_F(HostCallContextTest, ArgMapHostStringKeys) {
  std::vector<std::pair<Value, Value>> entries = {
      {Value::String("a"), Value::Int(1)}, {Value::String("b"), Value::Int(2)}};
  const uint32_t slot =
      refs_.InternMap(std::make_shared<HostMap>(std::move(entries)));
  mem_.Place(kArg0, MakeHostMap(slot));
  auto ctx = Ctx({kArg0});
  auto mv_or = ctx.ArgMap(0);
  ASSERT_TRUE(mv_or.ok()) << mv_or.status();
  EXPECT_EQ(mv_or->Size(), 2u);
  EXPECT_TRUE(mv_or->ContainsKey(Value::String("b")));
  ASSERT_TRUE(mv_or->Get(Value::String("a")).ok());
  EXPECT_EQ(*mv_or->Get(Value::String("a"))->AsInt(), 1);
}

TEST_F(HostCallContextTest, ArgMapHostMissingKeyReturnsErrorValue) {
  std::vector<std::pair<Value, Value>> entries = {
      {Value::String("a"), Value::Int(1)}};
  const uint32_t slot =
      refs_.InternMap(std::make_shared<HostMap>(std::move(entries)));
  mem_.Place(kArg0, MakeHostMap(slot));
  auto ctx = Ctx({kArg0});
  auto mv_or = ctx.ArgMap(0);
  ASSERT_TRUE(mv_or.ok());
  EXPECT_FALSE(mv_or->ContainsKey(Value::String("z")));
  auto got = mv_or->Get(Value::String("z"));
  ASSERT_TRUE(got.ok());  // missing key is a spec error Value, not a status
  EXPECT_TRUE(got->IsError());
}

TEST_F(HostCallContextTest, ArgMapArenaStringKeys) {
  // arena map { "k" : 7 } — key string bytes staged after the entries.
  const uint32_t key_ptr = kStrArea + 0x400;
  mem_.Place(kArg0,
             MakeArenaMap({{MakeStr(CEL_STRING, key_ptr, "k"), MakeInt(7)}}));
  auto ctx = Ctx({kArg0});
  auto mv_or = ctx.ArgMap(0);
  ASSERT_TRUE(mv_or.ok()) << mv_or.status();
  EXPECT_EQ(mv_or->Size(), 1u);
  EXPECT_TRUE(mv_or->ContainsKey(Value::String("k")));
  ASSERT_TRUE(mv_or->Get(Value::String("k")).ok());
  EXPECT_EQ(*mv_or->Get(Value::String("k"))->AsInt(), 7);
}

TEST_F(HostCallContextTest, ArgMapHostIntKeysCrossType) {
  // int-keyed map — exercises the non-string map-key path.
  std::vector<std::pair<Value, Value>> entries = {
      {Value::Int(7), Value::String("seven")}};
  const uint32_t slot =
      refs_.InternMap(std::make_shared<HostMap>(std::move(entries)));
  mem_.Place(kArg0, MakeHostMap(slot));
  auto ctx = Ctx({kArg0});
  auto mv_or = ctx.ArgMap(0);
  ASSERT_TRUE(mv_or.ok());
  EXPECT_TRUE(mv_or->ContainsKey(Value::Int(7)));
  ASSERT_TRUE(mv_or->Get(Value::Int(7)).ok());
  EXPECT_EQ(*mv_or->Get(Value::Int(7))->AsString(), "seven");
}

TEST_F(HostCallContextTest, ArgMapRejectsWrongKind) {
  mem_.Place(kArg0, MakeInt(1));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgMap(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ═══════════════ complex / nested aggregates (the motivating cases) ═

// map<string, list<customer>> — the headline composite type.  Get the
// list value, then walk its message elements.
TEST_F(HostCallContextTest, ArgMapNestedListOfMessages) {
  Customer a;
  a.set_name("a");
  Customer b;
  b.set_name("b");
  std::vector<Value> inner = {Value::Message(a), Value::Message(b)};
  std::vector<std::pair<Value, Value>> entries = {
      {Value::String("team"), Value::List(std::move(inner))}};
  const uint32_t slot =
      refs_.InternMap(std::make_shared<HostMap>(std::move(entries)));
  mem_.Place(kArg0, MakeHostMap(slot));
  auto ctx = Ctx({kArg0});
  auto mv_or = ctx.ArgMap(0);
  ASSERT_TRUE(mv_or.ok());
  auto list_or = mv_or->Get(Value::String("team"));
  ASSERT_TRUE(list_or.ok());
  ASSERT_EQ(list_or->kind(), Value::Kind::kList);
  ASSERT_TRUE(list_or->ListBacking().ok());
  const HostListBacking* inner_list = *list_or->ListBacking();
  ASSERT_EQ(inner_list->Size(), 2u);
  auto e0 = inner_list->At(0, CelType{});
  ASSERT_TRUE(e0.ok());
  ASSERT_TRUE(e0->MessageBacking().ok());
  const auto* c =
      dynamic_cast<const Customer*>((*e0->MessageBacking())->message());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->name(), "a");
}

// list<customer> as an arena list whose elements are host messages —
// exercises the arena element-walk recursing into externref messages.
TEST_F(HostCallContextTest, ArgListArenaOfMessages) {
  Customer a;
  a.set_name("arena-msg");
  const uint32_t msg_slot = refs_.Intern(std::make_shared<ProtoBacking>(&a));
  mem_.Place(kArg0, MakeArenaList({MakeMessage(msg_slot)}));
  auto ctx = Ctx({kArg0});
  auto lv_or = ctx.ArgList(0);
  ASSERT_TRUE(lv_or.ok());
  EXPECT_EQ(lv_or->Size(), 1u);
  auto e0 = lv_or->At(0);
  ASSERT_TRUE(e0.ok());
  ASSERT_TRUE(e0->MessageBacking().ok());
  const auto* c =
      dynamic_cast<const Customer*>((*e0->MessageBacking())->message());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->name(), "arena-msg");
}

// ════════════════════════ ArgValue escape hatch ════════════════════

TEST_F(HostCallContextTest, ArgValueDecodesScalar) {
  mem_.Place(kArg0, MakeInt(42));
  auto ctx = Ctx({kArg0});
  auto v_or = ctx.ArgValue(0);
  ASSERT_TRUE(v_or.ok());
  EXPECT_EQ(v_or->kind(), Value::Kind::kInt);
  EXPECT_EQ(*v_or->AsInt(), 42);
}

TEST_F(HostCallContextTest, ArgValueDecodesNestedArenaList) {
  // list<list<int>> via arena — exercises the recursive decoder.
  // Outer list of one element which is itself a CEL_LIST_ARENA.
  // Build inner list first at a distinct region, then the outer.
  const uint32_t inner_hdr = kStrArea + 0x800;
  const uint32_t inner_elems = inner_hdr + sizeof(ArenaListHeader);
  ArenaListHeader ih{};
  ih.count = 2;
  ih.capacity = 2;
  ih.elements_offset = inner_elems;
  std::memcpy(mem_.data() + inner_hdr, &ih, sizeof(ih));
  mem_.WriteCelValue(inner_elems, MakeInt(5));
  mem_.WriteCelValue(inner_elems + kCelListEntryStride, MakeInt(6));
  CelValue inner{};
  inner.kind = CEL_LIST_ARENA;
  inner.payload.arena_list.header_ptr = inner_hdr;

  mem_.Place(kArg0, MakeArenaList({inner}));
  auto ctx = Ctx({kArg0});
  auto v_or = ctx.ArgValue(0);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  EXPECT_EQ(v_or->kind(), Value::Kind::kList);
  ASSERT_TRUE(v_or->ListBacking().ok());
  EXPECT_EQ((*v_or->ListBacking())->Size(), 1u);
}

// ════════════════════════ scalar returns ═══════════════════════════

TEST_F(HostCallContextTest, ReturnScalarsEncodeInline) {
  {
    auto ctx = Ctx({});
    ASSERT_TRUE(ctx.ReturnBool(true).ok());
    EXPECT_EQ(Out().kind, CEL_BOOL);
    EXPECT_NE(Out().payload.b, 0);
  }
  {
    auto ctx = Ctx({});
    ASSERT_TRUE(ctx.ReturnInt(-7).ok());
    EXPECT_EQ(Out().kind, CEL_INT);
    EXPECT_EQ(Out().payload.i, -7);
  }
  {
    auto ctx = Ctx({});
    ASSERT_TRUE(ctx.ReturnUint(9u).ok());
    EXPECT_EQ(Out().kind, CEL_UINT);
    EXPECT_EQ(Out().payload.u, 9u);
  }
  {
    auto ctx = Ctx({});
    ASSERT_TRUE(ctx.ReturnDouble(2.5).ok());
    EXPECT_EQ(Out().kind, CEL_DOUBLE);
    EXPECT_EQ(Out().payload.d, 2.5);
  }
  {
    auto ctx = Ctx({});
    ASSERT_TRUE(ctx.ReturnNull().ok());
    EXPECT_EQ(Out().kind, CEL_NULL);
  }
}

TEST_F(HostCallContextTest, ReturnDurationTimestampEncode) {
  {
    auto ctx = Ctx({});
    ASSERT_TRUE(ctx.ReturnDuration(absl::Seconds(3)).ok());
    EXPECT_EQ(Out().kind, CEL_DURATION);
    EXPECT_EQ(Out().payload.dur.seconds, 3);
  }
  {
    auto ctx = Ctx({});
    const absl::Time t = absl::FromUnixSeconds(100);
    ASSERT_TRUE(ctx.ReturnTimestamp(t).ok());
    EXPECT_EQ(Out().kind, CEL_TIMESTAMP);
    EXPECT_EQ(Out().payload.ts.seconds, 100);
  }
}

TEST_F(HostCallContextTest, ReturnStringAllocatesInArena) {
  auto ctx = Ctx({});
  ASSERT_TRUE(ctx.ReturnString("NEW").ok());
  EXPECT_EQ(Out().kind, CEL_STRING);
  // Newly allocated — the span must land in the arena region, not at a
  // staged input offset.
  EXPECT_GE(Out().payload.s.ptr, kArenaBase);
  EXPECT_EQ(
      std::string(mem_.ReadSpan(Out().payload.s.ptr, Out().payload.s.len)),
      "NEW");
}

TEST_F(HostCallContextTest, ReturnBytesAllocatesInArena) {
  std::string b;
  b.push_back('\0');
  b.push_back('z');
  auto ctx = Ctx({});
  ASSERT_TRUE(ctx.ReturnBytes(b).ok());
  EXPECT_EQ(Out().kind, CEL_BYTES);
  EXPECT_EQ(
      std::string(mem_.ReadSpan(Out().payload.s.ptr, Out().payload.s.len)), b);
}

// ════════════════════════ aggregate returns ════════════════════════

TEST_F(HostCallContextTest, ReturnProtoInternsMessage) {
  auto m = std::make_unique<Customer>();
  m->set_name("Built");
  auto ctx = Ctx({});
  ASSERT_TRUE(ctx.ReturnProto(std::move(m)).ok());
  EXPECT_EQ(Out().kind, CEL_MESSAGE);
  const HostMessageBacking* backing = refs_.Lookup(Out().payload.msg_slot);
  ASSERT_NE(backing, nullptr);
  const auto* c = dynamic_cast<const Customer*>(backing->message());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->name(), "Built");
}

TEST_F(HostCallContextTest, ReturnListInternsHostList) {
  const Value elems[] = {Value::Int(1), Value::Int(2)};
  auto ctx = Ctx({});
  ASSERT_TRUE(ctx.ReturnList(elems).ok());
  EXPECT_EQ(Out().kind, CEL_LIST_HOST);
  const HostListBacking* backing = refs_.LookupList(Out().payload.ref_slot);
  ASSERT_NE(backing, nullptr);
  EXPECT_EQ(backing->Size(), 2u);
  EXPECT_EQ(*backing->At(0, CelType{})->AsInt(), 1);
}

TEST_F(HostCallContextTest, ReturnMapInternsHostMap) {
  const std::pair<Value, Value> entries[] = {
      {Value::String("k"), Value::Int(99)}};
  auto ctx = Ctx({});
  ASSERT_TRUE(ctx.ReturnMap(entries).ok());
  EXPECT_EQ(Out().kind, CEL_MAP_HOST);
  const HostMapBacking* backing = refs_.LookupMap(Out().payload.ref_slot);
  ASSERT_NE(backing, nullptr);
  EXPECT_EQ(backing->Size(), 1u);
  EXPECT_TRUE(backing->ContainsKey(Value::String("k")));
}

// ════════════════════════ 3VL returns ══════════════════════════════

TEST_F(HostCallContextTest, ReturnUnknownMintsSentinelDescriptor) {
  // Function-origin unknown: a 1-element UnknownSet descriptor whose
  // id array carries the reserved sentinel (the descriptor wire of
  // doc/design/03-abi-and-memory.md §8.2 — a raw sentinel in
  // payload.unk would be dereferenced as a descriptor offset).
  auto ctx = Ctx({});
  ASSERT_TRUE(ctx.ReturnUnknown().ok());
  EXPECT_EQ(Out().kind, CEL_UNKNOWN);
  EXPECT_THAT(test::ReadUnknownIds(mem_, Out()),
              ::testing::ElementsAre(kFunctionUnknownSentinel));
}

TEST_F(HostCallContextTest, ReturnErrorEncodesCode) {
  ErrorPayload p;
  p.code = ErrorCode::kDivideByZero;
  auto ctx = Ctx({});
  ASSERT_TRUE(ctx.ReturnError(std::move(p)).ok());
  EXPECT_EQ(Out().kind, CEL_ERROR);
  EXPECT_EQ(Out().payload.err, static_cast<uint32_t>(ErrorCode::kDivideByZero));
}

TEST_F(HostCallContextTest, ReturnValueScalar) {
  auto ctx = Ctx({});
  ASSERT_TRUE(ctx.ReturnValue(Value::Int(123)).ok());
  EXPECT_EQ(Out().kind, CEL_INT);
  EXPECT_EQ(Out().payload.i, 123);
}

TEST_F(HostCallContextTest, ReturnValuePreservesPropagatedUnknownAttribute) {
  // A propagated input unknown carries a real attribute id (here 4),
  // distinct from the function-origin sentinel — it must round-trip
  // verbatim, not get rewritten to the sentinel.
  auto ctx = Ctx({});
  ASSERT_TRUE(ctx.ReturnValue(Value::Unknown(AttributeId{4})).ok());
  EXPECT_EQ(Out().kind, CEL_UNKNOWN);
  EXPECT_THAT(test::ReadUnknownIds(mem_, Out()), ::testing::ElementsAre(4u));
}

TEST_F(HostCallContextTest, ReturnValuePreservesMergedUnknownSet) {
  // A merged unknown (several attribute identities) round-trips the
  // whole set; the descriptor's id array is sorted ascending.
  auto ctx = Ctx({});
  ASSERT_TRUE(ctx.ReturnValue(Value::Unknown(std::vector<AttributeId>{
                                  AttributeId{9}, AttributeId{2}}))
                  .ok());
  EXPECT_EQ(Out().kind, CEL_UNKNOWN);
  EXPECT_THAT(test::ReadUnknownIds(mem_, Out()),
              ::testing::ElementsAre(2u, 9u));
}

TEST_F(HostCallContextTest, UnknownArgDecodesEveryDescriptorId) {
  // The host-call arg decoder dereferences the descriptor and
  // surfaces EVERY merged id — the host-fn side of the §8.2 contract.
  constexpr uint32_t kDescOff = 1024;
  constexpr uint32_t kIdsOff = kDescOff + 8;
  const uint32_t desc[2] = {kIdsOff, 2};
  std::memcpy(mem_.data() + kDescOff, desc, sizeof(desc));
  const uint32_t ids[2] = {3, 8};
  std::memcpy(mem_.data() + kIdsOff, ids, sizeof(ids));
  CelValue cv{};
  cv.kind = CEL_UNKNOWN;
  cv.payload.unk = kDescOff;
  mem_.Place(kArg0, cv);

  auto ctx = Ctx({kArg0});
  auto v = ctx.ArgValue(0);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_TRUE(v->IsUnknown());
  ASSERT_TRUE(v->UnknownAttributes().ok());
  EXPECT_THAT(*v->UnknownAttributes(),
              ::testing::ElementsAre(AttributeId{3}, AttributeId{8}));
}

TEST_F(HostCallContextTest, UnknownArgWithZeroPayloadDecodesEmptySet) {
  // payload.unk == 0 is the legal empty UnknownSet — decodes to an
  // unknown with no recorded provenance, not an error.
  CelValue cv{};
  cv.kind = CEL_UNKNOWN;
  cv.payload.unk = 0;
  mem_.Place(kArg0, cv);
  auto ctx = Ctx({kArg0});
  auto v = ctx.ArgValue(0);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_TRUE(v->IsUnknown());
  ASSERT_TRUE(v->UnknownAttributes().ok());
  EXPECT_TRUE(v->UnknownAttributes()->empty());
}

TEST_F(HostCallContextTest, UnknownArgWithOutOfBoundsDescriptorErrors) {
  // A descriptor offset past the memory end must surface a clean
  // error, never an OOB host read.
  CelValue cv{};
  cv.kind = CEL_UNKNOWN;
  cv.payload.unk = mem_.Size() - 4;  // 8-byte descriptor won't fit
  mem_.Place(kArg0, cv);
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(ctx.ArgValue(0).status(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ════════════════ wire-error decode + kind diagnostics ════════════════

// A CEL_ERROR arg decodes to a Value error whose ErrorPayload code
// mirrors the wire byte 1:1 (DecodeWireError's recognized set).
TEST_F(HostCallContextTest, ArgValueDecodesRecognizedWireErrorCodes) {
  const ErrorCode codes[] = {
      ErrorCode::kOverflow,         ErrorCode::kDivideByZero,
      ErrorCode::kDuplicateKey,     ErrorCode::kIndexOutOfBounds,
      ErrorCode::kInvalidArgument,  ErrorCode::kFieldNotFound,
      ErrorCode::kUnknownType,      ErrorCode::kCustomFnFailed,
      ErrorCode::kHostAdapterError, ErrorCode::kTimeout,
  };
  for (ErrorCode code : codes) {
    CelValue cv{};
    cv.kind = CEL_ERROR;
    cv.payload.err = static_cast<uint32_t>(code);
    mem_.Place(kArg0, cv);
    auto ctx = Ctx({kArg0});
    auto v_or = ctx.ArgValue(0);
    ASSERT_TRUE(v_or.ok()) << static_cast<int>(code);
    ASSERT_TRUE(v_or->IsError()) << static_cast<int>(code);
    auto payload = v_or->ErrorInfo();
    ASSERT_TRUE(payload.ok());
    EXPECT_EQ((*payload)->code, code);
    EXPECT_EQ((*payload)->message, ErrorCodeName(code));
  }
}

// An unrecognized wire byte degrades to kHostAdapterError naming the
// raw code — the decoder must not crash on future/corrupt bytes.
TEST_F(HostCallContextTest, ArgValueDegradesUnknownWireErrorByte) {
  CelValue cv{};
  cv.kind = CEL_ERROR;
  cv.payload.err = 250;  // not a recognized ErrorCode
  mem_.Place(kArg0, cv);
  auto ctx = Ctx({kArg0});
  auto v_or = ctx.ArgValue(0);
  ASSERT_TRUE(v_or.ok());
  ASSERT_TRUE(v_or->IsError());
  auto payload = v_or->ErrorInfo();
  ASSERT_TRUE(payload.ok());
  EXPECT_EQ((*payload)->code, ErrorCode::kHostAdapterError);
  EXPECT_THAT((*payload)->message, ::testing::HasSubstr("250"));
}

// A CEL_MAP_ARENA arg eagerly decodes its entries through ArgValue
// (DecodeArenaMapEntries) — distinct from the lazy HostMapView path.
TEST_F(HostCallContextTest, ArgValueDecodesArenaMapEntries) {
  mem_.Place(kArg0, MakeArenaMap({{MakeInt(1), MakeInt(10)},
                                  {MakeInt(2), MakeInt(20)}}));
  auto ctx = Ctx({kArg0});
  auto v_or = ctx.ArgValue(0);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  ASSERT_EQ(v_or->kind(), Value::Kind::kMap);
  auto backing = v_or->MapBacking();
  ASSERT_TRUE(backing.ok());
  EXPECT_EQ((*backing)->Size(), 2u);
  auto v1 = (*backing)->Get(Value::Int(1), CelType::Int());
  auto v2 = (*backing)->Get(Value::Int(2), CelType::Int());
  ASSERT_TRUE(v1.ok() && v2.ok());
  EXPECT_EQ(*v1->AsInt(), 10);
  EXPECT_EQ(*v2->AsInt(), 20);
}

// header_ptr == 0 is the empty-map wire sentinel.
TEST_F(HostCallContextTest, ArgValueDecodesEmptyArenaMapSentinel) {
  CelValue cv{};
  cv.kind = CEL_MAP_ARENA;
  cv.payload.arena_map.header_ptr = 0;
  mem_.Place(kArg0, cv);
  auto ctx = Ctx({kArg0});
  auto v_or = ctx.ArgValue(0);
  ASSERT_TRUE(v_or.ok()) << v_or.status();
  ASSERT_EQ(v_or->kind(), Value::Kind::kMap);
  auto backing = v_or->MapBacking();
  ASSERT_TRUE(backing.ok());
  EXPECT_EQ((*backing)->Size(), 0u);
}

// A kind-mismatch diagnostic names the offending wire kind — one row
// per WireKindName arm the scalar reject cases above don't reach.
TEST_F(HostCallContextTest, MismatchDiagnosticNamesWireKind) {
  struct Row {
    CelValue slot;
    absl::string_view kind_name;
  };
  CelValue type_cv{};
  type_cv.kind = CEL_TYPE;
  CelValue unknown_cv{};
  unknown_cv.kind = CEL_UNKNOWN;
  CelValue error_cv{};
  error_cv.kind = CEL_ERROR;
  const Row rows[] = {
      {MakeNull(), "null"},
      {MakeDuration(absl::Seconds(1)), "duration"},
      {MakeTimestamp(absl::UnixEpoch()), "timestamp"},
      {MakeArenaList({}), "list"},
      {MakeArenaMap({}), "map"},
      {MakeMessage(1), "message"},
      {type_cv, "type"},
      {unknown_cv, "unknown"},
      {error_cv, "error"},
  };
  for (const Row& row : rows) {
    mem_.Place(kArg0, row.slot);
    auto ctx = Ctx({kArg0});
    const absl::Status s = ctx.ArgInt(0).status();
    EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument))
        << row.kind_name;
    EXPECT_THAT(std::string(s.message()),
                ::testing::HasSubstr(std::string(row.kind_name)))
        << row.kind_name;
  }
}

// Value::StructurallyEquals over AGGREGATE kinds compares by backing
// IDENTITY, not by contents: two separately-built lists with equal
// elements are structurally unequal, while two Values sharing one
// backing are equal.  That is deliberate — deep comparison of a
// host-backed aggregate would mean calling back into the embedder's
// backing, which this host-side check must not do.  (Tested here
// rather than in value_test because the aggregate factories live in
// this target's dep, not //eval:value's.)
// Backing-identity semantics themselves are pinned by
// host_list_test.cc's ValueListTest.StructurallyEqualsIsPointerIdentity,
// which builds the backing explicitly.  What is unique here is the
// `Value::List(...)` FACTORY path — it mints a fresh backing per call,
// so two equal-content lists compare unequal — plus the empty
// aggregate, whose emptiness must not make it equal to a non-aggregate.
TEST(ValueStructuralEqualityAggregateTest, FactoryMintsDistinctBackings) {
  const Value a = Value::List({Value::Int(1)});
  const Value b = Value::List({Value::Int(1)});
  EXPECT_FALSE(a.StructurallyEquals(b)) << "distinct backings, equal contents";
  EXPECT_FALSE(Value::List({}).StructurallyEquals(Value::Null()));
}

}  // namespace
}  // namespace celwasm
