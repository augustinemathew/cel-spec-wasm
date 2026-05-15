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

// Tri-state numeric comparison result, hoisted up here so
// `map_keys_equal` (Slice 1.6) and `cel_value_eq_polymorphic` can
// consult `numeric_compare_kernel`.  The kernel + per-pair
// comparators (`cmp_i64` etc.) live in the M5.B step 2 section
// below; only the typedef + signature need to be visible here.
typedef enum {
  kCmpLess = 0,
  kCmpEqual = 1,
  kCmpGreater = 2,
  kCmpNanInequal = 3,
} CmpResult;

static CmpResult numeric_compare_kernel(const CelValue* a, const CelValue* b);
static int is_numeric_kind(uint32_t kind);

// Slice 1.6: numeric-key map equality consults the polymorphic
// `numeric_compare_kernel` — handles every {int, uint, double}
// pair, including double-typed *queries* against int/uint keys.
// `double` is NOT a valid map-key kind (`is_valid_map_key_kind`
// rejects it on `cel_map_insert`), but it CAN appear on the
// query side: `1.0 in {1: "a"}` is allowed.  `numeric_keys_equal`
// already handled the int↔uint cross-kind case via mathematical
// comparison; the polymorphic kernel adds double↔int / double↔uint.

static int map_keys_equal(const CelValue* a, const CelValue* b) {
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
// List literals construct via `cel_list_create(out, count)` followed
// by `cel_list_set(out, i, elem)` for each i in [0, count); both
// only ever produce CEL_LIST_ARENA values — kHost values originate
// from proto reflection (REPEATED fields) or `Activation::Bind`,
// never from emitted codegen.  The list shape is fixed-length —
// codegen knows the element count up front, so there is no growth
// path (no `cel_list_grow` / `cel_list_append`).

static ArenaListHeader* arena_list_header(const CelValue* l) {
  return (ArenaListHeader*)(cel_memory_base_() +
                            l->payload.arena_list.header_ptr);
}

static CelValue* arena_list_element(ArenaListHeader* hdr, uint32_t i) {
  return (CelValue*)(cel_memory_base_() + hdr->elements_offset +
                     ((size_t)kCelListEntryStride * i));
}

void cel_list_create(uint32_t out_slot, uint32_t count) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  uint32_t hdr_off = cel_alloc((uint32_t)sizeof(ArenaListHeader));
  if (hdr_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint32_t elements_off = 0;
  if (count > 0) {
    elements_off = cel_alloc((uint32_t)((size_t)kCelListEntryStride * count));
    if (elements_off == 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return;
    }
    // cel_alloc zero-fills, leaving every CelValue with kind=CEL_NULL
    // (CEL_NULL == 0 by the enum order), so an unset element reads as
    // null rather than a garbage kind.  Any codegen-correct emit
    // overwrites every slot via cel_list_set before the list is used.
  }
  ArenaListHeader* hdr = (ArenaListHeader*)(cel_memory_base_() + hdr_off);
  hdr->count = count;
  hdr->capacity = count;
  hdr->elements_offset = elements_off;
  hdr->_pad = 0;
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = hdr_off;
}

void cel_list_set(uint32_t list_slot, uint32_t index, uint32_t elem_slot) {
  CEL_LOG("enter");
  CelValue* l = cel_value_at(list_slot);
  // If the list is already poisoned, every subsequent set is a no-op
  // — error sticks.
  if (l->kind != CEL_LIST_ARENA) {
    return;
  }
  ArenaListHeader* hdr = arena_list_header(l);
  // List literals are fixed-length — codegen knows `index < count`
  // because both are compile-time constants.  An out-of-range index
  // means codegen drifted out of sync with the runtime; poison
  // defensively so the bug surfaces at the first observable boundary
  // instead of silently scribbling past the elements arena.
  if (index >= hdr->count) {
    poison(l, CEL_ERR_OVERFLOW);
    return;
  }
  *arena_list_element(hdr, index) = *cel_value_at(elem_slot);
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
static int cel_value_eq_polymorphic(const CelValue* a, const CelValue* b);

// Pre-Slice-1.6 same-kind matcher.  Retained as a thin alias around
// the polymorphic matcher so other callers (cel_list_eq_arena /
// cel_map_eq_arena value comparison) inherit the polymorphic
// behaviour automatically — list equality is element-wise polymorphic
// per langdef §"Equality" too.  No callsites depend on the old
// "no implicit promotion" behaviour.
static int cel_value_eq(const CelValue* a, const CelValue* b) {
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
// M5.B — arithmetic + comparison helpers (slot-out helper ABI).
// Each helper's signature is `(out_slot, arg_slots...) -> void`; the
// caller already knows the result lives at `out_slot`, so there's no
// return value.  Cel-cpp parity citations point at
// `third_party/cel-cpp/runtime/standard/{arithmetic,equality,
// comparison}_functions.cc`.  See `m5-kcall-comprehensions.md §2.1`
// for the design rationale and the WAT traces 16-17 for the wire
// shape.
// =====================================================================

// 3VL absorption / write_* / require_kinds / poison shared inlines
// live in cel_internal.h (included at the top of this TU).

// Manual u64 multiply-overflow detection via split 32×32→64
// partial products.  We avoid `__builtin_mul_overflow` for 64-bit
// operands because clang lowers it through `__multi3` (a 128-bit
// multiply from compiler-rt) — and the wasm32 freestanding build
// doesn't link compiler-rt by design.  We avoid the divide-by-b
// bounds-check shape too: clang's optimiser recognises it and
// re-folds it back into a `__multi3` call.  Splitting into
// 32-bit halves means every multiply here is a 32×32→64 op the
// wasm32 backend lowers natively as `i64.mul`, with no high-half
// reasoning the optimiser can lift to 128-bit math.
//
// Logic:
//   a*b = (ah*2^32 + al) * (bh*2^32 + bl)
//       = al*bl + (ah*bl + al*bh)*2^32 + ah*bh*2^64
//   Overflow iff (ah * bh) != 0
//                OR (ah*bl + al*bh) overflows 32 bits
//                OR adding the shifted middle to al*bl overflows.
static int uint64_mul_overflows(uint64_t a, uint64_t b, uint64_t* r) {
  uint64_t ah = a >> 32;
  uint64_t al = a & 0xFFFFFFFFULL;
  uint64_t bh = b >> 32;
  uint64_t bl = b & 0xFFFFFFFFULL;
  if (ah != 0 && bh != 0) return 1;
  uint64_t mid = (ah * bl) + (al * bh);  // operands ≤32 bits, sum may carry
  if ((mid >> 32) != 0) return 1;
  uint64_t lo = al * bl;
  uint64_t result = lo + (mid << 32);
  if (result < lo) return 1;  // unsigned add overflow → product > UINT64_MAX
  *r = result;
  return 0;
}

// Signed int64 mul overflow on top of the unsigned check: take
// magnitudes, run the unsigned check, then validate the signed
// range against INT64_MIN / INT64_MAX based on operand signs.
static int int64_mul_overflows(int64_t a, int64_t b, int64_t* r) {
  if (a == 0 || b == 0) {
    *r = 0;
    return 0;
  }
  // INT64_MIN handled specially: |INT64_MIN| > INT64_MAX, so the
  // standard magnitude trick can't represent it.  Only INT64_MIN*1
  // and INT64_MIN*0 don't overflow; we already handled 0 above.
  if (a == INT64_MIN) return b != 1 ? 1 : (*r = INT64_MIN, 0);
  if (b == INT64_MIN) return a != 1 ? 1 : (*r = INT64_MIN, 0);
  uint64_t ua = (uint64_t)(a < 0 ? -a : a);
  uint64_t ub = (uint64_t)(b < 0 ? -b : b);
  uint64_t up;
  if (uint64_mul_overflows(ua, ub, &up)) return 1;
  // Signs determine whether result is +/-.  Range:
  //   positive: [0, INT64_MAX]
  //   negative: [INT64_MIN, 0]   (so |result| ≤ -(INT64_MIN+1)+1 = INT64_MAX+1)
  int negative = (a < 0) ^ (b < 0);
  if (negative) {
    if (up > (uint64_t)INT64_MAX + 1ULL) return 1;
    *r = -(int64_t)up;
  } else {
    if (up > (uint64_t)INT64_MAX) return 1;
    *r = (int64_t)up;
  }
  return 0;
}

// ---- int64 arithmetic ----------------------------------------------------
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/arithmetic_functions.cc::
//     add_int64 / sub_int64 / mul_int64 / div_int64 / mod_int64 /
//     negate_int64

void cel_int_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  int64_t r;
  if (__builtin_add_overflow(a->payload.i, b->payload.i, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, r);
}

void cel_int_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  int64_t r;
  if (__builtin_sub_overflow(a->payload.i, b->payload.i, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, r);
}

void cel_int_mul_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  int64_t r;
  if (int64_mul_overflows(a->payload.i, b->payload.i, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, r);
}

void cel_int_div_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  if (b->payload.i == 0) {
    poison(out, CEL_ERR_DIVIDE_BY_ZERO);
    return;
  }
  // INT64_MIN / -1 overflows in two's complement.
  if (a->payload.i == INT64_MIN && b->payload.i == -1) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, a->payload.i / b->payload.i);
}

void cel_int_mod_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_INT)) return;
  if (b->payload.i == 0) {
    poison(out, CEL_ERR_MODULUS_BY_ZERO);
    return;
  }
  // INT64_MIN % -1 is undefined behaviour in C; cel-cpp returns 0
  // for this case explicitly (the mathematically-correct result).
  if (a->payload.i == INT64_MIN && b->payload.i == -1) {
    write_int(out, 0);
    return;
  }
  write_int(out, a->payload.i % b->payload.i);
}

