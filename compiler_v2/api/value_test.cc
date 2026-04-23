#include "compiler_v2/api/value.h"

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/time/time.h"
#include "compiler_v2/api/attribute.h"
#include "compiler_v2/api/error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace cel {
namespace {

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
  auto v = Value::Bytes("\x00\x01\x02");
  EXPECT_EQ(v.kind(), Value::Kind::kBytes);
  // Bytes stored as std::string; AsBytes tags-guarded.
  EXPECT_THAT(v.AsBytes(), IsOkAndHolds("\x00\x01\x02"));
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
}

TEST(ValueTest, ErrorCarriesPayload) {
  auto v = Value::Error(ErrorPayload{.code = ErrorCode::kDivideByZero,
                                     .message = "bad math",
                                     .expr_id = 42});
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
  EXPECT_TRUE(
      Value::Unknown(AttributeId{3})
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
          .StructurallyEquals(Value::Error({.code = ErrorCode::kOverflow,
                                            .message = "x",
                                            .expr_id = 1})));
  EXPECT_FALSE(
      Value::Error({.code = ErrorCode::kOverflow, .message = "x", .expr_id = 1})
          .StructurallyEquals(Value::Error({.code = ErrorCode::kOverflow,
                                            .message = "y",
                                            .expr_id = 1})));
}

TEST(ValueDeathTest, ListBuilderFiresCheckUntilM6) {
  EXPECT_DEATH({ (void)Value::List({}); }, "stub until M6");
}

TEST(ValueDeathTest, MapBuilderFiresCheckUntilM6) {
  EXPECT_DEATH({ (void)Value::Map({}); }, "stub until M6");
}

TEST(ValueTest, KindNamesCoverAllKinds) {
  EXPECT_EQ(ValueKindName(Value::Kind::kNull), "null");
  EXPECT_EQ(ValueKindName(Value::Kind::kInt), "int");
  EXPECT_EQ(ValueKindName(Value::Kind::kUnknown), "unknown");
  EXPECT_EQ(ValueKindName(Value::Kind::kError), "error");
}

TEST(ValueTest, CopyableAndMovable) {
  auto a = Value::String("abc");
  Value b = a;                                      // copy
  EXPECT_THAT(b.AsString(), IsOkAndHolds("abc"));
  EXPECT_THAT(a.AsString(), IsOkAndHolds("abc"));   // original intact
  Value c = std::move(a);                           // move
  EXPECT_THAT(c.AsString(), IsOkAndHolds("abc"));
}

}  // namespace
}  // namespace cel
