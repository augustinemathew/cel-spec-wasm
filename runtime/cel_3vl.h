// 3VL / control-flow helpers.
//
// CEL's logical operators (`_&&_`, `_||_`, `!_`) follow non-strict
// 3VL semantics per langdef §"Logical operators":
//   - `false && X = false` for any X (including ERROR / UNKNOWN).
//   - `true  || X = true`  symmetrically.
//   - Otherwise: ERROR > UNKNOWN > OK(bool) dominance — ERROR on
//     either side propagates first; both UNKNOWN merges the
//     attribute-id sets; bool-bool is the obvious truth table.
//
// All four operator helpers use the uniform slot-out ABI
// (`(out_slot, a_slot[, b_slot]) -> void`).  `cel_unknown_merge` is
// the both-UNKNOWN tail, factored out so codegen can call it
// directly when both operands are statically known to be UNKNOWN.
//
// `cel_copy_slot` is a 24-byte memcpy between CelValue slots.  The
// ternary lowering emits a `BinaryenIf` whose branches each leave
// their result in a per-arm slot; `cel_copy_slot` materialises the
// chosen arm into the expression's out slot without re-evaluating
// either branch.  Codegen could instead emit `BinaryenMemoryCopy`
// inline at every ternary site, but a runtime-side helper keeps
// expr_lower lean and the WAT shape regular.
//
// UnknownSet wire shape (mirrors v1, locked at the spike): the
// CelValue's `payload.unk` is a u32 byte-offset to a 2-word
// descriptor `{ ids_off:u32, len:u32 }`; `ids_off` then points at a
// contiguous u32 array of attribute ids in sorted, deduplicated
// order.  `payload.unk == 0` is a legal "empty" UnknownSet — the
// cel_host trampoline mints UNKNOWNs that way for FULL pattern
// matches before per-id provenance is wired through.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_3VL_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_3VL_H_

#include <stdint.h>

#include "runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// `_&&_` — non-strict 3VL conjunction.  Same-slot aliasing is
// well-defined: the body never reads `a`/`b` after writing `out`.
void cel_and(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

// `_||_` — non-strict 3VL disjunction.
void cel_or(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

// `!_` — unary boolean inverse.  ERROR / UNKNOWN propagate
// verbatim; non-bool / non-3VL operand → CEL_ERR_TYPE_MISMATCH.
void cel_not(uint32_t out_slot, uint32_t a_slot);

// Sorted-deduplicated union of two UnknownSets.  Both operands
// must already be CEL_UNKNOWN — kind-mismatch → type error.  The
// merged ids array is freshly allocated in the bump arena; an
// out-of-arena failure surfaces as CEL_ERR_OVERFLOW.  Empty side
// (`payload.unk == 0`) yields the other side verbatim; both empty
// → empty UNKNOWN.
void cel_unknown_merge(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

// Memcpy a 24-byte CelValue from src_slot into dst_slot.  Used by
// the ternary lowering to materialise the chosen branch's result
// into the expression's out slot.  Aliasing (dst == src) is a
// no-op-equivalent self-copy.
void cel_copy_slot(uint32_t dst_slot, uint32_t src_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_3VL_H_
