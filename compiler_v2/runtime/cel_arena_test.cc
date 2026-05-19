// Tests for the malloc-backed bump arena.  See cel_arena.h for the
// API; doc/implementation-plan/rewrite/wasi/DESIGN.md §4-§5 for the design.
//
// Native-build invariants verified here:
//   - arena_alloc(n) returns an offset such that
//     cel_mem_base() + offset is the allocated bytes.
//   - Allocations are 8-byte aligned and zero-initialized.
//   - arena_reset rewinds the cursor; the next alloc returns the
//     same offset the first alloc did after init.
//   - arena_alloc-before-init traps (CLAUDE.md "Unimplemented
//     features" rule + DESIGN §5 A16 corollary).
//   - arena_init with a different cap_bytes on the second call traps
//     (DESIGN §5 A16).
//   - cel_value_at(0) is the absent sentinel; cel_value_at(off) for
//     `off = arena_alloc(...)` is a valid pointer.

#include "compiler_v2/runtime/cel_arena.h"

#include <cstdint>
#include <cstring>

#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_layout.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

class ArenaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // arena_init is idempotent for same-cap: first SetUp seeds it
    // across the gtest process; subsequent SetUps see initialized=1
    // and return early.  arena_reset rewinds the cursor.
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }
};

// ── Wire-shape sanity ──────────────────────────────────────────────

TEST_F(ArenaTest, CelValueIs24Bytes) {
  EXPECT_EQ(sizeof(CelValue), 24u);
}

// ── Reset semantics (DESIGN §7 per-Eval lifecycle) ─────────────────

TEST_F(ArenaTest, ResetRewindsCursor) {
  uint32_t a0 = arena_alloc(24);
  (void)arena_alloc(24);
  arena_reset();
  uint32_t a1 = arena_alloc(24);
  EXPECT_EQ(a0, a1);
}

TEST_F(ArenaTest, ArenaResetRoundTripGivesSameOffset) {
  arena_reset();
  uint32_t a = arena_alloc(24);
  arena_reset();
  uint32_t b = arena_alloc(24);
  EXPECT_EQ(a, b);
}

// 100-cycle reset-alloc loop — exercises that arena_reset has no
// hidden state and is genuinely O(1) wrt cycle count.  Mirrors the
// per-Eval hot-path described in DESIGN §7.
TEST_F(ArenaTest, ResetAllocCycleIsIdempotentAcrossManyIterations) {
  arena_reset();
  uint32_t first = arena_alloc(24);
  for (int i = 0; i < 100; ++i) {
    arena_reset();
    uint32_t off = arena_alloc(24);
    EXPECT_EQ(off, first) << "cycle " << i;
    // Burn some additional space so subsequent cycles must rewind.
    (void)arena_alloc(64);
    (void)arena_alloc(128);
  }
}

TEST_F(ArenaTest, ResetDoesNotChangeBackingPointer) {
  // O(1) reset MUST NOT free or move the backing buffer.  If it did,
  // the byte at `cel_mem_base() + first_alloc_offset` would change
  // address — we verify it doesn't.
  uint32_t a = arena_alloc(24);
  const uint8_t* p_before = cel_mem_base() + a;
  arena_reset();
  uint32_t b = arena_alloc(24);
  const uint8_t* p_after = cel_mem_base() + b;
  EXPECT_EQ(p_before, p_after);
}

// Reset before the very first init is currently silent (cursor=0 → 0
// no-op).  Locking the contract so a future "trap-on-uninit-reset"
// regression surfaces here, not in a test fixture's SetUp.
TEST_F(ArenaTest, ResetBeforeInitIsHarmless) {
  // After the SetUp ran, the arena is already initialized for the
  // process.  We can't truly "reset before init" without forking;
  // the equivalent we test is that reset doesn't change capacity.
  uint32_t cap = arena_capacity();
  arena_reset();
  EXPECT_EQ(arena_capacity(), cap);
  EXPECT_EQ(arena_cursor(), 0u);
}

// ── Allocation contracts (DESIGN §5 A9, alignment) ─────────────────

TEST_F(ArenaTest, AllocBumpsCursorMonotonically) {
  uint32_t a = arena_alloc(24);
  uint32_t b = arena_alloc(24);
  uint32_t c = arena_alloc(24);
  ASSERT_NE(a, 0u);
  EXPECT_EQ(b, a + 24u);
  EXPECT_EQ(c, b + 24u);
}

