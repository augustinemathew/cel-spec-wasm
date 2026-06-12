#include "eval/value.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "eval/attribute.h"
#include "eval/error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {
using ::celwasm::AttributeId;

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

TEST(ValueTest, DefaultIsNull) {
  Value v;
  EXPECT_EQ(v.kind(), Value::Kind::kNull);
  EXPECT_TRUE(v.IsNull());
}

TEST(ValueTest, NullBuilder) {
  auto v = Value::Null();
  EXPECT_TRUE(v.IsNull());
  EXPECT_FALSE(v.IsError());
  EXPECT_FALSE(v.IsUnknown());
}

TEST(ValueTest, BoolRoundTrips) {
  auto t = Value::Bool(true);
  auto f = Value::Bool(false);
  EXPECT_THAT(t.AsBool(), IsOkAndHolds(true));
  EXPECT_THAT(f.AsBool(), IsOkAndHolds(false));
}

TEST(ValueTest, IntRoundTrips) {
  EXPECT_THAT(Value::Int(42).AsInt(), IsOkAndHolds(42));
  EXPECT_THAT(Value::Int(-1).AsInt(), IsOkAndHolds(-1));
  EXPECT_THAT(Value::Int(INT64_MAX).AsInt(), IsOkAndHolds(INT64_MAX));
}

TEST(ValueTest, UintRoundTrips) {
  EXPECT_THAT(Value::Uint(42u).AsUint(), IsOkAndHolds(42u));
  EXPECT_THAT(Value::Uint(UINT64_MAX).AsUint(), IsOkAndHolds(UINT64_MAX));
}

TEST(ValueTest, DoubleRoundTrips) {
  EXPECT_THAT(Value::Double(3.14).AsDouble(), IsOkAndHolds(3.14));
  EXPECT_THAT(Value::Double(-0.0).AsDouble(), IsOkAndHolds(-0.0));
}

TEST(ValueTest, StringRoundTrips) {
  auto v = Value::String("hello");
  EXPECT_THAT(v.AsString(), IsOkAndHolds("hello"));
  EXPECT_EQ(v.kind(), Value::Kind::kString);
}

TEST(ValueTest, EmptyStringRoundTrips) {
  auto v = Value::String("");
  EXPECT_THAT(v.AsString(), IsOkAndHolds(""));
}

TEST(ValueTest, BytesRoundTrips) {
  // Explicit-length construction: a bare "\x00..." literal would
  // truncate at the embedded NUL.
  const std::string payload("\x00\x01\x02", 3);
  auto v = Value::Bytes(payload);
  EXPECT_EQ(v.kind(), Value::Kind::kBytes);
  // Bytes stored as std::string; AsBytes tags-guarded.
  EXPECT_THAT(v.AsBytes(), IsOkAndHolds(payload));
}

TEST(ValueTest, StringViewRoundTripsWithoutOwning) {
  const std::string storage = "hello view";
  auto v = Value::StringView(storage);
  EXPECT_EQ(v.kind(), Value::Kind::kString);
  auto sv = v.AsString();
  ASSERT_TRUE(sv.ok());
  EXPECT_EQ(*sv, "hello view");
  // Non-owning: the accessor returns a view over the caller's
  // storage, not a copy held by the Value.
  EXPECT_EQ(sv->data(), storage.data());
}

TEST(ValueTest, EmptyStringViewRoundTrips) {
  EXPECT_THAT(Value::StringView(absl::string_view()).AsString(),
              IsOkAndHolds(""));
}

TEST(ValueTest, BytesViewRoundTripsEmbeddedNul) {
  const std::string payload("\x00\x01\x02", 3);
  auto v = Value::BytesView(payload);
  EXPECT_EQ(v.kind(), Value::Kind::kBytes);
  EXPECT_THAT(v.AsBytes(), IsOkAndHolds(payload));
}

