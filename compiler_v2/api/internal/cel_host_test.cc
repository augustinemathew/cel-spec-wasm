// cel_host Layer 1 + Layer 2 tests.  Layer 1 tests ProtoBacking's
// field-read semantics against HostMsg3 / HostMsg2 / Customer
// fixtures; Layer 2 tests the trampoline dispatch (absorption,
// unknown-pattern match, aliasing, marshal) through fake
// MemoryView/ExternrefTable/ArenaAllocator impls.

#include "compiler_v2/api/internal/cel_host.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "compiler/testdata/host_fixture_proto2.pb.h"
#include "compiler/testdata/host_fixture_proto3.pb.h"
#include "compiler_v2/api/error.h"
#include "compiler_v2/api/internal/cel_host_test_fakes.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
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

// Dummy — ProtoBacking reads via reflection, not the hint.
const cel::CelType& IgnoredType() {
  static const auto& kAny = *new cel::CelType(cel::CelType::Int());
  return kAny;
}

const HostMessageBacking* BackingFromValue(const cel::Value& v) {
  auto b = v.MessageBacking();
  EXPECT_TRUE(b.ok()) << b.status();
  return b.ok() ? *b : nullptr;
}

// ═══════════ Layer 1 — ProtoBacking ═══════════

TEST(ProtoBackingReadFieldTest, Proto3ScalarKinds) {
  // One case per cpp_type — if any arm regresses, a single failure
  // names which.
  HostMsg3 m;
  m.set_b(true);
  m.set_i32(-42);
  m.set_i64(9'000'000'000'000LL);
  m.set_u32(5);
  m.set_u64(1'000'000uLL);
  m.set_si32(-7);
  m.set_si64(-999);
  m.set_fx32(0x12345678u);
  m.set_fx64(0x1234567890ABCDEFULL);
  m.set_sfx32(-1);
  m.set_sfx64(-999'999);
  m.set_f32(3.5f);  // exactly representable
  m.set_f64(2.718281828);
  m.set_s("hello");
  m.set_by(std::string("\xde\xad\xbe\xef", 4));
  m.set_kind(HostMsg3::KIND_SEVEN);
  ProtoBacking pb(&m);

  auto read = [&](int num, absl::string_view name) {
    auto v = pb.ReadField(num, name, IgnoredType());
    EXPECT_THAT(v, IsOk()) << name;
    return std::move(v).value();
  };

  EXPECT_EQ(*read(1, "b").AsBool(), true);
  EXPECT_EQ(*read(2, "i32").AsInt(), -42);
  EXPECT_EQ(*read(3, "i64").AsInt(), 9'000'000'000'000LL);
  EXPECT_EQ(*read(4, "u32").AsUint(), 5u);
  EXPECT_EQ(*read(5, "u64").AsUint(), 1'000'000uLL);
  EXPECT_EQ(*read(6, "si32").AsInt(), -7);
  EXPECT_EQ(*read(7, "si64").AsInt(), -999);
  EXPECT_EQ(*read(8, "fx32").AsUint(), 0x12345678u);
  EXPECT_EQ(*read(9, "fx64").AsUint(), 0x1234567890ABCDEFULL);
  EXPECT_EQ(*read(10, "sfx32").AsInt(), -1);
  EXPECT_EQ(*read(11, "sfx64").AsInt(), -999'999);
  EXPECT_DOUBLE_EQ(*read(12, "f32").AsDouble(), 3.5);
  EXPECT_DOUBLE_EQ(*read(13, "f64").AsDouble(), 2.718281828);
  EXPECT_EQ(*read(14, "s").AsString(), "hello");
  EXPECT_EQ(*read(15, "by").AsBytes(),
            absl::string_view("\xde\xad\xbe\xef", 4));
  EXPECT_EQ(*read(16, "kind").AsInt(), 7);  // enum → int (langdef §2.4.7)
}

TEST(ProtoBackingReadFieldTest, NestedMessageReturnsSubBacking) {
  HostMsg3 m;
  m.mutable_inner()->set_b(true);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(17, "inner", IgnoredType());
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->kind(), cel::Value::Kind::kMessage);
  auto inner = BackingFromValue(*v)->ReadField(1, "b", IgnoredType());
  ASSERT_THAT(inner, IsOk());
  EXPECT_EQ(*inner->AsBool(), true);
}

TEST(ProtoBackingReadFieldTest, TwoHopCustomerBillingAddressCity) {
  Customer c;
  c.mutable_billing_address()->set_city("Seattle");
  ProtoBacking root(&c);
  auto billing = root.ReadField(9, "billing_address", IgnoredType());
  ASSERT_THAT(billing, IsOk());
  auto city = BackingFromValue(*billing)->ReadField(1, "city", IgnoredType());
  ASSERT_THAT(city, IsOk());
  EXPECT_EQ(*city->AsString(), "Seattle");
}

TEST(ProtoBackingReadFieldTest, Proto3UnsetStringDefaultsEmpty) {
  HostMsg3 m;  // default-constructed
  ProtoBacking pb(&m);
  auto v = pb.ReadField(14, "s", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsString(), "");
}

// M4.G flipped: REPEATED fields now return Value::HostList(ProtoList).
// Detailed coverage of element kinds + boundary semantics lives in
// compiler_v2/api/internal/proto_list_test.cc; this test pins the
// shape ProtoBacking::ReadField returns.
TEST(ProtoBackingReadFieldTest, RepeatedReturnsHostList) {
  HostMsg3 m;
  m.add_rep_i32(1);
  m.add_rep_i32(2);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(18, "rep_i32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(v->kind(), cel::Value::Kind::kList);
  auto b = v->ListBacking();
  ASSERT_THAT(b, IsOk());
  EXPECT_EQ((*b)->Size(), 2u);
}

TEST(ProtoBackingReadFieldTest, UnknownFieldReturnsCelError) {
  HostMsg3 m;
  ProtoBacking pb(&m);
  auto v = pb.ReadField(9999, "nonexistent", IgnoredType());
  ASSERT_THAT(v, IsOk());
  auto err = v->ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, cel::ErrorCode::kFieldNotFound);
}

TEST(ProtoBackingReadFieldTest, FieldNumberZeroFallsBackToName) {
  HostMsg3 m;
  m.set_i32(42);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(/*field_number=*/0, "i32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), 42);
}

TEST(ProtoBackingReadFieldTest, FieldNumberZeroUnknownNameIsFieldNotFound) {
  HostMsg3 m;
  ProtoBacking pb(&m);
  auto v = pb.ReadField(0, "nope", IgnoredType());
  ASSERT_THAT(v, IsOk());
  auto err = v->ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, cel::ErrorCode::kFieldNotFound);
}

TEST(ProtoBackingHasFieldTest, Proto3Presence) {
  HostMsg3 m;
  ProtoBacking pb_unset(&m);
  EXPECT_FALSE(pb_unset.HasField(2, "i32"));
  EXPECT_FALSE(pb_unset.HasField(17, "inner"));
  EXPECT_FALSE(pb_unset.HasField(18, "rep_i32"));
  EXPECT_FALSE(pb_unset.HasField(9999, "nope"));

  m.set_i32(3);
  (void)m.mutable_inner();
  m.add_rep_i32(1);
  ProtoBacking pb_set(&m);
  EXPECT_TRUE(pb_set.HasField(2, "i32"));
  EXPECT_TRUE(pb_set.HasField(17, "inner"));
  EXPECT_TRUE(pb_set.HasField(18, "rep_i32"));
}

TEST(ProtoBackingHasFieldTest, Proto2ExplicitPresence) {
  HostMsg2 m;
  ProtoBacking pb_unset(&m);
  EXPECT_FALSE(pb_unset.HasField(2, "i32"));
  m.set_i32(0);  // set to default; proto2 still reports true
  ProtoBacking pb_set(&m);
  EXPECT_TRUE(pb_set.HasField(2, "i32"));
}

// ═══════════ Value::Message convenience ═══════════

TEST(ValueMessageTest, MessageConstructorWrapsProtoBacking) {
  HostMsg3 m;
  m.set_i32(7);
  auto v = cel::Value::Message(m);
  auto field = BackingFromValue(v)->ReadField(2, "i32", IgnoredType());
  ASSERT_THAT(field, IsOk());
  EXPECT_EQ(*field->AsInt(), 7);
}

TEST(ValueMessageTest, HostMessageCarriesSuppliedBackingPointer) {
  HostMsg3 m;
  auto backing = std::make_shared<ProtoBacking>(&m);
  auto v = cel::Value::HostMessage(backing);
  EXPECT_EQ(BackingFromValue(v), backing.get());
}

TEST(ValueMessageTest, MessageBackingOnNonMessageFails) {
  auto v = cel::Value::Int(42);
  EXPECT_EQ(v.MessageBacking().status().code(),
            absl::StatusCode::kInvalidArgument);
}

// ═══════════ JSON-ish backing (non-proto Layer 1) ═══════════

class JsonLikeBacking : public HostMessageBacking {
 public:
  explicit JsonLikeBacking(absl::flat_hash_map<std::string, int64_t> fields)
      : fields_(std::move(fields)) {}

  absl::StatusOr<cel::Value> ReadField(int, absl::string_view name,
                                       const cel::CelType&) const override {
    auto it = fields_.find(std::string(name));
    if (it == fields_.end()) {
      return cel::Value::Error(cel::ErrorPayload{cel::ErrorCode::kFieldNotFound,
                                                 std::string(name), 0});
    }
    return cel::Value::Int(it->second);
  }

  bool HasField(int, absl::string_view name) const override {
    return fields_.contains(std::string(name));
  }

 private:
  absl::flat_hash_map<std::string, int64_t> fields_;
};

TEST(JsonLikeBackingTest, ReadAndHasResolveByName) {
  auto backing = std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{{"x", 42}});
  auto v = backing->ReadField(0, "x", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), 42);
  EXPECT_TRUE(backing->HasField(0, "x"));
  EXPECT_FALSE(backing->HasField(0, "missing"));
}

