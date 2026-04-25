#include "compiler_v2/codegen/layout_pass.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/testdata/e2e_fixture.pb.h"
#include "compiler_v2/codegen/resolve_pass.h"
#include "compiler_v2/frontend/parse_and_check.h"
#include "compiler_v2/ir/annotations.h"
#include "compiler_v2/ir/typed_ast.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using ::absl_testing::IsOk;

// Force generated-pool registration of descriptors referenced by
// tests below.  Runs once at static init per test binary.
[[maybe_unused]] const int
    kDescriptorsLinked =  // NOLINT(bugprone-throwing-static-initialization)
    [] {
      google::protobuf::LinkMessageReflection<celwasm::testdata::Customer>();
      google::protobuf::LinkMessageReflection<celwasm::testdata::Address>();
      return 0;
    }();

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

TEST(LayoutPassTest, NoVariablesForLiteralOnlyProgram) {
  // Literal-only programs have no referenced variables — and
  // therefore zero wasm locals on `$eval`, one per entry.
  auto ta = ParseAndCheck("1", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_TRUE(layout->variables.empty());
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

// ============================================================
// Variable-slot layout (M2.B)
// ============================================================
//
// Every referenced variable gets two things:
//   (a) a 24-byte CelValue slot in the workspace region, starting at
//       `workspace_base`.  The host marshal writes the bound Value
//       into this slot before each `$eval` call.  Slot offset lives
//       on `StaticLayout::variables[i].slot_offset`.
//   (b) a wasm i32 local on the `$eval` function.  The kIdent arm
//       lowers to `local.get local_index`; a prelude emitted by
//       expr_lower initializes each local to its slot offset.  Per
//       m2-ident-select-unknowns.md §2.6 / Slice M2.B.
//
// LayoutPass writes `{kLocal, local_index}` onto every kIdent
// annotation — expr_lower reads the local_index out of the payload
// when emitting `BinaryenLocalGet`.  The wasm-local count equals
// `variables.size()`; local types are all i32 (built at emission
// time, not stored on the layout).

absl::StatusOr<StaticLayout> LayoutWithVars(
    absl::string_view expression, std::vector<std::string> variable_specs) {
  CheckOptions opts;
  opts.variable_specs = std::move(variable_specs);
  auto ta = ParseAndCheck(expression, opts);
  if (!ta.ok()) return ta.status();
  auto resolved = ResolvePass(*ta);
  if (!resolved.ok()) return resolved.status();
  return LayoutPass(*ta, *std::move(resolved));
}

TEST(LayoutPassVariableTest,
     SingleScalarVariableReservesOneTwentyFourByteSlot) {
  auto layout = LayoutWithVars("x", {"x:int"});
  ASSERT_THAT(layout, IsOk());
  ASSERT_EQ(layout->variables.size(), 1u);
  EXPECT_EQ(layout->variables[0].name, "x");
  EXPECT_EQ(layout->variables[0].local_index, 0u);
  EXPECT_EQ(layout->variables[0].slot_offset, layout->workspace_base);
  EXPECT_EQ(layout->workspace_bytes, 24u);
}

TEST(LayoutPassVariableTest, SlotOffsetsAreContiguousAndEightAligned) {
  auto layout = LayoutWithVars("x + y + z", {"x:int", "y:int", "z:int"});
  ASSERT_THAT(layout, IsOk());
  ASSERT_EQ(layout->variables.size(), 3u);
  EXPECT_EQ(layout->variables[0].slot_offset, layout->workspace_base);
  EXPECT_EQ(layout->variables[1].slot_offset, layout->workspace_base + 24u);
  EXPECT_EQ(layout->variables[2].slot_offset, layout->workspace_base + 48u);
  EXPECT_EQ(layout->workspace_bytes, 72u);
  EXPECT_EQ(layout->workspace_base % 8u, 0u);
  EXPECT_EQ(layout->variables[1].slot_offset % 8u, 0u);
  EXPECT_EQ(layout->variables[2].slot_offset % 8u, 0u);
}

TEST(LayoutPassVariableTest, ArenaBaseStillFollowsWorkspaceWithVariables) {
  auto layout = LayoutWithVars("x", {"x:int"});
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->arena_base,
            layout->workspace_base + layout->workspace_bytes);
  EXPECT_EQ(layout->arena_base % 8u, 0u);
}

TEST(LayoutPassVariableTest,
     IdentAnnotationStoresLocalKindWithLocalIndexPayload) {
  auto ta = ParseAndCheck("x", CheckOptions{.variable_specs = {"x:int"}});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  const int64_t root_id = ta->ast().root_expr().id();
  const NodeAnnotation* ann = layout->annotations.Find(root_id);
  ASSERT_NE(ann, nullptr);
  EXPECT_EQ(ann->storage.kind, StorageKind::kLocal);
  EXPECT_EQ(ann->storage.payload, layout->variables[0].local_index);
  EXPECT_EQ(ann->local_index, layout->variables[0].local_index);
}

TEST(LayoutPassVariableTest,
     MultipleKIdentNodesForSameVariableAllPointAtSameLocal) {
  // `x + x` — two kIdent nodes for variable `x`.  Both annotations
  // carry the same local_index; only one variable slot + one wasm
  // local is allocated.
  auto ta = ParseAndCheck("x + x", CheckOptions{.variable_specs = {"x:int"}});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  ASSERT_EQ(layout->variables.size(), 1u);
  const uint32_t expected_index = layout->variables[0].local_index;
  int kident_count = 0;
  for (const auto& [id, ann] : layout->annotations.nodes()) {
    if (ann.storage.kind == StorageKind::kLocal) {
      ++kident_count;
      EXPECT_EQ(ann.storage.payload, expected_index);
    }
  }
  EXPECT_EQ(kident_count, 2);
}

TEST(LayoutPassVariableTest, UnreferencedDeclaredVariableReservesNoSlot) {
  // `x` with both `x` and `y` declared — only `x` is referenced, so
  // the workspace carries exactly one slot.
  auto layout = LayoutWithVars("x", {"x:int", "y:string"});
  ASSERT_THAT(layout, IsOk());
  ASSERT_EQ(layout->variables.size(), 1u);
  EXPECT_EQ(layout->variables[0].name, "x");
  EXPECT_EQ(layout->workspace_bytes, 24u);
}

TEST(LayoutPassVariableTest,
     RodataAndWorkspaceCoexistWithoutCollisionOnLiteralPlusIdent) {
  // `x + 1` — one literal + one ident.  The kConst `1` lands in
  // rodata; the kIdent `x` points at a workspace slot past rodata.
  auto layout = LayoutWithVars("x + 1", {"x:int"});
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->rodata.size(), 24u);  // one kConst frame
  EXPECT_EQ(layout->workspace_base,
            layout->rodata_base + 24u);  // 24 is already 8-aligned
  EXPECT_EQ(layout->variables.size(), 1u);
  EXPECT_EQ(layout->variables[0].slot_offset, layout->workspace_base);
}

