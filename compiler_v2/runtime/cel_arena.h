// Bump-allocator over the shared linear memory.
//
// Lifecycle.  Codegen emits `cel_reset(arena_base, arena_limit)` as the
// first instructions of each generated `eval()`; both arguments are
// compile-time constants (end of rodata, end of initial memory) baked
// in via `i32.const`, so there is no host-side init phase — the host
// just instantiates and calls `eval()`.  `cel_reset` writes the pair
// into the fixed cursor slot at bytes 8/12, giving each eval a fresh
// arena.  `cel_alloc(n)` bumps the cursor by `align_up(n, 8)` and
// returns the pre-bump offset (or 0 if out of space).
//
// M1 note: pure literal eval never calls either helper — every value
// lives in `.rodata` — but both are exported so the memory model is
// observable from host code and testable independently of codegen.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_ARENA_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_ARENA_H_

#include <stdint.h>

#include "compiler_v2/runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

void cel_reset(uint32_t arena_base, uint32_t arena_limit);
uint32_t cel_alloc(uint32_t n);

// Offset → CelValue* helper.  Returns NULL when `off == 0` so callers
// can treat a zero offset uniformly as "absent".  Invalidated by
// `cel_reset`.
CelValue* cel_value_at(uint32_t off);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_ARENA_H_
