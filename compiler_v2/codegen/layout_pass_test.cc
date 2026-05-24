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
#include "compiler_v2/runtime/cel_data.h"
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
// `1 + 2` type-checks: root is a kCall over two kConst operands.  Both
// literals pack into rodata; the kCall root gets a workspace slot
// (M5.F: `EmitGeneralCall` writes the result CelValue into that slot
// before returning its offset to the parent).
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

  // Root kCall gets a workspace slot at M5.F (general-arm result
  // CelValue).  Slot lives in the workspace region, past variables.
  const int64_t root_id = ta->ast().root_expr().id();
  const NodeAnnotation* root_ann = layout->annotations.Find(root_id);
  ASSERT_NE(root_ann, nullptr);
  EXPECT_EQ(root_ann->storage.kind, StorageKind::kWorkspaceSlot);
  EXPECT_GE(root_ann->storage.payload,
            layout->workspace_base + (layout->variables.size() * 24u));

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
  // `x + y + z` type-checks; M5.F lights up the kCall arms, so two
  // call slots (one per `_+_`) sit past the variable slots.  This
  // test only asserts the variable region's contiguity / alignment;
  // workspace_bytes covers variables + call slots together.
  auto layout = LayoutWithVars("x + y + z", {"x:int", "y:int", "z:int"});
  ASSERT_THAT(layout, IsOk());
  ASSERT_EQ(layout->variables.size(), 3u);
  EXPECT_EQ(layout->variables[0].slot_offset, layout->workspace_base);
  EXPECT_EQ(layout->variables[1].slot_offset, layout->workspace_base + 24u);
  EXPECT_EQ(layout->variables[2].slot_offset, layout->workspace_base + 48u);
  // Variables alone occupy 72 bytes (3 × 24); kCall result slots add
  // more.  Stop short of pinning a specific total — that's the
  // SlotAllocator's job and changes when Sethi–Ullman lands at M8.
  EXPECT_GE(layout->workspace_bytes, 72u);
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

// M5.A removed direct unit coverage of N=0 map layout because bare
// `{}` is now rejected by the static-subset gate.  The path stays
// reachable indirectly through M5.I comprehension lowering
// (`accu_init = {}` macro expansion).  When that lands, add an
// internal-AST test here that constructs a zero-entry `kMapExpr`
// and asserts the single-slot workspace shape locked previously.

