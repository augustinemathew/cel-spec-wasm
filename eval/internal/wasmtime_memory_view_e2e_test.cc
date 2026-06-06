// True e2e regression for cleanup-backlog #36 — exercises the
// production `WasmtimeMemoryView` against a REAL
// `wasmtime_sharedmemory_t` (not the FakeMemoryView covered by
// `memory_view_bounds_test.cc`).  Confirms that the bounds-check
// guards added to `cel_host_wasmtime.h:ReadCelValue` /
// `ReadSpan` / `WriteCelValue` / `WriteU32` actually fire when
// called against the wasmtime-backed memory in production.
//
// Pre-fix (the audit case behind #36): a malicious or buggy
// wasm module passing a CelValue with `payload.s.ptr =
// 0xFFFFFFFF` through a `@host` trampoline would cause
// `WasmtimeMemoryView::ReadSpan(0xFFFFFFFF, N)` to dereference
// `Data() + 0xFFFFFFFF` and either SIGSEGV on the guard page
// or return a `string_view` over host memory adjacent to the
// wasm reservation.  Post-fix, the call returns an empty
// `string_view` — verified here against a 1-page shared memory.

#include <cstdint>
#include <cstring>
#include <string>

#include "absl/strings/string_view.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/cel_host_wasmtime.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// One wasm page = 64 KiB.  Smallest shared memory we can create.
constexpr uint32_t kPageBytes = 64u * 1024u;

struct SharedMemHarness {
  wasm_engine_t* engine = nullptr;
  wasm_memorytype_t* memtype = nullptr;
  wasmtime_sharedmemory_t* mem = nullptr;
  wasmtime_store_t* store = nullptr;

  ~SharedMemHarness() {
    if (mem != nullptr) wasmtime_sharedmemory_delete(mem);
    if (memtype != nullptr) wasm_memorytype_delete(memtype);
    if (store != nullptr) wasmtime_store_delete(store);
    if (engine != nullptr) wasm_engine_delete(engine);
  }
};

