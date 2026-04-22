// CEL WASM runtime — M1 scope.
//
// Data model + arena allocator shared between the AOT-generated module and
// native unit tests.  Written in plain C so it can cross-compile to wasm32
// with brew clang (freestanding, no libc) while still linking into
// googletest on the host.
//
// Memory model differs from v1: the expr module defines and exports linear
// memory as `(memory $mem 1) (export "memory" (memory $mem))`; the runtime
// module imports it via `(import "cel" "memory" (memory $mem 1))`.  The
// runtime owns no memory of its own — every "pointer" is a 32-bit byte
// offset into the shared memory.
//
// Arena state lives at fixed offsets inside that shared memory (parent
// design doc §8.2):
//
//   [0..8)    reserved sentinel (offset 0 = "absent" everywhere)
//   [8..12)   u32  bump  (next free byte; the arena cursor)
//   [12..16)  u32  limit (end-of-arena byte)
//   [16..)    .rodata (expr module's active data segment) followed by the
//             bump arena.  The host initializes (bump, limit) via
//             `cel_reset(arena_base, arena_limit)` after instantiation.
//
// On the native-host build the shared memory is backed by a static byte
// buffer (`cel_mem_base()` / `cel_mem_size()`) so the same helpers
// exercise the exact same layout the wasm build uses.
//
// M1 deliberately ships the minimum: CelValue layout, arena, and the
// scalar `cel_make_*` helpers (used by tests and host-boundary boxing).
// Arithmetic / comparison / 3VL / collection primitives land with the
// milestones that add the corresponding codegen arms.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_RUNTIME_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_RUNTIME_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Final CelKind set.  Even kinds unused at M1 are declared so the wire
// encoding stays stable as later milestones light up new codegen arms.
typedef enum {
  CEL_NULL = 0,
  CEL_BOOL = 1,
  CEL_INT = 2,
  CEL_UINT = 3,
  CEL_DOUBLE = 4,
  CEL_STRING = 5,
  CEL_BYTES = 6,
  CEL_LIST = 7,
  CEL_MAP = 8,
  CEL_MESSAGE = 9,
  CEL_TYPE = 10,
  CEL_DURATION = 11,
  CEL_TIMESTAMP = 12,
  CEL_OPTIONAL = 13,
  CEL_UNKNOWN = 14,
  CEL_ERROR = 15,
} CelKind;

typedef struct {
  uint32_t ptr;
  uint32_t len;
} CelSpan;

typedef struct {
  uint32_t ptr;
  uint32_t len;
} CelArray;

typedef struct {
  uint32_t pairs_ptr;
  uint32_t len;
} CelMap;

typedef struct {
  int64_t seconds;
  int32_t nanos;
  int32_t _pad;
} CelDurTs;

typedef struct CelValue CelValue;
struct CelValue {
  uint32_t kind;
  uint32_t _pad;
  union {
    int32_t b;
    int64_t i;
    uint64_t u;
    double d;
    CelSpan s;
    CelSpan bytes;
    CelArray list;
    CelMap map;
    uint32_t msg_slot;
    uint32_t type_id;
    CelDurTs dur;
    CelDurTs ts;
    uint32_t opt;
    uint32_t unk;
    uint32_t err;
  } payload;
};

_Static_assert(sizeof(CelValue) == 24, "CelValue must remain 24 bytes");

// Error codes carried in `cel_make_error(code, ...)`.  Kept numeric so
// the wasm side stays allocation-free in the happy path — the host
// pretty-printer maps code → message when formatting a failed eval.
enum {
  CEL_ERR_OVERFLOW = 10,
  CEL_ERR_DIVIDE_BY_ZERO = 11,
  CEL_ERR_MODULUS_BY_ZERO = 12,
  CEL_ERR_TYPE_MISMATCH = 13,
};

// Shared-memory accessors.  On the host build these return the static
// buffer that backs the "imported" memory; on wasm32 they return the
// module's `memory[0]` base and byte size.  Both are callable before
// `cel_reset` — `cel_mem_base` in particular is how unit tests stage
// rodata bytes before kicking off an eval.
uint8_t* cel_mem_base(void);
uint32_t cel_mem_size(void);

// Bump-allocator.  `cel_reset` writes (arena_base, arena_limit) into
// the fixed cursor slot at bytes 8/12.  `cel_alloc(n)` bumps the cursor
// by `align_up(n, 8)` and returns the pre-bump offset (or 0 if out of
// space).  Neither is called by M1's eval path — pure literal eval is
// all `.rodata` — but both are exported so the memory model is
// observable from host code and testable.
void cel_reset(uint32_t arena_base, uint32_t arena_limit);
uint32_t cel_alloc(uint32_t n);

// Offset → CelValue* helper.  Returns NULL when `off == 0` so callers
// can treat a zero offset uniformly as "absent".  Invalidated by
// `cel_reset`.
CelValue* cel_value_at(uint32_t off);

// Scalar constructors.  Each allocates a fresh 24-byte CelValue in the
// arena and returns its offset (0 on OOM).  String / bytes variants
// copy `len` source bytes into the arena; `_view` variants wrap an
// already-arena-resident span without copying — used when the host has
// already streamed bytes in via a prior `cel_alloc`.
uint32_t cel_make_null(void);
uint32_t cel_make_bool(int32_t b);
uint32_t cel_make_int(int64_t i);
uint32_t cel_make_uint(uint64_t u);
uint32_t cel_make_double(double d);

uint32_t cel_make_string(const char* src, uint32_t len);
uint32_t cel_make_bytes(const void* src, uint32_t len);
uint32_t cel_make_string_view(uint32_t ptr, uint32_t len);
uint32_t cel_make_bytes_view(uint32_t ptr, uint32_t len);

// ---- Debug / audit logging (cel_log) -------------------------------------
//
// Shared host import: the wasm module declares the import, the host
// loader binds `cel_env.cel_log` to a decoder (see host/cel_log.cc).
// Used for two purposes:
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

static inline uint32_t cel_strlen_(const char* s) {
  uint32_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_RUNTIME_H_
