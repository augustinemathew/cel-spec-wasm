// String + bytes operation helpers.
//
// Carved out of cel_runtime.c per
// `doc/implementation-plan/rewrite/cel-runtime-c-split-plan.md`.
// See cel_string_ops.h for the public ABI.  Concat is the only
// allocator: writes a fresh payload into the arena.  Other helpers
// read operand spans without allocating.  3VL + type-mismatch
// envelope mirrors cel_arith.h / cel_compare.h.
//
// cel-cpp parity: third_party/cel-cpp/runtime/standard/string_functions.cc

#include "runtime/cel_string_ops.h"

#include <stdint.h>

#include "runtime/cel_arena.h"
#include "runtime/cel_internal.h"

// Span equality byte-for-byte.  CEL strings are UTF-8 byte arrays
// at the langdef level; equality is byte equality (no Unicode
// normalisation).  Same shape works for bytes operands.
//
// Note: cel_internal.h has a value-form `spans_equal(CelSpan, CelSpan)`
// for cross-TU sharing; this pointer-form is the legacy local
// equivalent.  Both compile to the same bytes; leaving the local
// version in place avoids touching the in-arena map / equality
// callers in cel_runtime.c.
static int span_eq(const CelSpan* a, const CelSpan* b) {
  if (a->len != b->len) return 0;
  const uint8_t* base = cel_memory_base_();
  return cel_byteptr_equal_(base + a->ptr, base + b->ptr, a->len);
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
  return cel_byteptr_equal_(ph, ps, sub->len);
}

// Substring search.  langdef pins string ops to byte granularity (no
// Unicode normalisation); cel-cpp's `StringContains::Apply` is also a
// byte scan.  We anchor on the substring's first byte: `memchr` finds
// the next candidate start, and only there do we compare the full
// `sub`.  This skips every haystack position whose first byte can't
// begin a match, and `memchr` itself (wasi-libc / musl) scans
// word-at-a-time via the has-zero bit-trick rather than byte-by-byte —
// so a needle whose first byte is absent costs one word-parallel pass,
// not `hay.len` failed compares.
static int span_contains(const CelSpan* hay, const CelSpan* sub) {
  if (sub->len == 0) return 1;
  if (sub->len > hay->len) return 0;
  const uint8_t* base = cel_memory_base_();
  const uint8_t* hbeg = base + hay->ptr;
  const uint8_t* ps = base + sub->ptr;
  const uint32_t last = hay->len - sub->len;  // last valid start offset
  uint32_t i = 0;
  for (;;) {
    const uint8_t* hit = memchr(hbeg + i, ps[0], (size_t)(last - i) + 1);
    if (hit == NULL) return 0;
    const uint32_t pos = (uint32_t)(hit - hbeg);
    if (cel_byteptr_equal_(hbeg + pos, ps, sub->len)) return 1;
    if (pos >= last) return 0;
    i = pos + 1;
  }
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
  // The u32 length add can wrap for adversarial spans; a wrapped
  // total under-allocates and the memcpys below scribble past the
  // run.  Reject → poison, never wrap.
  if (b->len > UINT32_MAX - a->len) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint32_t total = a->len + b->len;
  uint32_t off = 0;
  if (total > 0) {
    off = arena_alloc(total);
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

// UTF-8 code-point count.  Each byte either:
//   - 0xxxxxxx                                 → 1-byte codepoint (ASCII)
//   - 110xxxxx, 1110xxxx, 11110xxx             → leading byte of a 2/3/4-byte
//                                                codepoint (count once)
//   - 10xxxxxx                                 → continuation byte (skip)
// langdef §"size": `size(string)` is the number of Unicode code points,
// NOT the byte length.  Pre-2026-06-05 we returned the byte length,
// failing conformance rows `size/one_unicode` (`size('ÿ')` → 1 not 2)
// and `size/unicode` (`size('πέντε')` → 5 not 10).  Validation of the
// UTF-8 encoding itself is NOT this kernel's job — CelString invariants
// guarantee well-formed UTF-8.  See `runtime/cel_string_ops.c`.
static int64_t utf8_codepoint_count(const uint8_t* p, uint32_t len) {
  int64_t n = 0;
  for (uint32_t i = 0; i < len; ++i) {
    if ((p[i] & 0xC0u) != 0x80u) ++n;
  }
  return n;
}

void cel_string_size_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_STRING) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const uint8_t* p = cel_memory_base_() + v->payload.s.ptr;
  write_int(out, utf8_codepoint_count(p, v->payload.s.len));
}

void cel_bytes_size_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_BYTES) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_int(out, (int64_t)v->payload.s.len);
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
