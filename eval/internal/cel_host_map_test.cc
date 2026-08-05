// cel_host map-family Layer-2 guard tests — the absorb / operand-kind
// / externref-lookup / key-decode guard arms of `cel_map_size` and
// `cel_map_in`.  (Lookup / eq / iter-open coverage lives in
// cel_map_lookup_impl_test.cc, cel_map_eq_impl_test.cc, and
// cel_iter_open_impl_test.cc.)

#include "eval/internal/cel_host_map.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "eval/error.h"
#include "eval/internal/cel_host_test_harness.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::celwasm::test::Layer2Fixture;

// ═══════════ Map-trampoline guards ═══════════
//
// `cel_map_size` / `cel_map_in` open with an absorb, then guard the
// operand kind, the externref lookup, and (for `in`) whether the key
// decodes to a valid map-key kind.  A type-checked expression cannot
// deliver a non-map or an undecodable key, so these arms are reached by
// staging the wire values directly.

// Stage `cv` at the fixture's message slot and run cel_map_size.
CelValue MapSize(Layer2Fixture& f, const CelValue& cv) {
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  EXPECT_THAT(
      CelMapSizeImpl(Layer2Fixture::kOutSlot, Layer2Fixture::kMsgSlot, f.Ctx()),
      IsOk());
  return f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
}

TEST(MapTrampolineGuardTest, SizeOnNonMapPoisons) {
  Layer2Fixture f;
  CelValue i{};
  i.kind = CEL_INT;
  i.payload.i = 1;
  EXPECT_EQ(MapSize(f, i).kind, CEL_ERROR);
}

TEST(MapTrampolineGuardTest, SizeAbsorbsErrorAndUnknown) {
  Layer2Fixture f;
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = static_cast<uint32_t>(celwasm::ErrorCode::kOverflow);
  EXPECT_EQ(MapSize(f, err).kind, CEL_ERROR);
  CelValue unk{};
  unk.kind = CEL_UNKNOWN;
  unk.payload.unk = 5;
  const CelValue got = MapSize(f, unk);
  EXPECT_EQ(got.kind, CEL_UNKNOWN);
  EXPECT_EQ(got.payload.unk, 5u);
}

TEST(MapTrampolineGuardTest, SizeOnUninternedRefIsAnInfrastructureFailure) {
  Layer2Fixture f;
  CelValue cv{};
  cv.kind = CEL_MAP_HOST;
  cv.payload.ref_slot = 999;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  EXPECT_THAT(
      CelMapSizeImpl(Layer2Fixture::kOutSlot, Layer2Fixture::kMsgSlot, f.Ctx()),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               testing::HasSubstr("not found in ExternrefTable")));
}

TEST(MapTrampolineGuardTest, InOnNonMapPoisons) {
  Layer2Fixture f;
  constexpr uint32_t kKeySlot = 96;
  CelValue key{};
  key.kind = CEL_INT;
  key.payload.i = 1;
  f.mem.WriteCelValue(kKeySlot, key);
  CelValue not_a_map{};
  not_a_map.kind = CEL_INT;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, not_a_map);
  EXPECT_THAT(CelMapInImpl(Layer2Fixture::kOutSlot, kKeySlot,
                           Layer2Fixture::kMsgSlot, f.Ctx()),
              IsOk());
  EXPECT_EQ(f.mem.ReadCelValue(Layer2Fixture::kOutSlot).kind, CEL_ERROR);
}

// `cel_map_in` decodes the key to a `celwasm::Value` before asking the
// backing.  A key kind outside the map-key set (langdef restricts them
// to bool / int / uint / string) fails to decode and poisons — the
// checker prevents it, so only a staged wire value reaches the arm.
TEST(MapTrampolineGuardTest, InWithUndecodableKeyPoisons) {
  Layer2Fixture f;
  constexpr uint32_t kKeySlot = 96;
  // A double is not a valid map-key kind.
  CelValue key{};
  key.kind = CEL_DOUBLE;
  key.payload.d = 1.5;
  f.mem.WriteCelValue(kKeySlot, key);
  CelValue map_cv{};
  map_cv.kind = CEL_MAP_HOST;
  map_cv.payload.ref_slot = f.refs.InternMap(std::make_shared<HostMap>(
      std::vector<std::pair<celwasm::Value, celwasm::Value>>{
          {celwasm::Value::Int(1), celwasm::Value::Int(2)}}));
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, map_cv);
  EXPECT_THAT(CelMapInImpl(Layer2Fixture::kOutSlot, kKeySlot,
                           Layer2Fixture::kMsgSlot, f.Ctx()),
              IsOk());
  EXPECT_EQ(f.mem.ReadCelValue(Layer2Fixture::kOutSlot).kind, CEL_ERROR);
}

// The happy path through the same trampoline, so the poison rows above
// are not the only thing pinning it.
TEST(MapTrampolineGuardTest, InWithValidKeyReportsMembership) {
  Layer2Fixture f;
  constexpr uint32_t kKeySlot = 96;
  CelValue key{};
  key.kind = CEL_INT;
  key.payload.i = 1;
  f.mem.WriteCelValue(kKeySlot, key);
  CelValue map_cv{};
  map_cv.kind = CEL_MAP_HOST;
  map_cv.payload.ref_slot = f.refs.InternMap(std::make_shared<HostMap>(
      std::vector<std::pair<celwasm::Value, celwasm::Value>>{
          {celwasm::Value::Int(1), celwasm::Value::Int(2)}}));
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, map_cv);
  EXPECT_THAT(CelMapInImpl(Layer2Fixture::kOutSlot, kKeySlot,
                           Layer2Fixture::kMsgSlot, f.Ctx()),
              IsOk());
  const CelValue out = f.mem.ReadCelValue(Layer2Fixture::kOutSlot);
  EXPECT_EQ(out.kind, CEL_BOOL);
  EXPECT_NE(out.payload.b, 0);
}

TEST(MapTrampolineGuardTest, InOnUninternedRefIsAnInfrastructureFailure) {
  Layer2Fixture f;
  constexpr uint32_t kKeySlot = 96;
  CelValue key{};
  key.kind = CEL_INT;
  f.mem.WriteCelValue(kKeySlot, key);
  CelValue cv{};
  cv.kind = CEL_MAP_HOST;
  cv.payload.ref_slot = 999;
  f.mem.WriteCelValue(Layer2Fixture::kMsgSlot, cv);
  EXPECT_THAT(CelMapInImpl(Layer2Fixture::kOutSlot, kKeySlot,
                           Layer2Fixture::kMsgSlot, f.Ctx()),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       testing::HasSubstr("not found in ExternrefTable")));
}

}  // namespace
}  // namespace celwasm