TEST(JsonLikeBackingTest, MissingFieldReturnsCelError) {
  auto backing = std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{});
  auto v = backing->ReadField(0, "absent", IgnoredType());
  ASSERT_THAT(v, IsOk());
  auto err = v->ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, cel::ErrorCode::kFieldNotFound);
}

TEST(JsonLikeBackingTest, RoundTripsThroughValueHostMessage) {
  auto backing = std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{{"n", 99}});
  auto v = cel::Value::HostMessage(backing);
  auto field = BackingFromValue(v)->ReadField(0, "n", IgnoredType());
  ASSERT_THAT(field, IsOk());
  EXPECT_EQ(*field->AsInt(), 99);
}

// ═══════════ Layer 2 — trampoline fakes ═══════════
//
// Defined in `cel_host_test_fakes.h` and shared with every other
// Layer-2 unit test (cel_map_lookup_impl_test, cel_list_at_impl_test,
// host_list_test, …).  Aliased into this TU's namespace for brevity.
using FakeMemoryView = test::FakeMemoryView;
using FakeExternrefTable = test::FakeExternrefTable;
using FakeArenaAllocator = test::FakeArenaAllocator;

// Fixture bundling mem/refs/alloc/bindings + a `Get`/`Has` helper
// that handles slot wiring.  Tests that exercise the trampoline
// instantiate one and call the helper — boilerplate stays here.
struct Layer2Fixture {
  static constexpr uint32_t kMsgSlot = 16;
  static constexpr uint32_t kOutSlot = 64;
  static constexpr uint32_t kArenaBase = 2048;

