#include "compiler_v2/api/error.h"

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"

namespace cel {

absl::string_view ErrorCodeName(ErrorCode c) {
  switch (c) {
    case ErrorCode::kOverflow:
      return "overflow";
    case ErrorCode::kDivideByZero:
      return "divide_by_zero";
    case ErrorCode::kModulusByZero:
      return "modulus_by_zero";
    case ErrorCode::kTypeMismatch:
      return "type_mismatch";
    case ErrorCode::kTypeUnsupported:
      return "type_unsupported";
    case ErrorCode::kFieldNotFound:
      return "field_not_found";
    case ErrorCode::kKeyNotFound:
      return "key_not_found";
    case ErrorCode::kDuplicateKey:
      return "duplicate_key";
    case ErrorCode::kIndexOutOfBounds:
      return "index_out_of_bounds";
    case ErrorCode::kUnknownType:
      return "unknown_type";
    case ErrorCode::kCustomFnFailed:
      return "custom_fn_failed";
    case ErrorCode::kHostAdapterError:
      return "host_adapter_error";
    case ErrorCode::kTimeout:
      return "timeout";
  }
  ABSL_CHECK(false) << "unhandled ErrorCode = " << static_cast<int>(c);
}

}  // namespace cel
