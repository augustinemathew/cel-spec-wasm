// ErrorCode + ErrorPayload — payload of `Value::Error`.
//
// Kept in its own translation unit (not folded into `attribute.h`)
// so callers that only need partial-eval / attribute machinery don't
// pull in the error catalogue, and so error-code evolution has a
// single obvious home.

#ifndef CELWASM_EVAL_ERROR_H_
#define CELWASM_EVAL_ERROR_H_

#include <cstdint>
#include <string>

#include "absl/strings/string_view.h"

namespace celwasm {

// Extensible error-code catalogue.  Numeric values are stable on
// the wire and MUST mirror `runtime/cel_data.h::CEL_ERR_*` so a
// `Value::Error` decoded from a CelValue is `static_cast`-equivalent
// to the wire byte.  Add new entries at the end; never renumber.
// In-memory size is u8; the on-wire `CelValue.payload.err` encoding
// promotes to u16 for alignment.
enum class ErrorCode : uint8_t {
  kOverflow = 10,
  kDivideByZero = 11,
  kModulusByZero = 12,
  kTypeMismatch = 13,
  // Returned by `ProtoBacking::ReadField` on MAP / REPEATED fields
  // until M6 lifts the envelope — mirrors `CEL_ERR_TYPE_UNSUPPORTED`
  // in `runtime/cel_data.h` (m2-ident-select-unknowns.md §2.8).
  kTypeUnsupported = 14,
  // Mirrors `CEL_ERR_NO_SUCH_KEY` (cel_data.h).  Returned by
  // `cel_map_lookup_arena` when the key is absent — per langdef
  // §"Indexing", map indexing on a missing key is a no_such_key
  // error, not null.
  kKeyNotFound = 15,
  // Mirrors `CEL_ERR_DUPLICATE_KEY`.  Returned by `cel_map_insert`
  // when a literal contains a duplicate key — per langdef §"Map
  // literals", repeated keys are an error captured at construction.
  kDuplicateKey = 16,
  // Mirrors `CEL_ERR_INDEX_OUT_OF_BOUNDS`.  Returned by
  // `cel_list_at_arena` (and the kDynamic dispatcher's arena arm)
  // when the index is outside `[0, count)`.
  kIndexOutOfBounds = 17,
  // M7B.D: parse failures on `timestamp(str)` / `duration(str)`.
  // Wire-mirrors `CEL_ERR_INVALID_ARGUMENT` (cel_data.h).
  kInvalidArgument = 18,
  kFieldNotFound = 20,
  kUnknownType = 30,
  kCustomFnFailed = 40,
  kHostAdapterError = 41,
  kTimeout = 50,
};

absl::string_view ErrorCodeName(ErrorCode c);

struct ErrorPayload {
  ErrorCode code = ErrorCode::kHostAdapterError;
  std::string message;
  // Index into CheckedExpr.source_info — resolves to a line/col at
  // diagnostic time.  `0` means "not originating from a specific
  // subexpression" (e.g. a host-side failure before eval started).
  uint32_t expr_id = 0;
};

}  // namespace celwasm

#endif  // CELWASM_EVAL_ERROR_H_