  FakeMemoryView mem{4096};
  FakeExternrefTable refs;
  FakeArenaAllocator alloc{&mem, kArenaBase, /*capacity=*/2048};
  std::vector<FieldRefEntry> field_refs{FieldRefEntry{}};  // index 0 sentinel
  std::vector<AttributeEntry> attributes;
  std::vector<cel::AttributePattern> unknown_patterns;
  CelHostBindings bindings_scratch;  // outlives TrampolineContext ref

  TrampolineContext Ctx() {
    bindings_scratch = CelHostBindings{absl::MakeConstSpan(field_refs),
                                       absl::MakeConstSpan(attributes),
                                       absl::MakeConstSpan(unknown_patterns)};
    return TrampolineContext{bindings_scratch, mem, refs, alloc};
  }

  // Intern `backing`, write a CEL_MESSAGE CelValue at kMsgSlot, and
  // register `(field_number, field_name)` at field_ref_id = 1.
  uint32_t BindMessage(std::shared_ptr<HostMessageBacking> backing,
                       uint32_t field_number, absl::string_view field_name) {
    const uint32_t slot = refs.Intern(std::move(backing));
    CelValue cv{};
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = slot;
    mem.WriteCelValue(kMsgSlot, cv);
    field_refs.push_back(FieldRefEntry{field_number, std::string(field_name)});
    return slot;
  }

  // Default dispatch: out_slot=64, msg_slot=16, field_ref_id=1,
  // attribute_id=0.  Tests that need aliasing / non-zero
  // attribute_id call CelGetFieldImpl directly.
  CelValue Get(uint32_t attribute_id = 0) {
    auto status = CelGetFieldImpl(kOutSlot, kMsgSlot, /*field_ref_id=*/1,
                                  attribute_id, Ctx());
    EXPECT_THAT(status, IsOk());
    return mem.ReadCelValue(kOutSlot);
  }
  CelValue Has() {
    auto status =
        CelHasFieldImpl(kOutSlot, kMsgSlot, /*field_ref_id=*/1, 0, Ctx());
    EXPECT_THAT(status, IsOk());
    return mem.ReadCelValue(kOutSlot);
  }
};

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
  in.payload.err = static_cast<uint32_t>(cel::ErrorCode::kOverflow);
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, in);

  const CelValue out = f.Get();
  EXPECT_EQ(out.kind, CEL_ERROR);
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(cel::ErrorCode::kOverflow));
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
            static_cast<uint32_t>(cel::ErrorCode::kTypeMismatch));
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
            static_cast<uint32_t>(cel::ErrorCode::kFieldNotFound));
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
            static_cast<uint32_t>(cel::ErrorCode::kHostAdapterError));
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
  auto pat = cel::AttributePattern::Parse("c.name");
  ASSERT_THAT(pat, IsOk());
  f.unknown_patterns.push_back(*std::move(pat));

  const CelValue out = f.Get(/*attribute_id=*/1);
  EXPECT_EQ(out.kind, CEL_UNKNOWN);
  EXPECT_EQ(out.payload.unk, 1u);
}

