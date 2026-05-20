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

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_EXT_H_
