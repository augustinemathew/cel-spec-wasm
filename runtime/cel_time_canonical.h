// Canonical string forms for timestamp / duration values — the ONE
// implementation both emitters share:
//
//   - `cel_timestamp_format_at_v` / `cel_duration_format_at_v`
//     (`cel_time_parse.cc`, the `string(<timestamp|duration>)`
//     conversion kernels), and
//   - the `%s` renderers in `cel_string_format_render.cc`
//     (`<string>.format([...])`).
//
// Conformance scores these byte-exactly against cel-cpp, so the two
// call sites must never drift: a one-character difference fails
// corpus rows.  Notable canon quirks both sites inherit from here:
//
//   - Timestamp: RFC3339 with trailing `Z` (never `+00:00`); the
//     fractional second is `%E*S` minimal-digits (`.5`, not `.500`);
//     the year is unpadded (year 1 renders `1-01-01T00:00:00Z`).
//   - Duration: proto JSON text form `[-]<seconds>[.<frac>]s` with
//     the fraction at 3 / 6 / 9 digits (trailing zero TRIPLES
//     trimmed, but zeros within a triple kept: `0.500s`).

#ifndef CELWASM_RUNTIME_CEL_TIME_CANONICAL_H_
#define CELWASM_RUNTIME_CEL_TIME_CANONICAL_H_

#include <cstdint>
#include <string>

namespace celwasm {

// Canonical RFC3339 form of the timestamp instant
// `epoch + seconds + nanos` (the CelDurTs payload pair; a
// sign-correlated negative-nanos pre-epoch pair is welcome — the
// instant is formed by exact addition before formatting).
std::string FormatTimestampRfc3339(int64_t seconds, int32_t nanos);

// Canonical proto-Duration JSON text form of the sign-correlated
// (seconds, nanos) pair: `[-]<abs-seconds>[.<frac>]s`.
std::string FormatProtoDuration(int64_t seconds, int32_t nanos);

}  // namespace celwasm

#endif  // CELWASM_RUNTIME_CEL_TIME_CANONICAL_H_
