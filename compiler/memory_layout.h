#ifndef CELWASM_COMPILER_MEMORY_LAYOUT_H_
#define CELWASM_COMPILER_MEMORY_LAYOUT_H_

// Single source of truth for compile-time memory-layout constants the
// CEL→wasm compiler reasons about.  Every part of the compiler that
// computes a wasm linear-memory offset, a workspace cap, or a slot
// stride includes this header — no one redefines `262144` or `32` or
// `16` inline.
//
// Why this matters.  cel.memory's first 256 KiB is the **only** region
// the expr module can safely write to.  `-Wl,--global-base=262144` on
// the runtime build pins wasi-libc's static data + heap + stack
// **above** that line, so any byte the expr module writes at or
// past `kReservedLowMemoryBytes` falls into wasi-libc's address
// space — corrupting `malloc` bookkeeping, libc globals, the
// shadow stack, or whatever else happens to live there.  No wasm
// trap is emitted; the process limps along with corrupted libc
// state and traps minutes later inside an unrelated helper.  So
// the LayoutPass MUST refuse to compile any expression whose
// rodata + workspace would extend past the reserved low region.
//
// `kGuardBytes` is the slack we keep between the last workspace
// cell and `kReservedLowMemoryBytes`.  It exists to:
//
//   - Give an early-fail signal if a future slot-allocator bug
//     (e.g. an off-by-one in `peak_slots`) starts producing
//     writes that *would have* spilled into wasi-libc.  The gate
//     in LayoutPass refuses to compile before the spill happens.
//   - Leave room for any forgotten data-segment we may want to
//     emit alongside rodata (currently none; future-proofing).
//
// Mirror of the runtime side.  The C macros in
// `runtime/cel_layout.h` (`CELWASM_RESERVED_LOW_MEMORY_BYTES`,
// `CELWASM_INITIAL_MEMORY_PAGES`, …) are the same numbers viewed
// from the C runtime build.  Drift between the two is checked by
// the `static_assert`s at the bottom of this header — any place
// that re-defines a constant has to update both at once or the
// next build fails.

#include <cstdint>

#include "runtime/cel_layout.h"

namespace celwasm {

// Constants the compiler reads to bound its layout decisions.
//
// All sizes are bytes unless suffixed `Pages`.  All offsets are
// linear-memory byte offsets from the start of `cel.memory`.
struct MemoryLayout {
  // The wasm spec page size.  Do not change.
  static constexpr uint32_t kWasmPageSize = 65536;

  // Initial linear-memory pages declared by `cel_runtime.wasm`.
  // The runtime grows beyond this via dlmalloc + `memory.grow` up
  // to `kMaxMemoryBytes` (a wasm32-wasi-threads link flag).  5 because
  // the reserved low region is 256 KiB (4 pages) and wasi-libc's static
  // data sits above it (see kReservedLowMemoryBytes).
  static constexpr uint32_t kInitialMemoryPages = 5;

  // Hard ceiling on linear-memory growth (`-Wl,--max-memory=` on
  // the wasi-threads toolchain).  The runtime may not grow past
  // this; we record it here so future allocators that span past
  // the reserved low region can be bounded against it without
  // re-typing the number.
  static constexpr uint32_t kMaxMemoryBytes = 64u * 1024u * 1024u;  // 64 MiB

  // First N bytes of `cel.memory` are reserved for the expr
  // module's active data segments (rodata + workspace).
  // `-Wl,--global-base=N` on the runtime build pins wasi-libc's
  // static data + stack + heap above this offset — anything the
  // expr module writes at or past this byte falls into wasi-libc.
  static constexpr uint32_t kReservedLowMemoryBytes = 262144;

  // Slack between the last workspace cell and
  // `kReservedLowMemoryBytes`.  Workspace bytes must not extend
  // into this band; the LayoutPass exhaustion gate enforces it.
  // Catches off-by-one regressions in the slot allocator before
  // they corrupt wasi-libc.
  static constexpr uint32_t kGuardBytes = 256;

  // First valid rodata offset.  Skips the two reserved
  // sentinel slots at `[0, 16)` (null sentinel + legacy arena
  // cursor slot).
  static constexpr uint32_t kRodataBaseMin = 16;

  // Workspace cell stride.  Must match
  // `SlotAllocator::kSlotStride`; the `static_assert` further down
  // in `compiler/codegen/slot_allocator.h` enforces it.
  static constexpr uint32_t kSlotStride = 32;

  // Max workspace bytes the compiler will admit for an expression
  // whose rodata occupies the closed interval `[rodata_base,
  // rodata_base + rodata_size)`.  Negative values (rodata alone
  // exceeds the reserved region) are clamped to zero so the
  // caller's `>` comparison trips reliably.
  static constexpr uint32_t MaxWorkspaceBytes(uint32_t rodata_base,
                                              uint32_t rodata_size) {
    const uint32_t used = rodata_base + rodata_size + kGuardBytes;
    return used >= kReservedLowMemoryBytes ? 0u
                                            : kReservedLowMemoryBytes - used;
  }
};

// Cross-check parity with `runtime/cel_layout.h`.  If a runtime-side
// macro and the C++ constant drift, the build fails here rather than
// silently shipping a corrupted layout.
static_assert(MemoryLayout::kWasmPageSize == CELWASM_WASM_PAGE_SIZE,
              "compiler MemoryLayout::kWasmPageSize must match runtime's "
              "CELWASM_WASM_PAGE_SIZE");
static_assert(MemoryLayout::kInitialMemoryPages == CELWASM_INITIAL_MEMORY_PAGES,
              "compiler MemoryLayout::kInitialMemoryPages must match runtime's "
              "CELWASM_INITIAL_MEMORY_PAGES");
static_assert(MemoryLayout::kReservedLowMemoryBytes ==
                  CELWASM_RESERVED_LOW_MEMORY_BYTES,
              "compiler MemoryLayout::kReservedLowMemoryBytes must match "
              "runtime's CELWASM_RESERVED_LOW_MEMORY_BYTES");
static_assert(MemoryLayout::kGuardBytes >= MemoryLayout::kSlotStride,
              "guard band must be at least one slot stride wide so a 1-slot "
              "overrun trips the gate");
static_assert(MemoryLayout::kReservedLowMemoryBytes >
                  MemoryLayout::kRodataBaseMin + MemoryLayout::kGuardBytes,
              "reserved low region must leave at least one workspace slot "
              "after rodata base + guard");

}  // namespace celwasm

#endif  // CELWASM_COMPILER_MEMORY_LAYOUT_H_
