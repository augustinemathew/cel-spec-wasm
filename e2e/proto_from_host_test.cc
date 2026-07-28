// Proto construction from host-origin aggregates, e2e.
//
// Every case compiles + evaluates through the production pipeline
// (Compiler::Compile → Engine::Plan → Instance::Eval) with the
// aggregate bound through the Activation — so the value reaches
// `cel_set_field` as CEL_MAP_HOST / CEL_LIST_HOST and exercises the
// host-backed setter matrix in eval/internal/cel_host.cc
// (SetHostMapEntry{Key,Value} families, AppendRepeatedFromHostListValue
// families).  The literal-aggregate (arena) construction paths are
// covered by e2e/wkt_field_set_test.cc; this suite is the host-origin
// complement, plus the message-backed collection reads that only a
// bound proto can reach (ProtoMap::ContainsKey, ProtoList::ForEach,
// host temporal/numeric backing equality).
//
// Matrix per doc/langdef.md §Field Selection / message construction:
// map key kinds are the closed set bool/int/uint/string; map value and
// repeated element kinds cover every scalar cpp_type plus enum,
// message, and Duration.  Each case asserts the constructed proto
// byte-equals the expected message (MessageDifferencer), the same
// check the conformance harness's object_value matcher applies.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "cel/expr/conformance/proto3/test_all_types.pb.h"
#include "compiler/compiler.h"
#include "compiler/program.h"
#include "e2e/link_mode_e2e_helpers.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/internal/cel_host.h"  // HostMessageBacking
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "google/protobuf/util/message_differencer.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::cel::expr::conformance::proto3::TestAllTypes;

[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<TestAllTypes>();
      return 0;
    }();

using ::celwasm::e2e::GlobalEngine;

constexpr absl::string_view kP3 = "cel.expr.conformance.proto3";
constexpr absl::string_view kNested =
    "cel.expr.conformance.proto3.TestAllTypes.NestedMessage";

using Decls = std::vector<std::pair<std::string, CelType>>;
using Binds = std::vector<std::pair<std::string, Value>>;

// Compile `source` with `decls` declared, plan it, bind `binds`, eval.
Value EvalBound(absl::string_view source, const Decls& decls, Binds binds) {
  auto b = Compiler::NewBuilder();
  for (const auto& [name, type] : decls) {
    b.DeclareVariable(name, type);
  }
  auto compiler = std::move(b).Build();
  ABSL_CHECK_OK(compiler.status());
  CompilerOptions opts;
  opts.container = std::string(kP3);
  opts.link_mode = e2e::kE2ELinkMode;
  auto program = compiler->Compile(source, opts);
  ABSL_CHECK_OK(program.status()) << source;
  auto instance = GlobalEngine().Plan(*program);
  ABSL_CHECK_OK(instance.status()) << source;
  Activation act;
  for (auto& [name, value] : binds) {
    act.Bind(name, std::move(value));
  }
  auto v = instance->Eval(act);
  ABSL_CHECK_OK(v.status()) << source;
  return *std::move(v);
}

// Assert `source` (a TestAllTypes construction) builds `expected`.
void ExpectConstructs(absl::string_view source, const Decls& decls, Binds binds,
                      const TestAllTypes& expected) {
  Value v = EvalBound(source, decls, std::move(binds));
  auto backing = v.MessageBacking();
  ASSERT_THAT(backing, IsOk())
      << source << " (kind=" << static_cast<int>(v.kind()) << ")";
  const google::protobuf::Message* got = (*backing)->message();
  ASSERT_NE(got, nullptr) << source;
  std::string diff;
  google::protobuf::util::MessageDifferencer differ;
  differ.ReportDifferencesToString(&diff);
  EXPECT_TRUE(differ.Compare(expected, *got)) << source << "\ndiff:\n" << diff;
}

// Assert the boolean `source` evaluates true under `decls`/`binds`.
void ExpectBoundTrue(absl::string_view source, const Decls& decls,
                     Binds binds) {
  Value v = EvalBound(source, decls, std::move(binds));
  auto b = v.AsBool();
  ASSERT_THAT(b, IsOk()) << source << " (kind=" << static_cast<int>(v.kind())
                         << ")";
  EXPECT_TRUE(*b) << source;
}

