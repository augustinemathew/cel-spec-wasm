#include "compiler/codegen/slot_allocator.h"

#include <cstdint>
#include <limits>
#include <string>

#include "absl/log/absl_check.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

TEST(SlotAllocatorTest, EmptyAtConstruction) {
  SlotAllocator a(/*base_offset=*/0, /*debug_mode=*/true);
  EXPECT_EQ(a.peak_slots(), 0u);
  EXPECT_EQ(a.total_bytes(), 0u);
  EXPECT_EQ(a.base_offset(), 0u);
  EXPECT_TRUE(a.debug_mode());
}

TEST(SlotAllocatorTest, AcquireIsMonotonicFromBase) {
  // 32-aligned base (matches `kSlotStride`).  Three Acquires bump
  // by kSlotStride (32B), not by sizeof(CelValue) (24B).
  SlotAllocator a(/*base_offset=*/32, /*debug_mode=*/true);
  EXPECT_EQ(a.Acquire(), 32u);
  EXPECT_EQ(a.Acquire(), 32u + 32u);
  EXPECT_EQ(a.Acquire(), 32u + 64u);
  EXPECT_EQ(a.peak_slots(), 3u);
  EXPECT_EQ(a.total_bytes(), 96u);
}

TEST(SlotAllocatorTest, ReleaseIsNoOpInDebugMode) {
  SlotAllocator a(/*base_offset=*/0, /*debug_mode=*/true);
  const uint32_t s0 = a.Acquire();
  const uint32_t s1 = a.Acquire();
  a.Release(s0);
  a.Release(s1);
  // Debug mode pins bump-only — Release is a no-op, the next
  // Acquire still hands out a fresh cell.
  EXPECT_EQ(a.Acquire(), 64u);
  EXPECT_EQ(a.peak_slots(), 3u);
}

TEST(SlotAllocatorTest, ReleaseEnablesReuseInProdMode) {
  // Production allocator (debug_mode == false) recycles released
  // cells via a LIFO free list — what makes the m4 / m7 nested-
  // aggregate fix viable (the aggregate visitor uses PreVisit
  // Acquire to keep its slot OUT of the free list during the
  // subtree's lifetime; kCall / kSelect use this Release-Acquire
  // reuse path safely because their helpers are read-before-write).
  SlotAllocator a(/*base_offset=*/0, /*debug_mode=*/false);
  const uint32_t s0 = a.Acquire();
  const uint32_t s1 = a.Acquire();
  EXPECT_EQ(a.peak_slots(), 2u);
  a.Release(s1);
  a.Release(s0);
  // LIFO: the most-recently-released cell comes back first.
  EXPECT_EQ(a.Acquire(), s0);
  EXPECT_EQ(a.Acquire(), s1);
  EXPECT_EQ(a.peak_slots(), 2u);
}

TEST(SlotAllocatorTest, DebugModeFlagIsPreserved) {
  SlotAllocator debug(/*base_offset=*/0, /*debug_mode=*/true);
  SlotAllocator prod(/*base_offset=*/0, /*debug_mode=*/false);
  EXPECT_TRUE(debug.debug_mode());
  EXPECT_FALSE(prod.debug_mode());
  // First Acquire on a fresh allocator is identical regardless of
  // mode — both bump from the base; divergence only appears once
  // Release is called.
  EXPECT_EQ(debug.Acquire(), 0u);
  EXPECT_EQ(prod.Acquire(), 0u);
}

TEST(SlotAllocatorTest, NonZeroBaseOffsetRespected) {
  SlotAllocator a(/*base_offset=*/1024, /*debug_mode=*/true);
  EXPECT_EQ(a.Acquire(), 1024u);
  EXPECT_EQ(a.Acquire(), 1056u);  // base + kSlotStride
  EXPECT_EQ(a.base_offset(), 1024u);
}

TEST(SlotAllocatorDeathTest, UnalignedBaseOffsetChecks) {
  // Anything not a multiple of 16 fails — including values that
  // satisfied the old 8-byte rule (8, 24).  This is load-bearing:
  // an 8-aligned base with 24-byte stride would put odd-indexed
  // slots at offset % 16 == 8 and the runtime helpers' memory.
  // atomic.* ops would trap on the misaligned slot.
  EXPECT_DEATH(
      { SlotAllocator a(/*base_offset=*/1, /*debug_mode=*/true); },
      "16-byte aligned");
  EXPECT_DEATH(
      { SlotAllocator a(/*base_offset=*/8, /*debug_mode=*/true); },
      "16-byte aligned");
  EXPECT_DEATH(
      { SlotAllocator a(/*base_offset=*/24, /*debug_mode=*/true); },
      "16-byte aligned");
}

