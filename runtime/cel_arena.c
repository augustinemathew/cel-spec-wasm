// Bump arena over a chained list of malloc()-backed buffers.  See
// cel_arena.h for the public ABI and lifecycle.  Replaces the pre-
// WASI bump arena that lived at fixed memory bytes 8/12 with a
// malloc'd backing buffer in the dlmalloc heap region.
//
// The arena struct lives in BSS (zero-initialized at module
// instantiation).  Per Instance, `arena_init` seeds the FIRST
// chunk with the embedder-chosen `cap_bytes`; later allocs that
// would overflow the current chunk malloc a NEW chunk (twice the
// previous chunk's size, capped at 1 MiB per chunk) and continue
// bumping there.  Each `arena_reset()` frees every extra chunk
// and rewinds the first chunk's cursor — the first chunk is
// kept across resets to avoid per-Eval malloc churn for the
// common case where the embedder's initial sizing is sufficient.
// Cleanup-backlog #34 was the PBT discovery that drove the move
// to a chained model; prior fixed-cap arena reliably hit OOM at
// depth-7 nested comp × `_in_` on humble-size grammar inputs.

#include "runtime/cel_arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/cel_internal.h"
#include "runtime/cel_layout.h"
#include "runtime/cel_log.h"

// One bump chunk in the chain.  `base` is the malloc'd buffer's
// wasm-memory offset; `cursor` is the next free byte relative to
// `base`; `capacity` is the chunk's size in bytes.  `next` is the
// younger chunk in the chain (or NULL).
typedef struct CelArenaChunk {
  uint8_t* base;
  uint32_t capacity;
  uint32_t cursor;
  struct CelArenaChunk* next;
} CelArenaChunk;

typedef struct {
  CelArenaChunk* head;   // first chunk; kept across resets
  CelArenaChunk* tail;   // current chunk allocations go into
  uint32_t total_used;   // sum of cursors across all chunks
  uint32_t total_cap;    // sum of capacities across all chunks
  uint32_t initialized;  // 0 or 1
} CelArena;

static CelArena g_arena = {0, 0, 0, 0, 0};

// Emergency poison block (see cel_arena.h).  Reserved once per
// Instance at arena_init, OUTSIDE the resettable chunk chain, so it
// survives arena_reset and stays addressable when arena_alloc OOMs.
static uint32_t g_oom_block = 0;

// Minimum follow-on chunk size — keeps malloc count bounded for
// tiny embedders without over-allocating large chunks up front.
#define CEL_ARENA_MIN_GROW_BYTES 4096u
// Per-chunk cap — large enough to absorb the worst grammar-found
// nested-comp × `_in_` patterns from cleanup-backlog #34 in a
// handful of chunks; small enough that a runaway eval doesn't
// silently consume megabytes per malloc.
#define CEL_ARENA_MAX_GROW_BYTES (1u << 20)  // 1 MiB

static CelArenaChunk* alloc_chunk(uint32_t want_bytes) {
  CelArenaChunk* c = (CelArenaChunk*)malloc(sizeof(CelArenaChunk));
  if (c == NULL) return NULL;
  void* p = malloc((size_t)want_bytes);
  if (p == NULL) {
    free(c);
    return NULL;
  }
  c->base = (uint8_t*)p;
  c->capacity = want_bytes;
  c->cursor = 0;
  c->next = NULL;
  return c;
}

static void free_chunk(CelArenaChunk* c) {
  if (c == NULL) return;
  free(c->base);
  free(c);
}

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
    if (g_arena.head != NULL && g_arena.head->capacity != cap_bytes) {
      __builtin_trap();  // double-init with different first-chunk size
    }
    return;
  }

#ifdef __wasm__
  // wasm build: arena buffer comes from dlmalloc.  The returned
  // pointer is an offset in the same linear memory the host reads
  // via wasmtime_memory_data, so `cel_mem_base() + arena_alloc(n)`
  // resolves correctly on both sides of the wasm/host boundary.
  CelArenaChunk* c = alloc_chunk(cap_bytes);
  if (c == NULL) return;
  g_arena.head = c;
  g_arena.tail = c;
  g_arena.total_cap = cap_bytes;
  // Emergency poison block: a separate dlmalloc allocation whose
  // absolute offset works exactly like an arena_alloc return value.
  // Reserved here — before any per-Eval allocation can exhaust
  // memory — so OOM paths can still vend a poisoned value.
  void* oom = malloc((size_t)CELWASM_ARENA_OOM_BLOCK_BYTES);
  g_oom_block = oom != NULL ? (uint32_t)(uintptr_t)oom : 0u;
  if (oom != NULL) memset(oom, 0, (size_t)CELWASM_ARENA_OOM_BLOCK_BYTES);
#else
  // native build: there is no shared linear memory.  Tests address
  // CelValues via `cel_mem_base() + offset`, so the arena MUST be
  // backed by the same buffer cel_mem_base() returns.  Bytes [0, 16)
  // stay reserved (null sentinel + the legacy cursor slot bytes
  // 8/12); the emergency poison block takes the next
  // CELWASM_ARENA_OOM_BLOCK_BYTES; the arena proper starts after it.
  // The native build does NOT chain — there's no real linear memory
  // to grow into, and tests use bounded fixtures.
  const uint32_t kNativeBlockOff = 16u;
  const uint32_t kNativeArenaOff = 16u + CELWASM_ARENA_OOM_BLOCK_BYTES;
  if (kNativeArenaOff + cap_bytes > cel_memory_size_()) {
    return;  // can't fit; leave g_arena zero so alloc returns 0
  }
  CelArenaChunk* c = (CelArenaChunk*)malloc(sizeof(CelArenaChunk));
  if (c == NULL) return;
  g_oom_block = kNativeBlockOff;
  c->base = cel_memory_base_() + kNativeArenaOff;
  c->capacity = cap_bytes;
  c->cursor = 0;
  c->next = NULL;
  g_arena.head = c;
  g_arena.tail = c;
  g_arena.total_cap = cap_bytes;
