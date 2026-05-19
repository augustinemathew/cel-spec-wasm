// Probe: does wasi-libc expose dlmalloc's mspace_* API?
//
// If yes, we can mass-free allocations with mspace_destroy().
// If no, we need to use malloc() + track allocations ourselves
// OR re-instantiate the wasm module per-eval (expensive).
//
// Build:
//   ./wasi-sdk/bin/clang --target=wasm32-wasi -Oz -nostartfiles \
//     -Wl,--no-entry -Wl,--export=run \
//     exp_b_mspace.c -o exp_b_mspace.wasm

#include <stdint.h>
#include <stdlib.h>

// dlmalloc's mspace API — declared here to test if the
// linker can find the symbols.
typedef void* mspace;
extern mspace create_mspace(size_t capacity, int locked);
extern mspace create_mspace_with_base(void* base, size_t capacity, int locked);
extern size_t destroy_mspace(mspace msp);
extern void*  mspace_malloc(mspace msp, size_t bytes);

// run(): tries to use mspace.  Returns 1 on success, 0 if a
// pointer is NULL.  If mspace_* aren't linked, the wasm
// won't link at all.
int32_t run(void) {
  mspace msp = create_mspace(64 * 1024, /*locked=*/0);
  if (!msp) return 0;
  void* p = mspace_malloc(msp, 128);
  if (!p) return 0;
  destroy_mspace(msp);
  return 1;
}
