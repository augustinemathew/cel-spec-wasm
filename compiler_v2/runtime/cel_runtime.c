#include "compiler_v2/runtime/cel_runtime.h"

// Host build has libc; wasm32 cross-compile is freestanding.  Declare
// byte-loop implementations of the `<string.h>` helpers we use on wasm
// so the runtime stays self-contained without pulling compiler-rt.
#ifdef __wasm__
static void* memcpy(void* dst, const void* src, size_t n) {
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  for (size_t i = 0; i < n; ++i)
    d[i] = s[i];
  return dst;
}
static void* memset(void* dst, int v, size_t n) {
  unsigned char* d = (unsigned char*)dst;
  for (size_t i = 0; i < n; ++i)
    d[i] = (unsigned char)v;
  return dst;
}
#else
#include <string.h>
#endif

// Shared linear memory.  On wasm32 this extern resolves to the start of
// the module's imported `cel.memory`; wasm-ld fixes it up at link time
// via `--import-memory`.  On the host build we back it with a static
// byte buffer so native tests exercise the exact same layout.
#ifdef __wasm__
// wasm-ld provides `__heap_base` after linking; with `--import-memory`
// the base of memory is index 0, addressable directly.  We bypass the
// extern and synthesize a byte pointer from offset 0.
static uint8_t* cel_memory_base_(void) {
  return (uint8_t*)0;
}
static uint32_t cel_memory_size_(void) {
  // The module imports a 1-page memory (64 KiB) at M1; later milestones
  // negotiate a size via the `cel.abi` section.  Returning a fixed
  // value here is fine — cel_alloc's bounds check uses `limit` from
  // the cursor slot, not this.
  return 64u * 1024u;
}
#else
#ifndef CELWASM_ARENA_BYTES
#define CELWASM_ARENA_BYTES (64u * 1024u)
#endif
static uint8_t g_memory[CELWASM_ARENA_BYTES];
static uint8_t* cel_memory_base_(void) {
  return g_memory;
}
static uint32_t cel_memory_size_(void) {
  return (uint32_t)sizeof(g_memory);
}
#endif

// Arena cursor offsets in the shared memory (parent §8.2).
enum {
  kBumpOffset = 8u,
  kLimitOffset = 12u,
};

// NB: use an aligned pointer-cast load/store rather than memcpy.
// clang's wasm32 backend lowered `memcpy(dst, &v, 4)` as three
// byte-stores (to the high 3 bytes only) which left the low byte of
// `arena_base` / `arena_limit` at bytes 8 / 12 untouched — the arena
// cursor ended up wrong by the LSB of the address on every reset.
// An explicit `*(uint32_t*)p = v` compiles to a single `i32.store`
// that actually writes all four bytes.
static uint32_t load_u32(uint32_t off) {
  return *(const uint32_t*)(cel_memory_base_() + off);
}

static void store_u32(uint32_t off, uint32_t v) {
  *(uint32_t*)(cel_memory_base_() + off) = v;
}

static uint32_t align_up(uint32_t n, uint32_t align) {
  return (n + (align - 1u)) & ~(align - 1u);
}

uint8_t* cel_mem_base(void) {
  return cel_memory_base_();
}

uint32_t cel_mem_size(void) {
  return cel_memory_size_();
}

void cel_reset(uint32_t arena_base, uint32_t arena_limit) {
  CEL_LOG("enter");
  store_u32(kBumpOffset, arena_base);
  store_u32(kLimitOffset, arena_limit);
}

uint32_t cel_alloc(uint32_t n) {
  CEL_LOG("enter");
  uint32_t need = align_up(n, 8u);
  if (need == 0) need = 8u;
  uint32_t bump = load_u32(kBumpOffset);
  uint32_t limit = load_u32(kLimitOffset);
  if (bump + need > limit) return 0;
  store_u32(kBumpOffset, bump + need);
  memset(cel_memory_base_() + bump, 0, need);
  return bump;
}

// Native-caller entry points.  Take an explicit memory-base pointer
// (typically the expr wasm instance's linear memory, obtained by the
// host from `wasmtime_caller_t`) instead of the C global.  The host's
// `cel.cel_reset` / `cel.cel_alloc` trampolines forward straight into
// these — no logic duplication between trampoline and runtime.
//
// Keeps the regular `cel_reset` / `cel_alloc` intact for the wasm
// build path (where `cel_memory_base_()` returns 0 and the functions
// operate on the module's own linear memory) and for the native test
// build (where `cel_memory_base_()` returns `g_memory`).
void cel_reset_native(uint8_t* mem, uint32_t arena_base, uint32_t arena_limit) {
  *(uint32_t*)(mem + kBumpOffset) = arena_base;
  *(uint32_t*)(mem + kLimitOffset) = arena_limit;
}

uint32_t cel_alloc_native(uint8_t* mem, uint32_t n) {
  uint32_t need = align_up(n, 8u);
  if (need == 0) need = 8u;
  uint32_t* bump = (uint32_t*)(mem + kBumpOffset);
  const uint32_t* limit = (const uint32_t*)(mem + kLimitOffset);
  if (*bump + need > *limit) return 0;
  uint32_t out = *bump;
  *bump += need;
  memset(mem + out, 0, need);
  return out;
}

static CelValue* cv_at(uint32_t off) {
  return (CelValue*)(cel_memory_base_() + off);
}

CelValue* cel_value_at(uint32_t off) {
  CEL_LOG("enter");
  if (off == 0) return (CelValue*)0;
  return cv_at(off);
}

static uint32_t alloc_cv(void) {
  return cel_alloc((uint32_t)sizeof(CelValue));
}

uint32_t cel_make_null(void) {
  CEL_LOG("enter");
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_NULL;
  return off;
}

uint32_t cel_make_bool(int32_t b) {
  CEL_LOG("enter");
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_BOOL;
  v->payload.b = b ? 1 : 0;
  return off;
}

uint32_t cel_make_int(int64_t i) {
  CEL_LOG("enter");
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_INT;
  v->payload.i = i;
  return off;
}

uint32_t cel_make_uint(uint64_t u) {
  CEL_LOG("enter");
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_UINT;
  v->payload.u = u;
  return off;
}

uint32_t cel_make_double(double d) {
  CEL_LOG("enter");
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_DOUBLE;
  v->payload.d = d;
  return off;
}

static uint32_t make_span_copy(CelKind kind, const void* src, uint32_t len) {
  uint32_t data_off = 0;
  if (len > 0) {
    data_off = cel_alloc(len);
    if (data_off == 0) return 0;
    memcpy(cel_memory_base_() + data_off, src, len);
  }
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = kind;
  v->payload.s.ptr = data_off;
  v->payload.s.len = len;
  return off;
}

uint32_t cel_make_string(const char* src, uint32_t len) {
  CEL_LOG("enter");
  return make_span_copy(CEL_STRING, src, len);
}

uint32_t cel_make_bytes(const void* src, uint32_t len) {
  CEL_LOG("enter");
  return make_span_copy(CEL_BYTES, src, len);
}

static uint32_t make_span_view(CelKind kind, uint32_t ptr, uint32_t len) {
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = kind;
  v->payload.s.ptr = ptr;
  v->payload.s.len = len;
  return off;
}

uint32_t cel_make_string_view(uint32_t ptr, uint32_t len) {
  CEL_LOG("enter");
  return make_span_view(CEL_STRING, ptr, len);
}

uint32_t cel_make_bytes_view(uint32_t ptr, uint32_t len) {
  CEL_LOG("enter");
  return make_span_view(CEL_BYTES, ptr, len);
}

// ---- cel_log trampoline --------------------------------------------------

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
