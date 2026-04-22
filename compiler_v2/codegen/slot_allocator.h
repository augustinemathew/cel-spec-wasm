#ifndef CELWASM_COMPILER_V2_CODEGEN_SLOT_ALLOCATOR_H_
#define CELWASM_COMPILER_V2_CODEGEN_SLOT_ALLOCATOR_H_

// Hands out 24-byte CelValue cells in the workspace region of linear
// memory.  Callers `Acquire` a cell for each result they need to
// store, and `Release` it once they're done reading from it — a
// regular alloc/free pair, no assumptions about how or whether
// offsets get reused.  Every `kCall` / `kSelect` / `kList` /
// `kCreateMap` / `kCreateStruct` node's `NodeAnnotation::storage`
// carries a `kWorkspaceSlot` with the byte offset of its cell.
//
// Usage (LayoutPass):
//
//   // Workspace starts right after rodata; 8-byte aligned.
//   SlotAllocator slots(/*base_offset=*/rodata_base + rodata_bytes,
//                       /*debug_mode=*/true);
//
//   // Post-order walk.  Leaves store into rodata / locals; every
//   // internal node acquires one cell for its result, and releases
//   // its children's cells now that it has consumed them.
//   void AssignSlots(const cel::Expr& e, WasmAnnotations& anno) {
//     for (const cel::Expr* child : Children(e)) AssignSlots(*child, anno);
//     NodeAnnotation& a = *anno.Mutable(e.id());
//     switch (e.kind_case()) {
//       case cel::ExprKindCase::kConstExpr:
//         // Literal packed by StaticMemoryBuilder; payload is the
//         // rodata offset, not a workspace cell.
//         break;
//       case cel::ExprKindCase::kIdentExpr:
//         a.storage = {StorageKind::kLocal, a.local_index};
//         break;
//       default:
//         for (const cel::Expr* child : Children(e)) {
//           const Storage& s = anno.Get(child->id()).storage;
//           if (s.kind == StorageKind::kWorkspaceSlot) slots.Release(s.offset);
//         }
//         a.storage = {StorageKind::kWorkspaceSlot, slots.Acquire()};
//         break;
//     }
//   }
//
//   // After the walk, this is how many bytes of workspace the expr
//   // module's memory needs:
//   const uint32_t workspace_bytes = slots.total_bytes();
//
// Example trace for `(a + b) * c` (base_offset = 128):
//   visit a           kIdentExpr, no slot
//   visit b           kIdentExpr, no slot
//   visit (a + b)     Acquire → 128
//   visit c           kIdentExpr, no slot
//   visit ((a+b)*c)   Release(128); Acquire → 128 (reused) or 152 (fresh)
//
// The walker code above is the whole contract — LayoutPass doesn't
// inspect the numbers that come back.  Implementation note: M1's
// `Acquire` is monotonic and `Release` is a no-op (peak_slots() = 2,
// total_bytes() = 48 for the example); M10's Sethi-Ullman path reuses
// released cells via a free-list under `!debug_mode` (peak_slots() =
// 1, total_bytes() = 24 for the same expression).

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
