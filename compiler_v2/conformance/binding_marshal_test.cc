#include "compiler_v2/conformance/binding_marshal.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "cel/expr/eval.pb.h"
#include "cel/expr/value.pb.h"
#include "compiler_v2/api/activation.h"
#include "compiler_v2/api/value.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"

namespace celwasm::conformance {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::cel::expr::Decl;
using ::cel::expr::ExprValue;
using ::cel::expr::Value;
using ::cel::expr::conformance::test::SimpleTest;

TEST(ValueFromProto, NullRoundTrip) {
  Value v;
  v.set_null_value(google::protobuf::NULL_VALUE);
  auto got = ValueFromProto(v);
  ASSERT_THAT(got, IsOk());
  EXPECT_TRUE(got->IsNull());
}

TEST(ValueFromProto, BoolRoundTrip) {
  Value v;
  v.set_bool_value(true);
  auto got = ValueFromProto(v);
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got->AsBool(), true);
}

TEST(ValueFromProto, Int64RoundTrip) {
  Value v;
  v.set_int64_value(-42);
  auto got = ValueFromProto(v);
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got->AsInt(), -42);
}

TEST(ValueFromProto, Uint64RoundTrip) {
  Value v;
  v.set_uint64_value(7u);
  auto got = ValueFromProto(v);
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got->AsUint(), 7u);
}

TEST(ValueFromProto, DoubleRoundTrip) {
  Value v;
  v.set_double_value(3.14);
  auto got = ValueFromProto(v);
  ASSERT_THAT(got, IsOk());
  EXPECT_DOUBLE_EQ(*got->AsDouble(), 3.14);
}

TEST(ValueFromProto, StringRoundTrip) {
  Value v;
  v.set_string_value("hi");
  auto got = ValueFromProto(v);
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got->AsString(), "hi");
}

TEST(ValueFromProto, BytesRoundTrip) {
  Value v;
  v.set_bytes_value("\x00\x01\x02", 3);
  auto got = ValueFromProto(v);
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(got->AsBytes()->size(), 3u);
}

TEST(ValueFromProto, ListUnimplemented) {
  Value v;
  v.mutable_list_value();
  EXPECT_THAT(ValueFromProto(v), StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(ValueFromProto, MapUnimplemented) {
  Value v;
  v.mutable_map_value();
  EXPECT_THAT(ValueFromProto(v), StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(ValueFromProto, ObjectUnimplemented) {
  Value v;
  v.mutable_object_value();
  EXPECT_THAT(ValueFromProto(v), StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(ValueFromProto, EnumUnimplemented) {
  Value v;
  v.mutable_enum_value()->set_type("e.E");
  EXPECT_THAT(ValueFromProto(v), StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(ValueFromProto, TypeUnimplemented) {
  Value v;
  v.set_type_value("int");
  EXPECT_THAT(ValueFromProto(v), StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(ValueFromProto, NoKindSetIsInvalid) {
  Value v;
  EXPECT_THAT(ValueFromProto(v), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(VariableSpecFromDecl, BoolPrimitive) {
  Decl d;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(name: "x"
           ident { type { primitive: BOOL } })pb",
      &d));
  auto got = VariableSpecFromDecl(d);
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got, "x:bool");
}

TEST(VariableSpecFromDecl, AllScalarPrimitives) {
  struct Case {
    std::string textproto;
    std::string want;
  };
  const Case cases[] = {
      {R"pb(name: "x"
            ident { type { primitive: INT64 } })pb",
       "x:int"},
      {R"pb(name: "x"
            ident { type { primitive: UINT64 } })pb",
       "x:uint"},
      {R"pb(name: "x"
            ident { type { primitive: DOUBLE } })pb",
       "x:double"},
      {R"pb(name: "x"
            ident { type { primitive: STRING } })pb",
       "x:string"},
      {R"pb(name: "x"
            ident { type { primitive: BYTES } })pb",
       "x:bytes"},
  };
  for (const auto& c : cases) {
    Decl d;
    ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(c.textproto, &d));
    auto got = VariableSpecFromDecl(d);
    ASSERT_THAT(got, IsOk()) << c.textproto;
    EXPECT_EQ(*got, c.want) << c.textproto;
  }
}

TEST(VariableSpecFromDecl, QualifiedNamePreserved) {
  Decl d;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(name: "x.y"
           ident { type { primitive: BOOL } })pb",
      &d));
  auto got = VariableSpecFromDecl(d);
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got, "x.y:bool");
}

