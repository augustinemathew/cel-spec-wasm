#include "tools/cel/value_format.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status_matchers.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "eval/error.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm::tools::cel {
namespace {

using ::absl_testing::IsOk;
using ::celwasm::Value;
using ::celwasm::testdata::Customer;

TEST(ValueFormatTest, ScalarBool) {
  EXPECT_EQ(*FormatScalar(Value::Bool(true)), "true");
  EXPECT_EQ(*FormatScalar(Value::Bool(false)), "false");
}

TEST(ValueFormatTest, ScalarInt) {
  EXPECT_EQ(*FormatScalar(Value::Int(42)), "42");
  EXPECT_EQ(*FormatScalar(Value::Int(-7)), "-7");
  EXPECT_EQ(*FormatScalar(Value::Int(0)), "0");
}

TEST(ValueFormatTest, ScalarUint) {
  EXPECT_EQ(*FormatScalar(Value::Uint(42)), "42u");
}

TEST(ValueFormatTest, ScalarDouble) {
  EXPECT_EQ(*FormatScalar(Value::Double(3.14)), "3.14");
}

TEST(ValueFormatTest, ScalarString) {
  EXPECT_EQ(*FormatScalar(Value::String("hi")), "\"hi\"");
  EXPECT_EQ(*FormatScalar(Value::String("a\nb")), "\"a\\nb\"");
  EXPECT_EQ(*FormatScalar(Value::String("\x01")), "\"\\x01\"");
}

TEST(ValueFormatTest, ScalarBytes) {
  EXPECT_EQ(*FormatScalar(Value::Bytes(std::string("\x00\x01"
                                                   "ab",
                                                   4))),
            "b\"\\x00\\x01ab\"");
}

TEST(ValueFormatTest, ScalarNull) {
  EXPECT_EQ(*FormatScalar(Value::Null()), "null");
}

TEST(ValueFormatTest, ScalarDurationTimestamp) {
  EXPECT_EQ(*FormatScalar(Value::Duration(absl::Seconds(3))),
            "duration(\"3s\")");
  // Just verify timestamp prints with a duration(...) / timestamp(...)
  // wrapping; the inner RFC3339 string is timezone-dependent only via
  // UTC suffix which we hardcode.
  auto t = *FormatScalar(Value::Timestamp(absl::FromUnixSeconds(0)));
  EXPECT_TRUE(absl::StartsWith(t, "timestamp(\""));
}

TEST(ValueFormatTest, ScalarError) {
  ::celwasm::ErrorPayload p{::celwasm::ErrorCode::kDivideByZero, "div by zero",
                            0};
  auto s = FormatScalar(Value::Error(p));
  ASSERT_THAT(s, IsOk());
  EXPECT_TRUE(absl::StrContains(*s, "error:"));
  EXPECT_TRUE(absl::StrContains(*s, "div by zero"));
}

// Runtime-raised errors carry the code name as their message, so the
// naive `<code> <message>` render read `divide_by_zero divide_by_zero`.
// A message that adds nothing is dropped; a distinct one is kept (the
// case above).
TEST(ValueFormatTest, ScalarErrorMessageEqualToCodeNameIsNotRepeated) {
  ::celwasm::ErrorPayload p{::celwasm::ErrorCode::kDivideByZero,
                            std::string(::celwasm::ErrorCodeName(
                                ::celwasm::ErrorCode::kDivideByZero)),
                            0};
  auto s = FormatScalar(Value::Error(p));
  ASSERT_THAT(s, IsOk());
  EXPECT_EQ(*s, "error: divide_by_zero");
}

TEST(ValueFormatTest, ScalarErrorWithEmptyMessageRendersCodeOnly) {
  ::celwasm::ErrorPayload p{::celwasm::ErrorCode::kKeyNotFound, "", 0};
  auto s = FormatScalar(Value::Error(p));
  ASSERT_THAT(s, IsOk());
  EXPECT_EQ(*s,
            absl::StrCat("error: ", ::celwasm::ErrorCodeName(
                                        ::celwasm::ErrorCode::kKeyNotFound)));
}

TEST(ValueFormatTest, ScalarUnknown) {
  auto s = FormatScalar(Value::Unknown(::celwasm::AttributeId{7}));
  ASSERT_THAT(s, IsOk());
  EXPECT_EQ(*s, "<unknown:7>");
}

TEST(ValueFormatTest, ScalarList) {
  Value v = Value::List({Value::Int(1), Value::Int(2), Value::Int(3)});
  auto s = FormatScalar(v);
  ASSERT_THAT(s, IsOk());
  EXPECT_EQ(*s, "[1, 2, 3]");
}

TEST(ValueFormatTest, ScalarMap) {
  std::vector<std::pair<Value, Value>> entries;
  entries.emplace_back(Value::String("us"), Value::Int(1));
  Value v = Value::Map(std::move(entries));
  auto s = FormatScalar(v);
  ASSERT_THAT(s, IsOk());
  EXPECT_EQ(*s, "{\"us\": 1}");
}

// ---------- Message format paths --------------------------------------------

TEST(ValueFormatTest, MessageTextprotoDefault) {
  Customer c;
  c.set_name("Ada");
  c.set_age(36);
  Value v = Value::Message(c);
  auto s = FormatMessage(v, /*formats=*/{});
  ASSERT_THAT(s, IsOk());
  EXPECT_TRUE(absl::StrContains(*s, "name: \"Ada\""));
  EXPECT_TRUE(absl::StrContains(*s, "age: 36"));
  // No section header for single-format output.
  EXPECT_FALSE(absl::StrContains(*s, "---"));
}

TEST(ValueFormatTest, MessageJson) {
  Customer c;
  c.set_name("Ada");
  c.set_age(36);
  Value v = Value::Message(c);
  auto s = FormatMessage(v, {Format::kJson});
  ASSERT_THAT(s, IsOk());
  EXPECT_TRUE(absl::StrContains(*s, "\"name\":\"Ada\""));
  EXPECT_TRUE(absl::StrContains(*s, "\"age\":36"));
}

TEST(ValueFormatTest, MessageMultiFormatLabeled) {
  Customer c;
  c.set_name("Ada");
  c.set_age(36);
  Value v = Value::Message(c);
  auto s = FormatMessage(v, {Format::kTextproto, Format::kJson});
  ASSERT_THAT(s, IsOk());
  EXPECT_TRUE(absl::StrContains(*s, "--- textproto ---"));
  EXPECT_TRUE(absl::StrContains(*s, "--- json ---"));
  EXPECT_TRUE(absl::StrContains(*s, "name: \"Ada\""));
  EXPECT_TRUE(absl::StrContains(*s, "\"name\":\"Ada\""));
}

TEST(ValueFormatTest, MessageCelLiteral) {
  Customer c;
  c.set_name("Ada");
  c.set_age(36);
  Value v = Value::Message(c);
  auto s = FormatMessage(v, {Format::kCel});
  ASSERT_THAT(s, IsOk());
  EXPECT_TRUE(absl::StartsWith(*s, "celwasm.testdata.Customer{"));
  EXPECT_TRUE(absl::StrContains(*s, "name: \"Ada\""));
  EXPECT_TRUE(absl::StrContains(*s, "age: 36"));
}

TEST(ValueFormatTest, ParseFormatName) {
  EXPECT_EQ(*ParseFormatName("textproto"), Format::kTextproto);
  EXPECT_EQ(*ParseFormatName("txtpb"), Format::kTextproto);
  EXPECT_EQ(*ParseFormatName("json"), Format::kJson);
  EXPECT_EQ(*ParseFormatName("cel"), Format::kCel);
  EXPECT_FALSE(ParseFormatName("yaml").ok());
}

}  // namespace
}  // namespace celwasm::tools::cel
