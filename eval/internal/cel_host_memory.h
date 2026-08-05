// MemoryView + ArenaAllocator — the runtime-facing memory
// abstractions the cel_host trampolines are driven by.  Split out of
// the aggregator `cel_host.h` so the leaf error/absorber TU
// (`cel_host_error`) can name `MemoryView` without depending on the
// backing classes or the trampoline entry points.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_MEMORY_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_MEMORY_H_

#include <cstddef>
#include <cstdint>

#include "absl/base/nullability.h"
#include "absl/strings/string_view.h"
#include "runtime/cel_data.h"

namespace celwasm {

// Abstract 24-byte CelValue + span read/write over the expr module's
// linear memory.  Production impl wraps `wasmtime_memory_t`; tests use
// a vector-backed fake.
class MemoryView {
 public:
  virtual ~MemoryView() = default;

  MemoryView() = default;
  MemoryView(const MemoryView&) = delete;
  MemoryView& operator=(const MemoryView&) = delete;

  // Total bytes of the underlying linear memory.  Used by the
  // default `IsInBounds` helper below and by callers that need to
  // bounds-check `ptr + len` against memory size before iterating
  // (e.g. when walking an attacker-controlled length field from a
  // CelValue payload).
  //
  // An implementation over GROWABLE memory may return a cached
  // snapshot that lags the true size while the memory grows mid-Eval
  // (`arena_alloc` → dlmalloc → memory.grow).  The snapshot is
  // monotonic-safe — it only ever UNDER-approximates — and
  // `IsInBounds` is the authoritative bounds predicate: it refreshes
  // the snapshot before rejecting (see `WasmtimeMemoryView`).
  virtual uint32_t Size() const = 0;

  // True iff `[ptr, ptr+len)` lies entirely inside the linear
  // memory, i.e. the read/write of `len` bytes at `ptr` is safe.
  // Empty ranges (`len == 0`) are always in-bounds — they perform no
  // memory access.  Subtraction is rearranged
  // (`len <= Size() - ptr`) to avoid overflow when computing
  // `ptr + len` on the u32s.
  //
  // Virtual so implementations over growable memory can refresh a
  // cached size snapshot before rejecting: a probe that fails
  // against a stale snapshot of a memory that has since grown MUST
  // re-fetch and re-test, never falsely reject an offset the grow
  // made valid (a false reject corrupts results — e.g. a map-lookup
  // key span in a freshly grown arena page decoding as empty).
  // Pinned by `ViewConstructedBeforeGrowAcceptsGrownPages`
  // (wasmtime_memory_view_e2e_test.cc) and
  // `MemoryGrowStabilityTest` (memory_grow_stability_test.cc).
  virtual bool IsInBounds(uint32_t ptr, uint32_t len) const {
    if (len == 0) return true;
    const uint32_t size = Size();
    return ptr <= size && len <= size - ptr;
  }

  // Read/write methods MUST bounds-check `offset + sizeof(...)` (or
  // `ptr + len`) against `Size()` before touching memory.  On OOB:
  //
  //   - `ReadCelValue` returns a zeroed `CelValue` (kind == 0,
  //     payload == 0) — observable as a kNull on the host side, so
  //     the trampoline propagates a defined-but-empty value rather
  //     than reading host memory adjacent to the wasm reservation.
  //   - `ReadSpan` returns an empty `absl::string_view` — the
  //     caller sees a zero-length string/bytes, never a span
  //     pointing past the wasm sandbox.
  //   - `WriteCelValue` / `WriteU32` are no-ops on OOB — the
  //     write is silently dropped; subsequent reads observe the
  //     prior memory state.  No partial writes.
  //
  // This is the security-relevant contract: a malicious or buggy
  // wasm module that passes out-of-bounds `ptr`/`offset` values
  // through a host trampoline CANNOT leak host memory or corrupt
  // host state via these methods.  The eval that produced the
  // bad value still proceeds — the value it ultimately yields is
  // observably wrong (typically kNull or an empty container), so
  // a downstream assertion in well-written CEL catches it.  See
  // `cleanup-backlog #36` for the audit that closed this gap.
  virtual CelValue ReadCelValue(uint32_t offset) const = 0;
  virtual void WriteCelValue(uint32_t offset, const CelValue& v) = 0;
  virtual absl::string_view ReadSpan(uint32_t ptr, uint32_t len) const = 0;

  // Raw u32 write at `offset`.  Used by aggregate-iter trampolines
  // that populate runtime-side state structs (e.g. `MapIterState`)
  // whose 4-byte fields don't fit the 24-byte CelValue shape.
  virtual void WriteU32(uint32_t offset, uint32_t value) = 0;
};

// Bump allocator for string/bytes payloads.  `out_offset` receives
// the wasm-side offset the CelSpan carries.  Zero-byte alloc returns
// a valid pointer; OOM returns nullptr.
class ArenaAllocator {
 public:
  virtual ~ArenaAllocator() = default;

  ArenaAllocator() = default;
  ArenaAllocator(const ArenaAllocator&) = delete;
  ArenaAllocator& operator=(const ArenaAllocator&) = delete;

  virtual uint8_t* absl_nullable Alloc(size_t len,
                                       uint32_t* absl_nonnull out_offset) = 0;
};

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_MEMORY_H_
