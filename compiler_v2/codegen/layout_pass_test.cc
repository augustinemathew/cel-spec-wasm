#include "compiler_v2/codegen/layout_pass.h"

#include <cstdint>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status_matchers.h"
#include "compiler_v2/codegen/resolve_pass.h"
#include "compiler_v2/frontend/parse_and_check.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Runs parse → check → resolve → layout and returns the root node's
// storage field.  Any pipeline failure dereferences a bad StatusOr and
// crashes with the absl-standard message — per-kind tests below only
// care that each scalar variant lands in rodata.
Storage RootStorage(absl::string_view expression) {
  auto ta = ParseAndCheck(expression, {});
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  return layout->annotations.Find(ta->ast().root_expr().id())->storage;
}

// --- Per-kind kConst → kStaticRodata --------------------------------------

TEST(LayoutPassTest, BoolLandsInRodata) {
  Storage s = RootStorage("true");
  EXPECT_EQ(s.kind, StorageKind::kStaticRodata);
  EXPECT_GE(s.payload, 16u);
}
TEST(LayoutPassTest, IntLandsInRodata) {
  Storage s = RootStorage("42");
  EXPECT_EQ(s.kind, StorageKind::kStaticRodata);
  EXPECT_GE(s.payload, 16u);
}
TEST(LayoutPassTest, UintLandsInRodata) {
  Storage s = RootStorage("42u");
  EXPECT_EQ(s.kind, StorageKind::kStaticRodata);
  EXPECT_GE(s.payload, 16u);
}
TEST(LayoutPassTest, DoubleLandsInRodata) {
  Storage s = RootStorage("3.14");
  EXPECT_EQ(s.kind, StorageKind::kStaticRodata);
  EXPECT_GE(s.payload, 16u);
}
TEST(LayoutPassTest, NullLandsInRodata) {
  Storage s = RootStorage("null");
  EXPECT_EQ(s.kind, StorageKind::kStaticRodata);
  EXPECT_GE(s.payload, 16u);
}
TEST(LayoutPassTest, StringLandsInRodata) {
  Storage s = RootStorage("\"hi\"");
  EXPECT_EQ(s.kind, StorageKind::kStaticRodata);
  EXPECT_GE(s.payload, 16u);
}
TEST(LayoutPassTest, BytesLandsInRodata) {
  Storage s = RootStorage("b\"x\"");
  EXPECT_EQ(s.kind, StorageKind::kStaticRodata);
  EXPECT_GE(s.payload, 16u);
}

// --- StaticLayout top-level fields ----------------------------------------

TEST(LayoutPassTest, RodataBaseIsSixteen) {
  auto ta = ParseAndCheck("42", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->rodata_base, 16u);
}

TEST(LayoutPassTest, FirstFrameLandsAtRodataBase) {
  auto ta = ParseAndCheck("42", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  const int64_t root_id = ta->ast().root_expr().id();
  const NodeAnnotation* ann = layout->annotations.Find(root_id);
  ASSERT_NE(ann, nullptr);
  EXPECT_EQ(ann->storage.payload, layout->rodata_base);
}

TEST(LayoutPassTest, IntFrameIsTwentyFourBytes) {
  auto ta = ParseAndCheck("42", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->rodata.size(), 24u);
}

TEST(LayoutPassTest, WorkspaceBytesZeroForLiteralOnlyProgram) {
  auto ta = ParseAndCheck("42", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->workspace_bytes, 0u);
  EXPECT_EQ(layout->peak_slots, 0u);
}

TEST(LayoutPassTest, WorkspaceBaseIsEightAlignedPastRodata) {
  // "hi" is a 2-byte payload → rodata size is 24 (frame) + 2 (payload) = 26,
  // padded to 32 on AllocateString's internal write.  Check the builder
  // actually pads by asserting workspace_base is 8-aligned and >= 16 + 26.
  auto ta = ParseAndCheck("\"hi\"", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->workspace_base % 8, 0u);
  EXPECT_GE(layout->workspace_base,
            layout->rodata_base + static_cast<uint32_t>(layout->rodata.size()));
}

TEST(LayoutPassTest, ArenaBaseFollowsWorkspace) {
  auto ta = ParseAndCheck("42", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->arena_base % 8, 0u);
  EXPECT_EQ(layout->arena_base,
            layout->workspace_base + layout->workspace_bytes);
}

TEST(LayoutPassTest, LocalTypesForwardedFromResolveOutput) {
  auto ta = ParseAndCheck("1", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_TRUE(layout->local_types.empty());
}

TEST(LayoutPassTest, DebugLayoutOptionForwardedToStaticLayout) {
  auto ta = ParseAndCheck("42", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  LayoutOptions opts;
  opts.debug_layout = true;
  auto layout = LayoutPass(*ta, *std::move(resolved), opts);
  ASSERT_THAT(layout, IsOk());
  EXPECT_TRUE(layout->debug_mode);
}

// --- Multi-kConst AST: every kConst gets storage --------------------------
// `1 + 2` type-checks: root is a kCall over two kConst operands.  M1's
// expr_lower will later reject the kCall, but LayoutPass still needs to
// pack both literals into rodata so M3's call lowering has them available
// without re-visiting.  Root kCall stays at kNone — expr_lower fails before
// anything consumes it.
TEST(LayoutPassTest, PacksAllKConstsInMultiNodeAst) {
  auto ta = ParseAndCheck("1 + 2", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  int kconst_with_rodata = 0;
  for (const auto& [id, ann] : layout->annotations.nodes()) {
    if (ann.storage.kind == StorageKind::kStaticRodata) {
      EXPECT_GE(ann.storage.payload, layout->rodata_base);
      ++kconst_with_rodata;
    }
  }
  EXPECT_EQ(kconst_with_rodata, 2);

  // Root kCall has no storage at M1.
  const int64_t root_id = ta->ast().root_expr().id();
  const NodeAnnotation* root_ann = layout->annotations.Find(root_id);
  ASSERT_NE(root_ann, nullptr);
  EXPECT_EQ(root_ann->storage.kind, StorageKind::kNone);

  // Two 24-byte frames; padding to 8 is a no-op here.
  EXPECT_EQ(layout->rodata.size(), 48u);
}

// --- Distinct rodata offsets for distinct literals ------------------------
TEST(LayoutPassTest, DistinctLiteralsGetDistinctOffsets) {
  auto ta = ParseAndCheck("1 + 2", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  absl::flat_hash_set<uint32_t> offsets;
  for (const auto& [id, ann] : layout->annotations.nodes()) {
    if (ann.storage.kind == StorageKind::kStaticRodata) {
      offsets.insert(ann.storage.payload);
    }
  }
  EXPECT_EQ(offsets.size(), 2u);
}

}  // namespace
}  // namespace celwasm
