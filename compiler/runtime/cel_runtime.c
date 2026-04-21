#include "compiler/runtime/cel_runtime.h"

// The host build has a real libc; the wasm32 cross-compile build is
// freestanding and does not.  `<string.h>` is one of the hosted-only
// headers, so for wasm we declare the two functions we actually use
// and let clang lower them to LLVM intrinsics (wasm-ld resolves the
// intrinsics to compiler-rt's own `memcpy` / `memset`).
#ifdef __wasm__
// Freestanding wasm32 build: no libc.  Provide trivial byte-loop
// implementations of the three `<string.h>` functions we use.  They
// are not perf-critical (runtime calls are per-CelValue, not per
// byte) and keeping them in-tree avoids pulling in compiler-rt.
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
static int memcmp(const void* a, const void* b, size_t n) {
  const unsigned char* x = (const unsigned char*)a;
  const unsigned char* y = (const unsigned char*)b;
  for (size_t i = 0; i < n; ++i) {
    if (x[i] != y[i]) return (int)x[i] - (int)y[i];
  }
  return 0;
}
#else
#include <string.h>
#endif

#ifndef CELWASM_ARENA_BYTES
#define CELWASM_ARENA_BYTES (64u * 1024u)
#endif

// Layout of g_memory (byte indices):
//   [0 .. 24)                    reserved null sentinel. Offset 0 is
//                                interpreted as "absent" everywhere that
//                                can carry a nullable CelValue* (optional,
//                                field references). Leaving a CEL_NULL
//                                there means a stray dereference is still
//                                well-defined.
//   [kStaticStart .. g_static_end)
//                                preallocated singletons: null, true, false,
//                                optional-none. Kept stable across
//                                cel_reset() so expressions can share them.
//   [g_static_end .. g_cel_arena.limit)
//                                bump arena. cel_reset() rewinds bump to
//                                g_static_end.

static uint8_t g_memory[CELWASM_ARENA_BYTES];

enum { kStaticStart = 24u };

static uint32_t g_singleton_null_off;
static uint32_t g_singleton_true_off;
static uint32_t g_singleton_false_off;
static uint32_t g_singleton_optional_none_off;
static uint32_t g_static_end;

CelArena g_cel_arena = {0, 0};

static uint32_t align_up(uint32_t n, uint32_t align) {
  return (n + (align - 1u)) & ~(align - 1u);
}

static CelValue* cv_at(uint32_t off) {
  return (CelValue*)(g_memory + off);
}

static void write_cv(uint32_t off, const CelValue* v) {
  memcpy(g_memory + off, v, sizeof(*v));
}

static void ensure_initialized(void) {
  if (g_cel_arena.limit != 0) return;
  memset(g_memory, 0, sizeof(g_memory));
  uint32_t p = kStaticStart;

  g_singleton_null_off = p;
  {
    CelValue v = {0};
    v.kind = CEL_NULL;
    write_cv(p, &v);
  }
  p += (uint32_t)sizeof(CelValue);

  g_singleton_true_off = p;
  {
    CelValue v = {0};
    v.kind = CEL_BOOL;
    v.payload.b = 1;
    write_cv(p, &v);
  }
  p += (uint32_t)sizeof(CelValue);

  g_singleton_false_off = p;
  {
    CelValue v = {0};
    v.kind = CEL_BOOL;
    v.payload.b = 0;
    write_cv(p, &v);
  }
  p += (uint32_t)sizeof(CelValue);

  g_singleton_optional_none_off = p;
  {
    CelValue v = {0};
    v.kind = CEL_OPTIONAL;
    v.payload.opt = 0;
    write_cv(p, &v);
  }
  p += (uint32_t)sizeof(CelValue);

  g_static_end = align_up(p, 8);
  g_cel_arena.bump = g_static_end;
  g_cel_arena.limit = (uint32_t)sizeof(g_memory);
}

uint8_t* cel_mem_base(void) {
  ensure_initialized();
  return g_memory;
}

uint32_t cel_mem_size(void) {
  return (uint32_t)sizeof(g_memory);
}

uint32_t cel_alloc(uint32_t n) {
  ensure_initialized();
  uint32_t need = align_up(n, 8u);
  if (need == 0) need = 8u;
  if (g_cel_arena.bump + need > g_cel_arena.limit) {
    return 0;
  }
  uint32_t off = g_cel_arena.bump;
  g_cel_arena.bump += need;
  memset(g_memory + off, 0, need);
  return off;
}

void cel_reset(void) {
  ensure_initialized();
  g_cel_arena.bump = g_static_end;
}

CelValue* cel_value_at(uint32_t off) {
  if (off == 0) return (CelValue*)0;
  return cv_at(off);
}

static uint32_t alloc_cv(void) {
  return cel_alloc((uint32_t)sizeof(CelValue));
}

uint32_t cel_make_null(void) {
  ensure_initialized();
  return g_singleton_null_off;
}

uint32_t cel_make_bool(int32_t b) {
  ensure_initialized();
  return b ? g_singleton_true_off : g_singleton_false_off;
}

uint32_t cel_make_int(int64_t i) {
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_INT;
  v->payload.i = i;
  return off;
}

uint32_t cel_make_uint(uint64_t u) {
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_UINT;
  v->payload.u = u;
  return off;
}

uint32_t cel_make_double(double d) {
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_DOUBLE;
  v->payload.d = d;
  return off;
}

static uint32_t make_span(CelKind kind, const void* src, uint32_t len) {
  uint32_t data_off = 0;
  if (len > 0) {
    data_off = cel_alloc(len);
    if (data_off == 0) return 0;
    memcpy(g_memory + data_off, src, len);
  }
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = (uint32_t)kind;
  v->payload.s.ptr = data_off;
  v->payload.s.len = len;
  return off;
}

static uint32_t make_span_view(CelKind kind, uint32_t ptr, uint32_t len) {
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = (uint32_t)kind;
  v->payload.s.ptr = ptr;
  v->payload.s.len = len;
  return off;
}

