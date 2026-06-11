#include "e2e/fuzz/compare.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

#include "cel/expr/value.pb.h"
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"

namespace celwasm::fuzz {
namespace {

cel::expr::Value OInt(int64_t x) {
  cel::expr::Value v;
  v.set_int64_value(x);
  return v;
}
cel::expr::Value ODouble(double x) {
  cel::expr::Value v;
  v.set_double_value(x);
  return v;
}
cel::expr::Value OString(const std::string& x) {
  cel::expr::Value v;
  v.set_string_value(x);
  return v;
}
cel::expr::Value OIntList(std::initializer_list<int64_t> xs) {
  cel::expr::Value v;
  for (int64_t x : xs) {
    v.mutable_list_value()->add_values()->set_int64_value(x);
  }
  // An empty initializer still must mark the kind as list.
  v.mutable_list_value();
  return v;
}
cel::expr::Value OStringIntMap(
    std::initializer_list<std::pair<const char*, int64_t>> kvs) {
  cel::expr::Value v;
  v.mutable_map_value();
  for (const auto& [k, x] : kvs) {
    auto* e = v.mutable_map_value()->add_entries();
    e->mutable_key()->set_string_value(k);
    e->mutable_value()->set_int64_value(x);
  }
  return v;
}

// ── Scalars ──────────────────────────────────────────────────────

TEST(CompareScalarTest, EqualAndUnequalInt) {
  EXPECT_TRUE(Compare(Value::Int(7), OInt(7), CelType::Int()).equal);
  const CompareResult r = Compare(Value::Int(7), OInt(8), CelType::Int());
  EXPECT_FALSE(r.equal);
  EXPECT_EQ(r.ours, "7");
  EXPECT_EQ(r.oracle, "8");
}

TEST(CompareScalarTest, KindMismatchRendersWrongKind) {
  const CompareResult r = Compare(Value::Bool(true), OInt(1), CelType::Int());
  EXPECT_FALSE(r.equal);
  EXPECT_EQ(r.ours, "<wrong-kind=Bool>");
}

TEST(CompareScalarTest, NanAgreesWithNan) {
  const double nan = std::nan("");
  EXPECT_TRUE(Compare(Value::Double(nan), ODouble(nan), CelType::Double()).equal);
  EXPECT_FALSE(Compare(Value::Double(nan), ODouble(1.0), CelType::Double()).equal);
}

TEST(CompareScalarTest, StringPayload) {
  EXPECT_TRUE(
      Compare(Value::String("hi"), OString("hi"), CelType::String()).equal);
  EXPECT_FALSE(
      Compare(Value::String("hi"), OString("ho"), CelType::String()).equal);
}

// ── Lists ────────────────────────────────────────────────────────

TEST(CompareListTest, EqualLists) {
  const Value ours =
      Value::List({Value::Int(1), Value::Int(2), Value::Int(3)});
  EXPECT_TRUE(
      Compare(ours, OIntList({1, 2, 3}), CelType::List(CelType::Int())).equal);
}

TEST(CompareListTest, ElementMismatch) {
  const Value ours = Value::List({Value::Int(1), Value::Int(9)});
  const CompareResult r =
      Compare(ours, OIntList({1, 2}), CelType::List(CelType::Int()));
  EXPECT_FALSE(r.equal);
  EXPECT_EQ(r.ours, "[1, 9]");
  EXPECT_EQ(r.oracle, "[1, 2]");
}

TEST(CompareListTest, LengthMismatch) {
  const Value ours = Value::List({Value::Int(1)});
  EXPECT_FALSE(
      Compare(ours, OIntList({1, 2}), CelType::List(CelType::Int())).equal);
}

TEST(CompareListTest, EmptyListsAgree) {
  EXPECT_TRUE(
      Compare(Value::List({}), OIntList({}), CelType::List(CelType::Int()))
          .equal);
}

TEST(CompareListTest, WrongOuterKind) {
  const CompareResult r =
      Compare(Value::Int(1), OIntList({1}), CelType::List(CelType::Int()));
  EXPECT_FALSE(r.equal);
  EXPECT_EQ(r.ours, "<wrong-kind=Int>");
}

// ── Maps ─────────────────────────────────────────────────────────

TEST(CompareMapTest, EqualMapsAnyOrder) {
  const Value ours = Value::Map({{Value::String("a"), Value::Int(2)},
                                 {Value::String("b"), Value::Int(3)}});
  // Oracle entries in the OPPOSITE order — order is not semantics.
  EXPECT_TRUE(Compare(ours, OStringIntMap({{"b", 3}, {"a", 2}}),
                      CelType::Map(CelType::String(), CelType::Int()))
                  .equal);
}

TEST(CompareMapTest, ValueMismatch) {
  const Value ours = Value::Map({{Value::String("a"), Value::Int(2)}});
  EXPECT_FALSE(Compare(ours, OStringIntMap({{"a", 9}}),
                       CelType::Map(CelType::String(), CelType::Int()))
                   .equal);
}

TEST(CompareMapTest, MissingKey) {
  const Value ours = Value::Map({{Value::String("a"), Value::Int(2)}});
  EXPECT_FALSE(Compare(ours, OStringIntMap({{"x", 2}}),
                       CelType::Map(CelType::String(), CelType::Int()))
                   .equal);
}

TEST(CompareMapTest, SizeMismatch) {
  const Value ours = Value::Map({{Value::String("a"), Value::Int(2)}});
  EXPECT_FALSE(Compare(ours, OStringIntMap({{"a", 2}, {"b", 3}}),
                       CelType::Map(CelType::String(), CelType::Int()))
                   .equal);
}

// ── Unsupported ──────────────────────────────────────────────────

TEST(CompareTest, UnsupportedTypeNeverAgrees) {
  const CompareResult r =
      Compare(Value::Int(1), OInt(1), CelType::Duration());
  EXPECT_FALSE(r.equal);
  EXPECT_EQ(r.ours, "<unsupported-type>");
}

}  // namespace
}  // namespace celwasm::fuzz
