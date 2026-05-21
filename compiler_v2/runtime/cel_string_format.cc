// M12 Slice D — format directive parser.
//
// The parser walks `fmt` once and produces a `DirectiveOp[]`.  Each
// `%` starts a directive; everything else accumulates into a literal
// run.  Literal runs are coalesced — every contiguous non-`%` byte
// span becomes a single `kLiteral` op, NOT one op per byte.  `%%`
// resolves to a single literal `%` byte and joins the surrounding
// literal run.
//
// Diagnostic strings mirror cel-cpp's `extensions/formatting.cc`
// (`ParsePrecision` / `Format` / `ParseAndFormatClause`) so error
// messages are line-for-line equivalent — keeps conformance failure
// output diff-clean against upstream.
//
// Renderer body stays stubbed until Slice E per the unimplemented-
// feature rule in CLAUDE.md.

#include "compiler_v2/runtime/cel_string_format.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_memory.h"
#include "compiler_v2/runtime/cel_string_format_internal.h"

namespace celwasm::string_format_internal {

namespace {

// Append a literal byte at `byte_off` to the current op list,
// coalescing with the previous op if it is itself a literal that
// ends at `byte_off`.  This keeps literal runs as one op even when
// they're produced one byte at a time (e.g. `%%` resolving to a
// `%` byte that joins a surrounding run of plain bytes).
void AppendLiteralByte(std::vector<DirectiveOp>* ops, uint32_t byte_off) {
  if (!ops->empty() && ops->back().kind == DirectiveKind::kLiteral &&
      ops->back().byte_off + ops->back().len == byte_off) {
    ++ops->back().len;
    return;
  }
  ops->push_back(DirectiveOp{
      DirectiveKind::kLiteral, byte_off, 1, kPrecisionDefault});
}

// Parse the precision portion of a directive, starting at `fmt[i]`
// (just past the `%`).  On entry, `fmt[i]` may be `.` (precision
// present) or the type byte (precision absent).  Returns the new
// `i` (pointing at the type byte) and the parsed precision.
//
// Diagnostic strings track cel-cpp `ParsePrecision`:
//   - `%.` with no digits and no type byte → "unable to find end of
//     precision specifier" (cel-cpp catches this in the `i ==
//     format.size()` branch).
//   - non-digit immediately after `%.`     → "unable to find end of
//     precision specifier" (cel-cpp parses `format.substr(1, i-1)`
//     as an int; empty string fails `SimpleAtoi`).
//   - precision > kMaxPrecision            → "precision specifier
//     exceeds maximum of 1000".
absl::StatusOr<std::pair<size_t, int>> ParsePrecision(absl::string_view fmt,
                                                     size_t i) {
  if (i >= fmt.size()) {
    return absl::InvalidArgumentError("unexpected end of format string");
  }
  if (fmt[i] != '.') {
    return std::pair<size_t, int>{i, kPrecisionDefault};
  }
  const size_t digits_begin = i + 1;
  size_t j = digits_begin;
  while (j < fmt.size() && absl::ascii_isdigit(fmt[j])) {
    ++j;
  }
  if (j == digits_begin || j == fmt.size()) {
    return absl::InvalidArgumentError(
        "unable to find end of precision specifier");
  }
  int precision = 0;
  if (!absl::SimpleAtoi(fmt.substr(digits_begin, j - digits_begin),
                        &precision)) {
    return absl::InvalidArgumentError(
        "unable to convert precision specifier to integer");
  }
  if (precision > kMaxPrecision) {
    return absl::InvalidArgumentError(
        absl::StrCat("precision specifier exceeds maximum of ", kMaxPrecision));
  }
  return std::pair<size_t, int>{j, precision};
}

// Map a type byte to its `DirectiveKind`.  Returns
// `kLiteral` (the never-emitted sentinel) on unknown type — the
// caller surfaces the error.
DirectiveKind ClassifyType(char c) {
  switch (c) {
    case 's':
      return DirectiveKind::kSubstring;
    case 'd':
      return DirectiveKind::kDecimal;
    case 'f':
      return DirectiveKind::kFixed;
    case 'e':
      return DirectiveKind::kScientific;
    case 'b':
      return DirectiveKind::kBinary;
    case 'o':
      return DirectiveKind::kOctal;
    case 'x':
      return DirectiveKind::kHexLower;
    case 'X':
      return DirectiveKind::kHexUpper;
    default:
      return DirectiveKind::kLiteral;
  }
}

// Parse a single `%[.<precision>]<type>` directive starting at
// `fmt[start]` (the byte just past the `%`).  Returns the new `i`
// (pointing one past the type byte) and the constructed
// `DirectiveOp`.  `%%` is handled by the caller (appended as a
// literal `%` byte); this routine sees the type byte only when it's
// a real directive.
absl::StatusOr<std::pair<size_t, DirectiveOp>> ParseDirective(
    absl::string_view fmt, size_t start) {
  auto precision_res = ParsePrecision(fmt, start);
  if (!precision_res.ok()) return precision_res.status();
  auto [type_idx, precision] = *precision_res;
  const char type_byte = fmt[type_idx];
  const DirectiveKind kind = ClassifyType(type_byte);
  if (kind == DirectiveKind::kLiteral) {
    return absl::InvalidArgumentError(
        absl::StrCat("unrecognized formatting clause \"", std::string(1, type_byte),
                     "\""));
  }
  // Reject precision on directives where cel-cpp ignores it.
  // Specifically, `%d / %s / %b / %o / %x / %X` don't honour
  // precision; cel-cpp accepts and silently drops it, but we
  // surface the error so users can't write misleading specifiers.
  // TODO: confirm against upstream — if cel-cpp accepts, change
  // this to silently accept.  For now, mirror upstream's actual
  // behaviour: silently drop precision for non-numeric-with-prec
  // directives.  Slice D test asserts the silent-drop policy.
  DirectiveOp op;
  op.kind = kind;
  op.byte_off = static_cast<uint32_t>(start - 1);  // include the leading `%`
  op.len = static_cast<uint32_t>(type_idx + 1 - (start - 1));
  op.precision =
      (kind == DirectiveKind::kFixed || kind == DirectiveKind::kScientific)
          ? precision
          : kPrecisionDefault;
  return std::pair<size_t, DirectiveOp>{type_idx + 1, std::move(op)};
}

}  // namespace

absl::StatusOr<std::vector<DirectiveOp>> ParseFormat(absl::string_view fmt) {
  std::vector<DirectiveOp> ops;
  size_t i = 0;
  while (i < fmt.size()) {
    if (fmt[i] != '%') {
      AppendLiteralByte(&ops, static_cast<uint32_t>(i));
      ++i;
      continue;
    }
    // `%` consumed; need at least one byte after.
    if (i + 1 >= fmt.size()) {
      return absl::InvalidArgumentError("unexpected end of format string");
    }
    if (fmt[i + 1] == '%') {
      AppendLiteralByte(&ops, static_cast<uint32_t>(i + 1));
      i += 2;
      continue;
    }
    auto dir_res = ParseDirective(fmt, i + 1);
    if (!dir_res.ok()) return dir_res.status();
    auto& [next_i, op] = *dir_res;
    ops.push_back(std::move(op));
    i = next_i;
  }
  return ops;
}

}  // namespace celwasm::string_format_internal

// ───────────────────────────────────────────────────────────────
// Public ABI — `cel_string_format_at_vv`.  Slice D: parser only;
// renderer body lands in Slice E.  Per CLAUDE.md, an unimplemented
// code path is `ABSL_CHECK(false) << "... is a stub until <slice>"`,
// not a silent skip.
// ───────────────────────────────────────────────────────────────

extern "C" void cel_string_format_at_vv(uint32_t out_slot, uint32_t s_slot,
                                        uint32_t args_slot) {
  (void)out_slot;
  (void)s_slot;
  (void)args_slot;
  ABSL_CHECK(false) << "cel_string_format_at_vv is a stub until M12 Slice E";
}
