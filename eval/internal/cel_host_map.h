// Layer-2 trampoline entry points for the kHost arms of the map
// runtime dispatchers (`cel_host.cel_map_*`).

#ifndef CELWASM_EVAL_INTERNAL_CEL_HOST_MAP_H_
#define CELWASM_EVAL_INTERNAL_CEL_HOST_MAP_H_

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "eval/internal/cel_host_common.h"

namespace celwasm {

// Layer-2 entry point for the kHost arm of map indexing.
// Reads the map slot's `ref_slot`, dereferences via
// `ExternrefTable::LookupMap` to a `HostMapBacking`, decodes the
// key CelValue, calls `backing->Get(key, ...)`, and marshals the
// returned `celwasm::Value` into `out_slot`.  Absorbs UNKNOWN / ERROR
// on either operand without dereferencing the backing — same 3VL
// contract as the runtime dispatcher.  Non-OK Status only on
// infrastructure failure (bad slot, missing reflection); spec-level
// errors (no_such_key, type-mismatch on key) travel inside the
// returned CelValue.
ABSL_MUST_USE_RESULT absl::Status CelMapLookupImpl(
    uint32_t out_slot, uint32_t map_slot, uint32_t key_slot,
    const TrampolineContext& ctx);

ABSL_MUST_USE_RESULT absl::Status CelMapSizeImpl(uint32_t out_slot,
                                                 uint32_t map_slot,
                                                 const TrampolineContext& ctx);

ABSL_MUST_USE_RESULT absl::Status CelMapInImpl(uint32_t out_slot,
                                               uint32_t key_slot,
                                               uint32_t map_slot,
                                               const TrampolineContext& ctx);

// Map equality accepts ANY origin pair (host+host, host+arena,
// arena+host) — both operands are normalized into host-side
// (key, value) CelValue snapshots before the set-equality walk, so a
// proto map field compares structurally against a map literal.  The
// runtime dispatcher short-circuits arena+arena in its own fast path
// before reaching here.
ABSL_MUST_USE_RESULT absl::Status CelMapEqImpl(uint32_t out_slot,
                                               uint32_t a_slot, uint32_t b_slot,
                                               const TrampolineContext& ctx);

// Comprehension-iter open for a `CEL_MAP_HOST` source.  Walks the
// HostMapBacking via ForEach, encodes each (key, value) pair into
// a flat 48-byte snapshot in the arena (key at +0, value at +24),
// and writes the runtime-side `MapIterState` at `state_offset`:
//
//     [ 0..4]  kind   = 1 (MAP_ITER_KIND_HOST)
//     [ 4..8]  cursor = 0 (pre-first)
//     [ 8..12] payload = snapshot start offset
//     [12..16] count   = snapshot entry count
//
// On empty source or arena OOM, sets `count = 0` so the runtime's
// `cel_map_iter_init` collapses the handle to 0 (empty iter).
//
// Pure scalar-key snapshots are 48B/entry (e.g. `{string→int}` →
// 96B for 2 entries).  Nested map/list/message values are encoded
// as `CEL_MAP_HOST` / `CEL_LIST_HOST` / `CEL_MESSAGE` referencing
// externref slots — same wire shape as inline indexed access.
ABSL_MUST_USE_RESULT absl::Status CelMapIterOpenImpl(
    uint32_t state_offset, uint32_t map_slot, const TrampolineContext& ctx);

}  // namespace celwasm

#endif  // CELWASM_EVAL_INTERNAL_CEL_HOST_MAP_H_
