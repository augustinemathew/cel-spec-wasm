// ProtoList (M4.D) — proto-reflection over a single REPEATED
// (non-map) field.  Mirrors proto_map_test.cc shape against the
// HostMsg3 `rep_*` fixture fields.

#include "compiler_v2/api/internal/cel_host.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "compiler_v2/api/error.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "compiler_v2/testdata/host_fixture_proto3.pb.h"
#include "google/protobuf/descriptor.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

#define PL_ASSIGN_OR_ASSERT(lhs, expr)             \
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

const google::protobuf::FieldDescriptor* absl_nonnull RepeatedField(
    const HostMsg3& m, const char* name) {
  const auto* fd = m.GetDescriptor()->FindFieldByName(name);
  ABSL_CHECK(fd != nullptr) << name;
  ABSL_CHECK(fd->is_repeated()) << name;
  ABSL_CHECK(!fd->is_map()) << name;
  return fd;
}

// ════════ ProtoList::At — per-element-kind round-trip ════════

TEST(ProtoListTest, RepeatedInt32Hits) {
  HostMsg3 m;
  m.add_rep_i32(11);
  m.add_rep_i32(22);
  m.add_rep_i32(33);
  ProtoList pl(&m, RepeatedField(m, "rep_i32"));
  EXPECT_EQ(pl.Size(), 3u);

  PL_ASSIGN_OR_ASSERT(v, pl.At(1, celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 22);
}

TEST(ProtoListTest, RepeatedStringHits) {
  HostMsg3 m;
  m.add_rep_s("alpha");
  m.add_rep_s("beta");
  ProtoList pl(&m, RepeatedField(m, "rep_s"));

  PL_ASSIGN_OR_ASSERT(v, pl.At(0, celwasm::api::CelType::String()));
  EXPECT_EQ(*v.AsString(), "alpha");
}

TEST(ProtoListTest, RepeatedBoolHits) {
  HostMsg3 m;
  m.add_rep_b(true);
  m.add_rep_b(false);
  ProtoList pl(&m, RepeatedField(m, "rep_b"));

  PL_ASSIGN_OR_ASSERT(v0, pl.At(0, celwasm::api::CelType::Bool()));
  EXPECT_EQ(*v0.AsBool(), true);
  PL_ASSIGN_OR_ASSERT(v1, pl.At(1, celwasm::api::CelType::Bool()));
  EXPECT_EQ(*v1.AsBool(), false);
}

TEST(ProtoListTest, RepeatedDoubleHits) {
  HostMsg3 m;
  m.add_rep_f64(1.5);
  m.add_rep_f64(-2.25);
  ProtoList pl(&m, RepeatedField(m, "rep_f64"));

  PL_ASSIGN_OR_ASSERT(v, pl.At(1, celwasm::api::CelType::Double()));
  EXPECT_EQ(*v.AsDouble(), -2.25);
}

TEST(ProtoListTest, RepeatedMessageWrapsAsHostMessage) {
  HostMsg3 m;
  m.add_rep_msg()->set_i64(99);
  m.add_rep_msg()->set_i64(101);
  ProtoList pl(&m, RepeatedField(m, "rep_msg"));

  PL_ASSIGN_OR_ASSERT(v, pl.At(1, celwasm::api::CelType::Int()));
  ASSERT_EQ(v.kind(), celwasm::api::Value::Kind::kMessage);
  PL_ASSIGN_OR_ASSERT(b, v.MessageBacking());
  ASSERT_NE(b, nullptr);
}

// Repeated WKT (Timestamp) elements peel to CEL_TIMESTAMP, mirroring
// the singular-field WKT read path — not a raw HostMessage.
TEST(ProtoListTest, RepeatedTimestampPeelsToTimestamp) {
  HostMsg3 m;
  m.add_repeated_timestamp()->set_seconds(7);
  ProtoList pl(&m, RepeatedField(m, "repeated_timestamp"));
  PL_ASSIGN_OR_ASSERT(v, pl.At(0, celwasm::api::CelType::Timestamp()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kTimestamp);
}

// ════════ Boundary ════════

TEST(ProtoListTest, EmptyRepeatedReadsAsZeroSize) {
  HostMsg3 m;  // no rep_i32 added
  ProtoList pl(&m, RepeatedField(m, "rep_i32"));
  EXPECT_EQ(pl.Size(), 0u);

  PL_ASSIGN_OR_ASSERT(v, pl.At(0, celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kError);
  PL_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kIndexOutOfBounds);
}

TEST(ProtoListTest, IndexAtCountErrors) {
  HostMsg3 m;
  m.add_rep_i32(11);
  ProtoList pl(&m, RepeatedField(m, "rep_i32"));
  PL_ASSIGN_OR_ASSERT(v, pl.At(1, celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kError);
  PL_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kIndexOutOfBounds);
}

// ════════ ForEach ════════

TEST(ProtoListTest, ForEachVisitsAllInOrder) {
  HostMsg3 m;
  m.add_rep_i32(10);
  m.add_rep_i32(20);
  m.add_rep_i32(30);
  ProtoList pl(&m, RepeatedField(m, "rep_i32"));
  std::vector<int64_t> seen;
  pl.ForEach([&](const celwasm::api::Value& v) {
    seen.push_back(*v.AsInt());
  });
  EXPECT_EQ(seen, std::vector<int64_t>({10, 20, 30}));
}

// ════════ ProtoBacking::ReadField on REPEATED → HostList ════════
//
// M4.G flipped: REPEATED fields now return
// `Value::HostList(ProtoList{...})` mirroring the M3.G map pattern.
TEST(ProtoBackingListTest, ReadFieldRepeatedReturnsHostList) {
  HostMsg3 m;
  m.add_rep_i32(7);
  m.add_rep_i32(8);
  ProtoBacking pb(&m);
  PL_ASSIGN_OR_ASSERT(v, pb.ReadField(/*field_number=*/18, "rep_i32",
                                      celwasm::api::CelType::Int()));
  ASSERT_EQ(v.kind(), celwasm::api::Value::Kind::kList);
  PL_ASSIGN_OR_ASSERT(b, v.ListBacking());
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->Size(), 2u);
}

#undef PL_ASSIGN_OR_ASSERT

}  // namespace
}  // namespace celwasm
