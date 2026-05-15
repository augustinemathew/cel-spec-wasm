// Type-subsystem runtime helpers (M9.B).  Slot-out helper ABI per
// `m9-type-subsystem.md` §4.5: `(out_slot, in_slot) -> void`.
// Body lives in `cel_runtime.c`; this header keeps the declaration
// separate so call sites (codegen + tests) can include only what
// they need.
//
// `cel_type_of_at_v(out, in)`:
//   Implements the spec `type(x)` standard function.  Reads the
//   operand's `kind` and writes a `CEL_TYPE` CelValue whose
//   `payload.s` carries `(ptr, len)` of the spec type-name string
//   (allocated in the per-Eval arena).
//
//   Absorbing-kind contract: `CEL_ERROR` / `CEL_UNKNOWN` operands
//   propagate verbatim into out_slot.
//
//   The CEL_MESSAGE arm dispatches to the host trampoline
//   `cel_host_resolve_message_type_name` (M9.C), which walks the
//   externref-table backing to resolve the descriptor's FQN.
//   Until M9.C lands, that arm poisons with kTypeMismatch — no
//   silent miscompiles.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_TYPE_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_TYPE_H_

#include <stdint.h>

#include "compiler_v2/runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// `type(x)` — see file header.  `out_slot` and `in_slot` are 24-byte
// CelValue offsets in shared linear memory.
void cel_type_of_at_v(uint32_t out_slot, uint32_t in_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_TYPE_H_
