// M14 Slice A — CEL optional<T> runtime kernels.
//
// See `compiler_v2/runtime/cel_optional.h` for the public ABI + the
// OptionalCell layout + the immutability contract.  Per-kernel
// rationale lives in `doc/.../wat/m14_optional_*.wat` headers and
// `doc/.../wat-traces.md` §M14.1-§M14.6.

#include "compiler_v2/runtime/cel_optional.h"

#include <stdint.h>

#include "compiler_v2/runtime/cel_arena.h"
#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/cel_internal.h"
#include "compiler_v2/runtime/cel_list.h"
#include "compiler_v2/runtime/cel_log.h"
#include "compiler_v2/runtime/cel_map.h"

// `cel_host.cel_set_field` — Layer-2 proto-field write.  Imported
// from the host module under `wasm32-wasi-threads`; on the host
// build (unit tests) the weak stub is a no-op that tests override
// with a strong symbol to record invocations.  Same pattern as
// `cel_host_cel_map_lookup` / `cel_host_cel_list_at` in
// `cel_runtime.c`.
#ifdef __wasm__
extern void cel_host_cel_set_field(uint32_t msg_slot, uint32_t field_ref_id,
                                   uint32_t value_slot)
    __attribute__((import_module("cel_host"), import_name("cel_set_field")));
#else
__attribute__((weak)) void
cel_host_cel_set_field(  // NOLINT(misc-use-internal-linkage)
    uint32_t msg_slot, uint32_t field_ref_id, uint32_t value_slot) {
  (void)msg_slot;
  (void)field_ref_id;
  (void)value_slot;
}
#endif

// ── Cell helpers ───────────────────────────────────────────

static OptionalCell* cell_at(uint32_t cell_off) {
  return (OptionalCell*)(cel_memory_base_() + cell_off);
}

// Allocate a fresh 32-byte OptionalCell.  Returns 0 on OOM.
// `arena_alloc` zero-initialises, so the caller only writes the
// fields it needs to be non-zero.
static uint32_t alloc_cell(void) {
  return arena_alloc((uint32_t)sizeof(OptionalCell));
}

// Stamp `out` to {CEL_OPTIONAL, payload.opt=cell_off}.  `opt` is the
// first 4 bytes of the 16-byte payload union; the remaining 12 bytes
// MUST NOT be written via a different union arm because every arm
// overlaps `opt` at offset 0 — a `payload.dur.seconds = 0` would
// clobber the cell offset we just wrote.  The codegen prologue
// zeroes workspace slots up front, so the trailing 12 bytes are
// already zero when we land here.
static void write_optional(CelValue* out, uint32_t cell_off) {
  out->kind = CEL_OPTIONAL;
  out->_pad = 0;
  out->payload.opt = cell_off;
}

// ── Zero-value predicate matrix (m14-optionals.md §3.4) ────

// Forward declarations of the per-kind helpers used by ofNonZeroValue.
// CEL_LIST_HOST / CEL_MAP_HOST / CEL_MESSAGE traps until Slice B
// (host trampoline for the host zero-predicate); CEL_OPTIONAL recurses
// into the inner cell.
static int is_zero_value(const CelValue* v) {
  switch (v->kind) {
    case CEL_NULL:
      // Any null is the zero null.
      return 1;
    case CEL_BOOL:
      return v->payload.b == 0;
    case CEL_INT:
      return v->payload.i == 0;
    case CEL_UINT:
      return v->payload.u == 0;
    case CEL_DOUBLE:
      // Bit-exact +0.0 / -0.0 both compare zero (==), matching cel-cpp
      // `DoubleValue::IsZeroValue` which uses `== 0.0`.
      return v->payload.d == 0.0;
    case CEL_STRING:
      return v->payload.s.len == 0;
    case CEL_BYTES:
      return v->payload.bytes.len == 0;
    case CEL_LIST_ARENA: {
      ArenaListHeader* hdr =
          (ArenaListHeader*)(cel_memory_base_() +
                             v->payload.arena_list.header_ptr);
      return hdr->count == 0;
    }
    case CEL_MAP_ARENA: {
      ArenaMapHeader* hdr = (ArenaMapHeader*)(cel_memory_base_() +
                                              v->payload.arena_map.header_ptr);
      return hdr->count == 0;
    }
    case CEL_DURATION:
    case CEL_TIMESTAMP:
      return v->payload.dur.seconds == 0 && v->payload.dur.nanos == 0;
    case CEL_TYPE:
      // A type carries identity; never zero.  cel-cpp:
      // `TypeValue::IsZeroValue() = false` (no override).
      return 0;
    case CEL_OPTIONAL: {
      // Recursive descent: outer None is zero; outer Some(inner) is
      // zero iff inner is zero.  Matches cel-cpp
      // `OptionalValue::IsZeroValue()`.
      OptionalCell* cell = cell_at(v->payload.opt);
      if (cell->present == 0) return 1;
      return is_zero_value(&cell->inner);
    }
    case CEL_LIST_HOST:
    case CEL_MAP_HOST:
    case CEL_MESSAGE:
      // Host-backed zero predicate needs a host trampoline (Slice B):
      // an empty proto repeated/map field or a default-constructed
      // proto message is zero per cel-cpp parsed_message_value.cc:78.
      // Trap until the trampoline lands so the call site shows up in
      // a backtrace rather than miscompiling.
      __builtin_trap();
    default:
      // CEL_UNKNOWN / CEL_ERROR should never reach here — callers
      // absorb via `absorb_3vl_unary` upstream.  Treat as non-zero
      // to be safe (so the operand propagates instead of vanishing).
      return 0;
  }
}

