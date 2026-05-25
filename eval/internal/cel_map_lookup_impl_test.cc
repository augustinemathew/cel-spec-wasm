// Layer-2 trampoline body for `cel_host.cel_map_lookup`.  Drives
// CelMapLookupImpl through fake MemoryView / ExternrefTable /
// ArenaAllocator impls so the test doesn't depend on wasmtime or
// the M2.C.0b trampoline scaffolding (still pending).
//
// Coverage matrix mirrors the runtime dispatcher's:
//   - 3VL on operand pair (unknown / error propagate)
//   - kHost arm dispatches into HostMap::Get
//   - non-CEL_MAP_HOST operand poisons TYPE_MISMATCH
//   - missing ref_slot surfaces a non-OK Status (infrastructure
//     failure — codegen drift, not user-visible)
//   - per-key-kind round-trip (bool / int / uint / string)
//   - error values returned by Get encode to the matching wire code

#include "eval/internal/cel_host.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "eval/error.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "common/type.h"
#include "eval/value.h"
#include "runtime/cel_data.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using FakeMemory = test::FakeMemoryView;
using FakeRefs = test::FakeExternrefTable;

// ─── Fixture ───────────────────────────────────────────────────────
struct Fixture {
  FakeMemory mem;
  FakeRefs refs;
  test::FakeArenaAllocator arena{&mem, /*base_offset=*/4096u,
                                 /*capacity=*/8192u};
  CelHostBindings bindings{};
  TrampolineContext ctx{bindings, mem, refs, arena};

  uint32_t map_slot = 24;  // offsets aligned to CelValue size for clarity
  uint32_t key_slot = 48;
  uint32_t out_slot = 72;
};

CelValue MakeMapValue(uint32_t ref_slot) {
  CelValue v{};
  v.kind = CEL_MAP_HOST;
  v.payload.ref_slot = ref_slot;
  return v;
}
CelValue MakeIntKey(int64_t i) {
  CelValue v{};
  v.kind = CEL_INT;
  v.payload.i = i;
  return v;
}
CelValue MakeUintKey(uint64_t u) {
  CelValue v{};
  v.kind = CEL_UINT;
  v.payload.u = u;
  return v;
}
CelValue MakeBoolKey(bool b) {
  CelValue v{};
  v.kind = CEL_BOOL;
  v.payload.b = b ? 1 : 0;
  return v;
}
// String payload is staged ahead of the CelValue itself; bytes copied
// into the FakeMemory by hand so the read path actually has data.
CelValue MakeStringKey(FakeMemory& mem, uint32_t bytes_off,
                       absl::string_view s) {
  std::memcpy(mem.data() + bytes_off, s.data(), s.size());
  CelValue v{};
  v.kind = CEL_STRING;
  v.payload.s.ptr = bytes_off;
  v.payload.s.len = static_cast<uint32_t>(s.size());
  return v;
}

std::shared_ptr<HostMap> SimpleMap() {
  std::vector<std::pair<celwasm::api::Value, celwasm::api::Value>> entries;
  entries.emplace_back(celwasm::api::Value::Int(7),
                       celwasm::api::Value::Int(70));
  entries.emplace_back(celwasm::api::Value::String("k"),
                       celwasm::api::Value::String("v"));
  entries.emplace_back(celwasm::api::Value::Bool(true),
                       celwasm::api::Value::Int(1));
  entries.emplace_back(celwasm::api::Value::Uint(42),
                       celwasm::api::Value::Int(420));
  return std::make_shared<HostMap>(std::move(entries));
}

// ════════ 3VL on operand pair ════════

TEST(CelMapLookupImplTest, UnknownKeyPropagates) {
  Fixture f;
  f.mem.Place(f.map_slot, MakeMapValue(f.refs.InternMap(SimpleMap())));
  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 99;
  f.mem.WriteCelValue(f.key_slot, unk);
  ASSERT_TRUE(CelMapLookupImpl(f.out_slot, f.map_slot, f.key_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_UNKNOWN));
  EXPECT_EQ(out.payload.unk, 99u);
}

TEST(CelMapLookupImplTest, ErrorMapPropagates) {
  Fixture f;
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = CEL_ERR_NO_SUCH_KEY;
  f.mem.WriteCelValue(f.map_slot, err);
  f.mem.WriteCelValue(f.key_slot, MakeIntKey(1));
  ASSERT_TRUE(CelMapLookupImpl(f.out_slot, f.map_slot, f.key_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY));
}

// ════════ Per-key-kind round-trip ════════

