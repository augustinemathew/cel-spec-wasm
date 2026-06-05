#ifndef CELWASM_COMPILER_CODEGEN_SLOT_ALLOCATOR_H_
#define CELWASM_COMPILER_CODEGEN_SLOT_ALLOCATOR_H_

// Hands out CelValue cells in the workspace region of linear
// memory.  Callers `Acquire` a cell for each computed result and
// `Release` the cell once every read of that result is done.
// A `Release`'d cell goes onto a LIFO free list and is reused by
// the next `Acquire`; if the free list is empty `Acquire` bumps a
// fresh cell at the next stride offset.  Slot offsets returned to
// callers are opaque, unique while live, and 16-byte aligned.
//
// Cell stride is 32 bytes (`kSlotStride`), not 24 (the size of a
// CelValue) — every slot must be 16-byte aligned from a 16-byte-
// aligned workspace base.  Why: the runtime is compiled under
// `wasm32-wasi-threads`, whose libc internals (and any cross-
// module call into a helper that touches a `__atomic_*` builtin)
// emit `memory.atomic.*` ops on the workspace region.  A 24-byte
// stride from an 8-aligned base puts every other slot at an
// offset whose `% 16 == 8`, and the first time a
// `memory.atomic.*` hits one of those addresses the engine traps
// with `wasm trap: unaligned atomic` — the symptom pinned by
// `e2e/known_bugs_test::LongArith_2000Terms_NoUnalignedAtomicTrap`.
// CelValue itself stays 24 bytes; the trailing 8 bytes of each
// 32-byte cell are pad that no codepath reads or writes.
//
// **When NOT to release.**  The reuse contract only works when the
// caller's emitted code reads the operand slot BEFORE writing the
// parent's slot.  kSelect / kCall arms honor this: their runtime
// helpers (e.g. `cel_int_add_at_vv`, `cel_host.cel_get_field`,
// `cel_map_lookup`) accept operands as inputs and the result slot
// as output, reading inputs first, so it's safe to release the
// operand and have the allocator hand back that same cell as the
// parent's result.
//
// kListExpr / kMapExpr / kStructExpr DO NOT honor it.  Their
// codegen pattern is:
//
//     cel_list_create(parent_slot, N)         // WRITES parent
//     for each element i:
//       <lower element_i>                     // writes element_i's slot
//       cel_list_append_at(parent_slot, e_i)  // reads parent + element_i
//
// If `parent_slot` aliased any `element_i`'s slot — even one
// released by a sibling visit several PostVisits earlier — the
// later element's emit (which writes that slot) would clobber the
// parent's just-written CEL_LIST_ARENA handle before the parent's
// next append could read it.  So **aggregate visitors must NOT
// call Release on their operand slots** — those slots stay live
// from the allocator's view through the rest of LayoutPass,
// guaranteeing no later Acquire can hand them back.  The static
// cost is a few never-reused cells per nested literal, which is
// negligible against the slot-exhaustion cap.  Surfaced by
// `e2e/m4_test::NestedListOuterRoundTrip` and
// `e2e/m7_test::MapStringToMessageFromLiteral`; pinned by the
// e2e assertion battery in `e2e/slot_aliasing_test.cc`.
//
// When to release.  LayoutPass is a post-order recursive visitor.
// Each `Visit` call returns the storage its subtree's result
// lives in, and when it returns, that storage is already live
// (the Acquire has happened, or the rodata/local slot has been
// fixed).  The parent reads the child's value exactly once — by
// emitting a load that references the child's offset — and has
// no further use for it afterwards.  So the rule is: after
// recursing into each child, release every workspace-resident
// child, then acquire a cell for this node's own result.  Non-
// workspace children (`kConstExpr` in rodata, `kIdentExpr` in a
// wasm local) have nothing to release.
//
// Usage (LayoutPass visitor):
//
//   SlotAllocator slots(/*base_offset=*/rodata_base + rodata_bytes,
//                       /*debug_mode=*/true);
//
//   Storage Visit(const cel::Expr& e, WasmAnnotations& anno) {
//     Storage storage;
//     switch (e.kind_case()) {
//       case cel::ExprKindCase::kConstExpr:
//         storage = {StorageKind::kRodata, RodataOffsetOf(e)};
//         break;
//       case cel::ExprKindCase::kIdentExpr:
//         storage = {StorageKind::kLocal, anno.Get(e.id()).local_index};
//         break;
//       default: {
//         // Recurse: every child returns with its cell already live.
//         std::vector<Storage> child_storage;
//         for (const cel::Expr* child : Children(e)) {
//           child_storage.push_back(Visit(*child, anno));
//         }
//         // Children's values have been read into our emitted
//         // instructions; their cells die here.
//         for (const Storage& s : child_storage) {
//           if (s.kind == StorageKind::kWorkspaceSlot) slots.Release(s.offset);
//         }
//         // And ours is born here.
//         storage = {StorageKind::kWorkspaceSlot, slots.Acquire()};
//         break;
//       }
//     }
//     anno.Mutable(e.id())->storage = storage;
//     return storage;
//   }
//
//   Visit(expr, anno);
//   const uint32_t workspace_bytes = slots.total_bytes();
//
// Example trace for `(a + b) + (c + d)` (base_offset = 128,
// allocator with free-list reuse):
//
//   visit a, b, c, d     idents, no workspace slots
//   visit (a + b)        Acquire       → 128      live: {128}
//   visit (c + d)        Acquire       → 160      live: {128, 160}
//   visit outer +        Release 128; Release 160
//                        Acquire       → 128      live: {128}
//   peak_slots() = 2     total_bytes() = 64
//
// Note how the outer node releases *both* children before its own
// Acquire — that's the rule at work, and it's what lets the free
// list hand back 128 instead of growing to 176.  Under a no-op
// `Release` the three Acquires would instead bump monotonically to
// 128, 152, 176 (peak_slots() = 3, total_bytes() = 72).  LayoutPass
// doesn't inspect these numbers — either allocator produces a
// workspace sized for the emitted loads and stores.  M1 ships the
// no-op form; M10 flips on the free list under `!debug_mode`.

