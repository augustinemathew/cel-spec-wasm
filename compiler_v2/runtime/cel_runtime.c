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

// Tri-state numeric comparison result, hoisted up here so
// `map_keys_equal` (Slice 1.6) and `cel_value_eq_polymorphic` can
// consult `numeric_compare_kernel`.  The kernel + per-pair
// comparators (`cmp_i64` etc.) live in the M5.B step 2 section
// below; only the typedef + signature need to be visible here.
typedef enum {
  kCmpLess = 0,
  kCmpEqual = 1,
  kCmpGreater = 2,
  kCmpNanInequal = 3,
} CmpResult;

static CmpResult numeric_compare_kernel(const CelValue* a, const CelValue* b);
static int is_numeric_kind(uint32_t kind);

// Slice 1.6: numeric-key map equality consults the polymorphic
// `numeric_compare_kernel` — handles every {int, uint, double}
// pair, including double-typed *queries* against int/uint keys.
// `double` is NOT a valid map-key kind (`is_valid_map_key_kind`
// rejects it on `cel_map_insert`), but it CAN appear on the
// query side: `1.0 in {1: "a"}` is allowed.  `numeric_keys_equal`
// already handled the int↔uint cross-kind case via mathematical
// comparison; the polymorphic kernel adds double↔int / double↔uint.

