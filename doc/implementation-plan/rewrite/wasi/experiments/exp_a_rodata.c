// Probe: where does rodata live in a wasi-sdk module?
//
// Build:
//   ./wasi-sdk/bin/clang --target=wasm32-wasi -Oz -nostartfiles \
//     -Wl,--no-entry -Wl,--export=get_a -Wl,--export=get_b \
//     -Wl,--export=stack_ptr -Wl,--export=heap_base \
//     exp_a_rodata.c -o exp_a_rodata.wasm
//
// We're trying to answer:
//  1. What offset does the linker pick for a const string?
//  2. What's __heap_base (the start of dlmalloc's territory)?
//  3. Can we know __data_end at compile-time?

#include <stdint.h>

// A small const string in rodata.
static const char kAlpha[] = "alpha";
static const char kBeta[]  = "beta-beta-beta";

// A larger zero-initialised global — these typically land in BSS,
// not active-data-segment-rodata.
static char g_buffer[4096];

// Linker symbols — wasm-ld exposes __heap_base + __data_end
// as defined-at-link-time when the C source declares them extern.
extern uint8_t __heap_base[];
extern uint8_t __data_end[];

// Probes the host calls.

int32_t addr_of_alpha(void) { return (int32_t)(uintptr_t)kAlpha; }
int32_t addr_of_beta(void)  { return (int32_t)(uintptr_t)kBeta; }
int32_t addr_of_buffer(void){ return (int32_t)(uintptr_t)g_buffer; }
int32_t addr_of_heap_base(void) { return (int32_t)(uintptr_t)__heap_base; }
int32_t addr_of_data_end(void)  { return (int32_t)(uintptr_t)__data_end; }

// Touch the buffer so the linker doesn't gc-section it.
int32_t buffer_byte(int32_t i) { return g_buffer[i]; }
