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

// =====================================================================
// M5.D step 1 — aggregate-op kArena fast paths.
//
// Each helper assumes `list_slot.kind == CEL_LIST_ARENA` (codegen
// only routes here when ResolvePass proves arena origin); other
// kinds → CEL_ERR_TYPE_MISMATCH.  3VL absorption matches the
// arith / compare / string envelope.  kHost / kDynamic siblings
// land in M5.D step 2.
//
// Element-equality (used by `in` and `eq`) reuses the same kind-
// aware matcher as `cel_map_lookup_arena`'s `map_keys_equal`, so
// numeric kinds compare by mathematical value (cross-type per
// langdef §"Equality") and string/bytes by byte equality.
// =====================================================================

// Writes `{CEL_INT, i = ArenaListHeader.count}` into `out_slot`.
void cel_list_size_arena(uint32_t out_slot, uint32_t list_slot);

// Writes `{CEL_BOOL, b = (value ∈ list ? 1 : 0)}` into `out_slot`.
// 3VL absorption applies to BOTH operands.
void cel_list_in_arena(uint32_t out_slot, uint32_t value_slot,
                       uint32_t list_slot);

// Writes `{CEL_BOOL, b = (a == b ? 1 : 0)}` into `out_slot`.  Per
// langdef §"Equality": list-equal iff same length and each element
// pairwise-equal.  Element comparison uses the kind-aware matcher.
void cel_list_eq_arena(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

// Writes a fresh CEL_LIST_ARENA into `out_slot` whose elements are
// `a` followed by `b`.  Allocates a new ArenaListHeader and a new
// elements run via `cel_alloc`; OOM → CEL_ERR_OVERFLOW.  The
// originals are unchanged.
void cel_list_concat_arena(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

// =====================================================================
// M5.D step 2 — kDynamic dispatchers for aggregate list ops.  Same
// musttail-dispatch shape as `cel_list_at` (line 67): 3VL absorb,
// branch on operand kind, tail-call into the kArena fast path or the
// kHost trampoline.  Codegen emits a `call` to these when ResolvePass
// cannot prove a single origin (`Origin::kDynamic` on the operand).
// `cel_list_eq` and `cel_list_concat` have an extra wrinkle vs
// `cel_list_at`: their two list operands may have different origins.
// Mixed-origin pairs route to the host trampoline (same-origin pairs
// take the corresponding fast path).
// =====================================================================

void cel_list_size(uint32_t out_slot, uint32_t list_slot);
void cel_list_in(uint32_t out_slot, uint32_t value_slot, uint32_t list_slot);
void cel_list_eq(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
void cel_list_concat(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_LIST_H_
