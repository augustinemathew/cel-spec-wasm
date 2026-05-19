// Probe: plain malloc/free works, can be reentered from outside,
// and the heap can be wiped+reset via memory.fill from the host.
//
// Build:
//   ./wasi-sdk/bin/clang --target=wasm32-wasi -Oz -nostartfiles \
//     -Wl,--no-entry -Wl,--export=alloc -Wl,--export=free_p \
//     -Wl,--export=alloc_count -Wl,--export=reset_counter \
//     -Wl,--export=__heap_base -Wl,--export=memory \
//     exp_c_malloc.c -o exp_c_malloc.wasm

#include <stdint.h>
#include <stdlib.h>

// Counts how many allocations happened.  Lives in BSS (below
// __heap_base) so memory.fill on the heap region doesn't wipe it.
static int32_t g_alloc_count = 0;

int32_t alloc(int32_t n) {
  void* p = malloc((size_t)n);
  if (p) g_alloc_count++;
  return (int32_t)(uintptr_t)p;
}

void free_p(int32_t off) {
  free((void*)(uintptr_t)off);
}

int32_t alloc_count(void) {
  return g_alloc_count;
}

void reset_counter(void) {
  g_alloc_count = 0;
}
