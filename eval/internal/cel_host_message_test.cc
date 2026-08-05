// cel_host message-family Layer-2 tests — the CelGetFieldImpl /
// CelHasFieldImpl trampoline dispatch (absorption, unknown-pattern
// match, aliasing, marshal, per-site resolved-field cache), the
// CelMessageIsZeroImpl probe, and the WKT literal-unwrap bridges.

#include "eval/internal/cel_host_message.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "eval/error.h"
#include "eval/internal/cel_host_test_harness.h"
#include "eval/value.h"
#include "google/protobuf/duration.pb.h"
#include "google/protobuf/message.h"
#include "google/protobuf/timestamp.pb.h"
#include "google/protobuf/wrappers.pb.h"
#include "gtest/gtest.h"
#include "shared/type.h"
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

// ═══════════ Layer 2 — absorption ═══════════

TEST(Layer2AbsorptionTest, UnknownInputPropagates) {
  Layer2Fixture f;
  f.field_refs.push_back(FieldRefEntry{1, "any"});
  CelValue in{};
  in.kind = CEL_UNKNOWN;
  in.payload.unk = 77;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, in);

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_UNKNOWN);
  EXPECT_EQ(out.payload.unk, 77u);
}

TEST(Layer2AbsorptionTest, ErrorInputPropagates) {
  Layer2Fixture f;
  f.field_refs.push_back(FieldRefEntry{1, "any"});
  CelValue in{};
  in.kind = CEL_ERROR;
  in.payload.err = static_cast<uint32_t>(celwasm::ErrorCode::kOverflow);
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, in);

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_ERROR);
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(celwasm::ErrorCode::kOverflow));
}

TEST(Layer2AbsorptionTest, NonMessageInputYieldsTypeMismatch) {
  Layer2Fixture f;
  f.field_refs.push_back(FieldRefEntry{1, "any"});
  CelValue in{};
  in.kind = CEL_INT;
  in.payload.i = 42;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, in);

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_ERROR);
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(celwasm::ErrorCode::kTypeMismatch));
}

TEST(Layer2AbsorptionTest, OutOfRangeFieldRefIdYieldsFieldNotFound) {
  Layer2Fixture f;  // field_refs has only the sentinel
  HostMsg3 m;
  f.BindMessage(std::make_shared<ProtoBacking>(&m), 0, "");
  // Dispatch with field_ref_id = 5 (OOR).
  auto status =
      CelGetFieldImpl(Layer2Fixture::kOutSlot, Layer2Fixture::kMsgSlot,
                      /*field_ref_id=*/5, 0, f.Ctx());
  ASSERT_THAT(status, IsOk());
  const CelValue out = f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
  EXPECT_EQ(out.kind, CEL_ERROR);
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(celwasm::ErrorCode::kFieldNotFound));
}

TEST(Layer2AbsorptionTest, InvalidExternrefSlotYieldsHostAdapterError) {
  Layer2Fixture f;
  f.field_refs.push_back(FieldRefEntry{1, "any"});
  CelValue cv{};
  cv.kind = CEL_MESSAGE;
  cv.payload.msg_slot = 9999;  // never interned
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_ERROR);
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(celwasm::ErrorCode::kHostAdapterError));
}

// ═══════════ CelMessageIsZeroImpl — proto zero-value probe ═══════════
//
// cel-cpp parity: `ParsedMessageValue::IsZeroValue()`
// (third_party/cel-cpp/common/values/parsed_message_value.cc:78) — a
// message is zero iff its unknown-field set is empty AND
// `Reflection::ListFields` returns no set fields.  Expected verdicts
// pinned empirically by `testdata/cel_cpp_oracle_test.cc`
// (OptionalOfNonZeroValueMessage.*).

// Helper: intern `backing`, stage a CEL_MESSAGE at kMsgSlot, run the
// probe, return the out CelValue.
CelValue RunMessageIsZero(
    Layer2Fixture& f,
    const std::shared_ptr<const HostMessageBacking>& backing) {
  const uint32_t slot = f.refs.Intern(backing);
  CelValue cv{};
  cv.kind = CEL_MESSAGE;
  cv.payload.msg_slot = slot;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  auto status = CelMessageIsZeroImpl(Layer2Fixture::kOutSlot,
                                     Layer2Fixture::kMsgSlot, f.Ctx());
  EXPECT_THAT(status, IsOk());
  return f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
}