void cel_int_neg_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_INT) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  if (v->payload.i == INT64_MIN) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_int(out, -v->payload.i);
}

// ---- uint64 arithmetic ---------------------------------------------------

void cel_uint_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  uint64_t r;
  if (__builtin_add_overflow(a->payload.u, b->payload.u, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, r);
}

void cel_uint_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  uint64_t r;
  if (__builtin_sub_overflow(a->payload.u, b->payload.u, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, r);
}

void cel_uint_mul_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  uint64_t r;
  if (uint64_mul_overflows(a->payload.u, b->payload.u, &r)) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_uint(out, r);
}

void cel_uint_div_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  if (b->payload.u == 0) {
    poison(out, CEL_ERR_DIVIDE_BY_ZERO);
    return;
  }
  write_uint(out, a->payload.u / b->payload.u);
}

void cel_uint_mod_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_UINT)) return;
  if (b->payload.u == 0) {
    poison(out, CEL_ERR_MODULUS_BY_ZERO);
    return;
  }
  write_uint(out, a->payload.u % b->payload.u);
}

// ---- double arithmetic ---------------------------------------------------
// Per langdef §"Numeric values": double follows IEEE 754.  No
// overflow / div-by-zero errors — inf/nan results are valid.

void cel_double_add_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d + b->payload.d);
}

