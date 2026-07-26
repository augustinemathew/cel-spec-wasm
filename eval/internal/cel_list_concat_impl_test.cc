// Layer-2 trampoline body for `cel_host.cel_list_concat` — the
// mixed-origin / both-host arms of CEL list `+` (`add_list`).  Drives
// CelListConcatImpl through the shared FakeMemoryView /
// FakeExternrefTable / FakeArenaAllocator fakes so the test doesn't
// depend on wasmtime.
//
// The load-bearing surface is MATERIALISATION: unlike `size` / `in` /
// `==`, concat must *produce* a value, so a host-backed operand's
// elements have to be lifted into the arena.  Before this landed,
// every origin pair but arena+arena poisoned TYPE_MISMATCH (pinned as
// `KnownBugs.PbtListConcatHostOriginPoisons`, found by e2e/fuzz).
//
// Coverage matrix:
//   - every origin pair: arena+host, host+arena, host+host,
//     arena+arena (the dispatcher short-circuits that last one, but
//     the trampoline must still be correct if routing changes)
//   - empty operand on each side, and both empty
//   - element ORDER: `a` elements precede `b` elements (list `+` is
//     sequence concatenation, langdef §"Addition")
//   - boundary element values (INT64_MIN / INT64_MAX / UINT64_MAX)
//     survive the lift unchanged
//   - string elements — the arm that allocates payload bytes through
//     the same arena as the elements run
//   - message elements from a host backing intern into the externref
//     table and land as CEL_MESSAGE handles
//   - nested list elements intern as CEL_LIST_HOST handles (nested
//     aggregates are NOT flattened; they stay reachable by handle)
//   - the operands are UNCHANGED (a fresh list is produced)
//   - 3VL on the operand pair (unknown / error propagate)
//   - non-list operand poisons TYPE_MISMATCH
//   - arena exhaustion poisons CEL_ERR_OVERFLOW (never a partial list)
//   - missing list ref_slot surfaces a non-OK Status (infrastructure
//     failure — codegen/interner drift, not user-visible)

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"
#include "shared/type.h"
#include "testdata/host_fixture_proto3.pb.h"

namespace celwasm {
namespace {

using ::celwasm::testdata::HostMsg3;
using FakeMemory = test::FakeMemoryView;
using FakeRefs = test::FakeExternrefTable;

// ─── Fixture ───────────────────────────────────────────────────────
struct Fixture {
  FakeMemory mem;
  FakeRefs refs;
  test::FakeArenaAllocator arena{&mem, /*base_offset=*/8192u,
                                 /*capacity=*/8192u};
  CelHostBindings bindings{};
  TrampolineContext ctx{bindings, mem, refs, arena};

  uint32_t a_slot = 24;
  uint32_t b_slot = 48;
  uint32_t out_slot = 72;

  // Staging cursor for hand-built arena lists, kept below the
  // FakeArenaAllocator's base so the trampoline's own allocations
  // can't collide with them.
  uint32_t list_cursor = 1024;

  HostMsg3 msg_i64;  // i64 = 1

