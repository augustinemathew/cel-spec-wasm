#ifndef CELWASM_COMPILER_V2_CODEGEN_SLOT_ALLOCATOR_H_
#define CELWASM_COMPILER_V2_CODEGEN_SLOT_ALLOCATOR_H_

// Hands out 24-byte CelValue cells in the workspace region of linear
// memory.  Every kCall / kSelect / kList / kCreateMap / kCreateStruct
// node's `NodeAnnotation::storage` carries a `kWorkspaceSlot` with the
// byte offset of its cell; LayoutPass uses `Acquire()` to assign
// those offsets during the slot-Strahler walk (§6.3).
//
// M1 — naive path (this file).  `Acquire` is monotonic; `Release` is
// a no-op.  Peak slot count equals the number of `kWorkspaceSlot`
// nodes in the AST — bounded linearly in expression size.
//
// M10 — Sethi–Ullman aliasing (future).  The constructor's
// `debug_mode` flag flips to off and `Release` starts returning
// freed slots to a free-list.  The naive path survives as the
// `debug_mode == true` mode so debug-layout dumps keep per-expr
// slot distinctness for arena walkers.

#include <cstdint>

namespace celwasm {

class SlotAllocator {
 public:
  // `base_offset` is the linear-memory offset at which the workspace
  // region starts; must be 8-byte aligned (CelValue alignment).
  // `debug_mode` is the M10-compatible surface: naive mode is
  // functionally `debug_mode == true`.  In M1 the flag is stored but
  // both paths behave identically (monotonic + no-op release).
  SlotAllocator(uint32_t base_offset, bool debug_mode);

  // Returns the byte offset of a fresh 24-byte CelValue cell.
  uint32_t Acquire();

  // Returns the cell at `offset` to the allocator.  No-op in M1 —
  // the naive path never reuses slots.  Kept on the interface so
  // callers can write Sethi–Ullman-shaped code now and get aliasing
  // for free at M10.
  void Release(uint32_t offset);

  // Number of distinct 24-byte cells the allocator has handed out.
  // After LayoutPass runs, this is the workspace size the expr
  // module needs (bytes = `peak_slots() * 24`).
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
