// Layer-2 trampoline entry points for the kHost arms of the list
// runtime dispatchers (`cel_host.cel_list_*`).

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_LIST_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_LIST_H_

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "eval/internal/cel_host_common.h"

namespace celwasm {

// Layer-2 entry point for the kHost arm of list indexing.
// Reads list_slot's `ref_slot`, dereferences via
// `ExternrefTable::LookupList` to a `HostListBacking`, decodes the
// index CelValue (must be CEL_INT; non-int → kTypeMismatch;
// negative or `>= Size()` → kIndexOutOfBounds), calls
// `backing->At(index)`, and marshals the returned `celwasm::Value` into
// `out_slot`.  Absorbs UNKNOWN / ERROR on either operand.  Same
// non-OK Status contract as CelMapLookupImpl.
ABSL_MUST_USE_RESULT absl::Status CelListAtImpl(uint32_t out_slot,
                                                uint32_t list_slot,
                                                uint32_t index_slot,
                                                const TrampolineContext& ctx);

// Comprehension-iter snapshot for a `CEL_LIST_HOST` source.  Walks
// the HostListBacking via `At(i)`, encodes each element into an
// arena-allocated `N×24-byte` elements run, allocates a 16-byte
// ArenaListHeader pointing at that run, and writes a synthetic
// `{kind:CEL_LIST_ARENA, payload.arena_list.header_ptr=...}`
// CelValue at `out_slot`.  Lets the inline arena prologue in
// `expr_lower_comprehension.cc` walk host lists unchanged.
//
// On empty source, OOM, or non-CEL_LIST_HOST input: writes an
// empty arena list (header_ptr=0) so the comprehension loop body
// never runs.  Exception: a CEL_UNKNOWN / CEL_ERROR input returns
// a non-OK Status — the comprehension prologue's range-absorption
// guard propagates poisoned ranges before the iterate path runs
// (expr_lower_comprehension.cc EmitRangeAbsorptionGuard), so
// reaching this impl with one is a codegen regression; the loud
// failure replaces the silent empty-range-identity wrong answer.
// Mirrors `CelMapIterOpenImpl` for maps; see m5b §CCF-8 for the
// design.
ABSL_MUST_USE_RESULT absl::Status CelListIterOpenImpl(
    uint32_t out_slot, uint32_t list_slot, const TrampolineContext& ctx);

// Layer-2 entry points for the kHost arms of the aggregate-op
// runtime dispatchers (`cel_list_size` / `cel_list_in` /
// `cel_list_eq` / `cel_list_concat` / `cel_map_size` / `cel_map_in`
// / `cel_map_eq`).  Each absorbs UNKNOWN / ERROR on its operands,
// dereferences the kHost backing via `ctx.refs.LookupList` /
// `LookupMap`, runs the spec-level operation, and writes the result
// CelValue into `out_slot`.  Spec-level errors (no_such_key,
// kind-mismatched element, …) travel inside the CelValue;
// non-OK Status only on infrastructure failure.  Routing rationale
// in `doc/implementation-plan/rewrite/map-list-dispatch.md`.
//
// Each Impl uses the 3-arg shape (out + 2 operands); Layer 3's
// `UncheckedHostThunk` derives the wasm arity from the impl's C++
// signature.  `CelListSizeImpl` and `CelMapSizeImpl` take only
// (out + 1 operand) — the dispatcher makes a 2-arg call, and the
// linker registers them as 2-arg host functions.
ABSL_MUST_USE_RESULT absl::Status CelListSizeImpl(uint32_t out_slot,
                                                  uint32_t list_slot,
                                                  const TrampolineContext& ctx);

ABSL_MUST_USE_RESULT absl::Status CelListInImpl(uint32_t out_slot,
                                                uint32_t value_slot,
                                                uint32_t list_slot,
                                                const TrampolineContext& ctx);

ABSL_MUST_USE_RESULT absl::Status CelListEqImpl(uint32_t out_slot,
                                                uint32_t a_slot,
                                                uint32_t b_slot,
                                                const TrampolineContext& ctx);

// Cross-origin (one CEL_LIST_ARENA + one CEL_LIST_HOST) and
// both-host concat.  Both operands are LIFTED into one fresh arena
// list allocated through the runtime's `arena_alloc`: arena elements
// copy verbatim, host elements encode through the same marshaller the
// comprehension-iter snapshot uses (scalars inline / arena, aggregate
// elements interned as CEL_MESSAGE / CEL_LIST_HOST / CEL_MAP_HOST
// handles).  The result is a CEL_LIST_ARENA, so downstream readers
// stay on the arena fast path.  Arena OOM poisons CEL_ERR_OVERFLOW,
// matching `cel_list_concat_arena`.  (The runtime dispatcher
// short-circuits arena+arena in its own fast path before reaching
// here.)
ABSL_MUST_USE_RESULT absl::Status CelListConcatImpl(
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot,
    const TrampolineContext& ctx);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_LIST_H_