TEST(CelMessageIsZeroTest, DefaultConstructedMessageIsZero) {
  HostMsg3 m;
  Layer2Fixture f;
  const CelValue out = RunMessageIsZero(f, std::make_shared<ProtoBacking>(&m));
  ASSERT_EQ(out.kind, CEL_BOOL);
  EXPECT_EQ(out.payload.b, 1);
}

TEST(CelMessageIsZeroTest, ProtoTwoDefaultConstructedMessageIsZero) {
  // Proto2 explicit-presence semantics: unset fields with non-zero
  // defaults still leave ListFields empty → zero.
  HostMsg2 m;
  Layer2Fixture f;
  const CelValue out = RunMessageIsZero(f, std::make_shared<ProtoBacking>(&m));
  ASSERT_EQ(out.kind, CEL_BOOL);
  EXPECT_EQ(out.payload.b, 1);
}

TEST(CelMessageIsZeroTest, SetFieldMakesNonZero) {
  HostMsg3 m;
  m.set_i64(1);
  Layer2Fixture f;
  const CelValue out = RunMessageIsZero(f, std::make_shared<ProtoBacking>(&m));
  ASSERT_EQ(out.kind, CEL_BOOL);
  EXPECT_EQ(out.payload.b, 0);
}

TEST(CelMessageIsZeroTest, FieldSetToProto3DefaultStaysZero) {
  // Proto3 implicit presence: setting a scalar to its default leaves
  // ListFields empty — the message is STILL zero.  This pins the
  // ListFields semantics (vs. a naive per-field value comparison).
  HostMsg3 m;
  m.set_i64(0);
  Layer2Fixture f;
  const CelValue out = RunMessageIsZero(f, std::make_shared<ProtoBacking>(&m));
  ASSERT_EQ(out.kind, CEL_BOOL);
  EXPECT_EQ(out.payload.b, 1);
}

TEST(CelMessageIsZeroTest, OnlyUnknownFieldsMakesNonZero) {
  // cel-cpp checks the unknown-field set FIRST: a message whose only
  // content is an unknown field is non-zero even though ListFields is
  // empty (parsed_message_value.cc:79-81).
  HostMsg3 m;
  HostMsg3::GetReflection()->MutableUnknownFields(&m)->AddVarint(
      /*number=*/9999, 1);
  Layer2Fixture f;
  const CelValue out = RunMessageIsZero(f, std::make_shared<ProtoBacking>(&m));
  ASSERT_EQ(out.kind, CEL_BOOL);
  EXPECT_EQ(out.payload.b, 0);
}

TEST(CelMessageIsZeroTest, OwnedProtoBackingParticipates) {
  // The proto-literal construction path (`TestAllTypes{...}`) interns
  // an OwnedProtoBacking — the probe must reach its message() too.
  auto owned = std::make_unique<HostMsg3>();
  Layer2Fixture f;
  const CelValue out = RunMessageIsZero(
      f, std::make_shared<OwnedProtoBacking>(std::move(owned)));
  ASSERT_EQ(out.kind, CEL_BOOL);
  EXPECT_EQ(out.payload.b, 1);
}

TEST(CelMessageIsZeroTest, UnknownOperandPropagates) {
  Layer2Fixture f;
  CelValue in{};
  in.kind = CEL_UNKNOWN;
  in.payload.unk = 5;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, in);
  ASSERT_THAT(CelMessageIsZeroImpl(Layer2Fixture::kOutSlot,
                                   Layer2Fixture::kMsgSlot, f.Ctx()),
              IsOk());
  const CelValue out = f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
  EXPECT_EQ(out.kind, CEL_UNKNOWN);
  EXPECT_EQ(out.payload.unk, 5u);
}

TEST(CelMessageIsZeroTest, ErrorOperandPropagates) {
  Layer2Fixture f;
  CelValue in{};
  in.kind = CEL_ERROR;
  in.payload.err = static_cast<uint32_t>(celwasm::ErrorCode::kOverflow);
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, in);
  ASSERT_THAT(CelMessageIsZeroImpl(Layer2Fixture::kOutSlot,
                                   Layer2Fixture::kMsgSlot, f.Ctx()),
              IsOk());
  const CelValue out = f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
  EXPECT_EQ(out.kind, CEL_ERROR);
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(celwasm::ErrorCode::kOverflow));
}