void cel_double_sub_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d - b->payload.d);
}

void cel_double_mul_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d * b->payload.d);
}

void cel_double_div_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_DOUBLE)) return;
  write_double(out, a->payload.d / b->payload.d);
}

void cel_double_neg_at_v(uint32_t out_slot, uint32_t v_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != CEL_DOUBLE) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_double(out, -v->payload.d);
}

// ---- comparison helpers --------------------------------------------------
// Same-kind only — cross-type numeric ladder lives in a separate
// `cel_numeric_*` set added with the kCall arm's ladder dispatch
// (M5.B step 2).  Each helper writes a CEL_BOOL CelValue.
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/equality_functions.cc
//   third_party/cel-cpp/runtime/standard/comparison_functions.cc

#define DEFINE_CMP_VV(name, kind, field, op)                       \
  void name(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) { \
    CelValue* out = cel_value_at(out_slot);                        \
    const CelValue* a = cel_value_at(a_slot);                      \
    const CelValue* b = cel_value_at(b_slot);                      \
    if (absorb_3vl_binary(out, a, b)) return;                      \
    if (require_kinds(out, a, b, kind)) return;                    \
    write_bool(out, a->payload.field op b->payload.field);         \
  }

DEFINE_CMP_VV(cel_int_eq_at_vv, CEL_INT, i, ==)
DEFINE_CMP_VV(cel_int_ne_at_vv, CEL_INT, i, !=)
DEFINE_CMP_VV(cel_int_lt_at_vv, CEL_INT, i, <)
DEFINE_CMP_VV(cel_int_le_at_vv, CEL_INT, i, <=)
DEFINE_CMP_VV(cel_int_gt_at_vv, CEL_INT, i, >)
DEFINE_CMP_VV(cel_int_ge_at_vv, CEL_INT, i, >=)