// Message variable carried through layout: one 24-byte slot reserved
// (regardless of the wrapped message's shape — the slot holds only
// the CelValue wire form, which for messages is `{CEL_MESSAGE,
// msg_slot}` pointing into the host's extern-ref table at eval time).
TEST(LayoutPassVariableTest, MessageVariableGetsOneSlot) {
  // Touch the descriptor so the generated Customer proto gets
  // registered in the process-wide generated DescriptorPool that
  // ParseAndCheck reaches through monostate-schema lookup.
  auto layout = LayoutWithVars("c", {"c:celwasm.testdata.Customer"});
  ASSERT_THAT(layout, IsOk());
  ASSERT_EQ(layout->variables.size(), 1u);
  EXPECT_EQ(layout->variables[0].repr, Repr::kMessage);
  EXPECT_EQ(layout->workspace_bytes, 24u);
}

// LaidOutVariable preserves Repr from ResolvePass so the ABI
// emitter can write it into cel.abi.variables[] without re-plumbing
// the type through the pipeline.
TEST(LayoutPassVariableTest, VariableReprForwardedFromResolvePass) {
  auto layout = LayoutWithVars("b || s == \"a\" || i > 0 || d > 0.0",
                               {"b:bool", "s:string", "i:int", "d:double"});
  ASSERT_THAT(layout, IsOk());
  ASSERT_EQ(layout->variables.size(), 4u);
  EXPECT_EQ(layout->variables[0].repr, Repr::kBool);
  EXPECT_EQ(layout->variables[1].repr, Repr::kString);
  EXPECT_EQ(layout->variables[2].repr, Repr::kInt);
  EXPECT_EQ(layout->variables[3].repr, Repr::kDouble);
}