static int map_keys_equal(const CelValue* a, const CelValue* b) {
  if (a->kind == CEL_BOOL && b->kind == CEL_BOOL) {
    return (a->payload.b != 0) == (b->payload.b != 0);
  }
  if (a->kind == CEL_STRING && b->kind == CEL_STRING) {
    return spans_equal(a->payload.s, b->payload.s);
  }
  if (is_numeric_kind(a->kind) && is_numeric_kind(b->kind)) {
    return numeric_compare_kernel(a, b) == kCmpEqual;
  }
  return 0;
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

// ---- list runtime --------------------------------------------------------
//
// Three-path dispatch design (`map-list-dispatch.md §4.2 / §6 / §7`):
//   - kArena   : `cel_list_at_arena`        (pure wasm, called directly)
//   - kHost    : `cel_host.cel_list_at`     (host trampoline, called direct)
//   - kDynamic : `cel_list_at`              (this dispatcher, tail-calls)
//
// List literals construct via `cel_list_create(out, count)` followed
// by `cel_list_set(out, i, elem)` for each i in [0, count); both
// only ever produce CEL_LIST_ARENA values — kHost values originate
// from proto reflection (REPEATED fields) or `Activation::Bind`,
// never from emitted codegen.  The list shape is fixed-length —
// codegen knows the element count up front, so there is no growth
// path (no `cel_list_grow` / `cel_list_append`).

static ArenaListHeader* arena_list_header(const CelValue* l) {
  return (ArenaListHeader*)(cel_memory_base_() +
                            l->payload.arena_list.header_ptr);
}

static CelValue* arena_list_element(ArenaListHeader* hdr, uint32_t i) {
  return (CelValue*)(cel_memory_base_() + hdr->elements_offset +
                     ((size_t)kCelListEntryStride * i));
}

void cel_list_create(uint32_t out_slot, uint32_t count) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  uint32_t hdr_off = cel_alloc((uint32_t)sizeof(ArenaListHeader));
  if (hdr_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint32_t elements_off = 0;
  if (count > 0) {
    elements_off = cel_alloc((uint32_t)((size_t)kCelListEntryStride * count));
    if (elements_off == 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return;
    }
    // cel_alloc zero-fills, leaving every CelValue with kind=CEL_NULL
    // (CEL_NULL == 0 by the enum order), so an unset element reads as
    // null rather than a garbage kind.  Any codegen-correct emit
    // overwrites every slot via cel_list_set before the list is used.
  }
  ArenaListHeader* hdr = (ArenaListHeader*)(cel_memory_base_() + hdr_off);
  hdr->count = count;
  hdr->capacity = count;
  hdr->elements_offset = elements_off;
  hdr->_pad = 0;
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
}

void cel_list_set(uint32_t list_slot, uint32_t index, uint32_t elem_slot) {
  CEL_LOG("enter");
  CelValue* l = cel_value_at(list_slot);
  // If the list is already poisoned, every subsequent set is a no-op
  // — error sticks.
  if (l->kind != CEL_LIST_ARENA) {
    return;
  }
  ArenaListHeader* hdr = arena_list_header(l);
  // List literals are fixed-length — codegen knows `index < count`
  // because both are compile-time constants.  An out-of-range index
  // means codegen drifted out of sync with the runtime; poison
  // defensively so the bug surfaces at the first observable boundary
  // instead of silently scribbling past the elements arena.
  if (index >= hdr->count) {
    poison(l, CEL_ERR_OVERFLOW);
    return;
  }
  *arena_list_element(hdr, index) = *cel_value_at(elem_slot);
}

void cel_list_at_arena(uint32_t out_slot, uint32_t list_slot,
                       uint32_t index_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  CelValue* index = cel_value_at(index_slot);
  if (index->kind == CEL_UNKNOWN || index->kind == CEL_ERROR) {
    *out = *index;
    return;
  }
  CelValue* l = cel_value_at(list_slot);
  if (l->kind == CEL_UNKNOWN || l->kind == CEL_ERROR) {
    *out = *l;
    return;
  }
  // Per langdef §"Indexing": list indices are int only; uint is a
  // checker error and never reaches here, but defend in depth.
  if (index->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* hdr = arena_list_header(l);
  int64_t i = index->payload.i;
  if (i < 0 || (uint64_t)i >= (uint64_t)hdr->count) {
    poison(out, CEL_ERR_INDEX_OUT_OF_BOUNDS);
    return;
  }
  *out = *arena_list_element(hdr, (uint32_t)i);
}

// kDynamic dispatcher — same shape as `cel_map_lookup`.  See the
// musttail commentary on that function; identical toolchain
// constraints apply here.
#ifdef __wasm__
extern void cel_host_cel_list_at(uint32_t out_slot, uint32_t list_slot,
                                 uint32_t index_slot)
    __attribute__((import_module("cel_host"), import_name("cel_list_at")));
#else
__attribute__((
    weak)) void cel_host_cel_list_at(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t list_slot, uint32_t index_slot) {
  (void)list_slot;
  (void)index_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
#endif

void cel_list_at(uint32_t out_slot, uint32_t list_slot, uint32_t index_slot) {
  CEL_LOG("enter");
  CelValue* l = cel_value_at(list_slot);
  if (l->kind == CEL_UNKNOWN || l->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *l;
    return;
  }
  if (l->kind == CEL_LIST_ARENA) {
    __attribute__((musttail)) return cel_list_at_arena(out_slot, list_slot,
                                                       index_slot);
  }
  if (l->kind == CEL_LIST_HOST) {
    __attribute__((musttail)) return cel_host_cel_list_at(out_slot, list_slot,
                                                          index_slot);
  }
  // Checker should have rejected; defence in depth.
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

// =====================================================================
// M5.D step 1 — aggregate-op kArena fast paths (size / in / eq /
// concat for lists, size / in / eq for maps).  No host trip; pure
// wasm.  kHost trampolines + the kDynamic dispatchers land in
// step 2.  See `m5-kcall-comprehensions.md §2.1`.
// =====================================================================

// Forward decls — bodies live in the M5.B section below (3VL
// absorber + write_* writers are shared across every M5 helper
// family).  Pre-declaring keeps them callable from M5.D's helpers
// without reshuffling the file.
static int absorb_3vl_binary(CelValue* out, const CelValue* a,
                             const CelValue* b);
static int absorb_3vl_unary(CelValue* out, const CelValue* a);
static void write_int(CelValue* out, int64_t v);
static void write_bool(CelValue* out, int b);

// Slice 1.6 — polymorphic element-equality matcher.  Used by
// `cel_list_in_arena` / `cel_map_in_arena` to test whether a
// scalar query equals any element / key under langdef §"Equality"
// semantics.  Differs from a same-kind matcher by routing every
// numeric pair through `numeric_compare_kernel` (defined further
// down in the M5.B step 2 section) — so `1 in [1.0]` returns true,
// matching the conformance corpus' `int_in_doubles` row.
//
// Returns 1 (equal), 0 (unequal-or-different-shape).  Caller has
// already absorbed 3VL on the operands; this matcher does not
// surface ERROR / UNKNOWN.
//
// Nested aggregates (CEL_LIST_*, CEL_MAP_*, CEL_MESSAGE) still
// return 0 here — the arena fast path is correct only for scalar
// element types; codegen gates routing accordingly.
//
// Forward-decls land at the top of the M5.B step 2 section; the
// kernel + predicate live below in this same TU and resolve at
// link-within-translation-unit time.
static int cel_value_eq_polymorphic(const CelValue* a, const CelValue* b);

// Pre-Slice-1.6 same-kind matcher.  Retained as a thin alias around
// the polymorphic matcher so other callers (cel_list_eq_arena /
// cel_map_eq_arena value comparison) inherit the polymorphic
// behaviour automatically — list equality is element-wise polymorphic
// per langdef §"Equality" too.  No callsites depend on the old
// "no implicit promotion" behaviour.
static int cel_value_eq(const CelValue* a, const CelValue* b) {
  return cel_value_eq_polymorphic(a, b);
}

void cel_list_size_arena(uint32_t out_slot, uint32_t list_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* l = cel_value_at(list_slot);
  if (absorb_3vl_unary(out, l)) return;
  if (l->kind != CEL_LIST_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* hdr = arena_list_header(l);
  write_int(out, (int64_t)hdr->count);
}

void cel_list_in_arena(uint32_t out_slot, uint32_t value_slot,
                       uint32_t list_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(value_slot);
  const CelValue* l = cel_value_at(list_slot);
  if (absorb_3vl_binary(out, v, l)) return;
  if (l->kind != CEL_LIST_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* hdr = arena_list_header(l);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (cel_value_eq(arena_list_element(hdr, i), v)) {
      write_bool(out, 1);
      return;
    }
  }
  write_bool(out, 0);
}

void cel_list_eq_arena(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (a->kind != CEL_LIST_ARENA || b->kind != CEL_LIST_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* ha = arena_list_header(a);
  ArenaListHeader* hb = arena_list_header(b);
  if (ha->count != hb->count) {
    write_bool(out, 0);
    return;
  }
  for (uint32_t i = 0; i < ha->count; ++i) {
    if (!cel_value_eq(arena_list_element(ha, i), arena_list_element(hb, i))) {
      write_bool(out, 0);
      return;
    }
  }
  write_bool(out, 1);
}

// Allocates header + elements arena for a fresh CEL_LIST_ARENA of
// `total` elements.  Returns the header offset, or 0 on OOM (with
// `out` poisoned with `CEL_ERR_OVERFLOW`).  Caller fills in the
// elements run.
static uint32_t alloc_concat_list(CelValue* out, uint32_t total) {
  uint32_t hdr_off = cel_alloc((uint32_t)sizeof(ArenaListHeader));
  if (hdr_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return 0;
  }
  uint32_t elements_off = 0;
  if (total > 0) {
    elements_off = cel_alloc((uint32_t)((size_t)kCelListEntryStride * total));
    if (elements_off == 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return 0;
    }
  }
  ArenaListHeader* hdr = (ArenaListHeader*)(cel_memory_base_() + hdr_off);
  hdr->count = total;
  hdr->capacity = total;
  hdr->elements_offset = elements_off;
  hdr->_pad = 0;
  return hdr_off;
}

static void copy_elements(uint32_t elements_off, uint32_t dst_index,
                          ArenaListHeader* src) {
  for (uint32_t i = 0; i < src->count; ++i) {
    *(CelValue*)(cel_memory_base_() + elements_off +
                 ((size_t)kCelListEntryStride * (dst_index + i))) =
        *arena_list_element(src, i);
  }
}

void cel_list_concat_arena(uint32_t out_slot, uint32_t a_slot,
                           uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (a->kind != CEL_LIST_ARENA || b->kind != CEL_LIST_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* ha = arena_list_header(a);
  ArenaListHeader* hb = arena_list_header(b);
  uint32_t hdr_off = alloc_concat_list(out, ha->count + hb->count);
  if (hdr_off == 0) return;  // OOM, out already poisoned.
  ArenaListHeader* hdr = (ArenaListHeader*)(cel_memory_base_() + hdr_off);
  copy_elements(hdr->elements_offset, 0, ha);
  copy_elements(hdr->elements_offset, ha->count, hb);
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
}

void cel_map_size_arena(uint32_t out_slot, uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* m = cel_value_at(map_slot);
  if (absorb_3vl_unary(out, m)) return;
  if (m->kind != CEL_MAP_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaMapHeader* hdr = arena_map_header(m);
  write_int(out, (int64_t)hdr->count);
}

void cel_map_in_arena(uint32_t out_slot, uint32_t key_slot, uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* k = cel_value_at(key_slot);
  const CelValue* m = cel_value_at(map_slot);
  if (absorb_3vl_binary(out, k, m)) return;
  if (m->kind != CEL_MAP_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaMapHeader* hdr = arena_map_header(m);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (map_keys_equal(arena_map_entry_key(hdr, i), k)) {
      write_bool(out, 1);
      return;
    }
  }
  write_bool(out, 0);
}

// Returns 1 iff entry `i` of `ha` is structurally equal to some
// entry of `hb`.  Caller has confirmed both maps have the same
// entry count, so a missing match means the maps differ.
static int arena_map_entry_matches(ArenaMapHeader* ha, uint32_t i,
                                   ArenaMapHeader* hb) {
  const CelValue* ka = arena_map_entry_key(ha, i);
  const CelValue* va = arena_map_entry_val(ha, i);
  for (uint32_t j = 0; j < hb->count; ++j) {
    if (map_keys_equal(ka, arena_map_entry_key(hb, j))) {
      return cel_value_eq(va, arena_map_entry_val(hb, j));
    }
  }
  return 0;
}

void cel_map_eq_arena(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (a->kind != CEL_MAP_ARENA || b->kind != CEL_MAP_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaMapHeader* ha = arena_map_header(a);
  ArenaMapHeader* hb = arena_map_header(b);
  if (ha->count != hb->count) {
    write_bool(out, 0);
    return;
  }
  // Map equality is set-equality on entries (langdef §"Equality":
  // map order is irrelevant).  For each entry in a, find a matching
  // key in b and compare values.
  for (uint32_t i = 0; i < ha->count; ++i) {
    if (!arena_map_entry_matches(ha, i, hb)) {
      write_bool(out, 0);
      return;
    }
  }
  write_bool(out, 1);
}

// =====================================================================
// M5.D step 2 — kDynamic dispatchers + kHost extern decls for
// aggregate ops (size / in / eq / concat for lists; size / in / eq
// for maps) plus the polymorphic `cel_message_eq` host helper.
// Each dispatcher mirrors the `cel_map_lookup` shape at line 434:
// 3VL absorption → branch on operand kind → `__attribute__((musttail))`
// to either an arena fast path (M5.D step 1) or a kHost trampoline.
// The kHost arms link to `cel_host.cel_*` imports on the wasm
// build; the host build supplies weak no-op stubs that poison
// with TYPE_MISMATCH so an accidental host invocation surfaces
// at the assertion boundary.  See `m5-kcall-comprehensions.md §2.1`.
// =====================================================================

#ifdef __wasm__
extern void cel_host_cel_list_size(uint32_t out_slot, uint32_t list_slot)
    __attribute__((import_module("cel_host"), import_name("cel_list_size")));
extern void cel_host_cel_list_in(uint32_t out_slot, uint32_t value_slot,
                                 uint32_t list_slot)
    __attribute__((import_module("cel_host"), import_name("cel_list_in")));
extern void cel_host_cel_list_eq(uint32_t out_slot, uint32_t a_slot,
                                 uint32_t b_slot)
    __attribute__((import_module("cel_host"), import_name("cel_list_eq")));
extern void cel_host_cel_list_concat(uint32_t out_slot, uint32_t a_slot,
                                     uint32_t b_slot)
    __attribute__((import_module("cel_host"), import_name("cel_list_concat")));
extern void cel_host_cel_map_size(uint32_t out_slot, uint32_t map_slot)
    __attribute__((import_module("cel_host"), import_name("cel_map_size")));
extern void cel_host_cel_map_in(uint32_t out_slot, uint32_t key_slot,
                                uint32_t map_slot)
    __attribute__((import_module("cel_host"), import_name("cel_map_in")));
extern void cel_host_cel_map_eq(uint32_t out_slot, uint32_t a_slot,
                                uint32_t b_slot)
    __attribute__((import_module("cel_host"), import_name("cel_map_eq")));
extern void cel_host_cel_message_eq(uint32_t out_slot, uint32_t a_slot,
                                    uint32_t b_slot)
    __attribute__((import_module("cel_host"), import_name("cel_message_eq")));
#else
__attribute__((
    weak)) void cel_host_cel_list_size(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t list_slot) {
  (void)list_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_list_in(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t value_slot, uint32_t list_slot) {
  (void)value_slot;
  (void)list_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_list_eq(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  (void)a_slot;
  (void)b_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_list_concat(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  (void)a_slot;
  (void)b_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_map_size(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t map_slot) {
  (void)map_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_map_in(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t key_slot, uint32_t map_slot) {
  (void)key_slot;
  (void)map_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_map_eq(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  (void)a_slot;
  (void)b_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_message_eq(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  (void)a_slot;
  (void)b_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
#endif

void cel_list_size(uint32_t out_slot, uint32_t list_slot) {
  CEL_LOG("enter");
  CelValue* l = cel_value_at(list_slot);
  if (l->kind == CEL_UNKNOWN || l->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *l;
    return;
  }
  if (l->kind == CEL_LIST_ARENA) {
    __attribute__((musttail)) return cel_list_size_arena(out_slot, list_slot);
  }
  if (l->kind == CEL_LIST_HOST) {
    __attribute__((musttail)) return cel_host_cel_list_size(out_slot,
                                                            list_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_list_in(uint32_t out_slot, uint32_t value_slot, uint32_t list_slot) {
  CEL_LOG("enter");
  CelValue* v = cel_value_at(value_slot);
  CelValue* l = cel_value_at(list_slot);
  if (v->kind == CEL_UNKNOWN || v->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *v;
    return;
  }
  if (l->kind == CEL_UNKNOWN || l->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *l;
    return;
  }
  if (l->kind == CEL_LIST_ARENA) {
    __attribute__((musttail)) return cel_list_in_arena(out_slot, value_slot,
                                                       list_slot);
  }
  if (l->kind == CEL_LIST_HOST) {
    __attribute__((musttail)) return cel_host_cel_list_in(out_slot, value_slot,
                                                          list_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_list_eq(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* a = cel_value_at(a_slot);
  CelValue* b = cel_value_at(b_slot);
  if (a->kind == CEL_UNKNOWN || a->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *a;
    return;
  }
  if (b->kind == CEL_UNKNOWN || b->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *b;
    return;
  }
  // Both arena → fast path; otherwise host trampoline materialises
  // both sides via the appropriate backing methods.  Cross-origin
  // (one arena, one host) routes to the host trampoline which
  // POISONs with TYPE_MISMATCH for now (M6 follow-up).
  if (a->kind == CEL_LIST_ARENA && b->kind == CEL_LIST_ARENA) {
    __attribute__((musttail)) return cel_list_eq_arena(out_slot, a_slot,
                                                       b_slot);
  }
  if ((a->kind == CEL_LIST_ARENA || a->kind == CEL_LIST_HOST) &&
      (b->kind == CEL_LIST_ARENA || b->kind == CEL_LIST_HOST)) {
    __attribute__((musttail)) return cel_host_cel_list_eq(out_slot, a_slot,
                                                          b_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_list_concat(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* a = cel_value_at(a_slot);
  CelValue* b = cel_value_at(b_slot);
  if (a->kind == CEL_UNKNOWN || a->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *a;
    return;
  }
  if (b->kind == CEL_UNKNOWN || b->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *b;
    return;
  }
  if (a->kind == CEL_LIST_ARENA && b->kind == CEL_LIST_ARENA) {
    __attribute__((musttail)) return cel_list_concat_arena(out_slot, a_slot,
                                                           b_slot);
  }
  // Mixed-origin or both-host: route to host trampoline.  For this
  // slice the host trampoline POISONs with TYPE_MISMATCH on either
  // mixed origins or both-host (full materialisation lands as a
  // follow-up).  Documented in CelListConcatImpl.
  if ((a->kind == CEL_LIST_ARENA || a->kind == CEL_LIST_HOST) &&
      (b->kind == CEL_LIST_ARENA || b->kind == CEL_LIST_HOST)) {
    __attribute__((musttail)) return cel_host_cel_list_concat(out_slot, a_slot,
                                                              b_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_map_size(uint32_t out_slot, uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  if (m->kind == CEL_UNKNOWN || m->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *m;
    return;
  }
  if (m->kind == CEL_MAP_ARENA) {
    __attribute__((musttail)) return cel_map_size_arena(out_slot, map_slot);
  }
  if (m->kind == CEL_MAP_HOST) {
    __attribute__((musttail)) return cel_host_cel_map_size(out_slot, map_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_map_in(uint32_t out_slot, uint32_t key_slot, uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* k = cel_value_at(key_slot);
  CelValue* m = cel_value_at(map_slot);
  if (k->kind == CEL_UNKNOWN || k->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *k;
    return;
  }
  if (m->kind == CEL_UNKNOWN || m->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *m;
    return;
  }
  if (m->kind == CEL_MAP_ARENA) {
    __attribute__((musttail)) return cel_map_in_arena(out_slot, key_slot,
                                                      map_slot);
  }
  if (m->kind == CEL_MAP_HOST) {
    __attribute__((musttail)) return cel_host_cel_map_in(out_slot, key_slot,
                                                         map_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_map_eq(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* a = cel_value_at(a_slot);
  CelValue* b = cel_value_at(b_slot);
  if (a->kind == CEL_UNKNOWN || a->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *a;
    return;
  }
  if (b->kind == CEL_UNKNOWN || b->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *b;
    return;
  }
  if (a->kind == CEL_MAP_ARENA && b->kind == CEL_MAP_ARENA) {
    __attribute__((musttail)) return cel_map_eq_arena(out_slot, a_slot, b_slot);
  }
  if ((a->kind == CEL_MAP_ARENA || a->kind == CEL_MAP_HOST) &&
      (b->kind == CEL_MAP_ARENA || b->kind == CEL_MAP_HOST)) {
    __attribute__((musttail)) return cel_host_cel_map_eq(out_slot, a_slot,
                                                         b_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

// =====================================================================
// M5.B — arithmetic + comparison helpers (slot-out helper ABI).
// Each helper's signature is `(out_slot, arg_slots...) -> void`; the
// caller already knows the result lives at `out_slot`, so there's no
// return value.  Cel-cpp parity citations point at
// `third_party/cel-cpp/runtime/standard/{arithmetic,equality,
// comparison}_functions.cc`.  See `m5-kcall-comprehensions.md §2.1`
// for the design rationale and the WAT traces 16-17 for the wire
// shape.
// =====================================================================

// 3VL absorption shared by every binary arith / compare helper.
// Returns 1 (and overwrites *out) if either operand is ERROR /
// UNKNOWN, in which case the caller must NOT proceed to the
// type-check + math.  Left-bias for both ERROR and UNKNOWN — a
// proper UNKNOWN+UNKNOWN merge lives in `cel_unknown_merge`
// (M5.G when control-flow lowering needs it).  Mirrors cel-cpp's
// `EquivalentTypeOrError` envelope.
static int absorb_3vl_binary(CelValue* out, const CelValue* a,
                             const CelValue* b) {
  if (a->kind == CEL_ERROR) {
    *out = *a;
    return 1;
  }
  if (b->kind == CEL_ERROR) {
    *out = *b;
    return 1;
  }
  if (a->kind == CEL_UNKNOWN) {
    *out = *a;
    return 1;
  }
  if (b->kind == CEL_UNKNOWN) {
    *out = *b;
    return 1;
  }
  return 0;
}

static int absorb_3vl_unary(CelValue* out, const CelValue* a) {
  if (a->kind == CEL_ERROR || a->kind == CEL_UNKNOWN) {
    *out = *a;
    return 1;
  }
  return 0;
}

// Helper: same-kind type check.  Wrong kind on either operand →
// poison and return 1 (skip math).
static int require_kinds(CelValue* out, const CelValue* a, const CelValue* b,
                         uint32_t want) {
  if (a->kind != want || b->kind != want) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  return 0;
}

static void write_int(CelValue* out, int64_t v) {
  out->kind = CEL_INT;
  out->payload.i = v;
}
static void write_uint(CelValue* out, uint64_t v) {
  out->kind = CEL_UINT;
  out->payload.u = v;
}
static void write_double(CelValue* out, double v) {
  out->kind = CEL_DOUBLE;
  out->payload.d = v;
}
static void write_bool(CelValue* out, int b) {
  out->kind = CEL_BOOL;
  out->payload.b = b ? 1 : 0;
}

// Manual u64 multiply-overflow detection via split 32×32→64
// partial products.  We avoid `__builtin_mul_overflow` for 64-bit
// operands because clang lowers it through `__multi3` (a 128-bit
// multiply from compiler-rt) — and the wasm32 freestanding build
// doesn't link compiler-rt by design.  We avoid the divide-by-b
// bounds-check shape too: clang's optimiser recognises it and
// re-folds it back into a `__multi3` call.  Splitting into
// 32-bit halves means every multiply here is a 32×32→64 op the
// wasm32 backend lowers natively as `i64.mul`, with no high-half
// reasoning the optimiser can lift to 128-bit math.
//
// Logic:
//   a*b = (ah*2^32 + al) * (bh*2^32 + bl)
//       = al*bl + (ah*bl + al*bh)*2^32 + ah*bh*2^64
//   Overflow iff (ah * bh) != 0
//                OR (ah*bl + al*bh) overflows 32 bits
//                OR adding the shifted middle to al*bl overflows.
static int uint64_mul_overflows(uint64_t a, uint64_t b, uint64_t* r) {
  uint64_t ah = a >> 32;
  uint64_t al = a & 0xFFFFFFFFULL;
  uint64_t bh = b >> 32;
  uint64_t bl = b & 0xFFFFFFFFULL;
  if (ah != 0 && bh != 0) return 1;
  uint64_t mid = (ah * bl) + (al * bh);  // operands ≤32 bits, sum may carry
  if ((mid >> 32) != 0) return 1;
  uint64_t lo = al * bl;
  uint64_t result = lo + (mid << 32);
  if (result < lo) return 1;  // unsigned add overflow → product > UINT64_MAX
  *r = result;
  return 0;
}

// Signed int64 mul overflow on top of the unsigned check: take
// magnitudes, run the unsigned check, then validate the signed
// range against INT64_MIN / INT64_MAX based on operand signs.
static int int64_mul_overflows(int64_t a, int64_t b, int64_t* r) {
  if (a == 0 || b == 0) {
    *r = 0;
    return 0;
  }
  // INT64_MIN handled specially: |INT64_MIN| > INT64_MAX, so the
  // standard magnitude trick can't represent it.  Only INT64_MIN*1
  // and INT64_MIN*0 don't overflow; we already handled 0 above.
  if (a == INT64_MIN) return b != 1 ? 1 : (*r = INT64_MIN, 0);
  if (b == INT64_MIN) return a != 1 ? 1 : (*r = INT64_MIN, 0);
  uint64_t ua = (uint64_t)(a < 0 ? -a : a);
  uint64_t ub = (uint64_t)(b < 0 ? -b : b);
  uint64_t up;
  if (uint64_mul_overflows(ua, ub, &up)) return 1;
  // Signs determine whether result is +/-.  Range:
  //   positive: [0, INT64_MAX]
  //   negative: [INT64_MIN, 0]   (so |result| ≤ -(INT64_MIN+1)+1 = INT64_MAX+1)
  int negative = (a < 0) ^ (b < 0);
  if (negative) {
    if (up > (uint64_t)INT64_MAX + 1ULL) return 1;
    *r = -(int64_t)up;
  } else {
    if (up > (uint64_t)INT64_MAX) return 1;
    *r = (int64_t)up;
  }
  return 0;
}

// ---- int64 arithmetic ----------------------------------------------------
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/arithmetic_functions.cc::
//     add_int64 / sub_int64 / mul_int64 / div_int64 / mod_int64 /
//     negate_int64

void cel_int_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  int64_t r;
  if (__builtin_add_overflow(a->payload.i, b->payload.i, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, r);
}

void cel_int_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  int64_t r;
  if (__builtin_sub_overflow(a->payload.i, b->payload.i, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, r);
}

void cel_int_mul_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  int64_t r;
  if (int64_mul_overflows(a->payload.i, b->payload.i, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, r);
}

void cel_int_div_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  if (b->payload.i == 0) {
    poison(out, CEL_ERR_DIVIDE_BY_ZERO);
    return;
  }
  // INT64_MIN / -1 overflows in two's complement.
  if (a->payload.i == INT64_MIN && b->payload.i == -1) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, a->payload.i / b->payload.i);
}

void cel_int_mod_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  if (b->payload.i == 0) {
    poison(out, CEL_ERR_MODULUS_BY_ZERO);
    return;
  }
  // INT64_MIN % -1 is undefined behaviour in C; cel-cpp returns 0
  // for this case explicitly (the mathematically-correct result).
  if (a->payload.i == INT64_MIN && b->payload.i == -1) {
    write_int(out, 0);
    return;
  }
  write_int(out, a->payload.i % b->payload.i);
}

void cel_int_neg_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (v->payload.i == INT64_MIN) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, -v->payload.i);
}

// ---- uint64 arithmetic ---------------------------------------------------

void cel_uint_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  uint64_t r;
  if (__builtin_add_overflow(a->payload.u, b->payload.u, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, r);
}

void cel_uint_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  uint64_t r;
  if (__builtin_sub_overflow(a->payload.u, b->payload.u, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, r);
}

void cel_uint_mul_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  uint64_t r;
  if (uint64_mul_overflows(a->payload.u, b->payload.u, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, r);
}

void cel_uint_div_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  if (b->payload.u == 0) {
    poison(out, CEL_ERR_DIVIDE_BY_ZERO);
    return;
  }
  write_uint(out, a->payload.u / b->payload.u);
}

void cel_uint_mod_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  if (b->payload.u == 0) {
    poison(out, CEL_ERR_MODULUS_BY_ZERO);
    return;
  }
  write_uint(out, a->payload.u % b->payload.u);
}

// ---- double arithmetic ---------------------------------------------------
// Per langdef §"Numeric values": double follows IEEE 754.  No
// overflow / div-by-zero errors — inf/nan results are valid.

void cel_double_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d + b->payload.d);
}

void cel_double_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d - b->payload.d);
}

void cel_double_mul_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d * b->payload.d);
}

void cel_double_div_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d / b->payload.d);
}

void cel_double_neg_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_double(out, -v->payload.d);
}

// ---- comparison helpers --------------------------------------------------
// Same-kind only — cross-type numeric ladder lives in a separate
// `cel_numeric_*` set added with the kCall arm's ladder dispatch
// (M5.B step 2).  Each helper writes a CEL_BOOL CelValue.
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/equality_functions.cc
//   third_party/cel-cpp/runtime/standard/comparison_functions.cc

#define DEFINE_CMP_VV(name, kind, field, op)                       \
  void name(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) { \
    CelValue* out = cel_value_at(out_slot);                        \
    const CelValue* a = cel_value_at(a_slot);                      \
    const CelValue* b = cel_value_at(b_slot);                      \
    if (absorb_3vl_binary(out, a, b)) return;                      \
    if (require_kinds(out, a, b, kind)) return;                    \
    write_bool(out, a->payload.field op b->payload.field);         \
  }

DEFINE_CMP_VV(cel_int_eq_at_vv, CEL_INT, i, ==)
DEFINE_CMP_VV(cel_int_ne_at_vv, CEL_INT, i, !=)
DEFINE_CMP_VV(cel_int_lt_at_vv, CEL_INT, i, <)
DEFINE_CMP_VV(cel_int_le_at_vv, CEL_INT, i, <=)
DEFINE_CMP_VV(cel_int_gt_at_vv, CEL_INT, i, >)
DEFINE_CMP_VV(cel_int_ge_at_vv, CEL_INT, i, >=)

DEFINE_CMP_VV(cel_uint_eq_at_vv, CEL_UINT, u, ==)
DEFINE_CMP_VV(cel_uint_ne_at_vv, CEL_UINT, u, !=)
DEFINE_CMP_VV(cel_uint_lt_at_vv, CEL_UINT, u, <)
DEFINE_CMP_VV(cel_uint_le_at_vv, CEL_UINT, u, <=)
DEFINE_CMP_VV(cel_uint_gt_at_vv, CEL_UINT, u, >)
DEFINE_CMP_VV(cel_uint_ge_at_vv, CEL_UINT, u, >=)

// Double: `==` / `!=` follow IEEE 754 (NaN != NaN, NaN == NaN
// false).  C's `==` and `!=` operators implement this directly.
// Ordering operators (<, <=, >, >=) likewise return false for any
// NaN-bearing comparison per IEEE.
DEFINE_CMP_VV(cel_double_eq_at_vv, CEL_DOUBLE, d, ==)
DEFINE_CMP_VV(cel_double_ne_at_vv, CEL_DOUBLE, d, !=)
DEFINE_CMP_VV(cel_double_lt_at_vv, CEL_DOUBLE, d, <)
DEFINE_CMP_VV(cel_double_le_at_vv, CEL_DOUBLE, d, <=)
DEFINE_CMP_VV(cel_double_gt_at_vv, CEL_DOUBLE, d, >)
DEFINE_CMP_VV(cel_double_ge_at_vv, CEL_DOUBLE, d, >=)

DEFINE_CMP_VV(cel_bool_eq_at_vv, CEL_BOOL, b, ==)
DEFINE_CMP_VV(cel_bool_ne_at_vv, CEL_BOOL, b, !=)
// Bool ordering — `false < true` per langdef §"Booleans".  Since
// `payload.b` is normalised to 0/1 by `write_bool` and
// `cel_make_bool`, the integer relational operators give the
// langdef order directly.
DEFINE_CMP_VV(cel_bool_lt_at_vv, CEL_BOOL, b, <)
DEFINE_CMP_VV(cel_bool_le_at_vv, CEL_BOOL, b, <=)
DEFINE_CMP_VV(cel_bool_gt_at_vv, CEL_BOOL, b, >)
DEFINE_CMP_VV(cel_bool_ge_at_vv, CEL_BOOL, b, >=)

#undef DEFINE_CMP_VV

// =====================================================================
// M5.B step 2 — cross-type numeric comparison ladder.
//
// Each helper accepts any combination of {CEL_INT, CEL_UINT,
// CEL_DOUBLE} on either operand.  The shared `numeric_compare_kernel`
// returns a tri-state result {kLess, kEqual, kGreater, kNanInequal}
// mirroring cel-cpp's `internal/number.h::ComparisonResult`.  Each
// op then collapses that tri-state to a CEL_BOOL.
//
// Boundary handling (cel-cpp parity, `internal/number.h` lines
// 25-44 / 95-165):
//   - int vs uint: negative int is always < any uint; otherwise
//     compare as uint64.
//   - int vs double: if double > kInt64Max → double > int; if
//     double < kInt64Min → double < int; otherwise IEEE-compare
//     (double)int vs double.
//   - uint vs double: if double > kUint64Max → double > uint; if
//     double < 0 → double < uint; otherwise IEEE-compare
//     (double)uint vs double.
//   - NaN: any comparison involving NaN returns kNanInequal so all
//     six op wrappers answer false (matches IEEE / langdef "NaN
//     compares unequal in every direction").
//
// Wasm32 freestanding constraint: this code MUST avoid `__multi3` /
// other compiler-rt 128-bit intrinsics (mirrors the M5.B step 1
// `int64_mul_overflows` precedent).  Only operations used here are
// 64-bit comparisons + a single int↔uint cast that the wasm32 backend
// lowers natively as `i64.lt_s` / `i64.lt_u` / `f64.lt`.  The
// `noinline` attribute on the kernel keeps clang from re-deriving a
// 128-bit fold across the leaf wrappers.
//
// `CmpResult` typedef + the kernel forward decl + `is_numeric_kind`
// were hoisted up to the M5.D-step-1 section of this file at Slice
// 1.6 so `map_keys_equal` / `cel_value_eq_polymorphic` (the
// element-equality matchers used by `_in_` membership) can call
// the kernel.  The full kernel body lands here unchanged.

// Per-pair comparators.  Each takes raw operands of one of three
// scalar shapes and returns the tri-state result.  Pulled out of
// the kernel both to keep each function under the lint size gate
// and to keep the rationale beside the boundary checks they encode
// (cel-cpp parity, `internal/number.h:25-165`).

static CmpResult cmp_i64(int64_t a, int64_t b) {
  if (a < b) return kCmpLess;
  if (a > b) return kCmpGreater;
  return kCmpEqual;
}

static CmpResult cmp_u64(uint64_t a, uint64_t b) {
  if (a < b) return kCmpLess;
  if (a > b) return kCmpGreater;
  return kCmpEqual;
}

static CmpResult cmp_double(double a, double b) {
  // IEEE: any NaN comparison is unordered.
  if (a != a || b != b) return kCmpNanInequal;
  if (a < b) return kCmpLess;
  if (a > b) return kCmpGreater;
  return kCmpEqual;
}

// int vs uint: negative int is always less than any uint;
// otherwise compare via uint64 cast.
static CmpResult cmp_int_vs_uint(int64_t a, uint64_t b) {
  if (a < 0) return kCmpLess;
  return cmp_u64((uint64_t)a, b);
}

// int vs double: boundary check before any narrowing cast.  See
// `third_party/cel-cpp/internal/number.h:127-143`.
static CmpResult cmp_int_vs_double(int64_t a, double b) {
  if (b != b) return kCmpNanInequal;
  if (b > (double)INT64_MAX) return kCmpLess;
  if (b < (double)INT64_MIN) return kCmpGreater;
  return cmp_double((double)a, b);
}

// uint vs double: any negative double is less than every uint;
// any double > UINT64_MAX is greater than every uint.
static CmpResult cmp_uint_vs_double(uint64_t a, double b) {
  if (b != b) return kCmpNanInequal;
  if (b > (double)UINT64_MAX) return kCmpLess;
  if (b < 0.0) return kCmpGreater;
  return cmp_double((double)a, b);
}

// Flip a tri-state result for the swapped-operand convention.  Used
// when we have a comparator for `a vs b` and need `b vs a`.
static CmpResult cmp_flip(CmpResult r) {
  if (r == kCmpLess) return kCmpGreater;
  if (r == kCmpGreater) return kCmpLess;
  return r;
}

// Pack the two operand kinds into a small dense key so the kernel
// dispatch is a single switch.  Caller has already verified both
// operands are numeric.
static uint32_t numeric_kind_pair(uint32_t a_kind, uint32_t b_kind) {
  return (a_kind << 8) | b_kind;
}

// Boundary constants — same byte-for-byte as
// `third_party/cel-cpp/internal/number.h:25-44`.  Inline rather than
// macros so each helper sees the type the wasm32 backend can type-
// check.
static __attribute__((noinline)) CmpResult
numeric_compare_kernel(const CelValue* a, const CelValue* b) {
  switch (numeric_kind_pair(a->kind, b->kind)) {
    case (CEL_INT << 8) | CEL_INT:
      return cmp_i64(a->payload.i, b->payload.i);
    case (CEL_UINT << 8) | CEL_UINT:
      return cmp_u64(a->payload.u, b->payload.u);
    case (CEL_DOUBLE << 8) | CEL_DOUBLE:
      return cmp_double(a->payload.d, b->payload.d);
    case (CEL_INT << 8) | CEL_UINT:
      return cmp_int_vs_uint(a->payload.i, b->payload.u);
    case (CEL_UINT << 8) | CEL_INT:
      return cmp_flip(cmp_int_vs_uint(b->payload.i, a->payload.u));
    case (CEL_INT << 8) | CEL_DOUBLE:
      return cmp_int_vs_double(a->payload.i, b->payload.d);
    case (CEL_DOUBLE << 8) | CEL_INT:
      return cmp_flip(cmp_int_vs_double(b->payload.i, a->payload.d));
    case (CEL_UINT << 8) | CEL_DOUBLE:
      return cmp_uint_vs_double(a->payload.u, b->payload.d);
    case (CEL_DOUBLE << 8) | CEL_UINT:
      return cmp_flip(cmp_uint_vs_double(b->payload.u, a->payload.d));
    default:
      // Caller (`numeric_prelude`) already filters non-numeric kinds;
      // a non-numeric pair reaching the kernel is an invariant
      // violation.  Return kNanInequal so all six op wrappers answer
      // false rather than miscompiling silently.
      return kCmpNanInequal;
  }
}

// True iff both operand kinds are numeric (int / uint / double).
static int is_numeric_kind(uint32_t kind) {
  return kind == CEL_INT || kind == CEL_UINT || kind == CEL_DOUBLE;
}

// Slice 1.6 — definition of the polymorphic element-equality matcher
// forward-declared at the top of the M5.D section.  Routes any
// numeric pair (cross-kind included) through `numeric_compare_kernel`
// so `1 in [1.0]` / `dyn(3) in [1u, 3u]` return true per langdef
// §"List Membership (in)" + §"Equality" + the conformance corpus'
// `int_in_doubles` / `uint_in_ints` rows.
static int cel_value_eq_polymorphic(const CelValue* a, const CelValue* b) {
  if (is_numeric_kind(a->kind) && is_numeric_kind(b->kind)) {
    return numeric_compare_kernel(a, b) == kCmpEqual;
  }
  if (a->kind == CEL_BYTES && b->kind == CEL_BYTES) {
    return spans_equal(a->payload.s, b->payload.s);
  }
  if (a->kind == CEL_NULL && b->kind == CEL_NULL) {
    return 1;
  }
  // Bool / string / cross-kind non-numeric fall through to
  // `map_keys_equal` which itself routes numerics polymorphically
  // (Slice 1.6 update below) and returns 0 for kind mismatches.
  return map_keys_equal(a, b);
}

// Shared prelude for the cross-type numeric helpers: 3VL absorption
// then a numeric-kind check on each operand (any non-numeric → type
// mismatch).  Returns 1 when out_slot has been written and the
// caller should skip the kernel.
static int numeric_prelude(CelValue* out, const CelValue* a,
                           const CelValue* b) {
  if (absorb_3vl_binary(out, a, b)) return 1;
  if (!is_numeric_kind(a->kind) || !is_numeric_kind(b->kind)) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  return 0;
}

void cel_numeric_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpEqual);
}

void cel_numeric_ne_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  // NaN-touching inequality returns TRUE — matches cel-cpp's
  // `Inequal<double>` default
  // (`runtime/standard/equality_functions.cc:78`), which is the
  // IEEE `lhs != rhs` semantic where `NaN != NaN` is true.  An
  // earlier version of this kernel returned false on
  // `kCmpNanInequal`; the prior comment claiming langdef
  // mandated false was a spec misread.  Slice 1.55 (2026-04-25)
  // flipped to `r != kCmpEqual` so every non-equal tri-state
  // (less / greater / nan-inequal) yields true.
  write_bool(out, r != kCmpEqual);
}

void cel_numeric_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpLess);
}

void cel_numeric_le_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpLess || r == kCmpEqual);
}

void cel_numeric_gt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpGreater);
}

void cel_numeric_ge_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpGreater || r == kCmpEqual);
}

void cel_null_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_NULL)) return;
  write_bool(out, 1);  // null == null is always true.
}

// =====================================================================
// M5.B step 2b — polymorphic equality dispatcher.
//
// `cel_equals_at_vv` and `cel_not_equals_at_vv` resolve every cel-cpp
// `equals` / `not_equals` overload at runtime via operand-kind
// switch.  Per langdef §"Equality":
//
//   - Numeric kinds (int / uint / double) compare cross-type by
//     mathematical value via the M5.B step 2 numeric ladder.
//   - Same-kind bool / string / bytes / null use the existing
//     `cel_*_eq_at_vv` helpers.
//   - Aggregate kinds (list / map) tail-call the M5.D step 2
//     dispatcher (`cel_list_eq` / `cel_map_eq`), which handles
//     arena vs host origin internally.
//   - CEL_MESSAGE values delegate to the kHost
//     `cel_host_cel_message_eq` import (M5.D step 2).
//   - Mismatched kinds return `false` per langdef
//     ("comparing incompatible types is not an error"), with one
//     exception: numeric ↔ non-numeric is also `false`.
//   - 3VL: any UNKNOWN / ERROR operand short-circuits via
//     `absorb_3vl_binary`.
//
// `not_equals` is `equals` with the result flipped; share a kernel
// to keep the dispatch ladder authoritative in one place.
//
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/equality_functions.cc
// =====================================================================

// 1 if `kind` is one of the three numeric kinds: int, uint, double.
// Lifted to a dedicated helper so the polymorphic dispatcher's
// branch table reads as a single conjunction per ladder rung.
static int is_numeric(uint32_t kind) {
  return kind == CEL_INT || kind == CEL_UINT || kind == CEL_DOUBLE;
}

// 1 if both operands are list-shaped (arena or host).  Mixed-origin
// pairs route through `cel_list_eq` which absorbs the origin
// difference via its dispatcher.
static int both_lists(uint32_t ka, uint32_t kb) {
  return (ka == CEL_LIST_ARENA || ka == CEL_LIST_HOST) &&
         (kb == CEL_LIST_ARENA || kb == CEL_LIST_HOST);
}

static int both_maps(uint32_t ka, uint32_t kb) {
  return (ka == CEL_MAP_ARENA || ka == CEL_MAP_HOST) &&
         (kb == CEL_MAP_ARENA || kb == CEL_MAP_HOST);
}

// `cel_string_eq_at_vv` / `cel_bytes_eq_at_vv` live further down in
// this file and are reachable from the equality kernel above; their
// declarations come in transitively via cel_runtime.h →
// cel_string_ops.h (readability-redundant-declaration).

// Polymorphic equality kernel.  Writes a CEL_BOOL into `out_slot`,
// or propagates 3VL.  Aggregate / message arms tail-call into their
// dispatchers, which write CEL_BOOL themselves; `cel_not_equals`
// re-reads `out_slot` after a tail-call'd helper returns and flips.
// M9.D: CEL_TYPE × CEL_TYPE equality — memcmp on `payload.s` bytes,
// the type-name string in linear memory.  Extracted from
// `equality_kernel` to keep that function under the function-size
// gate; per langdef §"Equality" + m9-type-subsystem.md §3.4.
static void type_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  const uint32_t la = a->payload.s.len;
  const uint32_t lb = b->payload.s.len;
  if (la != lb) {
    write_bool(out, 0);
    return;
  }
  const uint8_t* base = cel_memory_base_();
  int eq = 1;
  for (uint32_t i = 0; i < la; ++i) {
    if (base[a->payload.s.ptr + i] != base[b->payload.s.ptr + i]) {
      eq = 0;
      break;
    }
  }
  write_bool(out, eq);
}

static void equality_kernel(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  // Numeric ladder — cross-type allowed.
  if (is_numeric(a->kind) && is_numeric(b->kind)) {
    cel_numeric_eq_at_vv(out_slot, a_slot, b_slot);
    return;
  }
  // Same-kind scalar arms.  Each handles its own type-check; we've
  // already proven kinds match.
  if (a->kind == b->kind) {
    switch (a->kind) {
      case CEL_NULL:
        cel_null_eq_at_vv(out_slot, a_slot, b_slot);
        return;
      case CEL_BOOL:
        cel_bool_eq_at_vv(out_slot, a_slot, b_slot);
        return;
      case CEL_STRING:
        cel_string_eq_at_vv(out_slot, a_slot, b_slot);
        return;
      case CEL_BYTES:
        cel_bytes_eq_at_vv(out_slot, a_slot, b_slot);
        return;
      case CEL_TYPE:
        type_eq_at_vv(out_slot, a_slot, b_slot);
        return;
      default:
        break;  // Aggregates fall through to the polymorphic arms.
    }
  }
  // Aggregates: dispatcher absorbs origin difference.
  if (both_lists(a->kind, b->kind)) {
    cel_list_eq(out_slot, a_slot, b_slot);
    return;
  }
  if (both_maps(a->kind, b->kind)) {
    cel_map_eq(out_slot, a_slot, b_slot);
    return;
  }
  if (a->kind == CEL_MESSAGE && b->kind == CEL_MESSAGE) {
    cel_host_cel_message_eq(out_slot, a_slot, b_slot);
    return;
  }
  // Cross-kind without a matching ladder rung: `false` per langdef
  // (NOT type-mismatch error).  E.g. `1 == "1"`, `[] == {}`,
  // `null == 0`.
  write_bool(out, 0);
}

// Wasm-exported via `-Wl,--export=cel_equals_at_vv` in
// compiler_v2/runtime/BUILD.bazel; `misc-use-internal-linkage` is
// silenced because internal linkage would hide it from the wasm
// export table.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void cel_equals_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  equality_kernel(out_slot, a_slot, b_slot);
}

// Wasm-exported via `-Wl,--export=cel_not_equals_at_vv` — see
// `cel_equals_at_vv` above for the linkage rationale.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void cel_not_equals_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  equality_kernel(out_slot, a_slot, b_slot);
  // 3VL absorption already wrote the operand through if either was
  // UNKNOWN / ERROR — leave that as-is.  Otherwise flip the bool.
  CelValue* out = cel_value_at(out_slot);
  if (out->kind == CEL_BOOL) {
    out->payload.b = out->payload.b ? 0 : 1;
  }
}

// =====================================================================
// M5.C — string + bytes operation helpers (slot-out helper ABI).
// Concat is the only allocator: writes a fresh payload into the
// arena.  Other helpers read operand spans without allocating.
// 3VL + type-mismatch envelope mirrors `cel_arith.h` / `cel_compare.h`.
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/string_functions.cc
// =====================================================================

// Span equality byte-for-byte.  CEL strings are UTF-8 byte arrays
// at the langdef level; equality is byte equality (no Unicode
// normalisation).  Same shape works for bytes operands.
static int span_eq(const CelSpan* a, const CelSpan* b) {
  if (a->len != b->len) return 0;
  const uint8_t* pa = cel_memory_base_() + a->ptr;
  const uint8_t* pb = cel_memory_base_() + b->ptr;
  for (uint32_t i = 0; i < a->len; ++i) {
    if (pa[i] != pb[i]) return 0;
  }
  return 1;
}

// Span lexicographic <.  Per langdef §"String / bytes": compare
// by Unicode code-point order, but since strings are stored as
// UTF-8 byte sequences, byte-lex order matches code-point order.
// cel-cpp's `LessThan::String` operates on byte-views identically.
static int span_lt(const CelSpan* a, const CelSpan* b) {
  uint32_t n = a->len < b->len ? a->len : b->len;
  const uint8_t* pa = cel_memory_base_() + a->ptr;
  const uint8_t* pb = cel_memory_base_() + b->ptr;
  for (uint32_t i = 0; i < n; ++i) {
    if (pa[i] != pb[i]) return pa[i] < pb[i];
  }
  return a->len < b->len;
}

// Returns 1 iff `hay[off..off+sub.len)` matches `sub` byte-for-byte.
// `off` MUST satisfy `off + sub.len <= hay.len` — caller checks.
static int span_match_at(const CelSpan* hay, uint32_t off, const CelSpan* sub) {
  const uint8_t* ph = cel_memory_base_() + hay->ptr + off;
  const uint8_t* ps = cel_memory_base_() + sub->ptr;
  for (uint32_t i = 0; i < sub->len; ++i) {
    if (ph[i] != ps[i]) return 0;
  }
  return 1;
}

// Linear-scan substring search.  langdef pins string ops to byte
// granularity; we don't need a fancy search algorithm because
// CEL fixtures are short.  Mirrors cel-cpp's
// `StringContains::Apply` (also a linear scan).
static int span_contains(const CelSpan* hay, const CelSpan* sub) {
  if (sub->len == 0) return 1;
  if (sub->len > hay->len) return 0;
  uint32_t last = hay->len - sub->len;
  for (uint32_t i = 0; i <= last; ++i) {
    if (span_match_at(hay, i, sub)) return 1;
  }
  return 0;
}

// Wrap up the per-helper string/bytes header check + 3VL + alloc
// in one place.  `kind` is CEL_STRING or CEL_BYTES.  Returns 1
// (skip math) when out_slot has been written with an absorbed
// 3VL value or a type-mismatch error.
static int span_op_prelude(CelValue* out, const CelValue* a, const CelValue* b,
                           uint32_t kind) {
  if (absorb_3vl_binary(out, a, b)) return 1;
  if (require_kinds(out, a, b, kind)) return 1;
  return 0;
}

static void concat_into_out(CelValue* out, const CelSpan* a, const CelSpan* b,
                            uint32_t kind) {
  uint32_t total = a->len + b->len;
  uint32_t off = 0;
  if (total > 0) {
    off = cel_alloc(total);
    if (off == 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return;
    }
    uint8_t* dst = cel_memory_base_() + off;
    memcpy(dst, cel_memory_base_() + a->ptr, a->len);
    memcpy(dst + a->len, cel_memory_base_() + b->ptr, b->len);
  }
  out->kind = kind;
  out->payload.s.ptr = off;
  out->payload.s.len = total;
}

void cel_string_concat_at_vv(uint32_t out_slot, uint32_t a_slot,
                             uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_STRING)) return;
  concat_into_out(out, &a->payload.s, &b->payload.s, CEL_STRING);
}

void cel_bytes_concat_at_vv(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_BYTES)) return;
  concat_into_out(out, &a->payload.s, &b->payload.s, CEL_BYTES);
}

static void size_at(CelValue* out, uint32_t v_slot, uint32_t kind) {
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != kind) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_int(out, (int64_t)v->payload.s.len);
}