TEST(CelMessageIsZeroTest, NonMessageOperandIsTypeMismatch) {
  Layer2Fixture f;
  CelValue in{};
  in.kind = CEL_INT;
  in.payload.i = 7;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, in);
  ASSERT_THAT(CelMessageIsZeroImpl(Layer2Fixture::kOutSlot,
                                   Layer2Fixture::kMsgSlot, f.Ctx()),
              IsOk());
  const CelValue out = f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
  EXPECT_EQ(out.kind, CEL_ERROR);
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(celwasm::ErrorCode::kTypeMismatch));
}

TEST(CelMessageIsZeroTest, UnmappedSlotIsHostAdapterError) {
  Layer2Fixture f;
  CelValue in{};
  in.kind = CEL_MESSAGE;
  in.payload.msg_slot = 9999;  // never interned
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, in);
  ASSERT_THAT(CelMessageIsZeroImpl(Layer2Fixture::kOutSlot,
                                   Layer2Fixture::kMsgSlot, f.Ctx()),
              IsOk());
  const CelValue out = f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
  EXPECT_EQ(out.kind, CEL_ERROR);
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(celwasm::ErrorCode::kHostAdapterError));
}

TEST(CelMessageIsZeroTest, NonProtoBackingIsHostAdapterError) {
  // A custom backing has no reflection to walk and no zero-value
  // hook — clean poison, never a wrong "zero" verdict.
  Layer2Fixture f;
  const CelValue out = RunMessageIsZero(
      f, std::make_shared<JsonLikeBacking>(
             absl::flat_hash_map<std::string, int64_t>{{"x", 1}}));
  EXPECT_EQ(out.kind, CEL_ERROR);
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(celwasm::ErrorCode::kHostAdapterError));
}

// ═══════════ Layer 2 — happy paths ═══════════

TEST(Layer2DispatchTest, ScalarIntField) {
  Customer c;
  c.set_age(30);
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 2, "age");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_INT);
  EXPECT_EQ(out.payload.i, 30);
}

TEST(Layer2DispatchTest, ScalarBoolField) {
  Customer c;
  c.set_is_premium(true);
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 7, "is_premium");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_BOOL);
  EXPECT_EQ(out.payload.b, 1);
}

TEST(Layer2DispatchTest, ScalarDoubleField) {
  Customer c;
  c.set_credit_score(725.5);
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 6, "credit_score");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_DOUBLE);
  EXPECT_DOUBLE_EQ(out.payload.d, 725.5);
}

TEST(Layer2DispatchTest, ScalarUintField) {
  HostMsg3 m;
  m.set_u64(1'000'000uLL);
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&m), 5, "u64");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_UINT);
  EXPECT_EQ(out.payload.u, 1'000'000uLL);
}

// A float field widens to a CEL double (langdef: CEL has no float32).
TEST(Layer2DispatchTest, ScalarFloatFieldWidensToDouble) {
  HostMsg3 m;
  m.set_f32(3.5f);  // exactly representable, so == holds after widening
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&m), 12, "f32");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_DOUBLE);
  EXPECT_DOUBLE_EQ(out.payload.d, 3.5);
}

// An enum field surfaces as its int value (langdef §2.4.7).
TEST(Layer2DispatchTest, ScalarEnumFieldReadsAsInt) {
  HostMsg3 m;
  m.set_kind(HostMsg3::KIND_SEVEN);
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&m), 16, "kind");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_INT);
  EXPECT_EQ(out.payload.i, 7);
}

TEST(Layer2DispatchTest, StringFieldAllocatesArenaSpan) {
  Customer c;
  c.set_name("Ada");
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 1, "name");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_STRING);
  EXPECT_EQ(f.mem.ReadSpan(out.payload.s.ptr, out.payload.s.len), "Ada");
  EXPECT_GE(out.payload.s.ptr, Layer2Fixture::kArenaBase);
}

TEST(Layer2DispatchTest, BytesFieldAllocatesArenaSpan) {
  Customer c;
  c.set_session_token(std::string("\xde\xad\xbe\xef", 4));
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 8, "session_token");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_BYTES);
  EXPECT_EQ(f.mem.ReadSpan(out.payload.s.ptr, out.payload.s.len),
            absl::string_view("\xde\xad\xbe\xef", 4));
}