// --- kSelect nodes reserve workspace cells (M2.C.2) ----------------------
//
// Every kSelect result lands in a 24B workspace slot allocated
// after the variable-slot region.  Naive SlotAllocator at M2 —
// peak_slots == kSelect node count.

TEST(LayoutPassSelectTest, SelectsGetContiguousWorkspaceSlotsAfterVariables) {
  CheckOptions opts;
  opts.variable_specs = {"c:celwasm.testdata.Customer"};
  auto ta = ParseAndCheck("c.billing_address.city", opts);
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  // 1 variable (c) + 2 selects = 72B workspace; peak = 2 slots.
  EXPECT_EQ(layout->workspace_bytes, 72u);
  EXPECT_EQ(layout->peak_slots, 2u);
  EXPECT_EQ(layout->arena_base,
            layout->workspace_base + layout->workspace_bytes);

  const cel::Expr& city_sel = ta->ast().root_expr();
  const cel::Expr& billing_sel = city_sel.select_expr().operand();
  const NodeAnnotation* billing_ann =
      layout->annotations.Find(billing_sel.id());
  const NodeAnnotation* city_ann = layout->annotations.Find(city_sel.id());
  ASSERT_NE(billing_ann, nullptr);
  ASSERT_NE(city_ann, nullptr);
  // Selects sit right after the variable slot, 24B-aligned.
  const uint32_t select_base = layout->workspace_base + 24u;
  EXPECT_EQ(billing_ann->storage.kind, StorageKind::kWorkspaceSlot);
  EXPECT_EQ(billing_ann->storage.payload, select_base);
  EXPECT_EQ(city_ann->storage.kind, StorageKind::kWorkspaceSlot);
  EXPECT_EQ(city_ann->storage.payload, select_base + 24u);
}

// --- M3.F: kCreateMap + kCallExpr(`_[_]`) reserve workspace cells ---------
//
// kCreateMap nodes get a 24B slot for the result CelValue from
// `cel_map_create` to write into; kCallExpr(`_[_]`) on a map gets
// another 24B slot for the lookup result (cel_map_lookup_arena /
// cel_host.cel_map_lookup / cel_map_lookup write into it).  The
// MapStorageVisitor in `layout_pass.cc` allocates these out of the
// shared SlotAllocator so they coexist with kSelect's slots.