#endif
  g_arena.total_used = 0;
  g_arena.initialized = 1;
}

// Pick the next chunk's size: 2 × previous, clamped to
// [CEL_ARENA_MIN_GROW_BYTES, CEL_ARENA_MAX_GROW_BYTES], and at
// least `at_least_bytes` (the immediate allocation that triggered
// the grow — a single huge alloc gets a chunk sized to fit it,
// up to the per-chunk cap).
static uint32_t pick_grow_size(uint32_t prev_capacity,
                               uint32_t at_least_bytes) {
  uint32_t want = prev_capacity * 2u;
  if (want < CEL_ARENA_MIN_GROW_BYTES) want = CEL_ARENA_MIN_GROW_BYTES;
  if (want > CEL_ARENA_MAX_GROW_BYTES) want = CEL_ARENA_MAX_GROW_BYTES;
  if (at_least_bytes > want) want = at_least_bytes;
  return want;
}

uint32_t arena_alloc(uint32_t n) {
  CEL_LOG("enter");
  // align_up_8 wraps to 0 for n in (UINT32_MAX-7, UINT32_MAX], which
  // would silently turn a near-4GiB request into an 8-byte slot.
  // Reject the unalignable tail outright — it can never be satisfied.
  if (n > UINT32_MAX - 7u) return 0;
  uint32_t need = align_up_8(n);
  if (need == 0) need = 8u;  // A9: alloc(0) returns a valid 8-byte slot
  // A16 ('init exactly once per Instance') corollary: arena_alloc
  // must NOT be called before arena_init.  Silent zero return
  // misdiagnoses downstream as CEL_ERR_OVERFLOW; trap so the
  // backtrace names the call site (CLAUDE.md "Unimplemented features"
  // rule for any code path that shouldn't be reachable).
  if (!g_arena.initialized) __builtin_trap();

  // Tail-bump fast path: if the current chunk has room, allocate
  // there.  Subtraction-form bounds check (cursor <= capacity is
  // invariant, so capacity - cursor never wraps).
  CelArenaChunk* tail = g_arena.tail;
  if (need > tail->capacity - tail->cursor) {
    // Grow.  Native build does NOT chain — there's no shared
    // memory to extend into — so OOM is terminal.
#ifdef __wasm__
    uint32_t grow_bytes = pick_grow_size(tail->capacity, need);
    CelArenaChunk* nc = alloc_chunk(grow_bytes);
    if (nc == NULL) return 0;  // malloc OOM bubbles up
    tail->next = nc;
    g_arena.tail = nc;
    g_arena.total_cap += grow_bytes;
    tail = nc;
#else
    return 0;
#endif
  }
  uint32_t local_off = tail->cursor;
  tail->cursor += need;
  g_arena.total_used += need;
  uint8_t* p = tail->base + local_off;
  memset(p, 0, need);

  // Return value contract: caller expects `cel_mem_base() + ret` to
  // be the allocated bytes.
#ifdef __wasm__
  // Linear memory: malloc'd base IS an absolute offset in the shared
  // memory.  cel_mem_base() resolves to 0 (the base of the wasm
  // linear memory), so the return value must be the absolute offset.
  return (uint32_t)(uintptr_t)p;
#else
  // Native build: base = `g_memory + 16 + OOM block` (see arena_init).
  // cel_mem_base() returns `&g_memory[0]`, so the return value must
  // mirror that base offset to satisfy the contract.
  return 16u + CELWASM_ARENA_OOM_BLOCK_BYTES + local_off;
#endif
}

uint32_t arena_oom_block(void) {
  return g_oom_block;
}

void arena_reset(void) {
  CEL_LOG("enter");
  // O(extra-chunks) reset: drop every chunk except the first to
  // avoid per-Eval malloc churn for the common case where the
  // first chunk's capacity is sufficient.  If a previous Eval
  // grew the chain, those chunks get freed here so the next Eval
  // doesn't carry their bytes forward.  If arena_init hasn't been
  // called yet, the arena is empty so reset is a no-op.
  if (g_arena.head == NULL) return;
  CelArenaChunk* c = g_arena.head->next;
  while (c != NULL) {
    CelArenaChunk* nx = c->next;
    free_chunk(c);
    c = nx;
  }
  g_arena.head->cursor = 0;
  g_arena.head->next = NULL;
  g_arena.tail = g_arena.head;
  g_arena.total_used = 0;
  g_arena.total_cap = g_arena.head->capacity;
}

uint32_t arena_cursor(void) {
  // Backwards-compat: report the FIRST chunk's cursor for embedders
  // that snapshot+restore the cursor for nested-alloc patterns.
  // Chained chunks change this semantic — but the only in-tree
  // caller is diagnostics, so cursor of the first chunk is what
  // matters.  Total bytes allocated is `g_arena.total_used`.
  return g_arena.head != NULL ? g_arena.head->cursor : 0u;
}

uint32_t arena_capacity(void) {
  // Returns total capacity across the chain (initial chunk + any
  // grown chunks).  Embedders inspecting this for budgeting should
  // re-read after every Eval since growth is dynamic.
  return g_arena.total_cap;
}

CelValue* cel_value_at(uint32_t off) {
  CEL_LOG("enter");
  if (off == 0) return (CelValue*)0;
  return cv_at(off);
}