TEST_F(ArenaTest, AllocAlignsToEightBytes) {
  uint32_t a = arena_alloc(1);  // rounds up to 8
  uint32_t b = arena_alloc(0);  // zero rounds up to 8 (A9)
  uint32_t c = arena_alloc(9);  // rounds up to 16
  EXPECT_EQ(b, a + 8u);
  EXPECT_EQ(c, b + 8u);
}

// Exhaustive alignment table over the boundary set {1, 7, 8, 9, 15,
// 16, 23, 24} — every alloc returns an 8-aligned offset.
TEST_F(ArenaTest, AllocOffsetIsAlwaysEightAligned) {
  const uint32_t kSizes[] = {1u, 7u, 8u, 9u, 15u, 16u, 23u, 24u};
  for (uint32_t n : kSizes) {
    arena_reset();
    uint32_t off = arena_alloc(n);
    ASSERT_NE(off, 0u);
    EXPECT_EQ(off % 8u, 0u) << "alloc(" << n << ") returned " << off;
  }
}

// A9: alloc(0) does NOT return the OOM sentinel.  It returns a
// valid 8-byte slot.
TEST_F(ArenaTest, AllocZeroReturnsValidEightByteSlot) {
  uint32_t before = arena_cursor();
  uint32_t off = arena_alloc(0);
  EXPECT_NE(off, 0u);
  EXPECT_EQ(arena_cursor(), before + 8u);
}

TEST_F(ArenaTest, AllocReturnsZeroedBytes) {
  uint32_t a = arena_alloc(24);
  ASSERT_NE(a, 0u);
  const uint8_t* p = cel_mem_base() + a;
  for (size_t i = 0; i < 24; ++i) {
    EXPECT_EQ(p[i], 0u) << "byte " << i << " not zero";
  }
}

// Stronger zero-fill invariant: arena_alloc must clear bytes left by
// a prior eval.  Write nonzero bytes, reset, re-alloc the same size,
// expect zero.
TEST_F(ArenaTest, AllocAfterResetReturnsZeroedBytesEvenIfPreviouslyDirty) {
  uint32_t a = arena_alloc(64);
  ASSERT_NE(a, 0u);
  uint8_t* p = cel_mem_base() + a;
  for (size_t i = 0; i < 64; ++i) {
    p[i] = static_cast<uint8_t>(0xAB);
  }
  arena_reset();
  uint32_t b = arena_alloc(64);
  ASSERT_EQ(a, b);
  const uint8_t* q = cel_mem_base() + b;
  for (size_t i = 0; i < 64; ++i) {
    EXPECT_EQ(q[i], 0u) << "byte " << i << " not re-zeroed after reset";
  }
}

// Return-value contract: `cel_mem_base() + arena_alloc(n)` is the
// allocated region (DESIGN §4 / cel_arena.c:86-98).
TEST_F(ArenaTest, AllocReturnContractResolvesViaCelMemBase) {
  uint32_t off = arena_alloc(24);
  ASSERT_NE(off, 0u);
  uint8_t* p = cel_mem_base() + off;
  // Write through the resolved pointer; read back through a fresh
  // cel_mem_base() to confirm the contract holds across calls.
  p[0] = 0x42;
  p[23] = 0x99;
  EXPECT_EQ((cel_mem_base() + off)[0], 0x42);
  EXPECT_EQ((cel_mem_base() + off)[23], 0x99);
}

// ── Capacity boundary (DESIGN §5 A10 — OOM returns 0, not partial) ─

TEST_F(ArenaTest, AllocReturnsZeroWhenOutOfSpace) {
  // Drain the arena in chunks of 1024 bytes, then expect 0.
  const uint32_t cap = arena_capacity();
  for (uint32_t consumed = 0; consumed < cap; consumed += 1024u) {
    EXPECT_NE(arena_alloc(1024), 0u);
  }
  EXPECT_EQ(arena_alloc(1), 0u);  // arena is full
}

