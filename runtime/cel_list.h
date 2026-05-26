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

#ifndef CELWASM_RUNTIME_CEL_LIST_H_
#define CELWASM_RUNTIME_CEL_LIST_H_

#include <stdint.h>

#include "runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allocates a fresh `ArenaListHeader` plus an elements array of
// `capacity` slots in the bump arena; `count = 0`.  Writes a
// `{CEL_LIST_ARENA, header_ptr}` CelValue into `out_slot`.
// Single constructor for both list literals and comprehension
// accumulators:
//   - Literals: codegen knows the element count statically →
//     passes it as capacity, then emits N `cel_list_append_at`
//     in index order.  Final `count == capacity`.
//   - Comprehension accus (map/filter/transformList): codegen
//     loads `iter_range.count` at runtime → passes it as
//     capacity (the source-size bound: map produces exactly N,
//     filter ≤ N, per followon §10.A); per-iter appends.
//     Final `count ≤ capacity`.
// On OOM poisons `out_slot` with `{CEL_ERROR, CEL_ERR_OVERFLOW}`.
// cel:codegen-export
void cel_list_create(uint32_t out_slot, uint32_t capacity);

// Append the value at `value_slot` to the arena list at
// `list_slot`, bumping `hdr->count`.  Universal write primitive
// — used by both literal codegen and comprehension accu codegen
// (see `cel_list_create` above for the contract).
// PRESIZE_INVARIANT: traps via `__builtin_trap()` if
// `count >= capacity` — capacity is sized by codegen, exceeding
// it is a codegen regression we want to surface immediately.
// 3VL: ERROR / UNKNOWN value propagates verbatim into the list
// slot; subsequent appends see the error kind and become silent
// no-ops.
// cel:codegen-export
void cel_list_append_at(uint32_t list_slot, uint32_t value_slot);

// Predicate-gated append for `filter(v, p)` / conditional-map.
// Combines 3VL on the predicate with the append in a single
// helper.  See impl notes in cel_runtime.c.
// cel:codegen-export
void cel_list_append_at_if_bool(uint32_t list_slot, uint32_t pred_slot,
                                uint32_t value_slot);

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
// cel:codegen-export
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
// cel:codegen-export
void cel_list_at(uint32_t out_slot, uint32_t list_slot, uint32_t index_slot);

// =====================================================================
// Aggregate-op kArena fast paths.
//
// Each helper assumes `list_slot.kind == CEL_LIST_ARENA` (codegen
// only routes here when ResolvePass proves arena origin); other
// kinds → CEL_ERR_TYPE_MISMATCH.  3VL absorption matches the
// arith / compare / string envelope.  See
// `rewrite/map-list-dispatch.md` for the three-path dispatch
// contract.
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
// elements run via `arena_alloc`; OOM → CEL_ERR_OVERFLOW.  The
// originals are unchanged.
void cel_list_concat_arena(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

// =====================================================================
// kDynamic dispatchers for aggregate list ops.  Same
// musttail-dispatch shape as `cel_list_at` (line 67): 3VL absorb,
// branch on operand kind, tail-call into the kArena fast path or the
// kHost trampoline.  Codegen emits a `call` to these when ResolvePass
// cannot prove a single origin (`Origin::kDynamic` on the operand).
// `cel_list_eq` and `cel_list_concat` have an extra wrinkle vs
// `cel_list_at`: their two list operands may have different origins.
// Mixed-origin pairs route to the host trampoline (same-origin pairs
// take the corresponding fast path).
// =====================================================================

// cel:codegen-export
void cel_list_size(uint32_t out_slot, uint32_t list_slot);
// cel:codegen-export
void cel_list_in(uint32_t out_slot, uint32_t value_slot, uint32_t list_slot);
// cel:codegen-export
void cel_list_eq(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);
// cel:codegen-export
void cel_list_concat(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_LIST_H_
