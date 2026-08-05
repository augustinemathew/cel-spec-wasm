// cel_host `cel_set_field` tests — the pack arm (Any / WKT), the
// per-cpp_type kind-mismatch guards, the wrapper-field write side,
// the range-overflow poison contract, and the nested / repeated /
// map message-source guards.

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "eval/internal/cel_host_message.h"
#include "eval/internal/cel_host_test_harness.h"
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "google/protobuf/struct.pb.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"
#include "testdata/e2e_fixture.pb.h"
#include "testdata/host_fixture_proto2.pb.h"
#include "testdata/host_fixture_proto3.pb.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::celwasm::test::JsonLikeBacking;
using ::celwasm::test::Layer2Fixture;
using ::celwasm::testdata::Address;
using ::celwasm::testdata::Customer;
using ::celwasm::testdata::HostMsg2;
using ::celwasm::testdata::HostMsg3;

// Force generated-pool registration of descriptors referenced by
// tests below.  Runs once at static init per test binary.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<Customer>();
      google::protobuf::LinkMessageReflection<Address>();
      google::protobuf::LinkMessageReflection<HostMsg3>();
      return 0;
    }();

// ═══════════ Layer 2 — CelSetFieldImpl pack arm (M7-A.A) ═══════════
//
// Exercises WriteMessageOrPack across every cpp_type-MESSAGE call
// site: singular, repeated arena-source, repeated host-source, map
// arena-source, map host-source.  Byte-level invariants (type_url
// suffix, value bytes round-trip, empty-payload corner) live here
// because cel-cpp's checker types every selection through an Any-
// typed field as `dyn` and rejects v2 lowering — the e2e path can't
// reach the same assertion.  See m7a-any.md §11 / §10.3 + the
// `any_test.cc` AnyPackE2ETest section header.

// Minimal harness for pack-side trampoline tests.  Stages an
// OwnedProtoBacking<HostMsg3> as the mutable outer message, exposes
// a non-owning raw pointer for reflection-side readback, and bundles
// the rest of the Layer-2 context.
class PackHarness {
 public:
  PackHarness() {
    auto outer = std::make_unique<HostMsg3>();
    // outer_ aliases a body-local unique_ptr that is then moved into the
    // backing; it can't be a member initializer.
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    outer_ = outer.get();
    auto backing = std::make_shared<OwnedProtoBacking>(std::move(outer));
    outer_slot_ = f_.refs.Intern(backing);
    CelValue msg_cv{};
    msg_cv.kind = CEL_MESSAGE;
    msg_cv.payload.msg_slot = outer_slot_;
    f_.mem.WriteCelValue(kOuterMsgSlot, msg_cv);
  }

