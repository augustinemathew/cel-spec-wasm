#include "compiler_v2/api/internal/cel_host_error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/error.h"
#include "compiler_v2/api/internal/cel_host.h"
#include "compiler_v2/api/value.h"
#include "compiler_v2/runtime/cel_data.h"  // CEL_ERR_* wire codes

namespace celwasm {

// ──── cel::Value error factories ─────────────────────────────────

cel::Value FieldNotFound(absl::string_view name) {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/cel::ErrorCode::kFieldNotFound,
      /*message=*/std::string(name),
      /*expr_id=*/0,
  });
}

cel::Value MakeError(cel::ErrorCode code, std::string message) {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/code,
      /*message=*/std::move(message),
      /*expr_id=*/0,
  });
}

cel::Value KeyTypeMismatch() {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/cel::ErrorCode::kTypeMismatch,
      /*message=*/"map key kind is not bool/int/uint/string",
      /*expr_id=*/0,
  });
}

cel::Value NoSuchKey() {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/cel::ErrorCode::kKeyNotFound,
      /*message=*/"no such key",
      /*expr_id=*/0,
  });
}

cel::Value IndexOutOfBounds(std::size_t index, std::size_t count) {
  return cel::Value::Error(cel::ErrorPayload{
      /*code=*/cel::ErrorCode::kIndexOutOfBounds,
      /*message=*/
      absl::StrCat("index ", index, " out of range [0, ", count, ")"),
      /*expr_id=*/0,
  });
}

// ──── Wire-format error encoding ─────────────────────────────────

uint32_t WireErrorCode(cel::ErrorCode c) {
  switch (c) {
    case cel::ErrorCode::kOverflow:
      return CEL_ERR_OVERFLOW;
    case cel::ErrorCode::kDivideByZero:
      return CEL_ERR_DIVIDE_BY_ZERO;
    case cel::ErrorCode::kModulusByZero:
      return CEL_ERR_MODULUS_BY_ZERO;
    case cel::ErrorCode::kTypeMismatch:
      return CEL_ERR_TYPE_MISMATCH;
    case cel::ErrorCode::kTypeUnsupported:
      return CEL_ERR_TYPE_UNSUPPORTED;
    case cel::ErrorCode::kKeyNotFound:
      return CEL_ERR_NO_SUCH_KEY;
    case cel::ErrorCode::kFieldNotFound:
      return CEL_ERR_FIELD_NOT_FOUND;
    case cel::ErrorCode::kIndexOutOfBounds:
      return CEL_ERR_INDEX_OUT_OF_BOUNDS;
    case cel::ErrorCode::kInvalidArgument:
      return CEL_ERR_INVALID_ARGUMENT;
    case cel::ErrorCode::kHostAdapterError:
      return CEL_ERR_HOST_ADAPTER_ERROR;
    default:
      return CEL_ERR_TYPE_MISMATCH;
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
  cv.payload.err = WireErrorCode(cel::ErrorCode::kInvalidArgument);
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
  if (a.kind == CEL_UNKNOWN || a.kind == CEL_ERROR) {
    mem.WriteCelValue(out_slot, a);
    return true;
  }
  if (b.kind == CEL_UNKNOWN || b.kind == CEL_ERROR) {
    mem.WriteCelValue(out_slot, b);
    return true;
  }
  return false;
}

}  // namespace celwasm
