// Shared SwissTable hash kernel for arena maps.
//
// FROZEN, dual-build, correctness-critical.  This header is the single
// source of truth for the map index's hash / H1 / H2 / control-byte /
// SWAR-probe primitives, included verbatim by BOTH builds:
//
//   - the wasm32 runtime (`cel_runtime.c` and the index kernels), and
//   - the native host compiler (`compiler/codegen` baking a static
//     index into linear memory at codegen time).
//
// Both sides MUST compute byte-identical results, or a codegen-baked
// index won't match runtime lookups (a silent miscompile surfacing as a
// spurious `no_such_key`).  Everything here is therefore portable and
// deterministic: NO `absl::Hash` / `std::hash` (seeded, non-portable),
// NO SIMD, NO strict-aliasing violations.  Same kernel, same bytes.
//
// Design: `doc/implementation-plan/rewrite/m32-swisstable-map-index.md`
//   §3.2 (index block layout), §4 (SWAR group probing),
//   §5 (numeric-key hash canonicalization), §6 (shared-kernel rationale).
//
// ── THE LOAD-BEARING INVARIANT (§5) ──────────────────────────────────
//
// Keys that the runtime's `cel_value_eq` (`cel_runtime.c:655`,
// dispatching through `numeric_compare_kernel`, `cel_compare.c:157`)
// considers EQUAL MUST hash IDENTICALLY.  A hash collision is harmless
// (the probe re-checks equality via `cel_value_eq`); a false *miss* is a
// bug.  Canonicalization here guarantees no false miss; it is NOT a
// general-purpose value hash.

#ifndef CELWASM_RUNTIME_CEL_MAP_HASH_H_
#define CELWASM_RUNTIME_CEL_MAP_HASH_H_

#include <stdint.h>
#include <string.h>

#include "runtime/cel_data.h"      // CelValue, CelKind, CelSpan, the LE guard
#include "runtime/cel_internal.h"  // cel_memory_base_

