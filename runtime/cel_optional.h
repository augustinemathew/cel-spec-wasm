// CEL optional<T> runtime kernels — M14 Slice A.
//
// All kernels follow the canonical slot-out ABI used elsewhere in the
// runtime: `(out_slot, arg0_slot, ..., argN-1_slot) -> void`, where
// every slot is a u32 byte offset into the shared linear memory.
//
// ── OptionalCell layout (32 bytes, 8-byte aligned, arena-allocated)
//
//   offset  0: u32 present   — 0=None, 1=Some
//   offset  4: u32 _pad      — alignment for inner
//   offset  8: CelValue inner — 24 bytes (kind + pad + payload)
//
// A `CelValue{kind=CEL_OPTIONAL, payload.opt=<u32 byte offset>}` points
// at the start of an OptionalCell.
//
// Cells are allocated by `cel_optional_none_at` / `cel_optional_of_at_v`
// (and the absent-key branch of `cel_select_optional_field_at_vv`).
// Lifetime: until the next `arena_reset`.
//
// ── OptionalCell immutability contract
//
// Kernels that read a cell via `opt_slot.payload.opt` MUST NOT write
// through that offset.  All writes go through `out_slot` (and possibly
// a fresh arena_alloc'd cell).  This contract lets a future
// shared-static-None optimisation publish a single static cell with
// `present=0` at a fixed reserved offset and have `cel_optional_none_at`
// return that offset unconditionally — see
// `doc/.../wat/m14_optional_of_int.wat` "OptionalCell immutability
// contract" section for the full discussion.
//
// ── 3VL absorption (CLAUDE.md / langdef parity)
//
//   - UNKNOWN / ERROR operand → propagate verbatim into out_slot (no
//     cell allocation, no kind change).
//   - Kind mismatch (e.g. `null.hasValue()`) → poison out_slot with
//     `CEL_ERR_TYPE_MISMATCH`.
//
// ── Design references
//
//   - `doc/implementation-plan/rewrite/m14-optionals.md` — slice plan.
//   - `doc/implementation-plan/rewrite/wat-traces.md` §M14.1-§M14.6 —
//     per-kernel walkthrough + memory map.
//   - `third_party/cel-cpp/runtime/optional_types.cc` — cel-cpp parity.
//   - `third_party/cel-cpp/common/values/optional_value.cc` —
//     OptionalValue semantics + the static-None reference design.

#ifndef CELWASM_COMPILER_V2_RUNTIME_CEL_OPTIONAL_H_
#define CELWASM_COMPILER_V2_RUNTIME_CEL_OPTIONAL_H_

#include <stdint.h>

#include "compiler_v2/runtime/cel_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// The 32-byte arena-allocated cell pointed at by
// `CelValue.payload.opt`.  Exposed in the header so polymorphic
// kernels in sibling translation units (`cel_runtime.c::equality_kernel`)
// can `(OptionalCell*)cv_at(cv.payload.opt)` without re-declaring the
// layout.
typedef struct {
  uint32_t present;
  uint32_t _pad;
  CelValue inner;
} OptionalCell;

_Static_assert(sizeof(OptionalCell) == 32,
               "OptionalCell must be 32 bytes (WAT m14_optional_of_int.wat)");
_Static_assert(_Alignof(OptionalCell) == 8,
               "OptionalCell must be 8-byte aligned");

// `optional.none()`.  arena_alloc'd 32-byte cell with `present=0`;
// `out_slot` receives `{CEL_OPTIONAL, payload.opt=<cell_off>}`.
// See `wat/m14_optional_none.wat`.
void cel_optional_none_at(uint32_t out_slot);

// `optional.of(v)`.  Wraps `*v_slot` in a Some cell.  UNKNOWN/ERROR on
// `v` propagates verbatim into `out_slot` (no cell allocated).  See
// `wat/m14_optional_of_int.wat`.
void cel_optional_of_at_v(uint32_t out_slot, uint32_t v_slot);

// `optional.ofNonZeroValue(v)`.  Some if `*v_slot` is non-zero per the
// per-kind matrix (m14-optionals.md §3.4 + `wat/m14_optional_of_non_zero.wat`),
// else None.  Host-backed kinds (CEL_LIST_HOST / CEL_MAP_HOST /
// CEL_MESSAGE) trap until Slice B / E adds the host trampolines.
void cel_optional_of_non_zero_at_v(uint32_t out_slot, uint32_t v_slot);

// `opt.hasValue()` (post-receiver-flatten — receiver is `opt_slot`,
// not args[0]).  Reads `cell.present`, writes `CEL_BOOL` to `out_slot`.
// Kind mismatch on `opt_slot` → TYPE_MISMATCH.  See
// `wat/m14_optional_has_value.wat`.
void cel_optional_has_value_at_v(uint32_t out_slot, uint32_t opt_slot);

// `opt.value()` (post-receiver-flatten).  Unwraps the inner CelValue
// into `out_slot`.  None → `CEL_ERROR/CEL_ERR_INVALID_ARGUMENT`
// (cel-cpp parity: `optional_value.cc::OptionalValueInterface::Value`).
void cel_optional_value_at_v(uint32_t out_slot, uint32_t opt_slot);

