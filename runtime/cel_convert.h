// Conversion kernels — M10.B (numeric inter-conversion), M10.C
// (string parsing), M10.D (number / bool → string formatting), and
// M10.E (bytes ↔ string).
//
// Slot-out helper ABI per `design.md §4.2`:
// `(uint32_t out_slot, uint32_t in_slot) -> void`.  Bodies live in
// `cel_convert.c`; this header keeps the topic separate so call sites
// (codegen tests + runtime unit tests) can include only what they use.
//
// Semantics (langdef parity, citing
// `third_party/cel-cpp/internal/overflow.cc` /
// `runtime/standard/type_conversion_functions.cc`):
//
//   - Each helper absorbs `CEL_ERROR` / `CEL_UNKNOWN` on its input;
//     ERROR / UNKNOWN propagates verbatim into `out_slot`.
//   - Wrong-kind input → `out_slot = {CEL_ERROR,
//     err = CEL_ERR_TYPE_MISMATCH}`.
//   - Conversion failures (overflow, NaN, negative source, parse
//     error, invalid UTF-8) → `out_slot = {CEL_ERROR,
//     err = CEL_ERR_OVERFLOW}`.  The dedicated
//     `CEL_ERR_INVALID_ARGUMENT` / `CEL_ERR_INVALID_UTF8` codes can
//     land alongside an api/error.h mirror in a later slice; the
//     conformance contract is "IsError", not error-code-specific.
//
// String outputs are allocated in the per-Eval arena via `arena_alloc`;
// arena OOM on a string-output kernel poisons with
// `CEL_ERR_OVERFLOW`.  Bytes ↔ string aliases the source span — no
// arena allocation, safe for `out_slot == in_slot`.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_CONVERT_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_CONVERT_H_

#include <stdint.h>

#include "runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// M10.B — numeric inter-conversion (6 kernels).  Cross-kind numeric
// arith / compare is the only domain where these surface; the checker
// emits them when literal coercion isn't enough.
void cel_uint_to_int_at_v(uint32_t out, uint32_t in);
void cel_double_to_int_at_v(uint32_t out, uint32_t in);
void cel_int_to_uint_at_v(uint32_t out, uint32_t in);
void cel_double_to_uint_at_v(uint32_t out, uint32_t in);
void cel_int_to_double_at_v(uint32_t out, uint32_t in);
void cel_uint_to_double_at_v(uint32_t out, uint32_t in);

// M10.C — string parsing (4 kernels).  Hand-rolled byte-loop parsers
// mirroring `absl::SimpleAtoi` / `SimpleAtod` admit-sets.
void cel_string_to_int_at_v(uint32_t out, uint32_t in);
void cel_string_to_uint_at_v(uint32_t out, uint32_t in);
void cel_string_to_double_at_v(uint32_t out, uint32_t in);
void cel_string_to_bool_at_v(uint32_t out, uint32_t in);

// M10.D — number / bool → string formatting (4 kernels).  Output
// strings allocated in the per-Eval arena.
void cel_int_to_string_at_v(uint32_t out, uint32_t in);
void cel_uint_to_string_at_v(uint32_t out, uint32_t in);
void cel_bool_to_string_at_v(uint32_t out, uint32_t in);
void cel_double_to_string_at_v(uint32_t out, uint32_t in);

// M10.E — bytes ↔ string (2 kernels).  Both alias the source span
// (no arena copy).  `bytes(string)` is unconditional (CEL strings are
// guaranteed UTF-8 by construction); `string(bytes)` validates per
// RFC3629 and poisons on invalid UTF-8.
void cel_string_to_bytes_at_v(uint32_t out, uint32_t in);
void cel_bytes_to_string_at_v(uint32_t out, uint32_t in);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_CONVERT_H_