void cel_string_size_at_v(uint32_t out_slot, uint32_t v_slot) {
  size_at(cel_value_at(out_slot), v_slot, CEL_STRING);
}

void cel_bytes_size_at_v(uint32_t out_slot, uint32_t v_slot) {
  size_at(cel_value_at(out_slot), v_slot, CEL_BYTES);
}

void cel_string_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_STRING)) return;
  write_bool(out, span_eq(&a->payload.s, &b->payload.s));
}

void cel_string_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_STRING)) return;
  write_bool(out, span_lt(&a->payload.s, &b->payload.s));
}

void cel_bytes_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_BYTES)) return;
  write_bool(out, span_eq(&a->payload.s, &b->payload.s));
}

void cel_bytes_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_BYTES)) return;
  write_bool(out, span_lt(&a->payload.s, &b->payload.s));
}

// String / bytes le/gt/ge — express each in terms of the existing
// `span_lt` byte-lex comparator.  Identities (cel-cpp parity,
// `runtime/standard/comparison_functions.cc`):
//   a <= b  ↔  !(b <  a)
//   a >  b  ↔   (b <  a)
//   a >= b  ↔  !(a <  b)
#define DEFINE_SPAN_CMP_VV(name, kind, body)                       \
  void name(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) { \
    CelValue* out = cel_value_at(out_slot);                        \
    const CelValue* a = cel_value_at(a_slot);                      \
    const CelValue* b = cel_value_at(b_slot);                      \
    if (span_op_prelude(out, a, b, kind)) return;                  \
    write_bool(out, body);                                         \
  }