TEST(Layer2DispatchTest, NestedMessageInternsSubBacking) {
  Customer c;
  c.mutable_billing_address()->set_city("Seattle");
  Layer2Fixture f;
  const uint32_t parent_slot =
      f.BindMessage(std::make_shared<ProtoBacking>(&c), 9, "billing_address");

  const CelValue out = f.Get();
  ASSERT_EQ(out.kind, CEL_MESSAGE);
  EXPECT_NE(out.payload.msg_slot, 0u);
  EXPECT_NE(out.payload.msg_slot, parent_slot);

  // Second hop: feed sub_slot back in as msg_slot, read city.
  CelValue sub{};
  sub.kind = CEL_MESSAGE;
  sub.payload.msg_slot = out.payload.msg_slot;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, sub);
  f.field_refs.clear();
  f.field_refs.push_back(FieldRefEntry{});
  f.field_refs.push_back(FieldRefEntry{1, "city"});

  const CelValue city = f.Get();
  EXPECT_EQ(city.kind, CEL_STRING);
  EXPECT_EQ(f.mem.ReadSpan(city.payload.s.ptr, city.payload.s.len), "Seattle");
}

// M4.G flipped: REPEATED selects round-trip as CEL_LIST_HOST with
// the ProtoList interned into the externref table.  Element-level
// coverage lives in cel_list_at_impl_test.cc; this test pins the
// kSelect-returns-HostList contract end-to-end through Layer 2.
TEST(Layer2DispatchTest, RepeatedFieldSurfacesAsHostList) {
  HostMsg3 m;
  m.add_rep_i32(1);
  m.add_rep_i32(2);
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&m), 18, "rep_i32");

  const CelValue out = f.Get();
  ASSERT_EQ(out.kind, CEL_LIST_HOST);
  EXPECT_NE(out.payload.ref_slot, 0u);
  EXPECT_NE(f.refs.LookupList(out.payload.ref_slot), nullptr);
}

// ═══════════ Layer 2 — aliasing ═══════════

TEST(Layer2AliasingTest, MsgSlotEqualsOutSlot) {
  // Layer 2 reads into a local before dispatch, so an aliased
  // (msg_slot == out_slot) call must still produce the right answer.
  Customer c;
  c.set_age(99);
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 2, "age");

  const uint32_t shared = Layer2Fixture::kMsgSlot;
  auto status = CelGetFieldImpl(shared, shared, 1, 0, f.Ctx());
  ASSERT_THAT(status, IsOk());
  const CelValue out = f.mem.ReadCelValue(shared);
  EXPECT_EQ(out.kind, CEL_INT);
  EXPECT_EQ(out.payload.i, 99);
}

// ═══════════ Layer 2 — unknown-pattern match ═══════════

TEST(Layer2UnknownPatternTest, EmptyPatternSetDoesNotConsultAttributes) {
  // With empty unknown_patterns, any attribute_id (even OOR) must
  // behave like 0 — the matcher never looks up attributes[].
  Customer c;
  c.set_name("Ada");
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 1, "name");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_STRING);
}

TEST(Layer2UnknownPatternTest, MatchingPatternAbsorbs) {
  Customer c;
  c.set_name("Ada");
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 1, "name");
  f.attributes.push_back(AttributeEntry{});  // sentinel
  f.attributes.push_back(AttributeEntry{/*root=*/"c", /*quals=*/{}});
  auto pat = celwasm::AttributePattern::Parse("c.name");
  ASSERT_THAT(pat, IsOk());
  f.unknown_patterns.push_back(*std::move(pat));

  const CelValue out = f.Get(/*attribute_id=*/1);
  EXPECT_EQ(out.kind, CEL_UNKNOWN);
  // §8.2 descriptor wire: payload.unk points at a 1-element
  // UnknownSet descriptor carrying the matched attribute id — never
  // the raw id itself.
  EXPECT_THAT(test::ReadUnknownIds(f.mem, out), ::testing::ElementsAre(1u));
}