#include <cstdint>
#include <vector>

#include "compiler/memory_layout.h"

namespace celwasm {

class SlotAllocator {
 public:
  // Cell-to-cell byte stride.  Must be a multiple of 16 so every
  // slot the allocator hands out is 16-byte aligned from a
  // 16-byte-aligned base — required by `memory.atomic.*` ops in
  // the wasm32-wasi-threads runtime helpers (see header preamble).
  // 32 is the smallest such stride >= sizeof(CelValue) == 24.
  // Mirrors `MemoryLayout::kSlotStride` — the static_assert below
  // pins them together so the two can't drift.
  static constexpr uint32_t kSlotStride = MemoryLayout::kSlotStride;

  // `base_offset` is the linear-memory offset at which the workspace
  // region starts; must be 16-byte aligned (slot alignment).
  // `debug_mode == true` pins the allocator to bump-only Acquire —
  // every Acquire returns a fresh cell, Release is a no-op.  Useful
  // for layout dumps that want per-expr slot distinctness.
  // `debug_mode == false` enables free-list reuse: Release returns
  // the cell to a LIFO pool that the next Acquire pulls from.
  SlotAllocator(uint32_t base_offset, bool debug_mode);

  // Returns the byte offset of a 32-byte CelValue cell the caller
  // can use as result storage.  The first 24 bytes are the
  // CelValue; the trailing 8 bytes are padding.  Pass the offset
  // to `Release` when the cell's contents are no longer needed.
  uint32_t Acquire();

  // Returns the cell at `offset` to the allocator.  After this
  // call the caller must not read or write the cell; a later
  // `Acquire` may hand it back out.
  void Release(uint32_t offset);

  // Peak number of cells live simultaneously.  After LayoutPass
  // runs, this is the workspace size the expr module needs
  // (bytes = `peak_slots() * kSlotStride`).
  uint32_t peak_slots() const {
    return peak_slots_;
  }

  uint32_t total_bytes() const {
    return peak_slots_ * kSlotStride;
  }

  uint32_t base_offset() const {
    return base_offset_;
  }

  bool debug_mode() const {
    return debug_mode_;
  }

 private:
  uint32_t base_offset_;
  bool debug_mode_;
  // `bump_` is the monotonically-increasing next-cell index — the
  // fallback when the free list is empty (and the only path in
  // debug mode).
  uint32_t bump_ = 0;
  // `live_slots_` is the count of cells currently checked out
  // (Acquired minus Released).  Tracked only in non-debug mode.
  uint32_t live_slots_ = 0;
  // `peak_slots_` is the high-water mark — the workspace size the
  // expr module needs; monotonically non-decreasing.
  uint32_t peak_slots_ = 0;
  // LIFO free list of recently-Released cell offsets.  Used in
  // non-debug mode so the most-recently-Released cell is re-
  // Acquired first — keeps cache lines hot and the bump pointer
  // from drifting upward unnecessarily.
  std::vector<uint32_t> free_list_;
};

// Pins SlotAllocator's stride to the compile-time memory-layout
// constant.  Any future drift between the two (e.g. someone bumps
// MemoryLayout::kSlotStride to 64 to accommodate a wider CelValue
// without updating this class) fails the build here rather than
// silently miscompiling at the wasm.atomic boundary.
static_assert(SlotAllocator::kSlotStride == MemoryLayout::kSlotStride,
              "SlotAllocator::kSlotStride must mirror "
              "MemoryLayout::kSlotStride");

}  // namespace celwasm

#endif  // CELWASM_COMPILER_CODEGEN_SLOT_ALLOCATOR_H_
