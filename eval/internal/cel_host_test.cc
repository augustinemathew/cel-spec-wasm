// cel_host Layer 1 + Layer 2 tests.  Layer 1 tests ProtoBacking's
// field-read semantics against HostMsg3 / HostMsg2 / Customer
// fixtures; Layer 2 tests the trampoline dispatch (absorption,
// unknown-pattern match, aliasing, marshal) through fake
// MemoryView/ExternrefTable/ArenaAllocator impls.

#include "eval/internal/cel_host.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "cel/expr/conformance/proto2/test_all_types_extensions.pb.h"
#include "eval/error.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "eval/value.h"
#include "google/protobuf/any.pb.h"
#include "google/protobuf/duration.pb.h"
#include "google/protobuf/message.h"
#include "google/protobuf/struct.pb.h"
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
const celwasm::CelType& IgnoredType() {
  static const auto& kAny = *new celwasm::CelType(celwasm::CelType::Int());
  return kAny;
}

const HostMessageBacking* BackingFromValue(const celwasm::Value& v) {
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
  ASSERT_EQ(v->kind(), celwasm::Value::Kind::kMessage);
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
// eval/internal/proto_list_test.cc; this test pins the
// shape ProtoBacking::ReadField returns.
TEST(ProtoBackingReadFieldTest, RepeatedReturnsHostList) {
  HostMsg3 m;
  m.add_rep_i32(1);
  m.add_rep_i32(2);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(18, "rep_i32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(v->kind(), celwasm::Value::Kind::kList);
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
  EXPECT_EQ((*err)->code, celwasm::ErrorCode::kFieldNotFound);
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
  EXPECT_EQ((*err)->code, celwasm::ErrorCode::kFieldNotFound);
}

// Regression for the proto2-extension look-up gap closed in the
// 2026-06-05 conformance burndown: extension fields are addressed
// by their fully-qualified name (e.g.
// `cel.expr.conformance.proto2.int32_ext`) and aren't direct
// fields of the containing message — `FindFieldByName` /
// `FindFieldByNumber` won't find them.  Mirrors cel-cpp's
// `proto_message_type_adapter.cc::HasFieldImpl` /
// `GetFieldImpl` (lines 122-188): when by-name misses, fall
// through to `Reflection::FindKnownExtensionByName`.  Pins
// conformance rows `proto2/extensions_get/package_scoped_int32`
// and the parallel `extensions_has` row.
TEST(ProtoBackingExtensionTest, ReadKnownExtensionByFullName) {
  ::cel::expr::conformance::proto2::TestAllTypes m;
  m.SetExtension(::cel::expr::conformance::proto2::int32_ext, 42);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(/*field_number=*/0,
                        "cel.expr.conformance.proto2.int32_ext", IgnoredType());
  ASSERT_THAT(v, IsOk());
  auto as_int = v->AsInt();
  ASSERT_THAT(as_int, IsOk());
  EXPECT_EQ(*as_int, 42);
}

TEST(ProtoBackingExtensionTest, HasKnownExtensionByFullNameTrueWhenSet) {
  ::cel::expr::conformance::proto2::TestAllTypes m;
  m.SetExtension(::cel::expr::conformance::proto2::int32_ext, 42);
  ProtoBacking pb(&m);
  EXPECT_TRUE(
      pb.HasField(/*field_number=*/0, "cel.expr.conformance.proto2.int32_ext"));
}

TEST(ProtoBackingExtensionTest, HasKnownExtensionByFullNameFalseWhenUnset) {
  ::cel::expr::conformance::proto2::TestAllTypes m;
  ProtoBacking pb(&m);
  EXPECT_FALSE(
      pb.HasField(/*field_number=*/0, "cel.expr.conformance.proto2.int32_ext"));
}

TEST(ProtoBackingExtensionTest, UnknownExtensionByFullNameIsFieldNotFound) {
  ::cel::expr::conformance::proto2::TestAllTypes m;
  ProtoBacking pb(&m);
  auto v =
      pb.ReadField(/*field_number=*/0,
                   "cel.expr.conformance.proto2.no_such_ext", IgnoredType());
  ASSERT_THAT(v, IsOk());
  auto err = v->ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, celwasm::ErrorCode::kFieldNotFound);
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
  auto v = celwasm::Value::Message(m);
  auto field = BackingFromValue(v)->ReadField(2, "i32", IgnoredType());
  ASSERT_THAT(field, IsOk());
  EXPECT_EQ(*field->AsInt(), 7);
}

TEST(ValueMessageTest, HostMessageCarriesSuppliedBackingPointer) {
  HostMsg3 m;
  auto backing = std::make_shared<ProtoBacking>(&m);
  auto v = celwasm::Value::HostMessage(backing);
  EXPECT_EQ(BackingFromValue(v), backing.get());
}

TEST(ValueMessageTest, MessageBackingOnNonMessageFails) {
  auto v = celwasm::Value::Int(42);
  EXPECT_EQ(v.MessageBacking().status().code(),
            absl::StatusCode::kInvalidArgument);
}

// ═══════════ JSON-ish backing (non-proto Layer 1) ═══════════

class JsonLikeBacking : public HostMessageBacking {
 public:
  // `fields_` IS initialized below; pro-type-member-init false-fires on
  // the flat_hash_map member when clang-tidy can't see its default ctor.
  // A redundant `fields_{}` would instead trip readability-redundant-
  // member-init, so suppress here.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  explicit JsonLikeBacking(absl::flat_hash_map<std::string, int64_t> fields)
      : fields_(std::move(fields)) {}

  absl::StatusOr<celwasm::Value> ReadField(
      int, absl::string_view name, const celwasm::CelType&) const override {
    auto it = fields_.find(std::string(name));
    if (it == fields_.end()) {
      return celwasm::Value::Error(celwasm::ErrorPayload{
          celwasm::ErrorCode::kFieldNotFound, std::string(name), 0});
    }
    return celwasm::Value::Int(it->second);
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
  EXPECT_EQ((*err)->code, celwasm::ErrorCode::kFieldNotFound);
}

TEST(JsonLikeBackingTest, RoundTripsThroughValueHostMessage) {
  auto backing = std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{{"n", 99}});
  auto v = celwasm::Value::HostMessage(backing);
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
  std::vector<celwasm::AttributePattern> unknown_patterns;
  CelHostBindings bindings_scratch;  // outlives TrampolineContext ref

  TrampolineContext Ctx() {
    bindings_scratch = CelHostBindings{absl::MakeConstSpan(field_refs),
                                       absl::MakeConstSpan(attributes),
                                       absl::MakeConstSpan(unknown_patterns)};
    return TrampolineContext{bindings_scratch, mem, refs, alloc};
  }

  // Intern `backing`, write a CEL_MESSAGE CelValue at kMsgSlot, and
  // register `(field_number, field_name)` at field_ref_id = 1.
  uint32_t BindMessage(const std::shared_ptr<HostMessageBacking>& backing,
                       uint32_t field_number, absl::string_view field_name) {
    const uint32_t slot = refs.Intern(backing);
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

namespace {

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

  // Stage an arbitrary CelValue at kSrcSlot verbatim.  Used to drive
  // the kind-mismatch arms of the scalar / repeated set paths, which a
  // type-checked expression can never reach (the checker types the
  // field-init operand) but a misbehaving plugin or a checker
  // regression would.
  void StageRaw(const CelValue& cv) {
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

namespace {
// CEL_INT CelValue carrying `v` (poison-test source operand).
CelValue IntCv(int64_t v) {
  CelValue cv{};
  cv.kind = CEL_INT;
  cv.payload.i = v;
  return cv;
}
}  // namespace

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

// ═══════════ M11 Slice A — Any-of-Any iterative unwrap ═══════════
//
// Pins the post-M11 contract for UnpackAnyToValue:
//
//   - depth-1 `Any<Int32Value>` reads back as `int 7` (regression
//     against the M7-A path that previously worked at this depth).
//   - depth-2 `Any<Any<Int32Value>>` reads back as `int 7` (the P0
//     fix; pre-M11 this surfaced as `CEL_MESSAGE(google.protobuf.Any)`).
//   - depth-3 `Any<Any<Any<Int32Value>>>` reads back as `int 7`.
//   - Non-`type.googleapis.com/` URL prefix is rejected with a clean
//     `kFieldNotFound`-shaped error (the strict-URL gate).
//
// The death test for depth > 1024 lives in M11 Slice B
// (`cel_wkt_wire_test.cc`) — once the WKT peel moves to the runtime,
// the depth `ABSL_CHECK` will fire there as a wasm trap, which is
// the testable surface.

namespace {

// Build `Int32Value{value: v}` and return as a heap-owned message.
std::unique_ptr<google::protobuf::Int32Value> MakeInt32Value(int32_t v) {
  auto msg = std::make_unique<google::protobuf::Int32Value>();
  msg->set_value(v);
  return msg;
}

// Wrap a message in `google.protobuf.Any` via PackFrom.
std::unique_ptr<google::protobuf::Any> WrapInAny(
    const google::protobuf::Message& inner) {
  auto any = std::make_unique<google::protobuf::Any>();
  any->PackFrom(inner);
  return any;
}

// Read `host_msg.single_any` through ProtoBacking — same path the
// runtime takes for `msg.single_any` expressions.
celwasm::Value ReadSingleAny(const HostMsg3& host_msg) {
  // ProtoBacking::ReadField for field number 30 (single_any) goes
  // through UnpackAnyToValue, which is the function under test.
  ProtoBacking backing(&host_msg);
  auto v = backing.ReadField(/*field_number=*/30, "single_any", IgnoredType());
  EXPECT_TRUE(v.ok()) << v.status();
  return v.ok() ? *std::move(v) : celwasm::Value::Null();
}

}  // namespace

TEST(AnyOfAnyTest, Depth1WrappedInt32PeelsToInt) {
  HostMsg3 outer;
  auto inner = MakeInt32Value(7);
  auto wrapped = WrapInAny(*inner);  // Any<Int32Value{value:7}>
  *outer.mutable_single_any() = *wrapped;

  celwasm::Value got = ReadSingleAny(outer);
  ASSERT_THAT(got.AsInt(), IsOk());
  EXPECT_EQ(*got.AsInt(), 7);
}

TEST(AnyOfAnyTest, Depth2WrappedInt32PeelsToInt) {
  // This is the P0 regression: pre-M11 this surfaced as
  // `CEL_MESSAGE(google.protobuf.Any)` because UnpackAnyToValue
  // peeled exactly one layer.  Post-M11 the iterative loop peels
  // both layers and falls through to the wrapper-peel.
  HostMsg3 outer;
  auto inner = MakeInt32Value(7);
  auto wrapped_once = WrapInAny(*inner);
  auto wrapped_twice =
      WrapInAny(*wrapped_once);  // Any<Any<Int32Value{value:7}>>
  *outer.mutable_single_any() = *wrapped_twice;

  celwasm::Value got = ReadSingleAny(outer);
  ASSERT_THAT(got.AsInt(), IsOk()) << "Any-of-Any depth 2 did not peel to int";
  EXPECT_EQ(*got.AsInt(), 7);
}

TEST(AnyOfAnyTest, Depth3WrappedInt32PeelsToInt) {
  HostMsg3 outer;
  auto inner = MakeInt32Value(42);
  auto w1 = WrapInAny(*inner);
  auto w2 = WrapInAny(*w1);
  auto w3 = WrapInAny(*w2);  // Any<Any<Any<Int32Value{value:42}>>>
  *outer.mutable_single_any() = *w3;

  celwasm::Value got = ReadSingleAny(outer);
  ASSERT_THAT(got.AsInt(), IsOk());
  EXPECT_EQ(*got.AsInt(), 42);
}

TEST(AnyOfAnyTest, Depth4WrappedInt32PeelsToInt) {
  HostMsg3 outer;
  auto inner = MakeInt32Value(-1);
  auto w1 = WrapInAny(*inner);
  auto w2 = WrapInAny(*w1);
  auto w3 = WrapInAny(*w2);
  auto w4 = WrapInAny(*w3);
  *outer.mutable_single_any() = *w4;

  celwasm::Value got = ReadSingleAny(outer);
  ASSERT_THAT(got.AsInt(), IsOk());
  EXPECT_EQ(*got.AsInt(), -1);
}

TEST(AnyOfAnyTest, NonWktInnerSurfacesAsMessage) {
  // An Any whose innermost payload is a user-schema message (not a
  // WKT) should surface as a CEL_MESSAGE wrapping that inner type,
  // not as a scalar.  This is the "host-side fallback" path that
  // remains correct even after the iterative loop.
  HostMsg3 inner;
  inner.set_i32(99);
  auto wrapped = WrapInAny(inner);
  HostMsg3 outer;
  *outer.mutable_single_any() = *wrapped;

  celwasm::Value got = ReadSingleAny(outer);
  ASSERT_EQ(got.kind(), celwasm::Value::Kind::kMessage)
      << "Any<HostMsg3> should surface as a CEL_MESSAGE, got kind="
      << static_cast<int>(got.kind());
}

TEST(AnyOfAnyTest, StrictUrlPrefixRejectsNonStandardPrefix) {
  // M11 Slice A also tightens the URL-prefix check.  Pre-M11
  // accepted any URL with a slash and treated the suffix as FQN —
  // a quiet divergence from cel-cpp.  Post-M11: only
  // `type.googleapis.com/` and `type.googleprod.com/` prefixes
  // resolve; anything else surfaces as a clean error.
  HostMsg3 outer;
  // Hand-construct a malformed Any (don't use PackFrom because
  // that writes the strict prefix).
  outer.mutable_single_any()->set_type_url(
      "https://evil.example.com/google.protobuf.Int32Value");
  google::protobuf::Int32Value payload;
  payload.set_value(7);
  std::string bytes;
  ASSERT_TRUE(payload.SerializeToString(&bytes));
  outer.mutable_single_any()->set_value(bytes);

  celwasm::Value got = ReadSingleAny(outer);
  EXPECT_TRUE(got.IsError())
      << "Non-standard URL prefix should produce an error, got kind="
      << static_cast<int>(got.kind());
}

TEST(AnyOfAnyTest, ExplicitlySetButEmptyTypeUrlYieldsError) {
  // Distinct from "unset Any field → null".  When the user
  // EXPLICITLY constructs an Any with no type_url (HasField=true,
  // type_url=""), cel-cpp's AdaptAny errors because there's no
  // descriptor to unpack against
  // (`third_party/cel-cpp/internal/well_known_types.cc:1960-1966`).
  // Pinned by conformance row `dynamic/any/literal_empty`
  // (`google.protobuf.Any{}` expects an `eval_error`).
  HostMsg3 outer;
  outer.mutable_single_any();  // Sets `has_single_any() == true`,
                               // type_url empty.

  celwasm::Value got = ReadSingleAny(outer);
  EXPECT_TRUE(got.IsError())
      << "Explicitly-set empty Any should error (no descriptor), got kind="
      << static_cast<int>(got.kind());
}

TEST(AnyOfAnyTest, UnsetAnyFieldYieldsNull) {
  // The complementary contract: an UNSET Any field reads as null
  // (the field-read path checks `HasField` before calling
  // `UnpackAnyToValue`, so the empty-type_url error never
  // surfaces).  Pinned by conformance row
  // `dynamic/set_null/single_any`
  // (`TestAllTypes{single_any: null}.single_any` expects null).
  HostMsg3 outer;  // single_any unset.
  celwasm::Value got = ReadSingleAny(outer);
  EXPECT_TRUE(got.IsNull()) << "Unset Any field should read as null, got kind="
                            << static_cast<int>(got.kind());
}

TEST(AnyOfAnyTest, GprodPrefixAccepted) {
  // type.googleprod.com/ is the second accepted prefix per cel-cpp's
  // strict-URL rule.
  HostMsg3 outer;
  outer.mutable_single_any()->set_type_url(
      "type.googleprod.com/google.protobuf.Int32Value");
  google::protobuf::Int32Value payload;
  payload.set_value(5);
  std::string bytes;
  ASSERT_TRUE(payload.SerializeToString(&bytes));
  outer.mutable_single_any()->set_value(bytes);

  celwasm::Value got = ReadSingleAny(outer);
  ASSERT_THAT(got.AsInt(), IsOk());
  EXPECT_EQ(*got.AsInt(), 5);
}

// ─────── Negative paths through the iterative unwrap loop ───────

TEST(AnyOfAnyTest, MalformedPayloadBytesProduceError) {
  // Strict-URL prefix passes, FQN resolves, but the payload bytes
  // don't parse against the resolved descriptor.  Surfaces as
  // `kTypeMismatch` (the "Any payload doesn't parse" message).
  HostMsg3 outer;
  outer.mutable_single_any()->set_type_url(
      "type.googleapis.com/google.protobuf.Int32Value");
  outer.mutable_single_any()->set_value("\xFFnot-a-valid-int32-wire-format");

  celwasm::Value got = ReadSingleAny(outer);
  ASSERT_TRUE(got.IsError());
  auto info = got.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->code, celwasm::ErrorCode::kTypeMismatch);
}

TEST(AnyOfAnyTest, UnknownFqnInRegisteredPrefixProducesError) {
  // Strict-URL prefix passes, but the FQN isn't in the descriptor
  // pool.  Surfaces as `kFieldNotFound`.
  HostMsg3 outer;
  outer.mutable_single_any()->set_type_url(
      "type.googleapis.com/no.such.package.Message");
  outer.mutable_single_any()->set_value("");

  celwasm::Value got = ReadSingleAny(outer);
  ASSERT_TRUE(got.IsError());
  auto info = got.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->code, celwasm::ErrorCode::kFieldNotFound);
}

TEST(AnyOfAnyTest, UrlWithoutSlashRejected) {
  // The pre-M11 path treated any string-with-no-slash as a bare
  // FQN.  Post-M11 it must be rejected via the strict-prefix gate.
  HostMsg3 outer;
  outer.mutable_single_any()->set_type_url("google.protobuf.Int32Value");
  outer.mutable_single_any()->set_value("");

  celwasm::Value got = ReadSingleAny(outer);
  EXPECT_TRUE(got.IsError());
}

// ─────── Wrapper-kind coverage matrix (per langdef §"WKT") ───────
//
// Each of the 9 wrapper types is unwrappable both directly
// (Any<X{value: ...}>) and after an extra Any layer
// (Any<Any<X{value: ...}>>).  Direct-kind coverage is the leaf
// `UnpackWrapperMessage` arm; via-Any coverage is the iterative
// `UnpackAnyToValue` walking through.

namespace {

// Build `Any<wrapper>` carrying a wrapper-typed `inner` and read
// it back through HostMsg3.single_any.
template <typename WrapperT>
celwasm::Value AnyOfWrapper(const WrapperT& inner) {
  HostMsg3 outer;
  auto wrapped = WrapInAny(inner);
  *outer.mutable_single_any() = *wrapped;
  return ReadSingleAny(outer);
}

}  // namespace

TEST(AnyOfWrapperKindsTest, BoolValueUnwrapsToBool) {
  google::protobuf::BoolValue w;
  w.set_value(true);
  celwasm::Value got = AnyOfWrapper(w);
  ASSERT_THAT(got.AsBool(), IsOk());
  EXPECT_EQ(*got.AsBool(), true);
}

TEST(AnyOfWrapperKindsTest, Int64ValueUnwrapsToInt) {
  google::protobuf::Int64Value w;
  w.set_value(-9001);
  celwasm::Value got = AnyOfWrapper(w);
  ASSERT_THAT(got.AsInt(), IsOk());
  EXPECT_EQ(*got.AsInt(), -9001);
}

TEST(AnyOfWrapperKindsTest, UInt32ValueUnwrapsToUint) {
  google::protobuf::UInt32Value w;
  w.set_value(42u);
  celwasm::Value got = AnyOfWrapper(w);
  ASSERT_THAT(got.AsUint(), IsOk());
  EXPECT_EQ(*got.AsUint(), 42u);
}

TEST(AnyOfWrapperKindsTest, UInt64ValueUnwrapsToUint) {
  google::protobuf::UInt64Value w;
  w.set_value(std::numeric_limits<uint64_t>::max());
  celwasm::Value got = AnyOfWrapper(w);
  ASSERT_THAT(got.AsUint(), IsOk());
  EXPECT_EQ(*got.AsUint(), std::numeric_limits<uint64_t>::max());
}

TEST(AnyOfWrapperKindsTest, FloatValueUnwrapsToDouble) {
  // FloatValue.value : float — CEL widens to double on unwrap.
  google::protobuf::FloatValue w;
  w.set_value(1.5f);
  celwasm::Value got = AnyOfWrapper(w);
  ASSERT_THAT(got.AsDouble(), IsOk());
  EXPECT_EQ(*got.AsDouble(), 1.5);
}

TEST(AnyOfWrapperKindsTest, DoubleValueUnwrapsToDouble) {
  google::protobuf::DoubleValue w;
  w.set_value(3.14159);
  celwasm::Value got = AnyOfWrapper(w);
  ASSERT_THAT(got.AsDouble(), IsOk());
  EXPECT_EQ(*got.AsDouble(), 3.14159);
}

TEST(AnyOfWrapperKindsTest, StringValueUnwrapsToString) {
  google::protobuf::StringValue w;
  w.set_value("hello");
  celwasm::Value got = AnyOfWrapper(w);
  ASSERT_THAT(got.AsString(), IsOk());
  EXPECT_EQ(*got.AsString(), "hello");
}

TEST(AnyOfWrapperKindsTest, BytesValueUnwrapsToBytes) {
  google::protobuf::BytesValue w;
  w.set_value(std::string("\x00\x01\xff", 3));
  celwasm::Value got = AnyOfWrapper(w);
  ASSERT_THAT(got.AsBytes(), IsOk());
  EXPECT_EQ(*got.AsBytes(), std::string("\x00\x01\xff", 3));
}

TEST(AnyOfWrapperKindsTest, Int32ValueViaAnyOfAnyUnwrapsToInt) {
  // Two Any layers, inner wrapper.  Locks the iterative loop +
  // wrapper-peel composition end-to-end (the original P0 was the
  // depth-2 case for a single wrapper kind; here we pin every kind
  // via the helper above as the depth-1 direct path).
  HostMsg3 outer;
  google::protobuf::Int32Value inner;
  inner.set_value(11);
  auto w1 = WrapInAny(inner);
  auto w2 = WrapInAny(*w1);
  *outer.mutable_single_any() = *w2;

  celwasm::Value got = ReadSingleAny(outer);
  ASSERT_THAT(got.AsInt(), IsOk());
  EXPECT_EQ(*got.AsInt(), 11);
}

// ─────── WKT-time peel through Any (Timestamp / Duration) ───────

TEST(AnyOfWktTimeTest, TimestampUnwrapsToTimestamp) {
  google::protobuf::Timestamp ts;
  ts.set_seconds(1577836800);  // 2020-01-01T00:00:00Z
  ts.set_nanos(0);
  celwasm::Value got = AnyOfWrapper(ts);
  EXPECT_EQ(got.kind(), celwasm::Value::Kind::kTimestamp)
      << "Any<Timestamp> should peel to a CEL_TIMESTAMP, got kind="
      << static_cast<int>(got.kind());
}

TEST(AnyOfWktTimeTest, DurationUnwrapsToDuration) {
  google::protobuf::Duration d;
  d.set_seconds(3600);
  d.set_nanos(0);
  celwasm::Value got = AnyOfWrapper(d);
  EXPECT_EQ(got.kind(), celwasm::Value::Kind::kDuration)
      << "Any<Duration> should peel to a CEL_DURATION, got kind="
      << static_cast<int>(got.kind());
}

}  // namespace

// ═══════════ Set-field kind-mismatch arms ═══════════
//
// Every singular-scalar and repeated-element set arm range-checks the
// incoming wire kind before handing the payload to Reflection.  A
// type-checked CEL expression cannot reach these — `Foo{i32: 'x'}`
// fails in the checker — so they are only observable by driving the
// trampoline directly with a mismatched CelValue, which is what a
// plugin returning a value inconsistent with its declared return type
// would produce.

namespace {

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

}  // namespace

TEST_P(ScalarKindMismatchTest, RejectsWrongWireKind) {
  const ScalarMismatchCase& c = GetParam();
  PackHarness h;
  h.StageRaw(c.wrong);
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
  h.StageRaw(by);
  EXPECT_THAT(h.SetField(14, "s"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("is STRING but value kind is")));
}

TEST(ScalarKindMismatchStringBytesTest, BytesFieldRejectsString) {
  PackHarness h;
  h.StageRaw(h.MakeArenaString("ab"));
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
  h.StageRaw(c.good);
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
  h.StageRaw(c.bad);
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
  h.StageRaw(h.MakeArenaString("hi"));
  ASSERT_THAT(h.SetField(47, "wrap_s"), IsOk());
  EXPECT_EQ(h.outer()->wrap_s().value(), "hi");
}

TEST(WrapperFieldStringBytesTest, BytesWrapperRoundTrips) {
  PackHarness h;
  CelValue by = h.MakeArenaString("ab");
  by.kind = CEL_BYTES;
  h.StageRaw(by);
  ASSERT_THAT(h.SetField(48, "wrap_by"), IsOk());
  EXPECT_EQ(h.outer()->wrap_by().value(), "ab");
}

TEST(WrapperFieldStringBytesTest, StringWrapperRejectsBytes) {
  PackHarness h;
  CelValue by = h.MakeArenaString("ab");
  by.kind = CEL_BYTES;
  h.StageRaw(by);
  EXPECT_THAT(h.SetField(47, "wrap_s"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("CEL_STRING")));
}

TEST(WrapperFieldStringBytesTest, BytesWrapperRejectsString) {
  PackHarness h;
  h.StageRaw(h.MakeArenaString("ab"));
  EXPECT_THAT(h.SetField(48, "wrap_by"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("CEL_BYTES")));
}

// Read side: a set wrapper collapses to the inner scalar, an unset one
// reads as null.
TEST(WrapperFieldReadTest, SetWrapperReadsAsInnerScalar) {
  auto msg = std::make_shared<HostMsg3>();
  msg->mutable_wrap_i32()->set_value(7);
  msg->mutable_wrap_s()->set_value("hi");
  msg->mutable_wrap_b()->set_value(true);
  msg->mutable_wrap_f64()->set_value(1.5);
  ProtoBacking backing(msg.get());
  auto i32 = backing.ReadField(41, "wrap_i32", CelType::Int());
  ASSERT_THAT(i32, IsOk());
  EXPECT_EQ(i32->kind(), Value::Kind::kInt);
  auto s = backing.ReadField(47, "wrap_s", CelType::String());
  ASSERT_THAT(s, IsOk());
  EXPECT_EQ(s->kind(), Value::Kind::kString);
  auto b = backing.ReadField(40, "wrap_b", CelType::Bool());
  ASSERT_THAT(b, IsOk());
  EXPECT_EQ(b->kind(), Value::Kind::kBool);
  auto d = backing.ReadField(46, "wrap_f64", CelType::Double());
  ASSERT_THAT(d, IsOk());
  EXPECT_EQ(d->kind(), Value::Kind::kDouble);
}

TEST(WrapperFieldReadTest, UnsetWrapperReadsAsNull) {
  auto msg = std::make_shared<HostMsg3>();
  ProtoBacking backing(msg.get());
  auto v = backing.ReadField(41, "wrap_i32", CelType::Int());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(v->kind(), Value::Kind::kNull);
}

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

// ═══════════ JSON well-known-type reads ═══════════
//
// A `google.protobuf.Struct` / `ListValue` / `Value` field unpacks into
// the corresponding CEL value on read: Struct to a map, ListValue to a
// list, and Value to whichever scalar its `kind` oneof holds (an unset
// Value being null per the JSON rules).
//
// These are not reachable end-to-end: a read of such a field types as
// `dyn`, and the static subset rejects dyn before codegen, so the whole
// unpack path had no workload at all.  `ProtoBacking::ReadField` is
// native, so the unpackers are exercised directly here.

TEST(JsonWktReadTest, StructReadsAsMapOfScalars) {
  auto msg = std::make_shared<HostMsg3>();
  auto& f = *msg->mutable_single_struct()->mutable_fields();
  f["s"].set_string_value("x");
  f["n"].set_number_value(1.5);
  f["b"].set_bool_value(true);
  f["z"].set_null_value(google::protobuf::NULL_VALUE);
  ProtoBacking backing(msg.get());
  auto v = backing.ReadField(36, "single_struct",
                             CelType::Map(CelType::String(), CelType::Int()));
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->kind(), Value::Kind::kMap);
  auto m = v->MapBacking();
  ASSERT_THAT(m, IsOk());
  EXPECT_EQ((*m)->Size(), 4u);
}

TEST(JsonWktReadTest, ListValueReadsAsList) {
  auto msg = std::make_shared<HostMsg3>();
  auto* lv = msg->mutable_single_list_value();
  lv->add_values()->set_string_value("x");
  lv->add_values()->set_number_value(2.0);
  ProtoBacking backing(msg.get());
  auto v =
      backing.ReadField(37, "single_list_value", CelType::List(CelType::Int()));
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->kind(), Value::Kind::kList);
  auto l = v->ListBacking();
  ASSERT_THAT(l, IsOk());
  EXPECT_EQ((*l)->Size(), 2u);
}