uint32_t cel_make_string(const char* src, uint32_t len) {
  return make_span(CEL_STRING, src, len);
}

uint32_t cel_make_bytes(const void* src, uint32_t len) {
  return make_span(CEL_BYTES, src, len);
}

uint32_t cel_make_string_view(uint32_t ptr, uint32_t len) {
  return make_span_view(CEL_STRING, ptr, len);
}

uint32_t cel_make_bytes_view(uint32_t ptr, uint32_t len) {
  return make_span_view(CEL_BYTES, ptr, len);
}

uint32_t cel_make_message(uint32_t ref_slot) {
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_MESSAGE;
  v->payload.msg_slot = ref_slot;
  return off;
}

uint32_t cel_make_type(uint32_t type_id) {
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_TYPE;
  v->payload.type_id = type_id;
  return off;
}

uint32_t cel_make_duration(int64_t seconds, int32_t nanos) {
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_DURATION;
  v->payload.dur.seconds = seconds;
  v->payload.dur.nanos = nanos;
  return off;
}

uint32_t cel_make_timestamp(int64_t seconds, int32_t nanos) {
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_TIMESTAMP;
  v->payload.ts.seconds = seconds;
  v->payload.ts.nanos = nanos;
  return off;
}

uint32_t cel_make_optional_some(uint32_t inner) {
  if (inner == 0) return 0;
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_OPTIONAL;
  v->payload.opt = inner;
  return off;
}

uint32_t cel_make_optional_none(void) {
  ensure_initialized();
  return g_singleton_optional_none_off;
}

uint32_t cel_make_unknown(uint32_t attribute_id) {
  uint32_t ids_off = cel_alloc((uint32_t)sizeof(uint32_t));
  if (ids_off == 0) return 0;
  *(uint32_t*)(g_memory + ids_off) = attribute_id;

  uint32_t set_off = cel_alloc(2u * (uint32_t)sizeof(uint32_t));
  if (set_off == 0) return 0;
  uint32_t* set = (uint32_t*)(g_memory + set_off);
  set[0] = ids_off;
  set[1] = 1u;

  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_UNKNOWN;
  v->payload.unk = set_off;
  return off;
}

uint32_t cel_make_error(uint32_t code, uint32_t msg_ptr, uint32_t msg_len) {
  uint32_t err_off = cel_alloc(16u);
  if (err_off == 0) return 0;
  uint32_t* p = (uint32_t*)(g_memory + err_off);
  p[0] = code;
  p[1] = msg_ptr;
  p[2] = msg_len;
  p[3] = 0;

  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_ERROR;
  v->payload.err = err_off;
  return off;
}

static int32_t span_eq(uint32_t a, uint32_t b, uint32_t expected_kind) {
  if (a == 0 || b == 0) return 0;
  const CelValue* va = cv_at(a);
  const CelValue* vb = cv_at(b);
  if (va->kind != expected_kind || vb->kind != expected_kind) return 0;
  if (va->payload.s.len != vb->payload.s.len) return 0;
  if (va->payload.s.len == 0) return 1;
  return memcmp(g_memory + va->payload.s.ptr, g_memory + vb->payload.s.ptr,
                va->payload.s.len) == 0
             ? 1
             : 0;
}

int32_t cel_string_eq(uint32_t a, uint32_t b) {
  return span_eq(a, b, (uint32_t)CEL_STRING);
}

int32_t cel_bytes_eq(uint32_t a, uint32_t b) {
  return span_eq(a, b, (uint32_t)CEL_BYTES);
}

// String and bytes concat share everything except the kind tag.  Factor
// out so the per-kind wrapper stays a single forwarding line and the
// allocation / copy sequence has one home.  Snapshots the source
// pointers before allocating — `cel_alloc` may bump-advance into pages
// that happen to alias an input, but the source offsets themselves are
// stable so caching them pre-alloc is enough.
static uint32_t span_concat(uint32_t a, uint32_t b, uint32_t expected_kind) {
  if (a == 0 || b == 0) return 0;
  const CelValue* va = cv_at(a);
  const CelValue* vb = cv_at(b);
  if (va->kind != expected_kind || vb->kind != expected_kind) return 0;
  uint32_t la = va->payload.s.len;
  uint32_t lb = vb->payload.s.len;
  uint32_t total = la + lb;
  uint32_t a_ptr = va->payload.s.ptr;
  uint32_t b_ptr = vb->payload.s.ptr;

  uint32_t data_off = 0;
  if (total > 0) {
    data_off = cel_alloc(total);
    if (data_off == 0) return 0;
    if (la > 0) {
      memcpy(g_memory + data_off, g_memory + a_ptr, la);
    }
    if (lb > 0) {
      memcpy(g_memory + data_off + la, g_memory + b_ptr, lb);
    }
  }
  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = expected_kind;
  v->payload.s.ptr = data_off;
  v->payload.s.len = total;
  return off;
}

uint32_t cel_string_concat(uint32_t a, uint32_t b) {
  return span_concat(a, b, (uint32_t)CEL_STRING);
}

uint32_t cel_bytes_concat(uint32_t a, uint32_t b) {
  return span_concat(a, b, (uint32_t)CEL_BYTES);
}

int64_t cel_string_size(uint32_t s) {
  if (s == 0) return -1;
  const CelValue* v = cv_at(s);
  if (v->kind != (uint32_t)CEL_STRING) return -1;
  uint32_t len = v->payload.s.len;
  if (len == 0) return 0;
  const uint8_t* p = g_memory + v->payload.s.ptr;
  int64_t codepoints = 0;
  for (uint32_t i = 0; i < len; ++i) {
    // Count any byte that is NOT a UTF-8 continuation byte.  Continuations
    // match 0b10xxxxxx; everything else (ASCII 0b0xxxxxxx or a lead byte
    // 0b11xxxxxx) begins a new code point.
    if ((p[i] & 0xC0u) != 0x80u) {
      ++codepoints;
    }
  }
  return codepoints;
}

