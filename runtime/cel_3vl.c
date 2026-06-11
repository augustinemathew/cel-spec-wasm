// 3VL / control-flow helpers.
//
// Carved out of cel_runtime.c per
// `rewrite/cel-runtime-c-split-plan.md`.  Depends on cel_arena.c
// (arena_alloc, cel_value_at) via cel_internal.h and the public
// cel_arena.h.

#include "runtime/cel_3vl.h"

#include <stdint.h>

#include "runtime/cel_arena.h"
#include "runtime/cel_internal.h"
#include "runtime/cel_log.h"

// Sorted-deduplicated merge walk over two pre-sorted u32 id arrays.
// Mirrors v1's `merge_sorted_ids` shape (compiler/runtime/cel_runtime.c).
// Returns the post-dedup length; never reads past the input bounds.
static uint32_t merge_sorted_id_arrays(const uint32_t* ids_a, uint32_t len_a,
                                       const uint32_t* ids_b, uint32_t len_b,
                                       uint32_t* out) {
  uint32_t i = 0;
  uint32_t j = 0;
  uint32_t k = 0;
  while (i < len_a && j < len_b) {
    uint32_t ai = ids_a[i];
    uint32_t bj = ids_b[j];
    if (ai < bj) {
      out[k++] = ai;
      ++i;
    } else if (ai > bj) {
      out[k++] = bj;
      ++j;
    } else {
      out[k++] = ai;
      ++i;
      ++j;
    }
  }
  while (i < len_a) {
    out[k++] = ids_a[i++];
  }
  while (j < len_b) {
    out[k++] = ids_b[j++];
  }
  return k;
}

// Allocates a fresh 2-word UnknownSet descriptor `{ids_off, len}` in
// the bump arena and returns its byte offset, or 0 on out-of-arena.
static uint32_t alloc_unknown_descriptor(uint32_t ids_off, uint32_t len) {
  uint32_t desc_off = arena_alloc(2u * (uint32_t)sizeof(uint32_t));
  if (desc_off == 0) return 0;
  uint32_t* desc = (uint32_t*)(cel_memory_base_() + desc_off);
  desc[0] = ids_off;
  desc[1] = len;
  return desc_off;
}

// Walks two non-empty UnknownSet descriptors at `set_a`/`set_b`, mints a
// fresh sorted-deduped descriptor in the bump arena, and returns its
// byte offset.  Returns 0 on out-of-arena.  Snapshots descriptor scalars
// before any arena_alloc — wasm32 `memory.grow` can relocate the linear
// memory base, so pointers must be re-derived after each bump.
static uint32_t merge_unknown_descriptors(uint32_t set_a, uint32_t set_b) {
  uint32_t* desc_a = (uint32_t*)(cel_memory_base_() + set_a);
  uint32_t ids_a_off = desc_a[0];
  uint32_t len_a = desc_a[1];
  uint32_t* desc_b = (uint32_t*)(cel_memory_base_() + set_b);
  uint32_t ids_b_off = desc_b[0];
  uint32_t len_b = desc_b[1];

  uint32_t max_total = len_a + len_b;
  uint32_t bytes = max_total * (uint32_t)sizeof(uint32_t);
  if (bytes == 0) bytes = (uint32_t)sizeof(uint32_t);
  uint32_t out_ids = arena_alloc(bytes);
  if (out_ids == 0) return 0;

  const uint32_t* ids_a = (const uint32_t*)(cel_memory_base_() + ids_a_off);
  const uint32_t* ids_b = (const uint32_t*)(cel_memory_base_() + ids_b_off);
  uint32_t* dst = (uint32_t*)(cel_memory_base_() + out_ids);
  uint32_t k = merge_sorted_id_arrays(ids_a, len_a, ids_b, len_b, dst);
  return alloc_unknown_descriptor(out_ids, k);
}

// Writes `(CEL_UNKNOWN, payload.unk = desc_off)` into the slot.  Re-derives
// the slot pointer because `arena_alloc` may have relocated linear memory.
static void write_unknown_at(uint32_t out_slot, uint32_t desc_off) {
  CelValue* out = cel_value_at(out_slot);
  out->kind = CEL_UNKNOWN;
  out->payload.unk = desc_off;
}

void cel_unknown_merge(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (a->kind != CEL_UNKNOWN || b->kind != CEL_UNKNOWN) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  uint32_t set_a = a->payload.unk;
  uint32_t set_b = b->payload.unk;
  // An empty UnknownSet (payload.unk == 0) is a legal UNKNOWN with no
  // recorded provenance.  Every production writer (host trampolines,
  // activation marshal, host-fn returns) mints a non-empty descriptor,
  // so an empty side reaches here only from hand-built fixtures; treat
  // it as "no new ids to contribute" — both empty → empty UNKNOWN.
  if (set_a == 0 && set_b == 0) {
    write_unknown_at(out_slot, 0);
    return;
  }
  if (set_a == 0) {
    write_unknown_at(out_slot, set_b);
    return;
  }
  if (set_b == 0) {
    write_unknown_at(out_slot, set_a);
    return;
  }
  uint32_t desc_off = merge_unknown_descriptors(set_a, set_b);
  if (desc_off == 0) {
    // Re-derive `out` after the failed arena_alloc: memory.grow may have
    // relocated base before the failure.
    poison(cel_value_at(out_slot), CEL_ERR_OVERFLOW);
    return;
  }
  write_unknown_at(out_slot, desc_off);
}

