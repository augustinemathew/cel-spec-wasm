// Layer-2 trampoline body for `cel_host.cel_list_at`.  Drives
// CelListAtImpl through the shared FakeMemoryView /
// FakeExternrefTable / FakeArenaAllocator fakes; mirrors
// cel_map_lookup_impl_test's matrix on the list side.
//
// Coverage:
//   - 3VL on operand pair (unknown / error propagate)
//   - kHost arm dispatches into HostList::At / ProtoList::At
//   - non-CEL_LIST_HOST operand poisons TYPE_MISMATCH
//   - non-int index poisons TYPE_MISMATCH
//   - negative + >= count indices encode INDEX_OUT_OF_BOUNDS
//   - missing ref_slot surfaces a non-OK Status
//   - per-element-kind round-trip (int / string)
//   - nested message / list element interns into the right namespace

#include "compiler_v2/api/internal/cel_host.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "compiler/testdata/host_fixture_proto3.pb.h"
#include "compiler_v2/api/error.h"
#include "compiler_v2/api/internal/cel_host_test_fakes.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "compiler_v2/runtime/cel_data.h"
#include "google/protobuf/descriptor.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using FakeMemory = test::FakeMemoryView;
using FakeRefs = test::FakeExternrefTable;
using ::celwasm::testdata::HostMsg3;

[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<HostMsg3>();
      return 0;
    }();

struct Fixture {
  FakeMemory mem;
  FakeRefs refs;
  test::FakeArenaAllocator arena{&mem, /*base_offset=*/4096u,
                                  /*capacity=*/8192u};
  CelHostBindings bindings{};
  TrampolineContext ctx{bindings, mem, refs, arena};

  uint32_t list_slot = 24;
  uint32_t idx_slot = 48;
  uint32_t out_slot = 72;
};

CelValue MakeListValue(uint32_t ref_slot) {
  CelValue v{};
  v.kind = CEL_LIST_HOST;
  v.payload.ref_slot = ref_slot;
  return v;
}
CelValue MakeIntIdx(int64_t i) {
  CelValue v{};
  v.kind = CEL_INT;
  v.payload.i = i;
  return v;
}

std::shared_ptr<HostList> ThreeInts() {
  return std::make_shared<HostList>(std::vector<cel::Value>{
      cel::Value::Int(10), cel::Value::Int(20), cel::Value::Int(30)});
}

// ════════ 3VL absorption on operand pair ════════

TEST(CelListAtImplTest, UnknownIndexPropagates) {
  Fixture f;
  f.mem.Place(f.list_slot, MakeListValue(f.refs.InternList(ThreeInts())));
  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 42;
  f.mem.WriteCelValue(f.idx_slot, unk);
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(out.payload.unk, 42u);
}

TEST(CelListAtImplTest, ErrorListPropagates) {
  Fixture f;
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = CEL_ERR_OVERFLOW;
  f.mem.WriteCelValue(f.list_slot, err);
  f.mem.WriteCelValue(f.idx_slot, MakeIntIdx(0));
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

// ════════ Per-element-kind round-trip ════════

TEST(CelListAtImplTest, IntElementHits) {
  Fixture f;
  f.mem.Place(f.list_slot, MakeListValue(f.refs.InternList(ThreeInts())));
  f.mem.WriteCelValue(f.idx_slot, MakeIntIdx(1));
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(out.payload.i, 20);
}

TEST(CelListAtImplTest, StringElementMarshalledViaArena) {
  Fixture f;
  auto backing = std::make_shared<HostList>(std::vector<cel::Value>{
      cel::Value::String("alpha"), cel::Value::String("beta")});
  f.mem.Place(f.list_slot, MakeListValue(f.refs.InternList(backing)));
  f.mem.WriteCelValue(f.idx_slot, MakeIntIdx(1));
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(f.mem.ReadSpan(out.payload.s.ptr, out.payload.s.len), "beta");
  EXPECT_GE(out.payload.s.ptr, 4096u);  // in our arena
}

// ════════ Boundary semantics (langdef §"Indexing") ════════

TEST(CelListAtImplTest, NegativeIndexEncodesOutOfBounds) {
  Fixture f;
  f.mem.Place(f.list_slot, MakeListValue(f.refs.InternList(ThreeInts())));
  f.mem.WriteCelValue(f.idx_slot, MakeIntIdx(-1));
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(CEL_ERR_INDEX_OUT_OF_BOUNDS));
}