DEFINE_SPAN_CMP_VV(cel_string_le_at_vv, CEL_STRING,
                   !span_lt(&b->payload.s, &a->payload.s))
DEFINE_SPAN_CMP_VV(cel_string_gt_at_vv, CEL_STRING,
                   span_lt(&b->payload.s, &a->payload.s))
DEFINE_SPAN_CMP_VV(cel_string_ge_at_vv, CEL_STRING,
                   !span_lt(&a->payload.s, &b->payload.s))
DEFINE_SPAN_CMP_VV(cel_bytes_le_at_vv, CEL_BYTES,
                   !span_lt(&b->payload.s, &a->payload.s))
DEFINE_SPAN_CMP_VV(cel_bytes_gt_at_vv, CEL_BYTES,
                   span_lt(&b->payload.s, &a->payload.s))
DEFINE_SPAN_CMP_VV(cel_bytes_ge_at_vv, CEL_BYTES,
                   !span_lt(&a->payload.s, &b->payload.s))

#undef DEFINE_SPAN_CMP_VV

void cel_string_contains_at_vv(uint32_t out_slot, uint32_t s_slot,
                               uint32_t sub_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* sub = cel_value_at(sub_slot);
  if (span_op_prelude(out, s, sub, CEL_STRING)) return;
  write_bool(out, span_contains(&s->payload.s, &sub->payload.s));
}

