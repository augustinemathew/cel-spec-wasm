// Timestamp / duration parse + format kernels for the
// `cel_runtime.wasm` module.  Self-hosted inside the runtime via
// vendored absl + a small set of langdef-strict gates, replacing the
// equivalent host-side trampolines previously in
// `eval/internal/cel_host.cc`.
//
// ABI: each kernel takes the canonical `(out_slot, in_slot)` pair of
// linear-memory offsets, mirroring every other `_at_v` runtime
// helper.  Implementations live in `cel_time_parse.cc` (C++ TU; the
// runtime `cc_binary` links it alongside the C-only TUs).  See
// `doc/implementation-plan/rewrite/phase-c-plan.md` §4 for the
// per-kernel design notes and the conformance envelopes each kernel
// unlocks.

#ifndef CELWASM_RUNTIME_CEL_TIME_PARSE_H_
#define CELWASM_RUNTIME_CEL_TIME_PARSE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parse an RFC3339 timestamp string from the CEL_STRING at `in_slot`
// and write the resulting CEL_TIMESTAMP to `out_slot`.  On any
// failure (wrong input kind, parse error, langdef-strict reject,
// or out-of-range second), `out_slot` is poisoned with a CEL_ERROR
// of kind CEL_ERR_INVALID_ARGUMENT or CEL_ERR_OVERFLOW.
// cel:codegen-export
void cel_timestamp_parse_at_v(uint32_t out_slot, uint32_t in_slot);

// Parse an absl-style duration string (e.g. `"1h30m"`) from the
// CEL_STRING at `in_slot` and write the resulting CEL_DURATION to
// `out_slot`.  Rejects out-of-range (±315B seconds, proto JSON
// envelope) and langdef-strict rule violations (non-decreasing
// unit order, unknown unit suffix).
// cel:codegen-export
void cel_duration_parse_at_v(uint32_t out_slot, uint32_t in_slot);

// Format the CEL_TIMESTAMP at `in_slot` to an RFC3339 string in
// the per-Eval arena and write a CEL_STRING reference to `out_slot`.
// Output ends with `Z` (not `+00:00`); fractional seconds emitted at
// 3 / 6 / 9 digits per the proto JSON convention.
// cel:codegen-export
void cel_timestamp_format_at_v(uint32_t out_slot, uint32_t in_slot);

// Format the CEL_DURATION at `in_slot` to its proto JSON text form
// (`[-]<seconds>[.<frac>]s`).  Result string is arena-allocated.
// cel:codegen-export
void cel_duration_format_at_v(uint32_t out_slot, uint32_t in_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_TIME_PARSE_H_