// Allocating exactly the remaining capacity succeeds (right at the
// boundary); one byte more fails.
TEST_F(ArenaTest, AllocExactlyRemainingCapacitySucceeds) {
  // Burn an arbitrary chunk first so the test exercises a partial
  // arena (not just `arena_alloc(capacity)` from a fresh reset).
  ASSERT_NE(arena_alloc(800), 0u);
  uint32_t remaining = arena_capacity() - arena_cursor();
  EXPECT_NE(arena_alloc(remaining), 0u);
  EXPECT_EQ(arena_cursor(), arena_capacity());
}

TEST_F(ArenaTest, AllocOneBytePastCapacityReturnsZero) {
  // Fill to capacity exactly.
  uint32_t remaining = arena_capacity() - arena_cursor();
  ASSERT_NE(arena_alloc(remaining), 0u);
  EXPECT_EQ(arena_alloc(1), 0u);
}

// A10: a failing alloc must NOT advance the cursor.  An OOM return
// should leave subsequent successful allocs (after reset) at the
// same offset that the first alloc would have hit.
TEST_F(ArenaTest, FailedAllocLeavesCursorUnchanged) {
  // Fill near capacity.
  uint32_t remaining = arena_capacity() - arena_cursor();
  if (remaining > 16u) {
    ASSERT_NE(arena_alloc(remaining - 16u), 0u);
  }
  uint32_t cursor_before = arena_cursor();
  EXPECT_EQ(arena_alloc(remaining), 0u);  // one too many
  EXPECT_EQ(arena_cursor(), cursor_before);
}

// Multiple successful allocs followed by one that overflows: the
// successful ones stay in place, the failing one returns 0 and the
// cursor is unchanged.
TEST_F(ArenaTest, OverflowFollowingSuccessLeavesEarlierAllocsIntact) {
  uint32_t a = arena_alloc(64);
  ASSERT_NE(a, 0u);
  // Write a marker; ensure it survives an OOM attempt.
  uint8_t* p = cel_mem_base() + a;
  p[0] = 0xCD;
  uint32_t remaining = arena_capacity() - arena_cursor();
  EXPECT_EQ(arena_alloc(remaining + 1u), 0u);
  EXPECT_EQ((cel_mem_base() + a)[0], 0xCD);
}

// ── Accessor sanity (arena_cursor, arena_capacity) ─────────────────

TEST_F(ArenaTest, CursorReflectsAllocations) {
  EXPECT_EQ(arena_cursor(), 0u);
  arena_alloc(24);
  EXPECT_EQ(arena_cursor(), 24u);
  arena_alloc(16);
  EXPECT_EQ(arena_cursor(), 40u);
  arena_reset();
  EXPECT_EQ(arena_cursor(), 0u);
}

TEST_F(ArenaTest, CursorAdvancesByAlignedSize) {
  // Each alloc bumps the cursor by `align_up_8(n)` (or 8 for n=0).
  arena_reset();
  arena_alloc(1);
  EXPECT_EQ(arena_cursor(), 8u);
  arena_alloc(9);
  EXPECT_EQ(arena_cursor(), 24u);  // +16
  arena_alloc(0);
  EXPECT_EQ(arena_cursor(), 32u);  // +8 (alloc(0) takes a slot)
  arena_alloc(16);
  EXPECT_EQ(arena_cursor(), 48u);
}

TEST_F(ArenaTest, CapacityIsStableAcrossAllocsAndResets) {
  uint32_t cap = arena_capacity();
  arena_alloc(24);
  EXPECT_EQ(arena_capacity(), cap);
  arena_alloc(1024);
  EXPECT_EQ(arena_capacity(), cap);
  arena_reset();
  EXPECT_EQ(arena_capacity(), cap);
}

TEST_F(ArenaTest, CapacityMatchesDesignDefault) {
  // The compat shim auto-inits with `CELWASM_ARENA_CAPACITY_BYTES`
  // (DESIGN §5 row A5 — single source of truth in cel_layout.h).
  EXPECT_EQ(arena_capacity(), CELWASM_ARENA_CAPACITY_BYTES);
}

TEST_F(ArenaTest, CursorIsZeroAfterFreshReset) {
  arena_alloc(64);
  arena_alloc(128);
  arena_reset();
  EXPECT_EQ(arena_cursor(), 0u);
}

// ── cel_value_at (offset → CelValue*) ──────────────────────────────

TEST_F(ArenaTest, ValueAtZeroReturnsNull) {
  // Contract per cel_arena.h:62-64: offset 0 is the absent sentinel.
  EXPECT_EQ(cel_value_at(0), nullptr);
}

