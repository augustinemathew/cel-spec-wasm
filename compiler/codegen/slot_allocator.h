#ifndef CELWASM_COMPILER_V2_CODEGEN_SLOT_ALLOCATOR_H_
#define CELWASM_COMPILER_V2_CODEGEN_SLOT_ALLOCATOR_H_

// Hands out 24-byte CelValue cells in the workspace region of
// linear memory.  Callers `Acquire` a cell for each computed
// result and `Release` the cell once every read of that result
// is done.  The allocator may reuse a released cell for a later
// `Acquire`; callers treat the returned offsets as opaque
// storage.
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
//   visit (c + d)        Acquire       → 152      live: {128, 152}
//   visit outer +        Release 128; Release 152
//                        Acquire       → 128      live: {128}
//   peak_slots() = 2     total_bytes() = 48
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

namespace celwasm {

class SlotAllocator {
 public:
  // `base_offset` is the linear-memory offset at which the workspace
  // region starts; must be 8-byte aligned (CelValue alignment).
  // `debug_mode == true` pins the allocator to one cell per
  // `Acquire` even once M10 lands — useful for layout dumps that
  // want per-expr slot distinctness.
  SlotAllocator(uint32_t base_offset, bool debug_mode);

  // Returns the byte offset of a 24-byte CelValue cell the caller
  // can use as result storage.  Pass the offset to `Release` when
  // the cell's contents are no longer needed.
  uint32_t Acquire();

  // Returns the cell at `offset` to the allocator.  After this
  // call the caller must not read or write the cell; a later
  // `Acquire` may hand it back out.
  void Release(uint32_t offset);

  // Peak number of 24-byte cells live simultaneously.  After
  // LayoutPass runs, this is the workspace size the expr module
  // needs (bytes = `peak_slots() * 24`).
  uint32_t peak_slots() const {
    return peak_slots_;
  }

  uint32_t total_bytes() const {
    return peak_slots_ * kCelValueSize;
  }

  uint32_t base_offset() const {
    return base_offset_;
  }

  bool debug_mode() const {
    return debug_mode_;
  }

 private:
  static constexpr uint32_t kCelValueSize = 24;

  uint32_t base_offset_;
  bool debug_mode_;
  uint32_t peak_slots_ = 0;
};

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_CODEGEN_SLOT_ALLOCATOR_H_
