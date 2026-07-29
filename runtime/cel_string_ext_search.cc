// M12 Slice B — search/extract string_ext kernels: indexOf,
// lastIndexOf, substring, replace.  Each function has a 2- and 3-arg
// overload (replace adds a 4-arg form with `n`); total 8 kernels.
//
// Per §4.1 of `m12-string-ext.md`, the cross-cutting helpers live
// in `cel_string_ext_internal.h`; this TU owns the search-specific
// algorithms (`IndexOfImpl`, `LastIndexOfImpl`,
// `CodepointToByteOffset`, `ValidatePos`, `BuildReplaced`,
// `DoReplace`) in its anonymous namespace.
//
// Implementations track cel-cpp's `StringValue::IndexOf` /
// `::LastIndexOf` / `::Substring` / `::Replace`
// (`common/values/string_value.cc`) line-for-line.

#include "runtime/cel_string_ext.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "absl/strings/string_view.h"
#include "runtime/cel_data.h"
#include "runtime/cel_memory.h"
#include "runtime/cel_internal.h"
#include "runtime/cel_string_ext_internal.h"

namespace {

using celwasm::string_ext_internal::Absorb3vlBinary;
using celwasm::string_ext_internal::Absorb3vlUnary;
using celwasm::string_ext_internal::BorrowSpan;
using celwasm::string_ext_internal::Poison;
using celwasm::string_ext_internal::Utf8Decode;
using celwasm::string_ext_internal::WriteInt;
using celwasm::string_ext_internal::WriteStringFromBytes;
using celwasm::string_ext_internal::WriteSubspan;

// Advance the decode walk (`*p`, `*code_points`) forward to the next
// decode boundary at or past `target`, using the exact `Utf8Decode`
// stepping the byte-at-a-time loops used — with one accelerator:
// whole 16-byte blocks of pure ASCII bulk-advance (1 byte == 1 code
// point there, trivially decode-equivalent), so the scalar decode
// only ever runs on spans containing non-ASCII bytes.  Returns true
// iff the walk lands exactly ON `target` (i.e. `target` is a decode
// boundary); on overshoot — `target` sat inside a multi-byte
// sequence — `*p` is the first boundary past it.
bool AdvanceToBoundary(const uint8_t** p, const uint8_t* const end,
                       const uint8_t* const target, int64_t* code_points) {
  const uint8_t* q = *p;
  while (q < target) {
    const auto span = static_cast<uint32_t>(target - q);
    const uint32_t ascii = cel_ascii_prefix_blocks_(q, span);
    if (ascii != 0) {
      q += ascii;
      *code_points += ascii;
      continue;
    }
    uint32_t cp = 0;
    q += Utf8Decode(q, end, &cp);
    ++*code_points;
  }
  *p = q;
  return q == target;
}

// Index-of search.  Emits CODE-POINT indices, so the walk must count
// decode boundaries; `memcmp` on the prefix is the byte-level match.
// `pos < 0` is clamped to 0 (cel-cpp parity — `IndexOf(string, pos)`
// lines ~315-353 clamp negative pos before the search).  Returns the
// code-point index of the first match, or -1 if no match.
//
// The non-empty-needle path anchors on the needle's first byte
// (`cel_anchor_memchr_`: SIMD128 on wasm, SWAR fallback) instead of
// attempting a match at every boundary — a boundary whose byte
// differs from the anchor can never match, so outcomes are identical
// to the plain walk.  Anchors that land inside a multi-byte sequence
// are rejected by `AdvanceToBoundary` (the plain walk never tests
// non-boundary offsets).
int64_t IndexOfImpl(absl::string_view haystack, absl::string_view needle,
                    int64_t pos) {
  pos = std::max<int64_t>(pos, 0);
  const auto* p = reinterpret_cast<const uint8_t*>(haystack.data());
  const uint8_t* const end = p + haystack.size();
  int64_t code_points = 0;
  if (needle.empty()) {
    while (static_cast<size_t>(end - p) >= needle.size()) {
      if (code_points >= pos) return code_points;
      if (static_cast<size_t>(end - p) == needle.size()) break;
      uint32_t cp = 0;
      p += Utf8Decode(p, end, &cp);
      ++code_points;
    }
    return -1;
  }
  const uint8_t anchor = static_cast<uint8_t>(needle[0]);
  while (static_cast<size_t>(end - p) >= needle.size()) {
    const auto window =
        static_cast<uint32_t>(static_cast<size_t>(end - p) - needle.size() + 1);
    const uint8_t* hit = cel_anchor_memchr_(p, anchor, window);
    if (hit == nullptr) return -1;
    if (!AdvanceToBoundary(&p, end, hit, &code_points)) continue;
    if (code_points >= pos &&
        std::memcmp(p, needle.data(), needle.size()) == 0) {
      return code_points;
    }
    if (static_cast<size_t>(end - p) == needle.size()) break;
    uint32_t cp = 0;
    p += Utf8Decode(p, end, &cp);
    ++code_points;
  }
  return -1;
}

// Last-index-of search.  Mirrors cel-cpp's `LastIndexOf` (lines
// ~406-444 unbounded form, ~499-541 with-pos form): walk forward
// recording the most-recent match's code-point index; bail when
// either the haystack is exhausted (`size == needle.size`) or
// `code_points >= pos` (in the bounded form).
//
// `has_pos == false` is the 2-arg form (no `pos` limit).
// `has_pos == true` requires `pos >= 0` (caller errored out
// otherwise).
int64_t LastIndexOfImpl(absl::string_view haystack, absl::string_view needle,
                        int64_t pos, bool has_pos) {
  const auto* p = reinterpret_cast<const uint8_t*>(haystack.data());
  const uint8_t* const end = p + haystack.size();
  int64_t last_index = -1;
  int64_t code_points = 0;
  if (needle.empty()) {
    while (static_cast<size_t>(end - p) >= needle.size()) {
      last_index = code_points;
      const bool past_pos = has_pos && code_points >= pos;
      if (past_pos || static_cast<size_t>(end - p) == needle.size()) break;
      uint32_t cp = 0;
      p += Utf8Decode(p, end, &cp);
      ++code_points;
    }
    return last_index;
  }
  // Anchor-accelerated forward walk (see IndexOfImpl): only decode
  // boundaries whose byte equals the needle's first byte can match,
  // so jumping anchor-to-anchor visits exactly the boundaries the
  // plain walk would have matched at.  The bounded form stops at the
  // first boundary with `code_points >= pos` after testing it —
  // anchors past that limit are discarded before testing.
  const uint8_t anchor = static_cast<uint8_t>(needle[0]);
  while (static_cast<size_t>(end - p) >= needle.size()) {
    const auto window =
        static_cast<uint32_t>(static_cast<size_t>(end - p) - needle.size() + 1);
    const uint8_t* hit = cel_anchor_memchr_(p, anchor, window);
    if (hit == nullptr) break;
    if (!AdvanceToBoundary(&p, end, hit, &code_points)) continue;
    if (has_pos && code_points > pos) break;
    if (std::memcmp(p, needle.data(), needle.size()) == 0) {
      last_index = code_points;
    }
    const bool past_pos = has_pos && code_points >= pos;
    if (past_pos || static_cast<size_t>(end - p) == needle.size()) break;
    uint32_t cp = 0;
    p += Utf8Decode(p, end, &cp);
    ++code_points;
  }
  return last_index;
}

// Code-point index → byte offset.  Walks `s` forward; returns
// `s.size()` when `n == codepoint_count(s)` (the canonical end-of-
// string position), or -1 when `n > codepoint_count(s)`.  Mirrors
// cel-cpp's `SubstringImpl` lines ~600-619.
int64_t CodepointToByteOffset(absl::string_view s, int64_t n) {
  if (n < 0) return -1;
  const auto* base = reinterpret_cast<const uint8_t*>(s.data());
  const uint8_t* p = base;
  const uint8_t* const end = p + s.size();
  int64_t cp_count = 0;
  while (p < end) {
    if (cp_count == n) return static_cast<int64_t>(p - base);
    uint32_t cp = 0;
    const size_t units = Utf8Decode(p, end, &cp);
    p += units;
    ++cp_count;
  }
  if (cp_count == n) return static_cast<int64_t>(s.size());
  return -1;
}

// Pre-flight pos validation common to the 3-arg `indexOf` /
// `lastIndexOf` shapes.  Both reject a negative pos.
//
// The two guards read differently upstream but behave identically:
// `LastIndexOf3` tests `pos < 0 || pos > haystack.Size()`, while
// `IndexOf3` tests only `pos > haystack.Size()` — but `Size()` is
// unsigned, so a negative `pos` promotes to a huge unsigned value and
// trips that guard too (cel-cpp extensions/strings.cc:120, :133).  An
// earlier reading of the source took the missing `pos < 0` in
// `IndexOf3` at face value and clamped instead, which made
// `"abc".indexOf("a", -1)` return 0 where cel-cpp errors.
//
// Returns true on error (caller should bail; `out` has been
// poisoned).
bool ValidatePos(CelValue* out, int64_t pos, size_t haystack_bytes) {
  if (pos < 0 || static_cast<uint64_t>(pos) > haystack_bytes) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return true;
  }
  return false;
}

