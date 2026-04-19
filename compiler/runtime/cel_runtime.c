#include "compiler/runtime/cel_runtime.h"

#include <string.h>

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
  return (CelValue*)(void*)(g_memory + off);
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

uint32_t cel_mem_size(void) { return (uint32_t)sizeof(g_memory); }

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
  return memcmp(g_memory + va->payload.s.ptr,
                g_memory + vb->payload.s.ptr,
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
