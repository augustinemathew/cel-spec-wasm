// OOM / poisoned-source poison-view vends, and the adversarial-count
// allocation guards.
//
// Contract under test (the silent-degrade family this replaces):
//
//   - `cel_list_arena_view` used to return the SOURCE slot on
//     snapshot-alloc OOM and on poisoned/wrong-kind sources, which
//     downstream codegen walked as an EMPTY list — `[1/0].exists(x,
//     x == 2)` silently evaluated `false`.  It now vends a synthetic
//     one-element CEL_LIST_ARENA view whose element carries the
//     poison (source error/unknown verbatim; CEL_ERR_OVERFLOW for
//     OOM; CEL_ERR_TYPE_MISMATCH for kind drift).
//   - `cel_map_iter_init` used to return the 0 ("empty") handle on
//     state-alloc OOM and on poisoned sources; it now vends a
//     one-entry HOST-shaped iteration whose key and value carry the
//     poison.
//   - Both vendors fall back to the per-Instance emergency block
//     (`arena_oom_block`, reserved at arena_init OUTSIDE the
//     resettable arena) when the arena itself is exhausted, so the
//     poison stays expressible at true OOM.
//   - Aggregate allocation size math rejects adversarial counts
//     (stride×count / count+count would wrap u32 on wasm32) by
//     poisoning CEL_ERR_OVERFLOW — reject, never wrap.
//   - The deep-equality walk's scratch-cell OOM poisons
//     CEL_ERR_OVERFLOW instead of returning a false verdict.

#include <cstdint>
#include <initializer_list>
#include <utility>

#include "gtest/gtest.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_list.h"
#include "runtime/cel_make.h"
#include "runtime/cel_map.h"
#include "runtime/cel_memory.h"

extern "C" {
// codegen-export, intentionally not in an umbrella header — the only
// production caller is the emitted comprehension prologue.
uint32_t cel_list_arena_view(uint32_t list_slot);
}

namespace celwasm {
namespace {

class OomPoisonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
    arena_reset();
  }

  uint32_t NewSlot() {
    return arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  }

  uint32_t MakeIntList(std::initializer_list<int64_t> elems) {
    uint32_t l = NewSlot();
    cel_list_create(l, static_cast<uint32_t>(elems.size()));
    for (int64_t v : elems)
      cel_list_append_at(l, cel_make_int(v));
    return l;
  }

  uint32_t MakeIntMap(
      std::initializer_list<std::pair<int64_t, int64_t>> entries) {
    uint32_t m = NewSlot();
    cel_map_create(m, static_cast<uint32_t>(entries.size()));
    for (const auto& [k, v] : entries) {
      cel_map_insert(m, cel_make_int(k), cel_make_int(v));
    }
    return m;
  }

  uint32_t MakeError(uint32_t err) {
    uint32_t s = NewSlot();
    cel_value_at(s)->kind = CEL_ERROR;
    cel_value_at(s)->payload.err = err;
    return s;
  }

  // The native arena does not chain, so draining it is deterministic:
  // big chunks first, then byte-size allocations until the 0 sentinel.
  void ExhaustArena() {
    while (arena_alloc(4096) != 0) {
    }
    while (arena_alloc(8) != 0) {
    }
    ASSERT_EQ(arena_alloc(1), 0u);
  }

  // Asserts `view_slot` is a walkable one-element arena list whose
  // element is CEL_ERROR with `want_err` (or CEL_UNKNOWN when
  // `want_err == 0`).
  void ExpectPoisonView(uint32_t view_slot, uint32_t want_kind,
                        uint32_t want_err) {
    const CelValue* view = cel_value_at(view_slot);
    ASSERT_EQ(view->kind, static_cast<uint32_t>(CEL_LIST_ARENA));
    const auto* hdr = reinterpret_cast<const ArenaListHeader*>(
        cel_mem_base() + view->payload.arena_list.header_ptr);
    ASSERT_EQ(hdr->count, 1u);
    const auto* elem = reinterpret_cast<const CelValue*>(
        cel_mem_base() + hdr->elements_offset);
    EXPECT_EQ(elem->kind, want_kind);
    if (want_kind == CEL_ERROR) {
      EXPECT_EQ(elem->payload.err, want_err);
    }
  }
};