#ifdef __cplusplus
extern "C" {
#endif

// ─────────────────────────────────────────────────────────────────────
// §4 — SWAR group probing (portable, no SIMD).
//
// We use Abseil's `GroupPortableImpl` (8-byte SWAR via `uint64_t`), NOT
// SSE2 and NOT the wasm SIMD proposal — the runtime is wasm32 and we do
// not assume SIMD.  The *layout* (control bytes, cloned-byte mirror) is
// what must be frozen; the scan strategy is an implementation detail
// (wasm v128 is a future option that does not change the layout).
// ─────────────────────────────────────────────────────────────────────

// One SWAR group is 8 control bytes wide (one `uint64_t`).
enum { kGroupWidth = 8 };

// Empty-slot control byte: high bit set, the rest arbitrary.  A full
// slot's control byte is `cel_h2(h)` in range 0x00..0x7F (top bit
// clear), so a 7-bit H2 can never spuriously match kEmpty.
enum { kEmpty = 0x80 };

// Deleted-slot control byte.  NEVER written by this kernel: arena maps
// are pre-sized and never grow, so there are no tombstones.  Declared
// only to document the Abseil control-byte vocabulary; its absence is
// why `group_match_empty` (MaskEmpty, shift-6) — not MaskEmptyOrDeleted
// (shift-7) — is the correct probe-stop test.
enum { kDeleted = 0xFE };

// SWAR broadcast constants: 0x01 / 0x80 in every byte lane.
static const uint64_t kLsbs = 0x0101010101010101ULL;
static const uint64_t kMsbs = 0x8080808080808080ULL;

// Split a 64-bit hash into the slot-index bits (H1) and the 7-bit
// control byte (H2).  H1 indexes the (power-of-two) slot array via
// `cel_h1(h) & (num_slots - 1)`; H2 is the per-slot control byte whose
// top bit is always clear (so it never aliases kEmpty).
static inline uint64_t cel_h1(uint64_t h) {
  return h >> 7;
}
static inline uint8_t cel_h2(uint64_t h) {
  return (uint8_t)(h & 0x7F);
}

// Abseil Match: returns a mask whose byte i has its MSB set iff control
// byte i of `ctrl` equals `h2`.  Lane index of a match is
// `__builtin_ctzll(mask) >> 3`; clear it with `mask &= mask - 1`.
static inline uint64_t group_match(uint64_t ctrl, uint8_t h2) {
  uint64_t x = ctrl ^ (kLsbs * (uint64_t)h2);
  return (x - kLsbs) & ~x & kMsbs;
}

// Abseil GroupPortableImpl::MaskEmpty (shift-6): byte i's MSB is set iff
// control byte i == kEmpty (0x80).  We never write kDeleted, so MaskEmpty
// — not MaskEmptyOrDeleted (shift-7, which also matches kDeleted) — is
// the correct stop test: probing stops at the first empty slot.
//
//   ctrl byte 0x80 (empty): (~0x80 << 6) keeps its MSB set  → matched.
//   ctrl byte 0x00..0x7f (full, top bit clear): ctrl's MSB is already 0
//     → cleared by the leading `ctrl &` → not matched.
static inline uint64_t group_match_empty(uint64_t ctrl) {
  return ctrl & (~ctrl << 6) & kMsbs;
}

// Load 8 control bytes starting at `p` into a host-endianness uint64_t.
// `memcpy` is the strict-aliasing-clean, UBSan-safe spelling of an
// unaligned load (mirrors `cel_byteptr_equal_` in cel_internal.h); under
// wasi-sdk + LTO it lowers to a single `i64.load`.  The control bytes are
// compared lane-by-lane, so the host's LE byte order (build-guarded at
// `cel_data.h:208`) makes lane i == control byte i, identically on both
// builds.
static inline uint64_t cel_group_load(const uint8_t* p) {
  uint64_t g;
  memcpy(&g, p, 8);
  return g;
}

// ─────────────────────────────────────────────────────────────────────
// §5 — numeric-key hash canonicalization + the portable mixer.
// ─────────────────────────────────────────────────────────────────────

// Portable 64-bit mixer (SplitMix64 finalizer).  Deterministic and
// arch-independent — same input bytes → same output bytes on both
// builds.  Per §5 it need NOT match Abseil's mixer (the index is private
// to one map built and queried by the same kernel) but it MUST NOT be a
// seeded/non-portable hash.
static inline uint64_t cel_mix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  x = x ^ (x >> 31);
  return x;
}

// Kind salts.  Distinct numeric kinds that compare EQUAL (int/uint/double
// of the same mathematical integer) must share a token, so the numeric
// canonicalization below does NOT salt by kind — it folds them to one of
// two integer tokens.  bool / string / bytes get their own salt because
// `cel_value_eq` never matches them across the kind boundary:
//   - bool true vs int 1  → 0 (cel_runtime.c:662 guards bool==bool only)
//   - string "x" vs bytes "x" → 0 (cel_runtime.c:659/665 guard each kind)
// Salting them apart is correctness-neutral (they never compare equal so
// a shared hash would only be a harmless collision) but reduces those
// collisions.
static const uint64_t kCelHashSaltBool = 0x00B001B001B001B0ULL;
static const uint64_t kCelHashSaltStr = 0x0057812345678900ULL;
static const uint64_t kCelHashSaltBytes = 0x00B47E5678901234ULL;

// Sentinel hash for a double key that cannot compare equal to ANY stored
// int/uint key: non-integral, NaN, ±Inf, or out of [INT64_MIN, UINT64_MAX]
// integer range.  §5: such a double probes, finds no `cel_value_eq`
// confirmation, and correctly misses.  Mixed so it lands in a
// well-distributed slot rather than always slot 0.
static inline uint64_t cel_hash_double_sentinel(void) {
  return cel_mix64(0xDEADD0D0FFFFFFFFULL);
}