// Build the replaced string into `out_buf`.  `limit` is the maximum
// number of replacements (already normalised: 0 returns the
// original, negative becomes INT64_MAX upstream).  Mirrors cel-cpp's
// `Replace` body (lines ~1400-1445).
void BuildReplaced(absl::string_view haystack, absl::string_view needle,
                   absl::string_view replacement, int64_t limit,
                   std::string* out_buf) {
  if (needle.empty()) {
    // Empty needle: cel-cpp interleaves `replacement` BEFORE each
    // code point (up to `limit` times), then appends the remaining
    // tail; if any limit budget remains after consuming the whole
    // input, appends one trailing replacement.
    const auto* p = reinterpret_cast<const uint8_t*>(haystack.data());
    const uint8_t* const end = p + haystack.size();
    while (p < end && limit > 0) {
      out_buf->append(replacement.data(), replacement.size());
      uint32_t cp = 0;
      const size_t units = Utf8Decode(p, end, &cp);
      out_buf->append(reinterpret_cast<const char*>(p), units);
      p += units;
      --limit;
    }
    if (p < end) {
      out_buf->append(reinterpret_cast<const char*>(p),
                      static_cast<size_t>(end - p));
    }
    if (limit > 0) {
      out_buf->append(replacement.data(), replacement.size());
    }
    return;
  }
  size_t pos = 0;
  while (pos < haystack.size() && limit > 0) {
    // Linear-scan byte search (mirrors cel-cpp `value_.Find` in the
    // Replace body — Find delegates to absl::string_view::find for
    // string_views).
    const size_t found = haystack.find(needle, pos);
    if (found == absl::string_view::npos) break;
    out_buf->append(haystack.data() + pos, found - pos);
    out_buf->append(replacement.data(), replacement.size());
    pos = found + needle.size();
    --limit;
  }
  if (pos < haystack.size()) {
    out_buf->append(haystack.data() + pos, haystack.size() - pos);
  }
}

