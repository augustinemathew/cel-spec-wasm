// Load-time decoder for the `cel.abi` custom section.
//
// Returns the parsed `celwasm::abi::CelAbi` proto directly — the
// wire shape is exactly what the host-side Plan needs to walk.
// Indexing/lookup helpers live where they're used rather than on a
// mirror struct (MarshalActivation iterates `variables()` linearly
// at M2; no hash-by-name is warranted).

#ifndef CELWASM_COMPILER_V2_API_INTERNAL_ABI_DECODE_H_
#define CELWASM_COMPILER_V2_API_INTERNAL_ABI_DECODE_H_

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "compiler_v2/abi/cel_abi.pb.h"
#include "compiler_v2/ir/annotations.h"  // for Repr

namespace celwasm {

// Translate a `CelAbi.VariableEntry.repr` u32 back to the ir::Repr
// enum.  Values outside the enum range land as `Repr::kUnknown` —
// the host marshal will fail loudly on any bound variable whose
// Repr it can't encode rather than silently miscoding.
Repr DecodeRepr(uint32_t wire_value);

// Decode the `cel.abi` custom section from a wasm module's raw
// bytes.  Returns:
//   - OK: the parsed proto.
//   - InvalidArgument: malformed wasm header / section framing /
//     proto payload.
//   - NotFound: no `cel.abi` section present (M1-era modules +
//     synthetic WAT fixtures).  Callers fall back to an empty
//     CelAbi in that case — a variable-free Eval still works.
ABSL_MUST_USE_RESULT absl::StatusOr<celwasm::abi::CelAbi> DecodeCelAbiFromWasm(
    absl::Span<const uint8_t> wasm_bytes);

}  // namespace celwasm

#endif  // CELWASM_COMPILER_V2_API_INTERNAL_ABI_DECODE_H_