DEFINE_CMP_VV(cel_uint_eq_at_vv, CEL_UINT, u, ==)
DEFINE_CMP_VV(cel_uint_ne_at_vv, CEL_UINT, u, !=)
DEFINE_CMP_VV(cel_uint_lt_at_vv, CEL_UINT, u, <)
DEFINE_CMP_VV(cel_uint_le_at_vv, CEL_UINT, u, <=)
DEFINE_CMP_VV(cel_uint_gt_at_vv, CEL_UINT, u, >)
DEFINE_CMP_VV(cel_uint_ge_at_vv, CEL_UINT, u, >=)

// Double: `==` / `!=` follow IEEE 754 (NaN != NaN, NaN == NaN
// false).  C's `==` and `!=` operators implement this directly.
// Ordering operators (<, <=, >, >=) likewise return false for any
// NaN-bearing comparison per IEEE.
DEFINE_CMP_VV(cel_double_eq_at_vv, CEL_DOUBLE, d, ==)
DEFINE_CMP_VV(cel_double_ne_at_vv, CEL_DOUBLE, d, !=)
DEFINE_CMP_VV(cel_double_lt_at_vv, CEL_DOUBLE, d, <)
DEFINE_CMP_VV(cel_double_le_at_vv, CEL_DOUBLE, d, <=)
DEFINE_CMP_VV(cel_double_gt_at_vv, CEL_DOUBLE, d, >)
DEFINE_CMP_VV(cel_double_ge_at_vv, CEL_DOUBLE, d, >=)

DEFINE_CMP_VV(cel_bool_eq_at_vv, CEL_BOOL, b, ==)
DEFINE_CMP_VV(cel_bool_ne_at_vv, CEL_BOOL, b, !=)
// Bool ordering — `false < true` per langdef §"Booleans".  Since
// `payload.b` is normalised to 0/1 by `write_bool` and
// `cel_make_bool`, the integer relational operators give the
// langdef order directly.
DEFINE_CMP_VV(cel_bool_lt_at_vv, CEL_BOOL, b, <)
DEFINE_CMP_VV(cel_bool_le_at_vv, CEL_BOOL, b, <=)
DEFINE_CMP_VV(cel_bool_gt_at_vv, CEL_BOOL, b, >)
DEFINE_CMP_VV(cel_bool_ge_at_vv, CEL_BOOL, b, >=)

#undef DEFINE_CMP_VV

// =====================================================================
// M5.B step 2 — cross-type numeric comparison ladder.
//
// Each helper accepts any combination of {CEL_INT, CEL_UINT,
// CEL_DOUBLE} on either operand.  The shared `numeric_compare_kernel`
// returns a tri-state result {kLess, kEqual, kGreater, kNanInequal}
// mirroring cel-cpp's `internal/number.h::ComparisonResult`.  Each
// op then collapses that tri-state to a CEL_BOOL.
//
// Boundary handling (cel-cpp parity, `internal/number.h` lines
// 25-44 / 95-165):
//   - int vs uint: negative int is always < any uint; otherwise
//     compare as uint64.
//   - int vs double: if double > kInt64Max → double > int; if
//     double < kInt64Min → double < int; otherwise IEEE-compare
//     (double)int vs double.
//   - uint vs double: if double > kUint64Max → double > uint; if
//     double < 0 → double < uint; otherwise IEEE-compare
//     (double)uint vs double.
//   - NaN: any comparison involving NaN returns kNanInequal so all
//     six op wrappers answer false (matches IEEE / langdef "NaN
//     compares unequal in every direction").
//
// Wasm32 freestanding constraint: this code MUST avoid `__multi3` /
// other compiler-rt 128-bit intrinsics (mirrors the M5.B step 1
// `int64_mul_overflows` precedent).  Only operations used here are
// 64-bit comparisons + a single int↔uint cast that the wasm32 backend
// lowers natively as `i64.lt_s` / `i64.lt_u` / `f64.lt`.  The
// `noinline` attribute on the kernel keeps clang from re-deriving a
// 128-bit fold across the leaf wrappers.
//
// `CmpResult` typedef + the kernel forward decl + `is_numeric_kind`
// were hoisted up to the M5.D-step-1 section of this file at Slice
// 1.6 so `map_keys_equal` / `cel_value_eq_polymorphic` (the
// element-equality matchers used by `_in_` membership) can call
// the kernel.  The full kernel body lands here unchanged.

