// Bump arena over a single malloc()-backed buffer.  See cel_arena.h
// for the public ABI and lifecycle.  Replaces the pre-WASI bump arena
// that lived at fixed memory bytes 8/12 with a malloc'd backing buffer
// in the dlmalloc heap region.
//
// The arena struct lives in BSS (zero-initialized at module
// instantiation).  Per Instance, `arena_init` runs once via host
// reentry and seeds it.

#include "compiler_v2/runtime/cel_arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_v2/runtime/cel_internal.h"
#include "compiler_v2/runtime/cel_layout.h"
#include "compiler_v2/runtime/cel_log.h"

typedef struct {
  uint8_t* base;         // malloc'd buffer base in linear memory
  uint32_t capacity;     // total bytes
  uint32_t cursor;       // next free byte (relative to base)
  uint32_t initialized;  // 0 or 1
} CelArena;

static CelArena g_arena = {0, 0, 0, 0};

// Round up to the next multiple of 8.  Allocations are 8-aligned so a
// CelValue* derived from `base + arena_alloc(...)` always has
// alignment 8 (matches `_Alignas(8)` on `g_memory[]` in cel_memory.c
// for the native build).
static uint32_t align_up_8(uint32_t n) {
  return (n + 7u) & ~7u;
}

void arena_init(uint32_t cap_bytes) {
  CEL_LOG("enter");
  // A16: arena_init called exactly once per Instance.  Re-init with a
  // different cap_bytes is a programmer error — the caller (host)
  // owns the lifecycle.  We trap rather than silently leak the prior
  // buffer or corrupt the cursor.
  if (g_arena.initialized) {
    if (g_arena.capacity != cap_bytes) {
      __builtin_trap();  // double-init with different size
    }
    return;
  }

#ifdef __wasm__
  // wasm build: arena buffer comes from dlmalloc.  The returned
  // pointer is an offset in the same linear memory the host reads
  // via wasmtime_memory_data, so `cel_mem_base() + arena_alloc(n)`
  // resolves correctly on both sides of the wasm/host boundary.
  void* p = malloc((size_t)cap_bytes);
  if (p == NULL) return;
  g_arena.base = (uint8_t*)p;
#else
  // native build: there is no shared linear memory.  Tests address
  // CelValues via `cel_mem_base() + offset`, so the arena MUST be
  // backed by the same buffer cel_mem_base() returns.  Use the
  // [16, cap_bytes+16) slice of g_memory[] (offset 16 leaves the
  // null sentinel + the legacy cursor slot bytes 8/12 untouched, in
  // case anything still pokes there during the migration).
  if (16u + cap_bytes > cel_memory_size_()) {
    return;  // can't fit; leave g_arena zero so alloc returns 0
  }
  g_arena.base = cel_memory_base_() + 16u;
#endif
  g_arena.capacity = cap_bytes;
  g_arena.cursor = 0;
  g_arena.initialized = 1;
}

uint32_t arena_alloc(uint32_t n) {
  CEL_LOG("enter");
  uint32_t need = align_up_8(n);
  if (need == 0) need = 8u;  // A9: alloc(0) returns a valid 8-byte slot
  // A16 ('init exactly once per Instance') corollary: arena_alloc
  // must NOT be called before arena_init.  Silent zero return
  // misdiagnoses downstream as CEL_ERR_OVERFLOW; trap so the
  // backtrace names the call site (CLAUDE.md "Unimplemented features"
  // rule for any code path that shouldn't be reachable).
  if (!g_arena.initialized) __builtin_trap();
  if (g_arena.cursor + need > g_arena.capacity) return 0;  // A10: OOM → 0
  uint32_t local_off = g_arena.cursor;
  g_arena.cursor += need;
  uint8_t* p = g_arena.base + local_off;
  memset(p, 0, need);

  // Return value contract: caller expects `cel_mem_base() + ret` to
  // be the allocated bytes.
#ifdef __wasm__
  // Linear memory: malloc'd base IS an absolute offset in the shared
  // memory.  cel_mem_base() resolves to 0 (the base of the wasm
  // linear memory), so the return value must be the absolute offset.
  return (uint32_t)(uintptr_t)p;
#else
  // Native build: base = `g_memory + 16` (see arena_init).
  // cel_mem_base() returns `&g_memory[0]`, so the return value must be
  // `16 + local_off` to satisfy the contract.
  return 16u + local_off;
#endif
}

void arena_reset(void) {
  CEL_LOG("enter");
  // O(1) reset.  If arena_init hasn't been called yet, the arena is
  // empty so reset is a no-op.
  g_arena.cursor = 0;
}

uint32_t arena_cursor(void) {
  return g_arena.cursor;
}

uint32_t arena_capacity(void) {
  return g_arena.capacity;
}

// ---- Codegen-prologue compat shim (delete in M5) --------------------
//
// Codegen still emits `(call $cel_reset (i32.const arena_base)
// (i32.const arena_limit))` at the top of $eval; arena_base and
// arena_limit are no longer meaningful (they were offsets into a
// fixed-position cursor at memory bytes 8/12; the bump cursor now
// lives in BSS, not linear memory).  The shim ignores the args.
//
// M5 will swap codegen to `(call $arena_reset)` and delete this shim
// + its corresponding declaration in cel_arena.h.

void cel_reset(uint32_t arena_base, uint32_t arena_limit) {
  (void)arena_base;
  (void)arena_limit;
  // Auto-init on first call so tests written against the old
  // `cel_reset(base, limit)` ABI keep working without explicit
  // `arena_init`.  Deletes alongside the shim in M5.
  if (!g_arena.initialized) {
    arena_init(CELWASM_ARENA_CAPACITY_BYTES);
  }
  arena_reset();
}

CelValue* cel_value_at(uint32_t off) {
  CEL_LOG("enter");
  if (off == 0) return (CelValue*)0;
  return cv_at(off);
}