Value NestedMsg(int32_t bb) {
  auto m = std::make_unique<TestAllTypes::NestedMessage>();
  m->set_bb(bb);
  return Value::OwnedMessage(std::move(m));
}

// ── host map → proto map field: every key kind ───────────────────────

class HostMapKeyMatrix : public ::testing::Test {};

TEST_F(HostMapKeyMatrix, Int32Key) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_int64())[1] = 2;
  (*expected.mutable_map_int32_int64())[-3] = 4;
  ExpectConstructs("TestAllTypes{map_int32_int64: m}",
                   {{"m", CelType::Map(CelType::Int(), CelType::Int())}},
                   {{"m", Value::Map({{Value::Int(1), Value::Int(2)},
                                      {Value::Int(-3), Value::Int(4)}})}},
                   expected);
}

TEST_F(HostMapKeyMatrix, Int64Key) {
  TestAllTypes expected;
  (*expected.mutable_map_int64_int64())[int64_t{1} << 40] = -9;
  ExpectConstructs(
      "TestAllTypes{map_int64_int64: m}",
      {{"m", CelType::Map(CelType::Int(), CelType::Int())}},
      {{"m", Value::Map({{Value::Int(int64_t{1} << 40), Value::Int(-9)}})}},
      expected);
}

TEST_F(HostMapKeyMatrix, Uint32Key) {
  TestAllTypes expected;
  (*expected.mutable_map_uint32_uint32())[5u] = 6u;
  ExpectConstructs("TestAllTypes{map_uint32_uint32: m}",
                   {{"m", CelType::Map(CelType::Uint(), CelType::Uint())}},
                   {{"m", Value::Map({{Value::Uint(5), Value::Uint(6)}})}},
                   expected);
}

TEST_F(HostMapKeyMatrix, Uint64Key) {
  TestAllTypes expected;
  (*expected.mutable_map_uint64_uint64())[UINT64_MAX] = 8u;
  ExpectConstructs(
      "TestAllTypes{map_uint64_uint64: m}",
      {{"m", CelType::Map(CelType::Uint(), CelType::Uint())}},
      {{"m", Value::Map({{Value::Uint(UINT64_MAX), Value::Uint(8)}})}},
      expected);
}

TEST_F(HostMapKeyMatrix, BoolKey) {
  TestAllTypes expected;
  (*expected.mutable_map_bool_bool())[true] = false;
  ExpectConstructs(
      "TestAllTypes{map_bool_bool: m}",
      {{"m", CelType::Map(CelType::Bool(), CelType::Bool())}},
      {{"m", Value::Map({{Value::Bool(true), Value::Bool(false)}})}}, expected);
}

TEST_F(HostMapKeyMatrix, StringKey) {
  TestAllTypes expected;
  (*expected.mutable_map_string_string())["k"] = "v";
  ExpectConstructs(
      "TestAllTypes{map_string_string: m}",
      {{"m", CelType::Map(CelType::String(), CelType::String())}},
      {{"m", Value::Map({{Value::String("k"), Value::String("v")}})}},
      expected);
}

// ── host map → proto map field: every value cpp_type ─────────────────

class HostMapValueMatrix : public ::testing::Test {};

TEST_F(HostMapValueMatrix, Int32Value) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_int32())[1] = -7;
  ExpectConstructs("TestAllTypes{map_int32_int32: m}",
                   {{"m", CelType::Map(CelType::Int(), CelType::Int())}},
                   {{"m", Value::Map({{Value::Int(1), Value::Int(-7)}})}},
                   expected);
}

TEST_F(HostMapValueMatrix, Uint32Value) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_uint32())[1] = 9u;
  ExpectConstructs("TestAllTypes{map_int32_uint32: m}",
                   {{"m", CelType::Map(CelType::Int(), CelType::Uint())}},
                   {{"m", Value::Map({{Value::Int(1), Value::Uint(9)}})}},
                   expected);
}

TEST_F(HostMapValueMatrix, Uint64Value) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_uint64())[1] = UINT64_MAX;
  ExpectConstructs(
      "TestAllTypes{map_int32_uint64: m}",
      {{"m", CelType::Map(CelType::Int(), CelType::Uint())}},
      {{"m", Value::Map({{Value::Int(1), Value::Uint(UINT64_MAX)}})}},
      expected);
}

