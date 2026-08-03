#include "compiler/ir/annotations.h"

#include <cstdint>

#include "absl/strings/string_view.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// Enumerator-coverage tests for the three name functions.
//
// These are written to STAY correct when an enumerator is added, which
// a hand-written row list does not: the previous ReprName test listed
// 15 rows and was named CoversEveryEnumerator, but `kOptional` had been
// added since and was not among them.
//
// The mechanism is a `k*Max` constant beside each enum plus a pair of
// tests:
//
//   1. iterate [0, kMaxValue] and require a real name — so a new value
//      covered by the sentinel but missing its switch arm trips
//      ReprName's "?" fallback or OriginName's ABSL_CHECK;
//   2. death-test kMaxValue + 1 — so a new value added WITHOUT bumping
//      the sentinel is caught too, because the function now returns a
//      name where the test demands a crash.
//
// Either omission fails.  That is what makes it automatic.

TEST(NameCoverageTest, ReprNameCoversEveryEnumerator) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kReprMax); ++i) {
    const absl::string_view name = ReprName(static_cast<Repr>(i));
    EXPECT_NE(name, "?") << "Repr = " << static_cast<int>(i)
                         << " has no switch arm in ReprName";
    EXPECT_FALSE(name.empty()) << "Repr = " << static_cast<int>(i);
  }
}

TEST(NameCoverageTest, StorageKindNameCoversEveryEnumerator) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kStorageKindMax); ++i) {
    const absl::string_view name = StorageKindName(static_cast<StorageKind>(i));
    EXPECT_NE(name, "?") << "StorageKind = " << static_cast<int>(i)
                         << " has no switch arm in StorageKindName";
  }
}

TEST(NameCoverageTest, OriginNameCoversEveryEnumerator) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kOriginMax); ++i) {
    const absl::string_view name = OriginName(static_cast<Origin>(i));
    EXPECT_FALSE(name.empty()) << "Origin = " << static_cast<int>(i);
  }
}

// One past the sentinel must NOT have a name.  This is what catches an
// enumerator added without bumping kMaxValue: the loops above would
// skip it, but here the function would return a name instead of
// falling back / dying, and the expectation fails.
// The three cases below cast one past each enum's last enumerator.
// That value is deliberately outside the enum's range — it IS the
// property under test — so the analyzer's out-of-range check is
// suppressed for the block rather than per line, which survives
// clang-format rejoining the expressions.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
TEST(NameCoverageTest, ReprNameOnePastMaxHasNoName) {
  EXPECT_EQ(ReprName(static_cast<Repr>(static_cast<uint8_t>(kReprMax) + 1)),
            "?");
}

TEST(NameCoverageTest, StorageKindNameOnePastMaxHasNoName) {
  EXPECT_EQ(StorageKindName(static_cast<StorageKind>(
                static_cast<uint8_t>(kStorageKindMax) + 1)),
            "?");
}

TEST(NameCoverageDeathTest, OriginNameOnePastMaxFires) {
  // OriginName CHECKs rather than returning "?" (closed-enum rule), so
  // its out-of-range case is a death test.
  EXPECT_DEATH(
      {
        (void)OriginName(
            static_cast<Origin>(static_cast<uint8_t>(kOriginMax) + 1));
      },
      "OriginName: unknown Origin");
}
// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

// These two pin the exact SPELLINGS; NameCoverageTest above pins
// EXHAUSTIVENESS (every enumerator has some name, and one past the last
// has none).  Neither subsumes the other — a missing row here still
// fails there, and a wrong string there still passes.
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
      {Repr::kType, "type"},         {Repr::kOptional, "optional"},
  };
  for (const auto& row : rows) {
    EXPECT_EQ(ReprName(row.r), row.name) << "repr=" << static_cast<int>(row.r);
    EXPECT_NE(ReprName(row.r), "?")
        << "repr=" << static_cast<int>(row.r) << " fell through the switch";
  }
}

TEST(StorageKindNameTest, CoversEveryEnumerator) {
  EXPECT_EQ(StorageKindName(StorageKind::kNone), "none");
  EXPECT_EQ(StorageKindName(StorageKind::kStaticRodata), "static_rodata");
  EXPECT_EQ(StorageKindName(StorageKind::kWorkspaceSlot), "workspace_slot");
  EXPECT_EQ(StorageKindName(StorageKind::kLocal), "local");
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