// Per-pair comparators.  Each takes raw operands of one of three
// scalar shapes and returns the tri-state result.  Pulled out of
// the kernel both to keep each function under the lint size gate
// and to keep the rationale beside the boundary checks they encode
// (cel-cpp parity, `internal/number.h:25-165`).

static CmpResult cmp_i64(int64_t a, int64_t b) {
  if (a < b) return kCmpLess;
  if (a > b) return kCmpGreater;
  return kCmpEqual;
}

static CmpResult cmp_u64(uint64_t a, uint64_t b) {
  if (a < b) return kCmpLess;
  if (a > b) return kCmpGreater;
  return kCmpEqual;
}

static CmpResult cmp_double(double a, double b) {
  // IEEE: any NaN comparison is unordered.
  if (a != a || b != b) return kCmpNanInequal;
  if (a < b) return kCmpLess;
  if (a > b) return kCmpGreater;
  return kCmpEqual;
}

// int vs uint: negative int is always less than any uint;
// otherwise compare via uint64 cast.
static CmpResult cmp_int_vs_uint(int64_t a, uint64_t b) {
  if (a < 0) return kCmpLess;
  return cmp_u64((uint64_t)a, b);
}

// int vs double: boundary check before any narrowing cast.  See
// `third_party/cel-cpp/internal/number.h:127-143`.
static CmpResult cmp_int_vs_double(int64_t a, double b) {
  if (b != b) return kCmpNanInequal;
  if (b > (double)INT64_MAX) return kCmpLess;
  if (b < (double)INT64_MIN) return kCmpGreater;
  return cmp_double((double)a, b);
}

// uint vs double: any negative double is less than every uint;
// any double > UINT64_MAX is greater than every uint.
static CmpResult cmp_uint_vs_double(uint64_t a, double b) {
  if (b != b) return kCmpNanInequal;
  if (b > (double)UINT64_MAX) return kCmpLess;
  if (b < 0.0) return kCmpGreater;
  return cmp_double((double)a, b);
}

// Flip a tri-state result for the swapped-operand convention.  Used
// when we have a comparator for `a vs b` and need `b vs a`.
static CmpResult cmp_flip(CmpResult r) {
  if (r == kCmpLess) return kCmpGreater;
  if (r == kCmpGreater) return kCmpLess;
  return r;
}

// Pack the two operand kinds into a small dense key so the kernel
// dispatch is a single switch.  Caller has already verified both
// operands are numeric.
static uint32_t numeric_kind_pair(uint32_t a_kind, uint32_t b_kind) {
  return (a_kind << 8) | b_kind;
}

