#include "compiler_v2/runtime/cel_runtime.h"

#include "compiler_v2/runtime/cel_internal.h"

// ---- map runtime ---------------------------------------------------------
//
// Three-path dispatch design (`map-list-dispatch.md`):
//   - kArena   : `cel_map_lookup_arena`    (pure wasm, called directly)
//   - kHost    : `cel_host.cel_map_lookup` (host trampoline, called direct)
//   - kDynamic : `cel_map_lookup`          (this dispatcher, tail-calls)
//
// Map literals construct via `cel_map_create` + `cel_map_insert`; both
// only ever produce CEL_MAP_ARENA values — kHost values originate from
// proto reflection or `Activation::Bind`, never from emitted codegen.

static int is_valid_map_key_kind(uint32_t kind) {
  return kind == CEL_BOOL || kind == CEL_INT || kind == CEL_UINT ||
         kind == CEL_STRING;
}

// `CmpResult` + `numeric_compare_kernel` + `is_numeric_kind` come
// from cel_internal.h (defined in cel_compare.c).

// Slice 1.6: numeric-key map equality consults the polymorphic
// `numeric_compare_kernel` — handles every {int, uint, double}
// pair, including double-typed *queries* against int/uint keys.
// `double` is NOT a valid map-key kind (`is_valid_map_key_kind`
// rejects it on `cel_map_insert`), but it CAN appear on the
// query side: `1.0 in {1: "a"}` is allowed.  `numeric_keys_equal`
// already handled the int↔uint cross-kind case via mathematical
// comparison; the polymorphic kernel adds double↔int / double↔uint.

// NOLINTNEXTLINE(misc-use-internal-linkage)
int map_keys_equal(const CelValue* a, const CelValue* b) {
  if (a->kind == CEL_BOOL && b->kind == CEL_BOOL) {
    return (a->payload.b != 0) == (b->payload.b != 0);
  }
  if (a->kind == CEL_STRING && b->kind == CEL_STRING) {
    return spans_equal(a->payload.s, b->payload.s);
  }
  if (is_numeric_kind(a->kind) && is_numeric_kind(b->kind)) {
    return numeric_compare_kernel(a, b) == kCmpEqual;
  }
  return 0;
}

static ArenaMapHeader* arena_map_header(const CelValue* m) {
  return (ArenaMapHeader*)(cel_memory_base_() +
                           m->payload.arena_map.header_ptr);
}

static CelValue* arena_map_entry_key(ArenaMapHeader* hdr, uint32_t i) {
  return (CelValue*)(cel_memory_base_() + hdr->entries_offset +
                     ((size_t)kCelMapEntryStride * i));
}

static CelValue* arena_map_entry_val(ArenaMapHeader* hdr, uint32_t i) {
  return (CelValue*)(cel_memory_base_() + hdr->entries_offset +
                     ((size_t)kCelMapEntryStride * i) + sizeof(CelValue));
}

void cel_map_create(uint32_t out_slot, uint32_t initial_capacity) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  uint32_t hdr_off = cel_alloc((uint32_t)sizeof(ArenaMapHeader));
  if (hdr_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint32_t entries_off = 0;
  if (initial_capacity > 0) {
    entries_off =
        cel_alloc((uint32_t)((size_t)kCelMapEntryStride * initial_capacity));
    if (entries_off == 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return;
    }
  }
  ArenaMapHeader* hdr = (ArenaMapHeader*)(cel_memory_base_() + hdr_off);
  hdr->count = 0;
  hdr->capacity = initial_capacity;
  hdr->entries_offset = entries_off;
  hdr->_pad = 0;
  out->kind = CEL_MAP_ARENA;
  out->payload.arena_map.header_ptr = hdr_off;
}