// One row per arm of `google.protobuf.Value`'s `kind` oneof, plus the
// unset case, plus the two that recurse back into Struct / ListValue.
TEST(JsonWktReadTest, ValueUnwrapsEveryKindArm) {
  auto read = [](const std::function<void(google::protobuf::Value&)>& fill) {
    auto msg = std::make_shared<HostMsg3>();
    fill(*msg->mutable_single_value());
    ProtoBacking backing(msg.get());
    return backing.ReadField(35, "single_value", CelType::Int());
  };
  auto null_v = read([](google::protobuf::Value& v) {
    v.set_null_value(google::protobuf::NULL_VALUE);
  });
  ASSERT_THAT(null_v, IsOk());
  EXPECT_EQ(null_v->kind(), Value::Kind::kNull);

  auto num = read([](google::protobuf::Value& v) {
    v.set_number_value(1.5);
  });
  ASSERT_THAT(num, IsOk());
  EXPECT_EQ(num->kind(), Value::Kind::kDouble);

  auto str = read([](google::protobuf::Value& v) {
    v.set_string_value("hi");
  });
  ASSERT_THAT(str, IsOk());
  EXPECT_EQ(str->kind(), Value::Kind::kString);

  auto boolean = read([](google::protobuf::Value& v) {
    v.set_bool_value(true);
  });
  ASSERT_THAT(boolean, IsOk());
  EXPECT_EQ(boolean->kind(), Value::Kind::kBool);

  auto nested_struct = read([](google::protobuf::Value& v) {
    (*v.mutable_struct_value()->mutable_fields())["k"].set_number_value(1.0);
  });
  ASSERT_THAT(nested_struct, IsOk());
  EXPECT_EQ(nested_struct->kind(), Value::Kind::kMap);

  auto nested_list = read([](google::protobuf::Value& v) {
    v.mutable_list_value()->add_values()->set_number_value(1.0);
  });
  ASSERT_THAT(nested_list, IsOk());
  EXPECT_EQ(nested_list->kind(), Value::Kind::kList);

  // An unset Value has no oneof arm set and decodes to null.
  auto unset = read([](google::protobuf::Value&) {});
  ASSERT_THAT(unset, IsOk());
  EXPECT_EQ(unset->kind(), Value::Kind::kNull);
}