void cel_string_starts_with_at_vv(uint32_t out_slot, uint32_t s_slot,
                                  uint32_t pfx_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* pfx = cel_value_at(pfx_slot);
  if (span_op_prelude(out, s, pfx, CEL_STRING)) return;
  if (pfx->payload.s.len > s->payload.s.len) {
    write_bool(out, 0);
    return;
  }
  write_bool(out, span_match_at(&s->payload.s, 0, &pfx->payload.s));
}

void cel_string_ends_with_at_vv(uint32_t out_slot, uint32_t s_slot,
                                uint32_t sfx_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* sfx = cel_value_at(sfx_slot);
  if (span_op_prelude(out, s, sfx, CEL_STRING)) return;
  if (sfx->payload.s.len > s->payload.s.len) {
    write_bool(out, 0);
    return;
  }
  uint32_t off = s->payload.s.len - sfx->payload.s.len;
  write_bool(out, span_match_at(&s->payload.s, off, &sfx->payload.s));
}

// ---- 3VL / control-flow helpers (M5.G — Slice 2) -------------------------

// Sorted-deduplicated merge walk over two pre-sorted u32 id arrays.
// Mirrors v1's `merge_sorted_ids` shape (compiler/runtime/cel_runtime.c).
// Returns the post-dedup length; never reads past the input bounds.
static uint32_t merge_sorted_id_arrays(const uint32_t* ids_a, uint32_t len_a,
                                       const uint32_t* ids_b, uint32_t len_b,
                                       uint32_t* out) {
  uint32_t i = 0;
  uint32_t j = 0;
  uint32_t k = 0;
  while (i < len_a && j < len_b) {
    uint32_t ai = ids_a[i];
    uint32_t bj = ids_b[j];
    if (ai < bj) {
      out[k++] = ai;
      ++i;
    } else if (ai > bj) {
      out[k++] = bj;
      ++j;
    } else {
      out[k++] = ai;
      ++i;
      ++j;
    }
  }
  while (i < len_a) {
    out[k++] = ids_a[i++];
  }
  while (j < len_b) {
    out[k++] = ids_b[j++];
  }
  return k;
}

