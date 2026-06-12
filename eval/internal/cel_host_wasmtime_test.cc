// HostExternrefTable unit tests — slot issuance, lookup, and the
// proto-identity dedup behaviour (`Intern` by `message()`,
// `InternProtoMessage` / `InternProtoMapField` / `InternProtoListField`
// by the underlying pointer keys), plus the `Reset()` contract that
// drops both the slots and the dedup indexes between Evals.

#include "eval/internal/cel_host_wasmtime.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "eval/internal/cel_host.h"
#include "eval/value.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

using ::celwasm::testdata::Address;
using ::celwasm::testdata::Customer;

// Minimal non-proto backing (`message()` inherits the nullptr
// default) — pins that custom embedder backings are NEVER deduped:
// the table has no identity key for them.
class FakeJsonBacking final : public HostMessageBacking {
 public:
  absl::StatusOr<celwasm::Value> ReadField(
      int /*field_number*/, absl::string_view /*field_name*/,
      const celwasm::CelType& /*expected_type*/) const override {
    return celwasm::Value::Null();
  }
  bool HasField(int /*field_number*/,
                absl::string_view /*field_name*/) const override {
    return false;
  }
};

const google::protobuf::FieldDescriptor* FieldOf(const Customer& c,
                                                 absl::string_view name) {
  const google::protobuf::FieldDescriptor* f =
      c.GetDescriptor()->FindFieldByName(std::string(name));
  ABSL_CHECK(f != nullptr) << "missing test field " << name;
  return f;
}

TEST(HostExternrefTableTest, InternIssuesMonotonicSlotsAndLooksUp) {
  HostExternrefTable table;
  Address a;
  Address b;
  const uint32_t slot_a = table.Intern(std::make_shared<ProtoBacking>(&a));
  const uint32_t slot_b = table.Intern(std::make_shared<ProtoBacking>(&b));
  EXPECT_EQ(slot_a, 1u);  // slot 0 is the sentinel
  EXPECT_EQ(slot_b, 2u);
  EXPECT_EQ(table.Lookup(slot_a)->message(), &a);
  EXPECT_EQ(table.Lookup(slot_b)->message(), &b);
  EXPECT_EQ(table.Lookup(0), nullptr);
}

TEST(HostExternrefTableTest, InternDedupsByUnderlyingMessage) {
  HostExternrefTable table;
  Address a;
  const uint32_t first = table.Intern(std::make_shared<ProtoBacking>(&a));
  // A distinct backing object over the SAME message dedups to the
  // already-issued slot — the two are observably identical.
  const uint32_t second = table.Intern(std::make_shared<ProtoBacking>(&a));
  EXPECT_EQ(first, second);
  EXPECT_EQ(table.Lookup(first)->message(), &a);
}

TEST(HostExternrefTableTest, InternNonProtoBackingNeverDedups) {
  HostExternrefTable table;
  auto backing = std::make_shared<FakeJsonBacking>();
  const uint32_t first = table.Intern(backing);
  const uint32_t second = table.Intern(backing);
  EXPECT_NE(first, second);
}

TEST(HostExternrefTableTest, InternProtoMessageDedupsSameMessage) {
  HostExternrefTable table;
  Address a;
  const uint32_t first = table.InternProtoMessage(&a);
  const uint32_t second = table.InternProtoMessage(&a);
  EXPECT_EQ(first, second);
  EXPECT_EQ(table.Lookup(first)->message(), &a);
}

TEST(HostExternrefTableTest, InternProtoMessageDistinctMessagesGetOwnSlots) {
  HostExternrefTable table;
  Address a;
  Address b;
  EXPECT_NE(table.InternProtoMessage(&a), table.InternProtoMessage(&b));
}

TEST(HostExternrefTableTest, InternThenInternProtoMessageShareOneSlot) {
  HostExternrefTable table;
  Address a;
  const uint32_t via_intern = table.Intern(std::make_shared<ProtoBacking>(&a));
  EXPECT_EQ(table.InternProtoMessage(&a), via_intern);
}

TEST(HostExternrefTableTest, InternProtoMessageRecordsOwnedBackingMessage) {
  HostExternrefTable table;
  auto owned = std::make_unique<Address>();
  const google::protobuf::Message* raw = owned.get();
  const uint32_t owned_slot =
      table.Intern(std::make_shared<OwnedProtoBacking>(std::move(owned)));
  // A later sub-object hop that resolves to the owned message's own
  // pointer reuses the owning slot rather than wrapping a second,
  // non-owning backing around it.
  EXPECT_EQ(table.InternProtoMessage(raw), owned_slot);
}

TEST(HostExternrefTableTest, InternProtoMapFieldDedupsByOwnerAndField) {
  HostExternrefTable table;
  Customer c;
  Customer other;
  const auto* metadata = FieldOf(c, "metadata");
  const auto* quotas = FieldOf(c, "tier_quotas");
  const uint32_t first = table.InternProtoMapField(&c, metadata);
  EXPECT_EQ(table.InternProtoMapField(&c, metadata), first);
  EXPECT_NE(table.InternProtoMapField(&c, quotas), first);
  EXPECT_NE(table.InternProtoMapField(&other, metadata), first);
  EXPECT_NE(table.LookupMap(first), nullptr);
}

TEST(HostExternrefTableTest, InternProtoListFieldDedupsByOwnerAndField) {
  HostExternrefTable table;
  Customer c;
  Customer other;
  const auto* tags = FieldOf(c, "tags");
  const uint32_t first = table.InternProtoListField(&c, tags);
  EXPECT_EQ(table.InternProtoListField(&c, tags), first);
  EXPECT_NE(table.InternProtoListField(&other, tags), first);
  EXPECT_NE(table.LookupList(first), nullptr);
}

TEST(HostExternrefTableTest, ResetDropsSlotsAndDedupIndexes) {
  HostExternrefTable table;
  Customer c;
  const auto* metadata = FieldOf(c, "metadata");
  const auto* tags = FieldOf(c, "tags");
  const uint32_t msg_slot = table.InternProtoMessage(&c);
  const uint32_t map_slot = table.InternProtoMapField(&c, metadata);
  const uint32_t list_slot = table.InternProtoListField(&c, tags);
  table.Reset();
  // Old slots no longer resolve …
  EXPECT_EQ(table.Lookup(msg_slot), nullptr);
  EXPECT_EQ(table.LookupMap(map_slot), nullptr);
  EXPECT_EQ(table.LookupList(list_slot), nullptr);
  // … and the dedup indexes were dropped with them: re-interning
  // issues fresh (post-sentinel) slots backed by fresh wrappers.
  EXPECT_EQ(table.InternProtoMessage(&c), 1u);
  EXPECT_EQ(table.InternProtoMapField(&c, metadata), 1u);
  EXPECT_EQ(table.InternProtoListField(&c, tags), 1u);
}

}  // namespace
}  // namespace celwasm
