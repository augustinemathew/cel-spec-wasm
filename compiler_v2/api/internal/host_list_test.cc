// HostList (Layer 1, vector-backed) — host-surface coverage.
// Mirrors host_map_test.cc's shape: same OOB / round-trip /
// per-kind matrix, but without the equality ladder (lists index
// by position, not by key).
//
// Lives in its own translation unit so the M4.D coverage doesn't
// pull in cel_host_test.cc's wasmtime-trampoline scaffolding.

#include "compiler_v2/api/internal/cel_host.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "compiler_v2/api/error.h"
#include "compiler_v2/api/type.h"
#include "compiler_v2/api/value.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// Local convenience: dereference an `absl::StatusOr` after asserting
// it's OK.  Mirrors host_map_test's macro.
#define HL_ASSIGN_OR_ASSERT(lhs, expr)             \
  auto lhs##_or = (expr);                          \
  ASSERT_TRUE(lhs##_or.ok()) << lhs##_or.status(); \
  auto lhs = *std::move(lhs##_or)

celwasm::api::Value ThreeInts() {
  std::vector<celwasm::api::Value> elements;
  elements.emplace_back(celwasm::api::Value::Int(10));
  elements.emplace_back(celwasm::api::Value::Int(20));
  elements.emplace_back(celwasm::api::Value::Int(30));
  return celwasm::api::Value::List(std::move(elements));
}

// ════════ Value::List / Value::HostList factories ════════

TEST(ValueListTest, ListBuilderProducesKListKind) {
  celwasm::api::Value l = ThreeInts();
  EXPECT_EQ(l.kind(), celwasm::api::Value::Kind::kList);
  ASSERT_TRUE(l.ListBacking().ok());
  const HostListBacking* b = *l.ListBacking();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->Size(), 3u);
}

TEST(ValueListDeathTest, HostListNullBackingFires) {
  EXPECT_DEATH(
      { (void)celwasm::api::Value::HostList(nullptr); }, "must not be null");
}

TEST(ValueListTest, ListBackingMismatchKindReturnsInvalidArgument) {
  celwasm::api::Value v = celwasm::api::Value::Int(7);
  EXPECT_FALSE(v.ListBacking().ok());
  EXPECT_FALSE(v.SharedListBacking().ok());
}

TEST(ValueListTest, StructurallyEqualsIsPointerIdentity) {
  // Pointer-identity at M4 — same rationale as kMap / kMessage.
  // Spec-compliant element-wise equality lands with M5's `==` arm.
  auto backing = std::make_shared<HostList>(std::vector<celwasm::api::Value>{
      celwasm::api::Value::Int(1), celwasm::api::Value::Int(2)});
  celwasm::api::Value a = celwasm::api::Value::HostList(backing);
  celwasm::api::Value b = celwasm::api::Value::HostList(backing);
  EXPECT_TRUE(a.StructurallyEquals(b));

  celwasm::api::Value c = ThreeInts();  // distinct backing
  EXPECT_FALSE(a.StructurallyEquals(c));
}

// ════════ HostList::At — hit, OOB ════════

TEST(HostListTest, AtReturnsValueOnHit) {
  std::vector<celwasm::api::Value> elements;
  elements.emplace_back(celwasm::api::Value::Int(7));
  elements.emplace_back(celwasm::api::Value::String("hello"));
  HostList l(std::move(elements));

  HL_ASSIGN_OR_ASSERT(v0, l.At(0, celwasm::api::CelType::Int()));
  EXPECT_EQ(*v0.AsInt(), 7);

  HL_ASSIGN_OR_ASSERT(v1, l.At(1, celwasm::api::CelType::Int()));
  EXPECT_EQ(*v1.AsString(), "hello");
}

TEST(HostListTest, AtAtEqualsCountReturnsOutOfBoundsError) {
  HostList l({celwasm::api::Value::Int(1)});
  HL_ASSIGN_OR_ASSERT(v, l.At(1, celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kError);
  HL_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kIndexOutOfBounds);
}

TEST(HostListTest, AtFarPastCountReturnsOutOfBoundsError) {
  HostList l({});  // empty
  HL_ASSIGN_OR_ASSERT(v, l.At(999, celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kError);
  HL_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kIndexOutOfBounds);
}

// ════════ Per-element-kind round-trip ════════

struct ElementCase {
  const char* name;
  celwasm::api::Value (*make)();
  std::function<void(const celwasm::api::Value&)> verify;
};

class HostListElementRoundTripTest
    : public ::testing::TestWithParam<ElementCase> {};

TEST_P(HostListElementRoundTripTest, AtPosZeroHits) {
  const ElementCase& c = GetParam();
  HostList l({c.make()});
  HL_ASSIGN_OR_ASSERT(v, l.At(0, celwasm::api::CelType::Int()));
  c.verify(v);
}

INSTANTIATE_TEST_SUITE_P(
    AllElementKinds, HostListElementRoundTripTest,
    ::testing::Values(ElementCase{"null", &celwasm::api::Value::Null,
                                  [](const celwasm::api::Value& v) {
                                    EXPECT_TRUE(v.IsNull());
                                  }},
                      ElementCase{"bool",
                                  +[] {
                                    return celwasm::api::Value::Bool(true);
                                  },
                                  [](const celwasm::api::Value& v) {
                                    EXPECT_EQ(*v.AsBool(), true);
                                  }},
                      ElementCase{"int",
                                  +[] {
                                    return celwasm::api::Value::Int(-99);
                                  },
                                  [](const celwasm::api::Value& v) {
                                    EXPECT_EQ(*v.AsInt(), -99);
                                  }},
                      ElementCase{"uint",
                                  +[] {
                                    return celwasm::api::Value::Uint(7u);
                                  },
                                  [](const celwasm::api::Value& v) {
                                    EXPECT_EQ(*v.AsUint(), 7u);
                                  }},
                      ElementCase{"double",
                                  +[] {
                                    return celwasm::api::Value::Double(3.5);
                                  },
                                  [](const celwasm::api::Value& v) {
                                    EXPECT_EQ(*v.AsDouble(), 3.5);
                                  }},
                      ElementCase{"string",
                                  +[] {
                                    return celwasm::api::Value::String("abc");
                                  },
                                  [](const celwasm::api::Value& v) {
                                    EXPECT_EQ(*v.AsString(), "abc");
                                  }},
                      ElementCase{"bytes",
                                  +[] {
                                    return celwasm::api::Value::Bytes(
                                        "\x01\x02\x03");
                                  },
                                  [](const celwasm::api::Value& v) {
                                    EXPECT_EQ(v.AsBytes()->size(), 3u);
                                  }}),
    [](const ::testing::TestParamInfo<ElementCase>& i) {
      return std::string(i.param.name);
    });

// ════════ ForEach, edge cases ════════

TEST(HostListTest, ForEachVisitsAllInOrder) {
  HostList l({celwasm::api::Value::Int(1), celwasm::api::Value::Int(2),
              celwasm::api::Value::Int(3)});
  std::vector<int64_t> seen;
  l.ForEach([&](const celwasm::api::Value& v) {
    seen.push_back(*v.AsInt());
  });
  EXPECT_EQ(seen, std::vector<int64_t>({1, 2, 3}));
}

TEST(HostListTest, EmptyList) {
  HostList l({});
  EXPECT_EQ(l.Size(), 0u);
  HL_ASSIGN_OR_ASSERT(v, l.At(0, celwasm::api::CelType::Int()));
  EXPECT_EQ(v.kind(), celwasm::api::Value::Kind::kError);
  HL_ASSIGN_OR_ASSERT(e, v.ErrorInfo());
  EXPECT_EQ(e->code, celwasm::ErrorCode::kIndexOutOfBounds);
  l.ForEach([](const celwasm::api::Value&) {
    FAIL() << "ForEach on empty list should not invoke visitor";
  });
}

#undef HL_ASSIGN_OR_ASSERT

}  // namespace
}  // namespace celwasm