// Allocates a fresh 2-word UnknownSet descriptor `{ids_off, len}` in
// the bump arena and returns its byte offset, or 0 on out-of-arena.
static uint32_t alloc_unknown_descriptor(uint32_t ids_off, uint32_t len) {
  uint32_t desc_off = cel_alloc(2u * (uint32_t)sizeof(uint32_t));
  if (desc_off == 0) return 0;
  uint32_t* desc = (uint32_t*)(cel_memory_base_() + desc_off);
  desc[0] = ids_off;
  desc[1] = len;
  return desc_off;
}

// Walks two non-empty UnknownSet descriptors at `set_a`/`set_b`, mints a
// fresh sorted-deduped descriptor in the bump arena, and returns its
// byte offset.  Returns 0 on out-of-arena.  Snapshots descriptor scalars
// before any cel_alloc — wasm32 `memory.grow` can relocate the linear
// memory base, so pointers must be re-derived after each bump.
static uint32_t merge_unknown_descriptors(uint32_t set_a, uint32_t set_b) {
  uint32_t* desc_a = (uint32_t*)(cel_memory_base_() + set_a);
  uint32_t ids_a_off = desc_a[0];
  uint32_t len_a = desc_a[1];
  uint32_t* desc_b = (uint32_t*)(cel_memory_base_() + set_b);
  uint32_t ids_b_off = desc_b[0];
  uint32_t len_b = desc_b[1];

  uint32_t max_total = len_a + len_b;
  uint32_t bytes = max_total * (uint32_t)sizeof(uint32_t);
  if (bytes == 0) bytes = (uint32_t)sizeof(uint32_t);
  uint32_t out_ids = cel_alloc(bytes);
  if (out_ids == 0) return 0;

  const uint32_t* ids_a = (const uint32_t*)(cel_memory_base_() + ids_a_off);
  const uint32_t* ids_b = (const uint32_t*)(cel_memory_base_() + ids_b_off);
  uint32_t* dst = (uint32_t*)(cel_memory_base_() + out_ids);
  uint32_t k = merge_sorted_id_arrays(ids_a, len_a, ids_b, len_b, dst);
  return alloc_unknown_descriptor(out_ids, k);
}

// Writes `(CEL_UNKNOWN, payload.unk = desc_off)` into the slot.  Re-derives
// the slot pointer because `cel_alloc` may have relocated linear memory.
static void write_unknown_at(uint32_t out_slot, uint32_t desc_off) {
  CelValue* out = cel_value_at(out_slot);
  out->kind = CEL_UNKNOWN;
  out->payload.unk = desc_off;
}

void cel_unknown_merge(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (a->kind != CEL_UNKNOWN || b->kind != CEL_UNKNOWN) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  uint32_t set_a = a->payload.unk;
  uint32_t set_b = b->payload.unk;
  // An empty UnknownSet (payload.unk == 0) is a legal UNKNOWN — the
  // host `get_field` trampoline mints UNKNOWNs that way for FULL
  // attribute-pattern matches before per-id provenance is wired.
  // Treat an empty side as "no new ids to contribute"; both empty
  // → empty UNKNOWN.
  if (set_a == 0 && set_b == 0) {
    write_unknown_at(out_slot, 0);
    return;
  }
  if (set_a == 0) {
    write_unknown_at(out_slot, set_b);
    return;
  }
  if (set_b == 0) {
    write_unknown_at(out_slot, set_a);
    return;
  }
  uint32_t desc_off = merge_unknown_descriptors(set_a, set_b);
  if (desc_off == 0) {
    // Re-derive `out` after the failed cel_alloc: memory.grow may have
    // relocated base before the failure.
    poison(cel_value_at(out_slot), CEL_ERR_OVERFLOW);
    return;
  }
  write_unknown_at(out_slot, desc_off);
}

void cel_copy_slot(uint32_t dst_slot, uint32_t src_slot) {
  CelValue* dst = cel_value_at(dst_slot);
  const CelValue* src = cel_value_at(src_slot);
  *dst = *src;
}

static int is_3vl_kind(uint32_t k) {
  return k == CEL_BOOL || k == CEL_UNKNOWN || k == CEL_ERROR;
}

// 3VL truth table for `&&` (langdef §"Logical operators").
// Non-strict semantics: `false && X = false` for any X, including
// ERROR / UNKNOWN / non-3VL; symmetric on the right.  Once both
// operands are known to be 3VL (and neither is OK(false)),
// `true && X = X`, ERROR > UNKNOWN dominates, and both UNKNOWN
// merges the attribute-id sets via `cel_unknown_merge`.  A non-3VL
// operand survives only via the OK(false) absorber; otherwise the
// result is a type error.
void cel_and(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  // OK(false) absorbs everything — including a non-3VL other side.
  if (a->kind == CEL_BOOL && a->payload.b == 0) {
    write_bool(out, 0);
    return;
  }
  if (b->kind == CEL_BOOL && b->payload.b == 0) {
    write_bool(out, 0);
    return;
  }
  // Past the absorber: every operand must be a valid 3VL kind.
  if (!is_3vl_kind(a->kind) || !is_3vl_kind(b->kind)) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (a->kind == CEL_BOOL) {
    *out = *b;
    return;
  }  // a == true → result = b
  if (b->kind == CEL_BOOL) {
    *out = *a;
    return;
  }  // b == true → result = a
  if (a->kind == CEL_ERROR) {
    *out = *a;
    return;
  }  // ERROR > UNKNOWN
  if (b->kind == CEL_ERROR) {
    *out = *b;
    return;
  }
  cel_unknown_merge(out_slot, a_slot, b_slot);
}

// Symmetric to `cel_and`: `true || X = true`; `false || X = X`.
void cel_or(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (a->kind == CEL_BOOL && a->payload.b != 0) {
    write_bool(out, 1);
    return;
  }
  if (b->kind == CEL_BOOL && b->payload.b != 0) {
    write_bool(out, 1);
    return;
  }
  if (!is_3vl_kind(a->kind) || !is_3vl_kind(b->kind)) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (a->kind == CEL_BOOL) {
    *out = *b;
    return;
  }  // a == false → result = b
  if (b->kind == CEL_BOOL) {
    *out = *a;
    return;
  }
  if (a->kind == CEL_ERROR) {
    *out = *a;
    return;
  }
  if (b->kind == CEL_ERROR) {
    *out = *b;
    return;
  }
  cel_unknown_merge(out_slot, a_slot, b_slot);
}