TEST(Layer2UnknownPatternTest, NonMatchingPatternFallsThrough) {
  Customer c;
  c.set_name("Ada");
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 1, "name");
  f.attributes.push_back(AttributeEntry{});
  f.attributes.push_back(AttributeEntry{"c", {}});
  auto pat = celwasm::AttributePattern::Parse("c.age");  // different field
  ASSERT_THAT(pat, IsOk());
  f.unknown_patterns.push_back(*std::move(pat));

  const CelValue out = f.Get(/*attribute_id=*/1);
  EXPECT_EQ(out.kind, CEL_STRING);
}

TEST(Layer2UnknownPatternTest, WildcardMatchesConcreteSelect) {
  Customer c;
  c.mutable_billing_address()->set_city("Seattle");
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 9, "billing_address");
  f.attributes.push_back(AttributeEntry{});
  f.attributes.push_back(AttributeEntry{"c", {}});
  auto pat = celwasm::AttributePattern::Parse("c.*");
  ASSERT_THAT(pat, IsOk());
  f.unknown_patterns.push_back(*std::move(pat));

  EXPECT_EQ(f.Get(/*attribute_id=*/1).kind, CEL_UNKNOWN);
}

// ═══════════ Layer 2 — has() ═══════════

TEST(Layer2HasFieldTest, SetAndUnset) {
  Customer set;
  set.set_name("Ada");
  Layer2Fixture f_set;
  f_set.BindMessage(std::make_shared<ProtoBacking>(&set), 1, "name");
  EXPECT_EQ(f_set.Has().payload.b, 1);

  Customer unset;
  Layer2Fixture f_unset;
  f_unset.BindMessage(std::make_shared<ProtoBacking>(&unset), 1, "name");
  EXPECT_EQ(f_unset.Has().payload.b, 0);
}

TEST(Layer2HasFieldTest, UnknownInputShortCircuits) {
  Layer2Fixture f;
  f.field_refs.push_back(FieldRefEntry{1, "any"});
  CelValue in{};
  in.kind = CEL_UNKNOWN;
  in.payload.unk = 42;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, in);

  const CelValue out = f.Has();
  EXPECT_EQ(out.kind, CEL_UNKNOWN);
  EXPECT_EQ(out.payload.unk, 42u);
}

// ═══════════ Layer 2 — per-access-site resolved-field cache ═══════════
//
// `CelGetFieldImpl` / `CelHasFieldImpl` resolve proto fields through
// the single-entry cache on `FieldRefEntry::resolved` (keyed by the
// owning message's `Descriptor*` — see cel_host.h).  These pin the
// cache contract: a repeated read at the same site hits the cache and
// stays correct against a mutated message, and a message of a
// DIFFERENT descriptor arriving at the same site invalidates and
// re-resolves — the dynamic-pool correctness story (key by Descriptor
// pointer, never by field name/number alone).

TEST(Layer2FieldCacheTest, RepeatedReadsHitCacheAndStayCorrect) {
  Layer2Fixture f;
  HostMsg3 m;
  m.set_i64(41);
  f.BindMessage(std::make_shared<ProtoBacking>(&m), 3, "i64");

  const CelValue first = f.Get();
  EXPECT_EQ(first.kind, CEL_INT);
  EXPECT_EQ(first.payload.i, 41);
  // First read primed the site cache for HostMsg3's descriptor.
  EXPECT_EQ(f.field_refs[1].resolved.owner, HostMsg3::descriptor());
  ASSERT_NE(f.field_refs[1].resolved.field, nullptr);
  EXPECT_EQ(f.field_refs[1].resolved.read_class, ProtoFieldReadClass::kScalar);

  // Second read takes the cache-hit path and observes fresh data.
  m.set_i64(42);
  const CelValue second = f.Get();
  EXPECT_EQ(second.kind, CEL_INT);
  EXPECT_EQ(second.payload.i, 42);
}