// Boundary constants — same byte-for-byte as
// `third_party/cel-cpp/internal/number.h:25-44`.  Inline rather than
// macros so each helper sees the type the wasm32 backend can type-
// check.
static __attribute__((noinline)) CmpResult
numeric_compare_kernel(const CelValue* a, const CelValue* b) {
  switch (numeric_kind_pair(a->kind, b->kind)) {
    case (CEL_INT << 8) | CEL_INT:
      return cmp_i64(a->payload.i, b->payload.i);
    case (CEL_UINT << 8) | CEL_UINT:
      return cmp_u64(a->payload.u, b->payload.u);
    case (CEL_DOUBLE << 8) | CEL_DOUBLE:
      return cmp_double(a->payload.d, b->payload.d);
    case (CEL_INT << 8) | CEL_UINT:
      return cmp_int_vs_uint(a->payload.i, b->payload.u);
    case (CEL_UINT << 8) | CEL_INT:
      return cmp_flip(cmp_int_vs_uint(b->payload.i, a->payload.u));
    case (CEL_INT << 8) | CEL_DOUBLE:
      return cmp_int_vs_double(a->payload.i, b->payload.d);
    case (CEL_DOUBLE << 8) | CEL_INT:
      return cmp_flip(cmp_int_vs_double(b->payload.i, a->payload.d));
    case (CEL_UINT << 8) | CEL_DOUBLE:
      return cmp_uint_vs_double(a->payload.u, b->payload.d);
    case (CEL_DOUBLE << 8) | CEL_UINT:
      return cmp_flip(cmp_uint_vs_double(b->payload.u, a->payload.d));
    default:
      // Caller (`numeric_prelude`) already filters non-numeric kinds;
      // a non-numeric pair reaching the kernel is an invariant
      // violation.  Return kNanInequal so all six op wrappers answer
      // false rather than miscompiling silently.
      return kCmpNanInequal;
  }
}

// True iff both operand kinds are numeric (int / uint / double).
static int is_numeric_kind(uint32_t kind) {
  return kind == CEL_INT || kind == CEL_UINT || kind == CEL_DOUBLE;
}

// Slice 1.6 — definition of the polymorphic element-equality matcher
// forward-declared at the top of the M5.D section.  Routes any
// numeric pair (cross-kind included) through `numeric_compare_kernel`
// so `1 in [1.0]` / `dyn(3) in [1u, 3u]` return true per langdef
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
  // Bool / string / cross-kind non-numeric fall through to
  // `map_keys_equal` which itself routes numerics polymorphically
  // (Slice 1.6 update below) and returns 0 for kind mismatches.
  return map_keys_equal(a, b);
}

// Shared prelude for the cross-type numeric helpers: 3VL absorption
// then a numeric-kind check on each operand (any non-numeric → type
// mismatch).  Returns 1 when out_slot has been written and the
// caller should skip the kernel.
static int numeric_prelude(CelValue* out, const CelValue* a,
                           const CelValue* b) {
  if (absorb_3vl_binary(out, a, b)) return 1;
  if (!is_numeric_kind(a->kind) || !is_numeric_kind(b->kind)) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return 1;
  }
  return 0;
}

void cel_numeric_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpEqual);
}

void cel_numeric_ne_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  // NaN-touching inequality returns TRUE — matches cel-cpp's
  // `Inequal<double>` default
  // (`runtime/standard/equality_functions.cc:78`), which is the
  // IEEE `lhs != rhs` semantic where `NaN != NaN` is true.  An
  // earlier version of this kernel returned false on
  // `kCmpNanInequal`; the prior comment claiming langdef
  // mandated false was a spec misread.  Slice 1.55 (2026-04-25)
  // flipped to `r != kCmpEqual` so every non-equal tri-state
  // (less / greater / nan-inequal) yields true.
  write_bool(out, r != kCmpEqual);
}

void cel_numeric_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpLess);
}

void cel_numeric_le_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpLess || r == kCmpEqual);
}

void cel_numeric_gt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpGreater);
}

void cel_numeric_ge_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (numeric_prelude(out, a, b)) return;
  CmpResult r = numeric_compare_kernel(a, b);
  write_bool(out, r == kCmpGreater || r == kCmpEqual);
}

void cel_null_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (require_kinds(out, a, b, CEL_NULL)) return;
  write_bool(out, 1);  // null == null is always true.
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

// =====================================================================
// M5.C — string + bytes operation helpers (slot-out helper ABI).
// Concat is the only allocator: writes a fresh payload into the
// arena.  Other helpers read operand spans without allocating.
// 3VL + type-mismatch envelope mirrors `cel_arith.h` / `cel_compare.h`.
// cel-cpp parity:
//   third_party/cel-cpp/runtime/standard/string_functions.cc
// =====================================================================

