#include "compiler/ir/annotations.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace celwasm {
namespace {

TEST(ReprNameTest, CoversEveryEnumerator) {
  // Each entry pairs a Repr value with the exact string ReprName must return.
  // Any new Repr must add a row here; a fall-through silently mapping to "?"
  // would hide a missing switch arm and is caught by EXPECT_NE below.
  struct Row {
    Repr r;
    const char* name;
  };
  const Row rows[] = {
      {Repr::kUnknown, "unknown"},   {Repr::kNull, "null"},
      {Repr::kBool, "bool"},         {Repr::kInt, "int"},
      {Repr::kUint, "uint"},         {Repr::kDouble, "double"},
      {Repr::kString, "string"},     {Repr::kBytes, "bytes"},
      {Repr::kList, "list"},         {Repr::kMap, "map"},
      {Repr::kMessage, "message"},   {Repr::kEnum, "enum"},
      {Repr::kDuration, "duration"}, {Repr::kTimestamp, "timestamp"},
      {Repr::kType, "type"},
  };
  for (const auto& row : rows) {
    EXPECT_EQ(ReprName(row.r), row.name) << "repr=" << static_cast<int>(row.r);
    EXPECT_NE(ReprName(row.r), "?")
        << "repr=" << static_cast<int>(row.r) << " fell through the switch";
  }
}

TEST(ReprNameTest, FallsBackForOutOfRangeValue) {
  // A cast-in value the switch has no arm for must hit the sentinel.
  EXPECT_EQ(ReprName(static_cast<Repr>(250)), "?");
}

TEST(WasmAnnotationsTest, OperatorIndexCreatesDefaultEntry) {
  WasmAnnotations a;
  NodeAnnotation& node = a[42];
  EXPECT_EQ(node.repr, Repr::kUnknown);
  node.repr = Repr::kInt;
  EXPECT_EQ(a[42].repr, Repr::kInt);
  EXPECT_EQ(a.nodes().size(), 1u);
}

TEST(WasmAnnotationsTest, FindReturnsNullForMissingId) {
  WasmAnnotations a;
  EXPECT_EQ(a.Find(1), nullptr);
}

TEST(WasmAnnotationsTest, FindReturnsStoredEntry) {
  WasmAnnotations a;
  a[7].repr = Repr::kString;
  const NodeAnnotation* node = a.Find(7);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->repr, Repr::kString);
}

TEST(WasmAnnotationsTest, DistinctIdsStoreDistinctEntries) {
  WasmAnnotations a;
  a[1].repr = Repr::kBool;
  a[2].repr = Repr::kInt;
  a[3].repr = Repr::kMap;
  EXPECT_EQ(a.nodes().size(), 3u);
  EXPECT_EQ(a.Find(1)->repr, Repr::kBool);
  EXPECT_EQ(a.Find(2)->repr, Repr::kInt);
  EXPECT_EQ(a.Find(3)->repr, Repr::kMap);
}

TEST(WasmAnnotationsTest, NegativeExprIdIsAddressable) {
  // cel::ExprId is a signed int64; negative ids (e.g. synthesised macros)
  // must round-trip through the map alongside positive ones.
  WasmAnnotations a;
  a[-1].repr = Repr::kDouble;
  a[int64_t{INT64_MIN}].repr = Repr::kBytes;
  EXPECT_EQ(a.Find(-1)->repr, Repr::kDouble);
  EXPECT_EQ(a.Find(int64_t{INT64_MIN})->repr, Repr::kBytes);
}

}  // namespace
}  // namespace celwasm