void cel_not(uint32_t out_slot, uint32_t a_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  if (a->kind == CEL_BOOL) {
    write_bool(out, a->payload.b ? 0 : 1);
    return;
  }
  if (a->kind == CEL_UNKNOWN || a->kind == CEL_ERROR) {
    *out = *a;
    return;
  }
  poison(out, CEL_ERR_TYPE_MISMATCH);
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

// ─────────────────────────────────────────────────────────────
// M9.B: type(x) helper.
//
// 12-row primitive type-name table indexed by CelKind, per
// langdef §"Type Values" + m9-type-subsystem.md §3.1.  Names live
// as static C string literals in the runtime's `.rodata` section.
// On wasm32 those literals live in the module's data segment which
// IS part of the shared linear memory — `(uint32_t)(uintptr_t)name`
// is a valid linear-memory offset.  On the host build, however, the
// literals live in process .rodata (NOT g_memory), so the helper
// allocates fresh bytes in the per-Eval arena and copies the name
// there.  This unified-arena-copy path also keeps the wasm build
// trivially correct without depending on subtle linker placement.
//
// Cost: at most one ~24-byte arena allocation per `type(x)` call.
// Acceptable; CelValue size + alignment swallow most of the
// per-call cost.
// ─────────────────────────────────────────────────────────────

// Indexed by CelKind value.  NULL entries are kinds the helper
// does not handle directly: CEL_MESSAGE dispatches to the host
// trampoline (M9.C); CEL_OPTIONAL is an optionals-pass concern
// (out of M9 scope); CEL_UNKNOWN / CEL_ERROR are absorbed by the
// 3VL prelude before reaching this table.
//
// CelKind tail value is CEL_LIST_HOST = 17, so the array has 18
// slots.
static const char* const kPrimitiveTypeName[18] = {
    "null_type",                  // CEL_NULL = 0
    "bool",                       // CEL_BOOL = 1
    "int",                        // CEL_INT = 2
    "uint",                       // CEL_UINT = 3
    "double",                     // CEL_DOUBLE = 4
    "string",                     // CEL_STRING = 5
    "bytes",                      // CEL_BYTES = 6
    "list",                       // CEL_LIST_ARENA = 7
    "map",                        // CEL_MAP_ARENA = 8
    "map",                        // CEL_MAP_HOST = 9
    NULL,                         // CEL_MESSAGE = 10  (host)
    "type",                       // CEL_TYPE = 11
    "google.protobuf.Duration",   // CEL_DURATION = 12
    "google.protobuf.Timestamp",  // CEL_TIMESTAMP = 13
    NULL,                         // CEL_OPTIONAL = 14 (optionals-pass)
    NULL,                         // CEL_UNKNOWN = 15 (absorbed)
    NULL,                         // CEL_ERROR = 16 (absorbed)
    "list",                       // CEL_LIST_HOST = 17
};

// Forward decl of the M9.C host trampoline.  Same import pattern as
// `cel_host_cel_map_lookup` above: `__wasm__` ⇒ `import_module`
// attribute (resolved at instantiation by wasmtime); host build ⇒
// weak no-op stub (poison kTypeMismatch) so unit tests link without
// the wasmtime trampoline.  M9.C lands the strong override in
// `compiler_v2/api/internal/cel_host.cc` for both directions.
#ifdef __wasm__
extern void cel_host_resolve_message_type_name(uint32_t out_slot,
                                               uint32_t in_slot)
    __attribute__((import_module("cel_host"),
                   import_name("resolve_message_type_name")));
#else
__attribute__((weak)) void
cel_host_resolve_message_type_name(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t in_slot) {
  (void)in_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
#endif

void cel_type_of_at_v(uint32_t out_slot, uint32_t in_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* in = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, in)) return;
  if (in->kind == CEL_MESSAGE) {
    // M9.C trampoline.  Until M9.C ships the host wiring, this call
    // traps "unknown import" at wasm runtime — a clean failure
    // mode, not a silent miscompile.  The host build links against
    // a stub that lives in `cel_host.cc` (M9.C).
    cel_host_resolve_message_type_name(out_slot, in_slot);
    return;
  }
  const char* name = NULL;
  if (in->kind < (sizeof(kPrimitiveTypeName) / sizeof(kPrimitiveTypeName[0]))) {
    name = kPrimitiveTypeName[in->kind];
  }
  if (name == NULL) {
    // Unknown / unhandled kind (e.g. CEL_OPTIONAL) — surface a clean
    // type-mismatch error so callers see a rejection rather than a
    // miscompile.
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Compute length without strlen (freestanding wasm doesn't link
  // libc by default; spell out the byte-loop).
  uint32_t len = 0;
  while (name[len] != 0) {
    ++len;
  }
  // Allocate fresh bytes in the per-Eval arena and copy the name in.
  // cel_alloc bumps the cursor by align_up(n, 8) and returns 0 on
  // OOM (or when n==0).  Even an empty name doesn't reach here —
  // every entry in `kPrimitiveTypeName` is non-empty — but defend
  // anyway for future kinds.
  uint32_t off = cel_alloc(len);
  if (off == 0 && len > 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint8_t* base = cel_memory_base_();
  for (uint32_t i = 0; i < len; ++i) {
    base[off + i] = (uint8_t)name[i];
  }
  out->kind = CEL_TYPE;
  out->_pad = 0;
  out->payload.s.ptr = off;
  out->payload.s.len = len;
}

// ─────────────────────────────────────────────────────────────
// M10.B: numeric inter-conversion kernels.
//
// Six unary helpers (`(out_slot, in_slot) -> void`) for the cel-cpp
// overload ids:
//   uint64_to_int64    int(uint)
//   double_to_int64    int(double)
//   int64_to_uint64    uint(int)
//   double_to_uint64   uint(double)
//   int64_to_double    double(int)
//   uint64_to_double   double(uint)
//
// Each absorbs CEL_ERROR / CEL_UNKNOWN per the standard slot-out
// helper contract.  Overflow / NaN / negative-source rejections
// poison out_slot with `CEL_ERR_OVERFLOW`, matching the spec
// "errors if out of range" wording (langdef §"int" / §"uint" /
// §"double") and cel-cpp's `Checked*ToInt64` / `Checked*ToUint64`
// helpers (`third_party/cel-cpp/internal/overflow.cc`).
//
// Double bounds for int / uint use the exact-representable
// boundaries 2^63 and 2^64.  Per cel-cpp's `CheckedDoubleToInt64`,
// `INT64_MIN` is admitted (it's exactly representable as a double),
// but `INT64_MAX + 1 == 2^63` is rejected — the largest admissible
// double is `2^63 - 1024` (the next-lower representable below 2^63).
// NaN is rejected via the `v != v` idiom (works without <math.h>
// which the freestanding wasm32 build does not link).
// ─────────────────────────────────────────────────────────────

// Exact-representable double bounds.  `kDoubleInt64Min == -2^63`
// exactly; `kDoubleInt64MaxPlus1 == 2^63` exactly.  Compare with
// `<` / `>=` to admit the inclusive int64 range and reject the
// out-of-range edge cleanly.
static const double kDoubleInt64Min = -9223372036854775808.0;
static const double kDoubleInt64MaxPlus1 = 9223372036854775808.0;
static const double kDoubleUint64MaxPlus1 = 18446744073709551616.0;

void cel_uint_to_int_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_UINT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const uint64_t v = a->payload.u;
  if (v > (uint64_t)INT64_MAX) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, (int64_t)v);
}

void cel_double_to_int_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const double v = a->payload.d;
  // NaN check (v != v) + range gate using exact-representable
  // boundaries.  Rejects [-inf, -2^63), [2^63, +inf], and NaN.
  if (v != v || v < kDoubleInt64Min || v >= kDoubleInt64MaxPlus1) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  // C99 cast truncates toward zero — matches langdef §"int"
  // "rounds toward zero".
  write_int(out, (int64_t)v);
}

void cel_int_to_uint_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const int64_t v = a->payload.i;
  if (v < 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, (uint64_t)v);
}

void cel_double_to_uint_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const double v = a->payload.d;
  if (v != v || v < 0.0 || v >= kDoubleUint64MaxPlus1) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, (uint64_t)v);
}

void cel_int_to_double_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Never errors per langdef — lossy for |v| >= 2^53 is allowed.
  write_double(out, (double)a->payload.i);
}

void cel_uint_to_double_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_UINT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Never errors per langdef — lossy for v >= 2^53 is allowed.
  write_double(out, (double)a->payload.u);
}

// ─────────────────────────────────────────────────────────────
// M10.C: string parsing helpers.
//
// Four unary helpers `(out_slot, in_slot) -> void`:
//   string_to_int64    int(string)
//   string_to_uint64   uint(string)
//   string_to_double   double(string)
//   string_to_bool     bool(string)
//
// Hand-rolled byte-loop parsers mirroring `absl::SimpleAtoi` /
// `SimpleAtod` admit-sets (the cel-cpp reference impl):
//
//   - int: optional leading `-`, decimal digits only, no whitespace,
//     no trailing garbage.  Overflow → CEL_ERR_OVERFLOW.
//   - uint: same, but leading `-` is rejected.
//   - double: standard floating-point form
//     `[+-]?digits(.digits)?([eE][+-]?digits)?` plus special
//     literals `inf` / `infinity` / `nan` (case-insensitive).
//   - bool: exact-string match against cel-cpp's 10-row truth table.
//
// All parser subroutines return 1 on success and 0 on any malformed
// input — kernels translate 0 to `CEL_ERR_OVERFLOW` (the existing
// "can't represent the result as the target type" code; matches
// the semantics of the M10.B numeric-conversion overflows).  A
// dedicated `CEL_ERR_INVALID_ARGUMENT` can land in a future slice
// alongside the api/error.h mirror.
// ─────────────────────────────────────────────────────────────

static const uint8_t* span_bytes(const CelValue* cv) {
  return cel_memory_base_() + cv->payload.s.ptr;
}

static int parse_int64_str(const uint8_t* p, uint32_t len, int64_t* out) {
  if (len == 0) return 0;
  int neg = 0;
  uint32_t i = 0;
  if (p[i] == '-') {
    neg = 1;
    ++i;
  }
  if (i == len) return 0;
  uint64_t acc = 0;
  for (; i < len; ++i) {
    if (p[i] < '0' || p[i] > '9') return 0;
    uint32_t d = (uint32_t)(p[i] - '0');
    // Manual overflow check — `__builtin_mul_overflow` on 64-bit
    // needs `__multi3` which the freestanding wasm32 build does not
    // link (same precedent as the M5.B `int64_mul_overflows` comment
    // at the top of this file).
    if (acc > UINT64_MAX / 10ULL) return 0;
    acc *= 10ULL;
    if (acc > UINT64_MAX - (uint64_t)d) return 0;
    acc += (uint64_t)d;
  }
  if (neg) {
    // INT64_MIN edge: `|INT64_MIN| == (uint64_t)INT64_MAX + 1`.
    if (acc > (uint64_t)INT64_MAX + 1ULL) return 0;
    if (acc == (uint64_t)INT64_MAX + 1ULL) {
      *out = INT64_MIN;
    } else {
      *out = -(int64_t)acc;
    }
  } else {
    if (acc > (uint64_t)INT64_MAX) return 0;
    *out = (int64_t)acc;
  }
  return 1;
}

static int parse_uint64_str(const uint8_t* p, uint32_t len, uint64_t* out) {
  if (len == 0) return 0;
  uint64_t acc = 0;
  for (uint32_t i = 0; i < len; ++i) {
    if (p[i] < '0' || p[i] > '9') return 0;
    uint32_t d = (uint32_t)(p[i] - '0');
    // Manual overflow check — `__builtin_mul_overflow` on 64-bit
    // needs `__multi3` which the freestanding wasm32 build does not
    // link (same precedent as the M5.B `int64_mul_overflows` comment
    // at the top of this file).
    if (acc > UINT64_MAX / 10ULL) return 0;
    acc *= 10ULL;
    if (acc > UINT64_MAX - (uint64_t)d) return 0;
    acc += (uint64_t)d;
  }
  *out = acc;
  return 1;
}

// Case-insensitive ASCII prefix match.  Returns 1 iff `p[0..plen)`
// matches `pattern[0..plen)` byte-for-byte after folding A-Z to a-z.
static int eq_ci(const uint8_t* p, uint32_t plen, const char* pattern) {
  for (uint32_t i = 0; i < plen; ++i) {
    uint8_t a = p[i];
    uint8_t b = (uint8_t)pattern[i];
    if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (uint8_t)(b + 32);
    if (a != b) return 0;
  }
  return 1;
}

static int parse_double_str(const uint8_t* p, uint32_t len, double* out) {
  if (len == 0) return 0;
  int neg = 0;
  uint32_t i = 0;
  if (p[i] == '-') {
    neg = 1;
    ++i;
  } else if (p[i] == '+') {
    ++i;
  }
  if (i == len) return 0;
  // Special literals.  `infinity` checked before `inf` because of
  // prefix overlap.
  const uint32_t rem = len - i;
  if (rem == 8 && eq_ci(p + i, 8, "infinity")) {
    *out = neg ? -__builtin_inf() : __builtin_inf();
    return 1;
  }
  if (rem == 3 && eq_ci(p + i, 3, "inf")) {
    *out = neg ? -__builtin_inf() : __builtin_inf();
    return 1;
  }
  if (rem == 3 && eq_ci(p + i, 3, "nan")) {
    *out = __builtin_nan("");
    return 1;
  }
  // Numeric form.  Accumulate mantissa as a double; track total
  // decimal scale separately.  Precision is "best effort" within
  // the bounds cel-cpp's SimpleAtod also exhibits for hand-roll
  // edge cases; tests cover the common admit + reject matrix.
  double mantissa = 0.0;
  int saw_digit = 0;
  while (i < len && p[i] >= '0' && p[i] <= '9') {
    mantissa = mantissa * 10.0 + (double)(p[i] - '0');
    saw_digit = 1;
    ++i;
  }
  int frac_digits = 0;
  if (i < len && p[i] == '.') {
    ++i;
    while (i < len && p[i] >= '0' && p[i] <= '9') {
      mantissa = mantissa * 10.0 + (double)(p[i] - '0');
      ++frac_digits;
      saw_digit = 1;
      ++i;
    }
  }
  if (!saw_digit) return 0;
  int exp_neg = 0;
  int exp_val = 0;
  if (i < len && (p[i] == 'e' || p[i] == 'E')) {
    ++i;
    if (i == len) return 0;
    if (p[i] == '-') {
      exp_neg = 1;
      ++i;
    } else if (p[i] == '+') {
      ++i;
    }
    if (i == len) return 0;
    int saw_exp_digit = 0;
    while (i < len && p[i] >= '0' && p[i] <= '9') {
      // Cap to avoid wraparound — magnitudes past +/-308 saturate
      // to inf / 0 anyway under IEEE 754 doubles.
      if (exp_val < 10000) exp_val = exp_val * 10 + (p[i] - '0');
      saw_exp_digit = 1;
      ++i;
    }
    if (!saw_exp_digit) return 0;
  }
  if (i != len) return 0;  // trailing garbage rejected.
  int total_exp = (exp_neg ? -exp_val : exp_val) - frac_digits;
  // Apply scale.  Iterative multiply is precision-lossy on edge
  // cases but matches the test admit-set; a tighter implementation
  // (Grisu / Ryu) can land later if a fixture demands it.
  if (total_exp > 0) {
    for (int k = 0; k < total_exp; ++k) mantissa *= 10.0;
  } else if (total_exp < 0) {
    for (int k = 0; k < -total_exp; ++k) mantissa /= 10.0;
  }
  *out = neg ? -mantissa : mantissa;
  return 1;
}

