// Map runtime primitives — arena construction + arena-fast-path lookup
// + the kDynamic tail-call dispatcher.  Three-path origin design from
// `doc/implementation-plan/rewrite/map-list-dispatch.md`:
//
//   - kArena   — call `cel_map_lookup_arena` directly (codegen knows).
//   - kHost    — call `cel_host.cel_map_lookup` directly (codegen
//                knows).  See `api/internal/cel_host.{h,cc}`.
//   - kDynamic — call `cel_map_lookup`, the dispatcher below; it
//                branches on `kind` and `return_call`s the right arm.
//
// All offsets are u32 byte offsets into the shared linear memory
// (parent design §8.2).

#ifndef CELWASM_RUNTIME_CEL_MAP_H_
#define CELWASM_RUNTIME_CEL_MAP_H_

#include <stdint.h>

#include "runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// Construction.  Allocates a fresh `ArenaMapHeader` plus an entries
// array of exactly `capacity` slots in the bump arena, writes a
// `{CEL_MAP_ARENA, header_ptr}` CelValue into `out_slot`.  Map
// literals are fixed-length: the codegen knows the exact entry
// count at build time and passes it as `capacity`, so there is no
// growth path — a subsequent `cel_map_insert` past `capacity` is a
// codegen invariant violation and poisons the map.  On OOM poisons
// `out_slot` with `{CEL_ERROR, CEL_ERR_OVERFLOW}`.
// cel:codegen-export
void cel_map_create(uint32_t out_slot, uint32_t capacity);

// Linear-scan insert with duplicate-key poisoning.  If `key_slot`'s
// kind is unsupported (anything other than bool/int/uint/string) the
// map slot is poisoned with `CEL_ERR_TYPE_MISMATCH`; if the key is
// already present the map slot is poisoned with
// `CEL_ERR_DUPLICATE_KEY`, per langdef §"Map literals".  Inserting
// past the constructed `capacity` is a codegen-side invariant
// violation and poisons with `CEL_ERR_OVERFLOW` — the runtime never
// resizes; the literal's entry count is fixed at compile time.
// cel:codegen-export
void cel_map_insert(uint32_t map_slot, uint32_t key_slot, uint32_t value_slot);

// Dynamic-map insert for `transformMap` / `transformMapEntry`
// comprehension accumulators.  Unlike `cel_map_insert`:
// geometric 2× growth on full; collisions overwrite (last-write-
// wins); CEL_ERROR / CEL_UNKNOWN in key OR value propagates
// verbatim into the map slot.
// cel:codegen-export
void cel_map_insert_at(uint32_t map_slot, uint32_t key_slot,
                       uint32_t value_slot);

// 3VL predicate-gated map insert for conditional transformMap /
// transformMapEntry steps.  Mirror of
// cel_list_append_at_if_bool: pred ERROR/UNKNOWN → propagate into
// map slot (aborts comprehension); pred non-bool → poison TYPE_MISMATCH;
// pred false → no-op; pred true → cel_map_insert_at delegate.
// cel:codegen-export
void cel_map_insert_at_if_bool(uint32_t map_slot, uint32_t pred_slot,
                               uint32_t key_slot, uint32_t value_slot);

// Arena fast path — codegen calls this directly when ResolvePass
// proved the operand origin is `kArena`.  No host trip; pure wasm
// linear scan.  Cross-type numeric key equality follows langdef
// §"Equality": int/uint/double compare by mathematical value across
// the type ladder; bool and string compare by structural identity.
//
// On hit: writes the value CelValue into `out_slot`.
// On miss: writes `{CEL_ERROR, CEL_ERR_NO_SUCH_KEY}` into `out_slot`.
// On unknown/error key: propagates the operand into `out_slot` (3VL).
// cel:codegen-export
void cel_map_lookup_arena(uint32_t out_slot, uint32_t map_slot,
                          uint32_t key_slot);

