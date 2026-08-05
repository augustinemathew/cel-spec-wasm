#include "eval/internal/cel_host_error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "eval/error.h"
#include "eval/internal/cel_host_memory.h"
#include "eval/value.h"
#include "runtime/cel_data.h"  // CEL_ERR_* wire codes

namespace celwasm {

// ──── celwasm::Value error factories ─────────────────────────────────

celwasm::Value FieldNotFound(absl::string_view name) {
  return celwasm::Value::Error(celwasm::ErrorPayload{
      /*code=*/celwasm::ErrorCode::kFieldNotFound,
      /*message=*/std::string(name),
      /*expr_id=*/0,
  });
}

celwasm::Value MakeError(celwasm::ErrorCode code, std::string message) {
  return celwasm::Value::Error(celwasm::ErrorPayload{
      /*code=*/code,
      /*message=*/std::move(message),
      /*expr_id=*/0,
  });
}

celwasm::Value KeyTypeMismatch() {
  return celwasm::Value::Error(celwasm::ErrorPayload{
      /*code=*/celwasm::ErrorCode::kTypeMismatch,
      /*message=*/"map key kind is not bool/int/uint/string",
      /*expr_id=*/0,
  });
}

celwasm::Value NoSuchKey() {
  return celwasm::Value::Error(celwasm::ErrorPayload{
      /*code=*/celwasm::ErrorCode::kKeyNotFound,
      /*message=*/"no such key",
      /*expr_id=*/0,
  });
}

celwasm::Value IndexOutOfBounds(std::size_t index, std::size_t count) {
  return celwasm::Value::Error(celwasm::ErrorPayload{
      /*code=*/celwasm::ErrorCode::kIndexOutOfBounds,
      /*message=*/
      absl::StrCat("index ", index, " out of range [0, ", count, ")"),
      /*expr_id=*/0,
  });
}

// ──── Wire-format error encoding ─────────────────────────────────

uint32_t WireErrorCode(celwasm::ErrorCode c) {
  switch (c) {
    case celwasm::ErrorCode::kOverflow:
      return CEL_ERR_OVERFLOW;
    case celwasm::ErrorCode::kDivideByZero:
      return CEL_ERR_DIVIDE_BY_ZERO;
    case celwasm::ErrorCode::kModulusByZero:
      return CEL_ERR_MODULUS_BY_ZERO;
    case celwasm::ErrorCode::kTypeMismatch:
      return CEL_ERR_TYPE_MISMATCH;
    case celwasm::ErrorCode::kTypeUnsupported:
      return CEL_ERR_TYPE_UNSUPPORTED;
    case celwasm::ErrorCode::kKeyNotFound:
      return CEL_ERR_NO_SUCH_KEY;
    case celwasm::ErrorCode::kDuplicateKey:
      return CEL_ERR_DUPLICATE_KEY;
    case celwasm::ErrorCode::kFieldNotFound:
      return CEL_ERR_FIELD_NOT_FOUND;
    case celwasm::ErrorCode::kIndexOutOfBounds:
      return CEL_ERR_INDEX_OUT_OF_BOUNDS;
    case celwasm::ErrorCode::kInvalidArgument:
      return CEL_ERR_INVALID_ARGUMENT;
    case celwasm::ErrorCode::kUnknownType:
      return CEL_ERR_UNKNOWN_TYPE;
    case celwasm::ErrorCode::kCustomFnFailed:
      return CEL_ERR_CUSTOM_FN_FAILED;
    case celwasm::ErrorCode::kHostAdapterError:
      return CEL_ERR_HOST_ADAPTER_ERROR;
    case celwasm::ErrorCode::kTimeout:
      return CEL_ERR_TIMEOUT;
    default:
      // Open switch: `ErrorCode` has a fixed underlying type, so an
      // embedder-supplied payload can legally carry a value outside
      // the named set.  Pass the numeric through unchanged — the
      // decoders degrade an unrecognized wire byte to
      // kHostAdapterError ("runtime error code N") rather than
      // silently relabeling it as a type mismatch.
      return static_cast<uint32_t>(c);
  }
}

void WriteWireError(uint32_t wire_code, uint32_t out_slot, MemoryView& mem) {
  CelValue err{};
  err.kind = CEL_ERROR;
  err.payload.err = wire_code;
  mem.WriteCelValue(out_slot, err);
}

void WriteWireBool(bool v, uint32_t out_slot, MemoryView& mem) {
  CelValue cv{};
  cv.kind = CEL_BOOL;
  cv.payload.b = v ? 1 : 0;
  mem.WriteCelValue(out_slot, cv);
}

void WriteWireInt(int64_t v, uint32_t out_slot, MemoryView& mem) {
  CelValue cv{};
  cv.kind = CEL_INT;
  cv.payload.i = v;
  mem.WriteCelValue(out_slot, cv);
}

void WriteInvalidArgumentError(uint32_t out_slot, MemoryView& mem) {
  CelValue cv{};
  cv.kind = CEL_ERROR;
  cv.payload.err = WireErrorCode(celwasm::ErrorCode::kInvalidArgument);
  mem.WriteCelValue(out_slot, cv);
}

CelValue PoisonCelValue(uint32_t err_code) {
  CelValue v{};
  v.kind = CEL_ERROR;
  v.payload.err = err_code;
  return v;
}

// ──── 3VL absorbers ──────────────────────────────────────────────

bool AbsorbUnary(const CelValue& a, uint32_t out_slot, MemoryView& mem) {
  if (a.kind == CEL_UNKNOWN || a.kind == CEL_ERROR) {
    mem.WriteCelValue(out_slot, a);
    return true;
  }
  return false;
}

bool AbsorbBinary(const CelValue& a, const CelValue& b, uint32_t out_slot,
                  MemoryView& mem) {
  // ERROR dominates UNKNOWN across operands (left-bias within each
  // class), matching the kernel's absorb_3vl_binary and cel-cpp's
  // NoOverloadResult (eval/eval/function_step.cc), which propagates
  // the first ErrorValue arg before merging unknowns.  Pinned by
  // the PartialEvalOracle UnknownPlusErrorIsError /
  // ErrorPlusUnknownIsError cases in testdata/cel_cpp_oracle_test.cc.
  if (a.kind == CEL_ERROR) {
    mem.WriteCelValue(out_slot, a);
    return true;
  }
  if (b.kind == CEL_ERROR) {
    mem.WriteCelValue(out_slot, b);
    return true;
  }
  if (a.kind == CEL_UNKNOWN) {
    mem.WriteCelValue(out_slot, a);
    return true;
  }
  if (b.kind == CEL_UNKNOWN) {
    mem.WriteCelValue(out_slot, b);
    return true;
  }
  return false;
}

}  // namespace celwasm