// ── emergency block reservation ────────────────────────────────────

TEST_F(OomPoisonTest, OomBlockIsReservedAndSurvivesReset) {
  const uint32_t blk = arena_oom_block();
  ASSERT_NE(blk, 0u);
  arena_reset();
  EXPECT_EQ(arena_oom_block(), blk);
  // The block lives OUTSIDE the resettable arena: exhausting the
  // arena must not hand its bytes out.
  ExhaustArena();
  EXPECT_EQ(arena_oom_block(), blk);
}

// ── cel_list_arena_view ────────────────────────────────────────────

TEST_F(OomPoisonTest, ArenaListSourcePassesThrough) {
  uint32_t l = MakeIntList({1, 2});
  EXPECT_EQ(cel_list_arena_view(l), l);
}

TEST_F(OomPoisonTest, HostListSnapshotOomVendsOverflowView) {
  uint32_t l = NewSlot();
  cel_value_at(l)->kind = CEL_LIST_HOST;
  cel_value_at(l)->payload.ref_slot = 42;
  ExhaustArena();
  uint32_t view = cel_list_arena_view(l);
  ASSERT_NE(view, l);  // never the un-walkable source slot
  ExpectPoisonView(view, CEL_ERROR, CEL_ERR_OVERFLOW);
}

TEST_F(OomPoisonTest, ErrorListSourceVendsSourceErrorView) {
  // `[1/0].map(x, x)`: the iter_range slot holds the construction
  // error; the view's element must carry it VERBATIM so the
  // comprehension result is the divide-by-zero error, not [].
  uint32_t src = MakeError(CEL_ERR_DIVIDE_BY_ZERO);
  uint32_t view = cel_list_arena_view(src);
  ASSERT_NE(view, src);
  ExpectPoisonView(view, CEL_ERROR, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(OomPoisonTest, UnknownListSourceVendsUnknownView) {
  uint32_t src = NewSlot();
  cel_value_at(src)->kind = CEL_UNKNOWN;
  cel_value_at(src)->payload.unk = 5;
  uint32_t view = cel_list_arena_view(src);
  ExpectPoisonView(view, CEL_UNKNOWN, 0);
  // Unknown payload travels verbatim.
  const CelValue* v = cel_value_at(view);
  const auto* hdr = reinterpret_cast<const ArenaListHeader*>(
      cel_mem_base() + v->payload.arena_list.header_ptr);
  EXPECT_EQ(
      reinterpret_cast<const CelValue*>(cel_mem_base() + hdr->elements_offset)
          ->payload.unk,
      5u);
}

TEST_F(OomPoisonTest, WrongKindListSourceVendsTypeMismatchView) {
  uint32_t view = cel_list_arena_view(cel_make_int(7));
  ExpectPoisonView(view, CEL_ERROR, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(OomPoisonTest, ErrorSourceViewVendsAtTrueOomToo) {
  uint32_t src = MakeError(CEL_ERR_DIVIDE_BY_ZERO);
  ExhaustArena();
  uint32_t view = cel_list_arena_view(src);
  ExpectPoisonView(view, CEL_ERROR, CEL_ERR_DIVIDE_BY_ZERO);
}

// ── cel_map_iter_init ──────────────────────────────────────────────

void ExpectPoisonIter(uint32_t handle, uint32_t want_err) {
  ASSERT_NE(handle, 0u);
  ASSERT_EQ(cel_map_iter_next(handle), 1u);
  uint32_t kslot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  uint32_t vslot = arena_alloc(static_cast<uint32_t>(sizeof(CelValue)));
  // At true OOM these allocs fail; reuse fixed scratch in the block's
  // shadow is not available to tests, so only assert via reads when
  // slots are available.
  if (kslot != 0 && vslot != 0) {
    cel_map_iter_key_at(kslot, handle);
    cel_map_iter_value_at(vslot, handle);
    EXPECT_EQ(cel_value_at(kslot)->kind, static_cast<uint32_t>(CEL_ERROR));
    EXPECT_EQ(cel_value_at(kslot)->payload.err, want_err);
    EXPECT_EQ(cel_value_at(vslot)->kind, static_cast<uint32_t>(CEL_ERROR));
    EXPECT_EQ(cel_value_at(vslot)->payload.err, want_err);
  }
  // Exactly one iteration — the poison iter terminates.
  EXPECT_EQ(cel_map_iter_next(handle), 0u);
}

TEST_F(OomPoisonTest, MapIterStateOomVendsOverflowIteration) {
  uint32_t m = MakeIntMap({{1, 10}, {2, 20}});
  // Pre-allocate the read-back slots BEFORE exhausting so the
  // assertions can read the vended entry.
  uint32_t kslot = NewSlot();
  uint32_t vslot = NewSlot();
  ExhaustArena();
  uint32_t h = cel_map_iter_init(m);
  ASSERT_NE(h, 0u);
  ASSERT_EQ(cel_map_iter_next(h), 1u);
  cel_map_iter_key_at(kslot, h);
  cel_map_iter_value_at(vslot, h);
  EXPECT_EQ(cel_value_at(kslot)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(kslot)->payload.err,
            static_cast<uint32_t>(CEL_ERR_OVERFLOW));
  EXPECT_EQ(cel_value_at(vslot)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(vslot)->payload.err,
            static_cast<uint32_t>(CEL_ERR_OVERFLOW));
  EXPECT_EQ(cel_map_iter_next(h), 0u);
}

TEST_F(OomPoisonTest, ErrorMapSourceVendsSourceErrorIteration) {
  uint32_t src = MakeError(CEL_ERR_DIVIDE_BY_ZERO);
  ExpectPoisonIter(cel_map_iter_init(src), CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(OomPoisonTest, WrongKindMapSourceVendsTypeMismatchIteration) {
  ExpectPoisonIter(cel_map_iter_init(cel_make_int(3)), CEL_ERR_TYPE_MISMATCH);
}

TEST_F(OomPoisonTest, EmptyArenaMapStillReturnsZeroHandle) {
  // Control: a genuinely empty map keeps the cheap 0-handle path.
  uint32_t m = MakeIntMap({});
  EXPECT_EQ(cel_map_iter_init(m), 0u);
}

// ── map literal 3VL (construction strictness) ──────────────────────

TEST_F(OomPoisonTest, MapInsertPropagatesValueError) {
  // `{'a': 1/0}` must poison the MAP value at construction (strict
  // creation, matching list literals); it previously built a map
  // CONTAINING an error value, and `size()` / comprehensions over it
  // produced normal-looking wrong answers.
  uint32_t m = NewSlot();
  cel_map_create(m, 1);
  cel_map_insert(m, cel_make_int(1), MakeError(CEL_ERR_DIVIDE_BY_ZERO));
  EXPECT_EQ(cel_value_at(m)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(m)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

TEST_F(OomPoisonTest, MapInsertPropagatesKeyErrorBeforeKindCheck) {
  // `{1/0: 'a'}` surfaces divide_by_zero, not type_mismatch — the
  // 3VL check precedes the key-kind gate.
  uint32_t m = NewSlot();
  cel_map_create(m, 1);
  cel_map_insert(m, MakeError(CEL_ERR_DIVIDE_BY_ZERO), cel_make_int(1));
  EXPECT_EQ(cel_value_at(m)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(m)->payload.err,
            static_cast<uint32_t>(CEL_ERR_DIVIDE_BY_ZERO));
}

// ── adversarial allocation sizes (reject → poison, never wrap) ─────

TEST_F(OomPoisonTest, ListCreateAdversarialCapacityPoisons) {
  // 0xFFFFFFFF × 24 wraps u32 (and 32-bit size_t on wasm32) to a
  // small number — the unguarded form under-allocated and the
  // element writes scribbled past the run.
  uint32_t l = NewSlot();
  cel_list_create(l, UINT32_MAX);
  EXPECT_EQ(cel_value_at(l)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(l)->payload.err,
            static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(OomPoisonTest, ListCreateJustAboveU32ByteCeilingPoisons) {
  // Smallest capacity whose byte size exceeds the u32 ceiling:
  // ceil((2^32 - 7) / 24) — boundary of the reject region.
  const uint32_t cap = ((UINT32_MAX - 7u) / 24u) + 1u;
  uint32_t l = NewSlot();
  cel_list_create(l, cap);
  EXPECT_EQ(cel_value_at(l)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(l)->payload.err,
            static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(OomPoisonTest, MapCreateAdversarialCapacityPoisons) {
  uint32_t m = NewSlot();
  cel_map_create(m, UINT32_MAX / 24u);  // ×48 ≫ u32
  EXPECT_EQ(cel_value_at(m)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(m)->payload.err,
            static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(OomPoisonTest, ConcatCountAddOverflowPoisons) {
  // Hand-build two list headers with adversarial counts.  The counts
  // sum past UINT32_MAX; the guard must poison before any allocation
  // or element copy runs.
  uint32_t a = MakeIntList({});
  uint32_t b = MakeIntList({});
  auto* ha = reinterpret_cast<ArenaListHeader*>(
      cel_mem_base() + cel_value_at(a)->payload.arena_list.header_ptr);
  auto* hb = reinterpret_cast<ArenaListHeader*>(
      cel_mem_base() + cel_value_at(b)->payload.arena_list.header_ptr);
  ha->count = 0x80000000u;
  hb->count = 0x80000001u;
  uint32_t out = NewSlot();
  cel_list_concat_arena(out, a, b);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

TEST_F(OomPoisonTest, ConcatStrideMultiplyOverflowPoisons) {
  // Counts sum WITHOUT wrapping, but total × 24 exceeds u32 — the
  // stride-multiply guard fires.
  uint32_t a = MakeIntList({});
  uint32_t b = MakeIntList({});
  auto* ha = reinterpret_cast<ArenaListHeader*>(
      cel_mem_base() + cel_value_at(a)->payload.arena_list.header_ptr);
  ha->count = 0x20000000u;  // ×24 = 0x300000000 > u32
  uint32_t out = NewSlot();
  cel_list_concat_arena(out, a, b);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

// ── deep-equality scratch OOM ──────────────────────────────────────

TEST_F(OomPoisonTest, DeepEqScratchOomPoisonsInsteadOfFalseVerdict) {
  // Two single-element lists whose elements are CEL_MESSAGE values:
  // the deep walk needs a scratch cell for the kernel verdict.  With
  // the arena exhausted that allocation fails — the result must be
  // CEL_ERR_OVERFLOW, never a silent `false`.
  uint32_t ma = NewSlot();
  cel_value_at(ma)->kind = CEL_MESSAGE;
  cel_value_at(ma)->payload.msg_slot = 1;
  uint32_t mb = NewSlot();
  cel_value_at(mb)->kind = CEL_MESSAGE;
  cel_value_at(mb)->payload.msg_slot = 2;
  uint32_t a = NewSlot();
  cel_list_create(a, 1);
  cel_list_append_at(a, ma);
  uint32_t b = NewSlot();
  cel_list_create(b, 1);
  cel_list_append_at(b, mb);
  uint32_t out = NewSlot();
  ExhaustArena();
  cel_list_eq_arena(out, a, b);
  EXPECT_EQ(cel_value_at(out)->kind, static_cast<uint32_t>(CEL_ERROR));
  EXPECT_EQ(cel_value_at(out)->payload.err,
            static_cast<uint32_t>(CEL_ERR_OVERFLOW));
}

}  // namespace
}  // namespace celwasm
