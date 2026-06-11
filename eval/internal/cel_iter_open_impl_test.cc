// Layer-2 trampoline bodies for `cel_host.cel_list_iter_open` /
// `cel_host.cel_map_iter_open` — the comprehension iter snapshots for
// host-backed sources.  Drives CelListIterOpenImpl /
// CelMapIterOpenImpl through the shared FakeMemoryView /
// FakeExternrefTable / FakeArenaAllocator fakes.
//
// Coverage:
//   - host list/map snapshot happy path (count + elements land in
//     the arena in the documented layout)
//   - empty host source → zero-count view (loop body never runs)
//   - CEL_UNKNOWN / CEL_ERROR source → loud FailedPrecondition —
//     the comprehension prologue's range-absorption guard
//     (expr_lower_comprehension.cc EmitRangeAbsorptionGuard) must
//     propagate poisoned ranges before the iterate path runs, so a
//     poisoned value reaching these impls is a codegen regression,
//     not an empty range (the empty-range-identity soundness gap)
//   - other non-host kinds → defensive empty view (unchanged)
//   - missing ref_slot → FailedPrecondition

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "eval/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"

namespace celwasm {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;
using FakeMemory = test::FakeMemoryView;
using FakeRefs = test::FakeExternrefTable;

struct Fixture {
  FakeMemory mem;
  FakeRefs refs;
  test::FakeArenaAllocator arena{&mem, /*base_offset=*/4096u,
                                 /*capacity=*/8192u};
  CelHostBindings bindings{};
  TrampolineContext ctx{bindings, mem, refs, arena};

  uint32_t src_slot = 24;
  uint32_t out_slot = 72;  // list: out CelValue; map: MapIterState

