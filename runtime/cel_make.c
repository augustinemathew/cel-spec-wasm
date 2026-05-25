// Scalar / span CelValue constructors.
//
// Carved out of cel_runtime.c per
// `doc/implementation-plan/rewrite/cel-runtime-c-split-plan.md` (P4).
// Depends on cel_arena.c (arena_alloc, cv_at) via cel_internal.h.
//
// All constructors allocate from the per-Eval arena and return the
// byte offset into shared memory of the new CelValue (or 0 on OOM).

#include "runtime/cel_make.h"

#include <stdint.h>

#include "runtime/cel_arena.h"
#include "runtime/cel_internal.h"
#include "runtime/cel_log.h"

static uint32_t alloc_cv(void) {
  return arena_alloc((uint32_t)sizeof(CelValue));
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
    data_off = arena_alloc(len);
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