  Fixture() {
    msg_i64.set_i64(1);
  }
};

// ─── CelValue makers ───────────────────────────────────────────────
CelValue MakeInt(int64_t i) {
  CelValue v{};
  v.kind = CEL_INT;
  v.payload.i = i;
  return v;
}

CelValue MakeUint(uint64_t u) {
  CelValue v{};
  v.kind = CEL_UINT;
  v.payload.u = u;
  return v;
}

// Builds a CEL_LIST_ARENA list in the fake memory at the fixture's
// list cursor: 16-byte ArenaListHeader followed by the count×24-byte
// elements run (the layout `cel_runtime.c` writes).
CelValue MakeArenaList(Fixture& f, const std::vector<CelValue>& elements) {
  const uint32_t header_off = f.list_cursor;
  ArenaListHeader hdr{};
  hdr.count = static_cast<uint32_t>(elements.size());
  hdr.capacity = hdr.count;
  hdr.elements_offset = header_off + static_cast<uint32_t>(sizeof(hdr));
  std::memcpy(f.mem.data() + header_off, &hdr, sizeof(hdr));
  uint32_t off = hdr.elements_offset;
  for (const CelValue& e : elements) {
    f.mem.WriteCelValue(off, e);
    off += static_cast<uint32_t>(kCelListEntryStride);
  }
  f.list_cursor = off;
  CelValue cv{};
  cv.kind = CEL_LIST_ARENA;
  cv.payload.arena_list.header_ptr = header_off;
  return cv;
}

CelValue MakeHostList(Fixture& f, std::vector<celwasm::Value> elements) {
  CelValue cv{};
  cv.kind = CEL_LIST_HOST;
  cv.payload.ref_slot =
      f.refs.InternList(std::make_shared<HostList>(std::move(elements)));
  return cv;
}

// ─── Result readers ────────────────────────────────────────────────

ArenaListHeader ReadHeader(Fixture& f, const CelValue& cv) {
  ArenaListHeader hdr{};
  std::memcpy(&hdr, f.mem.data() + cv.payload.arena_list.header_ptr,
              sizeof(hdr));
  return hdr;
}

CelValue ElementAt(Fixture& f, const CelValue& cv, uint32_t i) {
  const ArenaListHeader hdr = ReadHeader(f, cv);
  return f.mem.ReadCelValue(hdr.elements_offset +
                            (i * static_cast<uint32_t>(kCelListEntryStride)));
}

// Stages `a` / `b` and runs CelListConcatImpl, returning the out
// CelValue.
CelValue RunConcat(Fixture& f, const CelValue& a, const CelValue& b) {
  f.mem.WriteCelValue(f.a_slot, a);
  f.mem.WriteCelValue(f.b_slot, b);
  absl::Status s = CelListConcatImpl(f.out_slot, f.a_slot, f.b_slot, f.ctx);
  EXPECT_TRUE(s.ok()) << s;
  return f.mem.ReadCelValue(f.out_slot);
}

// Asserts the result is an arena list of exactly `want` int elements.
// Split header-vs-elements: gtest's assertion macros each expand to
// several statements, so the combined check trips the
// readability-function-size gate.
void ExpectIntListHeader(Fixture& f, const CelValue& out,
                         std::size_t want_size) {
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  const ArenaListHeader hdr = ReadHeader(f, out);
  ASSERT_EQ(hdr.count, want_size);
  ASSERT_EQ(hdr.capacity, want_size);
}

void ExpectIntListElements(Fixture& f, const CelValue& out,
                           const std::vector<int64_t>& want) {
  for (uint32_t i = 0; i < want.size(); ++i) {
    const CelValue e = ElementAt(f, out, i);
    EXPECT_EQ(e.kind, static_cast<uint32_t>(CEL_INT)) << "element " << i;
    EXPECT_EQ(e.payload.i, want[i]) << "element " << i;
  }
}

void ExpectIntList(Fixture& f, const CelValue& out,
                   const std::vector<int64_t>& want) {
  ASSERT_NO_FATAL_FAILURE(ExpectIntListHeader(f, out, want.size()));
  ExpectIntListElements(f, out, want);
}

// ─── Parameterized origin matrix ───────────────────────────────────
//
// [1, 2] + [3, 4] under all four origin pairings must produce the
// same 4-element list.  arena+arena is short-circuited by the runtime
// dispatcher in production but must still be correct here.

enum class Origins : std::uint8_t {
  kArenaHost,
  kHostArena,
  kHostHost,
  kArenaArena,
};

std::string OriginsName(const ::testing::TestParamInfo<Origins>& info) {
  switch (info.param) {
    case Origins::kArenaHost:
      return "ArenaPlusHost";
    case Origins::kHostArena:
      return "HostPlusArena";
    case Origins::kHostHost:
      return "HostPlusHost";
    case Origins::kArenaArena:
      return "ArenaPlusArena";
  }
  return "Unknown";
}

class ListConcatOriginsTest : public ::testing::TestWithParam<Origins> {
 protected:
  bool LhsIsHost() const {
    return GetParam() == Origins::kHostArena ||
           GetParam() == Origins::kHostHost;
  }
  bool RhsIsHost() const {
    return GetParam() == Origins::kArenaHost ||
           GetParam() == Origins::kHostHost;
  }

  // Builds the operand of the requested origin holding `ints`.
  CelValue Operand(Fixture& f, bool host, const std::vector<int64_t>& ints) {
    if (host) {
      std::vector<celwasm::Value> elements;
      elements.reserve(ints.size());
      for (int64_t i : ints) {
        elements.push_back(celwasm::Value::Int(i));
      }
      return MakeHostList(f, std::move(elements));
    }
    std::vector<CelValue> elements;
    elements.reserve(ints.size());
    for (int64_t i : ints) {
      elements.push_back(MakeInt(i));
    }
    return MakeArenaList(f, elements);
  }