TEST(Layer2UnknownPatternTest, NonMatchingPatternFallsThrough) {
  Customer c;
  c.set_name("Ada");
  Layer2Fixture f;
  f.BindMessage(std::make_shared<ProtoBacking>(&c), 1, "name");
  f.attributes.push_back(AttributeEntry{});
  f.attributes.push_back(AttributeEntry{"c", {}});
  auto pat = cel::AttributePattern::Parse("c.age");  // different field
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
  auto pat = cel::AttributePattern::Parse("c.*");
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

// ═══════════ Layer 2 — CelSetFieldImpl pack arm (M7-A.A) ═══════════
//
// Exercises WriteMessageOrPack across every cpp_type-MESSAGE call
// site: singular, repeated arena-source, repeated host-source, map
// arena-source, map host-source.  Byte-level invariants (type_url
// suffix, value bytes round-trip, empty-payload corner) live here
// because cel-cpp's checker types every selection through an Any-
// typed field as `dyn` and rejects v2 lowering — the e2e path can't
// reach the same assertion.  See m7a-any.md §11 / §10.3 + the
// `m7a_test.cc` AnyPackE2ETest section header.

namespace {

// Minimal harness for pack-side trampoline tests.  Stages an
// OwnedProtoBacking<HostMsg3> as the mutable outer message, exposes
// a non-owning raw pointer for reflection-side readback, and bundles
// the rest of the Layer-2 context.
class PackHarness {
 public:
  PackHarness() {
    auto outer = std::make_unique<HostMsg3>();
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

  // Stage a null src at kSrcSlot — exercises the null-clear arm
  // ordering (must not reach WriteMessageOrPack).
  void StageNullSrc() {
    CelValue cv{};
    cv.kind = CEL_NULL;
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Stage a HostList-backed src (vector of cel::Value::Message) and
  // wire a CEL_LIST_HOST CelValue at kSrcSlot.
  void StageHostListSrc(std::vector<cel::Value> elements) {
    auto list_backing = std::make_shared<HostList>(std::move(elements));
    const uint32_t slot = f_.refs.InternList(std::move(list_backing));
    CelValue cv{};
    cv.kind = CEL_LIST_HOST;
    cv.payload.ref_slot = slot;
    f_.mem.WriteCelValue(kSrcSlot, cv);
  }

  // Stage a HostMap-backed src (vector of <key, value> cel::Value
  // pairs) and wire a CEL_MAP_HOST CelValue at kSrcSlot.
  void StageHostMapSrc(std::vector<std::pair<cel::Value, cel::Value>> entries) {
    auto map_backing = std::make_shared<HostMap>(std::move(entries));
    const uint32_t slot = f_.refs.InternMap(std::move(map_backing));
    CelValue cv{};
    cv.kind = CEL_MAP_HOST;
    cv.payload.ref_slot = slot;
    f_.mem.WriteCelValue(kSrcSlot, cv);
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

 private:
  static constexpr uint32_t kOuterMsgSlot = 16;
  static constexpr uint32_t kSrcSlot = 48;
  Layer2Fixture f_;
  HostMsg3* outer_ = nullptr;
  uint32_t outer_slot_ = 0;
};

// Expected type_url for HostMsg3 / HostMsg2.
constexpr absl::string_view kHostMsg3Url =
    "type.googleapis.com/celwasm.testdata.HostMsg3";
constexpr absl::string_view kHostMsg2Url =
    "type.googleapis.com/celwasm.testdata.HostMsg2";

}  // namespace

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
  std::vector<cel::Value> elems;
  elems.push_back(cel::Value::OwnedMessage(std::move(e0)));
  elems.push_back(cel::Value::OwnedMessage(std::move(e1)));
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
  std::vector<std::pair<cel::Value, cel::Value>> entries;
  entries.emplace_back(cel::Value::String("a"),
                       cel::Value::OwnedMessage(std::move(va)));
  entries.emplace_back(cel::Value::String("b"),
                       cel::Value::OwnedMessage(std::move(vb)));
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

}  // namespace
}  // namespace celwasm
