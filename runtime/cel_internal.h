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

// Polymorphic VALUE equality — the `==` operator's rule.  Routes
// numeric pairs through the cross-type numeric ladder per langdef
// §"Equality", which places int / uint / double on one continuous
// number line and tolerates the precision loss of the int→double
// cast: `9007199254740993 == 9007199254740992.0` is TRUE, matching
// cel-cpp's `internal::Number::operator==`.  Used for the `==`
// operator, list membership, and list/map element compares.
//
// NOT for map KEYS — see `cel_map_key_eq` below.
int cel_value_eq(const CelValue* a, const CelValue* b);

// Map-KEY equality — the lookup rule, which is LOSSLESS, not rounding.
//
// cel-cpp converts a query key to the stored key's type only when the
// conversion round-trips exactly, then compares exactly
// (`internal/number.h` `LosslessConvertibleToIntVisitor` /
// `LosslessConvertibleToUintVisitor`, driven from
// `eval/eval/container_access_step.cc::LookupInMap`,
// `runtime/standard/container_membership_functions.cc`'s
// `doubleKeyInSet` / `intKeyInSet` / `uintKeyInSet`, and
// `runtime/standard/equality_functions.cc::CheckAlternativeNumericType`).
//
// The two rules diverge above 2^53, where one double is the rounded
// image of a RANGE of int64s: `9007199254740992.0` and the int
// `9007199254740993` are `cel_value_eq`-equal but are DISTINCT map
// keys, so a lookup with the double must miss.  Oracle-pinned by
// `MapKeyNumericCrossType.DoubleAt2Pow53MissesNeighborIntKey` (miss)
// against `.IntPlus1EqDoubleAt2Pow53` (`==` says equal) in
// `testdata/cel_cpp_oracle_test.cc`.
//
// Non-numeric pairs fall through to `cel_value_eq` (string / bool /
// bytes / null / … are exact there already).
int cel_map_key_eq(const CelValue* a, const CelValue* b);

// The numeric half of `cel_map_key_eq`, exposed for the host-side
// trampolines (`eval/internal/cel_host.cc`), whose CelValues carry
// spans into the wasm instance's linear memory rather than this
// build's — so they may use the numeric predicate but never the
// span-reading one.  Both operands must be numeric kinds; a
// non-numeric operand compares unequal.
int cel_numeric_key_eq(const CelValue* a, const CelValue* b);

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

// ── Anchor-byte scans (shared by contains / indexOf / lastIndexOf) ──
//
// 8-byte SWAR memchr.  musl's memchr scans one `size_t` (4 bytes on
// wasm32) per iteration; wasm has native i64 ALU ops and tolerates
// unaligned loads, so scanning a uint64_t per iteration halves the
// trip count with the same has-zero bit trick and no alignment
// prologue.  In `w ^ pat`, bytes equal to `c` become 0x00;
// `(x - 0x01…) & ~x & 0x80…` sets the high bit of every zero byte.
// Borrow across lanes can corrupt flags ABOVE the lowest zero byte,
// so only the lowest flag is trusted — exactly what a forward scan
// consumes; wasm is little-endian, so ctz locates that byte.
//
// Portable path: `cel_anchor_memchr_` below supersedes it on wasm
// builds carrying `-msimd128` and reuses it for sub-16-byte tails.
static inline const uint8_t* cel_swar_memchr_(const uint8_t* p, uint8_t c,
                                              uint32_t n) {
  const uint64_t ones = 0x0101010101010101ull;
  const uint64_t highs = 0x8080808080808080ull;
  const uint64_t pat = ones * c;
  while (n >= 8) {
    uint64_t w;
    __builtin_memcpy(&w, p, 8);  // single unaligned i64 load
    const uint64_t x = w ^ pat;
    const uint64_t hit = (x - ones) & ~x & highs;
    if (hit) return p + (__builtin_ctzll(hit) >> 3);
    p += 8;
    n -= 8;
  }
  for (; n != 0; --n, ++p) {
    if (*p == c) return p;
  }
  return NULL;
}

#ifdef __wasm_simd128__
#include <wasm_simd128.h>

// 16-byte SIMD memchr: splat the anchor byte, compare a full v128
// lane-wise, and reduce the equality mask to one bit per lane with
// i8x16.bitmask — a hit's lane index is ctz of the mask.
// `wasm_v128_load` is specified alignment-tolerant; sub-16-byte tails
// fall through to the SWAR scan.  Cranelift lowers this loop to the
// host's native vector compare (NEON / SSE).
static inline const uint8_t* cel_anchor_memchr_(const uint8_t* p, uint8_t c,
                                                uint32_t n) {
  const v128_t pat = wasm_i8x16_splat((int8_t)c);
  while (n >= 16) {
    const v128_t w = wasm_v128_load(p);
    const uint32_t mask = wasm_i8x16_bitmask(wasm_i8x16_eq(w, pat));
    if (mask) return p + __builtin_ctz(mask);
    p += 16;
    n -= 16;
  }
  return cel_swar_memchr_(p, c, n);
}

// Length of the leading run of ASCII bytes (< 0x80), stepping WHOLE
// 16-byte blocks only — the return value is a multiple of 16 and the
// caller walks the remainder byte-wise.  i8x16.bitmask of the raw
// block IS the per-lane high-bit mask, so "block is ASCII" is one
// compare against zero.  Used by the UTF-8 search kernels to bulk-
// advance code-point counts across ASCII spans (1 byte == 1 code
// point there); block granularity keeps the exact-decode fallback in
// charge of every non-ASCII byte.
static inline uint32_t cel_ascii_prefix_blocks_(const uint8_t* p, uint32_t n) {
  uint32_t run = 0;
  while (n - run >= 16) {
    const v128_t w = wasm_v128_load(p + run);
    if (wasm_i8x16_bitmask(w) != 0) break;
    run += 16;
  }
  return run;
}
#else
// SIMD unavailable (native builds, or a wasm build without
// -msimd128): the SWAR scan is the anchor path.
static inline const uint8_t* cel_anchor_memchr_(const uint8_t* p, uint8_t c,
                                                uint32_t n) {
  return cel_swar_memchr_(p, c, n);
}

// 8-byte SWAR variant of the ASCII-run scan: a block is ASCII iff no
// byte has its high bit set.
static inline uint32_t cel_ascii_prefix_blocks_(const uint8_t* p, uint32_t n) {
  const uint64_t highs = 0x8080808080808080ull;
  uint32_t run = 0;
  while (n - run >= 8) {
    uint64_t w;
    __builtin_memcpy(&w, p + run, 8);
    if ((w & highs) != 0) break;
    run += 8;
  }
  return run;
}
#endif  // __wasm_simd128__

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_INTERNAL_H_
