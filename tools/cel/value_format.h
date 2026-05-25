// Output formatter for the `cel` CLI.
//
// Two distinct shapes for two distinct kinds of output:
//
//   - `FormatScalar(v)` — one-line, type-tagged display for any
//     non-message value.  Used for every `cel eval` result that
//     isn't a proto message.  Round-trips losslessly through the
//     `--var` parser (e.g. an `int` prints as `42`, a `string` as
//     `"hello"`, a `bytes` as `b"\x00\x01"`).
//
//   - `FormatMessages(v, formats)` — multi-format pretty-print
//     for a kMessage value.  `formats` may list any of
//     {`Format::kTextproto`, `Format::kJson`, `Format::kCel`}
//     repeatedly; the result has one labeled section per entry
//     when len > 1, or one bare section (no header) when len == 1.
//     Defaults to `{kTextproto}` if `formats` is empty.
//
// Aggregate non-message kinds (list / map / unknown / error)
// always print as a single tagged line — the multi-format
// machinery is message-specific.

#ifndef CELWASM_COMPILER_V2_TOOLS_CEL_VALUE_FORMAT_H_
#define CELWASM_COMPILER_V2_TOOLS_CEL_VALUE_FORMAT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "eval/value.h"

namespace celwasm::tools::cel {

enum class Format : std::uint8_t {
  kTextproto,
  kJson,
  kCel,
};

absl::string_view FormatName(Format f);

// Parse one `--format=` argument.  InvalidArgument on unknown names.
ABSL_MUST_USE_RESULT absl::StatusOr<Format> ParseFormatName(
    absl::string_view name);

// Format any non-message `celwasm::api::Value`.  See header doc for shape.
ABSL_MUST_USE_RESULT absl::StatusOr<std::string> FormatScalar(
    const ::celwasm::api::Value& v);

// Format a kMessage `celwasm::api::Value` in each requested format.  When
// `formats.size() == 1`, returns the body alone (no "--- X ---"
// header).  When > 1, sections are labeled.
ABSL_MUST_USE_RESULT absl::StatusOr<std::string> FormatMessage(
    const ::celwasm::api::Value& v, const std::vector<Format>& formats);

}  // namespace celwasm::tools::cel

#endif  // CELWASM_COMPILER_V2_TOOLS_CEL_VALUE_FORMAT_H_