void cel_copy_slot(uint32_t dst_slot, uint32_t src_slot) {
  CelValue* dst = cel_value_at(dst_slot);
  const CelValue* src = cel_value_at(src_slot);
  *dst = *src;
}

static int is_3vl_kind(uint32_t k) {
  return k == CEL_BOOL || k == CEL_UNKNOWN || k == CEL_ERROR;
}

// Shared non-bool tail for `cel_and` / `cel_or`.  Preconditions:
// both operands are valid 3VL kinds, neither hit the absorbing
// bool, and the surviving-bool cases (`true && X = X`,
// `false || X = X`) were already handled — each side is ERROR or
// UNKNOWN.  Applies the LOGIC-op precedence: UNKNOWN > ERROR (both
// UNKNOWN merges the attribute-id sets), the opposite of the
// strict-op rule (`absorb_3vl_binary`) — once the unknown resolves,
// the bool it stands for may short-circuit the error away, so the
// unknown is the more informative outcome.  Matches cel-cpp's
// `LogicalOpStep::Calculate` (eval/eval/logic_step.cc —
// MergeUnknowns runs BEFORE the ErrorValue scan); oracle-pinned by
// UnknownPayloadOracle.{And,Or}{UnknownLeftErrorRight,
// ErrorLeftUnknownRight}IsUnknown in
// testdata/cel_cpp_oracle_unknown_payload_test.cc.
static void logic_unknown_error_tail(uint32_t out_slot, uint32_t a_slot,
                                     uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (a->kind == CEL_UNKNOWN && b->kind == CEL_UNKNOWN) {
    cel_unknown_merge(out_slot, a_slot, b_slot);
    return;
  }
  if (a->kind == CEL_UNKNOWN) {
    *out = *a;
    return;
  }  // UNKNOWN > ERROR
  if (b->kind == CEL_UNKNOWN) {
    *out = *b;
    return;
  }
  *out = *a;  // both ERROR → left (cel-cpp checks args[0] first)
}

// 3VL truth table for `&&` (langdef §"Logical Operators").
// Non-strict semantics: `false && X = false` for any X, including
// ERROR / UNKNOWN / non-3VL; symmetric on the right.  Once both
// operands are known to be 3VL (and neither is OK(false)),
// `true && X = X`, then the UNKNOWN-over-ERROR logic-op tail above.
// A non-3VL operand survives only via the OK(false) absorber;
// otherwise the result is a type error.
void cel_and(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  // OK(false) absorbs everything — including a non-3VL other side.
  if (a->kind == CEL_BOOL && a->payload.b == 0) {
    write_bool(out, 0);
    return;
  }
  if (b->kind == CEL_BOOL && b->payload.b == 0) {
    write_bool(out, 0);
    return;
  }
  // Past the absorber: every operand must be a valid 3VL kind.
  if (!is_3vl_kind(a->kind) || !is_3vl_kind(b->kind)) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (a->kind == CEL_BOOL) {
    *out = *b;
    return;
  }  // a == true → result = b
  if (b->kind == CEL_BOOL) {
    *out = *a;
    return;
  }  // b == true → result = a
  logic_unknown_error_tail(out_slot, a_slot, b_slot);
}

// Symmetric to `cel_and`: `true || X = true`; `false || X = X`.
// Same UNKNOWN-over-ERROR logic-op tail as `cel_and`.
void cel_or(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (a->kind == CEL_BOOL && a->payload.b != 0) {
    write_bool(out, 1);
    return;
  }
  if (b->kind == CEL_BOOL && b->payload.b != 0) {
    write_bool(out, 1);
    return;
  }
  if (!is_3vl_kind(a->kind) || !is_3vl_kind(b->kind)) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (a->kind == CEL_BOOL) {
    *out = *b;
    return;
  }  // a == false → result = b
  if (b->kind == CEL_BOOL) {
    *out = *a;
    return;
  }
  logic_unknown_error_tail(out_slot, a_slot, b_slot);
}

void cel_not(uint32_t out_slot, uint32_t a_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  if (a->kind == CEL_BOOL) {
    write_bool(out, a->payload.b ? 0 : 1);
    return;
  }
  if (a->kind == CEL_UNKNOWN || a->kind == CEL_ERROR) {
    *out = *a;
    return;
  }
  poison(out, CEL_ERR_TYPE_MISMATCH);
}