TEST(VariableSpecFromDecl, MessageTypeUnimplemented) {
  Decl d;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(name: "m"
           ident { type { message_type: "foo.Bar" } })pb",
      &d));
  EXPECT_THAT(VariableSpecFromDecl(d),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(VariableSpecFromDecl, ListTypeUnimplemented) {
  Decl d;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(name: "l"
           ident { type { list_type { elem_type { primitive: INT64 } } } })pb",
      &d));
  EXPECT_THAT(VariableSpecFromDecl(d),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(VariableSpecFromDecl, FunctionDeclUnimplemented) {
  Decl d;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(name: "f"
           function { overloads { overload_id: "f_int" } })pb",
      &d));
  EXPECT_THAT(VariableSpecFromDecl(d),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(PopulateActivation, ScalarsBoundCorrectly) {
  SimpleTest t;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        bindings {
          key: "b"
          value { value { bool_value: true } }
        }
        bindings {
          key: "i"
          value { value { int64_value: 7 } }
        }
        bindings {
          key: "s"
          value { value { string_value: "hi" } }
        }
      )pb",
      &t));
  cel::Activation act;
  ASSERT_THAT(PopulateActivation(t, act), IsOk());
  ASSERT_NE(act.Find("b"), nullptr);
  EXPECT_EQ(*act.Find("b")->AsBool(), true);
  ASSERT_NE(act.Find("i"), nullptr);
  EXPECT_EQ(*act.Find("i")->AsInt(), 7);
  ASSERT_NE(act.Find("s"), nullptr);
  EXPECT_EQ(*act.Find("s")->AsString(), "hi");
}

TEST(PopulateActivation, UnknownBindingIsUnimplemented) {
  SimpleTest t;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(bindings {
             key: "u"
             value { unknown { exprs: 1 } }
           })pb",
      &t));
  cel::Activation act;
  EXPECT_THAT(PopulateActivation(t, act),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(PopulateActivation, ErrorBindingIsUnimplemented) {
  SimpleTest t;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(bindings {
             key: "e"
             value { error { errors {} } }
           })pb",
      &t));
  cel::Activation act;
  EXPECT_THAT(PopulateActivation(t, act),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(PopulateActivation, AggregateValueBindingIsUnimplemented) {
  SimpleTest t;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(bindings {
             key: "l"
             value { value { list_value {} } }
           })pb",
      &t));
  cel::Activation act;
  EXPECT_THAT(PopulateActivation(t, act),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(PopulateVariableSpecs, AppendsScalarSpecs) {
  SimpleTest t;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        type_env {
          name: "x"
          ident { type { primitive: INT64 } }
        }
        type_env {
          name: "y"
          ident { type { primitive: STRING } }
        }
      )pb",
      &t));
  std::vector<std::string> out;
  ASSERT_THAT(PopulateVariableSpecs(t, out), IsOk());
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], "x:int");
  EXPECT_EQ(out[1], "y:string");
}

TEST(PopulateVariableSpecs, AggregateDeclSurfaceUnimplemented) {
  SimpleTest t;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(type_env {
             name: "m"
             ident { type { message_type: "foo.Bar" } }
           })pb",
      &t));
  std::vector<std::string> out;
  EXPECT_THAT(PopulateVariableSpecs(t, out),
              StatusIs(absl::StatusCode::kUnimplemented));
}

}  // namespace
}  // namespace celwasm::conformance
