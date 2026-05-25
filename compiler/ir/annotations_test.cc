#include "compiler_v2/ir/annotations.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace celwasm {
namespace {

TEST(ReprNameTest, CoversEveryEnumerator) {
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
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  EXPECT_EQ(ReprName(static_cast<Repr>(250)), "?");
}

TEST(StorageKindNameTest, CoversEveryEnumerator) {
  EXPECT_EQ(StorageKindName(StorageKind::kNone), "none");
  EXPECT_EQ(StorageKindName(StorageKind::kStaticRodata), "static_rodata");
  EXPECT_EQ(StorageKindName(StorageKind::kWorkspaceSlot), "workspace_slot");
  EXPECT_EQ(StorageKindName(StorageKind::kLocal), "local");
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  EXPECT_EQ(StorageKindName(static_cast<StorageKind>(250)), "?");
}

TEST(WasmAnnotationsTest, OperatorIndexCreatesDefaultEntry) {
  WasmAnnotations a;
  NodeAnnotation& node = a[42];
  EXPECT_EQ(node.repr, Repr::kUnknown);
  EXPECT_EQ(node.field_number, 0u);
  EXPECT_EQ(node.overload_id, "");
  EXPECT_EQ(node.local_index, 0u);
  EXPECT_EQ(node.scope_id, 0u);
  EXPECT_EQ(node.storage.kind, StorageKind::kNone);
  EXPECT_EQ(node.storage.payload, 0u);
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
  a[7].storage = {StorageKind::kStaticRodata, 16};
  const NodeAnnotation* node = a.Find(7);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->repr, Repr::kString);
  EXPECT_EQ(node->storage.kind, StorageKind::kStaticRodata);
  EXPECT_EQ(node->storage.payload, 16u);
}

TEST(WasmAnnotationsTest, AllFieldsRoundTrip) {
  WasmAnnotations a;
  a[1].repr = Repr::kBool;
  a[1].field_number = 5;
  a[1].overload_id = "add_int64";
  a[1].local_index = 3;
  a[1].scope_id = 2;
  a[1].storage = {StorageKind::kWorkspaceSlot, 128};
  const NodeAnnotation* n = a.Find(1);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->repr, Repr::kBool);
  EXPECT_EQ(n->field_number, 5u);
  EXPECT_EQ(n->overload_id, "add_int64");
  EXPECT_EQ(n->local_index, 3u);
  EXPECT_EQ(n->scope_id, 2u);
  EXPECT_EQ(n->storage.kind, StorageKind::kWorkspaceSlot);
  EXPECT_EQ(n->storage.payload, 128u);
}

TEST(WasmAnnotationsTest, NegativeExprIdIsAddressable) {
  WasmAnnotations a;
  a[-1].repr = Repr::kDouble;
  a[int64_t{INT64_MIN}].repr = Repr::kBytes;
  EXPECT_EQ(a.Find(-1)->repr, Repr::kDouble);
  EXPECT_EQ(a.Find(int64_t{INT64_MIN})->repr, Repr::kBytes);
}

}  // namespace
}  // namespace celwasm