  CelValue RunPair(Fixture& f, const std::vector<int64_t>& lhs,
                   const std::vector<int64_t>& rhs) {
    const CelValue a = Operand(f, LhsIsHost(), lhs);
    const CelValue b = Operand(f, RhsIsHost(), rhs);
    return RunConcat(f, a, b);
  }
};

TEST_P(ListConcatOriginsTest, ConcatenatesInOrder) {
  Fixture f;
  ExpectIntList(f, RunPair(f, {1, 2}, {3, 4}), {1, 2, 3, 4});
}

TEST_P(ListConcatOriginsTest, EmptyLeftOperand) {
  Fixture f;
  ExpectIntList(f, RunPair(f, {}, {3, 4}), {3, 4});
}

TEST_P(ListConcatOriginsTest, EmptyRightOperand) {
  Fixture f;
  ExpectIntList(f, RunPair(f, {1, 2}, {}), {1, 2});
}

TEST_P(ListConcatOriginsTest, BothOperandsEmpty) {
  Fixture f;
  const CelValue out = RunPair(f, {}, {});
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  EXPECT_EQ(ReadHeader(f, out).count, 0u);
}

TEST_P(ListConcatOriginsTest, BoundaryIntElementsSurviveTheLift) {
  // INT64_MIN / INT64_MAX round-trip bit-exact through the
  // materialisation — a lossy encode would silently corrupt them.
  Fixture f;
  ExpectIntList(f, RunPair(f, {INT64_MIN, 0}, {INT64_MAX, -1}),
                {INT64_MIN, 0, INT64_MAX, -1});
}

TEST_P(ListConcatOriginsTest, OperandsAreUnchanged) {
  // `+` produces a FRESH list; neither operand is mutated or aliased
  // (a re-run must give the same answer).
  Fixture f;
  ExpectIntList(f, RunPair(f, {1, 2}, {3, 4}), {1, 2, 3, 4});
  EXPECT_EQ(
      f.mem.ReadCelValue(f.a_slot).kind,
      static_cast<uint32_t>(LhsIsHost() ? CEL_LIST_HOST : CEL_LIST_ARENA));
  EXPECT_EQ(
      f.mem.ReadCelValue(f.b_slot).kind,
      static_cast<uint32_t>(RhsIsHost() ? CEL_LIST_HOST : CEL_LIST_ARENA));
}

INSTANTIATE_TEST_SUITE_P(AllOriginPairs, ListConcatOriginsTest,
                         ::testing::Values(Origins::kArenaHost,
                                           Origins::kHostArena,
                                           Origins::kHostHost,
                                           Origins::kArenaArena),
                         OriginsName);

// ─── Element-kind coverage ─────────────────────────────────────────

TEST(CelListConcatImplTest, UintBoundaryElements) {
  Fixture f;
  const CelValue host = MakeHostList(f, {celwasm::Value::Uint(UINT64_MAX)});
  const CelValue arena = MakeArenaList(f, {MakeUint(0)});
  const CelValue out = RunConcat(f, host, arena);
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  ASSERT_EQ(ReadHeader(f, out).count, 2u);
  EXPECT_EQ(ElementAt(f, out, 0).payload.u, UINT64_MAX);
  EXPECT_EQ(ElementAt(f, out, 1).payload.u, 0u);
}

TEST(CelListConcatImplTest, StringElementsAllocateThroughTheSameArena) {
  // Host string elements need arena bytes for the payload as well as
  // the element run; the two allocations must not overlap.
  Fixture f;
  const CelValue host = MakeHostList(
      f, {celwasm::Value::String("alpha"), celwasm::Value::String("")});
  const CelValue empty_arena = MakeArenaList(f, {});
  const CelValue out = RunConcat(f, host, empty_arena);
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  ASSERT_EQ(ReadHeader(f, out).count, 2u);
  const CelValue e0 = ElementAt(f, out, 0);
  ASSERT_EQ(e0.kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(f.mem.ReadSpan(e0.payload.s.ptr, e0.payload.s.len), "alpha");
  const CelValue e1 = ElementAt(f, out, 1);
  ASSERT_EQ(e1.kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(e1.payload.s.len, 0u);
}

TEST(CelListConcatImplTest, MessageElementsInternAsHandles) {
  // A host list of messages materialises to CEL_MESSAGE elements
  // whose msg_slot resolves in the externref table — the same shape
  // codegen produces for a message element of a list literal.
  Fixture f;
  const CelValue host =
      MakeHostList(f, {celwasm::Value::HostMessage(
                          std::make_shared<ProtoBacking>(&f.msg_i64))});
  const CelValue arena = MakeArenaList(f, {MakeInt(7)});
  const CelValue out = RunConcat(f, host, arena);
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  ASSERT_EQ(ReadHeader(f, out).count, 2u);
  const CelValue e0 = ElementAt(f, out, 0);
  ASSERT_EQ(e0.kind, static_cast<uint32_t>(CEL_MESSAGE));
  ASSERT_NE(f.refs.Lookup(e0.payload.msg_slot), nullptr);
  EXPECT_EQ(f.refs.Lookup(e0.payload.msg_slot)->message(), &f.msg_i64);
  EXPECT_EQ(ElementAt(f, out, 1).payload.i, 7);
}

TEST(CelListConcatImplTest, NestedListElementsInternAsHostHandles) {
  // Nested aggregates are NOT flattened into the arena: the element
  // interns as a CEL_LIST_HOST handle, which every downstream reader
  // (indexing, equality, size) already dispatches on.
  Fixture f;
  auto inner = std::make_shared<HostList>(
      std::vector<celwasm::Value>{celwasm::Value::Int(5)});
  const CelValue host = MakeHostList(f, {celwasm::Value::HostList(inner)});
  const CelValue arena = MakeArenaList(f, {});
  const CelValue out = RunConcat(f, host, arena);
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_LIST_ARENA));
  ASSERT_EQ(ReadHeader(f, out).count, 1u);
  const CelValue e0 = ElementAt(f, out, 0);
  ASSERT_EQ(e0.kind, static_cast<uint32_t>(CEL_LIST_HOST));
  ASSERT_NE(f.refs.LookupList(e0.payload.ref_slot), nullptr);
  EXPECT_EQ(f.refs.LookupList(e0.payload.ref_slot)->Size(), 1u);
}

// ─── 3VL / poison / infrastructure surface ─────────────────────────

TEST(CelListConcatImplTest, UnknownOperandPropagates) {
  Fixture f;
  const CelValue host = MakeHostList(f, {celwasm::Value::Int(1)});
  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 99;
  const CelValue out = RunConcat(f, unk, host);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(out.payload.unk, 99u);
}

TEST(CelListConcatImplTest, ErrorOperandPropagates) {
  Fixture f;
  const CelValue host = MakeHostList(f, {celwasm::Value::Int(1)});
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = CEL_ERR_NO_SUCH_KEY;
  const CelValue out = RunConcat(f, host, err);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY));
}