  uint32_t ReadU32(uint32_t off) {
    uint32_t v = 0;
    std::memcpy(&v, mem.data() + off, sizeof(v));
    return v;
  }
};

CelValue MakeKind(CelKind kind, uint32_t ref_slot = 0) {
  CelValue v{};
  v.kind = kind;
  v.payload.ref_slot = ref_slot;
  return v;
}

CelValue MakeUnknown(uint32_t attr_id) {
  CelValue v{};
  v.kind = CEL_UNKNOWN;
  v.payload.unk = attr_id;
  return v;
}

CelValue MakeError(uint32_t code) {
  CelValue v{};
  v.kind = CEL_ERROR;
  v.payload.err = code;
  return v;
}

std::shared_ptr<HostList> ThreeInts() {
  return std::make_shared<HostList>(std::vector<celwasm::Value>{
      celwasm::Value::Int(10), celwasm::Value::Int(20),
      celwasm::Value::Int(30)});
}

std::shared_ptr<HostMap> TwoEntries() {
  std::vector<std::pair<celwasm::Value, celwasm::Value>> entries;
  entries.emplace_back(celwasm::Value::Int(7), celwasm::Value::Int(70));
  entries.emplace_back(celwasm::Value::Int(8), celwasm::Value::Int(80));
  return std::make_shared<HostMap>(std::move(entries));
}

// ════════ CelListIterOpenImpl ════════

TEST(CelListIterOpenImplTest, HostListSnapshotsIntoArena) {
  Fixture f;
  f.mem.Place(f.src_slot,
              MakeKind(CEL_LIST_HOST, f.refs.InternList(ThreeInts())));
  ASSERT_TRUE(CelListIterOpenImpl(f.out_slot, f.src_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  const uint32_t hdr = out.payload.arena_list.header_ptr;
  ASSERT_GE(hdr, 4096u);               // in our arena
  EXPECT_EQ(f.ReadU32(hdr + 0u), 3u);  // count
  const uint32_t elements = f.ReadU32(hdr + 8u);
  for (uint32_t i = 0; i < 3; ++i) {
    CelValue e = f.mem.ReadCelValue(elements + (i * sizeof(CelValue)));
    EXPECT_EQ(e.kind, static_cast<uint32_t>(CEL_INT));
    EXPECT_EQ(e.payload.i, 10 * (i + 1));
  }
}

TEST(CelListIterOpenImplTest, EmptyHostListWritesEmptyView) {
  Fixture f;
  auto empty = std::make_shared<HostList>(std::vector<celwasm::Value>{});
  f.mem.Place(f.src_slot, MakeKind(CEL_LIST_HOST, f.refs.InternList(empty)));
  ASSERT_TRUE(CelListIterOpenImpl(f.out_slot, f.src_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_EQ(f.ReadU32(out.payload.arena_list.header_ptr + 0u), 0u);
}

TEST(CelListIterOpenImplTest, UnknownRangeIsLoudFailure) {
  Fixture f;
  f.mem.Place(f.src_slot, MakeUnknown(/*attr_id=*/7));
  EXPECT_THAT(CelListIterOpenImpl(f.out_slot, f.src_slot, f.ctx),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("CEL_UNKNOWN")));
}

TEST(CelListIterOpenImplTest, ErrorRangeIsLoudFailure) {
  Fixture f;
  f.mem.Place(f.src_slot, MakeError(CEL_ERR_OVERFLOW));
  EXPECT_THAT(
      CelListIterOpenImpl(f.out_slot, f.src_slot, f.ctx),
      StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("CEL_ERROR")));
}

TEST(CelListIterOpenImplTest, OtherNonHostKindWritesEmptyView) {
  Fixture f;
  CelValue iv{};
  iv.kind = CEL_INT;
  iv.payload.i = 42;
  f.mem.Place(f.src_slot, iv);
  ASSERT_TRUE(CelListIterOpenImpl(f.out_slot, f.src_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_EQ(f.ReadU32(out.payload.arena_list.header_ptr + 0u), 0u);
}

TEST(CelListIterOpenImplTest, MissingRefSlotIsFailedPrecondition) {
  Fixture f;
  f.mem.Place(f.src_slot, MakeKind(CEL_LIST_HOST, /*ref_slot=*/999));
  EXPECT_THAT(
      CelListIterOpenImpl(f.out_slot, f.src_slot, f.ctx),
      StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("not found")));
}

// ════════ CelMapIterOpenImpl ════════
//
// MapIterState field offsets (mirrors cel_runtime.c): kind at 0,
// cursor at 4, payload at 8, count at 12.

TEST(CelMapIterOpenImplTest, HostMapSnapshotsEntries) {
  Fixture f;
  f.mem.Place(f.src_slot,
              MakeKind(CEL_MAP_HOST, f.refs.InternMap(TwoEntries())));
  ASSERT_TRUE(CelMapIterOpenImpl(f.out_slot, f.src_slot, f.ctx).ok());
  EXPECT_EQ(f.ReadU32(f.out_slot + 0u), 1u);   // MAP_ITER_KIND_HOST
  EXPECT_EQ(f.ReadU32(f.out_slot + 4u), 0u);   // cursor
  EXPECT_EQ(f.ReadU32(f.out_slot + 12u), 2u);  // count
  // Snapshot is key/value pairs at a 48-byte stride.
  const uint32_t snap = f.ReadU32(f.out_slot + 8u);
  ASSERT_GE(snap, 4096u);
  CelValue k0 = f.mem.ReadCelValue(snap);
  EXPECT_EQ(k0.kind, static_cast<uint32_t>(CEL_INT));
}

TEST(CelMapIterOpenImplTest, UnknownRangeIsLoudFailure) {
  Fixture f;
  f.mem.Place(f.src_slot, MakeUnknown(/*attr_id=*/9));
  EXPECT_THAT(CelMapIterOpenImpl(f.out_slot, f.src_slot, f.ctx),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("CEL_UNKNOWN")));
}

TEST(CelMapIterOpenImplTest, ErrorRangeIsLoudFailure) {
  Fixture f;
  f.mem.Place(f.src_slot, MakeError(CEL_ERR_NO_SUCH_KEY));
  EXPECT_THAT(
      CelMapIterOpenImpl(f.out_slot, f.src_slot, f.ctx),
      StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("CEL_ERROR")));
}

TEST(CelMapIterOpenImplTest, OtherNonHostKindWritesEmptyState) {
  Fixture f;
  CelValue iv{};
  iv.kind = CEL_INT;
  iv.payload.i = 42;
  f.mem.Place(f.src_slot, iv);
  ASSERT_TRUE(CelMapIterOpenImpl(f.out_slot, f.src_slot, f.ctx).ok());
  EXPECT_EQ(f.ReadU32(f.out_slot + 12u), 0u);  // count == 0 → empty iter
}

}  // namespace
}  // namespace celwasm
