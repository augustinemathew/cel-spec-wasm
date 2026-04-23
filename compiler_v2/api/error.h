// ErrorCode + ErrorPayload — payload of `Value::Error`.
//
// Kept in its own translation unit (not folded into `attribute.h`)
// so callers that only need partial-eval / attribute machinery don't
// pull in the error catalogue, and so error-code evolution has a
// single obvious home.

#ifndef CELWASM_COMPILER_V2_API_ERROR_H_
#define CELWASM_COMPILER_V2_API_ERROR_H_

#include <cstdint>
#include <string>

#include "absl/strings/string_view.h"

namespace cel {

// Extensible error-code catalogue.  Numeric values are stable on
// the wire — mirrored in `runtime/cel_data.h::CEL_ERR_*` where
// applicable.  Add new entries at the end; never renumber.
// In-memory size is u8; the on-wire `CelValue.payload.err` encoding
// promotes to u16 for alignment.
enum class ErrorCode : uint8_t {
  kOverflow = 10,
  kDivideByZero = 11,
  kModulusByZero = 12,
  kTypeMismatch = 13,
  kFieldNotFound = 20,
  kKeyNotFound = 21,
  kIndexOutOfBounds = 22,
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

}  // namespace cel

#endif  // CELWASM_COMPILER_V2_API_ERROR_H_