TEST(SlotAllocatorTest, AlignedBaseOffsetsAccepted) {
  // Any multiple of 16 is a legal base — 16-stride doesn't apply
  // to the base, only to the cells the allocator hands out.
  SlotAllocator a0(/*base_offset=*/0, /*debug_mode=*/true);
  SlotAllocator a16(/*base_offset=*/16, /*debug_mode=*/true);
  SlotAllocator a32(/*base_offset=*/32, /*debug_mode=*/true);
  SlotAllocator a48(/*base_offset=*/48, /*debug_mode=*/true);
  EXPECT_EQ(a0.Acquire(), 0u);
  EXPECT_EQ(a16.Acquire(), 16u);
  EXPECT_EQ(a32.Acquire(), 32u);
  EXPECT_EQ(a48.Acquire(), 48u);
}

TEST(SlotAllocatorTest, EverySlotIsSixteenByteAligned) {
  // The whole point of the kSlotStride bump is that no Acquire
  // ever returns an offset whose `% 16 != 0` — that's what kept
  // the wasm32-wasi-threads runtime helpers' memory.atomic.*
  // ops from trapping.  Hand out enough cells to walk both
  // modes through a few page boundaries; assert the invariant.
  for (uint32_t base : {0u, 32u, 64u, 1024u, 65536u}) {
    SlotAllocator a(base, /*debug_mode=*/true);
    for (int i = 0; i < 1024; ++i) {
      const uint32_t off = a.Acquire();
      EXPECT_EQ(off % 16u, 0u)
          << "base=" << base << " slot=" << i << " offset=" << off;
    }
  }
}

// ──────────────────────────────────────────────────────────────────
// Slot pressure for `1 + 2 + ... + N` — the left-associated `+`-chain
// that exposes the M10-never-shipped slot-reuse gap and (downstream)
// the wasm-trap regression pinned by
// `e2e/known_bugs_test::LongArith_2000Terms_UnalignedAtomicTrap`.
//
// Simulates exactly the traversal pattern `LayoutPass` runs: post-
// order over the AST, each `+` releases every workspace child
// before acquiring its own slot.  Constants on the right are
// rodata, no workspace slot — only the LHS subtree carries a
// workspace cell.
//
// For an N-term chain:
//   - Under proper Release reuse: peak_slots() = 1 — each inner `+`
//     releases its workspace child and Acquire hands the same cell
//     back.  Chain length doesn't matter.
//   - Under the naive (no-op Release) path: peak_slots() = N - 1.
//     Every intermediate gets a fresh cell.  At N >= 2000 the
//     workspace at 24-byte stride from an 8-aligned base eventually
//     hands out a CelValue offset whose `%16` boundary the
//     wasm32-wasi-threads shared-memory atomic ops in the runtime
//     helpers refuse — hence the e2e `unaligned atomic` trap.
//
// We simulate both shapes here as data points; the assertions document
// what each shape DOES today, and the planned post-fix assertions are
// in `LeftAssocAdditionChain_AfterReleaseFix_StaysConstant` below.
// ──────────────────────────────────────────────────────────────────

// Walks the simulated post-order traversal of `1 + 2 + ... + n`.
// Returns the allocator's final peak_slots() so a test can assert
// on the shape directly.
uint32_t WalkLeftAssocAdditionChain(uint32_t n, bool debug_mode) {
  ABSL_CHECK_GE(n, 2u) << "chain needs at least 2 terms";
  SlotAllocator a(/*base_offset=*/0, debug_mode);
  // First `+`: both children rodata (constants 1 and 2).
  //   - No workspace children to release.
  //   - Acquire result cell.
  uint32_t lhs_slot = a.Acquire();
  // Each subsequent `+ k`:
  //   - LHS is the prior subtree's result cell (workspace) — release.
  //   - RHS is rodata constant `k` — no release.
  //   - Acquire result cell.
  for (uint32_t k = 3; k <= n; ++k) {
    a.Release(lhs_slot);
    lhs_slot = a.Acquire();
  }
  return a.peak_slots();
}

class LeftAssocAdditionChainNaiveBlowsUp
    : public ::testing::TestWithParam<uint32_t> {};

TEST_P(LeftAssocAdditionChainNaiveBlowsUp, AcquireBumpsPerOperator) {
  const uint32_t n = GetParam();
  const uint32_t peak = WalkLeftAssocAdditionChain(n, /*debug_mode=*/true);
  // Current (naive) behaviour: every `+` grows the workspace by one
  // cell.  N terms = N-1 binary ops = N-1 acquires = N-1 peak slots.
  EXPECT_EQ(peak, n - 1u)
      << "N=" << n
      << " expected naive O(N) blow-up; if peak is now O(1) the "
         "slot-reuse fix has landed — un-skip the matching "
         "AfterReleaseFix test in this file and update this row.";
}

INSTANTIATE_TEST_SUITE_P(PowersAndStressSizes,
                         LeftAssocAdditionChainNaiveBlowsUp,
                         ::testing::Values(2u, 10u, 100u, 1000u, 2000u, 10000u),
                         [](const ::testing::TestParamInfo<uint32_t>& info) {
                           return "N" + std::to_string(info.param);
                         });

