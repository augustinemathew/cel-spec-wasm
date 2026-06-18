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

#include "runtime/cel_3vl.h"
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

// 3VL absorption for STRICT ops.  Returns 1 (and overwrites *out) if
// either operand is ERROR / UNKNOWN.  ERROR dominates UNKNOWN across
// operands, with left-bias within each class; both-UNKNOWN merges
// the attribute-id sets via `cel_unknown_merge` so neither operand's
// provenance is dropped.  Matches cel-cpp's `NoOverloadResult`
// (eval/eval/function_step.cc), which propagates the first ErrorValue
// argument and otherwise MERGES the unknown arguments; oracle-pinned
// by PartialEvalOracle.{UnknownPlusErrorIsError,
// ErrorPlusUnknownIsError} (testdata/cel_cpp_oracle_test.cc) and
// UnknownPayloadOracle.{Add,BareVarsAdd}BothUnknownMergesBothAttributes
// (testdata/cel_cpp_oracle_unknown_payload_test.cc).  (Logic ops use
// the opposite UNKNOWN-over-ERROR rule — see cel_3vl.h.)
//
// Callers must return immediately on 1: the merge path calls
// `arena_alloc`, which on wasm32 may `memory.grow` and relocate the
// linear-memory base, invalidating every CelValue* the caller holds.
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
    if (b->kind == CEL_UNKNOWN) {
      // Recover the slot offsets from the pointers — every CelValue a
      // kernel touches lives in the shared linear memory.
      uint8_t* base = cel_memory_base_();
      cel_unknown_merge((uint32_t)((const uint8_t*)out - base),
                        (uint32_t)((const uint8_t*)a - base),
                        (uint32_t)((const uint8_t*)b - base));
      return 1;
    }
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

// Raw byte-pointer equality.  Processes 8 bytes per iteration via a pair
// of unaligned u64 loads compared as whole words.  The `memcpy` is the
// portable spelling of an unaligned load: the wasm spec guarantees
// misaligned i64.load is legal and trap-free, so under wasi-sdk + LTO
// each `memcpy(&w, p, 8)` lowers to one `i64.load` and the comparison to
// a single `i64.eq` (verified by disassembly; `__builtin_memcmp(p,q,8)`
// does NOT — it lowers to eight separate `i32.load8_u`).  On the native
// build the same pattern is the platform's word-at-a-time compare.  A
// byte loop handles the 0–7 byte tail so we never read past the caller's
// range.
static inline int cel_byteptr_equal_(const uint8_t* pa, const uint8_t* pb,
                                     uint32_t n) {
  while (n >= 8) {
    uint64_t wa;
    uint64_t wb;
    __builtin_memcpy(&wa, pa, 8);
    __builtin_memcpy(&wb, pb, 8);
    if (wa != wb) return 0;
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
