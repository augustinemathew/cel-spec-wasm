// M12 Slice D — `cel_string_format` parser internals.
//
// `ParseFormat(fmt)` walks the format string once and returns a
// `std::vector<DirectiveOp>` where each op is either:
//   - kind == kLiteral: a literal-byte range `[byte_off, byte_off+len)`
//     into the original format-string bytes.
//   - kind == kDirective: a single `%[.<precision>]<type>` directive,
//     with `precision` either the parsed value or `kPrecisionDefault`
//     (which the renderer interprets as cel-cpp's per-type default —
//     6 for `%f` / `%e`, ignored for everything else).
//
// The renderer (Slice E) walks the op list against the args list to
// produce the final string.  Caching the parsed op list (single-slot
// most-recent-format cache, same shape as `cel_matches`) is also
// Slice E work — the parser API stays free-function for now.
//
// This header is NOT exposed via `cel_string_format.h`; only the
// `cel_string_format_test.cc` parser test #includes it directly.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_FORMAT_INTERNAL_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_FORMAT_INTERNAL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/runtime/cel_data.h"

namespace celwasm::string_format_internal {

// Sentinel: directive carries no explicit precision (renderer
// substitutes the per-type default — currently 6 for `%f` / `%e`,
// ignored elsewhere).  Picked as `-1` so the field stays a small
// signed type; cel-cpp uses `std::optional<int>` for the same.
inline constexpr int kPrecisionDefault = -1;

// Per-directive precision cap.  Mirrors cel-cpp's `kMaxPrecision`
// in `extensions/formatting.cc`.
inline constexpr int kMaxPrecision = 1000;

enum class DirectiveKind : uint8_t {
  kLiteral = 0,
  kSubstring = 1,   // %s
  kDecimal = 2,     // %d
  kFixed = 3,       // %f
  kScientific = 4,  // %e
  kBinary = 5,      // %b
  kOctal = 6,       // %o
  kHexLower = 7,    // %x
  kHexUpper = 8,    // %X
};

struct DirectiveOp {
  DirectiveKind kind;
  // For kLiteral: byte offset + length into the original format string.
  // For directives: byte_off / len name the run of original-format bytes
  // the directive consumed, useful for error messages but otherwise
  // unused by the renderer.
  uint32_t byte_off;
  uint32_t len;
  // For kFixed / kScientific: explicit precision, or `kPrecisionDefault`
  // if the user wrote `%f` rather than `%.<n>f`.  Unused (left at
  // `kPrecisionDefault`) for every other directive kind.
  int precision;
};

// Walks `fmt` left-to-right, producing the directive sequence.  On
// any malformed directive, returns `InvalidArgumentError` with a
// short message describing the failure (cel-cpp parity — see
// `Format` + `ParseAndFormatClause` in
// `third_party/cel-cpp/extensions/formatting.cc`).
//
// Specifically rejects:
//   - `%` at end of string                  → "unexpected end of format string"
//   - `%.<digits>` with no type byte        → "unable to find end of precision specifier"
//   - `%.` with no digits and no type byte  → "unable to find end of precision specifier"
//   - unknown type byte                     → "unrecognized formatting clause \"<c>\""
//   - precision > kMaxPrecision             → "precision specifier exceeds maximum of 1000"
//   - non-digit immediately after `%.`      → "unable to find end of precision specifier"
//
// `%%` is parsed as a literal `%` byte (cel-cpp escape).
absl::StatusOr<std::vector<DirectiveOp>> ParseFormat(absl::string_view fmt);

// ───────────────────────────────────────────────────────────────
// Renderer — Slice E.  Each `Render*` arm appends to `buf`.  On
// kind-mismatch (or any other render-time error), the function
// returns `false` and appends nothing — the caller poisons out
// with `CEL_ERR_INVALID_ARGUMENT`.  All renderers are pure with
// respect to the input slot value and the buffer; no arena
// allocation happens until the dispatcher commits the final
// result.
// ───────────────────────────────────────────────────────────────

// `%s` — spec-defined canonical string form.  Per cel-cpp's
// `FormatString` switch: STRING / BYTES (UTF-8 view) / BOOL /
// TIMESTAMP (RFC3339 with Z) / DURATION (Go-style "Ns") / LIST
// (`[a, b]`) / MAP (`{k: v}` with lex-sorted stringified keys) /
// NULL_VALUE ("null") / TYPE / INT / UINT / DOUBLE.  Host-backed
// lists/maps error — same deferred policy as `split`/`join`.
bool RenderString(std::string& buf, const CelValue* v);

// `%d` — INT, UINT.  Bool/string/etc errors.
bool RenderDecimal(std::string& buf, const CelValue* v);

// `%f` — DOUBLE, INT, UINT.  Precision defaults to 6 when
// `precision == kPrecisionDefault`.  NaN / +Inf / -Inf use the
// canonical "NaN" / "Infinity" / "-Infinity" tokens (cel-cpp's
// `FormatDouble` behaviour).
bool RenderFixed(std::string& buf, const CelValue* v, int precision);

// `%e` — DOUBLE, INT, UINT.  Same precision + NaN/Inf rules as
// `RenderFixed`; output is scientific notation.
bool RenderScientific(std::string& buf, const CelValue* v, int precision);

// `%b` — INT, UINT, BOOL.  Signed ints with sign bit; bool emits
// "1" / "0".
bool RenderBinary(std::string& buf, const CelValue* v);

// `%o` — INT, UINT.  Signed ints with sign byte (Go-style).
bool RenderOctal(std::string& buf, const CelValue* v);

// `%x` / `%X` — INT, UINT, STRING, BYTES.  String/bytes render as
// hex of the raw byte sequence; ints as hex of the magnitude with
// leading sign for negatives.
bool RenderHex(std::string& buf, const CelValue* v, bool upper);

}  // namespace celwasm::string_format_internal

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_STRING_FORMAT_INTERNAL_H_