// Canonical token for a value whose mathematical integer lies in
// [0, INT64_MAX]: `int N`, `uint N`, and an integral `double N.0` all
// reduce here, so they collide as §5 requires.  The token is the
// magnitude itself (always representable as uint64 in this range).
static inline uint64_t cel_hash_int_token(uint64_t magnitude_nonneg) {
  return cel_mix64(magnitude_nonneg);
}

// Canonical token for a NEGATIVE int64 (no uint or in-range double folds
// here from the positive side; a negative double folds here only when it
// is an exact integer in int64 range).  The magnitude is computed without
// UB even for INT64_MIN via `(uint64_t)(-(v + 1)) + 1` (§5), then tagged
// into a disjoint code space from the non-negative tokens.
static inline uint64_t cel_hash_neg_int_token(int64_t v) {
  uint64_t magnitude = (uint64_t)(-(v + 1)) + 1u;  // |v|, INT64_MIN-safe
  return cel_mix64(magnitude ^ 0x8000000000000000ULL);
}

// Canonical token for a value in (INT64_MAX, UINT64_MAX]: only a `uint`
// key, or a `double` that truncates here, can match.  `uint 2^63` and
// `double(2^63)` take the same token.  Disjoint from the non-negative int
// token space because no int64 reaches this magnitude.
static inline uint64_t cel_hash_uint_high_token(uint64_t v) {
  return cel_mix64(v ^ 0x4000000000000000ULL);
}

// Hash an integral numeric magnitude that is known non-negative, routing
// it to the int-token space [0, INT64_MAX] or the uint-only space
// (INT64_MAX, UINT64_MAX].  Shared by the int / uint / double arms so
// `int N`, `uint N`, and `double N.0` provably collide.
static inline uint64_t cel_hash_nonneg_integer(uint64_t v) {
  if (v <= (uint64_t)INT64_MAX) return cel_hash_int_token(v);
  return cel_hash_uint_high_token(v);
}

// Byte hash over a length-delimited span read through the shared linear
// memory (§5): embedded NULs and multibyte UTF-8 are included; we never
// `strlen`.  FNV-1a-style accumulation then a final mix; deterministic
// and identical on both builds.  `salt` distinguishes string from bytes.
static inline uint64_t cel_hash_bytes(CelSpan s, uint64_t salt) {
  const uint8_t* base = cel_memory_base_();
  const uint8_t* p = base + s.ptr;
  uint64_t h = salt ^ 0xCBF29CE484222325ULL;  // FNV offset basis
  for (uint32_t i = 0; i < s.len; ++i) {
    h ^= (uint64_t)p[i];
    h *= 0x00000100000001B3ULL;  // FNV prime
  }
  // Fold the length in so "" and a run of NULs never alias purely on
  // content, then finalize through the portable mixer.
  return cel_mix64(h ^ ((uint64_t)s.len << 1));
}