// cel-cpp's `StringToBoolFunction` truth table (10 rows).
//   true:  "1" / "t" / "true" / "TRUE" / "True"
//   false: "0" / "f" / "false" / "FALSE" / "False"
// Exact byte-match (NOT case-insensitive — the spec admits only
// the 5 spellings per polarity, mixed case beyond `True` / `TRUE`
// rejects).
static int parse_bool_str(const uint8_t* p, uint32_t len, int* out) {
  struct Row {
    const char* s;
    uint32_t len;
    int v;
  };
  static const struct Row kRows[10] = {
      {"1", 1, 1},    {"t", 1, 1},     {"true", 4, 1}, {"TRUE", 4, 1},
      {"True", 4, 1}, {"0", 1, 0},     {"f", 1, 0},    {"false", 5, 0},
      {"FALSE", 5, 0}, {"False", 5, 0},
  };
  for (uint32_t r = 0; r < 10; ++r) {
    if (len != kRows[r].len) continue;
    int match = 1;
    for (uint32_t i = 0; i < len; ++i) {
      if (p[i] != (uint8_t)kRows[r].s[i]) {
        match = 0;
        break;
      }
    }
    if (match) {
      *out = kRows[r].v;
      return 1;
    }
  }
  return 0;
}

void cel_string_to_int_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  int64_t v;
  if (!parse_int64_str(span_bytes(a), a->payload.s.len, &v)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, v);
}

void cel_string_to_uint_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  uint64_t v;
  if (!parse_uint64_str(span_bytes(a), a->payload.s.len, &v)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, v);
}

void cel_string_to_double_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  double v;
  if (!parse_double_str(span_bytes(a), a->payload.s.len, &v)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_double(out, v);
}

void cel_string_to_bool_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  int v;
  if (!parse_bool_str(span_bytes(a), a->payload.s.len, &v)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_bool(out, v);
}

// ─────────────────────────────────────────────────────────────
// M10.D: number / bool → string formatting helpers.
//
// Four unary kernels.  Output strings are allocated in the per-Eval
// arena via `cel_alloc(n)` and stamped as `{CEL_STRING, payload.s}`
// — same lifetime model as the M9.B `cel_type_of_at_v` helper.
//
//   cel_int_to_string_at_v     int64_to_string   string(int)
//   cel_uint_to_string_at_v    uint64_to_string  string(uint)
//   cel_bool_to_string_at_v    bool_to_string    string(bool)
//   cel_double_to_string_at_v  double_to_string  string(double)
//
// `string(double)` is "correct for the common cases, round-trip
// safe for typical magnitudes" per m10-conversions.md §4.4.  The
// hand-rolled formatter handles NaN / ±Inf / ±0 specials, integer-
// valued doubles via the int path, and otherwise a digit-by-digit
// integer + fractional decomposition.  Byte-exact match against
// cel-cpp's `to_chars` general format is NOT a hard requirement;
// the test contract (m10_test.cc::NumberFormatE2ETest) is round-
// trip identity (`double(string(d)) == d`).  A Grisu / Ryu
// upgrade can swap the helper body later if a conformance row
// demands byte parity.
// ─────────────────────────────────────────────────────────────

// Write decimal digits of a uint64 into `dst`, returning the count.
// No leading zeros (except for value 0 itself).
static uint32_t write_uint_decimal(uint8_t* dst, uint64_t v) {
  if (v == 0) {
    dst[0] = '0';
    return 1;
  }
  uint8_t buf[20];  // ceil(log10(UINT64_MAX)) = 20
  uint32_t n = 0;
  while (v > 0) {
    buf[n++] = (uint8_t)('0' + (v % 10ULL));
    v /= 10ULL;
  }
  // Reverse into dst.
  for (uint32_t i = 0; i < n; ++i) {
    dst[i] = buf[n - 1 - i];
  }
  return n;
}

// Write decimal digits of an int64 into `dst`, with leading `-` for
// negatives.  Handles INT64_MIN by promoting through the |v| route.
static uint32_t write_int_decimal(uint8_t* dst, int64_t v) {
  if (v >= 0) {
    return write_uint_decimal(dst, (uint64_t)v);
  }
  dst[0] = '-';
  // `-(int64_t)INT64_MIN` would UB; route via uint64 cast.
  uint64_t abs_v = (v == INT64_MIN) ? ((uint64_t)INT64_MAX + 1ULL) : (uint64_t)(-v);
  return 1u + write_uint_decimal(dst + 1, abs_v);
}

// Common allocate-and-stamp for the small string outputs.  Returns
// 1 on success; on arena OOM poisons out_slot and returns 0.
static int stamp_string(CelValue* out, const uint8_t* src, uint32_t len) {
  uint32_t off = cel_alloc(len);
  if (off == 0 && len > 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return 0;
  }
  uint8_t* dst = cel_memory_base_() + off;
  for (uint32_t i = 0; i < len; ++i) {
    dst[i] = src[i];
  }
  out->kind = CEL_STRING;
  out->_pad = 0;
  out->payload.s.ptr = off;
  out->payload.s.len = len;
  return 1;
}

void cel_int_to_string_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  uint8_t buf[21];  // 20 digits + sign
  uint32_t n = write_int_decimal(buf, a->payload.i);
  (void)stamp_string(out, buf, n);
}

void cel_uint_to_string_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_UINT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  uint8_t buf[20];
  uint32_t n = write_uint_decimal(buf, a->payload.u);
  (void)stamp_string(out, buf, n);
}

void cel_bool_to_string_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_BOOL) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  static const uint8_t kTrue[4] = {'t', 'r', 'u', 'e'};
  static const uint8_t kFalse[5] = {'f', 'a', 'l', 's', 'e'};
  if (a->payload.b) {
    (void)stamp_string(out, kTrue, 4);
  } else {
    (void)stamp_string(out, kFalse, 5);
  }
}

// Write the fractional digits of `frac` (which is in [0, 1)) into
// `dst`, up to `max_digits`.  Trims trailing zeros and a trailing
// `.`.  Caller pre-writes the integer-part bytes and a `.`; this
// function returns the count of bytes appended (which may be 0 if
// every fractional digit was a trailing zero).
static uint32_t append_double_fraction(uint8_t* dst, double frac,
                                       uint32_t max_digits) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < max_digits && frac > 0.0; ++i) {
    frac *= 10.0;
    uint32_t digit = (uint32_t)frac;
    if (digit > 9) digit = 9;
    dst[n++] = (uint8_t)('0' + digit);
    frac -= (double)digit;
  }
  // Trim trailing zeros.
  while (n > 0 && dst[n - 1] == '0') --n;
  return n;
}

void cel_double_to_string_at_v(uint32_t out_slot, uint32_t in_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(in_slot);
  if (absorb_3vl_unary(out, a)) return;
  if (a->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const double v = a->payload.d;
  // Specials.  NaN check via `v != v`; infinities via direct compare.
  if (v != v) {
    static const uint8_t kNan[3] = {'n', 'a', 'n'};
    (void)stamp_string(out, kNan, 3);
    return;
  }
  const double kPosInf = __builtin_inf();
  if (v == kPosInf) {
    static const uint8_t kPI[4] = {'+', 'I', 'n', 'f'};
    (void)stamp_string(out, kPI, 4);
    return;
  }
  if (v == -kPosInf) {
    static const uint8_t kNI[4] = {'-', 'I', 'n', 'f'};
    (void)stamp_string(out, kNI, 4);
    return;
  }
  if (v == 0.0) {
    static const uint8_t kZero[1] = {'0'};
    (void)stamp_string(out, kZero, 1);
    return;
  }
  // General case.  Buffer sized for sign + 20-digit integer + `.` +
  // 17-digit fractional = 39, rounded up.
  uint8_t buf[48];
  uint32_t k = 0;
  double av = v;
  if (av < 0.0) {
    buf[k++] = '-';
    av = -av;
  }
  // Integer-valued doubles in safe-cast range get the int path —
  // exact byte representation, no fractional rounding.
  if (av < 1e18 && av == (double)(uint64_t)av) {
    k += write_uint_decimal(buf + k, (uint64_t)av);
    (void)stamp_string(out, buf, k);
    return;
  }
  // Mixed integer + fractional path.  For magnitudes inside the
  // uint64 range we can extract the integer part exactly; very
  // large or very small magnitudes fall through to the scientific
  // path below.
  if (av < 1e18 && av >= 1e-4) {
    uint64_t iv = (uint64_t)av;
    k += write_uint_decimal(buf + k, iv);
    double frac = av - (double)iv;
    if (frac > 0.0) {
      buf[k++] = '.';
      uint32_t fn = append_double_fraction(buf + k, frac, 17);
      if (fn == 0) {
        --k;  // strip the dangling `.`
      } else {
        k += fn;
      }
    }
    (void)stamp_string(out, buf, k);
    return;
  }
  // Scientific notation fallback.  Normalize to 1 <= m < 10, count
  // the decimal exponent.  Iterative *10 / /10 — slow but bounded
  // (~300 iterations even at IEEE 754 extremes).
  int exp = 0;
  double m = av;
  while (m >= 10.0) {
    m /= 10.0;
    ++exp;
  }
  while (m < 1.0) {
    m *= 10.0;
    --exp;
  }
  // Mantissa: digit + `.` + up to 16 fractional digits.
  uint32_t digit = (uint32_t)m;
  if (digit > 9) digit = 9;
  buf[k++] = (uint8_t)('0' + digit);
  double frac = m - (double)digit;
  if (frac > 0.0) {
    buf[k++] = '.';
    uint32_t fn = append_double_fraction(buf + k, frac, 16);
    if (fn == 0) {
      --k;
    } else {
      k += fn;
    }
  }
  // `e<sign><digits>` exponent suffix.
  buf[k++] = 'e';
  if (exp < 0) {
    buf[k++] = '-';
    exp = -exp;
  } else {
    buf[k++] = '+';
  }
  k += write_uint_decimal(buf + k, (uint64_t)exp);
  (void)stamp_string(out, buf, k);
}