// `opt.or(other_opt)` (post-receiver-flatten).  Preserves optional-ness:
// the output kind is always CEL_OPTIONAL.  If `opt` is Some, copies
// `*opt_slot` to `out_slot`; else copies `*other_slot`.  See
// `wat-traces.md` §M14.4 "The `.or(other_opt)` overload" paragraph.
//
// Short-circuit codegen requirement: cel-cpp evaluates `other` only
// when `opt` is None (jump step).  This kernel is eager — both
// operands must be pre-evaluated.  For pure RHS (literal, ident) the
// behaviour is observationally identical; for impure RHS, codegen
// (Slice B) MUST emit a short-circuit branch instead of calling this
// kernel.  See wat-traces.md §M14.4 "Short-circuit codegen
// requirement".
void cel_optional_or_at_vv(uint32_t out_slot, uint32_t opt_slot,
                           uint32_t other_slot);

// `opt.orValue(default)` (post-receiver-flatten).  Unwraps: output
// kind is the inner kind (NOT CEL_OPTIONAL).  If `opt` is Some,
// copies `cell.inner` into `out_slot`; else copies `*default_slot`.
// See `wat/m14_optional_chain_or_value.wat`.
//
// Same short-circuit caveat as `cel_optional_or_at_vv` above.
void cel_optional_or_value_at_vv(uint32_t out_slot, uint32_t opt_slot,
                                 uint32_t default_slot);

// `obj.?field` AND `optional<obj>.field` — single kernel serving both
// paths (probe Q11; m14-optionals.md §1.7).  Polymorphic on
// `src.kind`:
//
//   - CEL_OPTIONAL: unwrap (`cell.present` ⇒ recurse on `cell.inner`;
//     `!cell.present` ⇒ result is also None).
//   - CEL_MAP_ARENA: tail-call `cel_map_lookup_arena` into a scratch
//     slot.  Absent key (`CEL_ERR_NO_SUCH_KEY` poison) ⇒ None.  Found
//     ⇒ Some(value).
//   - CEL_LIST_ARENA: tail-call `cel_list_at_arena`.  Absent
//     (`CEL_ERR_INDEX_OUT_OF_BOUNDS`) ⇒ None.  Found ⇒ Some(value).
//   - CEL_MAP_HOST / CEL_LIST_HOST / CEL_MESSAGE: trap until host
//     trampolines land (Slice B).
//   - other: poison out_slot with `CEL_ERR_TYPE_MISMATCH`.
//
// Non-absent errors from the lookup (TYPE_MISMATCH on a non-key kind,
// etc.) propagate verbatim into `out_slot` without being wrapped in
// an optional.  See `wat/m14_optional_select_field.wat`.
void cel_select_optional_field_at_vv(uint32_t out_slot, uint32_t src_slot,
                                     uint32_t key_slot);

// `{?key: opt_value}` map-literal entry — symmetric to
// `cel_map_insert_at_if_bool` but the predicate is the optional's
// `present` flag rather than a bool:
//
//   Some(v) ⇒ `cel_map_insert_at(map_slot, key_slot, &inner)`
//   None    ⇒ silent no-op
//   ERROR / UNKNOWN on `opt_value_slot` ⇒ propagate verbatim into
//     `map_slot` (aborts the literal per langdef 3VL)
//   non-CEL_OPTIONAL `opt_value_slot` ⇒ poison `map_slot` with
//     CEL_ERR_TYPE_MISMATCH
//
// If `map_slot` is not CEL_MAP_ARENA (e.g. already poisoned by an
// earlier 3VL absorption), the call is a no-op — same convention as
// `cel_map_insert_at_if_bool`.
//
// See `wat/m14_map_insert_if_present.wat`.
void cel_map_insert_at_if_present(uint32_t map_slot, uint32_t key_slot,
                                  uint32_t opt_value_slot);

// `[?elem]` list-literal entry — symmetric to
// `cel_list_append_at_if_bool` but the predicate is the optional's
// `present` flag.
//
//   Some(v) ⇒ `cel_list_append_at(list_slot, &inner)`
//   None    ⇒ silent no-op
//   ERROR / UNKNOWN on `opt_value_slot` ⇒ propagate verbatim into
//     `list_slot`
//   non-CEL_OPTIONAL `opt_value_slot` ⇒ poison `list_slot` with
//     CEL_ERR_TYPE_MISMATCH
//
// See `wat/m14_list_append_if_present.wat`.
void cel_list_append_at_if_present(uint32_t list_slot, uint32_t opt_value_slot);

// `Foo{?field: opt_value}` proto-literal entry — the third member
// of the optional-payload predicate trio.  Structurally identical
// to the map/list variants except the inner "actually set" step
// delegates to a host trampoline (`cel_host.cel_set_field`) rather
// than a pure-wasm primitive:
//
//   Some(v) ⇒ `cel_host.cel_set_field(msg_slot, field_ref_id, &inner)`
//   None    ⇒ silent no-op (field stays unset in the proto, so
//             `has(msg.field)` returns false)
//   ERROR / UNKNOWN on `opt_value_slot` ⇒ propagate verbatim into
//     `msg_slot`
//   non-CEL_OPTIONAL `opt_value_slot` ⇒ poison `msg_slot` with
//     CEL_ERR_TYPE_MISMATCH
//
// If `msg_slot` is not CEL_MESSAGE (e.g. already poisoned by an
// upstream 3VL absorption), the call is a no-op — same convention
// as the map/list `_if_present` siblings.
//
// The kernel is pure wasm; only the inner `cel_set_field` step
// crosses to the host.  See `wat/m14_proto_set_field_if_present.wat`
// and m14-optionals.md §0 "Scope pull-in 2026-05-22" for the
// rationale.
void cel_set_field_at_if_present(uint32_t msg_slot, uint32_t field_ref_id,
                                 uint32_t opt_value_slot);

#ifdef __cplusplus
}
#endif

#endif  // CELWASM_COMPILER_V2_RUNTIME_CEL_OPTIONAL_H_
