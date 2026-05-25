// M12 Slice D — `strings.quote` kernel.  Wraps an input string in
// `"..."` and escapes the 9 named C-style sequences (\a, \b, \f, \n,
// \r, \t, \v, \\, \").  Every other byte (including NUL, every byte
// in [0x01, 0x1F], and every multi-byte UTF-8 lead/continuation pair)
// passes through verbatim — cel-cpp's `StringValue::Quote` does the
// same (`common/values/string_value.cc::AppendQuoteCodePoint` only
// branches on the 9 named code points; the default arm calls
// `Utf8Encode`, which on a code point ≤ 0x7f writes the raw byte).
//
// Conformance fixture rows live in `string_ext.textproto::quote` (21
// rows; spans every escape sequence + verbatim ASCII + multi-byte
// UTF-8).
//
// Per §4.1 of `m12-string-ext.md`, the cross-cutting helpers
// (`Poison`, `Absorb3vlUnary`, `BorrowSpan`, UTF-8 decode) live in
// `cel_string_ext_internal.h`.  Quote-specific helpers (the
// 9-character escape switch) live in this TU's anon namespace.

#include "runtime/cel_string_ext.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "absl/strings/string_view.h"
#include "runtime/cel_arena.h"
#include "runtime/cel_data.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_string_ext_internal.h"

namespace {

using celwasm::string_ext_internal::Absorb3vlUnary;
using celwasm::string_ext_internal::BorrowSpan;
using celwasm::string_ext_internal::Poison;
using celwasm::string_ext_internal::Utf8Decode;
using celwasm::string_ext_internal::WriteStringFromBytes;

// Append one code point with cel-cpp's escape policy: the 9 named
// sequences get a two-byte `\<c>` form; every other code point's
// original UTF-8 bytes pass through unchanged.  Verbatim emission
// (rather than re-encoding via Utf8Encode) preserves the source's
// byte sequence on malformed input — `Utf8Decode` upstream falls
// back to a 1-byte advance carrying the raw lead byte, so a
// single-byte advance here re-emits it bit-identical.  `units` is
// the byte count the decoder consumed, sourced verbatim from `src`.
void AppendQuoteCodepoint(std::string& dst, uint32_t cp, const char* src,
                          size_t units) {
  switch (cp) {
    case '\a':
      dst.append("\\a");
      return;
    case '\b':
      dst.append("\\b");
      return;
    case '\f':
      dst.append("\\f");
      return;
    case '\n':
      dst.append("\\n");
      return;
    case '\r':
      dst.append("\\r");
      return;
    case '\t':
      dst.append("\\t");
      return;
    case '\v':
      dst.append("\\v");
      return;
    case '\\':
      dst.append("\\\\");
      return;
    case '\"':
      dst.append("\\\"");
      return;
    default:
      dst.append(src, units);
      return;
  }
}

}  // namespace

extern "C" void cel_string_quote_at_v(uint32_t out_slot, uint32_t s_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  if (Absorb3vlUnary(out, s)) return;
  if (s->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  const absl::string_view src = BorrowSpan(s->payload.s);
  // Reserve generous: every byte fits in two for the worst-case
  // escape-heavy input, plus the two wrapping quotes.
  std::string buf;
  buf.reserve(src.size() + 2);
  buf.push_back('"');
  const auto* p = reinterpret_cast<const uint8_t*>(src.data());
  const auto* end = p + src.size();
  while (p < end) {
    uint32_t cp = 0;
    const size_t units = Utf8Decode(p, end, &cp);
    AppendQuoteCodepoint(buf, cp, reinterpret_cast<const char*>(p), units);
    p += units;
  }
  buf.push_back('"');
  WriteStringFromBytes(out, buf.data(), buf.size());
}