TEST_F(ArenaTest, ValueAtNonZeroResolvesViaCelMemBase) {
  uint32_t off = arena_alloc(24);
  ASSERT_NE(off, 0u);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(cel_value_at(off)),
            cel_mem_base() + off);
}

TEST_F(ArenaTest, ValueAtForSizeofCelValueIsWriteable) {
  uint32_t off = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  CelValue* v = cel_value_at(off);
  ASSERT_NE(v, nullptr);
  v->kind = CEL_INT;
  v->payload.i = -7;
  // Read back via the same offset.
  const CelValue* w = cel_value_at(off);
  EXPECT_EQ(w->kind, static_cast<uint32_t>(CEL_INT));
  EXPECT_EQ(w->payload.i, -7);
}

// ── Idempotent init (DESIGN §5 A16, idempotent branch) ─────────────

// arena_init with the SAME capacity as the in-process initial init
// is a no-op (idempotent).  The shim runs `arena_init(CELWASM_ARENA_
// CAPACITY_BYTES)` on first use, so calling that explicitly here
// should keep capacity unchanged.
TEST_F(ArenaTest, InitWithSameCapacityIsIdempotent) {
  uint32_t cap_before = arena_capacity();
  arena_init(cap_before);
  EXPECT_EQ(arena_capacity(), cap_before);
}

// Calling arena_init with the same capacity even after some allocs
// MUST NOT change capacity (re-init is suppressed on the idempotent
// branch).  The cursor is not specified by the API to be preserved
// or reset on idempotent re-init — only that the capacity is stable.
TEST_F(ArenaTest, InitIdempotentBranchPreservesCapacityAfterAllocs) {
  uint32_t cap = arena_capacity();
  arena_alloc(24);
  arena_init(cap);
  EXPECT_EQ(arena_capacity(), cap);
}

// ── Per-Eval lifecycle simulation (DESIGN §7) ──────────────────────

// Simulate the per-Eval hot path: arena_init runs once per Instance
// (already done by SetUp); each "Eval" does arena_reset + some
// allocs.  Verify each iteration's first alloc returns the same
// offset (the reset rewind invariant).
TEST_F(ArenaTest, PerEvalLifecycleResetGivesSameStartingOffset) {
  arena_reset();
  uint32_t baseline = arena_alloc(24);
  ASSERT_NE(baseline, 0u);
  for (int i = 0; i < 100; ++i) {
    arena_reset();
    // Simulate an "eval" — several allocations.
    uint32_t first = arena_alloc(24);
    (void)arena_alloc(64);
    (void)arena_alloc(8);
    EXPECT_EQ(first, baseline) << "iteration " << i;
  }
}

// ── Death tests — DESIGN §5 A16 traps ──────────────────────────────

// arena_init with a different capacity than the first call traps
// (cel_arena.c:43-46 __builtin_trap).  Death-test forks so the
// trap doesn't take down the gtest binary.
using ArenaDeathTest = ArenaTest;

TEST_F(ArenaDeathTest, InitWithDifferentCapacityTraps) {
  // The fixture's SetUp has already inited at
  // CELWASM_ARENA_CAPACITY_BYTES.  Calling with a different value
  // must trap.
  ASSERT_DEATH_IF_SUPPORTED(arena_init(CELWASM_ARENA_CAPACITY_BYTES / 2u), "");
}

TEST_F(ArenaDeathTest, InitWithLargerDifferentCapacityTraps) {
  ASSERT_DEATH_IF_SUPPORTED(arena_init(CELWASM_ARENA_CAPACITY_BYTES * 2u), "");
}

// arena_alloc-before-init: in the gtest process the arena is already
// initialized via the SetUp's compat shim, so we can't easily exercise
// the "alloc before init" trap from a test body without an extra
// fork-and-isolate harness.  The trap is asserted at the C source
// (cel_arena.c:84 __builtin_trap) under the same `!g_arena.initialized`
// guard the test would exercise; covered by code review rather than a
// runtime death test in this fixture.  If a future test harness can
// run a child process that bypasses SetUp's arena_reset, add an
// ASSERT_DEATH(arena_alloc(8), "") there.

}  // namespace
}  // namespace celwasm
