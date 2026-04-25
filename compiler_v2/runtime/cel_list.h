// List runtime primitives — arena construction + arena-fast-path
// indexing + the kDynamic tail-call dispatcher.  Three-path origin
// design from `doc/implementation-plan/rewrite/map-list-dispatch.md`,
// mirroring `cel_map.h`:
//
//   - kArena   — call `cel_list_at_arena` directly (codegen knows).
//   - kHost    — call `cel_host.cel_list_at` directly (codegen
//                knows).  See `api/internal/cel_host.{h,cc}`.
//   - kDynamic — call `cel_list_at`, the dispatcher below; it
//                branches on `kind` and `return_call`s the right arm.
//
// All offsets are u32 byte offsets into the shared linear memory
// (parent design §8.2).

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_LIST_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_LIST_H_

#include <stdint.h>

#include "compiler_v2/runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// Construction.  Allocates a fresh `ArenaListHeader` plus an elements
// array of exactly `count` slots in the bump arena, writes a
// `{CEL_LIST_ARENA, header_ptr}` CelValue into `out_slot`.  All
// element slots are zero-initialized to `{CEL_NULL}` — codegen then
// follows up with `cel_list_set` for each element.  List literals
// are fixed-length: codegen knows the exact element count at build
// time, so there is no growth path and no separate append-and-bump
// counter.  On OOM poisons `out_slot` with
// `{CEL_ERROR, CEL_ERR_OVERFLOW}`.
void cel_list_create(uint32_t out_slot, uint32_t count);

// Writes the CelValue at `elem_slot` into the list at `list_slot`
// at the given (codegen-determined) `index`.  If the list is
// poisoned (already an error), the set is a no-op — error sticks.
// `index >= count` is a codegen invariant violation; the list is
// poisoned with `CEL_ERR_OVERFLOW` rather than scribbling past the
// elements arena.
void cel_list_set(uint32_t list_slot, uint32_t index, uint32_t elem_slot);

// Arena fast path — codegen calls this directly when ResolvePass
// proved the operand origin is `kArena`.  No host trip; pure wasm.
//
// The index slot must be `CEL_INT`; per langdef §"Indexing" the only
// numeric index kind for list[ ] is int (uint is a checker error).
//
// On in-bounds index: writes the element CelValue into `out_slot`.
// On out-of-bounds (negative or `>= count`): writes
//   `{CEL_ERROR, CEL_ERR_INDEX_OUT_OF_BOUNDS}`.
// On non-int index: writes `{CEL_ERROR, CEL_ERR_TYPE_MISMATCH}`.
// On unknown/error operand or index: propagates (3VL).
void cel_list_at_arena(uint32_t out_slot, uint32_t list_slot,
                       uint32_t index_slot);

// kDynamic dispatcher.  Codegen emits a `call` to this when
// ResolvePass cannot prove a single origin (mixed sources, e.g. a
// branch that yields either an arena literal or a proto repeated
// field).  Absorbs unknown / error operands, then `return_call`s
// (`__attribute__((musttail))`) into the kArena or kHost arm — never
// growing the wasm stack.  Same toolchain requirements as
// `cel_map_lookup` (`-mtail-call`,
// `wasmtime::Config::wasm_tail_call(true)`).
void cel_list_at(uint32_t out_slot, uint32_t list_slot, uint32_t index_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_LIST_H_