TEST(CelListConcatImplTest, NonListOperandPoisonsTypeMismatch) {
  Fixture f;
  const CelValue host = MakeHostList(f, {celwasm::Value::Int(1)});
  CelValue out = RunConcat(f, MakeInt(7), host);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
  out = RunConcat(f, host, MakeInt(7));
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST(CelListConcatImplTest, ArenaExhaustionPoisonsOverflow) {
  // A tiny arena can't hold the materialised run.  The failure must
  // be a loud CEL_ERR_OVERFLOW (matching `cel_list_concat_arena`'s
  // OOM contract), never a truncated list.
  FakeMemory mem;
  FakeRefs refs;
  test::FakeArenaAllocator tiny{&mem, /*base_offset=*/8192u, /*capacity=*/8u};
  CelHostBindings bindings{};
  TrampolineContext ctx{bindings, mem, refs, tiny};
  CelValue host{};
  host.kind = CEL_LIST_HOST;
  host.payload.ref_slot = refs.InternList(std::make_shared<HostList>(
      std::vector<celwasm::Value>{celwasm::Value::Int(1)}));
  mem.WriteCelValue(24, host);
  mem.WriteCelValue(48, host);
  ASSERT_TRUE(CelListConcatImpl(72, 24, 48, ctx).ok());
  const CelValue out = mem.ReadCelValue(72);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST(CelListConcatImplTest, MissingListRefSlotReturnsNonOkStatus) {
  Fixture f;
  CelValue bogus{};
  bogus.kind = CEL_LIST_HOST;
  bogus.payload.ref_slot = 9999;
  const CelValue arena = MakeArenaList(f, {MakeInt(1)});
  f.mem.WriteCelValue(f.a_slot, bogus);
  f.mem.WriteCelValue(f.b_slot, arena);
  EXPECT_FALSE(CelListConcatImpl(f.out_slot, f.a_slot, f.b_slot, f.ctx).ok());
}

}  // namespace
}  // namespace celwasm
