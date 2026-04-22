#include "compiler_v2/codegen/slot_allocator.h"

#include <cstdint>

#include "absl/log/absl_check.h"

namespace celwasm {

SlotAllocator::SlotAllocator(uint32_t base_offset, bool debug_mode)
    : base_offset_(base_offset), debug_mode_(debug_mode) {
  // CelValue is 24 bytes, 8-byte aligned — 24 is a multiple of 8,
  // so contiguous 24-byte cells from an 8-aligned base stay 8-aligned.
  ABSL_CHECK_EQ(base_offset_ % 8u, 0u)
      << "SlotAllocator base_offset must be 8-byte aligned, got "
      << base_offset_;
}

uint32_t SlotAllocator::Acquire() {
  const uint32_t offset = base_offset_ + (peak_slots_ * kCelValueSize);
  ++peak_slots_;
  return offset;
}

void SlotAllocator::Release(uint32_t offset) {
  // Naive path (M1–M9): no-op.  At M10 this returns `offset` to a
  // free-list when `!debug_mode_`; for now both paths behave the same.
  (void)offset;
}

}  // namespace celwasm
