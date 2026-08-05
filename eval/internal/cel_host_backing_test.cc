// cel_host Layer 1 tests — ProtoBacking field-read semantics against
// HostMsg3 / HostMsg2 / Customer fixtures, the Value::Message
// convenience constructors, the non-proto JsonLikeBacking, and the
// Any / WKT / JSON read-side peel chains.

#include "eval/internal/cel_host_backing.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "cel/expr/conformance/proto2/test_all_types.pb.h"
#include "cel/expr/conformance/proto2/test_all_types_extensions.pb.h"
#include "eval/error.h"
#include "eval/internal/cel_host_test_harness.h"
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
using ::celwasm::test::BackingFromValue;
using ::celwasm::test::IgnoredType;
using ::celwasm::test::JsonLikeBacking;
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

}  // namespace
}  // namespace celwasm
