// Bump arena over a single `malloc()`-backed buffer.
//
// Lifecycle (`doc/implementation-plan/wasi/DESIGN.md` §4 + §6):
//
//   1. Per Instance, the host calls `arena_init(N)` once via wasm
//      reentry.  `arena_init` mallocs N bytes from the dlmalloc heap
//      and stores the base offset + capacity in a static `Arena`
//      struct (in BSS).  Default N is `CELWASM_ARENA_CAPACITY_BYTES`
//      (64 KiB).
//   2. Per Eval, codegen emits `(call $arena_reset)` as the first
//      instruction of `$eval`.  Cursor = 0; allocations made by the
//      previous Eval are effectively freed (they aren't `free()`d,
//      but they're no longer reachable and the bytes are reused).
//   3. Within `$eval`, kernels call `arena_alloc(n)` for any
//      per-Eval working storage.  Returns an offset into linear
//      memory (or 0 on OOM).  Allocations are 8-byte aligned and
//      zero-initialized.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_ARENA_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_ARENA_H_

#include <stdint.h>

#include "compiler_v2/runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-time per-Instance setup.  Caller picks capacity; default is
// `CELWASM_ARENA_CAPACITY_BYTES` from cel_layout.h.  Idempotent only
// if called with the same `cap_bytes`; calling with a different value
// after the first call traps (A16 in DESIGN.md §5).
void arena_init(uint32_t cap_bytes);

// Bump-allocate `n` bytes from the arena.  Returns the absolute
// linear-memory offset of the allocated region, or 0 on OOM.
// Allocations are rounded up to 8 bytes and zero-initialized.
uint32_t arena_alloc(uint32_t n);

// Reset the cursor to the start of the arena.  Called from the
// `$eval` prologue as `(call $arena_reset)`.  O(1); does not free
// the underlying buffer.
void arena_reset(void);

// Diagnostics — current bump cursor and total capacity in bytes.
uint32_t arena_cursor(void);
uint32_t arena_capacity(void);

// Offset → CelValue* helper.  Returns NULL when `off == 0` so callers
// can treat a zero offset uniformly as "absent".
CelValue* cel_value_at(uint32_t off);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_ARENA_H_