TEST_F(HostMapValueMatrix, FloatValue) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_float())[1] = 1.5f;
  ExpectConstructs("TestAllTypes{map_int32_float: m}",
                   {{"m", CelType::Map(CelType::Int(), CelType::Double())}},
                   {{"m", Value::Map({{Value::Int(1), Value::Double(1.5)}})}},
                   expected);
}

TEST_F(HostMapValueMatrix, DoubleValue) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_double())[1] = -2.25;
  ExpectConstructs("TestAllTypes{map_int32_double: m}",
                   {{"m", CelType::Map(CelType::Int(), CelType::Double())}},
                   {{"m", Value::Map({{Value::Int(1), Value::Double(-2.25)}})}},
                   expected);
}

TEST_F(HostMapValueMatrix, StringValue) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_string())[1] = "s";
  ExpectConstructs("TestAllTypes{map_int32_string: m}",
                   {{"m", CelType::Map(CelType::Int(), CelType::String())}},
                   {{"m", Value::Map({{Value::Int(1), Value::String("s")}})}},
                   expected);
}

TEST_F(HostMapValueMatrix, BytesValueEmbeddedNul) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_bytes())[1] = std::string("a\0b", 3);
  ExpectConstructs(
      "TestAllTypes{map_int32_bytes: m}",
      {{"m", CelType::Map(CelType::Int(), CelType::Bytes())}},
      {{"m",
        Value::Map({{Value::Int(1), Value::Bytes(std::string("a\0b", 3))}})}},
      expected);
}

TEST_F(HostMapValueMatrix, EnumValueFromInt) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_enum())[1] = TestAllTypes::BAR;
  ExpectConstructs("TestAllTypes{map_int32_enum: m}",
                   {{"m", CelType::Map(CelType::Int(), CelType::Int())}},
                   {{"m", Value::Map({{Value::Int(1), Value::Int(1)}})}},
                   expected);
}

TEST_F(HostMapValueMatrix, DurationValue) {
  TestAllTypes expected;
  (*expected.mutable_map_bool_duration())[true].set_seconds(90);
  ExpectConstructs(
      "TestAllTypes{map_bool_duration: m}",
      {{"m", CelType::Map(CelType::Bool(), CelType::Duration())}},
      {{"m",
        Value::Map({{Value::Bool(true), Value::Duration(absl::Seconds(90))}})}},
      expected);
}

TEST_F(HostMapValueMatrix, TimestampValue) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_timestamp())[1].set_seconds(1234567890);
  ExpectConstructs(
      "TestAllTypes{map_int32_timestamp: m}",
      {{"m", CelType::Map(CelType::Int(), CelType::Timestamp())}},
      {{"m", Value::Map({{Value::Int(1), Value::Timestamp(absl::FromUnixSeconds(
                                             1234567890))}})}},
      expected);
}

TEST_F(HostMapValueMatrix, MessageValueNullPruned) {
  // A null value of a message-typed map field prunes the whole entry
  // (parity with the arena path's set_null/map_*_null_pruned rows).
  TestAllTypes expected;
  (*expected.mutable_map_int32_message())[1].set_bb(7);
  ExpectConstructs(
      "TestAllTypes{map_int32_message: m}",
      {{"m",
        CelType::Map(CelType::Int(), CelType::Message(std::string(kNested)))}},
      {{"m", Value::Map({{Value::Int(1), NestedMsg(7)},
                         {Value::Int(2), Value::Null()}})}},
      expected);
}

TEST_F(HostMapValueMatrix, MessageValue) {
  TestAllTypes expected;
  (*expected.mutable_map_int32_message())[1].set_bb(7);
  ExpectConstructs(
      "TestAllTypes{map_int32_message: m}",
      {{"m",
        CelType::Map(CelType::Int(), CelType::Message(std::string(kNested)))}},
      {{"m", Value::Map({{Value::Int(1), NestedMsg(7)}})}}, expected);
}

// ── host list → proto repeated field: every element cpp_type ─────────

class HostRepeatedMatrix : public ::testing::Test {};