// Build a shared memory with `min_pages` initial size and a 1-page
// max so the bounds we test are tight.  Returns true on success.
// `wasm_memorytype_new_with_shared` is the wasmtime API for
// declaring a *shared* memory type — plain `wasm_memorytype_new`
// produces a non-shared type that `wasmtime_sharedmemory_new`
// rejects.  Build with threads enabled in the engine config.
std::string ConsumeError(wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasmtime_error_message(err, &msg);
  std::string out(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  return out;
}

bool BuildSharedMem(SharedMemHarness* h, uint32_t min_pages,
                    uint32_t max_pages, std::string* err_out = nullptr) {
  wasm_config_t* config = wasm_config_new();
  wasmtime_config_wasm_threads_set(config, true);
  wasmtime_config_shared_memory_set(config, true);
  h->engine = wasm_engine_new_with_config(config);
  if (h->engine == nullptr) {
    if (err_out) *err_out = "engine_new_with_config returned null";
    return false;
  }
  h->store = wasmtime_store_new(h->engine, nullptr, nullptr);
  if (h->store == nullptr) {
    if (err_out) *err_out = "store_new returned null";
    return false;
  }
  // Use the extended constructor that takes 64-bit min/max and a
  // `shared = true` flag.  Plain wasm_memorytype_new() builds an
  // unshared type which wasmtime_sharedmemory_new rejects.
  // `page_size_log2 = 16` selects the default 64 KiB page size.
  wasmtime_error_t* mt_err = wasmtime_memorytype_new(
      /*min=*/min_pages, /*max_present=*/true, /*max=*/max_pages,
      /*is_64=*/false, /*shared=*/true,
      /*page_size_log2=*/16, &h->memtype);
  if (mt_err != nullptr) {
    if (err_out) *err_out = "memorytype_new: " + ConsumeError(mt_err);
    return false;
  }
  if (h->memtype == nullptr) {
    if (err_out) *err_out = "memorytype is null";
    return false;
  }
  wasmtime_error_t* err =
      wasmtime_sharedmemory_new(h->engine, h->memtype, &h->mem);
  if (err != nullptr) {
    if (err_out) *err_out = "sharedmemory_new: " + ConsumeError(err);
    return false;
  }
  return true;
}

// ── ReadSpan ────────────────────────────────────────────────────

TEST(WasmtimeMemoryViewE2E, ReadSpanAdversarialMaxPtrReturnsEmpty) {
  SharedMemHarness h;
  std::string err;
  ASSERT_TRUE(BuildSharedMem(&h, 1, 1, &err)) << err;
  WasmtimeMemoryView view(wasmtime_store_context(h.store), h.mem);

  // The exact adversarial input that motivated #36.  Pre-fix this
  // dereferenced `Data() + 0xFFFFFFFF` and either segv'd or
  // exfiltrated host memory.
  EXPECT_EQ(view.ReadSpan(0xFFFFFFFFu, 1u), absl::string_view{});
  EXPECT_EQ(view.ReadSpan(0xFFFFFFFFu, 0x80000000u), absl::string_view{});
  // u32-wrap adversarial: ptr + len wraps to a small in-range
  // address.  Pre-fix the additive check `ptr + len > Size()`
  // would have been false; the subtraction form catches it.
  EXPECT_EQ(view.ReadSpan(0x80000000u, 0x80000000u), absl::string_view{});
}

TEST(WasmtimeMemoryViewE2E, ReadSpanPastSinglePageReturnsEmpty) {
  SharedMemHarness h;
  std::string err;
  ASSERT_TRUE(BuildSharedMem(&h, 1, 1, &err)) << err;
  WasmtimeMemoryView view(wasmtime_store_context(h.store), h.mem);

  EXPECT_EQ(view.Size(), kPageBytes);
  // Just past the end.
  EXPECT_EQ(view.ReadSpan(kPageBytes, 1u), absl::string_view{});
  EXPECT_EQ(view.ReadSpan(kPageBytes - 1, 2u), absl::string_view{});
  EXPECT_EQ(view.ReadSpan(kPageBytes + 100u, 50u), absl::string_view{});
}

TEST(WasmtimeMemoryViewE2E, ReadSpanInBoundsRoundTrips) {
  SharedMemHarness h;
  std::string err;
  ASSERT_TRUE(BuildSharedMem(&h, 1, 1, &err)) << err;
  WasmtimeMemoryView view(wasmtime_store_context(h.store), h.mem);

  // Seed memory directly via the shared memory's data pointer —
  // bypassing the host-side bound-checked Write (the seeding
  // step is part of the test setup, not the API under test).
  uint8_t* data = wasmtime_sharedmemory_data(h.mem);
  std::memset(data + 100, 'A', 16);
  absl::string_view sv = view.ReadSpan(100, 16);
  EXPECT_EQ(sv.size(), 16u);
  EXPECT_EQ(sv, "AAAAAAAAAAAAAAAA");
}

// ── ReadCelValue / WriteCelValue ────────────────────────────────

TEST(WasmtimeMemoryViewE2E, ReadCelValuePastEndReturnsZero) {
  SharedMemHarness h;
  std::string err;
  ASSERT_TRUE(BuildSharedMem(&h, 1, 1, &err)) << err;
  WasmtimeMemoryView view(wasmtime_store_context(h.store), h.mem);

  // Read 24 bytes starting 16 bytes before end of memory →
  // 8 bytes spill into OOB → zero CelValue.
  CelValue cv = view.ReadCelValue(kPageBytes - 16u);
  EXPECT_EQ(cv.kind, 0u);
  EXPECT_EQ(cv.payload.i, 0);
  // Way past.
  CelValue cv2 = view.ReadCelValue(0xFFFFFFFFu);
  EXPECT_EQ(cv2.kind, 0u);
}

TEST(WasmtimeMemoryViewE2E, WriteCelValuePastEndIsNoop) {
  SharedMemHarness h;
  std::string err;
  ASSERT_TRUE(BuildSharedMem(&h, 1, 1, &err)) << err;
  WasmtimeMemoryView view(wasmtime_store_context(h.store), h.mem);

  // Place a sentinel at the last in-bounds slot.
  CelValue sentinel{};
  sentinel.kind = 0xDEAD;
  sentinel.payload.i = 0x4242;
  view.WriteCelValue(kPageBytes - sizeof(CelValue), sentinel);
  // OOB writes must NOT change the sentinel.
  CelValue evil{};
  evil.kind = 0xBEEF;
  view.WriteCelValue(kPageBytes - 8u, evil);       // straddles boundary
  view.WriteCelValue(kPageBytes + 100u, evil);
  view.WriteCelValue(0xFFFFFFFFu, evil);

  CelValue got = view.ReadCelValue(kPageBytes - sizeof(CelValue));
  EXPECT_EQ(got.kind, 0xDEADu);
  EXPECT_EQ(got.payload.i, 0x4242);
}

// ── Size reports the true shared memory length ──────────────────

TEST(WasmtimeMemoryViewE2E, SizeReportsSharedMemoryByteLength) {
  SharedMemHarness h;
  // 2 pages.
  std::string err;
  ASSERT_TRUE(BuildSharedMem(&h, 2, 4, &err)) << err;
  WasmtimeMemoryView view(wasmtime_store_context(h.store), h.mem);
  EXPECT_EQ(view.Size(), 2u * kPageBytes);
}

}  // namespace
}  // namespace celwasm
