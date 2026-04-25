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
// the base of memory is index 0, addressable directly.  We synthesize
// a byte pointer from offset 0.
//
// IMPORTANT: route the zero through `uintptr_t` + an inline-asm
// opacity barrier.  Without the barrier, clang's wasm32 backend sees
// `(uint8_t*)0` as a C null pointer and treats every store through
// `cel_memory_base_() + off` as undefined behaviour — which it then
// elides entirely.  Disassembly without this fix shows `cel_reset`
// compiles to a no-op (no `i32.store` at byte 8 or 12) and
// `cel_alloc` compiles to `unreachable`.  The barrier prevents the
// optimizer from reasoning about the value-of-zero through the
// pointer cast; the runtime cost is one register copy.
static uint8_t* cel_memory_base_(void) {
  uintptr_t p = 0;
  __asm__("" : "+r"(p));
  return (uint8_t*)p;
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

// ---- map runtime ---------------------------------------------------------
//
// Three-path dispatch design (`map-list-dispatch.md`):
//   - kArena   : `cel_map_lookup_arena`    (pure wasm, called directly)
//   - kHost    : `cel_host.cel_map_lookup` (host trampoline, called direct)
//   - kDynamic : `cel_map_lookup`          (this dispatcher, tail-calls)
//
// Map literals construct via `cel_map_create` + `cel_map_insert`; both
// only ever produce CEL_MAP_ARENA values — kHost values originate from
// proto reflection or `Activation::Bind`, never from emitted codegen.

static void poison(CelValue* v, uint32_t err_code) {
  v->kind = CEL_ERROR;
  v->payload.err = err_code;
}

static int is_valid_map_key_kind(uint32_t kind) {
  return kind == CEL_BOOL || kind == CEL_INT || kind == CEL_UINT ||
         kind == CEL_STRING;
}

// Cross-type numeric equality per langdef §"Equality": int/uint
// compare by mathematical value (no wraparound), with negative ints
// never equal to any uint.  M3 keys are bool/int/uint/string only —
// double keys are rejected by the checker.
static int numeric_keys_equal(const CelValue* a, const CelValue* b) {
  if (a->kind == CEL_INT && b->kind == CEL_INT) {
    return a->payload.i == b->payload.i;
  }
  if (a->kind == CEL_UINT && b->kind == CEL_UINT) {
    return a->payload.u == b->payload.u;
  }
  if (a->kind == CEL_INT && b->kind == CEL_UINT) {
    if (a->payload.i < 0) {
      return 0;
    }
    return (uint64_t)a->payload.i == b->payload.u;
  }
  if (a->kind == CEL_UINT && b->kind == CEL_INT) {
    if (b->payload.i < 0) {
      return 0;
    }
    return a->payload.u == (uint64_t)b->payload.i;
  }
  return 0;
}

static int spans_equal(CelSpan a, CelSpan b) {
  if (a.len != b.len) {
    return 0;
  }
  if (a.len == 0) {
    return 1;
  }
  const uint8_t* base = cel_memory_base_();
  for (uint32_t i = 0; i < a.len; ++i) {
    if (base[a.ptr + i] != base[b.ptr + i]) {
      return 0;
    }
  }
  return 1;
}

static int map_keys_equal(const CelValue* a, const CelValue* b) {
  if (a->kind == CEL_BOOL && b->kind == CEL_BOOL) {
    return (a->payload.b != 0) == (b->payload.b != 0);
  }
  if (a->kind == CEL_STRING && b->kind == CEL_STRING) {
    return spans_equal(a->payload.s, b->payload.s);
  }
  return numeric_keys_equal(a, b);
}

static ArenaMapHeader* arena_map_header(const CelValue* m) {
  return (ArenaMapHeader*)(cel_memory_base_() +
                           m->payload.arena_map.header_ptr);
}

static CelValue* arena_map_entry_key(ArenaMapHeader* hdr, uint32_t i) {
  return (CelValue*)(cel_memory_base_() + hdr->entries_offset +
                     ((size_t)kCelMapEntryStride * i));
}

static CelValue* arena_map_entry_val(ArenaMapHeader* hdr, uint32_t i) {
  return (CelValue*)(cel_memory_base_() + hdr->entries_offset +
                     ((size_t)kCelMapEntryStride * i) + sizeof(CelValue));
}

void cel_map_create(uint32_t out_slot, uint32_t initial_capacity) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  uint32_t hdr_off = cel_alloc((uint32_t)sizeof(ArenaMapHeader));
  if (hdr_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint32_t entries_off = 0;
  if (initial_capacity > 0) {
    entries_off =
        cel_alloc((uint32_t)((size_t)kCelMapEntryStride * initial_capacity));
    if (entries_off == 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return;
    }
  }
  ArenaMapHeader* hdr = (ArenaMapHeader*)(cel_memory_base_() + hdr_off);
  hdr->count = 0;
  hdr->capacity = initial_capacity;
  hdr->entries_offset = entries_off;
  hdr->_pad = 0;
  out->kind = CEL_MAP_ARENA;
  out->payload.arena_map.header_ptr = hdr_off;
}

void cel_map_insert(uint32_t map_slot, uint32_t key_slot, uint32_t value_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  // If the map is already poisoned (e.g. an earlier insert failed),
  // every subsequent insert is a no-op — error sticks.
  if (m->kind != CEL_MAP_ARENA) {
    return;
  }
  CelValue* key = cel_value_at(key_slot);
  CelValue* val = cel_value_at(value_slot);
  if (!is_valid_map_key_kind(key->kind)) {
    poison(m, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaMapHeader* hdr = arena_map_header(m);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (map_keys_equal(arena_map_entry_key(hdr, i), key)) {
      poison(m, CEL_ERR_DUPLICATE_KEY);
      return;
    }
  }
  // Map literals are fixed-length — codegen sized capacity to the
  // exact entry count via `cel_map_create`.  Exceeding it means
  // codegen drifted out of sync with the runtime; poison defensively
  // so the bug surfaces at the first observable boundary instead of
  // silently scribbling past the entries arena.
  if (hdr->count >= hdr->capacity) {
    poison(m, CEL_ERR_OVERFLOW);
    return;
  }
  *arena_map_entry_key(hdr, hdr->count) = *key;
  *arena_map_entry_val(hdr, hdr->count) = *val;
  hdr->count++;
}

void cel_map_lookup_arena(uint32_t out_slot, uint32_t map_slot,
                          uint32_t key_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  CelValue* key = cel_value_at(key_slot);
  if (key->kind == CEL_UNKNOWN || key->kind == CEL_ERROR) {
    *out = *key;
    return;
  }
  CelValue* m = cel_value_at(map_slot);
  // 3VL on operand — same path the dispatcher uses; codegen calls
  // this directly only when origin is kArena, but `m` could still be
  // a poisoned arena map if a prior insert failed.
  if (m->kind == CEL_UNKNOWN || m->kind == CEL_ERROR) {
    *out = *m;
    return;
  }
  ArenaMapHeader* hdr = arena_map_header(m);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (map_keys_equal(arena_map_entry_key(hdr, i), key)) {
      *out = *arena_map_entry_val(hdr, i);
      return;
    }
  }
  poison(out, CEL_ERR_NO_SUCH_KEY);
}

// kDynamic dispatcher.  `__attribute__((musttail))` forces clang to
// emit `return_call` (or `return_call_indirect`); any path it can't
// prove tail-callable is a hard compile error — the whole point.
// Consequence: this dispatcher's stack frame never grows, even when
// invoked recursively through e.g. nested map-of-map indexing.
//
// On the wasm32 target the import call to `cel_host.cel_map_lookup`
// becomes `return_call $import_index`, observable in the disassembly
// of `cel_runtime.wasm`.  On the host build (no `__wasm__`) the
// attribute degrades to a plain tail call — semantics preserved,
// stack guarantees relaxed (host tests don't depend on stack depth).
#ifdef __wasm__
extern void cel_host_cel_map_lookup(uint32_t out_slot, uint32_t map_slot,
                                    uint32_t key_slot)
    __attribute__((import_module("cel_host"), import_name("cel_map_lookup")));
#else
// Host build: weak no-op stub so C++ unit tests link without the
// wasmtime trampoline.  Tests that exercise the kHost path provide
// a strong override (mirrors the cel_log pattern earlier in this
// file).  Default behaviour is poison-with-type-mismatch — making
// any accidental host invocation visible at the assertion boundary.
// External linkage is required for the weak/strong override across TUs.
__attribute__((weak)) void
cel_host_cel_map_lookup(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t map_slot, uint32_t key_slot) {
  (void)map_slot;
  (void)key_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
#endif

void cel_map_lookup(uint32_t out_slot, uint32_t map_slot, uint32_t key_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  if (m->kind == CEL_UNKNOWN || m->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *m;
    return;
  }
  if (m->kind == CEL_MAP_ARENA) {
    __attribute__((musttail)) return cel_map_lookup_arena(out_slot, map_slot,
                                                          key_slot);
  }
  if (m->kind == CEL_MAP_HOST) {
    __attribute__((musttail)) return cel_host_cel_map_lookup(out_slot, map_slot,
                                                             key_slot);
  }
  // Checker should have rejected; defence in depth.
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
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