void cel_map_insert(uint32_t map_slot, uint32_t key_slot, uint32_t value_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  // If the map is already poisoned (e.g. an earlier insert failed),
  // every subsequent insert is a no-op — error sticks.
  if (m->kind != CEL_MAP_ARENA) {
    return;
  }
  CelValue* key = cel_value_at(key_slot);
  CelValue* val = cel_value_at(value_slot);
  if (!is_valid_map_key_kind(key->kind)) {
    poison(m, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaMapHeader* hdr = arena_map_header(m);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (map_keys_equal(arena_map_entry_key(hdr, i), key)) {
      poison(m, CEL_ERR_DUPLICATE_KEY);
      return;
    }
  }
  // Map literals are fixed-length — codegen sized capacity to the
  // exact entry count via `cel_map_create`.  Exceeding it means
  // codegen drifted out of sync with the runtime; poison defensively
  // so the bug surfaces at the first observable boundary instead of
  // silently scribbling past the entries arena.
  if (hdr->count >= hdr->capacity) {
    poison(m, CEL_ERR_OVERFLOW);
    return;
  }
  *arena_map_entry_key(hdr, hdr->count) = *key;
  *arena_map_entry_val(hdr, hdr->count) = *val;
  hdr->count++;
}

// M5.B Slice G — dynamic-map insert for `transformMap` /
// `transformMapEntry` accumulators.  Differs from `cel_map_insert`
// in three ways:
//   1. Geometric growth (2× capacity, min 4) when full; copies
//      existing entries into the new bucket array.  Old run is
//      abandoned in the forward-only arena (same trade-off as
//      cel_list_append_at).
//   2. Key-collision OVERWRITES the existing entry's value
//      (last-write-wins, matching cel-cpp's transformMap runtime
//      behaviour — see design §9.6).
//   3. CEL_ERROR / CEL_UNKNOWN in either key OR value propagates
//      the error verbatim into the map slot; subsequent inserts
//      are silent no-ops (error sticks).
void cel_map_insert_at(uint32_t map_slot, uint32_t key_slot,
                       uint32_t value_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  if (m->kind != CEL_MAP_ARENA) return;
  CelValue* key = cel_value_at(key_slot);
  CelValue* val = cel_value_at(value_slot);
  if (key->kind == CEL_ERROR || key->kind == CEL_UNKNOWN) {
    *m = *key;
    return;
  }
  if (val->kind == CEL_ERROR || val->kind == CEL_UNKNOWN) {
    *m = *val;
    return;
  }
  if (!is_valid_map_key_kind(key->kind)) {
    poison(m, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaMapHeader* hdr = arena_map_header(m);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (map_keys_equal(arena_map_entry_key(hdr, i), key)) {
      *arena_map_entry_val(hdr, i) = *val;
      return;
    }
  }
  // PRESIZE_INVARIANT: capacity is sized by codegen — N for
  // literals, iter_range.count for accus.  Trap rather than
  // grow / poison so a codegen regression surfaces here.
  if (hdr->count >= hdr->capacity) __builtin_trap();
  *arena_map_entry_key(hdr, hdr->count) = *key;
  *arena_map_entry_val(hdr, hdr->count) = *val;
  hdr->count++;
}

void cel_map_lookup_arena(uint32_t out_slot, uint32_t map_slot,
                          uint32_t key_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  CelValue* key = cel_value_at(key_slot);
  if (key->kind == CEL_UNKNOWN || key->kind == CEL_ERROR) {
    *out = *key;
    return;
  }
  CelValue* m = cel_value_at(map_slot);
  // 3VL on operand — same path the dispatcher uses; codegen calls
  // this directly only when origin is kArena, but `m` could still be
  // a poisoned arena map if a prior insert failed.
  if (m->kind == CEL_UNKNOWN || m->kind == CEL_ERROR) {
    *out = *m;
    return;
  }
  ArenaMapHeader* hdr = arena_map_header(m);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (map_keys_equal(arena_map_entry_key(hdr, i), key)) {
      *out = *arena_map_entry_val(hdr, i);
      return;
    }
  }
  poison(out, CEL_ERR_NO_SUCH_KEY);
}

// kDynamic dispatcher.  `__attribute__((musttail))` forces clang to
// emit `return_call` (or `return_call_indirect`); any path it can't
// prove tail-callable is a hard compile error — the whole point.
// Consequence: this dispatcher's stack frame never grows, even when
// invoked recursively through e.g. nested map-of-map indexing.
//
// On the wasm32 target the import call to `cel_host.cel_map_lookup`
// becomes `return_call $import_index`, observable in the disassembly
// of `cel_runtime.wasm`.  On the host build (no `__wasm__`) the
// attribute degrades to a plain tail call — semantics preserved,
// stack guarantees relaxed (host tests don't depend on stack depth).
#ifdef __wasm__
extern void cel_host_cel_map_lookup(uint32_t out_slot, uint32_t map_slot,
                                    uint32_t key_slot)
    __attribute__((import_module("cel_host"), import_name("cel_map_lookup")));
#else
// Host build: weak no-op stub so C++ unit tests link without the
// wasmtime trampoline.  Tests that exercise the kHost path provide
// a strong override (mirrors the cel_log pattern earlier in this
// file).  Default behaviour is poison-with-type-mismatch — making
// any accidental host invocation visible at the assertion boundary.
// External linkage is required for the weak/strong override across TUs.
__attribute__((weak)) void
cel_host_cel_map_lookup(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t map_slot, uint32_t key_slot) {
  (void)map_slot;
  (void)key_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
#endif

void cel_map_lookup(uint32_t out_slot, uint32_t map_slot, uint32_t key_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  if (m->kind == CEL_UNKNOWN || m->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *m;
    return;
  }
  if (m->kind == CEL_MAP_ARENA) {
    __attribute__((musttail)) return cel_map_lookup_arena(out_slot, map_slot,
                                                          key_slot);
  }
  if (m->kind == CEL_MAP_HOST) {
    __attribute__((musttail)) return cel_host_cel_map_lookup(out_slot, map_slot,
                                                             key_slot);
  }
  // Checker should have rejected; defence in depth.
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

// ---- list runtime --------------------------------------------------------
//
// Three-path dispatch design (`map-list-dispatch.md §4.2 / §6 / §7`):
//   - kArena   : `cel_list_at_arena`        (pure wasm, called directly)
//   - kHost    : `cel_host.cel_list_at`     (host trampoline, called direct)
//   - kDynamic : `cel_list_at`              (this dispatcher, tail-calls)
//
// All arena lists — literal AND comprehension-accumulator — share
// one constructor `cel_list_create(out, capacity)` and one writer
// `cel_list_append_at(out, elem)`.  Literals: codegen knows the
// element count statically, passes it as capacity, then emits N
// appends in index order; final count == capacity.  Comprehension
// accus: codegen loads `iter_range.count` at runtime, passes it
// as capacity (collection-producing macros are bounded above by
// source size, see m5-comprehensions-followon.md §10.A); appends
// run per-iter; final count ≤ capacity.  No growth path — the
// capacity is sufficient by construction, and the append's
// `count >= capacity` invariant traps via `__builtin_trap` if
// codegen ever drops the pre-size.

static ArenaListHeader* arena_list_header(const CelValue* l) {
  return (ArenaListHeader*)(cel_memory_base_() +
                            l->payload.arena_list.header_ptr);
}

static CelValue* arena_list_element(ArenaListHeader* hdr, uint32_t i) {
  return (CelValue*)(cel_memory_base_() + hdr->elements_offset +
                     ((size_t)kCelListEntryStride * i));
}

void cel_list_create(uint32_t out_slot, uint32_t capacity) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  uint32_t hdr_off = cel_alloc((uint32_t)sizeof(ArenaListHeader));
  if (hdr_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint32_t elements_off = 0;
  if (capacity > 0) {
    elements_off =
        cel_alloc((uint32_t)((size_t)kCelListEntryStride * capacity));
    if (elements_off == 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return;
    }
  }
  ArenaListHeader* hdr = (ArenaListHeader*)(cel_memory_base_() + hdr_off);
  hdr->count = 0;
  hdr->capacity = capacity;
  hdr->elements_offset = elements_off;
  hdr->_pad = 0;
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
}

// Append the value at `value_slot` to the arena list at
// `list_slot`, bumping `hdr->count`.  Universal write primitive
// for arena lists — used by both literal codegen (N appends in
// index order, final count == capacity) and comprehension accu
// codegen (per-iter, final count ≤ capacity).  Capacity is a
// codegen invariant; see `cel_list_create` above and followon
// §10.A.  PRESIZE_INVARIANT: trap if `count >= capacity`.
void cel_list_append_at(uint32_t list_slot, uint32_t value_slot) {
  CEL_LOG("enter");
  CelValue* l = cel_value_at(list_slot);
  if (l->kind != CEL_LIST_ARENA) {
    // Wrong kind or already poisoned — silent no-op so an upstream
    // accu_init / append error sticks all the way to `result`.
    return;
  }
  CelValue* v = cel_value_at(value_slot);
  if (v->kind == CEL_ERROR || v->kind == CEL_UNKNOWN) {
    // Per design §3.2: any error in `loop_step` aborts the
    // comprehension and becomes the result.
    *l = *v;
    return;
  }
  ArenaListHeader* hdr = arena_list_header(l);
  // PRESIZE_INVARIANT: capacity is sized by codegen — N for
  // literals, iter_range.count for accus.  Trap rather than
  // grow / poison so a codegen regression surfaces here.
  if (hdr->count >= hdr->capacity) __builtin_trap();
  *arena_list_element(hdr, hdr->count) = *cel_value_at(value_slot);
  ++hdr->count;
}

// M5.B Slice D — predicate-gated append for `filter(v, p)` /
// conditional-map.  Encapsulates 3VL on the predicate so codegen
// can lower the loop_step into a single call.  Semantics:
//   - list_slot poisoned (non-CEL_LIST_ARENA): silent no-op.
//   - pred CEL_ERROR / CEL_UNKNOWN: propagate into list_slot (the
//     comp's `result = @result` then surfaces it).
//   - pred not CEL_BOOL: poison list with CEL_ERR_TYPE_MISMATCH.
//   - pred CEL_BOOL false: silent no-op (predicate rejected iter).
//   - pred CEL_BOOL true: delegate to cel_list_append_at.
void cel_list_append_at_if_bool(uint32_t list_slot, uint32_t pred_slot,
                                uint32_t value_slot) {
  CEL_LOG("enter");
  CelValue* l = cel_value_at(list_slot);
  if (l->kind != CEL_LIST_ARENA) return;
  CelValue* p = cel_value_at(pred_slot);
  if (p->kind == CEL_ERROR || p->kind == CEL_UNKNOWN) {
    *l = *p;
    return;
  }
  if (p->kind != CEL_BOOL) {
    poison(l, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (p->payload.b == 0) return;
  cel_list_append_at(list_slot, value_slot);
}

// Slice G/H followup — 3VL-aware predicate-gated map insert for
// conditional transformMap / transformMapEntry steps.  Mirrors
// cel_list_append_at_if_bool exactly:
//   - pred ERROR / UNKNOWN: propagate verbatim into the map slot
//     (aborts the comprehension per design §3.2).
//   - pred not CEL_BOOL: poison map with CEL_ERR_TYPE_MISMATCH.
//   - pred false: silent no-op.
//   - pred true: delegate to cel_map_insert_at (which performs
//     its own 3VL on key + value).
// Surfaced by macros2/transformMap/error_filter conformance row:
// `{...}.transformMap(k, v, k=='baz' && 4/v==0, v)` where v=0
// produces a divide-by-zero ERROR predicate that must propagate
// rather than being interpreted as a bool.
void cel_map_insert_at_if_bool(uint32_t map_slot, uint32_t pred_slot,
                               uint32_t key_slot, uint32_t value_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  if (m->kind != CEL_MAP_ARENA) return;
  CelValue* p = cel_value_at(pred_slot);
  if (p->kind == CEL_ERROR || p->kind == CEL_UNKNOWN) {
    *m = *p;
    return;
  }
  if (p->kind != CEL_BOOL) {
    poison(m, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (p->payload.b == 0) return;
  cel_map_insert_at(map_slot, key_slot, value_slot);
}

void cel_list_at_arena(uint32_t out_slot, uint32_t list_slot,
                       uint32_t index_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  CelValue* index = cel_value_at(index_slot);
  if (index->kind == CEL_UNKNOWN || index->kind == CEL_ERROR) {
    *out = *index;
    return;
  }
  CelValue* l = cel_value_at(list_slot);
  if (l->kind == CEL_UNKNOWN || l->kind == CEL_ERROR) {
    *out = *l;
    return;
  }
  // Per langdef §"Indexing": list indices are int only; uint is a
  // checker error and never reaches here, but defend in depth.
  if (index->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* hdr = arena_list_header(l);
  int64_t i = index->payload.i;
  if (i < 0 || (uint64_t)i >= (uint64_t)hdr->count) {
    poison(out, CEL_ERR_INDEX_OUT_OF_BOUNDS);
    return;
  }
  *out = *arena_list_element(hdr, (uint32_t)i);
}

// kDynamic dispatcher — same shape as `cel_map_lookup`.  See the
// musttail commentary on that function; identical toolchain
// constraints apply here.
#ifdef __wasm__
extern void cel_host_cel_list_at(uint32_t out_slot, uint32_t list_slot,
                                 uint32_t index_slot)
    __attribute__((import_module("cel_host"), import_name("cel_list_at")));
#else
__attribute__((
    weak)) void cel_host_cel_list_at(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t list_slot, uint32_t index_slot) {
  (void)list_slot;
  (void)index_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
#endif

void cel_list_at(uint32_t out_slot, uint32_t list_slot, uint32_t index_slot) {
  CEL_LOG("enter");
  CelValue* l = cel_value_at(list_slot);
  if (l->kind == CEL_UNKNOWN || l->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *l;
    return;
  }
  if (l->kind == CEL_LIST_ARENA) {
    __attribute__((musttail)) return cel_list_at_arena(out_slot, list_slot,
                                                       index_slot);
  }
  if (l->kind == CEL_LIST_HOST) {
    __attribute__((musttail)) return cel_host_cel_list_at(out_slot, list_slot,
                                                          index_slot);
  }
  // Checker should have rejected; defence in depth.
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

// =====================================================================
// M5.D step 1 — aggregate-op kArena fast paths (size / in / eq /
// concat for lists, size / in / eq for maps).  No host trip; pure
// wasm.  kHost trampolines + the kDynamic dispatchers land in
// step 2.  See `m5-kcall-comprehensions.md §2.1`.
// =====================================================================

// 3VL absorbers + write_* writers come from cel_internal.h.

// Slice 1.6 — polymorphic element-equality matcher.  Used by
// `cel_list_in_arena` / `cel_map_in_arena` to test whether a
// scalar query equals any element / key under langdef §"Equality"
// semantics.  Differs from a same-kind matcher by routing every
// numeric pair through `numeric_compare_kernel` (defined further
// down in the M5.B step 2 section) — so `1 in [1.0]` returns true,
// matching the conformance corpus' `int_in_doubles` row.
//
// Returns 1 (equal), 0 (unequal-or-different-shape).  Caller has
// already absorbed 3VL on the operands; this matcher does not
// surface ERROR / UNKNOWN.
//
// Nested aggregates (CEL_LIST_*, CEL_MAP_*, CEL_MESSAGE) still
// return 0 here — the arena fast path is correct only for scalar
// element types; codegen gates routing accordingly.
//
// Forward-decls land at the top of the M5.B step 2 section; the
// kernel + predicate live below in this same TU and resolve at
// link-within-translation-unit time.
// Slice 1.6 — polymorphic element-equality matcher.  Routes any
// numeric pair (cross-kind included) through `numeric_compare_kernel`
// (defined in cel_compare.c, visible via cel_internal.h's extern) so
// `1 in [1.0]` / `dyn(3) in [1u, 3u]` return true per langdef
// §"List Membership (in)" + §"Equality" + the conformance corpus'
// `int_in_doubles` / `uint_in_ints` rows.
static int cel_value_eq_polymorphic(const CelValue* a, const CelValue* b) {
  if (is_numeric_kind(a->kind) && is_numeric_kind(b->kind)) {
    return numeric_compare_kernel(a, b) == kCmpEqual;
  }
  if (a->kind == CEL_BYTES && b->kind == CEL_BYTES) {
    return spans_equal(a->payload.s, b->payload.s);
  }
  if (a->kind == CEL_NULL && b->kind == CEL_NULL) {
    return 1;
  }
  // m7b.B — duration / timestamp equality is a 12-byte payload
  // compare on the sign-correlated (seconds, nanos) CelDurTs arm
  // (per the proto Duration / Timestamp text format Probe D pinned).
  // CEL_DURATION and CEL_TIMESTAMP are distinct kinds; cross-kind
  // returns 0 via the kind guards below.
  if (a->kind == CEL_DURATION && b->kind == CEL_DURATION) {
    return a->payload.dur.seconds == b->payload.dur.seconds &&
           a->payload.dur.nanos == b->payload.dur.nanos;
  }
  if (a->kind == CEL_TIMESTAMP && b->kind == CEL_TIMESTAMP) {
    return a->payload.ts.seconds == b->payload.ts.seconds &&
           a->payload.ts.nanos == b->payload.ts.nanos;
  }
  // Bool / string / cross-kind non-numeric fall through to
  // `map_keys_equal` which itself routes numerics polymorphically
  // and returns 0 for kind mismatches.
  return map_keys_equal(a, b);
}

// Pre-Slice-1.6 same-kind matcher.  Retained as a thin alias around
// the polymorphic matcher so other callers (cel_list_eq_arena /
// cel_map_eq_arena value comparison) inherit the polymorphic
// behaviour automatically — list equality is element-wise polymorphic
// per langdef §"Equality" too.  No callsites depend on the old
// "no implicit promotion" behaviour.
// NOLINTNEXTLINE(misc-use-internal-linkage)
int cel_value_eq(const CelValue* a, const CelValue* b) {
  return cel_value_eq_polymorphic(a, b);
}

void cel_list_size_arena(uint32_t out_slot, uint32_t list_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* l = cel_value_at(list_slot);
  if (absorb_3vl_unary(out, l)) return;
  if (l->kind != CEL_LIST_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* hdr = arena_list_header(l);
  write_int(out, (int64_t)hdr->count);
}

void cel_list_in_arena(uint32_t out_slot, uint32_t value_slot,
                       uint32_t list_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(value_slot);
  const CelValue* l = cel_value_at(list_slot);
  if (absorb_3vl_binary(out, v, l)) return;
  if (l->kind != CEL_LIST_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* hdr = arena_list_header(l);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (cel_value_eq(arena_list_element(hdr, i), v)) {
      write_bool(out, 1);
      return;
    }
  }
  write_bool(out, 0);
}

void cel_list_eq_arena(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (a->kind != CEL_LIST_ARENA || b->kind != CEL_LIST_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* ha = arena_list_header(a);
  ArenaListHeader* hb = arena_list_header(b);
  if (ha->count != hb->count) {
    write_bool(out, 0);
    return;
  }
  for (uint32_t i = 0; i < ha->count; ++i) {
    if (!cel_value_eq(arena_list_element(ha, i), arena_list_element(hb, i))) {
      write_bool(out, 0);
      return;
    }
  }
  write_bool(out, 1);
}

// Allocates header + elements arena for a fresh CEL_LIST_ARENA of
// `total` elements.  Returns the header offset, or 0 on OOM (with
// `out` poisoned with `CEL_ERR_OVERFLOW`).  Caller fills in the
// elements run.
static uint32_t alloc_concat_list(CelValue* out, uint32_t total) {
  uint32_t hdr_off = cel_alloc((uint32_t)sizeof(ArenaListHeader));
  if (hdr_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return 0;
  }
  uint32_t elements_off = 0;
  if (total > 0) {
    elements_off = cel_alloc((uint32_t)((size_t)kCelListEntryStride * total));
    if (elements_off == 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return 0;
    }
  }
  ArenaListHeader* hdr = (ArenaListHeader*)(cel_memory_base_() + hdr_off);
  hdr->count = total;
  hdr->capacity = total;
  hdr->elements_offset = elements_off;
  hdr->_pad = 0;
  return hdr_off;
}

static void copy_elements(uint32_t elements_off, uint32_t dst_index,
                          ArenaListHeader* src) {
  for (uint32_t i = 0; i < src->count; ++i) {
    *(CelValue*)(cel_memory_base_() + elements_off +
                 ((size_t)kCelListEntryStride * (dst_index + i))) =
        *arena_list_element(src, i);
  }
}

void cel_list_concat_arena(uint32_t out_slot, uint32_t a_slot,
                           uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (a->kind != CEL_LIST_ARENA || b->kind != CEL_LIST_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* ha = arena_list_header(a);
  ArenaListHeader* hb = arena_list_header(b);
  uint32_t hdr_off = alloc_concat_list(out, ha->count + hb->count);
  if (hdr_off == 0) return;  // OOM, out already poisoned.
  ArenaListHeader* hdr = (ArenaListHeader*)(cel_memory_base_() + hdr_off);
  copy_elements(hdr->elements_offset, 0, ha);
  copy_elements(hdr->elements_offset, ha->count, hb);
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
}

void cel_map_size_arena(uint32_t out_slot, uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* m = cel_value_at(map_slot);
  if (absorb_3vl_unary(out, m)) return;
  if (m->kind != CEL_MAP_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaMapHeader* hdr = arena_map_header(m);
  write_int(out, (int64_t)hdr->count);
}

void cel_map_in_arena(uint32_t out_slot, uint32_t key_slot, uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* k = cel_value_at(key_slot);
  const CelValue* m = cel_value_at(map_slot);
  if (absorb_3vl_binary(out, k, m)) return;
  if (m->kind != CEL_MAP_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaMapHeader* hdr = arena_map_header(m);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (map_keys_equal(arena_map_entry_key(hdr, i), k)) {
      write_bool(out, 1);
      return;
    }
  }
  write_bool(out, 0);
}

// Returns 1 iff entry `i` of `ha` is structurally equal to some
// entry of `hb`.  Caller has confirmed both maps have the same
// entry count, so a missing match means the maps differ.
static int arena_map_entry_matches(ArenaMapHeader* ha, uint32_t i,
                                   ArenaMapHeader* hb) {
  const CelValue* ka = arena_map_entry_key(ha, i);
  const CelValue* va = arena_map_entry_val(ha, i);
  for (uint32_t j = 0; j < hb->count; ++j) {
    if (map_keys_equal(ka, arena_map_entry_key(hb, j))) {
      return cel_value_eq(va, arena_map_entry_val(hb, j));
    }
  }
  return 0;
}

void cel_map_eq_arena(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (a->kind != CEL_MAP_ARENA || b->kind != CEL_MAP_ARENA) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaMapHeader* ha = arena_map_header(a);
  ArenaMapHeader* hb = arena_map_header(b);
  if (ha->count != hb->count) {
    write_bool(out, 0);
    return;
  }
  // Map equality is set-equality on entries (langdef §"Equality":
  // map order is irrelevant).  For each entry in a, find a matching
  // key in b and compare values.
  for (uint32_t i = 0; i < ha->count; ++i) {
    if (!arena_map_entry_matches(ha, i, hb)) {
      write_bool(out, 0);
      return;
    }
  }
  write_bool(out, 1);
}

// =====================================================================
// M5.D step 2 — kDynamic dispatchers + kHost extern decls for
// aggregate ops (size / in / eq / concat for lists; size / in / eq
// for maps) plus the polymorphic `cel_message_eq` host helper.
// Each dispatcher mirrors the `cel_map_lookup` shape at line 434:
// 3VL absorption → branch on operand kind → `__attribute__((musttail))`
// to either an arena fast path (M5.D step 1) or a kHost trampoline.
// The kHost arms link to `cel_host.cel_*` imports on the wasm
// build; the host build supplies weak no-op stubs that poison
// with TYPE_MISMATCH so an accidental host invocation surfaces
// at the assertion boundary.  See `m5-kcall-comprehensions.md §2.1`.
// =====================================================================

#ifdef __wasm__
extern void cel_host_cel_list_size(uint32_t out_slot, uint32_t list_slot)
    __attribute__((import_module("cel_host"), import_name("cel_list_size")));
extern void cel_host_cel_list_in(uint32_t out_slot, uint32_t value_slot,
                                 uint32_t list_slot)
    __attribute__((import_module("cel_host"), import_name("cel_list_in")));
extern void cel_host_cel_list_eq(uint32_t out_slot, uint32_t a_slot,
                                 uint32_t b_slot)
    __attribute__((import_module("cel_host"), import_name("cel_list_eq")));
extern void cel_host_cel_list_concat(uint32_t out_slot, uint32_t a_slot,
                                     uint32_t b_slot)
    __attribute__((import_module("cel_host"), import_name("cel_list_concat")));
extern void cel_host_cel_map_size(uint32_t out_slot, uint32_t map_slot)
    __attribute__((import_module("cel_host"), import_name("cel_map_size")));
extern void cel_host_cel_map_in(uint32_t out_slot, uint32_t key_slot,
                                uint32_t map_slot)
    __attribute__((import_module("cel_host"), import_name("cel_map_in")));
extern void cel_host_cel_map_eq(uint32_t out_slot, uint32_t a_slot,
                                uint32_t b_slot)
    __attribute__((import_module("cel_host"), import_name("cel_map_eq")));
extern void cel_host_cel_message_eq(uint32_t out_slot, uint32_t a_slot,
                                    uint32_t b_slot)
    __attribute__((import_module("cel_host"), import_name("cel_message_eq")));
#else
__attribute__((
    weak)) void cel_host_cel_list_size(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t list_slot) {
  (void)list_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_list_in(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t value_slot, uint32_t list_slot) {
  (void)value_slot;
  (void)list_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_list_eq(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  (void)a_slot;
  (void)b_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_list_concat(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  (void)a_slot;
  (void)b_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_map_size(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t map_slot) {
  (void)map_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_map_in(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t key_slot, uint32_t map_slot) {
  (void)key_slot;
  (void)map_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_map_eq(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  (void)a_slot;
  (void)b_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
__attribute__((weak)) void
cel_host_cel_message_eq(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  (void)a_slot;
  (void)b_slot;
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}
#endif

void cel_list_size(uint32_t out_slot, uint32_t list_slot) {
  CEL_LOG("enter");
  CelValue* l = cel_value_at(list_slot);
  if (l->kind == CEL_UNKNOWN || l->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *l;
    return;
  }
  if (l->kind == CEL_LIST_ARENA) {
    __attribute__((musttail)) return cel_list_size_arena(out_slot, list_slot);
  }
  if (l->kind == CEL_LIST_HOST) {
    __attribute__((musttail)) return cel_host_cel_list_size(out_slot,
                                                            list_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_list_in(uint32_t out_slot, uint32_t value_slot, uint32_t list_slot) {
  CEL_LOG("enter");
  CelValue* v = cel_value_at(value_slot);
  CelValue* l = cel_value_at(list_slot);
  if (v->kind == CEL_UNKNOWN || v->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *v;
    return;
  }
  if (l->kind == CEL_UNKNOWN || l->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *l;
    return;
  }
  if (l->kind == CEL_LIST_ARENA) {
    __attribute__((musttail)) return cel_list_in_arena(out_slot, value_slot,
                                                       list_slot);
  }
  if (l->kind == CEL_LIST_HOST) {
    __attribute__((musttail)) return cel_host_cel_list_in(out_slot, value_slot,
                                                          list_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_list_eq(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* a = cel_value_at(a_slot);
  CelValue* b = cel_value_at(b_slot);
  if (a->kind == CEL_UNKNOWN || a->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *a;
    return;
  }
  if (b->kind == CEL_UNKNOWN || b->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *b;
    return;
  }
  // Both arena → fast path; otherwise host trampoline materialises
  // both sides via the appropriate backing methods.  Cross-origin
  // (one arena, one host) routes to the host trampoline which
  // POISONs with TYPE_MISMATCH for now (M6 follow-up).
  if (a->kind == CEL_LIST_ARENA && b->kind == CEL_LIST_ARENA) {
    __attribute__((musttail)) return cel_list_eq_arena(out_slot, a_slot,
                                                       b_slot);
  }
  if ((a->kind == CEL_LIST_ARENA || a->kind == CEL_LIST_HOST) &&
      (b->kind == CEL_LIST_ARENA || b->kind == CEL_LIST_HOST)) {
    __attribute__((musttail)) return cel_host_cel_list_eq(out_slot, a_slot,
                                                          b_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_list_concat(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* a = cel_value_at(a_slot);
  CelValue* b = cel_value_at(b_slot);
  if (a->kind == CEL_UNKNOWN || a->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *a;
    return;
  }
  if (b->kind == CEL_UNKNOWN || b->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *b;
    return;
  }
  if (a->kind == CEL_LIST_ARENA && b->kind == CEL_LIST_ARENA) {
    __attribute__((musttail)) return cel_list_concat_arena(out_slot, a_slot,
                                                           b_slot);
  }
  // Mixed-origin or both-host: route to host trampoline.  For this
  // slice the host trampoline POISONs with TYPE_MISMATCH on either
  // mixed origins or both-host (full materialisation lands as a
  // follow-up).  Documented in CelListConcatImpl.
  if ((a->kind == CEL_LIST_ARENA || a->kind == CEL_LIST_HOST) &&
      (b->kind == CEL_LIST_ARENA || b->kind == CEL_LIST_HOST)) {
    __attribute__((musttail)) return cel_host_cel_list_concat(out_slot, a_slot,
                                                              b_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_map_size(uint32_t out_slot, uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  if (m->kind == CEL_UNKNOWN || m->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *m;
    return;
  }
  if (m->kind == CEL_MAP_ARENA) {
    __attribute__((musttail)) return cel_map_size_arena(out_slot, map_slot);
  }
  if (m->kind == CEL_MAP_HOST) {
    __attribute__((musttail)) return cel_host_cel_map_size(out_slot, map_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_map_in(uint32_t out_slot, uint32_t key_slot, uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* k = cel_value_at(key_slot);
  CelValue* m = cel_value_at(map_slot);
  if (k->kind == CEL_UNKNOWN || k->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *k;
    return;
  }
  if (m->kind == CEL_UNKNOWN || m->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *m;
    return;
  }
  if (m->kind == CEL_MAP_ARENA) {
    __attribute__((musttail)) return cel_map_in_arena(out_slot, key_slot,
                                                      map_slot);
  }
  if (m->kind == CEL_MAP_HOST) {
    __attribute__((musttail)) return cel_host_cel_map_in(out_slot, key_slot,
                                                         map_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

void cel_map_eq(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  CelValue* a = cel_value_at(a_slot);
  CelValue* b = cel_value_at(b_slot);
  if (a->kind == CEL_UNKNOWN || a->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *a;
    return;
  }
  if (b->kind == CEL_UNKNOWN || b->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *b;
    return;
  }
  if (a->kind == CEL_MAP_ARENA && b->kind == CEL_MAP_ARENA) {
    __attribute__((musttail)) return cel_map_eq_arena(out_slot, a_slot, b_slot);
  }
  if ((a->kind == CEL_MAP_ARENA || a->kind == CEL_MAP_HOST) &&
      (b->kind == CEL_MAP_ARENA || b->kind == CEL_MAP_HOST)) {
    __attribute__((musttail)) return cel_host_cel_map_eq(out_slot, a_slot,
                                                         b_slot);
  }
  poison(cel_value_at(out_slot), CEL_ERR_TYPE_MISMATCH);
}

// =====================================================================
// M5.B Slice E — map-key iteration helpers (Option β; see cel_map.h).
//
// The iterator handle is the arena offset of an 8-byte state struct
// `{ header_ptr, cursor }`.  `cursor` is the 1-based index of the
// "current" entry — i.e. the entry that the most recent `iter_next`
// returned 1 for.  `cursor == 0` is the pre-first state set by
// `iter_init`; key_at / value_at refuse to dereference it.
// =====================================================================

typedef struct {
  uint32_t header_ptr;  // ArenaMapHeader byte offset; 0 iff empty/poisoned.
  uint32_t cursor;      // 1-based current entry; 0 = pre-first.
} ArenaMapIterState;

_Static_assert(sizeof(ArenaMapIterState) == 8,
               "ArenaMapIterState must remain 8 bytes (iter ABI)");

// Resolve the iter-state struct from a handle, or NULL when the handle
// is the 0 sentinel (empty/poisoned map).  Centralising the deref keeps
// every caller's null-check identical.
static ArenaMapIterState* arena_map_iter_state(uint32_t handle) {
  if (handle == 0) return (ArenaMapIterState*)0;
  return (ArenaMapIterState*)(cel_memory_base_() + handle);
}

// Read the header an iterator points at.  Caller has already proven
// `state != NULL` and `state->header_ptr != 0`.
static ArenaMapHeader* iter_header(ArenaMapIterState* state) {
  return (ArenaMapHeader*)(cel_memory_base_() + state->header_ptr);
}

uint32_t cel_map_iter_init(uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  // Poisoned / wrong-kind / host-backed maps: codegen guarantees the
  // checker proved the source is `map(K, V)` (and the M5 envelope
  // gates host-backed map sources out of comprehensions today), but
  // a defensive 0 handle keeps `iter_next` / `key_at` safe.
  if (m->kind != CEL_MAP_ARENA) return 0;
  ArenaMapHeader* hdr = arena_map_header(m);
  // Empty map: skip the state alloc entirely — `iter_next(0)` returns
  // 0 immediately, so the comprehension loop exits without entering
  // the body.  Saves 8 arena bytes per empty-iter and keeps the
  // common `iter_next(handle)` hot path branch-light.
  if (hdr->count == 0) return 0;
  uint32_t state_off = cel_alloc((uint32_t)sizeof(ArenaMapIterState));
  if (state_off == 0) return 0;  // OOM: behave as empty.
  ArenaMapIterState* state =
      (ArenaMapIterState*)(cel_memory_base_() + state_off);
  state->header_ptr = m->payload.arena_map.header_ptr;
  state->cursor = 0;
  return state_off;
}

uint32_t cel_map_iter_next(uint32_t iter_handle) {
  CEL_LOG("enter");
  ArenaMapIterState* state = arena_map_iter_state(iter_handle);
  if (state == (ArenaMapIterState*)0) return 0;
  ArenaMapHeader* hdr = iter_header(state);
  // `cursor` is the 1-based index of the *current* entry.  After the
  // last entry has been exposed (cursor == count), iteration is done
  // and every further call returns 0 without mutating state.
  if (state->cursor >= hdr->count) return 0;
  state->cursor++;
  return 1;
}

// Write the current entry's key/value into `out_slot`.  Routed through
// a shared helper so the key/value variants stay one-line dispatches.
static void copy_iter_entry(uint32_t out_slot, uint32_t iter_handle,
                            int want_value) {
  CelValue* out = cel_value_at(out_slot);
  ArenaMapIterState* state = arena_map_iter_state(iter_handle);
  if (state == (ArenaMapIterState*)0 || state->cursor == 0) {
    // Codegen contract: a read without a preceding `iter_next` that
    // returned 1 is a generator bug.  Stamp an error rather than
    // dereferencing past the entries run; the eval surfaces it as
    // the comprehension's result via 3VL absorption upstream.
    poison(out, CEL_ERR_INDEX_OUT_OF_BOUNDS);
    return;
  }
  ArenaMapHeader* hdr = iter_header(state);
  uint32_t i = state->cursor - 1;
  *out =
      want_value ? *arena_map_entry_val(hdr, i) : *arena_map_entry_key(hdr, i);
}

void cel_map_iter_key_at(uint32_t out_slot, uint32_t iter_handle) {
  CEL_LOG("enter");
  copy_iter_entry(out_slot, iter_handle, /*want_value=*/0);
}

void cel_map_iter_value_at(uint32_t out_slot, uint32_t iter_handle) {
  CEL_LOG("enter");
  copy_iter_entry(out_slot, iter_handle, /*want_value=*/1);
}

// =====================================================================
// M5.B step 2b — polymorphic equality dispatcher.
//
// `cel_equals_at_vv` and `cel_not_equals_at_vv` resolve every cel-cpp
// `equals` / `not_equals` overload at runtime via operand-kind
// switch.  Per langdef §"Equality":
//
//   - Numeric kinds (int / uint / double) compare cross-type by
//     mathematical value via the M5.B step 2 numeric ladder.
//   - Same-kind bool / string / bytes / null use the existing
//     `cel_*_eq_at_vv` helpers.
//   - Aggregate kinds (list / map) tail-call the M5.D step 2
//     dispatcher (`cel_list_eq` / `cel_map_eq`), which handles
//     arena vs host origin internally.
//   - CEL_MESSAGE values delegate to the kHost
//     `cel_host_cel_message_eq` import (M5.D step 2).
//   - Mismatched kinds return `false` per langdef
//     ("comparing incompatible types is not an error"), with one
//     exception: numeric ↔ non-numeric is also `false`.
//   - 3VL: any UNKNOWN / ERROR operand short-circuits via
//     `absorb_3vl_binary`.
//
// `not_equals` is `equals` with the result flipped; share a kernel
// to keep the dispatch ladder authoritative in one place.
//
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/equality_functions.cc
// =====================================================================

// 1 if `kind` is one of the three numeric kinds: int, uint, double.
// Lifted to a dedicated helper so the polymorphic dispatcher's
// branch table reads as a single conjunction per ladder rung.
static int is_numeric(uint32_t kind) {
  return kind == CEL_INT || kind == CEL_UINT || kind == CEL_DOUBLE;
}

// 1 if both operands are list-shaped (arena or host).  Mixed-origin
// pairs route through `cel_list_eq` which absorbs the origin
// difference via its dispatcher.
static int both_lists(uint32_t ka, uint32_t kb) {
  return (ka == CEL_LIST_ARENA || ka == CEL_LIST_HOST) &&
         (kb == CEL_LIST_ARENA || kb == CEL_LIST_HOST);
}

static int both_maps(uint32_t ka, uint32_t kb) {
  return (ka == CEL_MAP_ARENA || ka == CEL_MAP_HOST) &&
         (kb == CEL_MAP_ARENA || kb == CEL_MAP_HOST);
}

// `cel_string_eq_at_vv` / `cel_bytes_eq_at_vv` live further down in
// this file and are reachable from the equality kernel above; their
// declarations come in transitively via cel_runtime.h →
// cel_string_ops.h (readability-redundant-declaration).

// Polymorphic equality kernel.  Writes a CEL_BOOL into `out_slot`,
// or propagates 3VL.  Aggregate / message arms tail-call into their
// dispatchers, which write CEL_BOOL themselves; `cel_not_equals`
// re-reads `out_slot` after a tail-call'd helper returns and flips.
// M9.D: CEL_TYPE × CEL_TYPE equality — memcmp on `payload.s` bytes,
// the type-name string in linear memory.  Extracted from
// `equality_kernel` to keep that function under the function-size
// gate; per langdef §"Equality" + m9-type-subsystem.md §3.4.
static void type_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  const uint32_t la = a->payload.s.len;
  const uint32_t lb = b->payload.s.len;
  if (la != lb) {
    write_bool(out, 0);
    return;
  }
  const uint8_t* base = cel_memory_base_();
  int eq = 1;
  for (uint32_t i = 0; i < la; ++i) {
    if (base[a->payload.s.ptr + i] != base[b->payload.s.ptr + i]) {
      eq = 0;
      break;
    }
  }
  write_bool(out, eq);
}

static void equality_kernel(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  // Numeric ladder — cross-type allowed.
  if (is_numeric(a->kind) && is_numeric(b->kind)) {
    cel_numeric_eq_at_vv(out_slot, a_slot, b_slot);
    return;
  }
  // Same-kind scalar arms.  Each handles its own type-check; we've
  // already proven kinds match.
  if (a->kind == b->kind) {
    switch (a->kind) {
      case CEL_NULL:
        cel_null_eq_at_vv(out_slot, a_slot, b_slot);
        return;
      case CEL_BOOL:
        cel_bool_eq_at_vv(out_slot, a_slot, b_slot);
        return;
      case CEL_STRING:
        cel_string_eq_at_vv(out_slot, a_slot, b_slot);
        return;
      case CEL_BYTES:
        cel_bytes_eq_at_vv(out_slot, a_slot, b_slot);
        return;
      case CEL_TYPE:
        type_eq_at_vv(out_slot, a_slot, b_slot);
        return;
      case CEL_DURATION:
      case CEL_TIMESTAMP:
        // m7b.B: 12-byte payload compare on the sign-correlated
        // CelDurTs arm — equivalent to comparing the absl::Duration
        // values themselves once both are normalised.  `dur` and
        // `ts` are the same union arm; reading either reads the
        // same CelDurTs bytes.
        write_bool(out, a->payload.dur.seconds == b->payload.dur.seconds &&
                            a->payload.dur.nanos == b->payload.dur.nanos);
        return;
      default:
        break;  // Aggregates fall through to the polymorphic arms.
    }
  }
  // Aggregates: dispatcher absorbs origin difference.
  if (both_lists(a->kind, b->kind)) {
    cel_list_eq(out_slot, a_slot, b_slot);
    return;
  }
  if (both_maps(a->kind, b->kind)) {
    cel_map_eq(out_slot, a_slot, b_slot);
    return;
  }
  if (a->kind == CEL_MESSAGE && b->kind == CEL_MESSAGE) {
    cel_host_cel_message_eq(out_slot, a_slot, b_slot);
    return;
  }
  // Cross-kind without a matching ladder rung: `false` per langdef
  // (NOT type-mismatch error).  E.g. `1 == "1"`, `[] == {}`,
  // `null == 0`.
  write_bool(out, 0);
}

// Wasm-exported via `-Wl,--export=cel_equals_at_vv` in
// compiler_v2/runtime/BUILD.bazel; `misc-use-internal-linkage` is
// silenced because internal linkage would hide it from the wasm
// export table.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void cel_equals_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  equality_kernel(out_slot, a_slot, b_slot);
}

// Wasm-exported via `-Wl,--export=cel_not_equals_at_vv` — see
// `cel_equals_at_vv` above for the linkage rationale.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void cel_not_equals_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  equality_kernel(out_slot, a_slot, b_slot);
  // 3VL absorption already wrote the operand through if either was
  // UNKNOWN / ERROR — leave that as-is.  Otherwise flip the bool.
  CelValue* out = cel_value_at(out_slot);
  if (out->kind == CEL_BOOL) {
    out->payload.b = out->payload.b ? 0 : 1;
  }
}