// Span equality byte-for-byte.  CEL strings are UTF-8 byte arrays
// at the langdef level; equality is byte equality (no Unicode
// normalisation).  Same shape works for bytes operands.
static int span_eq(const CelSpan* a, const CelSpan* b) {
  if (a->len != b->len) return 0;
  const uint8_t* pa = cel_memory_base_() + a->ptr;
  const uint8_t* pb = cel_memory_base_() + b->ptr;
  for (uint32_t i = 0; i < a->len; ++i) {
    if (pa[i] != pb[i]) return 0;
  }
  return 1;
}

// Span lexicographic <.  Per langdef §"String / bytes": compare
// by Unicode code-point order, but since strings are stored as
// UTF-8 byte sequences, byte-lex order matches code-point order.
// cel-cpp's `LessThan::String` operates on byte-views identically.
static int span_lt(const CelSpan* a, const CelSpan* b) {
  uint32_t n = a->len < b->len ? a->len : b->len;
  const uint8_t* pa = cel_memory_base_() + a->ptr;
  const uint8_t* pb = cel_memory_base_() + b->ptr;
  for (uint32_t i = 0; i < n; ++i) {
    if (pa[i] != pb[i]) return pa[i] < pb[i];
  }
  return a->len < b->len;
}

// Returns 1 iff `hay[off..off+sub.len)` matches `sub` byte-for-byte.
// `off` MUST satisfy `off + sub.len <= hay.len` — caller checks.
static int span_match_at(const CelSpan* hay, uint32_t off, const CelSpan* sub) {
  const uint8_t* ph = cel_memory_base_() + hay->ptr + off;
  const uint8_t* ps = cel_memory_base_() + sub->ptr;
  for (uint32_t i = 0; i < sub->len; ++i) {
    if (ph[i] != ps[i]) return 0;
  }
  return 1;
}

// Linear-scan substring search.  langdef pins string ops to byte
// granularity; we don't need a fancy search algorithm because
// CEL fixtures are short.  Mirrors cel-cpp's
// `StringContains::Apply` (also a linear scan).
static int span_contains(const CelSpan* hay, const CelSpan* sub) {
  if (sub->len == 0) return 1;
  if (sub->len > hay->len) return 0;
  uint32_t last = hay->len - sub->len;
  for (uint32_t i = 0; i <= last; ++i) {
    if (span_match_at(hay, i, sub)) return 1;
  }
  return 0;
}

// Wrap up the per-helper string/bytes header check + 3VL + alloc
// in one place.  `kind` is CEL_STRING or CEL_BYTES.  Returns 1
// (skip math) when out_slot has been written with an absorbed
// 3VL value or a type-mismatch error.
static int span_op_prelude(CelValue* out, const CelValue* a, const CelValue* b,
                           uint32_t kind) {
  if (absorb_3vl_binary(out, a, b)) return 1;
  if (require_kinds(out, a, b, kind)) return 1;
  return 0;
}

static void concat_into_out(CelValue* out, const CelSpan* a, const CelSpan* b,
                            uint32_t kind) {
  uint32_t total = a->len + b->len;
  uint32_t off = 0;
  if (total > 0) {
    off = cel_alloc(total);
    if (off == 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return;
    }
    uint8_t* dst = cel_memory_base_() + off;
    memcpy(dst, cel_memory_base_() + a->ptr, a->len);
    memcpy(dst + a->len, cel_memory_base_() + b->ptr, b->len);
  }
  out->kind = kind;
  out->payload.s.ptr = off;
  out->payload.s.len = total;
}

void cel_string_concat_at_vv(uint32_t out_slot, uint32_t a_slot,
                             uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_STRING)) return;
  concat_into_out(out, &a->payload.s, &b->payload.s, CEL_STRING);
}

void cel_bytes_concat_at_vv(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_BYTES)) return;
  concat_into_out(out, &a->payload.s, &b->payload.s, CEL_BYTES);
}

