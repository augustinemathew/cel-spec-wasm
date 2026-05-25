// HostMap (Layer 1, vector-backed) — host-surface coverage.  Mirrors
// the runtime-side `cel_map_test.cc` matrix on the host side: same
// equality ladder, same poison rules, but operating on `celwasm::api::Value`
// instead of `CelValue`.  Host- and arena-built maps must agree
// under langdef map-key equality.
//
// Lives in its own translation unit so the M3.D coverage doesn't
// pull in cel_host_test.cc's wasmtime-trampoline dependencies.

#include "eval/internal/cel_host.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "eval/error.h"
#include "common/type.h"
#include "eval/value.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// Local convenience: dereference an `absl::StatusOr` after asserting
// it's OK.  Equivalent to ASSERT_OK_AND_ASSIGN, kept inline because
// the macro lives outside abseil-cpp's exported header set.
#define HM_ASSIGN_OR_ASSERT(lhs, expr)             \
  auto lhs##_or = (expr);                          \
  ASSERT_TRUE(lhs##_or.ok()) << lhs##_or.status(); \
  auto lhs = *std::move(lhs##_or)

celwasm::api::Value SinglePair(int64_t k, int64_t v) {
  std::vector<std::pair<celwasm::api::Value, celwasm::api::Value>> entries;
  entries.emplace_back(celwasm::api::Value::Int(k),
                       celwasm::api::Value::Int(v));
  return celwasm::api::Value::Map(std::move(entries));
}

// ════════ Value::Map / Value::HostMap factories ════════

TEST(ValueMapTest, MapBuilderProducesKMapKind) {
  celwasm::api::Value m = SinglePair(1, 10);
  EXPECT_EQ(m.kind(), celwasm::api::Value::Kind::kMap);
  ASSERT_TRUE(m.MapBacking().ok());
  const HostMapBacking* b = *m.MapBacking();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->Size(), 1u);
}

TEST(ValueMapDeathTest, HostMapNullBackingFires) {
  EXPECT_DEATH(
      { (void)celwasm::api::Value::HostMap(nullptr); }, "must not be null");
}

TEST(ValueMapTest, MapBackingMismatchKindReturnsInvalidArgument) {
  celwasm::api::Value v = celwasm::api::Value::Int(7);
  EXPECT_FALSE(v.MapBacking().ok());
  EXPECT_FALSE(v.SharedMapBacking().ok());
}

// ════════ HostMap::Get — hit, miss, invalid kind ════════

TEST(HostMapTest, GetReturnsValueOnHit) {
  std::vector<std::pair<celwasm::api::Value, celwasm::api::Value>> entries;
  entries.emplace_back(celwasm::api::Value::Int(7),
                       celwasm::api::Value::Int(70));
  entries.emplace_back(celwasm::api::Value::String("k"),
                       celwasm::api::Value::Int(99));
  HostMap m(std::move(entries));

  HM_ASSIGN_OR_ASSERT(
      v, m.Get(celwasm::api::Value::Int(7), celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kInt);
  EXPECT_EQ(*v.AsInt(), 70);

  HM_ASSIGN_OR_ASSERT(
      s, m.Get(celwasm::api::Value::String("k"), celwasm::api::CelType::Int()));
  EXPECT_EQ(*s.AsInt(), 99);
}

TEST(HostMapTest, GetMissingKeyReturnsErrorValue) {
  std::vector<std::pair<celwasm::api::Value, celwasm::api::Value>> entries;
  entries.emplace_back(celwasm::api::Value::Int(1),
                       celwasm::api::Value::Int(10));
  HostMap m(std::move(entries));

  HM_ASSIGN_OR_ASSERT(
      v, m.Get(celwasm::api::Value::Int(999), celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kError);
  HM_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kKeyNotFound);
}

TEST(HostMapTest, GetInvalidKeyKindReturnsTypeMismatch) {
  HostMap m({});
  HM_ASSIGN_OR_ASSERT(
      v, m.Get(celwasm::api::Value::Double(1.5), celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kError);
  HM_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kTypeMismatch);
}

// ════════ Cross-type numeric equality (langdef §"Equality") ════════

struct CrossTypeCase {
  const char* name;
  celwasm::api::Value (*store)();
  celwasm::api::Value (*lookup)();
  bool expect_hit;
  int64_t value;
};
class HostMapCrossTypeTest : public ::testing::TestWithParam<CrossTypeCase> {};

TEST_P(HostMapCrossTypeTest, RespectsMathematicalValue) {
  const CrossTypeCase& c = GetParam();
  std::vector<std::pair<celwasm::api::Value, celwasm::api::Value>> entries;
  entries.emplace_back(c.store(), celwasm::api::Value::Int(c.value));
  HostMap m(std::move(entries));
  HM_ASSIGN_OR_ASSERT(v, m.Get(c.lookup(), celwasm::api::CelType::Int()));
  if (c.expect_hit) {
    ASSERT_EQ(v.kind(), celwasm::api::Value::Kind::kInt) << c.name;
    EXPECT_EQ(*v.AsInt(), c.value) << c.name;
  } else {
    ASSERT_EQ(v.kind(), celwasm::api::Value::Kind::kError) << c.name;
    HM_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
    EXPECT_EQ(e->code, celwasm::ErrorCode::kKeyNotFound) << c.name;
  }
}

INSTANTIATE_TEST_SUITE_P(
    EqualityLadder, HostMapCrossTypeTest,
    ::testing::Values(CrossTypeCase{
                          "int_stored_uint_lookup_hits",
                          +[]() {
                            return celwasm::api::Value::Int(42);
                          },
                          +[]() {
                            return celwasm::api::Value::Uint(42);
                          },
                          true,
                          420,
                      },
                      CrossTypeCase{
                          "uint_stored_int_lookup_hits",
                          +[]() {
                            return celwasm::api::Value::Uint(7);
                          },
                          +[]() {
                            return celwasm::api::Value::Int(7);
                          },
                          true,
                          700,
                      },
                      CrossTypeCase{
                          "negative_int_never_matches_uint",
                          +[]() {
                            return celwasm::api::Value::Uint(0);
                          },
                          +[]() {
                            return celwasm::api::Value::Int(-1);
                          },
                          false,
                          0,
                      },
                      CrossTypeCase{
                          "uint_above_int_max_never_matches_int",
                          +[]() {
                            return celwasm::api::Value::Uint(uint64_t{1} << 63);
                          },
                          +[]() {
                            return celwasm::api::Value::Int(INT64_MAX);
                          },
                          false,
                          0,
                      }),
    [](const ::testing::TestParamInfo<CrossTypeCase>& i) {
      return std::string(i.param.name);
    });

// ════════ ContainsKey, ForEach, edge cases ════════

TEST(HostMapTest, ContainsKey) {
  std::vector<std::pair<celwasm::api::Value, celwasm::api::Value>> entries;
  entries.emplace_back(celwasm::api::Value::Int(1),
                       celwasm::api::Value::Int(10));
  entries.emplace_back(celwasm::api::Value::String("k"),
                       celwasm::api::Value::Int(20));
  HostMap m(std::move(entries));
  EXPECT_TRUE(m.ContainsKey(celwasm::api::Value::Int(1)));
  EXPECT_TRUE(m.ContainsKey(celwasm::api::Value::String("k")));
  EXPECT_TRUE(m.ContainsKey(celwasm::api::Value::Uint(1)));  // cross-type
  EXPECT_FALSE(m.ContainsKey(celwasm::api::Value::Int(999)));
  EXPECT_FALSE(
      m.ContainsKey(celwasm::api::Value::Double(1.0)));  // invalid kind
  EXPECT_FALSE(
      m.ContainsKey(celwasm::api::Value::Int(-1)));  // never matches uint
}

TEST(HostMapTest, ForEachVisitsAllInOrder) {
  std::vector<std::pair<celwasm::api::Value, celwasm::api::Value>> entries;
  entries.emplace_back(celwasm::api::Value::Int(1),
                       celwasm::api::Value::Int(10));
  entries.emplace_back(celwasm::api::Value::Int(2),
                       celwasm::api::Value::Int(20));
  entries.emplace_back(celwasm::api::Value::Int(3),
                       celwasm::api::Value::Int(30));
  HostMap m(std::move(entries));
  std::vector<int64_t> seen_keys;
  std::vector<int64_t> seen_vals;
  m.ForEach([&](const celwasm::api::Value& k, const celwasm::api::Value& v) {
    seen_keys.push_back(*k.AsInt());
    seen_vals.push_back(*v.AsInt());
  });
  EXPECT_EQ(seen_keys, std::vector<int64_t>({1, 2, 3}));
  EXPECT_EQ(seen_vals, std::vector<int64_t>({10, 20, 30}));
}

TEST(HostMapTest, EmptyMap) {
  HostMap m({});
  EXPECT_EQ(m.Size(), 0u);
  EXPECT_FALSE(m.ContainsKey(celwasm::api::Value::Int(1)));
  HM_ASSIGN_OR_ASSERT(
      v, m.Get(celwasm::api::Value::Int(1), celwasm::api::CelType::Int()));
  HM_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kKeyNotFound);
  m.ForEach([](const celwasm::api::Value&, const celwasm::api::Value&) {
    FAIL() << "ForEach on empty map should not invoke visitor";
  });
}

// langdef: bool(true) and int(1) are distinct keys — kinds don't
// compare across the bool / int boundary.
TEST(HostMapTest, BoolAndIntOneAreDistinct) {
  std::vector<std::pair<celwasm::api::Value, celwasm::api::Value>> entries;
  entries.emplace_back(celwasm::api::Value::Bool(true),
                       celwasm::api::Value::Int(900));
  entries.emplace_back(celwasm::api::Value::Int(1),
                       celwasm::api::Value::Int(800));
  HostMap m(std::move(entries));
  EXPECT_EQ(m.Size(), 2u);
  HM_ASSIGN_OR_ASSERT(
      vb, m.Get(celwasm::api::Value::Bool(true), celwasm::api::CelType::Int()));
  EXPECT_EQ(*vb.AsInt(), 900);
  HM_ASSIGN_OR_ASSERT(
      vi, m.Get(celwasm::api::Value::Int(1), celwasm::api::CelType::Int()));
  EXPECT_EQ(*vi.AsInt(), 800);
}

#undef HM_ASSIGN_OR_ASSERT

}  // namespace
}  // namespace celwasm
