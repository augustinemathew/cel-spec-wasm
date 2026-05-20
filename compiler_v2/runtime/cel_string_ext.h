// M12 — `string_ext` extension runtime kernels (self-hosted in
// `cel_runtime.wasm`).  Public ABI for the 13 cel-cpp `strings`
// extension functions other than `format`; `format` lives in
// `cel_string_format.{h,cc}` because its directive parser dominates
// the TU.
//
// All kernels follow the canonical slot-out ABI used elsewhere in
// the runtime: `(out_slot, arg0_slot, ..., argN-1_slot) -> void`,
// where every slot is a u32 byte offset into the shared linear
// memory.  3VL absorb (ERROR / UNKNOWN) and kind-mismatch envelopes
// match `cel_string_ops.h`.
//
// **String allocation.**  Kernels that produce a new string payload
// (`charAt`, `reverse`, and the case-fold helpers when the input
// changes) arena_alloc the output bytes; the resulting CelValue's
// `payload.s.ptr` points into the per-Eval arena and is valid until
// the next `arena_reset`.  Kernels that can narrow without
// allocating (`trim`, `lowerAscii`/`upperAscii` on already-folded
// input) reuse the source span — the kind tag in the CelValue
// disambiguates ownership for any future GC step.
//
// **UTF-8 semantics.**  `charAt`, `reverse`, and `trim` operate on
// Unicode code points (per cel-cpp `extensions/strings.cc` +
// `common/values/string_value.cc`); `lowerAscii`/`upperAscii` fold
// only ASCII A-Z / a-z and pass through non-ASCII bytes verbatim
// (langdef §"String / bytes", cel-cpp's ASCII-only contract).
//
// See `doc/implementation-plan/rewrite/m12-string-ext.md` for the
// per-kernel test matrix and slice plan.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_EXT_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_EXT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// `s.charAt(i)`.  `i` is a code-point index.  Spec contract
// (cel-cpp `StringValue::CharAt`):
//   - `i < 0`                     -> CEL_ERROR/CEL_ERR_INVALID_ARGUMENT
//   - `i == codepoint_count(s)`   -> empty string (canonical
//                                    "end of string" sentinel)
//   - `i > codepoint_count(s)`    -> CEL_ERROR/CEL_ERR_INVALID_ARGUMENT
//   - otherwise                   -> a freshly arena-allocated
//                                    1-codepoint string carrying the
//                                    UTF-8 bytes of `s[i]`.
// Kind mismatch (non-string `s`, non-int `i`) poisons with
// CEL_ERR_TYPE_MISMATCH.  3VL absorbs ERROR / UNKNOWN from either
// operand.
void cel_string_char_at_at_vv(uint32_t out_slot, uint32_t s_slot,
                              uint32_t i_slot);

// `s.lowerAscii()` / `s.upperAscii()`.  ASCII-only case fold; bytes
// >= 0x80 pass through unchanged (the input may be non-UTF-8 or
// contain code points outside the ASCII range).  When no fold is
// needed the output reuses the source span without allocating.
void cel_string_lower_ascii_at_v(uint32_t out_slot, uint32_t s_slot);
void cel_string_upper_ascii_at_v(uint32_t out_slot, uint32_t s_slot);

// `s.trim()`.  Strips Unicode whitespace from both ends.  The
// whitespace set mirrors cel-cpp's `IsUnicodeWhitespace`
// (`common/values/string_value.cc::IsUnicodeWhitespace`): ASCII
// 0x09-0x0D + 0x20, plus the named Unicode separators (U+0085,
// U+00A0, U+1680, U+2000-U+200A, U+2028, U+2029, U+202F, U+205F,
// U+3000).  Returns a narrowed subspan into the original bytes
// without allocating.
void cel_string_trim_at_v(uint32_t out_slot, uint32_t s_slot);

// `s.reverse()`.  Code-point reversal (NOT byte reversal — a 3-byte
// `©` stays a single 3-byte unit, just relocated).  Output bytes
// are arena-allocated.  Empty input produces an empty (zero-len,
// zero-ptr) string slot.
void cel_string_reverse_at_v(uint32_t out_slot, uint32_t s_slot);

// ───────────────────────────────────────────────────────────────
// Search / extract family — Slice B.  Mirrors cel-cpp's
// `StringValue::IndexOf` / `::LastIndexOf` / `::Substring` /
// `::Replace`.  The search itself is byte-level (`memcmp`); the
// returned index, the `pos` argument, and substring's `start/end`
// are all CODE-POINT indices.  The 3-arg `indexOf`/`lastIndexOf`
// pos-bound check uses the haystack's BYTE size (cel-cpp parity —
// extensions/strings.cc `IndexOf3` / `LastIndexOf3`).
// ───────────────────────────────────────────────────────────────

// `s.indexOf(sub)`: code-point index of the first match, -1 if no
// match.  Empty needle matches at index 0.
void cel_string_index_of_at_vv(uint32_t out_slot, uint32_t s_slot,
                               uint32_t sub_slot);

// `s.indexOf(sub, pos)`: same, restricted to matches at code-point
// index >= pos.  Per cel-cpp `IndexOf3`: pos > byte_size(s) →
// CEL_ERROR(INVALID_ARGUMENT).  Negative pos is clamped to 0.
void cel_string_index_of_at_vvv(uint32_t out_slot, uint32_t s_slot,
                                uint32_t sub_slot, uint32_t pos_slot);

// `s.lastIndexOf(sub)`: code-point index of the last match, -1 if
// no match.  Empty needle matches at the last code-point boundary.
void cel_string_last_index_of_at_vv(uint32_t out_slot, uint32_t s_slot,
                                    uint32_t sub_slot);

// `s.lastIndexOf(sub, pos)`: same, restricted to matches at code-
// point index <= pos.  Per cel-cpp `LastIndexOf3`: pos < 0 or
// pos > byte_size(s) → CEL_ERROR(INVALID_ARGUMENT).
void cel_string_last_index_of_at_vvv(uint32_t out_slot, uint32_t s_slot,
                                     uint32_t sub_slot, uint32_t pos_slot);

// `s.substring(start)` / `s.substring(start, end)`.  Per cel-cpp
// `StringValue::Substring`: start/end are code-point indices.  Out-
// of-range (start<0, end<start, start/end > byte_size(s), or
// start/end exceeds the code-point count) → CEL_ERROR(INVALID_ARGUMENT).
// Output reuses the source span (no alloc) — substring of an arena
// string is still arena-backed.
void cel_string_substring_at_vv(uint32_t out_slot, uint32_t s_slot,
                                uint32_t start_slot);
void cel_string_substring_range_at_vvv(uint32_t out_slot, uint32_t s_slot,
                                       uint32_t start_slot, uint32_t end_slot);

// `s.replace(old, new)` / `s.replace(old, new, n)`.  Byte-level
// search; with `n` omitted or negative, replace all occurrences;
// `n == 0` returns the original.  Empty `old` interleaves `new`
// between every code-point boundary (cel-cpp parity — see
// `StringValue::Replace` lines ~1405-1430).  Output is arena-
// allocated.
void cel_string_replace_at_vvv(uint32_t out_slot, uint32_t s_slot,
                               uint32_t old_slot, uint32_t new_slot);
void cel_string_replace_n_at_vvvv(uint32_t out_slot, uint32_t s_slot,
                                  uint32_t old_slot, uint32_t new_slot,
                                  uint32_t n_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_EXT_H_
