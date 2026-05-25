#include "tools/cel/var_parser.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>

#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "eval/internal/cel_host.h"
#include "common/type.h"
#include "eval/value.h"
#include "testdata/e2e_fixture.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"

namespace celwasm::tools::cel {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::celwasm::api::CelType;
using ::celwasm::api::Value;
using ::celwasm::testdata::Customer;

// Test fixture exposing the generated descriptor pool — the Customer
// proto is statically linked so `generated_pool()` knows it.
class VarParserTest : public ::testing::Test {
 protected:
  const google::protobuf::DescriptorPool& pool() const {
    return *google::protobuf::DescriptorPool::generated_pool();
  }
  google::protobuf::DynamicMessageFactory factory_;
};

// ---------- Type spec --------------------------------------------------------

TEST_F(VarParserTest, TypeSpecScalars) {
  EXPECT_EQ(ParseTypeSpec("bool")->kind(), CelType::Kind::kBool);
  EXPECT_EQ(ParseTypeSpec("int")->kind(), CelType::Kind::kInt);
  EXPECT_EQ(ParseTypeSpec("uint")->kind(), CelType::Kind::kUint);
  EXPECT_EQ(ParseTypeSpec("double")->kind(), CelType::Kind::kDouble);
  EXPECT_EQ(ParseTypeSpec("string")->kind(), CelType::Kind::kString);
  EXPECT_EQ(ParseTypeSpec("bytes")->kind(), CelType::Kind::kBytes);
  EXPECT_EQ(ParseTypeSpec("duration")->kind(), CelType::Kind::kDuration);
  EXPECT_EQ(ParseTypeSpec("timestamp")->kind(), CelType::Kind::kTimestamp);
}

TEST_F(VarParserTest, TypeSpecContainers) {
  auto l = ParseTypeSpec("list<int>");
  ASSERT_THAT(l, IsOk());
  EXPECT_EQ(l->kind(), CelType::Kind::kList);
  EXPECT_EQ(l->list_element().kind(), CelType::Kind::kInt);

  auto m = ParseTypeSpec("map<string, int>");
  ASSERT_THAT(m, IsOk());
  EXPECT_EQ(m->kind(), CelType::Kind::kMap);
  EXPECT_EQ(m->map_key().kind(), CelType::Kind::kString);
  EXPECT_EQ(m->map_value().kind(), CelType::Kind::kInt);

  auto nested = ParseTypeSpec("map<string, list<int>>");
  ASSERT_THAT(nested, IsOk());
  EXPECT_EQ(nested->map_value().kind(), CelType::Kind::kList);
}

TEST_F(VarParserTest, TypeSpecMessage) {
  auto t = ParseTypeSpec("celwasm.testdata.Customer");
  ASSERT_THAT(t, IsOk());
  EXPECT_EQ(t->kind(), CelType::Kind::kMessage);
  EXPECT_EQ(t->message_fully_qualified_name(), "celwasm.testdata.Customer");
}

TEST_F(VarParserTest, TypeSpecTrailingGarbage) {
  EXPECT_THAT(ParseTypeSpec("int xx"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ---------- Scalar value parsing --------------------------------------------

TEST_F(VarParserTest, BoolValues) {
  auto t = ParseVarFlag("b:bool=true", pool(), factory_);
  ASSERT_THAT(t, IsOk());
  EXPECT_EQ(*t->value.AsBool(), true);

  auto f = ParseVarFlag("b:bool=false", pool(), factory_);
  ASSERT_THAT(f, IsOk());
  EXPECT_EQ(*f->value.AsBool(), false);

  EXPECT_THAT(ParseVarFlag("b:bool=maybe", pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(VarParserTest, IntValues) {
  EXPECT_EQ(*ParseVarFlag("x:int=42", pool(), factory_)->value.AsInt(), 42);
  EXPECT_EQ(*ParseVarFlag("x:int=-7", pool(), factory_)->value.AsInt(), -7);
  EXPECT_EQ(*ParseVarFlag("x:int=0", pool(), factory_)->value.AsInt(), 0);
  EXPECT_EQ(*ParseVarFlag(absl::StrCat("x:int=", INT64_MAX), pool(), factory_)
                 ->value.AsInt(),
            INT64_MAX);
  EXPECT_THAT(ParseVarFlag("x:int=oops", pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseVarFlag("x:int=3.14", pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(VarParserTest, UintValues) {
  EXPECT_EQ(*ParseVarFlag("x:uint=42", pool(), factory_)->value.AsUint(), 42u);
  EXPECT_EQ(*ParseVarFlag("x:uint=42u", pool(), factory_)->value.AsUint(), 42u);
  EXPECT_THAT(ParseVarFlag("x:uint=-1", pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(VarParserTest, DoubleValues) {
  EXPECT_DOUBLE_EQ(
      *ParseVarFlag("x:double=3.14", pool(), factory_)->value.AsDouble(), 3.14);
  EXPECT_DOUBLE_EQ(
      *ParseVarFlag("x:double=-0.5", pool(), factory_)->value.AsDouble(), -0.5);
  EXPECT_DOUBLE_EQ(
      *ParseVarFlag("x:double=1e9", pool(), factory_)->value.AsDouble(), 1e9);
}

TEST_F(VarParserTest, StringValues) {
  EXPECT_EQ(
      *ParseVarFlag(R"(s:string="hello")", pool(), factory_)->value.AsString(),
      "hello");
  EXPECT_EQ(
      *ParseVarFlag(R"(s:string='hi')", pool(), factory_)->value.AsString(),
      "hi");
  EXPECT_EQ(*ParseVarFlag(R"(s:string="a\nb\tc")", pool(), factory_)
                 ->value.AsString(),
            "a\nb\tc");
  EXPECT_EQ(
      *ParseVarFlag(R"(s:string="\x41")", pool(), factory_)->value.AsString(),
      "A");
  // Missing closing quote.
  EXPECT_THAT(ParseVarFlag(R"(s:string="oops)", pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(VarParserTest, BytesValues) {
  EXPECT_EQ(*ParseVarFlag(R"(s:bytes=b"\x00\x01ab")", pool(), factory_)
                 ->value.AsBytes(),
            std::string("\x00\x01"
                        "ab",
                        4));
  // Without b prefix is also accepted.
  EXPECT_EQ(
      *ParseVarFlag(R"(s:bytes="abc")", pool(), factory_)->value.AsBytes(),
      "abc");
}

TEST_F(VarParserTest, DurationAndTimestampValues) {
  auto d = ParseVarFlag(R"(d:duration="3s")", pool(), factory_);
  ASSERT_THAT(d, IsOk());
  EXPECT_EQ(*d->value.AsDuration(), absl::Seconds(3));

  auto t =
      ParseVarFlag(R"(t:timestamp="2024-01-01T00:00:00Z")", pool(), factory_);
  ASSERT_THAT(t, IsOk());
}

// ---------- List + map ------------------------------------------------------

TEST_F(VarParserTest, ListValues) {
  auto v = ParseVarFlag("xs:list<int>=[1, 2, 3]", pool(), factory_);
  ASSERT_THAT(v, IsOk());
  auto backing = v->value.ListBacking();
  ASSERT_THAT(backing, IsOk());
  EXPECT_EQ((*backing)->Size(), 3u);
  EXPECT_EQ(*(*backing)->At(0, ::celwasm::api::CelType{})->AsInt(), 1);
  EXPECT_EQ(*(*backing)->At(2, ::celwasm::api::CelType{})->AsInt(), 3);

  // Empty list.
  auto empty = ParseVarFlag("xs:list<int>=[]", pool(), factory_);
  ASSERT_THAT(empty, IsOk());
  EXPECT_EQ((*empty->value.ListBacking())->Size(), 0u);
}

TEST_F(VarParserTest, MapValues) {
  auto v =
      ParseVarFlag(R"(m:map<string,int>={"us": 1, "ca": 2})", pool(), factory_);
  ASSERT_THAT(v, IsOk());
  auto backing = v->value.MapBacking();
  ASSERT_THAT(backing, IsOk());
  EXPECT_EQ((*backing)->Size(), 2u);
  EXPECT_TRUE((*backing)->ContainsKey(Value::String("us")));
  EXPECT_TRUE((*backing)->ContainsKey(Value::String("ca")));
}

// ---------- Message ---------------------------------------------------------

TEST_F(VarParserTest, InlineTextprotoMessage) {
  // Customer is a generated proto registered in the generated pool.
  auto v =
      ParseVarFlag(R"(c:celwasm.testdata.Customer=txtpb:name: "Ada" age: 36)",
                   pool(), factory_);
  ASSERT_THAT(v, IsOk());
  ASSERT_EQ(v->value.kind(), Value::Kind::kMessage);
  auto backing = v->value.MessageBacking();
  ASSERT_THAT(backing, IsOk());
  const auto* m = (*backing)->message();
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->GetDescriptor()->full_name(), "celwasm.testdata.Customer");
  // Round-trip via TextFormat to assert on payload.
  Customer expected;
  expected.set_name("Ada");
  expected.set_age(36);
  std::string want;
  ASSERT_TRUE(google::protobuf::TextFormat::PrintToString(expected, &want));
  std::string got;
  ASSERT_TRUE(google::protobuf::TextFormat::PrintToString(*m, &got));
  EXPECT_EQ(got, want);
}

TEST_F(VarParserTest, InlineJsonMessage) {
  auto v = ParseVarFlag(
      R"(c:celwasm.testdata.Customer=json:{"name":"Ada","age":36})", pool(),
      factory_);
  ASSERT_THAT(v, IsOk());
  const auto* m = (*v->value.MessageBacking())->message();
  ASSERT_NE(m, nullptr);
  Customer copy;
  copy.CopyFrom(*m);
  EXPECT_EQ(copy.name(), "Ada");
  EXPECT_EQ(copy.age(), 36);
}

TEST_F(VarParserTest, FileTextprotoMessage) {
  // Spill the textproto to a temp file and read it back via @path.
  std::string path = absl::StrCat(::testing::TempDir(), "/customer.txtpb");
  {
    std::ofstream out(path);
    out << R"(name: "Ada"
age: 36
)";
  }
  auto v = ParseVarFlag(absl::StrCat("c:celwasm.testdata.Customer=@", path),
                        pool(), factory_);
  ASSERT_THAT(v, IsOk());
  const auto* m = (*v->value.MessageBacking())->message();
  ASSERT_NE(m, nullptr);
  Customer copy;
  copy.CopyFrom(*m);
  EXPECT_EQ(copy.name(), "Ada");
  EXPECT_EQ(copy.age(), 36);
  std::remove(path.c_str());
}

TEST_F(VarParserTest, MessageUnknownType) {
  EXPECT_THAT(ParseVarFlag("c:no.such.Message=txtpb:", pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(VarParserTest, MessageInlineMissingPrefix) {
  EXPECT_THAT(ParseVarFlag("c:celwasm.testdata.Customer={name: \"Ada\"}",
                           pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(VarParserTest, MessageBadInferredFormat) {
  // Unknown extension and no explicit prefix.
  EXPECT_THAT(ParseVarFlag("c:celwasm.testdata.Customer=@/tmp/foo.unknown",
                           pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// ---------- Flag-level shape ------------------------------------------------

TEST_F(VarParserTest, DeclarationOnly) {
  auto v = ParseVarFlag("x:int", pool(), factory_);
  ASSERT_THAT(v, IsOk());
  EXPECT_EQ(v->type.kind(), CelType::Kind::kInt);
  EXPECT_FALSE(v->has_value);
}

TEST_F(VarParserTest, MissingColonRejected) {
  EXPECT_THAT(ParseVarFlag("x", pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseVarFlag("=42", pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(VarParserTest, TrailingGarbageRejected) {
  EXPECT_THAT(ParseVarFlag("x:int=42 xx", pool(), factory_),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace
}  // namespace celwasm::tools::cel