TEST(Layer2FieldCacheTest, DescriptorChangeInvalidatesSiteCache) {
  Layer2Fixture f;
  // HostMsg3.b (bool) and Customer.name (string) are both field
  // number 1 — the same site entry resolves to different
  // FieldDescriptors with different read classes depending on the
  // bound message's descriptor.
  HostMsg3 a;
  a.set_b(true);
  f.BindMessage(std::make_shared<ProtoBacking>(&a), 1, "b");
  const CelValue first = f.Get();
  EXPECT_EQ(first.kind, CEL_BOOL);
  EXPECT_EQ(first.payload.b, 1u);
  EXPECT_EQ(f.field_refs[1].resolved.owner, HostMsg3::descriptor());

  // Rebind a Customer at the SAME msg slot + field_ref_id.  The
  // pointer-identity key must miss and re-resolve field 1 on
  // Customer's descriptor (the by-number path wins; the stale name
  // "b" is never consulted).
  Customer c;
  c.set_name("Ada");
  const uint32_t slot = f.refs.Intern(std::make_shared<ProtoBacking>(&c));
  CelValue cv{};
  cv.kind = CEL_MESSAGE;
  cv.payload.msg_slot = slot;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);

  const CelValue second = f.Get();
  EXPECT_EQ(second.kind, CEL_STRING);
  EXPECT_EQ(f.mem.ReadSpan(second.payload.s.ptr, second.payload.s.len), "Ada");
  EXPECT_EQ(f.field_refs[1].resolved.owner, Customer::descriptor());
  EXPECT_EQ(f.field_refs[1].resolved.read_class, ProtoFieldReadClass::kScalar);
}

TEST(Layer2FieldCacheTest, HasFieldSharesTheSiteCache) {
  Layer2Fixture f;
  HostMsg3 m;
  m.set_i64(7);
  f.BindMessage(std::make_shared<ProtoBacking>(&m), 3, "i64");

  EXPECT_EQ(f.Has().payload.b, 1);
  EXPECT_EQ(f.field_refs[1].resolved.owner, HostMsg3::descriptor());
  // A Get after the Has reuses the same primed entry.
  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_INT);
  EXPECT_EQ(out.payload.i, 7);
}

TEST(Layer2FieldCacheTest, UnresolvableFieldIsNotCached) {
  Layer2Fixture f;
  HostMsg3 m;
  f.BindMessage(std::make_shared<ProtoBacking>(&m), 0, "no_such_field");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_ERROR);
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(celwasm::ErrorCode::kFieldNotFound));
  // Negative resolutions stay uncached — a dynamic pool can gain
  // extensions between reads.
  EXPECT_EQ(f.field_refs[1].resolved.owner, nullptr);
}

// Message-class classification reaches the WKT peels through the
// sub-field descriptors cached at classification time; pin one
// wrapper and one timestamp read through the trampoline path
// (Layer-1 coverage for every WKT shape lives in the
// ProtoBackingReadFieldTest section + e2e wrapper rows).
TEST(Layer2FieldCacheTest, WrapperFieldClassifiedReadPeelsInnerScalar) {
  Layer2Fixture f;
  ::cel::expr::conformance::proto2::TestAllTypes m;
  m.mutable_single_int64_wrapper()->set_value(99);
  f.BindMessage(std::make_shared<ProtoBacking>(&m), 105,
                "single_int64_wrapper");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_INT);
  EXPECT_EQ(out.payload.i, 99);
  EXPECT_EQ(f.field_refs[1].resolved.read_class,
            ProtoFieldReadClass::kMessageWrapper);
  // The inner `value` field descriptor was cached at classify time.
  ASSERT_NE(f.field_refs[1].resolved.sub_field1, nullptr);
  EXPECT_EQ(f.field_refs[1].resolved.sub_field1->number(), 1);
}

TEST(Layer2FieldCacheTest, TimestampFieldClassifiedReadPeelsSecondsNanos) {
  Layer2Fixture f;
  HostMsg3 m;
  m.mutable_single_timestamp()->set_seconds(1234);
  m.mutable_single_timestamp()->set_nanos(567);
  f.BindMessage(std::make_shared<ProtoBacking>(&m), 34, "single_timestamp");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_TIMESTAMP);
  EXPECT_EQ(f.field_refs[1].resolved.read_class,
            ProtoFieldReadClass::kMessageTimestamp);
  ASSERT_NE(f.field_refs[1].resolved.sub_field1, nullptr);  // seconds
  ASSERT_NE(f.field_refs[1].resolved.sub_field2, nullptr);  // nanos
}

// ═══════════ Layer 2 — cross-backing ═══════════

TEST(Layer2CrossBackingTest, JsonLikeBackingDispatches) {
  Layer2Fixture f;
  auto backing = std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{{"count", 5}});
  // field_number = 0 — JsonLikeBacking resolves by name only.
  f.BindMessage(std::move(backing), 0, "count");

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_INT);
  EXPECT_EQ(out.payload.i, 5);
}