int64_t cel_bytes_size(uint32_t b) {
  if (b == 0) return -1;
  const CelValue* v = cv_at(b);
  if (v->kind != (uint32_t)CEL_BYTES) return -1;
  return (int64_t)v->payload.s.len;
}

int32_t cel_bool_from_value(uint32_t v) {
  if (v == 0) return 0;
  const CelValue* cv = cv_at(v);
  if (cv->kind != (uint32_t)CEL_BOOL) return 0;
  return cv->payload.b ? 1 : 0;
}

int64_t cel_int_from_value(uint32_t v) {
  if (v == 0) return 0;
  const CelValue* cv = cv_at(v);
  if (cv->kind != (uint32_t)CEL_INT) return 0;
  return cv->payload.i;
}

uint64_t cel_uint_from_value(uint32_t v) {
  if (v == 0) return 0;
  const CelValue* cv = cv_at(v);
  if (cv->kind != (uint32_t)CEL_UINT) return 0;
  return cv->payload.u;
}

double cel_double_from_value(uint32_t v) {
  if (v == 0) return 0.0;
  const CelValue* cv = cv_at(v);
  if (cv->kind != (uint32_t)CEL_DOUBLE) return 0.0;
  return cv->payload.d;
}

// Shared type-check shape for the three string member-call helpers.  On
// type error (either side zero / non-string) returns a sentinel via the
// out-param and `false`; the caller propagates 0 (matches cel_string_eq
// behavior, and keeps these helpers total-functions-returning-i32 which
// is what Binaryen imports want).
static int string_span_pair(uint32_t a, uint32_t b, const uint8_t** pa,
                            uint32_t* la, const uint8_t** pb, uint32_t* lb) {
  if (a == 0 || b == 0) return 0;
  const CelValue* va = cv_at(a);
  const CelValue* vb = cv_at(b);
  if (va->kind != (uint32_t)CEL_STRING || vb->kind != (uint32_t)CEL_STRING) {
    return 0;
  }
  *pa = g_memory + va->payload.s.ptr;
  *la = va->payload.s.len;
  *pb = g_memory + vb->payload.s.ptr;
  *lb = vb->payload.s.len;
  return 1;
}

int32_t cel_string_starts_with(uint32_t s, uint32_t prefix) {
  const uint8_t* sp;
  const uint8_t* pp;
  uint32_t sl;
  uint32_t pl;
  if (!string_span_pair(s, prefix, &sp, &sl, &pp, &pl)) return 0;
  if (pl == 0) return 1;
  if (pl > sl) return 0;
  return memcmp(sp, pp, pl) == 0 ? 1 : 0;
}

int32_t cel_string_ends_with(uint32_t s, uint32_t suffix) {
  const uint8_t* sp;
  const uint8_t* xp;
  uint32_t sl;
  uint32_t xl;
  if (!string_span_pair(s, suffix, &sp, &sl, &xp, &xl)) return 0;
  if (xl == 0) return 1;
  if (xl > sl) return 0;
  return memcmp(sp + (sl - xl), xp, xl) == 0 ? 1 : 0;
}

int32_t cel_string_contains(uint32_t s, uint32_t needle) {
  const uint8_t* sp;
  const uint8_t* np;
  uint32_t sl;
  uint32_t nl;
  if (!string_span_pair(s, needle, &sp, &sl, &np, &nl)) return 0;
  if (nl == 0) return 1;
  if (nl > sl) return 0;
  // Naive byte search.  CEL strings are short enough in practice that the
  // O(n*m) worst case is fine; swap to Boyer-Moore if profiling ever
  // flags it.
  const uint32_t last = sl - nl;
  for (uint32_t i = 0; i <= last; ++i) {
    if (memcmp(sp + i, np, nl) == 0) return 1;
  }
  return 0;
}

// ---- Uniform-boxed ABI (Slice F Step 4) ----------------------------------
//
// Shared prologue for the binary `_v` helpers.  Handles absorption and
// operand-kind checking.  Returns 0 when both operands are OK of the
// expected kind (caller should compute the result); otherwise returns
// the CelValue offset the caller should propagate verbatim — either
// the dominant non-OK status from `cel_status_either`, or a freshly
// boxed CEL_ERROR{TYPE_MISMATCH}.
static uint32_t span_v_prologue(uint32_t a, uint32_t b, uint32_t kind) {
  uint32_t st = cel_status_either(a, b);
  if (st != 0) return st;
  if (a == 0 || b == 0) return cel_make_error(CEL_ERR_TYPE_MISMATCH, 0, 0);
  if (cv_at(a)->kind != kind || cv_at(b)->kind != kind) {
    return cel_make_error(CEL_ERR_TYPE_MISMATCH, 0, 0);
  }
  return 0;
}

// Single-operand prologue for size `_v` helpers.  `cel_status_either`
// short-circuits to 0 for a zero offset, so status propagation is
// inlined as a direct kind inspection: UNKNOWN / ERROR pass through
// unchanged, a zero or wrong-kind operand becomes TYPE_MISMATCH.
static uint32_t size_v_prologue(uint32_t v, uint32_t kind) {
  if (v == 0) return cel_make_error(CEL_ERR_TYPE_MISMATCH, 0, 0);
  uint32_t k = cv_at(v)->kind;
  if (k == (uint32_t)CEL_ERROR || k == (uint32_t)CEL_UNKNOWN) return v;
  if (k != kind) return cel_make_error(CEL_ERR_TYPE_MISMATCH, 0, 0);
  return 0;
}

uint32_t cel_string_eq_v(uint32_t a, uint32_t b) {
  uint32_t st = span_v_prologue(a, b, (uint32_t)CEL_STRING);
  if (st != 0) return st;
  return cel_make_bool(span_eq(a, b, (uint32_t)CEL_STRING));
}