TEST_F(HostRepeatedMatrix, Int32Elements) {
  TestAllTypes expected;
  expected.add_repeated_int32(1);
  expected.add_repeated_int32(-2);
  ExpectConstructs("TestAllTypes{repeated_int32: xs}",
                   {{"xs", CelType::List(CelType::Int())}},
                   {{"xs", Value::List({Value::Int(1), Value::Int(-2)})}},
                   expected);
}

TEST_F(HostRepeatedMatrix, Int64Elements) {
  TestAllTypes expected;
  expected.add_repeated_int64(int64_t{1} << 40);
  ExpectConstructs("TestAllTypes{repeated_int64: xs}",
                   {{"xs", CelType::List(CelType::Int())}},
                   {{"xs", Value::List({Value::Int(int64_t{1} << 40)})}},
                   expected);
}

TEST_F(HostRepeatedMatrix, Uint32Elements) {
  TestAllTypes expected;
  expected.add_repeated_uint32(7u);
  ExpectConstructs("TestAllTypes{repeated_uint32: xs}",
                   {{"xs", CelType::List(CelType::Uint())}},
                   {{"xs", Value::List({Value::Uint(7)})}}, expected);
}

TEST_F(HostRepeatedMatrix, Uint64Elements) {
  TestAllTypes expected;
  expected.add_repeated_uint64(UINT64_MAX);
  ExpectConstructs("TestAllTypes{repeated_uint64: xs}",
                   {{"xs", CelType::List(CelType::Uint())}},
                   {{"xs", Value::List({Value::Uint(UINT64_MAX)})}}, expected);
}

TEST_F(HostRepeatedMatrix, FloatElements) {
  TestAllTypes expected;
  expected.add_repeated_float(1.5f);
  ExpectConstructs("TestAllTypes{repeated_float: xs}",
                   {{"xs", CelType::List(CelType::Double())}},
                   {{"xs", Value::List({Value::Double(1.5)})}}, expected);
}

TEST_F(HostRepeatedMatrix, DoubleElements) {
  TestAllTypes expected;
  expected.add_repeated_double(-2.25);
  ExpectConstructs("TestAllTypes{repeated_double: xs}",
                   {{"xs", CelType::List(CelType::Double())}},
                   {{"xs", Value::List({Value::Double(-2.25)})}}, expected);
}

TEST_F(HostRepeatedMatrix, BoolElements) {
  TestAllTypes expected;
  expected.add_repeated_bool(true);
  expected.add_repeated_bool(false);
  ExpectConstructs(
      "TestAllTypes{repeated_bool: xs}",
      {{"xs", CelType::List(CelType::Bool())}},
      {{"xs", Value::List({Value::Bool(true), Value::Bool(false)})}}, expected);
}

TEST_F(HostRepeatedMatrix, StringElements) {
  TestAllTypes expected;
  expected.add_repeated_string("a");
  ExpectConstructs("TestAllTypes{repeated_string: xs}",
                   {{"xs", CelType::List(CelType::String())}},
                   {{"xs", Value::List({Value::String("a")})}}, expected);
}

TEST_F(HostRepeatedMatrix, BytesElementsEmbeddedNul) {
  TestAllTypes expected;
  expected.add_repeated_bytes(std::string("a\0b", 3));
  ExpectConstructs(
      "TestAllTypes{repeated_bytes: xs}",
      {{"xs", CelType::List(CelType::Bytes())}},
      {{"xs", Value::List({Value::Bytes(std::string("a\0b", 3))})}}, expected);
}

TEST_F(HostRepeatedMatrix, EnumElementsFromInt) {
  TestAllTypes expected;
  expected.add_repeated_nested_enum(TestAllTypes::BAR);
  ExpectConstructs("TestAllTypes{repeated_nested_enum: xs}",
                   {{"xs", CelType::List(CelType::Int())}},
                   {{"xs", Value::List({Value::Int(1)})}}, expected);
}

TEST_F(HostRepeatedMatrix, MessageElements) {
  TestAllTypes expected;
  expected.add_repeated_nested_message()->set_bb(3);
  ExpectConstructs(
      "TestAllTypes{repeated_nested_message: xs}",
      {{"xs", CelType::List(CelType::Message(std::string(kNested)))}},
      {{"xs", Value::List({NestedMsg(3)})}}, expected);
}

