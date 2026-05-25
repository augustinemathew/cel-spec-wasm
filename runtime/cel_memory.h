// Shared linear-memory accessors.  The expr module defines and exports
// `(memory $mem 1)`; the runtime module imports it.  These helpers
// return the base / size of that shared memory so host code (and tests)
// can stage bytes into it before kicking off an eval.
//
// Arena state lives at fixed offsets inside the shared memory (parent
// design doc §8.2):
//
//   [0..8)    reserved sentinel (offset 0 = "absent" everywhere)
//   [8..12)   u32  bump  (next free byte; the arena cursor)
//   [12..16)  u32  limit (end-of-arena byte)
//   [16..)    .rodata (expr module's active data segment) followed by
//             the bump arena.
//
// On the native-host build the shared memory is backed by a static
// byte buffer so native tests exercise the exact same layout the wasm
// build uses.  Callable before any `arena_reset` — `cel_mem_base` in
// particular is how unit tests stage rodata bytes before eval.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_MEMORY_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_MEMORY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t* cel_mem_base(void);
uint32_t cel_mem_size(void);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_MEMORY_H_
