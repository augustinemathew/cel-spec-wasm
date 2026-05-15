// Bump-allocator over the shared linear memory.
//
// Carved out of cel_runtime.c per
// `doc/implementation-plan/rewrite/cel-runtime-c-split-plan.md` (P3).
// Depends only on cel_memory.c (via cel_internal.h's extern decls).
//
// See cel_arena.h for the public ABI and the design rationale
// (lifecycle: codegen emits cel_reset(arena_base, arena_limit) as the
// first instructions of each generated eval(); both args are
// compile-time constants).

#include "compiler_v2/runtime/cel_arena.h"

#include <stdint.h>

#include "compiler_v2/runtime/cel_internal.h"
#include "compiler_v2/runtime/cel_log.h"

// Arena cursor offsets in the shared memory (parent §8.2).
enum {
  kBumpOffset = 8u,
  kLimitOffset = 12u,
};

// NB: use an aligned pointer-cast load/store rather than memcpy.
// clang's wasm32 backend lowered `memcpy(dst, &v, 4)` as three
// byte-stores (to the high 3 bytes only) which left the low byte of
// `arena_base` / `arena_limit` at bytes 8 / 12 untouched — the arena
// cursor ended up wrong by the LSB of the address on every reset.
// An explicit `*(uint32_t*)p = v` compiles to a single `i32.store`
// that actually writes all four bytes.
static uint32_t load_u32(uint32_t off) {
  return *(const uint32_t*)(cel_memory_base_() + off);
}

static void store_u32(uint32_t off, uint32_t v) {
  *(uint32_t*)(cel_memory_base_() + off) = v;
}

static uint32_t align_up(uint32_t n, uint32_t align) {
  return (n + (align - 1u)) & ~(align - 1u);
}

void cel_reset(uint32_t arena_base, uint32_t arena_limit) {
  CEL_LOG("enter");
  store_u32(kBumpOffset, arena_base);
  store_u32(kLimitOffset, arena_limit);
}

uint32_t cel_alloc(uint32_t n) {
  CEL_LOG("enter");
  uint32_t need = align_up(n, 8u);
  if (need == 0) need = 8u;
  uint32_t bump = load_u32(kBumpOffset);
  uint32_t limit = load_u32(kLimitOffset);
  if (bump + need > limit) return 0;
  store_u32(kBumpOffset, bump + need);
  memset(cel_memory_base_() + bump, 0, need);
  return bump;
}

CelValue* cel_value_at(uint32_t off) {
  CEL_LOG("enter");
  if (off == 0) return (CelValue*)0;
  return cv_at(off);
}