uint32_t cel_bytes_eq_v(uint32_t a, uint32_t b) {
  uint32_t st = span_v_prologue(a, b, (uint32_t)CEL_BYTES);
  if (st != 0) return st;
  return cel_make_bool(span_eq(a, b, (uint32_t)CEL_BYTES));
}

uint32_t cel_string_concat_v(uint32_t a, uint32_t b) {
  uint32_t st = span_v_prologue(a, b, (uint32_t)CEL_STRING);
  if (st != 0) return st;
  return span_concat(a, b, (uint32_t)CEL_STRING);
}

uint32_t cel_bytes_concat_v(uint32_t a, uint32_t b) {
  uint32_t st = span_v_prologue(a, b, (uint32_t)CEL_BYTES);
  if (st != 0) return st;
  return span_concat(a, b, (uint32_t)CEL_BYTES);
}

uint32_t cel_string_starts_with_v(uint32_t s, uint32_t prefix) {
  uint32_t st = span_v_prologue(s, prefix, (uint32_t)CEL_STRING);
  if (st != 0) return st;
  return cel_make_bool(cel_string_starts_with(s, prefix));
}

uint32_t cel_string_ends_with_v(uint32_t s, uint32_t suffix) {
  uint32_t st = span_v_prologue(s, suffix, (uint32_t)CEL_STRING);
  if (st != 0) return st;
  return cel_make_bool(cel_string_ends_with(s, suffix));
}

uint32_t cel_string_contains_v(uint32_t s, uint32_t needle) {
  uint32_t st = span_v_prologue(s, needle, (uint32_t)CEL_STRING);
  if (st != 0) return st;
  return cel_make_bool(cel_string_contains(s, needle));
}

uint32_t cel_string_size_v(uint32_t s) {
  uint32_t st = size_v_prologue(s, (uint32_t)CEL_STRING);
  if (st != 0) return st;
  return cel_make_int(cel_string_size(s));
}

uint32_t cel_bytes_size_v(uint32_t b) {
  uint32_t st = size_v_prologue(b, (uint32_t)CEL_BYTES);
  if (st != 0) return st;
  return cel_make_int(cel_bytes_size(b));
}

// ---- Three-valued logic helpers ------------------------------------------

static int is_3vl_kind(uint32_t k) {
  return k == (uint32_t)CEL_BOOL || k == (uint32_t)CEL_UNKNOWN ||
         k == (uint32_t)CEL_ERROR;
}

static int is_ok_false(const CelValue* v) {
  return v->kind == (uint32_t)CEL_BOOL && v->payload.b == 0;
}

static int is_ok_true(const CelValue* v) {
  return v->kind == (uint32_t)CEL_BOOL && v->payload.b != 0;
}

// Allocates a CEL_UNKNOWN over a sorted, already-populated id array at
// `ids_off` with `len` valid entries.  Used by cel_unknown_merge after
// the merge walk finishes.  Takes the array offset rather than a pointer
// so cel_alloc activity in between is safe.
static uint32_t make_unknown_from_ids(uint32_t ids_off, uint32_t len) {
  uint32_t set_off = cel_alloc(2u * (uint32_t)sizeof(uint32_t));
  if (set_off == 0) return 0;
  uint32_t* desc = (uint32_t*)(g_memory + set_off);
  desc[0] = ids_off;
  desc[1] = len;

  uint32_t off = alloc_cv();
  if (off == 0) return 0;
  CelValue* v = cv_at(off);
  v->kind = CEL_UNKNOWN;
  v->payload.unk = set_off;
  return off;
}