static void size_at(CelValue* out, uint32_t v_slot, uint32_t kind) {
  const CelValue* v = cel_value_at(v_slot);
  if (absorb_3vl_unary(out, v)) return;
  if (v->kind != kind) {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  write_int(out, (int64_t)v->payload.s.len);
}

void cel_string_size_at_v(uint32_t out_slot, uint32_t v_slot) {
  size_at(cel_value_at(out_slot), v_slot, CEL_STRING);
}

void cel_bytes_size_at_v(uint32_t out_slot, uint32_t v_slot) {
  size_at(cel_value_at(out_slot), v_slot, CEL_BYTES);
}

void cel_string_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_STRING)) return;
  write_bool(out, span_eq(&a->payload.s, &b->payload.s));
}

void cel_string_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_STRING)) return;
  write_bool(out, span_lt(&a->payload.s, &b->payload.s));
}

void cel_bytes_eq_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_BYTES)) return;
  write_bool(out, span_eq(&a->payload.s, &b->payload.s));
}

void cel_bytes_lt_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_BYTES)) return;
  write_bool(out, span_lt(&a->payload.s, &b->payload.s));
}

// String / bytes le/gt/ge — express each in terms of the existing
// `span_lt` byte-lex comparator.  Identities (cel-cpp parity,
// `runtime/standard/comparison_functions.cc`):
//   a <= b  ↔  !(b <  a)
//   a >  b  ↔   (b <  a)
//   a >= b  ↔  !(a <  b)
#define DEFINE_SPAN_CMP_VV(name, kind, body)                       \
  void name(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) { \
    CelValue* out = cel_value_at(out_slot);                        \
    const CelValue* a = cel_value_at(a_slot);                      \
    const CelValue* b = cel_value_at(b_slot);                      \
    if (span_op_prelude(out, a, b, kind)) return;                  \
    write_bool(out, body);                                         \
  }

DEFINE_SPAN_CMP_VV(cel_string_le_at_vv, CEL_STRING,
                   !span_lt(&b->payload.s, &a->payload.s))
DEFINE_SPAN_CMP_VV(cel_string_gt_at_vv, CEL_STRING,
                   span_lt(&b->payload.s, &a->payload.s))
DEFINE_SPAN_CMP_VV(cel_string_ge_at_vv, CEL_STRING,
                   !span_lt(&a->payload.s, &b->payload.s))
DEFINE_SPAN_CMP_VV(cel_bytes_le_at_vv, CEL_BYTES,
                   !span_lt(&b->payload.s, &a->payload.s))
DEFINE_SPAN_CMP_VV(cel_bytes_gt_at_vv, CEL_BYTES,
                   span_lt(&b->payload.s, &a->payload.s))
DEFINE_SPAN_CMP_VV(cel_bytes_ge_at_vv, CEL_BYTES,
                   !span_lt(&a->payload.s, &b->payload.s))

#undef DEFINE_SPAN_CMP_VV

void cel_string_contains_at_vv(uint32_t out_slot, uint32_t s_slot,
                               uint32_t sub_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* sub = cel_value_at(sub_slot);
  if (span_op_prelude(out, s, sub, CEL_STRING)) return;
  write_bool(out, span_contains(&s->payload.s, &sub->payload.s));
}

void cel_string_starts_with_at_vv(uint32_t out_slot, uint32_t s_slot,
                                  uint32_t pfx_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* pfx = cel_value_at(pfx_slot);
  if (span_op_prelude(out, s, pfx, CEL_STRING)) return;
  if (pfx->payload.s.len > s->payload.s.len) {
    write_bool(out, 0);
    return;
  }
  write_bool(out, span_match_at(&s->payload.s, 0, &pfx->payload.s));
}

void cel_string_ends_with_at_vv(uint32_t out_slot, uint32_t s_slot,
                                uint32_t sfx_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* s = cel_value_at(s_slot);
  const CelValue* sfx = cel_value_at(sfx_slot);
  if (span_op_prelude(out, s, sfx, CEL_STRING)) return;
  if (sfx->payload.s.len > s->payload.s.len) {
    write_bool(out, 0);
    return;
  }
  uint32_t off = s->payload.s.len - sfx->payload.s.len;
  write_bool(out, span_match_at(&s->payload.s, off, &sfx->payload.s));
}
