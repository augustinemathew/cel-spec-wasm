// String / bytes operation helpers — slot-out helper ABI per
// `design.md §4.2` / `m5-kcall-comprehensions.md §2.1`.
//
// Every helper has the uniform wasm signature
// `(i32 out_slot, i32 a_slot, ..., i32 argN-1) -> void`.  Bodies
// live in `cel_runtime.c`; this header keeps the topic separate so
// call sites can include only what they use.
//
// **Concat is the only string op that allocates.**  `cel_string_concat`
// /  `cel_bytes_concat` use `cel_alloc` to build a fresh payload in
// the arena; the resulting CelValue's `payload.s.ptr` points into
// that arena.  Every dynamically-built string payload is owned by
// the arena `cel_reset` rewinds at the top of the next $eval, so
// the lifetime is bounded by one Eval call.  Mirrors the M1
// `cel_make_string` lifetime contract exactly.
//
// Other helpers (`size`, `eq`, `lt`, `contains`, `startsWith`,
// `endsWith`) read operand spans without allocating; the result is
// a fixed-size CelValue (CEL_INT for size, CEL_BOOL otherwise).
//
// **3VL + type-mismatch envelope** is identical to `cel_arith.h` /
// `cel_compare.h`: ERROR / UNKNOWN propagates verbatim; wrong kind
// on either operand → CEL_ERR_TYPE_MISMATCH.  Arena OOM during
// concat → CEL_ERR_OVERFLOW.
//
// **Regex `matches` is deferred** per `m5-kcall-comprehensions.md
// §1.2` — needs a regex engine choice that's orthogonal to the
// rest of M5.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_OPS_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_OPS_H_

#include <stdint.h>

#include "compiler_v2/runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// String — concat / size / eq / lt / contains / startsWith / endsWith.
// `size` is byte-count per langdef §"String / bytes" (NOT codepoint
// count); cel-cpp's `Size::String` does the same.
void cel_string_concat_at_vv(uint32_t out_slot, uint32_t a_slot,
                             uint32_t b_slot);
void cel_string_size_at_v(uint32_t out_slot, uint32_t v_slot);
void cel_string_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
// String ordering — full lt/le/gt/ge matrix.  Byte-lex order;
// per langdef §"String / bytes", UTF-8 byte order matches Unicode
// code-point order.
void cel_string_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_string_le_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_string_gt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_string_ge_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_string_contains_at_vv(uint32_t out_slot, uint32_t s_slot,
                               uint32_t sub_slot);
void cel_string_starts_with_at_vv(uint32_t out_slot, uint32_t s_slot,
                                  uint32_t pfx_slot);
void cel_string_ends_with_at_vv(uint32_t out_slot, uint32_t s_slot,
                                uint32_t sfx_slot);

// Bytes — concat / size / eq + full ordering matrix (lt/le/gt/ge).
// Per langdef §"String / bytes", contains/startsWith/endsWith are
// string-only operations; ordering compares as unsigned bytes.
void cel_bytes_concat_at_vv(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot);
void cel_bytes_size_at_v(uint32_t out_slot, uint32_t v_slot);
void cel_bytes_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_bytes_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_bytes_le_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_bytes_gt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_bytes_ge_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_OPS_H_
