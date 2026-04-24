// Tests for cel_host Layer 1 (HostMessageBacking + ProtoBacking).
//
// Per m2-ident-select-unknowns.md §5 Slice M2.C.0, Layer 1 lands
// before any wasmtime or codegen work touches cel_host — the
// field-read semantics get exhaustively tested in pure C++ so
// later slices can build on a locked interface.
//
// Coverage matrix from §6.1.1 (the parts Layer 1 owns):
//
//   ReadField × every proto3 cpp_type               ✓ (HostMsg3 fixture)
//   ReadField × repeated → CEL_ERR_TYPE_UNSUPPORTED ✓
//   ReadField × unresolvable field → CEL_ERROR      ✓
//   HasField  × proto3 implicit presence            ✓ (HostMsg3)
//   HasField  × proto2 explicit presence            ✓ (HostMsg2)
//   Self-recursive nested select                    ✓ (HostMsg3.inner.b)
//   Two-hop c.billing_address.city                  ✓ (Customer + Address)
//   field_ref_id = 0 name-fallback                  ✓
//   Sub-message backings wrap in fresh ProtoBacking ✓
//
// Coverage the later slices handle:
//   Trampoline absorption (UNKNOWN / ERROR inputs)  — Layer 2 test
//   Unknown-pattern matching                        — Layer 2 test
//   Aliasing (msg_slot == out_slot)                 — Layer 2 test
//   JsonBacking parity                              — Layer 1 but needs a
//                                                     test-only subclass;
//                                                     added below

#include "compiler_v2/api/internal/cel_host.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "compiler/testdata/host_fixture_proto2.pb.h"
#include "compiler/testdata/host_fixture_proto3.pb.h"
#include "compiler_v2/api/error.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::testdata::Address;
using ::celwasm::testdata::Customer;
using ::celwasm::testdata::HostMsg2;
using ::celwasm::testdata::HostMsg3;

// A dummy CelType — ProtoBacking ignores it at M2 (field resolution
// via descriptor reflection, not the CelType hint).  Plumbed for
// shape.
const cel::CelType& IgnoredType() {
  static const auto& kAny = *new cel::CelType(cel::CelType::Int());
  return kAny;
}

// Extract a mutable HostMessageBacking* from a Message-kind Value.
// Value::MessageBacking() returns a `const HostMessageBacking*`, but
// Layer 1's ReadField / HasField are non-const on the interface
// (embedders may memoise).  Tests need the non-const handle to
// actually drive the read path.  The underlying ProtoBacking is
// stateless, so dropping const is safe; the helper scopes the
// pattern to one place so clang-tidy doesn't flag every call site
// with `const_cast` warnings.
HostMessageBacking* MutableBackingFromValue(const cel::Value& v) {
  auto b = v.MessageBacking();
  EXPECT_TRUE(b.ok()) << b.status();
  if (!b.ok() || *b == nullptr) return nullptr;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  return const_cast<HostMessageBacking*>(*b);
}

// ──────────────────────────────────────────────────────────────
// ProtoBacking::ReadField × every proto3 cpp_type
// ──────────────────────────────────────────────────────────────