// HostMap coverage lives in `host_map_test.cc` to keep this file
// scoped to the trampoline / wasmtime-edge tests it was originally
// shaped around.

// ═══════════ WKT unwrap trampolines ═══════════
//
// `CelWktUnwrapWrapperImpl` / `CelWktUnwrapTimeImpl` are the
// kStructExpr tail-unwrap arms: codegen emits them for a construction
// whose type is one of the 9 wrapper FQNs or Timestamp/Duration, and
// they peel the inner scalar out of the message.  Every guard below
// the happy path is a codegen-regression tripwire — a type-checked
// expression cannot reach them — so they are only observable by
// driving the trampoline with a hand-staged CelValue.

namespace {

// Stage `cv` at the fixture's message slot and unwrap it as a wrapper
// expecting inner kind `wrapper_kind`.
CelValue UnwrapWrapper(Layer2Fixture& f, const CelValue& cv,
                       uint32_t wrapper_kind) {
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  EXPECT_THAT(
      CelWktUnwrapWrapperImpl(Layer2Fixture::kOutSlot, Layer2Fixture::kMsgSlot,
                              wrapper_kind, f.Ctx()),
      IsOk());
  return f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
}

CelValue UnwrapTime(Layer2Fixture& f, const CelValue& cv) {
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  EXPECT_THAT(CelWktUnwrapTimeImpl(Layer2Fixture::kOutSlot,
                                   Layer2Fixture::kMsgSlot, f.Ctx()),
              IsOk());
  return f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
}

// A CEL_MESSAGE CelValue pointing at a freshly interned backing.
CelValue InternedMessage(Layer2Fixture& f,
                         std::shared_ptr<HostMessageBacking> backing) {
  CelValue cv{};
  cv.kind = CEL_MESSAGE;
  cv.payload.msg_slot = f.refs.Intern(std::move(backing));
  return cv;
}

}  // namespace

TEST(WktUnwrapWrapperTest, HappyPathPeelsInnerScalar) {
  Layer2Fixture f;
  auto wrapper = std::make_unique<google::protobuf::Int32Value>();
  wrapper->set_value(7);
  const CelValue in = InternedMessage(
      f, std::make_shared<OwnedProtoBacking>(std::move(wrapper)));
  const CelValue out = UnwrapWrapper(f, in, CEL_INT);
  EXPECT_EQ(out.kind, CEL_INT);
  EXPECT_EQ(out.payload.i, 7);
}

TEST(WktUnwrapWrapperTest, AbsorbsErrorAndUnknown) {
  Layer2Fixture f;
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = static_cast<uint32_t>(celwasm::ErrorCode::kOverflow);
  const CelValue got_err = UnwrapWrapper(f, err, CEL_INT);
  EXPECT_EQ(got_err.kind, CEL_ERROR);
  EXPECT_EQ(got_err.payload.err,
            static_cast<uint32_t>(celwasm::ErrorCode::kOverflow));

  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 3;
  const CelValue got_unk = UnwrapWrapper(f, unk, CEL_INT);
  EXPECT_EQ(got_unk.kind, CEL_UNKNOWN);
  EXPECT_EQ(got_unk.payload.unk, 3u);
}

TEST(WktUnwrapWrapperTest, NonMessageInputPoisons) {
  Layer2Fixture f;
  CelValue i{};
  i.kind = CEL_INT;
  i.payload.i = 1;
  EXPECT_EQ(UnwrapWrapper(f, i, CEL_INT).kind, CEL_ERROR);
}

TEST(WktUnwrapWrapperTest, UninternedRefIsAnInfrastructureFailure) {
  Layer2Fixture f;
  CelValue cv{};
  cv.kind = CEL_MESSAGE;
  cv.payload.msg_slot = 999;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  EXPECT_THAT(
      CelWktUnwrapWrapperImpl(Layer2Fixture::kOutSlot, Layer2Fixture::kMsgSlot,
                              CEL_INT, f.Ctx()),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               testing::HasSubstr("not found in ExternrefTable")));
}

