// Probe: hand-rolled bump arena backed by ONE malloc.  Per-eval
// mass-reset (free + re-alloc OR cursor=0).  Coexists with global
// malloc for library calls.
//
// This is what the migration would actually ship — bump-arena
// semantics on top of wasi-libc's malloc, replacing today's
// fixed-offset cursor + memory-bytes-8/12 trick.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Arena {
  uint8_t* base;
  size_t   capacity;
  size_t   cursor;
} Arena;

// Plan-lifetime: one arena per Instance, lives across evals.
static Arena g_arena = {0};

void arena_init(int32_t cap_bytes) {
  if (g_arena.base) free(g_arena.base);
  g_arena.base = (uint8_t*)malloc((size_t)cap_bytes);
  g_arena.capacity = (size_t)cap_bytes;
  g_arena.cursor = 0;
}

int32_t arena_alloc(int32_t n) {
  size_t aligned = (size_t)((n + 7) & ~7);  // 8-aligned
  if (g_arena.cursor + aligned > g_arena.capacity) return 0;
  uint8_t* p = g_arena.base + g_arena.cursor;
  g_arena.cursor += aligned;
  memset(p, 0, aligned);
  return (int32_t)(uintptr_t)p;
}

// Per-eval reset.  Just bump cursor back to 0.  The arena buffer
// stays malloc'd; reused across evals.  This is the "free" reset.
void arena_reset(void) {
  g_arena.cursor = 0;
}

int32_t arena_cursor(void) { return (int32_t)g_arena.cursor; }
int32_t arena_capacity(void) { return (int32_t)g_arena.capacity; }
int32_t arena_base(void) { return (int32_t)(uintptr_t)g_arena.base; }
