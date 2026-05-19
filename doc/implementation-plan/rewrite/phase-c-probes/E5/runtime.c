// Probe E5 — stub C portion of a C+C++ wasm module.  Mirrors the
// cel_runtime arena shape but trimmed to the minimum: two functions
// the C++ side can call (arena_alloc, arena_reset).  Real runtime
// uses cel_arena.c; for the probe we want to prove C-only and C++-
// only TUs link together.
//
// The arena buffer is a bss array; for E5 we don't care about
// realloc / cross-eval lifetimes, just that the C side gives the
// C++ side a contiguous region.

#include <stddef.h>
#include <stdint.h>

#define ARENA_SIZE (64 * 1024)

static uint8_t arena_buf[ARENA_SIZE];
static size_t arena_cursor = 0;

__attribute__((export_name("arena_reset")))
void arena_reset(void) { arena_cursor = 0; }

__attribute__((export_name("arena_alloc")))
void* arena_alloc(size_t n) {
  // 8-byte align.
  arena_cursor = (arena_cursor + 7) & ~(size_t)7;
  if (arena_cursor + n > ARENA_SIZE) return (void*)0;
  void* p = &arena_buf[arena_cursor];
  arena_cursor += n;
  return p;
}