// kDynamic dispatcher.  Codegen emits a `call` to this when
// ResolvePass cannot prove a single origin (mixed sources, e.g. a
// branch that yields either an arena literal or a proto map).  The
// dispatcher absorbs unknown / error operands, then `return_call`s
// (`__attribute__((musttail))`) into the kArena or kHost arm — never
// growing the wasm stack.  See `cel_runtime.c` for the `musttail`
// invariant; toolchain requires `-mtail-call` on clang and
// `wasmtime::Config::wasm_tail_call(true)`.
// cel:codegen-export
void cel_map_lookup(uint32_t out_slot, uint32_t map_slot, uint32_t key_slot);

// =====================================================================
// Aggregate-op kArena fast paths for maps.
//
// `cel_map_size_arena` writes a CEL_INT.  `cel_map_in_arena`
// reuses the existing `map_keys_equal` matcher used by
// `cel_map_lookup_arena`.  `cel_map_eq_arena` walks both maps
// pairwise — entry order is allowed to differ (langdef map
// equality is set-equality on entries).
// =====================================================================

void cel_map_size_arena(uint32_t out_slot, uint32_t map_slot);
void cel_map_in_arena(uint32_t out_slot, uint32_t key_slot, uint32_t map_slot);
void cel_map_eq_arena(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

// =====================================================================
// kDynamic dispatchers for aggregate map ops.  Same
// musttail-dispatch shape as `cel_map_lookup` (line 65): 3VL absorb,
// branch on operand kind, tail-call the kArena fast path or the kHost
// trampoline.  Codegen emits a `call` to these when ResolvePass
// cannot prove a single origin.
// =====================================================================

// cel:codegen-export
void cel_map_size(uint32_t out_slot, uint32_t map_slot);
// cel:codegen-export
void cel_map_in(uint32_t out_slot, uint32_t key_slot, uint32_t map_slot);
// cel:codegen-export
void cel_map_eq(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot);

// =====================================================================
// Map-key iteration helpers used by comprehensions over a
// `map(K, V)` source.  Per `rewrite/m5-comprehensions-design.md`
// §3.5 Option β (in-place key iteration; no keys-list
// materialisation) and `rewrite/wat/64_comprehension_exists_map.wat`.
//
// The iterator handle is an opaque u32 — codegen stores it in a wasm
// local and shuttles it between the three calls without inspecting it.
// Internally it is the arena offset of an 8-byte iterator-state struct
// `{ uint32_t header_ptr; uint32_t cursor; }` allocated by `iter_init`;
// the layout is a private runtime detail and may change without
// affecting the WAT shape.  A `0` handle is the sentinel "no entries"
// (empty / poisoned map): `iter_next(0)` returns 0 immediately and
// `iter_{key,value}_at` are silent no-ops on it.
//
// Semantics:
//   - `cel_map_iter_init(map_slot) -> handle`
//       Allocates iterator state for `map_slot`.  Returns the handle,
//       or 0 if the map is empty / poisoned / OOM.  Cursor starts at
//       "before first" — i.e. zero entries have been yielded.
//
//   - `cel_map_iter_next(handle) -> u32`
//       Advances the cursor.  Returns 1 if a new entry is available
//       (the next `iter_{key,value}_at` call will read that entry);
//       returns 0 if iteration is done.  Idempotent at the end:
//       subsequent calls return 0 without modifying state.
//
//   - `cel_map_iter_key_at(out_slot, handle)`
//       Copies the *current* entry's key as a CelValue into `out_slot`.
//       "Current" = the entry that the most recent `iter_next` returned
//       1 for.  Calling before any `iter_next` returned 1 is undefined
//       per the codegen contract (loop guard ensures next-then-read);
//       defensively a no-op rather than a crash.
//
//   - `cel_map_iter_value_at(out_slot, handle)`
//       Same as `iter_key_at` but copies the entry's value.  Required
//       by the two-iter-var map shape (`m.exists(k, v, p)`).
// =====================================================================

// cel:codegen-export
uint32_t cel_map_iter_init(uint32_t map_slot);
// cel:codegen-export
uint32_t cel_map_iter_next(uint32_t iter_handle);
// cel:codegen-export
void cel_map_iter_key_at(uint32_t out_slot, uint32_t iter_handle);
// cel:codegen-export
void cel_map_iter_value_at(uint32_t out_slot, uint32_t iter_handle);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_RUNTIME_CEL_MAP_H_
