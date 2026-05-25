// ProtoMap (M3.G) — proto-reflection over a single map field.
// Mirrors the HostMap test matrix on a real proto fixture so the
// host-surface contract is identical regardless of backing.
//
// Lives separate from cel_host_test.cc so it doesn't pull in the
// wasmtime-trampoline scaffolding (still pending behind M2.C.0b).

#include "eval/internal/cel_host.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "eval/error.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/host_fixture_proto3.pb.h"

namespace celwasm {
namespace {

#define PM_ASSIGN_OR_ASSERT(lhs, expr)             \
  auto lhs##_or = (expr);                          \
  ASSERT_TRUE(lhs##_or.ok()) << lhs##_or.status(); \
  auto lhs = *std::move(lhs##_or)

using ::celwasm::testdata::HostMsg3;

[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<HostMsg3>();
      return 0;
    }();

const google::protobuf::FieldDescriptor* absl_nonnull MapField(
    const HostMsg3& m, const char* name) {
  const auto* fd = m.GetDescriptor()->FindFieldByName(name);
  ABSL_CHECK(fd != nullptr) << name;
  ABSL_CHECK(fd->is_map()) << name;
  return fd;
}

// ════════ ProtoBacking::ReadField → HostMap(ProtoMap) ════════

TEST(ProtoBackingMapTest, ReadFieldReturnsHostMapForMapField) {
  HostMsg3 m;
  (*m.mutable_str_to_i32())["alpha"] = 1;
  (*m.mutable_str_to_i32())["beta"] = 2;

  ProtoBacking pb(&m);
  PM_ASSIGN_OR_ASSERT(v, pb.ReadField(/*field_number=*/19, "str_to_i32",
                                      celwasm::api::CelType::Int()));
  ASSERT_EQ(v.kind(), celwasm::api::Value::Kind::kMap);
  PM_ASSIGN_OR_ASSERT(b, v.MapBacking());
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->Size(), 2u);
}

TEST(ProtoBackingMapTest, EmptyMapReadsAsZeroSizedHostMap) {
  HostMsg3 m;  // map empty by default
  ProtoBacking pb(&m);
  PM_ASSIGN_OR_ASSERT(
      v, pb.ReadField(19, "str_to_i32", celwasm::api::CelType::Int()));
  PM_ASSIGN_OR_ASSERT(b, v.MapBacking());
  EXPECT_EQ(b->Size(), 0u);
}

// ════════ ProtoMap::Get — per-key-kind round-trip ════════

TEST(ProtoMapTest, StringKeyHits) {
  HostMsg3 m;
  (*m.mutable_str_to_i32())["alpha"] = 10;
  (*m.mutable_str_to_i32())["beta"] = 20;
  ProtoMap pm(&m, MapField(m, "str_to_i32"));

  PM_ASSIGN_OR_ASSERT(v, pm.Get(celwasm::api::Value::String("beta"),
                                celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 20);
}

TEST(ProtoMapTest, IntKeyHits) {
  HostMsg3 m;
  (*m.mutable_i64_to_str())[7] = "seven";
  (*m.mutable_i64_to_str())[42] = "forty-two";
  ProtoMap pm(&m, MapField(m, "i64_to_str"));

  PM_ASSIGN_OR_ASSERT(
      v, pm.Get(celwasm::api::Value::Int(42), celwasm::api::CelType::String()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kString);
  EXPECT_EQ(*v.AsString(), "forty-two");
}

TEST(ProtoMapTest, UintKeyHits) {
  HostMsg3 m;
  (*m.mutable_u32_to_f64())[3] = 3.14;
  ProtoMap pm(&m, MapField(m, "u32_to_f64"));

  PM_ASSIGN_OR_ASSERT(
      v, pm.Get(celwasm::api::Value::Uint(3), celwasm::api::CelType::Double()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kDouble);
  EXPECT_DOUBLE_EQ(*v.AsDouble(), 3.14);
}

TEST(ProtoMapTest, BoolKeyHits) {
  HostMsg3 m;
  (*m.mutable_bool_to_i64())[true] = 111;
  (*m.mutable_bool_to_i64())[false] = 222;
  ProtoMap pm(&m, MapField(m, "bool_to_i64"));

  PM_ASSIGN_OR_ASSERT(vt, pm.Get(celwasm::api::Value::Bool(true),
                                 celwasm::api::CelType::Int()));
  EXPECT_EQ(*vt.AsInt(), 111);
  PM_ASSIGN_OR_ASSERT(vf, pm.Get(celwasm::api::Value::Bool(false),
                                 celwasm::api::CelType::Int()));
  EXPECT_EQ(*vf.AsInt(), 222);
}

// ════════ Cross-type equality (langdef §"Equality") ════════

TEST(ProtoMapTest, IntKeyHitsViaUintLookup) {
  // Map stores int64 keys; look up via Uint(42) — cross-type
  // numeric equality matches.  Mirrors the HostMap behaviour so
  // proto- and vector-backed maps stay interchangeable.
  HostMsg3 m;
  (*m.mutable_i64_to_str())[42] = "match";
  ProtoMap pm(&m, MapField(m, "i64_to_str"));

  PM_ASSIGN_OR_ASSERT(v, pm.Get(celwasm::api::Value::Uint(42),
                                celwasm::api::CelType::String()));
  EXPECT_EQ(*v.AsString(), "match");
}

TEST(ProtoMapTest, NegativeIntNeverMatchesUintKey) {
  HostMsg3 m;
  (*m.mutable_u32_to_f64())[0] = 99.0;
  ProtoMap pm(&m, MapField(m, "u32_to_f64"));

  PM_ASSIGN_OR_ASSERT(
      v, pm.Get(celwasm::api::Value::Int(-1), celwasm::api::CelType::Double()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kError);
  PM_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kKeyNotFound);
}

// ════════ Miss / invalid key kind ════════

TEST(ProtoMapTest, MissingKeyReturnsNoSuchKey) {
  HostMsg3 m;
  (*m.mutable_str_to_i32())["alpha"] = 1;
  ProtoMap pm(&m, MapField(m, "str_to_i32"));

  PM_ASSIGN_OR_ASSERT(v, pm.Get(celwasm::api::Value::String("missing"),
                                celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kError);
  PM_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kKeyNotFound);
}

TEST(ProtoMapTest, InvalidKeyKindReturnsTypeMismatch) {
  HostMsg3 m;
  ProtoMap pm(&m, MapField(m, "str_to_i32"));

  PM_ASSIGN_OR_ASSERT(v, pm.Get(celwasm::api::Value::Double(1.0),
                                celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kError);
  PM_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kTypeMismatch);
}

// ════════ ContainsKey / ForEach / message values ════════

TEST(ProtoMapTest, ContainsKeyMatchesGetSemantics) {
  HostMsg3 m;
  (*m.mutable_str_to_i32())["alpha"] = 1;
  ProtoMap pm(&m, MapField(m, "str_to_i32"));

  EXPECT_TRUE(pm.ContainsKey(celwasm::api::Value::String("alpha")));
  EXPECT_FALSE(pm.ContainsKey(celwasm::api::Value::String("missing")));
  EXPECT_FALSE(
      pm.ContainsKey(celwasm::api::Value::Double(1.0)));  // invalid kind
}

TEST(ProtoMapTest, ForEachVisitsAllEntries) {
  HostMsg3 m;
  (*m.mutable_i64_to_str())[1] = "one";
  (*m.mutable_i64_to_str())[2] = "two";
  (*m.mutable_i64_to_str())[3] = "three";
  ProtoMap pm(&m, MapField(m, "i64_to_str"));

  std::vector<int64_t> keys;
  std::vector<std::string> values;
  pm.ForEach([&](const celwasm::api::Value& k, const celwasm::api::Value& v) {
    keys.push_back(*k.AsInt());
    values.push_back(std::string(*v.AsString()));
  });
  EXPECT_EQ(keys.size(), 3u);
  EXPECT_EQ(values.size(), 3u);
}

// Map values that are themselves messages — proto allows this; CEL
// reads them as `Value::HostMessage(ProtoBacking)`.
TEST(ProtoMapTest, MessageValueWrapsAsHostMessage) {
  HostMsg3 m;
  HostMsg3 inner;
  inner.set_i32(42);
  (*m.mutable_str_to_msg())["k"] = inner;
  ProtoMap pm(&m, MapField(m, "str_to_msg"));

  PM_ASSIGN_OR_ASSERT(v, pm.Get(celwasm::api::Value::String("k"),
                                celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kMessage);
  PM_ASSIGN_OR_ASSERT(b, v.MessageBacking());
  ASSERT_NE(b, nullptr);
  // Read back the i32 field through the wrapped backing.
  PM_ASSIGN_OR_ASSERT(i, b->ReadField(2, "i32", celwasm::api::CelType::Int()));
  EXPECT_EQ(*i.AsInt(), 42);
}

#undef PM_ASSIGN_OR_ASSERT

}  // namespace
}  // namespace celwasm