  // Stage a typed-message src in the externref table and wire a
  // CEL_MESSAGE CelValue at kSrcSlot pointing at it.  Returns the
  // backing pointer for the caller to keep alive if desired.
  template <typename M>
  void StageMessageSrc(std::unique_ptr<M> msg) {
    auto src_backing = std::make_shared<OwnedProtoBacking>(std::move(msg));
    const uint32_t src_slot = f_.refs.Intern(std::move(src_backing));
    CelValue cv{};
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = src_slot;
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Stage an arbitrary backing (not necessarily proto-carrying) in the
  // externref table and point a CEL_MESSAGE CelValue at it.  Reaches
  // the "backing has no proto message" arm, which a real proto backing
  // cannot.
  void StageBackingSrc(std::shared_ptr<HostMessageBacking> backing) {
    const uint32_t src_slot = f_.refs.Intern(std::move(backing));
    CelValue cv{};
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = src_slot;
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Intern `backing` and return a CEL_MESSAGE CelValue pointing at it,
  // without touching kSrcSlot — for embedding inside a staged arena
  // list.
  CelValue InternBackingElement(std::shared_ptr<HostMessageBacking> backing) {
    CelValue cv{};
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = f_.refs.Intern(std::move(backing));
    return cv;
  }

  // Stage a null src at kSrcSlot — exercises the null-clear arm
  // ordering (must not reach WriteMessageOrPack).
  void StageNullSrc() {
    CelValue cv{};
    cv.kind = CEL_NULL;
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Stage a HostList-backed src (vector of celwasm::Value::Message) and
  // wire a CEL_LIST_HOST CelValue at kSrcSlot.
  void StageHostListSrc(std::vector<celwasm::Value> elements) {
    auto list_backing = std::make_shared<HostList>(std::move(elements));
    const uint32_t slot = f_.refs.InternList(std::move(list_backing));
    CelValue cv{};
    cv.kind = CEL_LIST_HOST;
    cv.payload.ref_slot = slot;
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Stage a HostMap-backed src (vector of <key, value> celwasm::Value
  // pairs) and wire a CEL_MAP_HOST CelValue at kSrcSlot.
  void StageHostMapSrc(
      std::vector<std::pair<celwasm::Value, celwasm::Value>> entries) {
    auto map_backing = std::make_shared<HostMap>(std::move(entries));
    const uint32_t slot = f_.refs.InternMap(std::move(map_backing));
    CelValue cv{};
    cv.kind = CEL_MAP_HOST;
    cv.payload.ref_slot = slot;
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Stage a bare scalar / null CelValue at kSrcSlot.  String/bytes
  // payloads must be written into the arena separately; this overload
  // covers null / bool / int / uint / double.
  void StageScalar(const CelValue& cv) {
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Stage a CEL_STRING at kSrcSlot, copying `s` into the arena.
  void StageString(absl::string_view s) {
    const uint32_t off = arena_top_;
    std::memcpy(f_.mem.data() + off, s.data(), s.size());
    arena_top_ += static_cast<uint32_t>(s.size());
    CelValue cv{};
    cv.kind = CEL_STRING;
    cv.payload.s.ptr = off;
    cv.payload.s.len = static_cast<uint32_t>(s.size());
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Stage a CEL_DURATION / CEL_TIMESTAMP at kSrcSlot from a
  // (seconds, nanos) pair.
  void StageDurTs(CelKind kind, int64_t seconds, int32_t nanos) {
    CelValue cv{};
    cv.kind = kind;
    CelDurTs dt{seconds, nanos, 0};
    if (kind == CEL_DURATION) {
      cv.payload.dur = dt;
    } else {
      cv.payload.ts = dt;
    }
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Stage a CEL_LIST_ARENA at kSrcSlot from a vector of element
  // CelValues.  Lays out an ArenaListHeader + contiguous 24-byte
  // element run in a scratch arena region.
  void StageArenaList(const std::vector<CelValue>& elems) {
    const uint32_t elems_off = arena_top_;
    for (const CelValue& e : elems) {
      f_.mem.WriteCelValue(arena_top_, e);
      arena_top_ += static_cast<uint32_t>(sizeof(CelValue));
    }
    const uint32_t hdr_off = arena_top_;
    f_.mem.WriteU32(hdr_off + 0, static_cast<uint32_t>(elems.size()));
    f_.mem.WriteU32(hdr_off + 4, static_cast<uint32_t>(elems.size()));
    f_.mem.WriteU32(hdr_off + 8, elems_off);
    f_.mem.WriteU32(hdr_off + 12, 0);
    arena_top_ += 16;
    CelValue cv{};
    cv.kind = CEL_LIST_ARENA;
    cv.payload.arena_list.header_ptr = hdr_off;
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Stage a CEL_MAP_ARENA at kSrcSlot from (key, val) CelValue pairs.
  // Lays out an ArenaMapHeader + contiguous 48-byte entry run.
  void StageArenaMap(
      const std::vector<std::pair<CelValue, CelValue>>& entries) {
    const uint32_t entries_off = arena_top_;
    for (const auto& [k, v] : entries) {
      f_.mem.WriteCelValue(arena_top_, k);
      f_.mem.WriteCelValue(arena_top_ + 24, v);
      arena_top_ += 48;
    }
    const uint32_t hdr_off = arena_top_;
    f_.mem.WriteU32(hdr_off + 0, static_cast<uint32_t>(entries.size()));
    f_.mem.WriteU32(hdr_off + 4, static_cast<uint32_t>(entries.size()));
    f_.mem.WriteU32(hdr_off + 8, entries_off);
    f_.mem.WriteU32(hdr_off + 12, 0);
    arena_top_ += 16;
    CelValue cv{};
    cv.kind = CEL_MAP_ARENA;
    cv.payload.arena_map.header_ptr = hdr_off;
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Build a standalone arena string CelValue (for embedding inside a
  // staged arena list / map) without overwriting kSrcSlot.
  CelValue MakeArenaString(absl::string_view s) {
    const uint32_t off = arena_top_;
    std::memcpy(f_.mem.data() + off, s.data(), s.size());
    arena_top_ += static_cast<uint32_t>(s.size());
    CelValue cv{};
    cv.kind = CEL_STRING;
    cv.payload.s.ptr = off;
    cv.payload.s.len = static_cast<uint32_t>(s.size());
    return cv;
  }

  // Drive CelSetFieldImpl pointing at `field_name`.  Returns the
  // trampoline's Status; on OK the caller reads back via outer().
  absl::Status SetField(uint32_t field_number, absl::string_view field_name) {
    f_.field_refs.push_back(
        FieldRefEntry{field_number, std::string(field_name)});
    const auto field_ref_id = static_cast<uint32_t>(f_.field_refs.size() - 1);
    return CelSetFieldImpl(kOuterMsgSlot, field_ref_id, kSrcSlot, f_.Ctx());
  }

  HostMsg3* outer() {
    return outer_;
  }

  // Read back the CelValue at the outer message slot.  Used to observe
  // the poison contract: a range-overflow field write overwrites this
  // slot with a CEL_ERROR in place rather than trapping.
  CelValue MsgSlot() const {
    return f_.mem.ReadCelValue(kOuterMsgSlot);
  }

 private:
  static constexpr uint32_t kOuterMsgSlot = 16;
  static constexpr uint32_t kSrcSlot = 48;
  Layer2Fixture f_;
  HostMsg3* outer_ = nullptr;
  uint32_t outer_slot_ = 0;
  // Scratch arena cursor — kArenaBase is 2048 in Layer2Fixture; the
  // FakeMemoryView is 4096 bytes, leaving room for small literals.
  uint32_t arena_top_ = 2048;
};

// Expected type_url for HostMsg3 / HostMsg2.
constexpr absl::string_view kHostMsg3Url =
    "type.googleapis.com/celwasm.testdata.HostMsg3";
constexpr absl::string_view kHostMsg2Url =
    "type.googleapis.com/celwasm.testdata.HostMsg2";

// (1) Singular Any pack: type_url has the src FQN suffix; value
// bytes round-trip via ParseFromString back to the original proto.
TEST(CelSetFieldAnyPackTest, SingularAnyPackRoundTrips) {
  PackHarness h;
  auto src = std::make_unique<HostMsg3>();
  src->set_i32(7);
  h.StageMessageSrc(std::move(src));
  ASSERT_THAT(h.SetField(/*field_number=*/30, "single_any"), IsOk());
  EXPECT_TRUE(h.outer()->has_single_any());
  EXPECT_EQ(h.outer()->single_any().type_url(), kHostMsg3Url);
  HostMsg3 round;
  ASSERT_TRUE(round.ParseFromString(h.outer()->single_any().value()));
  EXPECT_EQ(round.i32(), 7);
}

// (2) Empty payload: SerializeAsString on a default-constructed
// HostMsg3 returns the empty string; type_url is still set.
TEST(CelSetFieldAnyPackTest, SingularAnyEmptyPayload) {
  PackHarness h;
  h.StageMessageSrc(std::make_unique<HostMsg3>());
  ASSERT_THAT(h.SetField(30, "single_any"), IsOk());
  EXPECT_TRUE(h.outer()->has_single_any());
  EXPECT_EQ(h.outer()->single_any().type_url(), kHostMsg3Url);
  EXPECT_TRUE(h.outer()->single_any().value().empty());
}

// (3) Cross-syntax: pack a proto2 message into a proto3 outer's Any.
TEST(CelSetFieldAnyPackTest, CrossSyntaxProto2SrcPacks) {
  PackHarness h;
  h.StageMessageSrc(std::make_unique<HostMsg2>());
  ASSERT_THAT(h.SetField(30, "single_any"), IsOk());
  EXPECT_EQ(h.outer()->single_any().type_url(), kHostMsg2Url);
}

// (4) Non-Any descriptor match takes the CopyFrom branch — pin that
// the helper doesn't accidentally Any-pack a HostMsg3 into the
// HostMsg3-typed `inner` field.
TEST(CelSetFieldAnyPackTest, NonAnyDescriptorMatchCopies) {
  PackHarness h;
  auto src = std::make_unique<HostMsg3>();
  src->set_i32(9);
  h.StageMessageSrc(std::move(src));
  ASSERT_THAT(h.SetField(/*field_number=*/17, "inner"), IsOk());
  EXPECT_EQ(h.outer()->inner().i32(), 9);
  EXPECT_FALSE(h.outer()->has_single_any());  // no pack side-effect.
}

// (5) Descriptor-mismatched message-vs-message field set: src and
// dst differ AND dst is not Any AND dst is not a wrapper type.
// Post-M8.A this surfaces as `InvalidArgument` (embedder error,
// not codegen stub) — wrapper auto-wrap now handles the only
// legitimate descriptor-mismatch shape; everything else is wrong
// input.
TEST(CelSetFieldAnyPackTest, DescriptorMismatchOnSingularMessageInvalidArg) {
  PackHarness h;
  // Source is a HostMsg2 but the dst field is the HostMsg3-typed
  // `inner` — neither descriptor matches nor is the dst Any/wrapper.
  h.StageMessageSrc(std::make_unique<HostMsg2>());
  EXPECT_EQ(h.SetField(/*field_number=*/17, "inner").code(),
            absl::StatusCode::kInvalidArgument);
}

// (6) Null-clear ordering: CEL_NULL on a singular Any reaches the
// switch's null arm before the pack helper.  Pin that the field
// stays unset and type_url is empty.
TEST(CelSetFieldAnyPackTest, NullSrcClearsSingularAny) {
  PackHarness h;
  h.StageNullSrc();
  ASSERT_THAT(h.SetField(30, "single_any"), IsOk());
  EXPECT_FALSE(h.outer()->has_single_any());
  EXPECT_TRUE(h.outer()->single_any().type_url().empty());
}

// (7) Repeated-Any host-source pack: two heterogeneous elements
// (proto3 + proto2) each get their own type_url.
TEST(CelSetFieldAnyPackTest, RepeatedAnyHostSourcePacksTwoElements) {
  PackHarness h;
  auto e0 = std::make_unique<HostMsg3>();
  e0->set_i32(11);
  auto e1 = std::make_unique<HostMsg2>();
  std::vector<celwasm::Value> elems;
  elems.push_back(celwasm::Value::OwnedMessage(std::move(e0)));
  elems.push_back(celwasm::Value::OwnedMessage(std::move(e1)));
  h.StageHostListSrc(std::move(elems));
  ASSERT_THAT(h.SetField(/*field_number=*/31, "repeated_any"), IsOk());
  ASSERT_EQ(h.outer()->repeated_any_size(), 2);
  EXPECT_EQ(h.outer()->repeated_any(0).type_url(), kHostMsg3Url);
  EXPECT_EQ(h.outer()->repeated_any(1).type_url(), kHostMsg2Url);
  HostMsg3 round;
  ASSERT_TRUE(round.ParseFromString(h.outer()->repeated_any(0).value()));
  EXPECT_EQ(round.i32(), 11);
}

// (8) Map<string, Any> host-source pack: two entries, each with a
// distinct type_url.  The proto map_field iteration order isn't
// stable so we look up by key.
TEST(CelSetFieldAnyPackTest, MapStringToAnyHostSourcePacksTwoEntries) {
  PackHarness h;
  auto va = std::make_unique<HostMsg3>();
  va->set_i32(1);
  auto vb = std::make_unique<HostMsg2>();
  std::vector<std::pair<celwasm::Value, celwasm::Value>> entries;
  entries.emplace_back(celwasm::Value::String("a"),
                       celwasm::Value::OwnedMessage(std::move(va)));
  entries.emplace_back(celwasm::Value::String("b"),
                       celwasm::Value::OwnedMessage(std::move(vb)));
  h.StageHostMapSrc(std::move(entries));
  ASSERT_THAT(h.SetField(/*field_number=*/32, "map_str_to_any"), IsOk());
  ASSERT_EQ(h.outer()->map_str_to_any_size(), 2);
  EXPECT_EQ(h.outer()->map_str_to_any().at("a").type_url(), kHostMsg3Url);
  EXPECT_EQ(h.outer()->map_str_to_any().at("b").type_url(), kHostMsg2Url);
  HostMsg3 round;
  ASSERT_TRUE(
      round.ParseFromString(h.outer()->map_str_to_any().at("a").value()));
  EXPECT_EQ(round.i32(), 1);
}

// ═══════════ WKT pack arm — Duration/Timestamp/Value/Struct ═══════
//
// Inverse of the read-side WKT peelers: a non-message CelValue
// assigned to a Duration / Timestamp / Value / Struct / ListValue /
// Any field synthesises the matching well-known message.

// Singular Duration field ← CEL_DURATION payload (seconds, nanos).
TEST(CelSetFieldWktPackTest, SingularDurationFromCelDuration) {
  PackHarness h;
  h.StageDurTs(CEL_DURATION, /*seconds=*/123, /*nanos=*/0);
  ASSERT_THAT(h.SetField(/*field_number=*/33, "single_duration"), IsOk());
  EXPECT_TRUE(h.outer()->has_single_duration());
  EXPECT_EQ(h.outer()->single_duration().seconds(), 123);
  EXPECT_EQ(h.outer()->single_duration().nanos(), 0);
}

// Singular Timestamp field ← CEL_TIMESTAMP payload (seconds since
// epoch + nanos).
TEST(CelSetFieldWktPackTest, SingularTimestampFromCelTimestamp) {
  PackHarness h;
  h.StageDurTs(CEL_TIMESTAMP, /*seconds=*/1234567890, /*nanos=*/500);
  ASSERT_THAT(h.SetField(/*field_number=*/34, "single_timestamp"), IsOk());
  EXPECT_TRUE(h.outer()->has_single_timestamp());
  EXPECT_EQ(h.outer()->single_timestamp().seconds(), 1234567890);
  EXPECT_EQ(h.outer()->single_timestamp().nanos(), 500);
}

// Singular Value field ← CEL_STRING → string_value.
TEST(CelSetFieldWktPackTest, SingularValueFromString) {
  PackHarness h;
  h.StageString("foo");
  ASSERT_THAT(h.SetField(/*field_number=*/35, "single_value"), IsOk());
  EXPECT_EQ(h.outer()->single_value().kind_case(),
            google::protobuf::Value::kStringValue);
  EXPECT_EQ(h.outer()->single_value().string_value(), "foo");
}

// Singular Value field ← CEL_INT → number_value (ints become doubles
// in JSON Value).
TEST(CelSetFieldWktPackTest, SingularValueFromInt) {
  PackHarness h;
  CelValue cv{};
  cv.kind = CEL_INT;
  cv.payload.i = 42;
  h.StageScalar(cv);
  ASSERT_THAT(h.SetField(/*field_number=*/35, "single_value"), IsOk());
  EXPECT_EQ(h.outer()->single_value().kind_case(),
            google::protobuf::Value::kNumberValue);
  EXPECT_EQ(h.outer()->single_value().number_value(), 42.0);
}

// Singular Value field ← CEL_NULL → null_value.
TEST(CelSetFieldWktPackTest, SingularValueFromNull) {
  PackHarness h;
  CelValue cv{};
  cv.kind = CEL_NULL;
  h.StageScalar(cv);
  ASSERT_THAT(h.SetField(/*field_number=*/35, "single_value"), IsOk());
  EXPECT_EQ(h.outer()->single_value().kind_case(),
            google::protobuf::Value::kNullValue);
}

// Singular Value field ← CEL_BOOL → bool_value.
TEST(CelSetFieldWktPackTest, SingularValueFromBool) {
  PackHarness h;
  CelValue cv{};
  cv.kind = CEL_BOOL;
  cv.payload.b = 1;
  h.StageScalar(cv);
  ASSERT_THAT(h.SetField(/*field_number=*/35, "single_value"), IsOk());
  EXPECT_EQ(h.outer()->single_value().kind_case(),
            google::protobuf::Value::kBoolValue);
  EXPECT_TRUE(h.outer()->single_value().bool_value());
}

// Singular Struct field ← CEL_MAP_ARENA {'one':1, 'two':2}.  Each
// value packs into a number_value (ints → doubles).
TEST(CelSetFieldWktPackTest, SingularStructFromArenaMap) {
  PackHarness h;
  CelValue v1{};
  v1.kind = CEL_INT;
  v1.payload.i = 1;
  CelValue v2{};
  v2.kind = CEL_INT;
  v2.payload.i = 2;
  std::vector<std::pair<CelValue, CelValue>> entries;
  entries.emplace_back(h.MakeArenaString("one"), v1);
  entries.emplace_back(h.MakeArenaString("two"), v2);
  h.StageArenaMap(entries);
  ASSERT_THAT(h.SetField(/*field_number=*/36, "single_struct"), IsOk());
  ASSERT_EQ(h.outer()->single_struct().fields_size(), 2);
  EXPECT_EQ(h.outer()->single_struct().fields().at("one").number_value(), 1.0);
  EXPECT_EQ(h.outer()->single_struct().fields().at("two").number_value(), 2.0);
}

// Singular ListValue field ← CEL_LIST_ARENA [1, 'x'].  Heterogeneous
// elements each pack into a Value of the matching kind.
TEST(CelSetFieldWktPackTest, SingularListValueFromArenaList) {
  PackHarness h;
  CelValue e0{};
  e0.kind = CEL_INT;
  e0.payload.i = 1;
  h.StageArenaList({e0, h.MakeArenaString("x")});
  ASSERT_THAT(h.SetField(/*field_number=*/37, "single_list_value"), IsOk());
  ASSERT_EQ(h.outer()->single_list_value().values_size(), 2);
  EXPECT_EQ(h.outer()->single_list_value().values(0).number_value(), 1.0);
  EXPECT_EQ(h.outer()->single_list_value().values(1).string_value(), "x");
}

// Singular Value field ← CEL_LIST_ARENA → list_value (nested
// ListValue inside a Value).
TEST(CelSetFieldWktPackTest, SingularValueFromList) {
  PackHarness h;
  CelValue e0{};
  e0.kind = CEL_INT;
  e0.payload.i = 7;
  h.StageArenaList({e0});
  ASSERT_THAT(h.SetField(/*field_number=*/35, "single_value"), IsOk());
  EXPECT_EQ(h.outer()->single_value().kind_case(),
            google::protobuf::Value::kListValue);
  ASSERT_EQ(h.outer()->single_value().list_value().values_size(), 1);
  EXPECT_EQ(h.outer()->single_value().list_value().values(0).number_value(),
            7.0);
}

// Singular Any field ← CEL_STRING — scalar wraps into a
// google.protobuf.Value, then Any-packs that.  type_url names Value.
TEST(CelSetFieldWktPackTest, SingularAnyFromScalarWrapsViaValue) {
  PackHarness h;
  h.StageString("foo");
  ASSERT_THAT(h.SetField(/*field_number=*/30, "single_any"), IsOk());
  EXPECT_TRUE(h.outer()->has_single_any());
  EXPECT_EQ(h.outer()->single_any().type_url(),
            "type.googleapis.com/google.protobuf.Value");
  google::protobuf::Value round;
  ASSERT_TRUE(round.ParseFromString(h.outer()->single_any().value()));
  EXPECT_EQ(round.string_value(), "foo");
}

// Singular Any field ← CEL_INT — wraps into an Int64Value (NOT a JSON
// number) so the value round-trips as an int through the read-side
// wrapper peel.  type_url names Int64Value.
TEST(CelSetFieldWktPackTest, SingularAnyFromIntWrapsInt64Value) {
  PackHarness h;
  CelValue cv{};
  cv.kind = CEL_INT;
  cv.payload.i = 5;
  h.StageScalar(cv);
  ASSERT_THAT(h.SetField(/*field_number=*/30, "single_any"), IsOk());
  EXPECT_EQ(h.outer()->single_any().type_url(),
            "type.googleapis.com/google.protobuf.Int64Value");
  google::protobuf::Int64Value round;
  ASSERT_TRUE(round.ParseFromString(h.outer()->single_any().value()));
  EXPECT_EQ(round.value(), 5);
}

// Singular Any field ← CEL_BYTES — wraps into a BytesValue.
TEST(CelSetFieldWktPackTest, SingularAnyFromBytesWrapsBytesValue) {
  PackHarness h;
  CelValue cv = h.MakeArenaString("foo");
  cv.kind = CEL_BYTES;
  h.StageScalar(cv);
  ASSERT_THAT(h.SetField(/*field_number=*/30, "single_any"), IsOk());
  EXPECT_EQ(h.outer()->single_any().type_url(),
            "type.googleapis.com/google.protobuf.BytesValue");
  google::protobuf::BytesValue round;
  ASSERT_TRUE(round.ParseFromString(h.outer()->single_any().value()));
  EXPECT_EQ(round.value(), "foo");
}

// Repeated Timestamp field ← [timestamp(1), null]: the null element
// is PRUNED, leaving a single timestamp.
TEST(CelSetFieldWktPackTest, RepeatedTimestampNullElementPruned) {
  PackHarness h;
  CelValue ts{};
  ts.kind = CEL_TIMESTAMP;
  ts.payload.ts = CelDurTs{1, 0, 0};
  CelValue nul{};
  nul.kind = CEL_NULL;
  h.StageArenaList({ts, nul});
  ASSERT_THAT(h.SetField(/*field_number=*/38, "repeated_timestamp"), IsOk());
  ASSERT_EQ(h.outer()->repeated_timestamp_size(), 1);
  EXPECT_EQ(h.outer()->repeated_timestamp(0).seconds(), 1);
}

// Map<string, Timestamp> ← {'a': timestamp(1), 'b': null}: the
// null-valued entry is PRUNED.
TEST(CelSetFieldWktPackTest, MapTimestampNullValuePruned) {
  PackHarness h;
  CelValue ts{};
  ts.kind = CEL_TIMESTAMP;
  ts.payload.ts = CelDurTs{1, 0, 0};
  CelValue nul{};
  nul.kind = CEL_NULL;
  std::vector<std::pair<CelValue, CelValue>> entries;
  entries.emplace_back(h.MakeArenaString("a"), ts);
  entries.emplace_back(h.MakeArenaString("b"), nul);
  h.StageArenaMap(entries);
  ASSERT_THAT(h.SetField(/*field_number=*/39, "map_str_to_ts"), IsOk());
  ASSERT_EQ(h.outer()->map_str_to_ts_size(), 1);
  EXPECT_EQ(h.outer()->map_str_to_ts().at("a").seconds(), 1);
}

// ═══════════ Field-set poison contract (range overflow) ═══════════
//
// An out-of-range scalar / enum field assignment is a CEL value-level
// error (cel-cpp returns an ErrorValue), surfaced through the void
// cel_set_field ABI by poisoning the message slot in place with a
// CEL_ERROR{CEL_ERR_OVERFLOW} rather than trapping.  These tests pin
// each branch of that contract at the trampoline boundary.

// CEL_INT CelValue carrying `v` (poison-test source operand).
CelValue IntCv(int64_t v) {
  CelValue cv{};
  cv.kind = CEL_INT;
  cv.payload.i = v;
  return cv;
}

// An int32 field write past INT32_MAX poisons the slot (no trap) with
// the overflow error code; the helper reports OK so the poison rides
// out via the message slot.
TEST(CelSetFieldPoisonTest, Int32OverflowPoisonsSlot) {
  PackHarness h;
  h.StageScalar(IntCv(5000000000));  // > INT32_MAX
  ASSERT_THAT(h.SetField(/*field_number=*/2, "i32"), IsOk());
  const CelValue slot = h.MsgSlot();
  EXPECT_EQ(slot.kind, CEL_ERROR);
  EXPECT_EQ(slot.payload.err, CEL_ERR_OVERFLOW);
}

// Negative int32 overflow (< INT32_MIN) likewise poisons.
TEST(CelSetFieldPoisonTest, Int32UnderflowPoisonsSlot) {
  PackHarness h;
  h.StageScalar(IntCv(-7000000000));  // < INT32_MIN
  ASSERT_THAT(h.SetField(/*field_number=*/2, "i32"), IsOk());
  EXPECT_EQ(h.MsgSlot().kind, CEL_ERROR);
}

// An out-of-range enum field write poisons the same way — the enum arm
// narrows to int32 and shares the range check.
TEST(CelSetFieldPoisonTest, EnumOverflowPoisonsSlot) {
  PackHarness h;
  h.StageScalar(IntCv(5000000000));
  ASSERT_THAT(h.SetField(/*field_number=*/16, "kind"), IsOk());
  const CelValue slot = h.MsgSlot();
  EXPECT_EQ(slot.kind, CEL_ERROR);
  EXPECT_EQ(slot.payload.err, CEL_ERR_OVERFLOW);
}

// Once a slot is poisoned, a subsequent field set on the SAME message
// no-ops and leaves the error untouched — so the first overflow in a
// multi-field constructor propagates to the result.
TEST(CelSetFieldPoisonTest, SetOnPoisonedSlotIsNoOp) {
  PackHarness h;
  h.StageScalar(IntCv(5000000000));
  ASSERT_THAT(h.SetField(/*field_number=*/2, "i32"), IsOk());
  ASSERT_EQ(h.MsgSlot().kind, CEL_ERROR);
  // A perfectly valid second assignment must not resurrect the message.
  h.StageScalar(IntCv(7));
  ASSERT_THAT(h.SetField(/*field_number=*/3, "i64"), IsOk());
  EXPECT_EQ(h.MsgSlot().kind, CEL_ERROR);
}

// Positive control: an in-range value sets the field and leaves the
// slot a live CEL_MESSAGE (the poison path is not taken at the
// boundary INT32_MAX).
TEST(CelSetFieldPoisonTest, InRangeInt32SetsFieldNoPoison) {
  PackHarness h;
  h.StageScalar(IntCv(2147483647));  // INT32_MAX, in range
  ASSERT_THAT(h.SetField(/*field_number=*/2, "i32"), IsOk());
  EXPECT_EQ(h.MsgSlot().kind, CEL_MESSAGE);
  EXPECT_EQ(h.outer()->i32(), 2147483647);
}

// ═══════════ Set-field kind-mismatch arms ═══════════
//
// Every singular-scalar and repeated-element set arm range-checks the
// incoming wire kind before handing the payload to Reflection.  A
// type-checked CEL expression cannot reach these — `Foo{i32: 'x'}`
// fails in the checker — so they are only observable by driving the
// trampoline directly with a mismatched CelValue, which is what a
// host callback returning a value inconsistent with its declared
// return type would produce.

CelValue RawScalar(uint8_t kind, int64_t i) {
  CelValue cv{};
  cv.kind = kind;
  cv.payload.i = i;
  return cv;
}

CelValue RawDouble(double d) {
  CelValue cv{};
  cv.kind = CEL_DOUBLE;
  cv.payload.d = d;
  return cv;
}

struct ScalarMismatchCase {
  uint32_t field_number;
  absl::string_view field_name;
  CelValue wrong;
  absl::string_view want_type_token;
};

class ScalarKindMismatchTest
    : public testing::TestWithParam<ScalarMismatchCase> {};

TEST_P(ScalarKindMismatchTest, RejectsWrongWireKind) {
  const ScalarMismatchCase& c = GetParam();
  PackHarness h;
  h.StageScalar(c.wrong);
  const absl::Status s = h.SetField(c.field_number, c.field_name);
  EXPECT_THAT(s, StatusIs(absl::StatusCode::kInvalidArgument,
                          testing::AllOf(testing::HasSubstr(c.field_name),
                                         testing::HasSubstr(c.want_type_token),
                                         testing::HasSubstr("value kind is"))));
}

INSTANTIATE_TEST_SUITE_P(
    Singular, ScalarKindMismatchTest,
    testing::Values(
        ScalarMismatchCase{1, "b", RawScalar(CEL_INT, 1), "BOOL"},
        ScalarMismatchCase{2, "i32", RawScalar(CEL_UINT, 1), "INT32"},
        ScalarMismatchCase{3, "i64", RawScalar(CEL_BOOL, 1), "INT64"},
        ScalarMismatchCase{4, "u32", RawScalar(CEL_INT, 1), "UINT32"},
        ScalarMismatchCase{5, "u64", RawScalar(CEL_INT, 1), "UINT64"},
        ScalarMismatchCase{12, "f32", RawScalar(CEL_INT, 1), "FLOAT"},
        ScalarMismatchCase{13, "f64", RawScalar(CEL_INT, 1), "DOUBLE"},
        ScalarMismatchCase{16, "kind", RawScalar(CEL_UINT, 7), "ENUM"}));

// STRING / BYTES share CPPTYPE_STRING and are distinguished by
// `field.type()`, so each rejects the *other* CEL kind as well as the
// unrelated ones.
TEST(ScalarKindMismatchStringBytesTest, StringFieldRejectsBytes) {
  PackHarness h;
  CelValue by = h.MakeArenaString("ab");
  by.kind = CEL_BYTES;
  h.StageScalar(by);
  EXPECT_THAT(h.SetField(14, "s"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("is STRING but value kind is")));
}

TEST(ScalarKindMismatchStringBytesTest, BytesFieldRejectsString) {
  PackHarness h;
  h.StageScalar(h.MakeArenaString("ab"));
  EXPECT_THAT(h.SetField(15, "by"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("is BYTES but value kind is")));
}

// Repeated element append checks the element kind per cpp_type; the
// arms live in a separate helper from the singular ones, so each needs
// its own row.
TEST(RepeatedAppendMismatchTest, RejectsWrongElementKind) {
  struct Row {
    uint32_t number;
    absl::string_view name;
    CelValue elem;
    absl::string_view token;
  };
  const Row rows[] = {
      {18, "rep_i32", RawScalar(CEL_UINT, 1), "INT32"},
      {25, "rep_b", RawScalar(CEL_INT, 1), "BOOL"},
      {26, "rep_f64", RawScalar(CEL_INT, 1), "DOUBLE"},
  };
  for (const Row& r : rows) {
    PackHarness h;
    h.StageArenaList({r.elem});
    EXPECT_THAT(h.SetField(r.number, r.name),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         testing::HasSubstr(r.token)))
        << "field " << r.name;
  }
}

TEST(RepeatedAppendMismatchTest, StringElementRejectsNonString) {
  PackHarness h;
  h.StageArenaList({RawScalar(CEL_INT, 1)});
  EXPECT_THAT(h.SetField(24, "rep_s"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("STRING/BYTES")));
}

TEST(RepeatedAppendMismatchTest, MessageElementRejectsNonMessage) {
  PackHarness h;
  h.StageArenaList({RawDouble(1.5)});
  EXPECT_THAT(h.SetField(27, "rep_msg"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("element kind=")));
}

// ═══════════ WKT wrapper fields ═══════════
//
// CEL models `google.protobuf.*Value` fields as nullable scalars
// (`doc/langdef.md`, "Wrapper Types").  On the write side a field-init
// hands the trampoline a bare scalar and `SetWrapperFieldFromScalar`
// synthesises the wrapper message; on the read side
// `UnpackWrapperMessage` collapses a set wrapper back to its inner
// scalar.  One row per wrapper cpp_type covers both directions plus
// the inner-kind guard.

namespace {

struct WrapperCase {
  uint32_t field_number;
  absl::string_view field_name;
  CelValue good;
  CelValue bad;
  absl::string_view want_expected_kind;
};

class WrapperFieldTest : public testing::TestWithParam<WrapperCase> {};

}  // namespace

TEST_P(WrapperFieldTest, ScalarSetSynthesisesWrapperMessage) {
  const WrapperCase& c = GetParam();
  PackHarness h;
  h.StageScalar(c.good);
  ASSERT_THAT(h.SetField(c.field_number, c.field_name), IsOk());
  const google::protobuf::Message& outer = *h.outer();
  const google::protobuf::FieldDescriptor* fd =
      outer.GetDescriptor()->FindFieldByNumber(
          static_cast<int>(c.field_number));
  ASSERT_NE(fd, nullptr);
  EXPECT_TRUE(outer.GetReflection()->HasField(outer, fd))
      << "wrapper field `" << c.field_name << "` not set";
}

TEST_P(WrapperFieldTest, WrongInnerKindIsRejected) {
  const WrapperCase& c = GetParam();
  PackHarness h;
  h.StageScalar(c.bad);
  EXPECT_THAT(
      h.SetField(c.field_number, c.field_name),
      StatusIs(absl::StatusCode::kInvalidArgument,
               testing::AllOf(testing::HasSubstr("SetWrapperInnerValue"),
                              testing::HasSubstr(c.want_expected_kind))));
}

INSTANTIATE_TEST_SUITE_P(
    EveryInnerCppType, WrapperFieldTest,
    testing::Values(WrapperCase{40, "wrap_b", RawScalar(CEL_BOOL, 1),
                                RawScalar(CEL_INT, 1), "CEL_BOOL"},
                    WrapperCase{41, "wrap_i32", RawScalar(CEL_INT, 7),
                                RawScalar(CEL_UINT, 7), "CEL_INT"},
                    WrapperCase{42, "wrap_i64", RawScalar(CEL_INT, 7),
                                RawScalar(CEL_BOOL, 1), "CEL_INT"},
                    WrapperCase{43, "wrap_u32", RawScalar(CEL_UINT, 7),
                                RawScalar(CEL_INT, 7), "CEL_UINT"},
                    WrapperCase{44, "wrap_u64", RawScalar(CEL_UINT, 7),
                                RawScalar(CEL_INT, 7), "CEL_UINT"},
                    WrapperCase{45, "wrap_f32", RawDouble(1.5),
                                RawScalar(CEL_INT, 1), "CEL_DOUBLE"},
                    WrapperCase{46, "wrap_f64", RawDouble(1.5),
                                RawScalar(CEL_INT, 1), "CEL_DOUBLE"}));

// STRING / BYTES wrappers need an arena-resident payload, so they get
// focused cases rather than a table row.
TEST(WrapperFieldStringBytesTest, StringWrapperRoundTrips) {
  PackHarness h;
  h.StageScalar(h.MakeArenaString("hi"));
  ASSERT_THAT(h.SetField(47, "wrap_s"), IsOk());
  EXPECT_EQ(h.outer()->wrap_s().value(), "hi");
}

TEST(WrapperFieldStringBytesTest, BytesWrapperRoundTrips) {
  PackHarness h;
  CelValue by = h.MakeArenaString("ab");
  by.kind = CEL_BYTES;
  h.StageScalar(by);
  ASSERT_THAT(h.SetField(48, "wrap_by"), IsOk());
  EXPECT_EQ(h.outer()->wrap_by().value(), "ab");
}

TEST(WrapperFieldStringBytesTest, StringWrapperRejectsBytes) {
  PackHarness h;
  CelValue by = h.MakeArenaString("ab");
  by.kind = CEL_BYTES;
  h.StageScalar(by);
  EXPECT_THAT(h.SetField(47, "wrap_s"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("CEL_STRING")));
}

TEST(WrapperFieldStringBytesTest, BytesWrapperRejectsString) {
  PackHarness h;
  h.StageScalar(h.MakeArenaString("ab"));
  EXPECT_THAT(h.SetField(48, "wrap_by"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("CEL_BYTES")));
}

// ═══════════ Nested singular-message set guards ═══════════
//
// `SetNestedSingularMessage` validates the incoming wire value before
// touching Reflection: it must be a CEL_MESSAGE, its externref must
// resolve, and the backing must carry a real proto.  A type-checked
// field-init cannot violate any of those, so each is staged directly.

TEST(NestedMessageSetGuardTest, NonMessageValueIsRejected) {
  PackHarness h;
  CelValue i{};
  i.kind = CEL_INT;
  i.payload.i = 1;
  h.StageScalar(i);
  EXPECT_THAT(h.SetField(17, "inner"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("is MESSAGE but value kind is")));
}

TEST(NestedMessageSetGuardTest, UninternedSourceIsRejected) {
  PackHarness h;
  CelValue cv{};
  cv.kind = CEL_MESSAGE;
  cv.payload.msg_slot = 999;
  h.StageScalar(cv);
  EXPECT_THAT(h.SetField(17, "inner"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("has no externref entry")));
}

TEST(NestedMessageSetGuardTest, BackingWithoutAProtoIsRejected) {
  PackHarness h;
  h.StageBackingSrc(std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{{"x", 1}}));
  EXPECT_THAT(h.SetField(17, "inner"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("has no proto message")));
}

// Repeated-message element guards, the arena-list path.  Each element
// must resolve through the externref table to a backing that carries a
// real proto; batch-27's rows covered only the element-kind arm.
TEST(RepeatedMessageElementGuardTest, UninternedElementIsRejected) {
  PackHarness h;
  CelValue elem{};
  elem.kind = CEL_MESSAGE;
  elem.payload.msg_slot = 999;
  h.StageArenaList({elem});
  EXPECT_THAT(h.SetField(27, "rep_msg"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("element has no externref entry")));
}

TEST(RepeatedMessageElementGuardTest, ElementBackingWithoutAProtoIsRejected) {
  PackHarness h;
  CelValue elem = h.InternBackingElement(std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{{"x", 1}}));
  h.StageArenaList({elem});
  EXPECT_THAT(
      h.SetField(27, "rep_msg"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               testing::HasSubstr("element backing has no proto message")));
}

// The host-list repeated-message path has its own copies of the
// element guards (the arena-list path's are covered above): the element
// must be a kMessage, and its backing must carry a real proto.  A
// type-checked field-init cannot violate either.
TEST(HostListRepeatedMessageGuardTest, NonMessageElementIsRejected) {
  PackHarness h;
  h.StageHostListSrc({celwasm::Value::Int(1)});
  EXPECT_THAT(h.SetField(27, "rep_msg"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("element kind != kMessage")));
}

TEST(HostListRepeatedMessageGuardTest, ElementBackingWithoutAProtoIsRejected) {
  PackHarness h;
  h.StageHostListSrc(
      {celwasm::Value::HostMessage(std::make_shared<JsonLikeBacking>(
          absl::flat_hash_map<std::string, int64_t>{{"x", 1}}))});
  EXPECT_THAT(h.SetField(27, "rep_msg"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("backing has no proto message")));
}

}  // namespace
}  // namespace celwasm
