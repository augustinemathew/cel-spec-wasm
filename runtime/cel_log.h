// Debug / audit logging trampoline.
//
// Shared host import: the wasm module declares `cel_env.cel_log`; the
// host loader binds it to a decoder (see host/cel_log.cc).  Used for
// two purposes:
//
//   1. Dead-code audit.  Every public runtime helper begins with a
//      `CEL_LOG("enter", ...)` call; a full test-suite run records
//      which helpers fired.  Helpers that never show up are candidates
//      for deletion.
//   2. Ad-hoc runtime tracing.  Codegen does not emit `cel_log` calls
//      today, but the import is declared unconditionally on every
//      eval module so a trace can be inserted at any call site without
//      a re-link.
//
// Format mini-language (parsed host-side):
//
//   %s   string span           (u32 ptr, u32 len) packed into u64
//   %d   signed i64
//   %u   unsigned u64
//   %f   f64                   (bit-cast into u64 payload)
//   %b   bool i32              prints "true" / "false"
//   %v   CelValue offset u32   pretty-printed kind + payload
//   %%   literal percent

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_LOG_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_LOG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  CEL_LOG_TAG_STR = 1,
  CEL_LOG_TAG_INT = 2,
  CEL_LOG_TAG_UINT = 3,
  CEL_LOG_TAG_DOUBLE = 4,
  CEL_LOG_TAG_BOOL = 5,
  CEL_LOG_TAG_VALUE = 6,
};

// Host import on wasm; weak no-op on host builds (tests can override).
#ifdef __wasm__
__attribute__((import_module("cel_env"), import_name("cel_log")))
#endif
void cel_log(uint32_t file_ptr, uint32_t file_len, uint32_t fn_ptr,
             uint32_t fn_len, uint32_t line, uint32_t fmt_ptr, uint32_t fmt_len,
             uint32_t argv_ptr, uint32_t argc);

// Call-site helpers.  Each expands to a `(uint64_t) tag_word,
// (uint64_t) payload` pair packed into `CEL_LOG`'s compound-literal
// array.  Slot layout: (u32 tag, u32 pad, u64 payload) = 16 bytes.
#define CEL_LOG_STR(ptr, len) \
  (uint64_t)CEL_LOG_TAG_STR,  \
      ((uint64_t)(uint32_t)(ptr)) | ((uint64_t)(uint32_t)(len) << 32)

#define CEL_LOG_INT(i) (uint64_t)CEL_LOG_TAG_INT, (uint64_t)(int64_t)(i)

#define CEL_LOG_UINT(u) (uint64_t)CEL_LOG_TAG_UINT, (uint64_t)(u)

#define CEL_LOG_DBL(d) \
  (uint64_t)CEL_LOG_TAG_DOUBLE, __builtin_bit_cast(uint64_t, (double)(d))

#define CEL_LOG_BOOL(b) (uint64_t)CEL_LOG_TAG_BOOL, (uint64_t)((b) ? 1 : 0)

#define CEL_LOG_V(off) (uint64_t)CEL_LOG_TAG_VALUE, (uint64_t)(uint32_t)(off)

void cel_log_emit(const char* file, uint32_t file_len, const char* fn,
                  uint32_t fn_len, uint32_t line, const char* fmt,
                  uint32_t fmt_len, const uint64_t* argv, uint32_t argc);

static inline uint32_t cel_strlen_(const char* s) {
  uint32_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

#ifdef CEL_LOG_DISABLED
#define CEL_LOG(fmt_literal, ...) ((void)0)
#else
#define CEL_LOG(fmt_literal, ...)                                        \
  do {                                                                   \
    static const char kCelLogFmt_[] = (fmt_literal);                     \
    uint64_t argv_[] = {__VA_ARGS__ 0};                                  \
    uint32_t argc_ =                                                     \
        (uint32_t)(((sizeof(argv_) / sizeof(uint64_t)) - 1u) / 2u);      \
    cel_log_emit(__FILE__, (uint32_t)sizeof(__FILE__) - 1u, __func__,    \
                 cel_strlen_(__func__), (uint32_t)__LINE__, kCelLogFmt_, \
                 (uint32_t)sizeof(kCelLogFmt_) - 1u, argv_, argc_);      \
  } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_LOG_H_