class LeftAssocAdditionChainAfterReleaseFix
    : public ::testing::TestWithParam<uint32_t> {};

TEST_P(LeftAssocAdditionChainAfterReleaseFix, StaysAtOneSlot) {
  const uint32_t n = GetParam();
  const uint32_t peak = WalkLeftAssocAdditionChain(n, /*debug_mode=*/false);
  EXPECT_EQ(peak, 1u)
      << "N=" << n
      << " expected reuse to cap peak slots at 1 for any left-associated "
         "`+`-chain over constants; got "
      << peak;
}

INSTANTIATE_TEST_SUITE_P(PowersAndStressSizes,
                         LeftAssocAdditionChainAfterReleaseFix,
                         ::testing::Values(2u, 10u, 100u, 1000u, 2000u, 10000u),
                         [](const ::testing::TestParamInfo<uint32_t>& info) {
                           return "N" + std::to_string(info.param);
                         });

// ──────────────────────────────────────────────────────────────────
// Slot pressure for Sethi–Ullman labeling — `(a + b) + (c + d)` and
// the balanced-tree generalisation.  Under SU labeling, peak slots
// for a balanced binary-op tree of 2^d leaves should be d (NOT
// 2^d - 1 as the naive walker would emit).  Validates the future
// implementation; SKIP'd until LayoutPass runs SU.
// ──────────────────────────────────────────────────────────────────

// Simulates a balanced binary tree of depth `d`, recursive — each
// internal node visits left, releases left, visits right, releases
// right, then acquires its own cell.  Under Sethi–Ullman ordering
// the higher-labelled child is visited first; for a perfectly
// balanced tree the two subtrees have equal labels, so the order
// doesn't matter — both must coexist before the parent's Acquire.
//
// Peak slot count for a balanced tree of depth d (2^d leaves):
//   - Naive walker:       2^d - 1   (every op gets a fresh cell)
//   - Release + naive:    d         (two children live, then 1)
//   - Sethi-Ullman label: d         (same — SU labels for a balanced
//                                    tree match the depth)
uint32_t WalkBalancedAdditionTree(uint32_t depth, bool debug_mode);

uint32_t WalkBalancedSubtree(SlotAllocator& a, uint32_t depth) {
  if (depth == 0) {
    return std::numeric_limits<uint32_t>::max();  // sentinel: rodata leaf
  }
  // Walk left.
  uint32_t left = WalkBalancedSubtree(a, depth - 1);
  // Walk right.
  uint32_t right = WalkBalancedSubtree(a, depth - 1);
  // Release workspace children, acquire this node's cell.
  if (left != std::numeric_limits<uint32_t>::max()) a.Release(left);
  if (right != std::numeric_limits<uint32_t>::max()) a.Release(right);
  return a.Acquire();
}

uint32_t WalkBalancedAdditionTree(uint32_t depth, bool debug_mode) {
  SlotAllocator a(/*base_offset=*/0, debug_mode);
  WalkBalancedSubtree(a, depth);
  return a.peak_slots();
}

class BalancedAdditionTreeNaiveBlowsUp
    : public ::testing::TestWithParam<uint32_t> {};

TEST_P(BalancedAdditionTreeNaiveBlowsUp, AcquirePerInternalNode) {
  const uint32_t d = GetParam();
  const uint32_t peak = WalkBalancedAdditionTree(d, /*debug_mode=*/true);
  // 2^d leaves → 2^d - 1 internal nodes → 2^d - 1 acquires under naive.
  const uint32_t expected = (1u << d) - 1u;
  EXPECT_EQ(peak, expected)
      << "depth=" << d
      << " expected naive O(2^d) blow-up; if peak is now O(d) the "
         "slot-reuse fix has landed — un-skip AfterReleaseFix below.";
}

INSTANTIATE_TEST_SUITE_P(Depths, BalancedAdditionTreeNaiveBlowsUp,
                         ::testing::Values(1u, 2u, 3u, 4u, 8u, 10u),
                         [](const ::testing::TestParamInfo<uint32_t>& info) {
                           return "Depth" + std::to_string(info.param);
                         });

class BalancedAdditionTreeAfterReleaseFix
    : public ::testing::TestWithParam<uint32_t> {};

TEST_P(BalancedAdditionTreeAfterReleaseFix, StaysAtDepth) {
  const uint32_t d = GetParam();
  const uint32_t peak = WalkBalancedAdditionTree(d, /*debug_mode=*/false);
  EXPECT_EQ(peak, d)
      << "depth=" << d
      << " expected reuse to cap peak slots at the tree depth; got " << peak;
}

INSTANTIATE_TEST_SUITE_P(Depths, BalancedAdditionTreeAfterReleaseFix,
                         ::testing::Values(1u, 2u, 3u, 4u, 8u, 10u),
                         [](const ::testing::TestParamInfo<uint32_t>& info) {
                           return "Depth" + std::to_string(info.param);
                         });

}  // namespace
}  // namespace celwasm
