#include "compiler/codegen/slot_allocator.h"

#include <algorithm>
#include <cstdint>

#include "absl/log/absl_check.h"

namespace celwasm {

SlotAllocator::SlotAllocator(uint32_t base_offset, bool debug_mode)
    : base_offset_(base_offset), debug_mode_(debug_mode) {
  // Slot alignment requirement is 16 bytes — what the runtime
  // helpers' `memory.atomic.*` ops demand.  The stride
  // (`kSlotStride` == 32) is a multiple of 16, so a 16-aligned
  // base keeps every slot 16-aligned regardless of index.  We do
  // NOT require 32-alignment because a 16-aligned base already
  // satisfies the runtime contract; LayoutPass rounds the
  // workspace base to 16 (not 32) so it can pack rodata
  // immediately ahead.
  ABSL_CHECK_EQ(base_offset_ % 16u, 0u)
      << "SlotAllocator base_offset must be 16-byte aligned, got "
      << base_offset_;
}

uint32_t SlotAllocator::Acquire() {
  if (debug_mode_) {
    // Debug mode: bump-only, no reuse.  Every Acquire returns a
    // fresh cell and grows the watermark; Release is a no-op.
    // Preserves the original M1 semantics for layout dumps.
    const uint32_t offset = base_offset_ + (bump_ * kSlotStride);
    ++bump_;
    peak_slots_ = std::max(peak_slots_, bump_);
    return offset;
  }
  // Reuse path: pop the most-recently-Released cell.
  if (!free_list_.empty()) {
    const uint32_t offset = free_list_.back();
    free_list_.pop_back();
    ++live_slots_;
    return offset;
  }
  // Bump path: hand out the next cell, grow live + watermark.
  const uint32_t offset = base_offset_ + (bump_ * kSlotStride);
  ++bump_;
  ++live_slots_;
  peak_slots_ = std::max(peak_slots_, live_slots_);
  return offset;
}

void SlotAllocator::Release(uint32_t offset) {
  if (debug_mode_) {
    // Layout-dump path: no reuse.
    (void)offset;
    return;
  }
  ABSL_CHECK_GE(live_slots_, 1u)
      << "SlotAllocator::Release called more times than Acquire";
  --live_slots_;
  free_list_.push_back(offset);
}

}  // namespace celwasm