// Sorted-dedup'd merge walk over two already-sorted u32 id arrays.
// Writes into `out` and returns the post-dedup length.  Factored out of
// cel_unknown_merge so the enclosing function stays under the lint
// function-size threshold.
static uint32_t merge_sorted_ids(const uint32_t* ids_a, uint32_t len_a,
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

uint32_t cel_unknown_merge(uint32_t a, uint32_t b) {
  if (a == 0 || b == 0) return 0;
  const CelValue* va = cv_at(a);
  const CelValue* vb = cv_at(b);
  if (va->kind != (uint32_t)CEL_UNKNOWN || vb->kind != (uint32_t)CEL_UNKNOWN) {
    return 0;
  }
  uint32_t set_a = va->payload.unk;
  uint32_t set_b = vb->payload.unk;
  // An empty UnknownSet (payload.unk == 0) is a legal UNKNOWN — the
  // host `get_field` trampoline mints UNKNOWNs this way for FULL
  // attribute-pattern matches (provenance not surfaced yet).  Treat an
  // empty side as "no new ids to contribute" and return the other side
  // verbatim; if both are empty, return `a` (left-biased).  Only the 0
  // *offset* case (null CelValue) still yields 0.
  if (set_a == 0 && set_b == 0) return a;
  if (set_a == 0) return b;
  if (set_b == 0) return a;

  // Snapshot descriptor scalars before any further cel_alloc — on wasm32
  // memory.grow can relocate g_memory, so pointers derived from offsets
  // must be re-taken after each bump.
  uint32_t* desc_a = (uint32_t*)(g_memory + set_a);
  uint32_t* desc_b = (uint32_t*)(g_memory + set_b);
  uint32_t ids_a_off = desc_a[0];
  uint32_t len_a = desc_a[1];
  uint32_t ids_b_off = desc_b[0];
  uint32_t len_b = desc_b[1];

  uint32_t max_total = len_a + len_b;
  uint32_t bytes = max_total * (uint32_t)sizeof(uint32_t);
  uint32_t out_ids = cel_alloc(bytes == 0 ? (uint32_t)sizeof(uint32_t) : bytes);
  if (out_ids == 0) return 0;

  const uint32_t* ids_a = (const uint32_t*)(g_memory + ids_a_off);
  const uint32_t* ids_b = (const uint32_t*)(g_memory + ids_b_off);
  uint32_t* out = (uint32_t*)(g_memory + out_ids);
  uint32_t k = merge_sorted_ids(ids_a, len_a, ids_b, len_b, out);
  return make_unknown_from_ids(out_ids, k);
}

uint32_t cel_not(uint32_t a) {
  if (a == 0) return 0;
  const CelValue* va = cv_at(a);
  uint32_t k = va->kind;
  if (k == (uint32_t)CEL_BOOL) {
    return cel_make_bool(va->payload.b ? 0 : 1);
  }
  if (k == (uint32_t)CEL_UNKNOWN || k == (uint32_t)CEL_ERROR) {
    return a;
  }
  return 0;
}

uint32_t cel_and(uint32_t a, uint32_t b) {
  if (a == 0 || b == 0) return 0;
  const CelValue* va = cv_at(a);
  const CelValue* vb = cv_at(b);
  if (!is_3vl_kind(va->kind) || !is_3vl_kind(vb->kind)) return 0;
  // OK(false) short-circuits past everything, including ERROR / UNKNOWN.
  if (is_ok_false(va)) return a;
  if (is_ok_false(vb)) return b;
  // OK(true) && x = x.
  if (is_ok_true(va)) return b;
  if (is_ok_true(vb)) return a;
  // Neither side is a definite bool.  ERROR dominates UNKNOWN.
  if (va->kind == (uint32_t)CEL_ERROR) return a;
  if (vb->kind == (uint32_t)CEL_ERROR) return b;
  // Both UNKNOWN.
  return cel_unknown_merge(a, b);
}

uint32_t cel_or(uint32_t a, uint32_t b) {
  if (a == 0 || b == 0) return 0;
  const CelValue* va = cv_at(a);
  const CelValue* vb = cv_at(b);
  if (!is_3vl_kind(va->kind) || !is_3vl_kind(vb->kind)) return 0;
  // OK(true) short-circuits.
  if (is_ok_true(va)) return a;
  if (is_ok_true(vb)) return b;
  // OK(false) || x = x.
  if (is_ok_false(va)) return b;
  if (is_ok_false(vb)) return a;
  if (va->kind == (uint32_t)CEL_ERROR) return a;
  if (vb->kind == (uint32_t)CEL_ERROR) return b;
  return cel_unknown_merge(a, b);
}

uint32_t cel_status_either(uint32_t a, uint32_t b) {
  if (a == 0 || b == 0) return 0;
  const CelValue* va = cv_at(a);
  const CelValue* vb = cv_at(b);
  if (va->kind == (uint32_t)CEL_ERROR) return a;
  if (vb->kind == (uint32_t)CEL_ERROR) return b;
  int au = (va->kind == (uint32_t)CEL_UNKNOWN);
  int bu = (vb->kind == (uint32_t)CEL_UNKNOWN);
  if (au && bu) return cel_unknown_merge(a, b);
  if (au) return a;
  if (bu) return b;
  return 0;
}

// ---- Checked arithmetic (M4 Slice B + C) ---------------------------------
//
// The scalar overflow helpers below are reused by every `_at_ii` /
// `_at_uu` / `_neg_at_i` exported helper defined later in this file.
//
// INT64_MIN edge cases:
//   * `-INT64_MIN` overflows (|INT64_MIN| > INT64_MAX).
//   * `INT64_MIN / -1` overflows (same reason; C has UB for this).
//   * `INT64_MIN % -1` is mathematically 0; C has UB so special-case.

// Compiler builtins like `__builtin_mul_overflow` lower to compiler-rt
// helpers (e.g. `__multi3`) that aren't in the freestanding wasm32
// link, so overflow detection is hand-rolled against in-range
// arithmetic.  Each helper returns 1 on overflow, 0 otherwise, and
// writes the (defined) result to `*r` when safe — mirroring the
// signature of the builtins so the call sites stay readable.
//
// Clang's -O2 also recognizes the idioms `a > UINT64_MAX / b` and
// `a > INT64_MAX / y` as overflow-check equivalents and rewrites them
// back into `__builtin_{mul,add,sub}_overflow`, which for 64-bit mul
// on wasm32 re-pulls in `__multi3`.  The mul helpers below use a
// 32×32→64 hi/lo split so the optimizer sees only native 64-bit muls
// and can't re-fold the check.
static int s64_add_overflow(int64_t a, int64_t b, int64_t* r) {
  if (b > 0 && a > INT64_MAX - b) return 1;
  if (b < 0 && a < INT64_MIN - b) return 1;
  *r = a + b;
  return 0;
}

static int s64_sub_overflow(int64_t a, int64_t b, int64_t* r) {
  if (b < 0 && a > INT64_MAX + b) return 1;
  if (b > 0 && a < INT64_MIN + b) return 1;
  *r = a - b;
  return 0;
}

// 64×64 → 128 unsigned multiply, returned as (hi, lo).  Each partial
// product is 32×32 → 64, which wasm32 expresses as `i64.mul` after a
// widening load.  Exposing the high half explicitly prevents the
// overflow-idiom recognizer from pattern-matching; the comparison is
// against an arithmetic result, not a division.
static void u64_mul_full(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
  uint64_t al = (uint32_t)a;
  uint64_t ah = a >> 32;
  uint64_t bl = (uint32_t)b;
  uint64_t bh = b >> 32;
  uint64_t ll = al * bl;
  uint64_t lh = al * bh;
  uint64_t hl = ah * bl;
  uint64_t hh = ah * bh;
  uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
  *lo = (ll & 0xffffffffu) | (mid << 32);
  *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

static int u64_mul_overflow(uint64_t a, uint64_t b, uint64_t* r) {
  uint64_t hi;
  uint64_t lo;
  u64_mul_full(a, b, &hi, &lo);
  if (hi != 0) return 1;
  *r = lo;
  return 0;
}

static int s64_mul_overflow(int64_t a, int64_t b, int64_t* r) {
  // Signed overflow via unsigned full multiply: sign-correct the
  // absolute values (INT64_MIN abs is representable as uint64_t), do a
  // 128-bit unsigned mul, then apply the sign and check magnitude.
  uint64_t au = a < 0 ? (uint64_t)-(a + 1) + 1u : (uint64_t)a;
  uint64_t bu = b < 0 ? (uint64_t)-(b + 1) + 1u : (uint64_t)b;
  uint64_t hi;
  uint64_t lo;
  u64_mul_full(au, bu, &hi, &lo);
  if (hi != 0) return 1;
  int neg = (a < 0) ^ (b < 0);
  if (neg) {
    // Representable range for a negative signed 64-bit result is
    // [-2^63, -1] in magnitude [1, 2^63].
    if (lo > (uint64_t)INT64_MAX + 1u) return 1;
    *r = (lo == (uint64_t)INT64_MAX + 1u) ? INT64_MIN : -(int64_t)lo;
    return 0;
  }
  if (lo > (uint64_t)INT64_MAX) return 1;
  *r = (int64_t)lo;
  return 0;
}

static int u64_add_overflow(uint64_t a, uint64_t b, uint64_t* r) {
  uint64_t s = a + b;
  if (s < a) return 1;
  *r = s;
  return 0;
}

static int u64_sub_overflow(uint64_t a, uint64_t b, uint64_t* r) {
  if (b > a) return 1;
  *r = a - b;
  return 0;
}

// ---- Scratch-slot (sret) ABI (M4 Slice C) --------------------------------
//
// Each exported `_at_ii` / `_at_uu` / `_neg_at_i` helper is shaped:
//   1. `if (out == 0) return;`           // caller bug (null sentinel)
//   2. scalar overflow / div-0 / mod-0 check → `write_error_at` on failure
//   3. `write_int_at / write_uint_at` on success
//
// The scalar overflow helpers (s64_add_overflow etc.) are defined
// earlier in this file.  See `cel_runtime.h` for the ABI contract
// (24-byte slot, total-function guarantee).

// Writes a CEL_ERROR into *out, allocating a fresh error struct in the
// arena.  Arena-OOM still writes the kind tag with a zero err offset so
// the slot is well-formed — callers branch on the kind, not the payload.
static void write_error_at(uint32_t out, uint32_t code) {
  uint32_t err_off = cel_alloc(16u);
  if (err_off != 0) {
    uint32_t* p = (uint32_t*)(g_memory + err_off);
    p[0] = code;
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
  }
  CelValue* v = cv_at(out);
  v->kind = CEL_ERROR;
  v->payload.err = err_off;
}

void cel_box_int(uint32_t out, int64_t i) {
  if (out == 0) return;
  CelValue* v = cv_at(out);
  v->kind = CEL_INT;
  v->payload.i = i;
}

void cel_box_uint(uint32_t out, uint64_t u) {
  if (out == 0) return;
  CelValue* v = cv_at(out);
  v->kind = CEL_UINT;
  v->payload.u = u;
}

void cel_box_double(uint32_t out, double d) {
  if (out == 0) return;
  CelValue* v = cv_at(out);
  v->kind = CEL_DOUBLE;
  v->payload.d = d;
}

void cel_copy_celvalue_at(uint32_t out, uint32_t src) {
  if (out == 0) return;
  if (src == 0) {
    write_error_at(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  memcpy(g_memory + out, g_memory + src, sizeof(CelValue));
}

void cel_set_error_at(uint32_t out, uint32_t code) {
  if (out == 0) return;
  write_error_at(out, code);
}

static void write_int_at(uint32_t out, int64_t r) {
  CelValue* v = cv_at(out);
  v->kind = CEL_INT;
  v->payload.i = r;
}

static void write_uint_at(uint32_t out, uint64_t r) {
  CelValue* v = cv_at(out);
  v->kind = CEL_UINT;
  v->payload.u = r;
}

void cel_int_add_at_ii(uint32_t out, int64_t a, int64_t b) {
  if (out == 0) return;
  int64_t r;
  if (s64_add_overflow(a, b, &r)) {
    write_error_at(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int_at(out, r);
}

void cel_int_sub_at_ii(uint32_t out, int64_t a, int64_t b) {
  if (out == 0) return;
  int64_t r;
  if (s64_sub_overflow(a, b, &r)) {
    write_error_at(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int_at(out, r);
}

void cel_int_mul_at_ii(uint32_t out, int64_t a, int64_t b) {
  if (out == 0) return;
  int64_t r;
  if (s64_mul_overflow(a, b, &r)) {
    write_error_at(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int_at(out, r);
}

void cel_int_div_at_ii(uint32_t out, int64_t a, int64_t b) {
  if (out == 0) return;
  if (b == 0) {
    write_error_at(out, CEL_ERR_DIVIDE_BY_ZERO);
    return;
  }
  if (a == INT64_MIN && b == -1) {
    write_error_at(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int_at(out, a / b);
}

void cel_int_mod_at_ii(uint32_t out, int64_t a, int64_t b) {
  if (out == 0) return;
  if (b == 0) {
    write_error_at(out, CEL_ERR_MODULUS_BY_ZERO);
    return;
  }
  // INT64_MIN % -1 is mathematically 0 but C reserves UB for it.
  if (a == INT64_MIN && b == -1) {
    write_int_at(out, 0);
    return;
  }
  write_int_at(out, a % b);
}

void cel_uint_add_at_uu(uint32_t out, uint64_t a, uint64_t b) {
  if (out == 0) return;
  uint64_t r;
  if (u64_add_overflow(a, b, &r)) {
    write_error_at(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint_at(out, r);
}

void cel_uint_sub_at_uu(uint32_t out, uint64_t a, uint64_t b) {
  if (out == 0) return;
  uint64_t r;
  if (u64_sub_overflow(a, b, &r)) {
    write_error_at(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint_at(out, r);
}

void cel_uint_mul_at_uu(uint32_t out, uint64_t a, uint64_t b) {
  if (out == 0) return;
  uint64_t r;
  if (u64_mul_overflow(a, b, &r)) {
    write_error_at(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint_at(out, r);
}

void cel_uint_div_at_uu(uint32_t out, uint64_t a, uint64_t b) {
  if (out == 0) return;
  if (b == 0) {
    write_error_at(out, CEL_ERR_DIVIDE_BY_ZERO);
    return;
  }
  write_uint_at(out, a / b);
}

void cel_uint_mod_at_uu(uint32_t out, uint64_t a, uint64_t b) {
  if (out == 0) return;
  if (b == 0) {
    write_error_at(out, CEL_ERR_MODULUS_BY_ZERO);
    return;
  }
  write_uint_at(out, a % b);
}

void cel_int_neg_at_i(uint32_t out, int64_t a) {
  if (out == 0) return;
  if (a == INT64_MIN) {
    write_error_at(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int_at(out, -a);
}

// ---- Boxed-operand arithmetic (M4 Slice F Step 2) ------------------------
//
// Shared front-matter for every `_at_vv` helper: returns 1 iff both
// operands are OK of the expected kind (so the caller should proceed
// with its scalar op); returns 0 in which case the helper has
// already written the correct value into `*out` (dominant non-OK,
// or CEL_ERROR{TYPE_MISMATCH} for a kind mismatch, or nothing for
// `out == 0` — the total-function guarantee).
static int arith_boxed_prologue(uint32_t out, uint32_t a_off, uint32_t b_off,
                                uint32_t expected_kind, int64_t* ra,
                                int64_t* rb) {
  if (out == 0) return 0;
  uint32_t st = cel_status_either(a_off, b_off);
  if (st != 0) {
    cel_copy_celvalue_at(out, st);
    return 0;
  }
  if (a_off == 0 || b_off == 0) {
    write_error_at(out, CEL_ERR_TYPE_MISMATCH);
    return 0;
  }
  const CelValue* va = cv_at(a_off);
  const CelValue* vb = cv_at(b_off);
  if (va->kind != expected_kind || vb->kind != expected_kind) {
    write_error_at(out, CEL_ERR_TYPE_MISMATCH);
    return 0;
  }
  // Payloads are the same bit-width for both int and uint; the caller
  // reinterprets after the copy.  Using int64_t here keeps the
  // prologue kind-agnostic.
  *ra = va->payload.i;
  *rb = vb->payload.i;
  return 1;
}

void cel_int_add_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off) {
  int64_t a = 0;
  int64_t b = 0;
  if (!arith_boxed_prologue(out, a_off, b_off, CEL_INT, &a, &b)) return;
  cel_int_add_at_ii(out, a, b);
}

void cel_int_sub_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off) {
  int64_t a = 0;
  int64_t b = 0;
  if (!arith_boxed_prologue(out, a_off, b_off, CEL_INT, &a, &b)) return;
  cel_int_sub_at_ii(out, a, b);
}

void cel_int_mul_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off) {
  int64_t a = 0;
  int64_t b = 0;
  if (!arith_boxed_prologue(out, a_off, b_off, CEL_INT, &a, &b)) return;
  cel_int_mul_at_ii(out, a, b);
}

void cel_int_div_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off) {
  int64_t a = 0;
  int64_t b = 0;
  if (!arith_boxed_prologue(out, a_off, b_off, CEL_INT, &a, &b)) return;
  cel_int_div_at_ii(out, a, b);
}

void cel_int_mod_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off) {
  int64_t a = 0;
  int64_t b = 0;
  if (!arith_boxed_prologue(out, a_off, b_off, CEL_INT, &a, &b)) return;
  cel_int_mod_at_ii(out, a, b);
}

void cel_uint_add_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off) {
  int64_t a = 0;
  int64_t b = 0;
  if (!arith_boxed_prologue(out, a_off, b_off, CEL_UINT, &a, &b)) return;
  cel_uint_add_at_uu(out, (uint64_t)a, (uint64_t)b);
}

void cel_uint_sub_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off) {
  int64_t a = 0;
  int64_t b = 0;
  if (!arith_boxed_prologue(out, a_off, b_off, CEL_UINT, &a, &b)) return;
  cel_uint_sub_at_uu(out, (uint64_t)a, (uint64_t)b);
}

void cel_uint_mul_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off) {
  int64_t a = 0;
  int64_t b = 0;
  if (!arith_boxed_prologue(out, a_off, b_off, CEL_UINT, &a, &b)) return;
  cel_uint_mul_at_uu(out, (uint64_t)a, (uint64_t)b);
}

void cel_uint_div_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off) {
  int64_t a = 0;
  int64_t b = 0;
  if (!arith_boxed_prologue(out, a_off, b_off, CEL_UINT, &a, &b)) return;
  cel_uint_div_at_uu(out, (uint64_t)a, (uint64_t)b);
}

void cel_uint_mod_at_vv(uint32_t out, uint32_t a_off, uint32_t b_off) {
  int64_t a = 0;
  int64_t b = 0;
  if (!arith_boxed_prologue(out, a_off, b_off, CEL_UINT, &a, &b)) return;
  cel_uint_mod_at_uu(out, (uint64_t)a, (uint64_t)b);
}

void cel_int_neg_at_v(uint32_t out, uint32_t a_off) {
  if (out == 0) return;
  // `cel_status_either` short-circuits to 0 when one side is a null
  // offset, so a single-operand absorption check inspects the kind
  // tag directly.  ERROR and UNKNOWN both propagate to `*out`; any
  // other non-int surfaces as a type-mismatch error.
  if (a_off == 0) {
    write_error_at(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const CelValue* va = cv_at(a_off);
  if (va->kind == (uint32_t)CEL_ERROR || va->kind == (uint32_t)CEL_UNKNOWN) {
    cel_copy_celvalue_at(out, a_off);
    return;
  }
  if (va->kind != (uint32_t)CEL_INT) {
    write_error_at(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  cel_int_neg_at_i(out, va->payload.i);
}

// ---- 3VL-aware scalar comparison helpers (M4 Slice F1) -------------------
//
// Each helper mirrors the same four-step shape:
//   1.  Zero-offset type-error guard (matches the rest of the runtime).
//   2.  `cel_status_either(a, b)` propagates dominant non-OK status;
//       return it verbatim so the wrapping absorber sees the CelValue.
//   3.  Confirm both operands are OK of the expected scalar kind; if
//       not, return 0 (type error) so codegen's zero-check catches it.
//   4.  Read payload and return `cel_make_bool(raw)`.
//
// Ordered double compares additionally translate NaN into
// `cel_make_error(CEL_ERR_TYPE_MISMATCH)`, matching the spec rule
// and the scalar `LowerDoubleOrderedCompare` path.  Equality is
// IEEE-754 literal: `NaN == NaN` is false, `NaN != x` is true.
// Common front-matter for every `cel_cmp_*` helper.  Writes either a
// propagated non-OK status offset (cel_status_either) or 0 into
// `*status_out`, and returns 1 iff the caller should proceed to read
// the payload (both operands are OK and of the expected kind).  When
// it returns 0 the helper's caller returns `*status_out` verbatim —
// a zero offset when either operand is missing / wrong kind, or the
// dominant non-OK status otherwise.
static int cel_cmp_prologue(uint32_t a, uint32_t b, uint32_t expected_kind,
                            uint32_t* status_out) {
  if (a == 0 || b == 0) {
    *status_out = 0;
    return 0;
  }
  uint32_t st = cel_status_either(a, b);
  if (st != 0) {
    *status_out = st;
    return 0;
  }
  if (cv_at(a)->kind != expected_kind || cv_at(b)->kind != expected_kind) {
    *status_out = 0;
    return 0;
  }
  return 1;
}

#define CEL_CMP_INT_OP(suffix, op)                                    \
  uint32_t cel_cmp_int_##suffix(uint32_t a, uint32_t b) {             \
    uint32_t st;                                                      \
    if (!cel_cmp_prologue(a, b, (uint32_t)CEL_INT, &st)) return st;   \
    return cel_make_bool(cv_at(a)->payload.i op cv_at(b)->payload.i); \
  }

CEL_CMP_INT_OP(eq, ==)
CEL_CMP_INT_OP(ne, !=)
CEL_CMP_INT_OP(lt, <)
CEL_CMP_INT_OP(le, <=)
CEL_CMP_INT_OP(gt, >)
CEL_CMP_INT_OP(ge, >=)

#undef CEL_CMP_INT_OP

#define CEL_CMP_UINT_OP(suffix, op)                                   \
  uint32_t cel_cmp_uint_##suffix(uint32_t a, uint32_t b) {            \
    uint32_t st;                                                      \
    if (!cel_cmp_prologue(a, b, (uint32_t)CEL_UINT, &st)) return st;  \
    return cel_make_bool(cv_at(a)->payload.u op cv_at(b)->payload.u); \
  }

CEL_CMP_UINT_OP(eq, ==)
CEL_CMP_UINT_OP(ne, !=)
CEL_CMP_UINT_OP(lt, <)
CEL_CMP_UINT_OP(le, <=)
CEL_CMP_UINT_OP(gt, >)
CEL_CMP_UINT_OP(ge, >=)

#undef CEL_CMP_UINT_OP

// IEEE 754 equality is well-defined on NaN (NaN == x is false, NaN != x is
// true); CEL's `==` / `!=` inherit that, so no NaN-to-ERROR guard on the
// equality helpers.
#define CEL_CMP_DOUBLE_EQ_OP(suffix, op)                               \
  uint32_t cel_cmp_double_##suffix(uint32_t a, uint32_t b) {           \
    uint32_t st;                                                       \
    if (!cel_cmp_prologue(a, b, (uint32_t)CEL_DOUBLE, &st)) return st; \
    return cel_make_bool(cv_at(a)->payload.d op cv_at(b)->payload.d);  \
  }

CEL_CMP_DOUBLE_EQ_OP(eq, ==)
CEL_CMP_DOUBLE_EQ_OP(ne, !=)

#undef CEL_CMP_DOUBLE_EQ_OP

// Ordered double compares: NaN-in → CEL_ERROR{TYPE_MISMATCH} per spec
// (matches `LowerDoubleOrderedCompare` on the scalar fast path).
// `x != x` is the portable NaN check (NaN is the only value unequal
// to itself).
#define CEL_CMP_DOUBLE_ORD_OP(suffix, op)                              \
  uint32_t cel_cmp_double_##suffix(uint32_t a, uint32_t b) {           \
    uint32_t st;                                                       \
    if (!cel_cmp_prologue(a, b, (uint32_t)CEL_DOUBLE, &st)) return st; \
    double ad = cv_at(a)->payload.d;                                   \
    double bd = cv_at(b)->payload.d;                                   \
    if (ad != ad || bd != bd) {                                        \
      return cel_make_error(CEL_ERR_TYPE_MISMATCH, 0, 0);              \
    }                                                                  \
    return cel_make_bool(ad op bd);                                    \
  }

CEL_CMP_DOUBLE_ORD_OP(lt, <)
CEL_CMP_DOUBLE_ORD_OP(le, <=)
CEL_CMP_DOUBLE_ORD_OP(gt, >)
CEL_CMP_DOUBLE_ORD_OP(ge, >=)

#undef CEL_CMP_DOUBLE_ORD_OP

uint32_t cel_cmp_bool_eq(uint32_t a, uint32_t b) {
  uint32_t st;
  if (!cel_cmp_prologue(a, b, (uint32_t)CEL_BOOL, &st)) return st;
  return cel_make_bool(cv_at(a)->payload.b == cv_at(b)->payload.b);
}

uint32_t cel_cmp_bool_ne(uint32_t a, uint32_t b) {
  uint32_t st;
  if (!cel_cmp_prologue(a, b, (uint32_t)CEL_BOOL, &st)) return st;
  return cel_make_bool(cv_at(a)->payload.b != cv_at(b)->payload.b);
}
