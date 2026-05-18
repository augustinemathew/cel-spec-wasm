// Shared linear-memory backing for the CEL WASM runtime.
//
// Carved out of cel_runtime.c per
// `doc/implementation-plan/rewrite/cel-runtime-c-split-plan.md` (P2).
// This TU is the dependency hub — every other carved .c references
// `cel_memory_base_()` / `cel_memory_size_()` via `cel_internal.h`'s
// extern decls.
//
// IMPORTANT (wasm32): the inline-asm opacity barrier in
// `cel_memory_base_` is load-bearing.  Without it, clang's wasm32
// backend sees `(uint8_t*)0` as a C null pointer and treats every
// store through `cel_memory_base_() + off` as undefined behaviour —
// which it then elides entirely.  Disassembly without this fix shows
// `cel_reset` compiles to a no-op (no `i32.store` at byte 8 or 12)
// and `cel_alloc` compiles to `unreachable`.  The barrier prevents
// the optimizer from reasoning about the value-of-zero through the
// pointer cast; the runtime cost is one register copy.

#include "compiler_v2/runtime/cel_memory.h"

#include <stdint.h>

// External linkage is mandatory — cel_arena.c / cel_runtime.c / every
// carved TU references these accessors via cel_internal.h's extern
// decls.  Suppress the misc-use-internal-linkage lint.
#ifdef __wasm__
// wasm-ld provides `__heap_base` after linking; with `--import-memory`
// the base of memory is index 0, addressable directly.  We synthesize
// a byte pointer from offset 0 — see file header for the opacity-
// barrier rationale.
uint8_t* cel_memory_base_(void) {  // NOLINT(misc-use-internal-linkage)
  uintptr_t p = 0;
  __asm__("" : "+r"(p));
  return (uint8_t*)p;
}
uint32_t cel_memory_size_(void) {  // NOLINT(misc-use-internal-linkage)
  // The module imports a 1-page memory (64 KiB) at M1; later milestones
  // negotiate a size via the `cel.abi` section.  Returning a fixed
  // value here is fine — cel_alloc's bounds check uses `limit` from
  // the cursor slot, not this.
  return 64u * 1024u;
}
#else
#include "compiler_v2/runtime/cel_layout.h"
// Native test backing buffer must be at least the arena capacity plus
// a slack region for the [0, 16) reserved bytes the arena leaves
// before its own base.
#ifndef CELWASM_ARENA_BYTES
#define CELWASM_ARENA_BYTES (CELWASM_ARENA_CAPACITY_BYTES + 65536u)
#endif
// `_Alignas(8)` is load-bearing: every CelValue is 8-aligned (see
// `cel_memory_test::BaseIsEightByteAligned`), so the host-side backing
// buffer must be too — otherwise a CelValue* derived from
// `cel_memory_base_() + k * sizeof(CelValue)` is mis-aligned and the
// load/store traps on platforms that enforce alignment.  At -O2 the
// linker happened to pad this to 8; at -O3 + -flto it doesn't.
_Alignas(8) static uint8_t g_memory[CELWASM_ARENA_BYTES];
uint8_t* cel_memory_base_(void) {  // NOLINT(misc-use-internal-linkage)
  return g_memory;
}
uint32_t cel_memory_size_(void) {  // NOLINT(misc-use-internal-linkage)
  return (uint32_t)sizeof(g_memory);
}
#endif

uint8_t* cel_mem_base(void) {
  return cel_memory_base_();
}

uint32_t cel_mem_size(void) {
  return cel_memory_size_();
}