TEST(CelMapLookupImplTest, IntKeyHits) {
  Fixture f;
  f.mem.Place(f.map_slot, MakeMapValue(f.refs.InternMap(SimpleMap())));
  f.mem.WriteCelValue(f.key_slot, MakeIntKey(7));
  ASSERT_TRUE(CelMapLookupImpl(f.out_slot, f.map_slot, f.key_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(out.payload.i, 70);
}

TEST(CelMapLookupImplTest, UintKeyHitsViaCrossTypeEquality) {
  Fixture f;
  f.mem.Place(f.map_slot, MakeMapValue(f.refs.InternMap(SimpleMap())));
  // map stores Uint(42); look up via Int(42) — crosses the equality
  // ladder.
  f.mem.WriteCelValue(f.key_slot, MakeIntKey(42));
  ASSERT_TRUE(CelMapLookupImpl(f.out_slot, f.map_slot, f.key_slot, f.ctx).ok());
  EXPECT_EQ(f.mem.ReadCelValue(f.out_slot).payload.i, 420);
}

TEST(CelMapLookupImplTest, BoolKeyHits) {
  Fixture f;
  f.mem.Place(f.map_slot, MakeMapValue(f.refs.InternMap(SimpleMap())));
  f.mem.WriteCelValue(f.key_slot, MakeBoolKey(true));
  ASSERT_TRUE(CelMapLookupImpl(f.out_slot, f.map_slot, f.key_slot, f.ctx).ok());
  EXPECT_EQ(f.mem.ReadCelValue(f.out_slot).payload.i, 1);
}

TEST(CelMapLookupImplTest, StringKeyHitsAndEncodesValueViaArena) {
  Fixture f;
  f.mem.Place(f.map_slot, MakeMapValue(f.refs.InternMap(SimpleMap())));
  // Stage the lookup-key bytes at offset 200 (well below arena base).
  f.mem.WriteCelValue(f.key_slot, MakeStringKey(f.mem, /*bytes_off=*/200, "k"));
  ASSERT_TRUE(CelMapLookupImpl(f.out_slot, f.map_slot, f.key_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  ASSERT_EQ(out.kind, static_cast<uint32_t>(CEL_STRING));
  EXPECT_EQ(f.mem.ReadSpan(out.payload.s.ptr, out.payload.s.len), "v");
  // Allocated in our arena (base 4096), not below.
  EXPECT_GE(out.payload.s.ptr, 4096u);
}

// ════════ Error / poison surface ════════

TEST(CelMapLookupImplTest, MissingKeyEncodesNoSuchKey) {
  Fixture f;
  f.mem.Place(f.map_slot, MakeMapValue(f.refs.InternMap(SimpleMap())));
  f.mem.WriteCelValue(f.key_slot, MakeIntKey(999));
  ASSERT_TRUE(CelMapLookupImpl(f.out_slot, f.map_slot, f.key_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_NO_SUCH_KEY));
}

TEST(CelMapLookupImplTest, InvalidKeyKindEncodesTypeMismatch) {
  Fixture f;
  f.mem.Place(f.map_slot, MakeMapValue(f.refs.InternMap(SimpleMap())));
  CelValue dbl{};
  dbl.kind = CEL_DOUBLE;
  dbl.payload.d = 1.5;
  f.mem.WriteCelValue(f.key_slot, dbl);
  ASSERT_TRUE(CelMapLookupImpl(f.out_slot, f.map_slot, f.key_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST(CelMapLookupImplTest, NonHostMapOperandPoisonsTypeMismatch) {
  Fixture f;
  CelValue not_map{};
  not_map.kind = CEL_INT;
  not_map.payload.i = 42;
  f.mem.WriteCelValue(f.map_slot, not_map);
  f.mem.WriteCelValue(f.key_slot, MakeIntKey(0));
  ASSERT_TRUE(CelMapLookupImpl(f.out_slot, f.map_slot, f.key_slot, f.ctx).ok());
  CelValue out = f.mem.ReadCelValue(f.out_slot);
  EXPECT_EQ(out.kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(out.payload.err, static_cast<uint32_t>(CEL_ERR_TYPE_MISMATCH));
}

TEST(CelMapLookupImplTest, MissingRefSlotReturnsNonOkStatus) {
  // Codegen drift: kHost map points at a ref_slot the externref
  // table never interned.  Surface as a Status failure (Layer 3
  // logs it; the wasm side never sees it because the trampoline
  // converts to a trap).
  Fixture f;
  f.mem.WriteCelValue(f.map_slot, MakeMapValue(/*ref_slot=*/9999));
  f.mem.WriteCelValue(f.key_slot, MakeIntKey(0));
  EXPECT_FALSE(
      CelMapLookupImpl(f.out_slot, f.map_slot, f.key_slot, f.ctx).ok());
}

}  // namespace
}  // namespace celwasm
