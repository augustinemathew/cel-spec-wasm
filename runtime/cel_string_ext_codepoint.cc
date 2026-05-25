// M12 Slice A — code-point string_ext kernels: charAt, lowerAscii,
// upperAscii, trim, reverse.
//
// Per the multi-TU split in §4.1 of `m12-string-ext.md`, the
// cross-cutting helpers (3VL envelope, slot writers, UTF-8
// iteration) live in `cel_string_ext_internal.h`; this TU only
// owns the per-kernel bodies and the topic-specific helpers
// (`IsUnicodeWhitespace` for trim, `AsciiFoldInto` for the case-
// fold pair).
//
// Implementations track cel-cpp's `extensions/strings.cc` +
// `common/values/string_value.cc` semantics line-for-line so the
// conformance fixture (`tests/simple/testdata/string_ext.textproto`)
// matches byte-for-byte.

#include "runtime/cel_string_ext.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "absl/strings/ascii.h"
#include "absl/strings/string_view.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_string_ext_internal.h"

namespace {

using celwasm::string_ext_internal::Absorb3vlUnary;
using celwasm::string_ext_internal::BorrowSpan;
using celwasm::string_ext_internal::Poison;
using celwasm::string_ext_internal::PrevCodepoint;
using celwasm::string_ext_internal::Utf8Decode;
using celwasm::string_ext_internal::WriteStringFromBytes;
using celwasm::string_ext_internal::WriteStringFromSpan;

// cel-cpp's whitespace classifier for `trim` — verbatim mirror of
// `IsUnicodeWhitespace` in `common/values/string_value.cc`.  The
// `<= 0x0020` branch covers ASCII space + the C0 controls cel-cpp
// trims (HT/LF/VT/FF/CR).
bool IsUnicodeWhitespace(uint32_t c) {
  if (c <= 0x0020) {
    return c == 0x0020 || (c >= 0x0009 && c <= 0x000D);
  }
  if (c > 0x3000) return false;
  if (c == 0x0085 || c == 0x00a0 || c == 0x1680) return true;
  if (c >= 0x2000 && c <= 0x200a) return true;
  return c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
         c == 0x3000;
}

// Common path for ASCII case fold.  `fold` is the per-byte map
// applied to ASCII bytes the predicate flags; non-ASCII bytes and
// ASCII bytes the predicate rejects pass through verbatim.  Returns
// without allocating when no byte would change.
void AsciiFoldInto(CelValue* out, absl::string_view text,
                   bool (*needs_fold)(unsigned char),
                   char (*fold)(unsigned char)) {
  bool any = false;
  for (const char c : text) {
    if (needs_fold(static_cast<unsigned char>(c))) {
      any = true;
      break;
    }
  }
  if (!any) {
    out->kind = CEL_STRING;
    if (text.empty()) {
      out->payload.s.ptr = 0;
      out->payload.s.len = 0;
    } else {
      out->payload.s.ptr = static_cast<uint32_t>(
          reinterpret_cast<const uint8_t*>(text.data()) - cel_mem_base());
      out->payload.s.len = static_cast<uint32_t>(text.size());
    }
    return;
  }
  const uint32_t off = arena_alloc(static_cast<uint32_t>(text.size()));
  if (off == 0) {
    Poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint8_t* dst = cel_mem_base() + off;
  for (size_t k = 0; k < text.size(); ++k) {
    const auto c = static_cast<unsigned char>(text[k]);
    dst[k] = needs_fold(c) ? static_cast<uint8_t>(fold(c)) : c;
  }
  out->kind = CEL_STRING;
  out->payload.s.ptr = off;
  out->payload.s.len = static_cast<uint32_t>(text.size());
}

}  // namespace

// ───────────────────────────────────────────────────────────────
// charAt
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_char_at_at_vv(uint32_t out_slot, uint32_t s_slot,
                                         uint32_t i_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* i = cel_value_at(i_slot);
  if (Absorb3vlUnary(out, s)) return;
  if (Absorb3vlUnary(out, i)) return;
  if (s->kind != CEL_STRING || i->kind != CEL_INT) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const int64_t pos = i->payload.i;
  if (pos < 0) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  const absl::string_view text = BorrowSpan(s->payload.s);
  const auto* p = reinterpret_cast<const uint8_t*>(text.data());
  const uint8_t* const end = p + text.size();
  int64_t remaining = pos;
  while (p < end) {
    uint32_t cp = 0;
    const size_t units = Utf8Decode(p, end, &cp);
    if (remaining == 0) {
      WriteStringFromBytes(out, reinterpret_cast<const char*>(p), units);
      return;
    }
    p += units;
    --remaining;
  }
  // `pos == size_in_codepoints` is the canonical "end of string"
  // sentinel — empty string, not error (cel-cpp StringValue::CharAt
  // line ~1483).  Strictly past the end is an error.
  if (remaining == 0) {
    WriteStringFromSpan(out, 0, 0);
    return;
  }
  Poison(out, CEL_ERR_INVALID_ARGUMENT);
}

// ───────────────────────────────────────────────────────────────
// lowerAscii / upperAscii
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_lower_ascii_at_v(uint32_t out_slot,
                                            uint32_t s_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  if (Absorb3vlUnary(out, s)) return;
  if (s->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  AsciiFoldInto(
      out, BorrowSpan(s->payload.s),
      [](unsigned char c) -> bool {
        return absl::ascii_isupper(c) != 0;
      },
      [](unsigned char c) -> char {
        return absl::ascii_tolower(c);
      });
}

extern "C" void cel_string_upper_ascii_at_v(uint32_t out_slot,
                                            uint32_t s_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  if (Absorb3vlUnary(out, s)) return;
  if (s->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  AsciiFoldInto(
      out, BorrowSpan(s->payload.s),
      [](unsigned char c) -> bool {
        return absl::ascii_islower(c) != 0;
      },
      [](unsigned char c) -> char {
        return absl::ascii_toupper(c);
      });
}

// ───────────────────────────────────────────────────────────────
// trim
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_trim_at_v(uint32_t out_slot, uint32_t s_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  if (Absorb3vlUnary(out, s)) return;
  if (s->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const absl::string_view text = BorrowSpan(s->payload.s);
  const auto* const begin = reinterpret_cast<const uint8_t*>(text.data());
  const uint8_t* const end = begin + text.size();

  // Left-trim: walk forward consuming whitespace code points.
  const uint8_t* lhs = begin;
  while (lhs < end) {
    uint32_t cp = 0;
    const size_t units = Utf8Decode(lhs, end, &cp);
    if (!IsUnicodeWhitespace(cp)) break;
    lhs += units;
  }
  if (lhs == end) {
    WriteStringFromSpan(out, 0, 0);
    return;
  }
  // Right-trim: cel-cpp scans left-to-right tracking last non-ws
  // end (lines ~1004-1015).  Mirror that — code-point boundaries
  // are unambiguous when walking forward, ambiguous when walking
  // back from a span that may end mid-sequence.
  const uint8_t* p = lhs;
  const uint8_t* last_non_ws_end = lhs;
  while (p < end) {
    uint32_t cp = 0;
    const size_t units = Utf8Decode(p, end, &cp);
    if (!IsUnicodeWhitespace(cp)) last_non_ws_end = p + units;
    p += units;
  }
  const auto out_ptr = static_cast<uint32_t>(lhs - cel_mem_base());
  const auto out_len = static_cast<uint32_t>(last_non_ws_end - lhs);
  WriteStringFromSpan(out, out_ptr, out_len);
}

// ───────────────────────────────────────────────────────────────
// reverse
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_reverse_at_v(uint32_t out_slot, uint32_t s_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  if (Absorb3vlUnary(out, s)) return;
  if (s->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const absl::string_view text = BorrowSpan(s->payload.s);
  if (text.empty()) {
    WriteStringFromSpan(out, 0, 0);
    return;
  }
  const uint32_t off = arena_alloc(static_cast<uint32_t>(text.size()));
  if (off == 0) {
    Poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  // Walk source end -> begin; for each code point, copy its bytes
  // forward into the output cursor.  Mirrors cel-cpp's `Reverse`
  // (lines ~1175-1186); the continuation-byte mask in
  // `PrevCodepoint` finds each code point's start.
  uint8_t* const dst_base = cel_mem_base() + off;
  uint8_t* dst = dst_base;
  const auto* const begin = reinterpret_cast<const uint8_t*>(text.data());
  const uint8_t* ptr = begin + text.size();
  while (ptr > begin) {
    const uint8_t* const cp_end = ptr;
    ptr = PrevCodepoint(ptr, begin);
    const auto units = static_cast<size_t>(cp_end - ptr);
    std::memcpy(dst, ptr, units);
    dst += units;
  }
  out->kind = CEL_STRING;
  out->payload.s.ptr = off;
  out->payload.s.len = static_cast<uint32_t>(text.size());
}
