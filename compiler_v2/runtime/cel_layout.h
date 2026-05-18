// Single source of truth for runtime memory-layout constants.  Codegen
// (compiler_v2/codegen), host glue (compiler_v2/api), and the runtime
// itself all #include this header — drift between the three would
// otherwise cause silent miscompiles or out-of-bounds writes.
//
// See `doc/implementation-plan/wasi/DESIGN.md` §4 + §5 for the design
// rationale; the static_asserts in this file enforce the assumptions
// A5-A7 catalogued there.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_LAYOUT_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_LAYOUT_H_

#include <stdint.h>

// One wasm page = 64 KiB.  Spec constant; do not change.
#define CELWASM_WASM_PAGE_SIZE 65536u

// Initial linear memory size at Instance creation.  Two pages = 128 KiB
// matches today's pre-migration baseline.  Memory grows as needed via
// dlmalloc + memory.grow.
#define CELWASM_INITIAL_MEMORY_PAGES 2u

// First N bytes of linear memory are reserved for the expr module's
// active data segments (rodata + workspace).  Set via
// `-Wl,--global-base=N` on the runtime build so wasi-libc places its
// static data + stack + heap above this offset.  8 KiB is well above
// the typical expr rodata footprint; bump if a real expression needs
// more.
#define CELWASM_RESERVED_LOW_MEMORY_BYTES 8192u

// Default size of the per-Instance arena buffer that backs
// `arena_alloc`.  Malloc'd once per Instance via `arena_init`.
#define CELWASM_ARENA_CAPACITY_BYTES (64u * 1024u)

// Compile-time invariants (A5-A7 in DESIGN.md §5).
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
_Static_assert(CELWASM_RESERVED_LOW_MEMORY_BYTES <
               CELWASM_INITIAL_MEMORY_PAGES * CELWASM_WASM_PAGE_SIZE,
               "Reserved low region must fit inside initial memory");
_Static_assert((CELWASM_RESERVED_LOW_MEMORY_BYTES & 7u) == 0,
               "Reserved low region size must be 8-aligned");
_Static_assert(CELWASM_ARENA_CAPACITY_BYTES > 0u,
               "Arena capacity must be positive");
_Static_assert((CELWASM_ARENA_CAPACITY_BYTES &
                (CELWASM_ARENA_CAPACITY_BYTES - 1u)) == 0u,
               "Arena capacity should be power-of-2 (helps growth math)");
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_LAYOUT_H_
