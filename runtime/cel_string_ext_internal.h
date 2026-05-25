// Private cross-TU helpers for `cel_string_ext_*.cc`.
//
// Per `doc/implementation-plan/rewrite/m12-string-ext.md` §4.1, the
// string_ext kernels live in per-topic TUs (codepoint, search,
// list, quote) so each TU stays under the readability budget.
// Every helper used by 2+ TUs lives here as `static inline` so the
// `-O3 -flto` cc_library link picks up cross-TU inlining (mirrors
// `cel_internal.h`'s contract for the C runtime split).
//
// Helpers only used inside one TU live in that TU's anonymous
// namespace — not here.  The split rule:
//
//   - In this header     → 3VL envelope, slot writers, UTF-8
//                          iteration (used by codepoint + search +
//                          list + quote + future format).
//   - In a TU's anon ns  → topic-specific algorithms
//                          (IndexOfImpl, AsciiFoldInto,
//                          BuildReplaced, IsUnicodeWhitespace, …).
//
// Not exported from `cel_string_ext.h` — call sites that include
// the public header don't need these.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_EXT_INTERNAL_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_EXT_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "absl/strings/string_view.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_memory.h"

namespace celwasm::string_ext_internal {

// ───────────────────────────────────────────────────────────────
// 3VL / poison envelope helpers.
// ───────────────────────────────────────────────────────────────

inline void Poison(CelValue* out, uint32_t err_code) {
  out->kind = CEL_ERROR;
  out->payload.err = err_code;
}

inline bool Absorb3vlUnary(CelValue* out, const CelValue* in) {
  if (in->kind == CEL_ERROR || in->kind == CEL_UNKNOWN) {
    *out = *in;
    return true;
  }
  return false;
}

inline bool Absorb3vlBinary(CelValue* out, const CelValue* a,
                            const CelValue* b) {
  if (Absorb3vlUnary(out, a)) return true;
  if (Absorb3vlUnary(out, b)) return true;
  return false;
}

// ───────────────────────────────────────────────────────────────
// Span / slot writers.
// ───────────────────────────────────────────────────────────────

inline absl::string_view BorrowSpan(const CelSpan& s) {
  return {reinterpret_cast<const char*>(cel_mem_base()) + s.ptr, s.len};
}

// Writes a CEL_STRING into `out` whose payload.s names a freshly-
// allocated copy of `bytes`.  Empty input is encoded as `{ptr=0,
// len=0}` (the canonical empty-string sentinel; the runtime treats
// ptr==0 as "no backing bytes", consistent with `arena_alloc(0)`).
// Returns false on arena OOM (caller has already been poisoned).
inline bool WriteStringFromBytes(CelValue* out, const char* data, size_t len) {
  if (len == 0) {
    out->kind = CEL_STRING;
    out->payload.s.ptr = 0;
    out->payload.s.len = 0;
    return true;
  }
  const uint32_t off = arena_alloc(static_cast<uint32_t>(len));
  if (off == 0) {
    Poison(out, CEL_ERR_OVERFLOW);
    return false;
  }
  std::memcpy(cel_mem_base() + off, data, len);
  out->kind = CEL_STRING;
  out->payload.s.ptr = off;
  out->payload.s.len = static_cast<uint32_t>(len);
  return true;
}

// Writes a CEL_STRING into `out` reusing an existing span without
// allocating.  Used when no transformation is needed (lowerAscii on
// an already-lower input, trim with nothing to strip, …).
inline void WriteStringFromSpan(CelValue* out, uint32_t ptr, uint32_t len) {
  out->kind = CEL_STRING;
  out->payload.s.ptr = len == 0 ? 0u : ptr;
  out->payload.s.len = len;
}

// Output a subspan of an existing string in linear memory without
// allocating.  `byte_off` and `byte_len` are absolute byte offsets
// into the source string (NOT linear-memory offsets).
inline void WriteSubspan(CelValue* out, uint32_t source_ptr, uint32_t byte_off,
                         uint32_t byte_len) {
  out->kind = CEL_STRING;
  out->payload.s.ptr = byte_len == 0 ? 0u : source_ptr + byte_off;
  out->payload.s.len = byte_len;
}

// Write an int64 result.
inline void WriteInt(CelValue* out, int64_t v) {
  out->kind = CEL_INT;
  out->payload.i = v;
}

// ───────────────────────────────────────────────────────────────
// UTF-8 iteration.
// ───────────────────────────────────────────────────────────────

// Inner shared body for the 2/3/4-byte sequence path of UTF-8
// decoding.  `expected` is the byte count from the lead-byte class;
// `lead_mask` strips the lead's class bits leaving the data bits.
// Returns the consumed byte count, falling back to a 1-byte advance
// on any truncation / bad continuation byte (cel-cpp's
// `internal::Utf8Decode` does the same on malformed input — keeps
// the raw byte and moves on, deterministically).
inline size_t Utf8DecodeMulti(const uint8_t* p, const uint8_t* end,
                              size_t expected, uint8_t lead_mask,
                              uint32_t* cp) {
  if (p + expected > end) {
    *cp = *p;
    return 1;
  }
  auto acc = static_cast<uint32_t>(*p & lead_mask);
  for (size_t k = 1; k < expected; ++k) {
    const uint8_t bk = p[k];
    if ((bk & 0xC0) != 0x80) {
      *cp = *p;
      return 1;
    }
    acc = (acc << 6) | (bk & 0x3F);
  }
  *cp = acc;
  return expected;
}

// Decode one UTF-8 code point starting at `p` (with `end` as the
// hard limit).  Writes the decoded code point to `*cp` and returns
// the number of bytes consumed.  Strings in CEL are byte sequences
// with UTF-8 validation enforced at conversion boundaries
// (`bytes_to_string`), not inside every op — malformed input falls
// back to a 1-byte advance carrying the raw byte (see
// `Utf8DecodeMulti` above).
inline size_t Utf8Decode(const uint8_t* p, const uint8_t* end, uint32_t* cp) {
  const uint8_t b0 = *p;
  if (b0 < 0x80) {
    *cp = b0;
    return 1;
  }
  if ((b0 & 0xE0) == 0xC0) return Utf8DecodeMulti(p, end, 2, 0x1F, cp);
  if ((b0 & 0xF0) == 0xE0) return Utf8DecodeMulti(p, end, 3, 0x0F, cp);
  if ((b0 & 0xF8) == 0xF0) return Utf8DecodeMulti(p, end, 4, 0x07, cp);
  *cp = b0;
  return 1;
}

// Walks a UTF-8 span backwards from `end`, returning the byte
// position where the previous code-point starts.  Mirrors cel-cpp's
// `Reverse` (`common/values/string_value.cc::Reverse`): the
// continuation-byte mask `(*ptr & 0xC0) == 0x80` distinguishes
// trailing bytes from leads.  `begin` is the hard lower bound; the
// loop guarantees `ptr > begin` after each decrement.
inline const uint8_t* PrevCodepoint(const uint8_t* end, const uint8_t* begin) {
  const uint8_t* p = end - 1;
  while (p > begin && (*p & 0xC0) == 0x80) {
    --p;
  }
  return p;
}

}  // namespace celwasm::string_ext_internal

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_EXT_INTERNAL_H_