TEST(ProtoBackingReadFieldTest, Bool) {
  HostMsg3 m;
  m.set_b(true);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(1, "b", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(v->kind(), cel::Value::Kind::kBool);
  EXPECT_EQ(*v->AsBool(), true);
}

TEST(ProtoBackingReadFieldTest, Int32) {
  HostMsg3 m;
  m.set_i32(-42);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(2, "i32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), -42);
}

TEST(ProtoBackingReadFieldTest, Int64) {
  HostMsg3 m;
  m.set_i64(9'000'000'000'000LL);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(3, "i64", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), 9'000'000'000'000LL);
}

TEST(ProtoBackingReadFieldTest, Uint32) {
  HostMsg3 m;
  m.set_u32(5);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(4, "u32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsUint(), 5u);
}

TEST(ProtoBackingReadFieldTest, Uint64) {
  HostMsg3 m;
  m.set_u64(1'000'000uLL);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(5, "u64", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsUint(), 1'000'000uLL);
}

TEST(ProtoBackingReadFieldTest, Sint32AsInt) {
  HostMsg3 m;
  m.set_si32(-7);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(6, "si32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), -7);
}

TEST(ProtoBackingReadFieldTest, Sint64AsInt) {
  HostMsg3 m;
  m.set_si64(-999);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(7, "si64", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), -999);
}

TEST(ProtoBackingReadFieldTest, Fixed32AsUint) {
  HostMsg3 m;
  m.set_fx32(0x12345678u);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(8, "fx32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsUint(), 0x12345678u);
}

TEST(ProtoBackingReadFieldTest, Fixed64AsUint) {
  HostMsg3 m;
  m.set_fx64(0x1234567890ABCDEFULL);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(9, "fx64", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsUint(), 0x1234567890ABCDEFULL);
}

TEST(ProtoBackingReadFieldTest, Sfixed32AsInt) {
  HostMsg3 m;
  m.set_sfx32(-1);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(10, "sfx32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), -1);
}

TEST(ProtoBackingReadFieldTest, Sfixed64AsInt) {
  HostMsg3 m;
  m.set_sfx64(-999'999);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(11, "sfx64", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), -999'999);
}

TEST(ProtoBackingReadFieldTest, FloatAsDouble) {
  HostMsg3 m;
  m.set_f32(3.5f);  // exactly representable, no precision loss
  ProtoBacking pb(&m);
  auto v = pb.ReadField(12, "f32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_DOUBLE_EQ(*v->AsDouble(), 3.5);
}

TEST(ProtoBackingReadFieldTest, Double) {
  HostMsg3 m;
  m.set_f64(2.718281828);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(13, "f64", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_DOUBLE_EQ(*v->AsDouble(), 2.718281828);
}

TEST(ProtoBackingReadFieldTest, String) {
  HostMsg3 m;
  m.set_s("hello");
  ProtoBacking pb(&m);
  auto v = pb.ReadField(14, "s", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(v->kind(), cel::Value::Kind::kString);
  EXPECT_EQ(*v->AsString(), "hello");
}

TEST(ProtoBackingReadFieldTest, Bytes) {
  HostMsg3 m;
  m.set_by(std::string("\xde\xad\xbe\xef", 4));
  ProtoBacking pb(&m);
  auto v = pb.ReadField(15, "by", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(v->kind(), cel::Value::Kind::kBytes);
  auto b = *v->AsBytes();
  EXPECT_EQ(b, absl::string_view("\xde\xad\xbe\xef", 4));
}

TEST(ProtoBackingReadFieldTest, EnumAsInt) {
  HostMsg3 m;
  m.set_kind(HostMsg3::KIND_SEVEN);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(16, "kind", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), 7);
}

// ──────────────────────────────────────────────────────────────
// ProtoBacking::ReadField × Message (nested sub-backings)
// ──────────────────────────────────────────────────────────────

TEST(ProtoBackingReadFieldTest, NestedMessageReturnsSubBacking) {
  HostMsg3 m;
  m.mutable_inner()->set_b(true);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(17, "inner", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(v->kind(), cel::Value::Kind::kMessage);

  // Walk into the sub-backing and read from it.
  auto backing = v->MessageBacking();
  ASSERT_THAT(backing, IsOk());
  ASSERT_NE(*backing, nullptr);
  // Const-cast is safe here: MessageBacking returns a
  // `const HostMessageBacking*`, but ReadField is non-const on
  // the Layer-1 interface (implementors may stash memoisation).
  // The underlying ProtoBacking is stateless, so dropping const
  // is fine for the test's read path.
  auto* raw = MutableBackingFromValue(*v);
  auto inner_b = raw->ReadField(1, "b", IgnoredType());
  ASSERT_THAT(inner_b, IsOk());
  EXPECT_EQ(*inner_b->AsBool(), true);
}

TEST(ProtoBackingReadFieldTest, TwoHopCustomerBillingAddressCity) {
  // The M2.C end-to-end fixture shape: `c.billing_address.city`
  // = "Seattle" lands on the second hop's `s` read.
  (void)Customer::descriptor();
  (void)Address::descriptor();
  Customer c;
  c.mutable_billing_address()->set_city("Seattle");
  ProtoBacking root(&c);

  auto billing = root.ReadField(9, "billing_address", IgnoredType());
  ASSERT_THAT(billing, IsOk());
  EXPECT_EQ(billing->kind(), cel::Value::Kind::kMessage);

  auto* hop1 = MutableBackingFromValue(*billing);
  ASSERT_NE(hop1, nullptr);
  auto city = hop1->ReadField(1, "city", IgnoredType());
  ASSERT_THAT(city, IsOk());
  EXPECT_EQ(*city->AsString(), "Seattle");
}

TEST(ProtoBackingReadFieldTest, ProtoDefaultForUnsetStringIsEmpty) {
  // Proto3 singular scalars have implicit presence — reading an
  // unset string returns "".
  HostMsg3 m;  // default-constructed; s = "".
  ProtoBacking pb(&m);
  auto v = pb.ReadField(14, "s", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsString(), "");
}

// ──────────────────────────────────────────────────────────────
// ProtoBacking::ReadField × envelope boundary
// ──────────────────────────────────────────────────────────────

TEST(ProtoBackingReadFieldTest,
     RepeatedFieldReturnsCelErrorWithTypeUnsupported) {
  HostMsg3 m;
  m.add_rep_i32(1);
  m.add_rep_i32(2);
  ProtoBacking pb(&m);
  auto v = pb.ReadField(18, "rep_i32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->kind(), cel::Value::Kind::kError);
  auto err = v->ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, cel::ErrorCode::kTypeUnsupported);
}

TEST(ProtoBackingReadFieldTest, UnknownFieldNumberReturnsCelError) {
  HostMsg3 m;
  ProtoBacking pb(&m);
  auto v = pb.ReadField(/*field_number=*/9999, /*field_name=*/"nonexistent",
                        IgnoredType());
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->kind(), cel::Value::Kind::kError);
  auto err = v->ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, cel::ErrorCode::kFieldNotFound);
}

TEST(ProtoBackingReadFieldTest, FieldNumberZeroResolvesByName) {
  // The name-fallback path — plan §2.4.1 promises that
  // `field_number = 0` triggers a name-only lookup.  Useful for
  // non-proto backings but equally testable against ProtoBacking.
  HostMsg3 m;
  m.set_i32(42);
  ProtoBacking pb(&m);
  auto v =
      pb.ReadField(/*field_number=*/0, /*field_name=*/"i32", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), 42);
}

TEST(ProtoBackingReadFieldTest, FieldNumberZeroWithUnknownNameReturnsCelError) {
  HostMsg3 m;
  ProtoBacking pb(&m);
  auto v = pb.ReadField(0, "nope", IgnoredType());
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->kind(), cel::Value::Kind::kError);
  auto err = v->ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, cel::ErrorCode::kFieldNotFound);
}

// ──────────────────────────────────────────────────────────────
// ProtoBacking::HasField (proto3 + proto2)
// ──────────────────────────────────────────────────────────────

TEST(ProtoBackingHasFieldTest, Proto3SingularScalarSet) {
  HostMsg3 m;
  m.set_i32(3);
  ProtoBacking pb(&m);
  EXPECT_TRUE(pb.HasField(2, "i32"));
}

TEST(ProtoBackingHasFieldTest, Proto3SingularScalarUnsetReturnsFalse) {
  HostMsg3 m;
  ProtoBacking pb(&m);
  EXPECT_FALSE(pb.HasField(2, "i32"));
}

TEST(ProtoBackingHasFieldTest, Proto3MessageFieldUnsetReturnsFalse) {
  HostMsg3 m;
  ProtoBacking pb(&m);
  EXPECT_FALSE(pb.HasField(17, "inner"));
}

TEST(ProtoBackingHasFieldTest, Proto3MessageFieldSetReturnsTrue) {
  HostMsg3 m;
  (void)m.mutable_inner();  // allocates empty sub-message
  ProtoBacking pb(&m);
  EXPECT_TRUE(pb.HasField(17, "inner"));
}

TEST(ProtoBackingHasFieldTest, RepeatedFieldEmptyReturnsFalse) {
  HostMsg3 m;
  ProtoBacking pb(&m);
  EXPECT_FALSE(pb.HasField(18, "rep_i32"));
}

TEST(ProtoBackingHasFieldTest, RepeatedFieldNonEmptyReturnsTrue) {
  HostMsg3 m;
  m.add_rep_i32(1);
  ProtoBacking pb(&m);
  EXPECT_TRUE(pb.HasField(18, "rep_i32"));
}

TEST(ProtoBackingHasFieldTest, UnresolvableFieldReturnsFalse) {
  HostMsg3 m;
  ProtoBacking pb(&m);
  EXPECT_FALSE(pb.HasField(9999, "nope"));
}

TEST(ProtoBackingHasFieldTest, Proto2OptionalUnsetReturnsFalse) {
  // proto2's explicit-presence scalar: the optional qualifier
  // means HasField returns false unless the bit was set.
  HostMsg2 m;
  ProtoBacking pb(&m);
  EXPECT_FALSE(pb.HasField(2, "i32"));
}

TEST(ProtoBackingHasFieldTest, Proto2OptionalSetToDefaultReturnsTrue) {
  // The proto3-can't-distinguish case: set the field to its
  // default value (0).  proto2 still reports HasField == true
  // because the bit was set; proto3 would return false.
  HostMsg2 m;
  m.set_i32(0);
  ProtoBacking pb(&m);
  EXPECT_TRUE(pb.HasField(2, "i32"));
}

// ──────────────────────────────────────────────────────────────
// Value::Message / HostMessage convenience constructors
// ──────────────────────────────────────────────────────────────

TEST(ValueMessageTest, MessageConstructorWrapsInProtoBacking) {
  HostMsg3 m;
  m.set_i32(7);
  auto v = cel::Value::Message(m);
  EXPECT_EQ(v.kind(), cel::Value::Kind::kMessage);
  auto* raw = MutableBackingFromValue(v);
  ASSERT_NE(raw, nullptr);
  auto field = raw->ReadField(2, "i32", IgnoredType());
  ASSERT_THAT(field, IsOk());
  EXPECT_EQ(*field->AsInt(), 7);
}

TEST(ValueMessageTest, HostMessageCarriesSuppliedBackingPointerEquality) {
  HostMsg3 m;
  auto backing = std::make_shared<ProtoBacking>(&m);
  auto v = cel::Value::HostMessage(backing);
  auto got = v.MessageBacking();
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got, backing.get());
}

TEST(ValueMessageTest, MessageBackingOnNonMessageValueFailsInvalidArgument) {
  auto v = cel::Value::Int(42);
  auto b = v.MessageBacking();
  EXPECT_FALSE(b.ok());
  EXPECT_EQ(b.status().code(), absl::StatusCode::kInvalidArgument);
}

// ──────────────────────────────────────────────────────────────
// Custom HostMessageBacking subclass — "JSON-ish" test impl
//
// Proves the Layer 1 interface isn't accidentally proto-specific.
// `JsonLikeBacking` stores a string→int map and resolves fields
// by name only (field_number = 0 sentinel path).  Any future
// real JSON backing follows the same shape.
// ──────────────────────────────────────────────────────────────

class JsonLikeBacking : public HostMessageBacking {
 public:
  explicit JsonLikeBacking(absl::flat_hash_map<std::string, int64_t> fields)
      : fields_(std::move(fields)) {}

  absl::StatusOr<cel::Value> ReadField(
      int /*field_number*/, absl::string_view field_name,
      const cel::CelType& /*expected_type*/) override {
    auto it = fields_.find(std::string(field_name));
    if (it == fields_.end()) {
      return cel::Value::Error(cel::ErrorPayload{cel::ErrorCode::kFieldNotFound,
                                                 std::string(field_name), 0});
    }
    return cel::Value::Int(it->second);
  }

  bool HasField(int /*field_number*/, absl::string_view field_name) override {
    return fields_.contains(std::string(field_name));
  }

 private:
  absl::flat_hash_map<std::string, int64_t> fields_;
};

TEST(JsonLikeBackingTest, ReadFieldResolvesByName) {
  auto backing = std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{{"x", 42}});
  auto v = backing->ReadField(/*field_number=*/0, "x", IgnoredType());
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(*v->AsInt(), 42);
}

TEST(JsonLikeBackingTest, MissingFieldReturnsCelError) {
  auto backing = std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{});
  auto v = backing->ReadField(0, "absent", IgnoredType());
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->kind(), cel::Value::Kind::kError);
  auto err = v->ErrorInfo();
  ASSERT_THAT(err, IsOk());
  EXPECT_EQ((*err)->code, cel::ErrorCode::kFieldNotFound);
}

TEST(JsonLikeBackingTest, HasFieldChecksByName) {
  auto backing = std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{{"present", 1}});
  EXPECT_TRUE(backing->HasField(0, "present"));
  EXPECT_FALSE(backing->HasField(0, "missing"));
}

// Bound into a Value via HostMessage — proves the
// Activation::Bind carriage works for non-proto backings too.
TEST(JsonLikeBackingTest, RoundTripsThroughValueHostMessage) {
  auto backing = std::make_shared<JsonLikeBacking>(
      absl::flat_hash_map<std::string, int64_t>{{"n", 99}});
  auto v = cel::Value::HostMessage(backing);
  ASSERT_EQ(v.kind(), cel::Value::Kind::kMessage);
  auto* raw = MutableBackingFromValue(v);
  ASSERT_NE(raw, nullptr);
  auto field = raw->ReadField(0, "n", IgnoredType());
  ASSERT_THAT(field, IsOk());
  EXPECT_EQ(*field->AsInt(), 99);
}

}  // namespace
}  // namespace celwasm
