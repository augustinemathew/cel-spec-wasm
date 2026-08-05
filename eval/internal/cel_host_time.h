// Layer-2 trampoline entry point for the with-TZ timestamp accessor
// dispatch (`cel_host.cel_timestamp_tz_accessor`).

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_TIME_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_TIME_H_

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "eval/internal/cel_host_common.h"

namespace celwasm {

// Timestamp / duration parse + format trampolines (formerly four
// host-side Impls) are now self-hosted in
// `runtime/cel_time_parse.cc`; codegen routes
// `string_to_timestamp` etc. to `cel_runtime.cel_*_at_v` directly.
// See `doc/implementation-plan/rewrite/phase-c-plan.md` §4.

// Single dispatch trampoline for the 10 with-TZ accessor overloads.
// Reads the timestamp + TZ-name string operands; loads the IANA /
// fixed-offset zone via `absl::TimeZone::Load`; projects the
// requested civil-time field per `accessor_kind` (matches
// `CelTzAccessorKind` enum in cel_time.h).  Invalid TZ name →
// CEL_ERROR(kInvalidArgument).  Bad accessor_kind →
// CEL_ERROR(kTypeMismatch) (defence in depth; codegen wouldn't
// emit an unknown kind).
ABSL_MUST_USE_RESULT absl::Status CelTimestampTzAccessorImpl(
    uint32_t out_slot, uint32_t ts_slot, uint32_t tz_slot,
    uint32_t accessor_kind, const TrampolineContext& ctx);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_TIME_H_
