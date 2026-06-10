// Per-TU shared internals for the carved cel_runtime.c TUs.
//
// NOT exported by cel_runtime.h — call sites that need these helpers
// (tests, peer .c TUs) include this header directly.
//
// Two categories live here:
//
//   1. `static inline` helpers shared by every arith / compare /
//      string-ops / 3VL / conversion kernel.  Keeping these inline
//      (rather than `extern`) preserves cross-TU inlining for the
//      freestanding wasm32 cross-compile — clang-15 at -O2 does not
//      enable LTO, so an `extern` definition in a sibling TU would
//      become an indirect call.  See split-plan Risk #1 / #4.
//
//   2. `extern` declarations for the two shared accessors
//      (`cel_memory_base_`, `cel_memory_size_`) that live in
//      `cel_memory.c` and are linked across every TU.
//
// Per split-plan §2 "Recommended internal header layout".

#ifndef CELWASM_RUNTIME_CEL_INTERNAL_H_
#define CELWASM_RUNTIME_CEL_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shared linear memory accessors.  Definitions live in cel_memory.c.
// On wasm the base is the start of the imported `cel.memory`; on host
// it backs a fixed-size byte buffer.  See cel_memory.c for the asm
// opacity barrier that prevents clang from miscompiling `*(base+off)`
// stores as no-ops when `base` is a zero pointer literal.
uint8_t* cel_memory_base_(void);
uint32_t cel_memory_size_(void);

// Cross-type numeric tri-state compare result.  Defined in
// cel_compare.c (file-extern so callers in cel_runtime.c list/map
// equality + cel_compare.c numeric helpers all share the same body).
typedef enum {
  kCmpLess = 0,
  kCmpEqual = 1,
  kCmpGreater = 2,
  kCmpNanInequal = 3,
} CmpResult;

CmpResult numeric_compare_kernel(const CelValue* a, const CelValue* b);
int is_numeric_kind(uint32_t kind);

// Polymorphic value equality.  Defined in cel_compare.c; referenced
// by cel_list_eq_arena (cel_list.c) and cel_map_eq_arena (cel_map.c)
// for element / value comparisons.  Routes numeric pairs through the
// cross-type numeric ladder per langdef §"Equality".
int cel_value_eq(const CelValue* a, const CelValue* b);

// Map-key equality (bool / int / uint / string + cross-type numeric).
// Defined in cel_map.c; referenced by cel_compare.c's
// cel_value_eq_polymorphic for cross-kind non-numeric fallthrough.
int map_keys_equal(const CelValue* a, const CelValue* b);

// CelValue* at byte-offset `off`.  Caller must have verified
// `off != 0` for the absent-sentinel contract; for that, use the
// public `cel_value_at` in cel_arena.h.
static inline CelValue* cv_at(uint32_t off) {
  return (CelValue*)(cel_memory_base_() + off);
}

// Stamp `out` to CEL_ERROR with `err_code`.
static inline void poison(CelValue* v, uint32_t err_code) {
  v->kind = CEL_ERROR;
  v->payload.err = err_code;
}

// 3VL absorption.  Returns 1 (and overwrites *out) if either operand
// is ERROR / UNKNOWN.  Left-bias for both; the full UNKNOWN+UNKNOWN
// merge lives in `cel_unknown_merge`.  Mirrors cel-cpp's
// `EquivalentTypeOrError` envelope.
static inline int absorb_3vl_binary(CelValue* out, const CelValue* a,
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

static inline int absorb_3vl_unary(CelValue* out, const CelValue* a) {
  if (a->kind == CEL_ERROR || a->kind == CEL_UNKNOWN) {
    *out = *a;
    return 1;
  }
  return 0;
}

// Same-kind type check.  Wrong kind on either operand → poison and
// return 1 (skip math).
static inline int require_kinds(CelValue* out, const CelValue* a,
                                const CelValue* b, uint32_t want) {
  if (a->kind != want || b->kind != want) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  return 0;
}

static inline void write_int(CelValue* out, int64_t v) {
  out->kind = CEL_INT;
  out->payload.i = v;
}
static inline void write_uint(CelValue* out, uint64_t v) {
  out->kind = CEL_UINT;
  out->payload.u = v;
}
static inline void write_double(CelValue* out, double v) {
  out->kind = CEL_DOUBLE;
  out->payload.d = v;
}
static inline void write_bool(CelValue* out, int b) {
  out->kind = CEL_BOOL;
  out->payload.b = b ? 1 : 0;
}

// Raw byte-pointer equality.  Processes 8 bytes per iteration via an
// unaligned u64 load (the wasm spec guarantees misaligned i64.load is
// legal and traps-free; engines lower it as a single i64.load).
// Falls back to a byte loop for the 0–7 byte tail so we never read
// past the caller's range.  __builtin_memcmp on a constant length of
// 8 lowers under wasi-sdk's libc + LTO to a load+load+xor+i64.eqz on
// wasm32 and to the platform memcmp's fast path on the native build.
static inline int cel_byteptr_equal_(const uint8_t* pa, const uint8_t* pb,
                                     uint32_t n) {
  while (n >= 8) {
    if (__builtin_memcmp(pa, pb, 8) != 0) return 0;
    pa += 8;
    pb += 8;
    n -= 8;
  }
  while (n--) {
    if (*pa++ != *pb++) return 0;
  }
  return 1;
}

// Byte-span equality over the shared linear memory.  Unified version
// of the legacy `spans_equal` / `span_eq` helpers (split-plan Open
// Question #3).
static inline int spans_equal(CelSpan a, CelSpan b) {
  if (a.len != b.len) return 0;
  if (a.len == 0) return 1;
  const uint8_t* base = cel_memory_base_();
  return cel_byteptr_equal_(base + a.ptr, base + b.ptr, a.len);
}

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_INTERNAL_H_
