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
//
// Usage (LayoutPass, naive path):
//
//   // Workspace starts right after rodata; 8-byte aligned.
//   SlotAllocator slots(/*base_offset=*/rodata_base + rodata_bytes,
//                       /*debug_mode=*/true);
//
//   // Post-order walk: leaves store into rodata / locals (no Acquire);
//   // every internal node acquires one 24-byte slot for its result.
//   void AssignSlots(const cel::Expr& e, WasmAnnotations& anno) {
//     for (const cel::Expr* child : Children(e)) AssignSlots(*child, anno);
//     NodeAnnotation& a = *anno.Mutable(e.id());
//     switch (e.kind_case()) {
//       case cel::ExprKindCase::kConstExpr:
//         // Literal already packed by StaticMemoryBuilder; payload is
//         // the rodata offset.  No workspace slot acquired.
//         break;
//       case cel::ExprKindCase::kIdentExpr:
//         // Ident reads come from a wasm local, not workspace.
//         a.storage = {StorageKind::kLocal, a.local_index};
//         break;
//       default:
//         // Calls, selects, list/map/struct builds — every computed
//         // CelValue lands in its own 24-byte cell.
//         a.storage = {StorageKind::kWorkspaceSlot, slots.Acquire()};
//         break;
//     }
//   }
//
//   // After the walk, this is how many bytes of workspace the expr
//   // module's memory needs:
//   const uint32_t workspace_bytes = slots.total_bytes();
//
// Example for `(a + b) * c` under the naive path (base_offset = 128):
//   post-order: a, b, (a+b), c, ((a+b)*c)
//   Acquire() calls:                  128, 152        — slots for the
//                                                       two internal
//                                                       kCall nodes
//   peak_slots()                      2
//   total_bytes()                     48
//
// Under the M10 aliasing path the same expression needs only one slot
// (the inner `+` result aliases with the outer `*` result after the
// right-hand `c` is consumed); a `Release` between the two Acquires
// lets the allocator hand back the same offset.  Callers write the
// Sethi–Ullman dance (visit-heavy-subtree-first, Release non-aliased
// input, Acquire parent) today; in M1 the Release is a no-op so peak
// equals acquire count.

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
