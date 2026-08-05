// Wire-error helpers and 3VL absorbers — the leaf-level error
// vocabulary shared by every other cel_host_* TU.  Extracted from
// the monolithic `cel_host.cc` in M11 Slice E.
//
// Two distinct concerns live here:
//
//   1. `celwasm::Value` factories for the standard error shapes the
//      host emits inline (FieldNotFound, KeyTypeMismatch, etc.).
//      These return a `celwasm::Value::Error` payload that travels up
//      through the public API surface.
//
//   2. Wire-format encoders that write CEL_BOOL / CEL_INT /
//      CEL_ERROR slots directly into linear memory via `MemoryView`.
//      These are the building blocks every Layer-2 trampoline calls
//      after computing its result.
//
// Plus the 3VL absorbers (`AbsorbUnary`, `AbsorbBinary`) that pass
// through UNKNOWN / ERROR operands without invoking the real op —
// mirrors `cel_runtime.c::absorb_3vl_unary/binary` on the wasm side.

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_ERROR_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_ERROR_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "absl/strings/string_view.h"
#include "eval/error.h"
#include "eval/internal/cel_host_memory.h"
#include "eval/value.h"
#include "runtime/cel_data.h"

namespace celwasm {

// ──── celwasm::Value error factories ─────────────────────────────────

// `Value::Error(kFieldNotFound, name)` — emitted by proto-field
// readers when the requested field number / name doesn't resolve
// against the message's descriptor.
celwasm::Value FieldNotFound(absl::string_view name);

// Generic error factory.  Caller picks the `celwasm::ErrorCode` and
// formats the message.  Every other factory in this header is a
// pinned-message specialisation of this.
celwasm::Value MakeError(celwasm::ErrorCode code, std::string message);

// `Value::Error(kTypeMismatch, "map key kind is not bool/int/uint/string")`.
// Used by host-side map ops when the requested key kind violates the
// langdef map-key constraint.
celwasm::Value KeyTypeMismatch();

// `Value::Error(kKeyNotFound, "no such key")`.  Used by map.Get when
// the key kind is valid but the key isn't present.
celwasm::Value NoSuchKey();

// `Value::Error(kIndexOutOfBounds, "index N out of range [0, M)")`.
celwasm::Value IndexOutOfBounds(std::size_t index, std::size_t count);

// ──── Wire-format error encoding ─────────────────────────────────

// Map a host-side `celwasm::ErrorCode` to the wire-level `CEL_ERR_*`
// constant carried in `CelValue.payload.err`.  Both catalogues
// extend independently; unrecognised host-side codes surface as
// `CEL_ERR_TYPE_MISMATCH` rather than dropping silently.
uint32_t WireErrorCode(celwasm::ErrorCode c);

// Write a CEL_ERROR slot with the supplied wire code to `out_slot`.
void WriteWireError(uint32_t wire_code, uint32_t out_slot, MemoryView& mem);

// Write a CEL_BOOL slot.  Used by every comparison-trampoline Impl
// after the bool result is computed.
void WriteWireBool(bool v, uint32_t out_slot, MemoryView& mem);

// Write a CEL_INT slot.  Used by every numeric-arity trampoline that
// returns a scalar int (size_v, list-size, etc.).
void WriteWireInt(int64_t v, uint32_t out_slot, MemoryView& mem);

// Convenience: write `kInvalidArgument` error to a slot.  Used by
// the TZ-accessor trampoline when the timezone string is malformed.
void WriteInvalidArgumentError(uint32_t out_slot, MemoryView& mem);

// Construct a poisoned `CelValue` (kind=CEL_ERROR, payload=err_code)
// suitable for assignment to a `CelValue*` via `out = PoisonCelValue(...)`.
// Used by absorbers and wrapper-unwrap trampolines that need to
// compose error returns inline without a `MemoryView` roundtrip.
CelValue PoisonCelValue(uint32_t err_code);

// ──── 3VL absorbers ──────────────────────────────────────────────

// Unary: if `a` is UNKNOWN or ERROR, mirror it to `out_slot` and
// return true (caller should short-circuit).  Otherwise return false
// (caller invokes the real op).  Mirrors
// `cel_runtime.c::absorb_3vl_unary` on the host side.
bool AbsorbUnary(const CelValue& a, uint32_t out_slot, MemoryView& mem);

// Binary: same shape over two operands.  Precedence: ERROR beats
// UNKNOWN beats normal, across BOTH operands (left-bias within each
// class) — i.e. UNKNOWN(a) does NOT beat ERROR(b).  This matches the
// runtime kernel's `absorb_3vl_binary` and cel-cpp's
// `NoOverloadResult` (eval/eval/function_step.cc), which propagates
// the first ErrorValue argument before merging unknowns; confirmed
// empirically by the PartialEvalOracle UnknownPlusErrorIsError /
// ErrorPlusUnknownIsError cases in testdata/cel_cpp_oracle_test.cc.
// (Langdef §"Evaluation" leaves multi-error propagation order
// unspecified; the order here is the cel-cpp reference behavior.)
bool AbsorbBinary(const CelValue& a, const CelValue& b, uint32_t out_slot,
                  MemoryView& mem);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_ERROR_H_