// The canonicalizing key hash (§5).  Returns a 64-bit hash such that any
// two keys `cel_value_eq` considers equal hash identically.
//
//   - bool   → canonical 0/1 with a kind salt (distinct from numeric
//              0/1, which `cel_value_eq` requires; cel_runtime.c:662).
//   - int / uint / integral-in-range double of the same mathematical
//              value → the same integer token (cel_runtime.c:656 +
//              numeric_compare_kernel, cel_compare.c:157).
//   - non-integral / NaN / ±Inf / out-of-range double → the fixed
//              non-matching sentinel (cmp_int_vs_double cel_compare.c:132
//              / cmp_uint_vs_double :139 report equal only for an exact
//              in-range integer, so a sentinel never causes a false miss).
//   - string / bytes → length-delimited byte hash through linear memory,
//              salted apart (cel_runtime.c:659/665 guard each kind).
//
// Stored keys are only bool/int/uint/string (double rejected on insert
// by is_valid_map_key_kind, cel_runtime.c:21), so the double arm matters
// for LOOKUP keys.  The `≥ 2^53` rounding ambiguity (a double equal to a
// RANGE of int64s under the lossy `cel_value_eq`) is handled by the
// LOOKUP kernel's linear fallback (§5.1) — NOT here — but an in-range
// integral double must still hash to the int token it exactly represents,
// which it does.
// Hash a double key by the integer it EXACTLY represents (so an in-range
// integral double folds to the same token as the int/uint of that value),
// or the non-matching sentinel for non-integral / NaN / ±Inf / out-of-
// integer-range.  `d == d` rejects NaN; the cast round-trip rejects
// non-integral and ±Inf (their int cast does not round-trip).  The int64
// representation is tried first (covers negatives and the [0, INT64_MAX]
// overlap), then the uint64 representation for the (INT64_MAX, UINT64_MAX]
// tail.  The `≥ 2^53` lossy-range ambiguity is the LOOKUP kernel's linear
// fallback (§5.1), not this hash's concern — see `cel_map_key_hash`.
static inline uint64_t cel_hash_double_key(double d) {
  if (d == d) {
    if (d >= -9223372036854775808.0 /* (double)INT64_MIN */ &&
        d < 9223372036854775808.0 /* 2^63 == (double)INT64_MAX+1 */) {
      int64_t i = (int64_t)d;
      if ((double)i == d) {
        if (i >= 0) return cel_hash_nonneg_integer((uint64_t)i);
        return cel_hash_neg_int_token(i);
      }
    } else if (d >= 0.0 && d < 18446744073709551616.0 /* 2^64 */) {
      uint64_t u = (uint64_t)d;
      if ((double)u == d) return cel_hash_nonneg_integer(u);
    }
  }
  return cel_hash_double_sentinel();
}

static inline uint64_t cel_map_key_hash(const CelValue* key) {
  switch (key->kind) {
    case CEL_BOOL:
      return cel_mix64(kCelHashSaltBool ^ (key->payload.b != 0 ? 1u : 0u));

    case CEL_INT: {
      int64_t v = key->payload.i;
      if (v >= 0) return cel_hash_nonneg_integer((uint64_t)v);
      return cel_hash_neg_int_token(v);
    }

    case CEL_UINT:
      return cel_hash_nonneg_integer(key->payload.u);

    case CEL_DOUBLE:
      return cel_hash_double_key(key->payload.d);

    case CEL_STRING:
      return cel_hash_bytes(key->payload.s, kCelHashSaltStr);

    case CEL_BYTES:
      return cel_hash_bytes(key->payload.s, kCelHashSaltBytes);

    default:
      // Map keys are only bool/int/uint/string (insert) plus a double
      // lookup key (above).  Any other kind reaching the hash is an
      // invariant violation upstream; return a stable non-matching
      // sentinel so the probe misses rather than miscomputing.  (Not an
      // ABSL_CHECK: this header is also compiled into the freestanding
      // wasm32 runtime, where ABSL is unavailable.)
      return cel_hash_double_sentinel();
  }
}

// ─────────────────────────────────────────────────────────────────────
// §3.2 — index sizing.
// ─────────────────────────────────────────────────────────────────────

// `num_slots` = smallest power of two ≥ ceil(count / (7/8)), floored at
// kGroupWidth = 8 so one SWAR group is always a full load.  Power-of-two
// so `cel_h1(h) & (num_slots - 1)` replaces a modulo.  Max load factor
// 7/8 (Abseil's kMaxLoadFactor).  Worked boundaries (§3.2):
//   count 8 → 16, 14 → 16, 15 → 32, 56 → 64, 64 → 128.
static inline uint32_t cel_map_index_num_slots(uint32_t count) {
  // ceil(8 * count / 7).  count is a map entry count (≤ capacity, far
  // below 2^32 / 8), so 64-bit arithmetic never overflows.
  uint64_t needed = (((uint64_t)count * 8u) + 6u) / 7u;
  if (needed < kGroupWidth) needed = kGroupWidth;
  // Smallest power of two ≥ needed.
  uint32_t slots = kGroupWidth;
  while ((uint64_t)slots < needed) {
    slots <<= 1;
  }
  return slots;
}

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_MAP_HASH_H_
