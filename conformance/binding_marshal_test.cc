#include "conformance/binding_marshal.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "cel/expr/checked.pb.h"
#include "cel/expr/conformance/test/simple.pb.h"
#include "cel/expr/eval.pb.h"
#include "cel/expr/value.pb.h"
#include "eval/activation.h"
#include "eval/value.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"

namespace celwasm::conformance {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::cel::expr::Decl;
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

TEST(ValueFromProto, ObjectUnknownTypeRejected) {
  // M7: object_value with an unregistered type URL surfaces as
  // InvalidArgument (the descriptor pool has no entry for the type).
  Value v;
  v.mutable_object_value()->set_type_url(
      "type.googleapis.com/com.example.Unknown");
  EXPECT_THAT(ValueFromProto(v), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ValueFromProto, EnumDecodesToInt) {
  // M7: enum_value bindings decode to a kInt holding the numeric
  // value (langdef §"Enumerated Types": enums are spec-typed as int).
  Value v;
  v.mutable_enum_value()->set_type("e.E");
  v.mutable_enum_value()->set_value(7);
  auto out = ValueFromProto(v);
  ASSERT_TRUE(out.ok()) << out.status();
  ASSERT_EQ(out->kind(), celwasm::Value::Kind::kInt);
  EXPECT_EQ(*out->AsInt(), 7);
}

TEST(ValueFromProto, TypeProducesValueType) {
  // M9.A: type_value bindings now decode to Value::Type(name) — the
  // proto string maps verbatim to the kType payload (no name validation).
  Value v;
  v.set_type_value("int");
  auto out_or = ValueFromProto(v);
  ASSERT_THAT(out_or, IsOk());
  ASSERT_EQ(out_or->kind(), celwasm::Value::Kind::kType);
  ASSERT_THAT(out_or->AsType(), IsOk());
  EXPECT_EQ(*out_or->AsType(), "int");
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

TEST(VariableSpecFromDecl, MessageTypeFqnPreserved) {
  // M7: message_type decls return `name:<FQN>`; the spec parser
  // routes through ParseMessageType → DescriptorPool lookup.
  Decl d;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(name: "m"
           ident { type { message_type: "foo.Bar" } })pb",
      &d));
  auto got = VariableSpecFromDecl(d);
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got, "m:foo.Bar");
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
  celwasm::Activation act;
  ASSERT_THAT(PopulateActivation(t, act), IsOk());
  auto b = act.Resolve("b");
  ASSERT_THAT(b, IsOk());
  ASSERT_NE(*b, nullptr);
  EXPECT_EQ(*(*b)->AsBool(), true);
  auto i = act.Resolve("i");
  ASSERT_THAT(i, IsOk());
  ASSERT_NE(*i, nullptr);
  EXPECT_EQ(*(*i)->AsInt(), 7);
  auto s = act.Resolve("s");
  ASSERT_THAT(s, IsOk());
  ASSERT_NE(*s, nullptr);
  EXPECT_EQ(*(*s)->AsString(), "hi");
}

TEST(PopulateActivation, UnknownBindingIsUnimplemented) {
  SimpleTest t;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(bindings {
             key: "u"
             value { unknown { exprs: 1 } }
           })pb",
      &t));
  celwasm::Activation act;
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
  celwasm::Activation act;
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
  celwasm::Activation act;
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

TEST(PopulateVariableSpecs, MessageTypeDeclEmitted) {
  // M7: message_type decls now graduate from Unimplemented to a
  // `name:<FQN>` spec the checker resolves against the descriptor
  // pool.
  SimpleTest t;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(type_env {
             name: "m"
             ident { type { message_type: "foo.Bar" } }
           })pb",
      &t));
  std::vector<std::string> out;
  ASSERT_THAT(PopulateVariableSpecs(t, out), IsOk());
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], "m:foo.Bar");
}

}  // namespace
}  // namespace celwasm::conformance