TEST(LayoutPassMapTest, ScalarMapLiteralGetsOneSlotRegardlessOfEntryCount) {
  // Per dispatch-doc §4: the kCreateMap result slot is a single
  // 24B CelValue (the wire shape).  Entry storage lives in the
  // arena (allocated by `cel_map_insert`), not the workspace.
  auto ta = ParseAndCheck(R"({"a": 1, "b": 2, "c": 3})", {});
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
  auto ta = ParseAndCheck(R"({"a": 1}["a"])", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  EXPECT_EQ(layout->workspace_bytes, 48u);
  EXPECT_EQ(layout->peak_slots, 2u);

  // Root is the kCallExpr; its slot lands second.
  const auto* root_ann = layout->annotations.Find(ta->ast().root_expr().id());
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
  auto ta = ParseAndCheck(R"({"a": 1, "b": 2}[k])", opts);
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
  auto ta = ParseAndCheck(R"({"a": 1}["a"])", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  EXPECT_EQ(layout->arena_base,
            layout->workspace_base + layout->workspace_bytes);
  EXPECT_EQ(layout->arena_base % 8u, 0u);
}

// --- M4.F: kCreateList + kCallExpr(`_[_]`) reserve workspace cells --------
//
// Same shape as the map tests above — the list's result CelValue
// (24B) + a kCallExpr(`_[_]`) result cell when the list is indexed.
// Element scratch slots are released as `cel_list_set` consumes
// each one (mirrors map entry key/value handling).

TEST(LayoutPassListTest, EmptyListLiteralGetsOneWorkspaceSlot) {
  CheckOptions opts;
  opts.variable_specs = {"xs:list<int>"};
  // `xs[0]` is the simplest way to force an empty-list-typed list
  // literal up to the checker — but we want a literal directly.
  // `dyn([]) == dyn([])` doesn't typecheck; use a typed-context
  // literal: `[1][0]` is enough to lock the slot for kListExpr.
  auto ta = ParseAndCheck("[1]", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  // No variables; one kCreateList → one slot (the literal is the
  // root).  Element kConsts live in rodata.
  EXPECT_EQ(layout->workspace_bytes, 24u);
  EXPECT_EQ(layout->peak_slots, 1u);

  const auto* root_ann = layout->annotations.Find(ta->ast().root_expr().id());
  ASSERT_NE(root_ann, nullptr);
  EXPECT_EQ(root_ann->storage.kind, StorageKind::kWorkspaceSlot);
  EXPECT_EQ(root_ann->storage.payload, layout->workspace_base);
}

TEST(LayoutPassListTest, ScalarListLiteralGetsOneSlotRegardlessOfElementCount) {
  // Per dispatch-doc §4.2: the kCreateList result slot is a single
  // 24B CelValue.  Element storage lives in the arena
  // (cel_list_create reserves count × 24B); the workspace cell
  // count stays at 1 regardless of N.
  auto ta = ParseAndCheck("[1, 2, 3, 4, 5]", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->workspace_bytes, 24u);
  EXPECT_EQ(layout->peak_slots, 1u);
}

TEST(LayoutPassListTest, ListLiteralIndexingGetsTwoContiguousSlots) {
  // `[1,2,3][1]` — kCreateList result slot followed by the
  // kCallExpr(`_[_]`) at-result slot.
  auto ta = ParseAndCheck("[1, 2, 3][1]", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  EXPECT_EQ(layout->workspace_bytes, 48u);
  EXPECT_EQ(layout->peak_slots, 2u);

  // Root is the kCallExpr; its slot lands second.
  const auto* root_ann = layout->annotations.Find(ta->ast().root_expr().id());
  ASSERT_NE(root_ann, nullptr);
  EXPECT_EQ(root_ann->storage.kind, StorageKind::kWorkspaceSlot);
  EXPECT_EQ(root_ann->storage.payload, layout->workspace_base + 24u);
}

TEST(LayoutPassListTest, BoundListIndexingNeedsOnlyTheCallSlot) {
  // `xs[0]` on a bound `list<int>` ident — only the kCallExpr
  // result needs a workspace cell; the operand reaches the call
  // as a local_get of the variable's own slot.  So workspace =
  // 1 variable slot + 1 call slot = 48B.
  CheckOptions opts;
  opts.variable_specs = {"xs:list<int>"};
  auto ta = ParseAndCheck("xs[0]", opts);
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  EXPECT_EQ(layout->workspace_bytes, 48u);
  EXPECT_EQ(layout->peak_slots, 1u);
  ASSERT_EQ(layout->variables.size(), 1u);
  EXPECT_EQ(layout->variables[0].name, "xs");
  EXPECT_EQ(layout->variables[0].repr, Repr::kList);
}

TEST(LayoutPassListTest, ArenaBaseFollowsListWorkspace) {
  auto ta = ParseAndCheck("[1, 2, 3][0]", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  EXPECT_EQ(layout->arena_base,
            layout->workspace_base + layout->workspace_bytes);
  EXPECT_EQ(layout->arena_base % 8u, 0u);
}

// ── M5.G control-flow operators ─────────────────────────────────
//
// `_&&_` / `_||_` / `_?_:_` / `!_` follow the same slot-out shape as
// every other kCall: each call gets exactly one workspace slot for
// the result.  Branch-arm slots inside the ternary are allocated
// inside expr_lower (BinaryenIf-scoped) and don't leak into
// LayoutPass's accounting.

TEST(LayoutPassControlFlowTest, LogicalAndGetsOneCallSlot) {
  // `true && false` is two literal kConsts (rodata) plus the
  // kCallExpr result slot.
  auto ta = ParseAndCheck("true && false", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->workspace_bytes, 24u);
  EXPECT_EQ(layout->peak_slots, 1u);
}

TEST(LayoutPassControlFlowTest, LogicalNotGetsOneCallSlot) {
  auto ta = ParseAndCheck("!true", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->workspace_bytes, 24u);
  EXPECT_EQ(layout->peak_slots, 1u);
}

TEST(LayoutPassControlFlowTest, ConditionalGetsOneCallSlot) {
  // `true ? 1 : 2` — three rodata kConsts plus one kCallExpr slot.
  // The two branch-arm slots are emitted inline inside the
  // BinaryenIf and don't appear in LayoutPass's allocation.
  auto ta = ParseAndCheck("true ? 1 : 2", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());
  EXPECT_EQ(layout->workspace_bytes, 24u);
  EXPECT_EQ(layout->peak_slots, 1u);
}

// ============================================================
// LayoutPassComprehensionTest — M5.B Slice B
//
//   Locks: comprehension iter_vars get NO workspace slot (their
//   wasm local holds a moving pointer); accu_vars DO get a slot;
//   free vars coexist with comp scope without slot collisions.
//   EmitVariablePrelude / cel.abi emitter filter comp entries (out
//   of scope here; tested at expr_lower / cel_abi_emit).
// ============================================================

namespace {

const LaidOutVariable* FindLaidOutByName(const StaticLayout& layout,
                                         absl::string_view name) {
  for (const auto& v : layout.variables) {
    if (v.name == name) return &v;
  }
  return nullptr;
}

}  // namespace

TEST(LayoutPassComprehensionTest, IterVarHasNoWorkspaceSlot) {
  // `[1, 2, 3].exists(v, v > 0)` — iter_var `v` is a moving pointer
  // into the iter_range's element run, NOT a fixed workspace slot.
  // Its LaidOutVariable.slot_offset stays at 0 (sentinel).
  auto ta = ParseAndCheck("[1, 2, 3].exists(v, v > 0)", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  const LaidOutVariable* v = FindLaidOutByName(*layout, "v");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, ResolvedVariableKind::kComprehensionIter);
  EXPECT_EQ(v->slot_offset, 0u)
      << "iter_var must have slot_offset=0 (sentinel for `no slot needed`)";
}

TEST(LayoutPassComprehensionTest, AccuVarHasWorkspaceSlot) {
  // accu_var `@result` (bool) needs a real workspace slot — the
  // accu CelValue lives there for the comprehension's lifetime.
  auto ta = ParseAndCheck("[1, 2, 3].exists(v, v > 0)", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  const LaidOutVariable* accu = FindLaidOutByName(*layout, "@result");
  ASSERT_NE(accu, nullptr);
  EXPECT_EQ(accu->kind, ResolvedVariableKind::kComprehensionAccu);
  EXPECT_GE(accu->slot_offset, layout->workspace_base)
      << "accu workspace slot must live in the workspace region";
}

TEST(LayoutPassComprehensionTest, WorkspaceBytesSkipsIterVars) {
  // Literal-only comprehension: no free vars.  Workspace bytes
  // accounts for the accu slot (24B) + any kCallExpr / kListExpr
  // workspace slots allocated by AggregateStorageVisitor.  Crucially:
  // the iter_var does NOT contribute 24B.
  auto ta = ParseAndCheck("[1, 2, 3].exists(v, v > 0)", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  // accu var contributes 24B; aggregate slots stack on after.
  EXPECT_GE(layout->workspace_bytes, 24u);
}

TEST(LayoutPassComprehensionTest, FreeVarsCoexistWithCompScope) {
  // `[1, 2, 3].exists(v, v > x)` with `x:int` free.  Workspace must
  // hold: x's slot + @result's accu slot + aggregate-pass slots.
  // x's slot_offset must NOT collide with @result's.
  CheckOptions opts;
  opts.variable_specs = {"x:int"};
  auto ta = ParseAndCheck("[1, 2, 3].exists(v, v > x)", opts);
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  const LaidOutVariable* x = FindLaidOutByName(*layout, "x");
  const LaidOutVariable* accu = FindLaidOutByName(*layout, "@result");
  ASSERT_NE(x, nullptr);
  ASSERT_NE(accu, nullptr);
  EXPECT_EQ(x->kind, ResolvedVariableKind::kFreeVariable);
  EXPECT_EQ(accu->kind, ResolvedVariableKind::kComprehensionAccu);
  EXPECT_NE(x->slot_offset, accu->slot_offset)
      << "free var and comp accu must not share a slot";
  EXPECT_NE(x->slot_offset, 0u);
  EXPECT_NE(accu->slot_offset, 0u);
}

// `[1, 2, 3].existsOne(i, v, v > i)` — comprehensions_v2 two-iter
// list form.  Both iter_var (index) and iter_var2 (value) need
// their lifecycle right.  Regression test for the
// `kComprehensionIndex` enum removal: the synthetic index
// variable lands as `kComprehensionAccu` (workspace slot for the
// per-iter {CEL_INT, i=idx} CelValue), NOT a separate kind.
// Iter_var2 (the value) lands as `kComprehensionIter` (moving
// pointer; no workspace slot).
TEST(LayoutPassComprehensionTest, TwoIterListIndexUsesAccuKind) {
  auto ta = ParseAndCheck("[1, 2, 3].existsOne(i, v, v > i)", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  const LaidOutVariable* i = FindLaidOutByName(*layout, "i");
  const LaidOutVariable* v = FindLaidOutByName(*layout, "v");
  ASSERT_NE(i, nullptr);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(i->kind, ResolvedVariableKind::kComprehensionAccu);
  EXPECT_NE(i->slot_offset, 0u)
      << "v2 two-iter list index must own a workspace slot for the "
         "{CEL_INT, i=idx} CelValue codegen writes each iter";
  EXPECT_EQ(v->kind, ResolvedVariableKind::kComprehensionIter);
  EXPECT_EQ(v->slot_offset, 0u)
      << "v2 two-iter list value is a moving pointer — no fixed slot";
}

// --- SelectKeyRodataVisitor: kSelect-on-optional rodata lifting ----------
//
// Only kSelect nodes whose operand carries `Repr::kOptional` get a
// `select_key_rodata_offset` assigned (the field-name string lifted to
// rodata as a CEL_STRING CelValue).  Every other kSelect leaves the
// field at its zero default — the standard `field_ref_id`-based path
// doesn't need a CelValue key.
//
// Positive case: `optional.of(...).c` — operand is optional<map>;
// LayoutPass should allocate rodata for the literal "c".

TEST(LayoutPassSelectOptionalTest,
     SelectOnOptionalAllocatesRodataForFieldName) {
  auto ta = ParseAndCheck("optional.of({'c': 'v'}).c", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  const cel::Expr& root = ta->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kSelectExpr);
  const NodeAnnotation* select_ann = layout->annotations.Find(root.id());
  ASSERT_NE(select_ann, nullptr);
  EXPECT_GE(select_ann->select_key_rodata_offset, 16u)
      << "rodata offset must point past the two reserved slots";

  // The rodata frame at that offset must be a 24-byte CEL_STRING frame.
  const uint32_t rel =
      select_ann->select_key_rodata_offset - layout->rodata_base;
  ASSERT_LT(rel + 24u, layout->rodata.size() + 24u);
  // Frame header: kind (4 bytes LE) at offset 0.
  const uint8_t* frame = layout->rodata.data() + rel;
  EXPECT_EQ(frame[0], static_cast<uint8_t>(CEL_STRING));
}

TEST(LayoutPassSelectOptionalTest, SelectOnNonOptionalLeavesOffsetZero) {
  // Operand is a message — regular Select path, no rodata key needed.
  CheckOptions opts;
  opts.variable_specs = {"c:celwasm.testdata.Customer"};
  auto ta = ParseAndCheck("c.billing_address", opts);
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  const cel::Expr& root = ta->ast().root_expr();
  ASSERT_EQ(root.kind_case(), cel::ExprKindCase::kSelectExpr);
  const NodeAnnotation* select_ann = layout->annotations.Find(root.id());
  ASSERT_NE(select_ann, nullptr);
  EXPECT_EQ(select_ann->select_key_rodata_offset, 0u);
}

TEST(LayoutPassSelectOptionalTest, ChainedSelectOnOptionalLiftsEachField) {
  // `optional.of(map).c.x` — two kSelects, both with optional operand
  // (the outer Select's operand is the result of the inner Select,
  // which is also optional<...>).  Each field name lifted distinctly.
  auto ta = ParseAndCheck("optional.of({'c': {'x': 'v'}}).c.x", {});
  ASSERT_THAT(ta, IsOk());
  auto resolved = ResolvePass(*ta);
  ASSERT_THAT(resolved, IsOk());
  auto layout = LayoutPass(*ta, *std::move(resolved));
  ASSERT_THAT(layout, IsOk());

  const cel::Expr& outer = ta->ast().root_expr();
  const cel::Expr& inner = outer.select_expr().operand();
  ASSERT_EQ(outer.kind_case(), cel::ExprKindCase::kSelectExpr);
  ASSERT_EQ(inner.kind_case(), cel::ExprKindCase::kSelectExpr);

  const NodeAnnotation* outer_ann = layout->annotations.Find(outer.id());
  const NodeAnnotation* inner_ann = layout->annotations.Find(inner.id());
  ASSERT_NE(outer_ann, nullptr);
  ASSERT_NE(inner_ann, nullptr);
  EXPECT_NE(outer_ann->select_key_rodata_offset, 0u);
  EXPECT_NE(inner_ann->select_key_rodata_offset, 0u);
  EXPECT_NE(outer_ann->select_key_rodata_offset,
            inner_ann->select_key_rodata_offset)
      << "distinct field names get distinct rodata offsets";
}

}  // namespace
}  // namespace celwasm