// Route 2 into the JSON unpackers: an `Any` whose payload is a JSON
// well-known type.  `ReadAnyMessageArm` shares `MaybeUnpackWktMessage`
// with the field-read arm, so unwrapping the Any should chain straight
// into `UnpackJsonStruct` rather than handing back a message backing.
TEST(JsonWktReadTest, AnyPayloadOfStructChainsIntoTheJsonUnpacker) {
  auto msg = std::make_shared<HostMsg3>();
  google::protobuf::Struct payload;
  (*payload.mutable_fields())["k"].set_number_value(7.0);
  ASSERT_TRUE(msg->mutable_single_any()->PackFrom(payload));
  ProtoBacking backing(msg.get());
  auto v = backing.ReadField(30, "single_any", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(v->kind(), Value::Kind::kMap)
      << "Any(Struct) should unpack to a CEL map, got kind "
      << static_cast<int>(v->kind());
}

TEST(JsonWktReadTest, AnyPayloadOfListValueChainsIntoTheJsonUnpacker) {
  auto msg = std::make_shared<HostMsg3>();
  google::protobuf::ListValue payload;
  payload.add_values()->set_number_value(1.0);
  ASSERT_TRUE(msg->mutable_single_any()->PackFrom(payload));
  ProtoBacking backing(msg.get());
  auto v = backing.ReadField(30, "single_any", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(v->kind(), Value::Kind::kList)
      << "Any(ListValue) should unpack to a CEL list, got kind "
      << static_cast<int>(v->kind());
}

// ═══════════ Map-trampoline guards ═══════════
//
// `cel_map_size` / `cel_map_in` open with an absorb, then guard the
// operand kind, the externref lookup, and (for `in`) whether the key
// decodes to a valid map-key kind.  A type-checked expression cannot
// deliver a non-map or an undecodable key, so these arms are reached by
// staging the wire values directly.

namespace {

// Stage `cv` at the fixture's message slot and run cel_map_size.
CelValue MapSize(Layer2Fixture& f, const CelValue& cv) {
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  EXPECT_THAT(
      CelMapSizeImpl(Layer2Fixture::kOutSlot, Layer2Fixture::kMsgSlot, f.Ctx()),
      IsOk());
  return f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
}

}  // namespace

TEST(MapTrampolineGuardTest, SizeOnNonMapPoisons) {
  Layer2Fixture f;
  CelValue i{};
  i.kind = CEL_INT;
  i.payload.i = 1;
  EXPECT_EQ(MapSize(f, i).kind, CEL_ERROR);
}

TEST(MapTrampolineGuardTest, SizeAbsorbsErrorAndUnknown) {
  Layer2Fixture f;
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = static_cast<uint32_t>(celwasm::ErrorCode::kOverflow);
  EXPECT_EQ(MapSize(f, err).kind, CEL_ERROR);
  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 5;
  const CelValue got = MapSize(f, unk);
  EXPECT_EQ(got.kind, CEL_UNKNOWN);
  EXPECT_EQ(got.payload.unk, 5u);
}

TEST(MapTrampolineGuardTest, SizeOnUninternedRefIsAnInfrastructureFailure) {
  Layer2Fixture f;
  CelValue cv{};
  cv.kind = CEL_MAP_HOST;
  cv.payload.ref_slot = 999;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  EXPECT_THAT(
      CelMapSizeImpl(Layer2Fixture::kOutSlot, Layer2Fixture::kMsgSlot, f.Ctx()),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               testing::HasSubstr("not found in ExternrefTable")));
}

TEST(MapTrampolineGuardTest, InOnNonMapPoisons) {
  Layer2Fixture f;
  constexpr uint32_t kKeySlot = 96;
  CelValue key{};
  key.kind = CEL_INT;
  key.payload.i = 1;
  f.mem.WriteCelValue(kKeySlot, key);
  CelValue not_a_map{};
  not_a_map.kind = CEL_INT;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, not_a_map);
  EXPECT_THAT(CelMapInImpl(Layer2Fixture::kOutSlot, kKeySlot,
                           Layer2Fixture::kMsgSlot, f.Ctx()),
              IsOk());
  EXPECT_EQ(f.mem.ReadCelValue(Layer2Fixture::kOutSlot).kind, CEL_ERROR);
}

TEST(MapTrampolineGuardTest, InOnUninternedRefIsAnInfrastructureFailure) {
  Layer2Fixture f;
  constexpr uint32_t kKeySlot = 96;
  CelValue key{};
  key.kind = CEL_INT;
  f.mem.WriteCelValue(kKeySlot, key);
  CelValue cv{};
  cv.kind = CEL_MAP_HOST;
  cv.payload.ref_slot = 999;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  EXPECT_THAT(CelMapInImpl(Layer2Fixture::kOutSlot, kKeySlot,
                           Layer2Fixture::kMsgSlot, f.Ctx()),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       testing::HasSubstr("not found in ExternrefTable")));
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
  h.StageRaw(i);
  EXPECT_THAT(h.SetField(17, "inner"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       testing::HasSubstr("is MESSAGE but value kind is")));
}

TEST(NestedMessageSetGuardTest, UninternedSourceIsRejected) {
  PackHarness h;
  CelValue cv{};
  cv.kind = CEL_MESSAGE;
  cv.payload.msg_slot = 999;
  h.StageRaw(cv);
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

}  // namespace celwasm