TEST_F(HostRepeatedMatrix, DurationElements) {
  TestAllTypes expected;
  expected.add_repeated_duration()->set_seconds(-90);
  ExpectConstructs("TestAllTypes{repeated_duration: xs}",
                   {{"xs", CelType::List(CelType::Duration())}},
                   {{"xs", Value::List({Value::Duration(absl::Seconds(-90))})}},
                   expected);
}

TEST_F(HostRepeatedMatrix, TimestampElements) {
  TestAllTypes expected;
  expected.add_repeated_timestamp()->set_seconds(1234567890);
  ExpectConstructs(
      "TestAllTypes{repeated_timestamp: xs}",
      {{"xs", CelType::List(CelType::Timestamp())}},
      {{"xs",
        Value::List({Value::Timestamp(absl::FromUnixSeconds(1234567890))})}},
      expected);
}

TEST_F(HostRepeatedMatrix, MessageElementNullPruned) {
  // A null element of a message-typed repeated field is pruned, not
  // appended (parity with the arena path's repeated_*_null_pruned).
  TestAllTypes expected;
  expected.add_repeated_nested_message()->set_bb(3);
  ExpectConstructs(
      "TestAllTypes{repeated_nested_message: xs}",
      {{"xs", CelType::List(CelType::Message(std::string(kNested)))}},
      {{"xs", Value::List({NestedMsg(3), Value::Null()})}}, expected);
}

// ── message-backed collection reads only a bound proto reaches ───────

class MessageBackedReadTest : public ::testing::Test {};

Value BoundTestAllTypes(const std::function<void(TestAllTypes&)>& fill) {
  auto msg = std::make_unique<TestAllTypes>();
  fill(*msg);
  return Value::OwnedMessage(std::move(msg));
}

TEST_F(MessageBackedReadTest, InOperatorOnProtoMapField) {
  // ProtoMap::ContainsKey: `in` against the message-backed map view.
  ExpectBoundTrue(
      "5 in msg.map_int32_int64 && !(7 in msg.map_int32_int64)",
      {{"msg", CelType::Message("cel.expr.conformance.proto3.TestAllTypes")}},
      {{"msg", BoundTestAllTypes([](TestAllTypes& m) {
          (*m.mutable_map_int32_int64())[5] = 6;
        })}});
}

TEST_F(MessageBackedReadTest, ComprehensionOverProtoRepeatedField) {
  // ProtoList::ForEach: a macro over the message-backed list view.
  ExpectBoundTrue(
      "msg.repeated_int32.exists(x, x == 2) && "
      "msg.repeated_int32.all(x, x < 4)",
      {{"msg", CelType::Message("cel.expr.conformance.proto3.TestAllTypes")}},
      {{"msg", BoundTestAllTypes([](TestAllTypes& m) {
          m.add_repeated_int32(1);
          m.add_repeated_int32(2);
          m.add_repeated_int32(3);
        })}});
}

TEST_F(MessageBackedReadTest, BoundTimestampEqualsLiteral) {
  // TemporalBackingEqualsQuery: host-backed timestamp vs arena literal.
  ExpectBoundTrue(
      "t == timestamp('2009-02-13T23:31:30Z') && "
      "t != timestamp('2009-02-13T23:31:31Z')",
      {{"t", CelType::Timestamp()}},
      {{"t", Value::Timestamp(absl::FromUnixSeconds(1234567890))}});
}

TEST_F(MessageBackedReadTest, BoundDurationEqualsLiteral) {
  ExpectBoundTrue("d == duration('90s')", {{"d", CelType::Duration()}},
                  {{"d", Value::Duration(absl::Seconds(90))}});
}

TEST_F(MessageBackedReadTest, CrossNumericHostMapKeyLookup) {
  // Map-key equality is value-based across int/uint/double (langdef
  // §Maps): a host-bound map whose STORED key kind differs from the
  // declared key type still matches on numeric value.  Statically
  // `1.0 == 1` never type-checks, so the host lookup's cross-numeric
  // key matching (HostNumericCrossEq) is only reachable this way.
  ExpectBoundTrue("m[5] == 6",
                  {{"m", CelType::Map(CelType::Int(), CelType::Int())}},
                  {{"m", Value::Map({{Value::Uint(5), Value::Int(6)}})}});
}

}  // namespace
}  // namespace celwasm