TEST(WktUnwrapWrapperTest, BackingWithoutAProtoPoisons) {
  Layer2Fixture f;
  const CelValue in = InternedMessage(
      f, std::make_shared<JsonLikeBacking>(
             absl::flat_hash_map<std::string, int64_t>{{"x", 1}}));
  EXPECT_EQ(UnwrapWrapper(f, in, CEL_INT).kind, CEL_ERROR);
}

TEST(WktUnwrapWrapperTest, NonWrapperMessagePoisons) {
  Layer2Fixture f;
  const CelValue in = InternedMessage(
      f, std::make_shared<OwnedProtoBacking>(std::make_unique<HostMsg3>()));
  EXPECT_EQ(UnwrapWrapper(f, in, CEL_INT).kind, CEL_ERROR);
}

// The kind cross-check is the codegen tripwire: the wrapper unwraps
// cleanly, but to a kind the caller did not claim.
TEST(WktUnwrapWrapperTest, InnerKindDisagreeingWithCallerPoisons) {
  Layer2Fixture f;
  auto wrapper = std::make_unique<google::protobuf::Int32Value>();
  wrapper->set_value(7);
  const CelValue in = InternedMessage(
      f, std::make_shared<OwnedProtoBacking>(std::move(wrapper)));
  EXPECT_EQ(UnwrapWrapper(f, in, CEL_STRING).kind, CEL_ERROR);
}

TEST(WktUnwrapTimeTest, HappyPathPeelsTimestampAndDuration) {
  {
    Layer2Fixture f;
    auto ts = std::make_unique<google::protobuf::Timestamp>();
    ts->set_seconds(1234567890);
    const CelValue in =
        InternedMessage(f, std::make_shared<OwnedProtoBacking>(std::move(ts)));
    const CelValue out = UnwrapTime(f, in);
    EXPECT_EQ(out.kind, CEL_TIMESTAMP);
    EXPECT_EQ(out.payload.ts.seconds, 1234567890);
  }
  {
    Layer2Fixture f;
    auto d = std::make_unique<google::protobuf::Duration>();
    d->set_seconds(5);
    const CelValue in =
        InternedMessage(f, std::make_shared<OwnedProtoBacking>(std::move(d)));
    const CelValue out = UnwrapTime(f, in);
    EXPECT_EQ(out.kind, CEL_DURATION);
    EXPECT_EQ(out.payload.dur.seconds, 5);
  }
}

TEST(WktUnwrapTimeTest, AbsorbsErrorAndUnknown) {
  Layer2Fixture f;
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = static_cast<uint32_t>(celwasm::ErrorCode::kOverflow);
  EXPECT_EQ(UnwrapTime(f, err).kind, CEL_ERROR);

  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 3;
  const CelValue got = UnwrapTime(f, unk);
  EXPECT_EQ(got.kind, CEL_UNKNOWN);
  EXPECT_EQ(got.payload.unk, 3u);
}

TEST(WktUnwrapTimeTest, NonMessageInputPoisons) {
  Layer2Fixture f;
  CelValue i{};
  i.kind = CEL_INT;
  EXPECT_EQ(UnwrapTime(f, i).kind, CEL_ERROR);
}

TEST(WktUnwrapTimeTest, UninternedRefIsAnInfrastructureFailure) {
  Layer2Fixture f;
  CelValue cv{};
  cv.kind = CEL_MESSAGE;
  cv.payload.msg_slot = 999;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  EXPECT_THAT(CelWktUnwrapTimeImpl(Layer2Fixture::kOutSlot,
                                   Layer2Fixture::kMsgSlot, f.Ctx()),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       testing::HasSubstr("not found in ExternrefTable")));
}

TEST(WktUnwrapTimeTest, BackingWithoutAProtoPoisons) {
  Layer2Fixture f;
  const CelValue in = InternedMessage(
      f, std::make_shared<JsonLikeBacking>(
             absl::flat_hash_map<std::string, int64_t>{{"x", 1}}));
  EXPECT_EQ(UnwrapTime(f, in).kind, CEL_ERROR);
}

TEST(WktUnwrapTimeTest, NonTemporalMessagePoisons) {
  Layer2Fixture f;
  const CelValue in = InternedMessage(
      f, std::make_shared<OwnedProtoBacking>(std::make_unique<HostMsg3>()));
  EXPECT_EQ(UnwrapTime(f, in).kind, CEL_ERROR);
}

}  // namespace
}  // namespace celwasm