void DoReplace(CelValue* out, const CelValue* s, const CelValue* old_v,
               const CelValue* new_v, int64_t limit) {
  if (limit == 0) {
    // cel-cpp: limit==0 → return original (lines ~1394-1397).
    *out = *s;
    return;
  }
  if (limit < 0) limit = std::numeric_limits<int64_t>::max();
  const absl::string_view haystack = BorrowSpan(s->payload.s);
  const absl::string_view needle = BorrowSpan(old_v->payload.s);
  const absl::string_view replacement = BorrowSpan(new_v->payload.s);
  std::string buf;
  // Pre-reserve a reasonable upper bound to dodge the std::string
  // grow loop in the common case (no replacements).  Worst case the
  // empty-needle path triples the size by interleaving replacement.
  buf.reserve(haystack.size() + replacement.size());
  BuildReplaced(haystack, needle, replacement, limit, &buf);
  WriteStringFromBytes(out, buf.data(), buf.size());
}

}  // namespace

// ───────────────────────────────────────────────────────────────
// indexOf
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_index_of_at_vv(uint32_t out_slot, uint32_t s_slot,
                                          uint32_t sub_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* sub = cel_value_at(sub_slot);
  if (Absorb3vlBinary(out, s, sub)) return;
  if (s->kind != CEL_STRING || sub->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  WriteInt(out, IndexOfImpl(BorrowSpan(s->payload.s),
                            BorrowSpan(sub->payload.s), 0));
}

extern "C" void cel_string_index_of_at_vvv(uint32_t out_slot, uint32_t s_slot,
                                           uint32_t sub_slot,
                                           uint32_t pos_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* sub = cel_value_at(sub_slot);
  const CelValue* pos = cel_value_at(pos_slot);
  if (Absorb3vlBinary(out, s, sub)) return;
  if (Absorb3vlUnary(out, pos)) return;
  if (s->kind != CEL_STRING || sub->kind != CEL_STRING ||
      pos->kind != CEL_INT) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // cel-cpp `IndexOf3` bound — see the ValidatePos header comment.
  if (ValidatePos(out, pos->payload.i, s->payload.s.len)) {
    return;
  }
  WriteInt(out, IndexOfImpl(BorrowSpan(s->payload.s),
                            BorrowSpan(sub->payload.s), pos->payload.i));
}

// ───────────────────────────────────────────────────────────────
// lastIndexOf
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_last_index_of_at_vv(uint32_t out_slot,
                                               uint32_t s_slot,
                                               uint32_t sub_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* sub = cel_value_at(sub_slot);
  if (Absorb3vlBinary(out, s, sub)) return;
  if (s->kind != CEL_STRING || sub->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  WriteInt(out, LastIndexOfImpl(BorrowSpan(s->payload.s),
                                BorrowSpan(sub->payload.s), 0,
                                /*has_pos=*/false));
}

extern "C" void cel_string_last_index_of_at_vvv(uint32_t out_slot,
                                                uint32_t s_slot,
                                                uint32_t sub_slot,
                                                uint32_t pos_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* sub = cel_value_at(sub_slot);
  const CelValue* pos = cel_value_at(pos_slot);
  if (Absorb3vlBinary(out, s, sub)) return;
  if (Absorb3vlUnary(out, pos)) return;
  if (s->kind != CEL_STRING || sub->kind != CEL_STRING ||
      pos->kind != CEL_INT) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // cel-cpp `LastIndexOf3` byte-size bound + negative-pos reject.
  if (ValidatePos(out, pos->payload.i, s->payload.s.len)) {
    return;
  }
  WriteInt(out, LastIndexOfImpl(BorrowSpan(s->payload.s),
                                BorrowSpan(sub->payload.s), pos->payload.i,
                                /*has_pos=*/true));
}

// ───────────────────────────────────────────────────────────────
// substring
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_substring_at_vv(uint32_t out_slot, uint32_t s_slot,
                                           uint32_t start_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* start = cel_value_at(start_slot);
  if (Absorb3vlBinary(out, s, start)) return;
  if (s->kind != CEL_STRING || start->kind != CEL_INT) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Mirrors cel-cpp `Substring(start)` (lines ~647-700): start<0
  // and start>byte_size are early-out errors; the code-point walk
  // refines start>codepoint_count to an error.
  if (ValidatePos(out, start->payload.i, s->payload.s.len)) {
    return;
  }
  const absl::string_view text = BorrowSpan(s->payload.s);
  const int64_t byte_off = CodepointToByteOffset(text, start->payload.i);
  if (byte_off < 0) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  WriteSubspan(out, s->payload.s.ptr, static_cast<uint32_t>(byte_off),
               static_cast<uint32_t>(s->payload.s.len -
                                     static_cast<uint32_t>(byte_off)));
}

extern "C" void cel_string_substring_range_at_vvv(uint32_t out_slot,
                                                  uint32_t s_slot,
                                                  uint32_t start_slot,
                                                  uint32_t end_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* start = cel_value_at(start_slot);
  const CelValue* end = cel_value_at(end_slot);
  if (Absorb3vlBinary(out, s, start)) return;
  if (Absorb3vlUnary(out, end)) return;
  if (s->kind != CEL_STRING || start->kind != CEL_INT || end->kind != CEL_INT) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // Mirrors cel-cpp `Substring(start, end)` (lines ~764-820):
  // start<0, end<start, start/end>byte_size are early-out errors;
  // the code-point walk catches start/end>codepoint_count.
  if (start->payload.i < 0 || end->payload.i < start->payload.i) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  if (static_cast<uint64_t>(start->payload.i) > s->payload.s.len ||
      static_cast<uint64_t>(end->payload.i) > s->payload.s.len) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  const absl::string_view text = BorrowSpan(s->payload.s);
  const int64_t start_byte = CodepointToByteOffset(text, start->payload.i);
  const int64_t end_byte = CodepointToByteOffset(text, end->payload.i);
  if (start_byte < 0 || end_byte < 0) {
    Poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  WriteSubspan(out, s->payload.s.ptr, static_cast<uint32_t>(start_byte),
               static_cast<uint32_t>(end_byte - start_byte));
}

// ───────────────────────────────────────────────────────────────
// replace
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_replace_at_vvv(uint32_t out_slot, uint32_t s_slot,
                                          uint32_t old_slot,
                                          uint32_t new_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* old_v = cel_value_at(old_slot);
  const CelValue* new_v = cel_value_at(new_slot);
  if (Absorb3vlBinary(out, s, old_v)) return;
  if (Absorb3vlUnary(out, new_v)) return;
  if (s->kind != CEL_STRING || old_v->kind != CEL_STRING ||
      new_v->kind != CEL_STRING) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  // 3-arg `replace` has no limit → replace all (cel-cpp passes -1).
  DoReplace(out, s, old_v, new_v, -1);
}

extern "C" void cel_string_replace_n_at_vvvv(uint32_t out_slot, uint32_t s_slot,
                                             uint32_t old_slot,
                                             uint32_t new_slot,
                                             uint32_t n_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* old_v = cel_value_at(old_slot);
  const CelValue* new_v = cel_value_at(new_slot);
  const CelValue* n = cel_value_at(n_slot);
  if (Absorb3vlBinary(out, s, old_v)) return;
  if (Absorb3vlBinary(out, new_v, n)) return;
  if (s->kind != CEL_STRING || old_v->kind != CEL_STRING ||
      new_v->kind != CEL_STRING || n->kind != CEL_INT) {
    Poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  DoReplace(out, s, old_v, new_v, n->payload.i);
}
