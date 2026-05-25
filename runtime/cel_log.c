// cel_log trampoline.
//
// Carved out of cel_runtime.c per
// `doc/implementation-plan/rewrite/cel-runtime-c-split-plan.md` (P1).
// One function + one weak host stub.  No shared static dependencies on
// the rest of the runtime — the body issues an import call directly,
// so this TU links cleanly without touching `cel_internal.h`.

#include "runtime/cel_log.h"

#include <stdint.h>

// Emit layer.  On wasm this posts args as u32 offsets into linear memory.
// On host it is a weak no-op — tests that want to capture runtime-native
// log lines override `cel_log` directly with a strong definition.
// Parameter counts on `cel_log` and `cel_log_emit` are fixed by the
// `cel_env.cel_log` wasm import signature (9 i32s); they cannot be
// reduced by packing into a struct without changing the ABI.  Suppress
// the function-size gate at the two declaration sites.
#ifdef __wasm__
// NOLINTNEXTLINE(readability-function-size)
void cel_log_emit(const char* file, uint32_t file_len, const char* fn,
                  uint32_t fn_len, uint32_t line, const char* fmt,
                  uint32_t fmt_len, const uint64_t* argv, uint32_t argc) {
  cel_log((uint32_t)(uintptr_t)file, file_len, (uint32_t)(uintptr_t)fn, fn_len,
          line, (uint32_t)(uintptr_t)fmt, fmt_len, (uint32_t)(uintptr_t)argv,
          argc);
}
#else
// NOLINTNEXTLINE(readability-function-size)
__attribute__((weak)) void cel_log(uint32_t file_ptr, uint32_t file_len,
                                   uint32_t fn_ptr, uint32_t fn_len,
                                   uint32_t line, uint32_t fmt_ptr,
                                   uint32_t fmt_len, uint32_t argv_ptr,
                                   uint32_t argc) {
  (void)file_ptr;
  (void)file_len;
  (void)fn_ptr;
  (void)fn_len;
  (void)line;
  (void)fmt_ptr;
  (void)fmt_len;
  (void)argv_ptr;
  (void)argc;
}

// NOLINTNEXTLINE(readability-function-size)
void cel_log_emit(const char* file, uint32_t file_len, const char* fn,
                  uint32_t fn_len, uint32_t line, const char* fmt,
                  uint32_t fmt_len, const uint64_t* argv, uint32_t argc) {
  cel_log((uint32_t)(uintptr_t)file, file_len, (uint32_t)(uintptr_t)fn, fn_len,
          line, (uint32_t)(uintptr_t)fmt, fmt_len, (uint32_t)(uintptr_t)argv,
          argc);
}
#endif