TEST(CelListAtImplTest, IndexEqualToCountEncodesOutOfBounds) {
  Fixture f;
  f.mem.Place(f.list_slot, MakeListValue(f.refs.InternList(ThreeInts())));
  f.mem.WriteCelValue(f.idx_slot, MakeIntIdx(3));  // == size
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err,
            static_cast<uint32_t>(CEL_ERR_INDEX_OUT_OF_BOUNDS));
}

TEST(CelListAtImplTest, NonIntIndexEncodesTypeMismatch) {
  Fixture f;
  f.mem.Place(f.list_slot, MakeListValue(f.refs.InternList(ThreeInts())));
  CelValue u{};
  u.kind = CEL_UINT;
  u.payload.u = 0u;
  f.mem.WriteCelValue(f.idx_slot, u);
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST(CelListAtImplTest, NonHostListOperandEncodesTypeMismatch) {
  Fixture f;
  CelValue not_list{};
  not_list.kind = CEL_INT;
  not_list.payload.i = 42;
  f.mem.WriteCelValue(f.list_slot, not_list);
  f.mem.WriteCelValue(f.idx_slot, MakeIntIdx(0));
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST(CelListAtImplTest, MissingRefSlotReturnsNonOkStatus) {
  Fixture f;
  // ref_slot 99 was never interned.
  f.mem.Place(f.list_slot, MakeListValue(/*ref_slot=*/99));
  f.mem.WriteCelValue(f.idx_slot, MakeIntIdx(0));
  EXPECT_FALSE(
      CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
}

// ════════ Backing dispatch — HostList vs ProtoList ════════

TEST(CelListAtImplTest, ProtoListBackingDispatchesByReflection) {
  Fixture f;
  HostMsg3 m;
  m.add_rep_i32(11);
  m.add_rep_i32(22);
  m.add_rep_i32(33);
  const auto* fd = m.GetDescriptor()->FindFieldByName("rep_i32");
  auto backing = std::make_shared<ProtoList>(&m, fd);
  f.mem.Place(f.list_slot, MakeListValue(f.refs.InternList(backing)));
  f.mem.WriteCelValue(f.idx_slot, MakeIntIdx(2));
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(out.payload.i, 33);
}

// ════════ Nested aggregate elements intern into matching namespace ════════

TEST(CelListAtImplTest, NestedMessageElementInternsToMessageSlot) {
  Fixture f;
  // Wrap a HostMsg3 as a Value::Message and put it in a list.
  HostMsg3 sub;
  sub.set_i64(99);
  auto backing = std::make_shared<HostList>(std::vector<cel::Value>{
      cel::Value::Message(sub)});
  f.mem.Place(f.list_slot, MakeListValue(f.refs.InternList(backing)));
  f.mem.WriteCelValue(f.idx_slot, MakeIntIdx(0));
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_MESSAGE));
  EXPECT_NE(f.refs.Lookup(out.payload.msg_slot), nullptr);
}

TEST(CelListAtImplTest, NestedListElementInternsToListSlot) {
  Fixture f;
  auto inner = std::make_shared<HostList>(std::vector<cel::Value>{
      cel::Value::Int(7)});
  auto outer = std::make_shared<HostList>(std::vector<cel::Value>{
      cel::Value::HostList(inner)});
  f.mem.Place(f.list_slot, MakeListValue(f.refs.InternList(outer)));
  f.mem.WriteCelValue(f.idx_slot, MakeIntIdx(0));
  ASSERT_TRUE(CelListAtImpl(f.out_slot, f.list_slot, f.idx_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_LIST_HOST));
  EXPECT_NE(f.refs.LookupList(out.payload.ref_slot), nullptr);
}

}  // namespace
}  // namespace celwasm