// ── Kernels: simple constructors ────────────────────────────

void cel_optional_none_at(uint32_t out_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  uint32_t cell_off = alloc_cell();
  if (cell_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  // arena_alloc zero-initialises, so cell.present is already 0 (None)
  // and cell.inner is zeroed.  Just write the out_slot.
  write_optional(out, cell_off);
}

void cel_optional_of_at_v(uint32_t out_slot, uint32_t v_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  uint32_t cell_off = alloc_cell();
  if (cell_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  OptionalCell* cell = cell_at(cell_off);
  cell->present = 1;
  cell->inner = *v;
  write_optional(out, cell_off);
}

void cel_optional_of_non_zero_at_v(uint32_t out_slot, uint32_t v_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (is_zero_value(v)) {
    cel_optional_none_at(out_slot);
    return;
  }
  cel_optional_of_at_v(out_slot, v_slot);
}

// ── Kernels: accessors ──────────────────────────────────────

// Validate `*opt` and write any 3VL / kind-mismatch envelope into
// `*out`.  Returns 0 if the operand is a well-formed optional that
// the caller should now process; returns 1 if the operand was 3VL or
// the wrong kind (and `*out` already holds the propagated /
// poisoned result).
//
// Returning bool rather than `OptionalCell*` keeps the LTO-inlined
// codegen clean on wasm: clang's earlier `OptionalCell* require_optional`
// shape emitted a spurious `cel_memory_base_() == 0` early-return on
// wasm where memory_base is legitimately 0, leaving the kernel out_slot
// untouched.  Splitting validation from the cell load fixes that.
static int absorb_or_typecheck_optional(CelValue* out, const CelValue* opt) {
  if (absorb_3vl_unary(out, opt)) return 1;
  if (opt->kind != CEL_OPTIONAL) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  return 0;
}

void cel_optional_has_value_at_v(uint32_t out_slot, uint32_t opt_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* opt = cel_value_at(opt_slot);
  if (absorb_or_typecheck_optional(out, opt)) return;
  OptionalCell* cell = cell_at(opt->payload.opt);
  write_bool(out, cell->present != 0);
}

void cel_optional_value_at_v(uint32_t out_slot, uint32_t opt_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* opt = cel_value_at(opt_slot);
  if (absorb_or_typecheck_optional(out, opt)) return;
  OptionalCell* cell = cell_at(opt->payload.opt);
  if (cell->present == 0) {
    // cel-cpp parity: `OptionalValueInterface::Value` on None returns
    // an error.  cel-cpp uses kInvalidArgument; mirror that.
    poison(out, CEL_ERR_INVALID_ARGUMENT);
    return;
  }
  *out = cell->inner;
}

void cel_optional_or_at_vv(uint32_t out_slot, uint32_t opt_slot,
                           uint32_t other_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* opt = cel_value_at(opt_slot);
  const CelValue* other = cel_value_at(other_slot);
  // Bind-bias on `opt`'s 3VL — if `opt` is poisoned the result is
  // poisoned regardless of `other`.  If `opt` is Some, `other`'s
  // 3VL is discarded (matches cel-cpp's jump-step semantics for the
  // common case; for impure RHS, codegen short-circuits per
  // wat-traces.md §M14.4).
  if (absorb_3vl_unary(out, opt)) return;
  if (opt->kind != CEL_OPTIONAL) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  OptionalCell* opt_cell = cell_at(opt->payload.opt);
  if (opt_cell->present) {
    *out = *opt;
    return;
  }
  // Fall through to other.  Validate kind on the fallthrough so a
  // type-mismatched `other` produces TYPE_MISMATCH (matches the eager
  // ABI; for short-circuit codegen the impure RHS branch never runs
  // anyway).
  if (absorb_3vl_unary(out, other)) return;
  if (other->kind != CEL_OPTIONAL) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  *out = *other;
}

void cel_optional_or_value_at_vv(uint32_t out_slot, uint32_t opt_slot,
                                 uint32_t default_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* opt = cel_value_at(opt_slot);
  if (absorb_3vl_unary(out, opt)) return;
  if (opt->kind != CEL_OPTIONAL) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  OptionalCell* opt_cell = cell_at(opt->payload.opt);
  if (opt_cell->present) {
    *out = opt_cell->inner;
    return;
  }
  // Eager-evaluated default: in the short-circuit codegen this branch
  // would be the only path that ran, so any UNKNOWN/ERROR on default
  // surfaces here.  cel-cpp's jump step has the same effect.
  const CelValue* dflt = cel_value_at(default_slot);
  *out = *dflt;
}

// ── Kernel: shared select-field ────────────────────────────

// Is `e` one of the absent-key error codes that the select-field
// kernel reinterprets as `optional.none()`?  See
// `wat/m14_optional_select_field.wat` lines 60-71 for the rationale.
static int is_absent_error(const CelValue* e) {
  if (e->kind != CEL_ERROR) return 0;
  return e->payload.err == CEL_ERR_NO_SUCH_KEY ||
         e->payload.err == CEL_ERR_INDEX_OUT_OF_BOUNDS ||
         e->payload.err == CEL_ERR_FIELD_NOT_FOUND;
}

// Perform the underlying lookup for a non-optional (post-unwrap)
// source kind, into the scratch slot at byte offset `scratch_off`.
// Returns 1 if the lookup wrote a result to scratch (caller examines
// it for absent / found / error); returns 0 after writing a
// TYPE_MISMATCH poison into `out` for unsupported kinds.
static int dispatch_lookup(CelValue* out, const CelValue* src,
                           uint32_t src_slot, uint32_t key_slot,
                           uint32_t scratch_off) {
  switch (src->kind) {
    case CEL_MAP_ARENA:
      cel_map_lookup_arena(scratch_off, src_slot, key_slot);
      return 1;
    case CEL_LIST_ARENA:
      cel_list_at_arena(scratch_off, src_slot, key_slot);
      return 1;
    case CEL_MAP_HOST:
    case CEL_LIST_HOST:
    case CEL_MESSAGE:
      // Host-backed select-field needs a host trampoline that wraps
      // the existing `cel_host.cel_map_lookup` / `cel_host.cel_list_at`
      // / `cel_host.cel_get_field` paths and surfaces FIELD_NOT_FOUND
      // as a CEL_ERR_NO_SUCH_KEY / _INDEX_OUT_OF_BOUNDS / _FIELD_NOT_FOUND
      // poison the way the arena helpers do.  Until that trampoline
      // exists, trap so an unintended use shows up at the call site
      // rather than silently miscompiling.  Today this path is
      // unreachable for the conformance corpus because every
      // optional value the corpus constructs holds an arena-backed
      // map or list; a future host-backed `optional<message>`
      // binding (e.g. `Activation::Bind("opt", Value::Optional(msg))`)
      // would light it up.
      __builtin_trap();
    default:
      poison(out, CEL_ERR_TYPE_MISMATCH);
      return 0;
  }
}

// Interpret the lookup result the dispatcher wrote into `cell->inner`.
// Returns true if it published a result through `out` (either the
// final CEL_OPTIONAL or an error/unknown propagation); false to tell
// the caller "found a real value, publish as Some(value)."
static int finalize_lookup_result(CelValue* out, OptionalCell* cell,
                                  uint32_t cell_off) {
  if (is_absent_error(&cell->inner)) {
    // Absent key/index/field ⇒ None.  Zero the inner so the cell
    // looks like a fresh None (matches the cel_optional_none_at
    // invariant; important for memcmp-style cell-equality).
    cell->present = 0;
    cell->inner.kind = CEL_NULL;
    cell->inner._pad = 0;
    cell->inner.payload.i = 0;
    write_optional(out, cell_off);
    return 1;
  }
  if (cell->inner.kind == CEL_ERROR || cell->inner.kind == CEL_UNKNOWN) {
    // Non-absent error/unknown — propagate.  The fresh cell becomes
    // unreachable; arena_reset reclaims.
    *out = cell->inner;
    return 1;
  }
  return 0;
}

void cel_select_optional_field_at_vv(uint32_t out_slot, uint32_t src_slot,
                                     uint32_t key_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* src = cel_value_at(src_slot);
  const CelValue* key = cel_value_at(key_slot);
  if (absorb_3vl_binary(out, src, key)) return;

  // Alloc the result cell up front; its `inner` field doubles as the
  // scratch buffer for the underlying lookup.
  uint32_t cell_off = alloc_cell();
  if (cell_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  OptionalCell* cell = cell_at(cell_off);

  // If src is optional, unwrap.  None propagates through immediately
  // (cell stays present=0, inner stays zero from arena_alloc).
  uint32_t inner_src_slot = src_slot;
  if (src->kind == CEL_OPTIONAL) {
    OptionalCell* src_cell = cell_at(src->payload.opt);
    if (src_cell->present == 0) {
      write_optional(out, cell_off);
      return;
    }
    inner_src_slot = src->payload.opt + (uint32_t)offsetof(OptionalCell, inner);
    src = &src_cell->inner;
  }

  const uint32_t inner_off = cell_off + (uint32_t)offsetof(OptionalCell, inner);
  if (!dispatch_lookup(out, src, inner_src_slot, key_slot, inner_off)) {
    return;  // dispatch_lookup poisoned out for TYPE_MISMATCH.
  }
  if (finalize_lookup_result(out, cell, cell_off)) return;

  // Found a real value; publish as Some(value).
  cell->present = 1;
  write_optional(out, cell_off);
}

// ── Kernels: predicate-gated insert / append for `{?k: v}` / `[?e]`

// Shared 3VL + kind validation for the `_if_present` kernels.  Returns
// 1 if the caller should stop (either propagation/poison already
// written to `dst_slot`, or a None no-op); 0 if `*cell_out` was
// populated with a Some cell ready to read.
static int absorb_optional_predicate(CelValue* dst, const CelValue* opt,
                                     OptionalCell** cell_out) {
  if (opt->kind == CEL_ERROR || opt->kind == CEL_UNKNOWN) {
    *dst = *opt;
    return 1;
  }
  if (opt->kind != CEL_OPTIONAL) {
    poison(dst, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  OptionalCell* cell = cell_at(opt->payload.opt);
  if (cell->present == 0) return 1;  // None → silent no-op
  *cell_out = cell;
  return 0;
}

void cel_map_insert_at_if_present(uint32_t map_slot, uint32_t key_slot,
                                  uint32_t opt_value_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  if (m->kind != CEL_MAP_ARENA) return;
  const CelValue* opt = cel_value_at(opt_value_slot);
  OptionalCell* cell = NULL;
  if (absorb_optional_predicate(m, opt, &cell)) return;
  // Reuse cell.inner as the value slot — its memory offset is stable
  // until the next arena_reset, so passing the inner's offset to
  // cel_map_insert_at is byte-equivalent to staging the inner into a
  // workspace scratch slot first.
  uint32_t inner_off =
      opt->payload.opt + (uint32_t)offsetof(OptionalCell, inner);
  cel_map_insert_at(map_slot, key_slot, inner_off);
  (void)cell;  // populated for symmetry; offset comes from opt->payload.opt
}

void cel_list_append_at_if_present(uint32_t list_slot,
                                   uint32_t opt_value_slot) {
  CEL_LOG("enter");
  CelValue* l = cel_value_at(list_slot);
  if (l->kind != CEL_LIST_ARENA) return;
  const CelValue* opt = cel_value_at(opt_value_slot);
  OptionalCell* cell = NULL;
  if (absorb_optional_predicate(l, opt, &cell)) return;
  uint32_t inner_off =
      opt->payload.opt + (uint32_t)offsetof(OptionalCell, inner);
  cel_list_append_at(list_slot, inner_off);
  (void)cell;
}

void cel_set_field_at_if_present(uint32_t msg_slot, uint32_t field_ref_id,
                                 uint32_t opt_value_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(msg_slot);
  if (m->kind != CEL_MESSAGE) return;
  const CelValue* opt = cel_value_at(opt_value_slot);
  OptionalCell* cell = NULL;
  if (absorb_optional_predicate(m, opt, &cell)) return;
  // The host trampoline reads a 24-byte CelValue at value_slot.
  // The inner CelValue lives 8 bytes into the OptionalCell (past
  // `present` + `_pad`); passing that offset is byte-equivalent to
  // staging the inner into a workspace slot first.  Stable until
  // the next arena_reset, same as any arena offset.
  uint32_t inner_off =
      opt->payload.opt + (uint32_t)offsetof(OptionalCell, inner);
  cel_host_cel_set_field(msg_slot, field_ref_id, inner_off);
  (void)cell;
}