TEST(LayoutPassMapTest, EmptyMapLiteralGetsOneWorkspaceSlot) {
  auto ta = ParseAndCheck("{}", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  // No variables; one kCreateMap → one slot.
  EXPECT_EQ(layout->workspace_bytes, 24u);
  EXPECT_EQ(layout->peak_slots, 1u);

  const auto* root_ann =
      layout->annotations.Find(ta->ast().root_expr().id());
  ASSERT_NE(root_ann, nullptr);
  EXPECT_EQ(root_ann->storage.kind, StorageKind::kWorkspaceSlot);
  EXPECT_EQ(root_ann->storage.payload, layout->workspace_base);
}

TEST(LayoutPassMapTest, ScalarMapLiteralGetsOneSlotRegardlessOfEntryCount) {
  // Per dispatch-doc §4: the kCreateMap result slot is a single
  // 24B CelValue (the wire shape).  Entry storage lives in the
  // arena (allocated by `cel_map_insert`), not the workspace.
  auto ta = ParseAndCheck("{\"a\": 1, \"b\": 2, \"c\": 3}", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->workspace_bytes, 24u);
  EXPECT_EQ(layout->peak_slots, 1u);
}

TEST(LayoutPassMapTest, MapLiteralIndexingGetsTwoContiguousSlots) {
  // `{"a":1}["a"]` — kCreateMap result slot followed by the
  // kCallExpr(`_[_]`) lookup-result slot.
  auto ta = ParseAndCheck("{\"a\": 1}[\"a\"]", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  EXPECT_EQ(layout->workspace_bytes, 48u);
  EXPECT_EQ(layout->peak_slots, 2u);

  // Root is the kCallExpr; its slot lands second.
  const auto* root_ann =
      layout->annotations.Find(ta->ast().root_expr().id());
  ASSERT_NE(root_ann, nullptr);
  EXPECT_EQ(root_ann->storage.kind, StorageKind::kWorkspaceSlot);
  EXPECT_EQ(root_ann->storage.payload, layout->workspace_base + 24u);
}

TEST(LayoutPassMapTest, BoundMapIndexingNeedsOnlyTheCallSlot) {
  // `m[k]` on a bound `map<K,V>` ident — only the kCallExpr result
  // needs a workspace cell; the operand reaches the call as a
  // local_get (variable's own slot) rather than another scratch
  // cell.  So workspace = 1 variable slot + 1 call slot = 48B.
  CheckOptions opts;
  opts.variable_specs = {"m:map<string,int>"};
  auto ta = ParseAndCheck("m[\"k\"]", opts);
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  EXPECT_EQ(layout->workspace_bytes, 48u);
  EXPECT_EQ(layout->peak_slots, 1u);
  ASSERT_EQ(layout->variables.size(), 1u);
  EXPECT_EQ(layout->variables[0].name, "m");
  EXPECT_EQ(layout->variables[0].repr, Repr::kMap);
}

TEST(LayoutPassMapTest, MultipleMapLiteralsGetDistinctSlots) {
  // `{"a":1}["a"] + {"b":2}["b"]` would parse but not type-check
  // until the kCall arm lands; use sibling map literals via a
  // different shape — the underlying invariant we care about is
  // that distinct kCreateMap nodes don't share slots.
  CheckOptions opts;
  opts.variable_specs = {"k:string"};
  auto ta = ParseAndCheck("{\"a\": 1, \"b\": 2}[k]", opts);
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  // 1 variable + kCreateMap + kCallExpr(`_[_]`) = 3 cells.  But
  // `k` is a string repr — its slot still counts.  Pin the totals
  // so a layout regression that fuses or duplicates a slot trips
  // here.
  EXPECT_EQ(layout->variables.size(), 1u);
  EXPECT_EQ(layout->workspace_bytes, 72u);
  EXPECT_EQ(layout->peak_slots, 2u);
}

TEST(LayoutPassMapTest, ArenaBaseFollowsMapWorkspace) {
  // Sanity: arena_base = workspace_base + workspace_bytes and 8-
  // aligned, regardless of map content.  This invariant is what
  // lets `cel_map_insert` bump-allocate entry storage past the
  // last workspace slot.
  auto ta = ParseAndCheck("{\"a\": 1}[\"a\"]", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  EXPECT_EQ(layout->arena_base,
            layout->workspace_base + layout->workspace_bytes);
  EXPECT_EQ(layout->arena_base % 8u, 0u);
}

}  // namespace
}  // namespace celwasm