TEST(ValueTest, ViewAndOwnedFactoriesShareKindRules) {
  EXPECT_THAT(Value::StringView("x").AsBytes(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected bytes, got string")));
  EXPECT_THAT(Value::BytesView("x").AsString(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected string, got bytes")));
}

TEST(ValueTest, StructurallyEqualsAcrossOwnedAndViewRepresentations) {
  const std::string storage = "abc";
  // Representation (owning string vs non-owning view) is invisible
  // to equality — only kind + bytes matter.
  EXPECT_TRUE(Value::StringView(storage).StructurallyEquals(
      Value::String("abc")));
  EXPECT_TRUE(Value::String("abc").StructurallyEquals(
      Value::StringView(storage)));
  EXPECT_TRUE(Value::BytesView(storage).StructurallyEquals(
      Value::Bytes("abc")));
  EXPECT_FALSE(Value::StringView(storage).StructurallyEquals(
      Value::String("abd")));
  EXPECT_FALSE(Value::StringView(storage).StructurallyEquals(
      Value::Bytes("abc")));  // kind still distinguishes
}

TEST(ValueTest, StringAndBytesDoNotCrossAccessors) {
  EXPECT_THAT(Value::String("x").AsBytes(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected bytes, got string")));
  EXPECT_THAT(Value::Bytes("x").AsString(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected string, got bytes")));
}

TEST(ValueTest, DurationAndTimestampRoundTrip) {
  auto d = Value::Duration(absl::Seconds(5));
  EXPECT_THAT(d.AsDuration(), IsOkAndHolds(absl::Seconds(5)));
  auto t = Value::Timestamp(absl::FromUnixSeconds(1000));
  EXPECT_THAT(t.AsTimestamp(), IsOkAndHolds(absl::FromUnixSeconds(1000)));
}

TEST(ValueTest, WrongKindAccessorReturnsInvalidArgument) {
  EXPECT_THAT(Value::Int(1).AsBool(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected bool, got int")));
  EXPECT_THAT(Value::Null().AsInt(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected int, got null")));
}

TEST(ValueTest, UnknownCarriesAttributeId) {
  auto v = Value::Unknown(AttributeId{7});
  EXPECT_TRUE(v.IsUnknown());
  EXPECT_FALSE(v.IsError());
  EXPECT_THAT(v.UnknownAttribute(), IsOkAndHolds(AttributeId{7}));
  // The single-id factory builds a 1-element set.
  ASSERT_TRUE(v.UnknownAttributes().ok());
  EXPECT_THAT(*v.UnknownAttributes(), ::testing::ElementsAre(AttributeId{7}));
}

TEST(ValueTest, UnknownSetCarriesAllAttributeIds) {
  auto v =
      Value::Unknown(std::vector<AttributeId>{AttributeId{9}, AttributeId{2}});
  EXPECT_TRUE(v.IsUnknown());
  ASSERT_TRUE(v.UnknownAttributes().ok());
  // Canonical form: sorted ascending.
  EXPECT_THAT(*v.UnknownAttributes(),
              ::testing::ElementsAre(AttributeId{2}, AttributeId{9}));
}

TEST(ValueTest, UnknownSetDeduplicates) {
  auto v = Value::Unknown(
      std::vector<AttributeId>{AttributeId{4}, AttributeId{4}, AttributeId{1}});
  ASSERT_TRUE(v.UnknownAttributes().ok());
  EXPECT_THAT(*v.UnknownAttributes(),
              ::testing::ElementsAre(AttributeId{1}, AttributeId{4}));
}

TEST(ValueTest, UnknownAttributeOnMultiIdSetIsFailedPrecondition) {
  auto v =
      Value::Unknown(std::vector<AttributeId>{AttributeId{1}, AttributeId{2}});
  // The single-id accessor refuses to pick a winner from a merged
  // set — that would silently drop provenance.
  EXPECT_THAT(v.UnknownAttribute(),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("UnknownAttributes()")));
}

TEST(ValueTest, UnknownAccessorsOnWrongKindAreInvalidArgument) {
  EXPECT_THAT(Value::Int(1).UnknownAttributes(),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(Value::Int(1).UnknownAttribute(),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ValueTest, UnknownEmptySetIsLegal) {
  auto v = Value::Unknown(std::vector<AttributeId>{});
  EXPECT_TRUE(v.IsUnknown());
  ASSERT_TRUE(v.UnknownAttributes().ok());
  EXPECT_TRUE(v.UnknownAttributes()->empty());
  EXPECT_THAT(v.UnknownAttribute(),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST(ValueTest, UnknownStructuralEqualityIsSetEquality) {
  auto ab =
      Value::Unknown(std::vector<AttributeId>{AttributeId{1}, AttributeId{2}});
  auto ba =
      Value::Unknown(std::vector<AttributeId>{AttributeId{2}, AttributeId{1}});
  EXPECT_TRUE(ab.StructurallyEquals(ba));  // order-insensitive
  auto ac =
      Value::Unknown(std::vector<AttributeId>{AttributeId{1}, AttributeId{3}});
  EXPECT_FALSE(ab.StructurallyEquals(ac));
  EXPECT_FALSE(ab.StructurallyEquals(Value::Unknown(AttributeId{1})));
}

TEST(ValueTest, ErrorCarriesPayload) {
  auto v = Value::Error(ErrorPayload{
      .code = ErrorCode::kDivideByZero, .message = "bad math", .expr_id = 42});
  EXPECT_TRUE(v.IsError());
  EXPECT_FALSE(v.IsUnknown());
  auto info = v.ErrorInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ((*info)->code, ErrorCode::kDivideByZero);
  EXPECT_EQ((*info)->message, "bad math");
  EXPECT_EQ((*info)->expr_id, 42u);
}

TEST(ValueTest, StructurallyEqualsSameKindSameValue) {
  EXPECT_TRUE(Value::Int(5).StructurallyEquals(Value::Int(5)));
  EXPECT_TRUE(Value::String("a").StructurallyEquals(Value::String("a")));
  EXPECT_TRUE(Value::Null().StructurallyEquals(Value::Null()));
  EXPECT_TRUE(Value::Unknown(AttributeId{3})
                  .StructurallyEquals(Value::Unknown(AttributeId{3})));
}

TEST(ValueTest, StructurallyDoesNotEqualAcrossKinds) {
  // Same numeric value but different kinds (int vs uint vs double).
  EXPECT_FALSE(Value::Int(1).StructurallyEquals(Value::Uint(1u)));
  EXPECT_FALSE(Value::Int(1).StructurallyEquals(Value::Double(1.0)));
  // String vs bytes — same bytes-on-disk, different kind.
  EXPECT_FALSE(Value::String("x").StructurallyEquals(Value::Bytes("x")));
}

TEST(ValueTest, StructurallyDoesNotEqualSameKindDifferentValue) {
  EXPECT_FALSE(Value::Int(1).StructurallyEquals(Value::Int(2)));
  EXPECT_FALSE(Value::String("a").StructurallyEquals(Value::String("b")));
  EXPECT_FALSE(Value::Unknown(AttributeId{1})
                   .StructurallyEquals(Value::Unknown(AttributeId{2})));
}

TEST(ValueTest, ErrorStructurallyEqualsFieldByField) {
  EXPECT_TRUE(
      Value::Error({.code = ErrorCode::kOverflow, .message = "x", .expr_id = 1})
          .StructurallyEquals(Value::Error(
              {.code = ErrorCode::kOverflow, .message = "x", .expr_id = 1})));
  EXPECT_FALSE(
      Value::Error({.code = ErrorCode::kOverflow, .message = "x", .expr_id = 1})
          .StructurallyEquals(Value::Error(
              {.code = ErrorCode::kOverflow, .message = "y", .expr_id = 1})));
}

// Value::List + Value::Map both land in cel_host.cc (one-way dep:
// cel_host → value, never the reverse).  Positive coverage —
// HostList / HostMap construction, ListBacking / MapBacking
// retrieval, StructurallyEquals on aggregate kinds — lives in
// eval/internal/cel_host_test.cc, not here.

TEST(ValueTest, KindNamesCoverAllKinds) {
  EXPECT_EQ(ValueKindName(Value::Kind::kNull), "null");
  EXPECT_EQ(ValueKindName(Value::Kind::kInt), "int");
  EXPECT_EQ(ValueKindName(Value::Kind::kUnknown), "unknown");
  EXPECT_EQ(ValueKindName(Value::Kind::kError), "error");
}

TEST(ValueTest, CopyableAndMovable) {
  auto a = Value::String("abc");
  Value b = a;  // copy
  EXPECT_THAT(b.AsString(), IsOkAndHolds("abc"));
  EXPECT_THAT(a.AsString(), IsOkAndHolds("abc"));  // original intact
  Value c = std::move(a);                          // move
  EXPECT_THAT(c.AsString(), IsOkAndHolds("abc"));
}

}  // namespace
}  // namespace celwasm
