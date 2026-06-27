#include "runtime/cel_runtime.h"

#include "runtime/cel_internal.h"
#include "runtime/cel_net_ext.h"
#include "runtime/cel_optional.h"

// ---- map runtime ---------------------------------------------------------
//
// Three-path dispatch by map origin (see `map-list-dispatch.md`):
//   - kArena   : `cel_map_lookup_arena`    (pure wasm, called directly)
//   - kHost    : `cel_host.cel_map_lookup` (host trampoline, called direct)
//   - kDynamic : `cel_map_lookup`          (this dispatcher, tail-calls)
//
// Map literals construct via `cel_map_create` + `cel_map_insert`; both
// only ever produce CEL_MAP_ARENA values — kHost values originate from
// proto reflection or `Activation::Bind`, never from emitted codegen.
//
// `CmpResult`, `numeric_compare_kernel`, `is_numeric_kind`, and
// `cel_value_eq` come from cel_internal.h (defined in cel_compare.c).

static int is_valid_map_key_kind(uint32_t kind) {
  return kind == CEL_BOOL || kind == CEL_INT || kind == CEL_UINT ||
         kind == CEL_STRING;
}

// A double lookup key with magnitude ≥ 2^53 must bypass the hash index
// and linear-scan instead.  Under the lossy `cel_value_eq` map-key
// comparison, such a double is map-equal to a *range* of int64s that
// all round to it, but a single hash token can only land it on one slot
// — so the index could falsely miss a key the linear scan would hit.
// Keeping these (rare) keys on the linear path keeps the index in exact
// parity with the scan.  See m32-swisstable-map-index.md §5.1.
static int key_forces_linear(const CelValue* key) {
  if (key->kind != CEL_DOUBLE) return 0;
  const double d = key->payload.d;
  // 2^53 == 9007199254740992.0.  `-d >= 2^53` covers the negative side
  // without <math.h>; NaN compares false on both, which is correct (a
  // NaN key never matches a stored int/uint key under cel_value_eq).
  return d >= 9007199254740992.0 || -d >= 9007199254740992.0;
}

static ArenaMapHeader* arena_map_header(const CelValue* m) {
  return (ArenaMapHeader*)(cel_memory_base_() +
                           m->payload.arena_map.header_ptr);
}

// Overflow-guarded byte size for `count` entries of `stride` bytes.
// On wasm32 `size_t` is 32 bits, so the unguarded
// `(uint32_t)((size_t)stride * count)` form WRAPS for adversarial
// counts and under-allocates — later entry writes then scribble past
// the run.  Returns 0 (never a valid size for count > 0) when the
// product exceeds what `arena_alloc` can represent; callers poison
// CEL_ERR_OVERFLOW on 0, exactly as they do for an arena_alloc OOM.
static uint32_t entries_bytes_checked(uint32_t stride, uint32_t count) {
  const uint64_t bytes = (uint64_t)stride * (uint64_t)count;
  // arena_alloc rejects requests above UINT32_MAX-7 (8-byte align
  // headroom); mirror that ceiling so the reject happens here with
  // the multiply, not as a confusing downstream alloc failure.
  if (bytes > (uint64_t)(UINT32_MAX - 7u)) return 0;
  return (uint32_t)bytes;
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
  uint32_t hdr_off = arena_alloc((uint32_t)sizeof(ArenaMapHeader));
  if (hdr_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint32_t entries_off = 0;
  if (initial_capacity > 0) {
    const uint32_t bytes =
        entries_bytes_checked((uint32_t)kCelMapEntryStride, initial_capacity);
    entries_off = bytes == 0 ? 0 : arena_alloc(bytes);
    if (entries_off == 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return;
    }
  }
  ArenaMapHeader* hdr = (ArenaMapHeader*)(cel_memory_base_() + hdr_off);
  hdr->count = 0;
  hdr->capacity = initial_capacity;
  hdr->entries_offset = entries_off;
  hdr->index_offset = 0;  // built later by cel_map_index_build (or never).
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
  // 3VL: an errored / unknown key or value poisons the whole literal
  // (map construction is strict, matching list literals via
  // `cel_list_append_at` and cel-cpp's create-map step) — checked
  // BEFORE the key-kind gate so `{1/0: 'a'}` surfaces divide_by_zero,
  // not type_mismatch.
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
    if (cel_value_eq(arena_map_entry_key(hdr, i), key)) {
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

// Insert for `transformMap` / `transformMapEntry` comprehension
// accumulators.  Differs from `cel_map_insert` in two ways:
//   1. Key-collision OVERWRITES the existing entry's value
//      (last-write-wins, matching cel-cpp's transformMap runtime
//      behaviour).
//   2. CEL_ERROR / CEL_UNKNOWN in either key OR value propagates
//      the error verbatim into the map slot; subsequent inserts
//      are silent no-ops (error sticks).
// Like `cel_map_insert`, capacity is a codegen pre-size invariant —
// there is no growth path; exceeding it traps (PRESIZE_INVARIANT).
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
    if (cel_value_eq(arena_map_entry_key(hdr, i), key)) {
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
  // Hash-index fast path: usable only when an index is built AND the key
  // does not force a linear scan (§5.1).  cel_map_index_find returns
  // UINT32_MAX when index_offset == 0, so the fallback is the same scan.
  if (hdr->index_offset != 0 && !key_forces_linear(key)) {
    const uint32_t idx = cel_map_index_find(hdr, key);
    if (idx != UINT32_MAX) {
      *out = *arena_map_entry_val(hdr, idx);
      return;
    }
    poison(out, CEL_ERR_NO_SUCH_KEY);
    return;
  }
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (cel_value_eq(arena_map_entry_key(hdr, i), key)) {
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
// Three-path dispatch by list origin (see `map-list-dispatch.md`):
//   - kArena   : `cel_list_at_arena`        (pure wasm, called directly)
//   - kHost    : `cel_host.cel_list_at`     (host trampoline, called direct)
//   - kDynamic : `cel_list_at`              (this dispatcher, tail-calls)
//
// All arena lists — literal AND comprehension-accumulator — share
// one constructor `cel_list_create(out, capacity)` and one writer
// `cel_list_append_at(out, elem)`.  Literals: codegen knows the
// element count statically, passes it as capacity, then emits N
// appends in index order; final count == capacity.  Comprehension
// accus: codegen loads `iter_range.count` at runtime, passes it as
// capacity (collection-producing macros are bounded above by source
// size); appends run per-iter; final count ≤ capacity.  No growth
// path — the capacity is sufficient by construction, and the append's
// `count >= capacity` invariant traps via `__builtin_trap` if codegen
// ever drops the pre-size.

// Host trampoline: snapshots a CEL_LIST_HOST source into an
// arena-allocated ArenaListHeader + N×24-byte elements run, then
// writes a synthetic CelValue at `out_slot` of shape
// `{kind: CEL_LIST_ARENA, payload.arena_list.header_ptr = ...}`.
// On empty / OOM, writes a synthetic empty arena list.  Lets the
// existing inline arena prologue walk host lists unchanged.  Same
// iter-snapshot pattern as `cel_host.cel_map_iter_open`.
#ifdef __wasm__
extern void cel_host_cel_list_iter_open(uint32_t out_slot, uint32_t list_slot)
    __attribute__((import_module("cel_host"),
                   import_name("cel_list_iter_open")));
#else
// Host build: weak no-op stub for native unit tests.  Strong
// override lives in cel_host_wasmtime.cc.
__attribute__((weak)) void
cel_host_cel_list_iter_open(  // NOLINT(misc-use-internal-linkage)
    uint32_t out_slot, uint32_t list_slot) {
  (void)list_slot;
  CelValue* out = (CelValue*)(cel_memory_base_() + out_slot);
  // Empty arena list — empty walk, no comprehension body runs.
  out->kind = CEL_LIST_ARENA;
  out->payload.arena_list.header_ptr = 0;
}
#endif

static ArenaListHeader* arena_list_header(const CelValue* l) {
  return (ArenaListHeader*)(cel_memory_base_() +
                            l->payload.arena_list.header_ptr);
}

// ---- poisoned comprehension sources ---------------------------------
//
// The comprehension prologue walks its list/map source through raw
// header loads (no kind check — see `EmitListPrologue` /
// `cel_map_iter_init`).  Two source shapes can't take that walk:
//
//   - a poisoned source (the iter_range evaluated to CEL_ERROR /
//     CEL_UNKNOWN, e.g. `[1/0].map(x, x)`), and
//   - an OOM while materialising the walkable view (snapshot slot /
//     iter state allocation failed).
//
// Both used to degrade to an EMPTY walk — a silent wrong answer
// (`[1/0].exists(x, x == 2)` returned `false` instead of the divide-
// by-zero error).  Instead we vend a synthetic ONE-element view whose
// element (or key+value pair) carries the poison CelValue: the loop
// body's 3VL absorption then propagates it into the accu, so the
// comprehension result IS the error/unknown — matching cel-cpp, where
// an errored iter_range errors the comprehension.
//
// Backing bytes come from a fresh arena allocation when available;
// when the arena itself is exhausted, from the per-Instance emergency
// block `arena_oom_block()` reserved at arena_init (see cel_arena.h).
// The emergency block is shared by every OOM vend in an eval — fine,
// because its contents are rewritten at each vend and only ever read
// as ERROR/UNKNOWN elements.  Byte layout of the 128-byte block:
//
//   +0   ArenaListHeader (16)   } list view
//   +16  element CelValue (24)  }
//   +40  view CelValue (24)     }
//   +64  MapIterState (16)      } map iter view
//   +80  key CelValue (24)      } (HOST-shaped snapshot entry:
//   +104 value CelValue (24)    }  key at +0, value at +24)
enum {
  kOomBlockListHeaderOff = 0,
  kOomBlockListElemOff = 16,
  kOomBlockListViewOff = 40,
  kOomBlockMapIterStateOff = 64,
  kOomBlockMapEntryOff = 80,
};

// Vend a CEL_LIST_ARENA view of one element `*poison`.  Returns the
// byte offset of the view CelValue.  Traps only when the emergency
// block was never reserved (arena_init-time malloc failure) — there
// is no honest verdict to give in that state.
static uint32_t vend_poison_list_view(const CelValue* poison) {
  uint32_t hdr_off = arena_alloc(64u);
  if (hdr_off == 0) {
    if (arena_oom_block() == 0) __builtin_trap();
    hdr_off = arena_oom_block() + (uint32_t)kOomBlockListHeaderOff;
  }
  const uint32_t elem_off = hdr_off + (uint32_t)kOomBlockListElemOff;
  const uint32_t view_off = hdr_off + (uint32_t)kOomBlockListViewOff;
  ArenaListHeader* hdr = (ArenaListHeader*)(cel_memory_base_() + hdr_off);
  hdr->count = 1;
  hdr->capacity = 1;
  hdr->elements_offset = elem_off;
  hdr->_pad = 0;
  *cv_at(elem_off) = *poison;
  CelValue* view = cv_at(view_off);
  view->kind = CEL_LIST_ARENA;
  view->payload.arena_list.header_ptr = hdr_off;
  return view_off;
}

// Resolve a list iter_range source to a slot offset whose CelValue
// is arena-shaped (CEL_LIST_ARENA).  Arena sources pass through;
// host sources are snapshotted via `cel_host.cel_list_iter_open`
// into a fresh arena allocation and a fresh slot returned.  Lets
// codegen emit the same inline arena prologue regardless of origin.
//
// Never returns a non-walkable slot: poisoned sources and OOM both
// vend a one-poison-element view (see the block comment above).
// cel:codegen-export
uint32_t cel_list_arena_view(uint32_t list_slot) {
  CEL_LOG("enter");
  CelValue* l = cel_value_at(list_slot);
  if (l->kind == CEL_LIST_ARENA) {
    return list_slot;
  }
  if (l->kind == CEL_LIST_HOST) {
    uint32_t synth_slot = arena_alloc((uint32_t)sizeof(CelValue));
    if (synth_slot == 0) {
      CelValue oom;
      poison(&oom, CEL_ERR_OVERFLOW);
      return vend_poison_list_view(&oom);
    }
    cel_host_cel_list_iter_open(synth_slot, list_slot);
    return synth_slot;
  }
  if (l->kind == CEL_ERROR || l->kind == CEL_UNKNOWN) {
    // Poisoned source — propagate it verbatim through the walk.
    return vend_poison_list_view(l);
  }
  // Wrong kind: the checker guarantees a list-typed iter_range, so
  // this is codegen/runtime drift.  Vend a TYPE_MISMATCH element so
  // the drift surfaces as an eval error instead of a garbage walk.
  CelValue mismatch;
  poison(&mismatch, CEL_ERR_TYPE_MISMATCH);
  return vend_poison_list_view(&mismatch);
}

static CelValue* arena_list_element(ArenaListHeader* hdr, uint32_t i) {
  return (CelValue*)(cel_memory_base_() + hdr->elements_offset +
                     ((size_t)kCelListEntryStride * i));
}

void cel_list_create(uint32_t out_slot, uint32_t capacity) {
  CEL_LOG("enter");
  CelValue* out = cel_value_at(out_slot);
  uint32_t hdr_off = arena_alloc((uint32_t)sizeof(ArenaListHeader));
  if (hdr_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  uint32_t elements_off = 0;
  if (capacity > 0) {
    const uint32_t bytes =
        entries_bytes_checked((uint32_t)kCelListEntryStride, capacity);
    elements_off = bytes == 0 ? 0 : arena_alloc(bytes);
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
// codegen invariant; see `cel_list_create` above.
// PRESIZE_INVARIANT: trap if `count >= capacity`.
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
    // Any error in a comprehension's loop_step aborts the
    // comprehension and becomes its result.
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

// Predicate-gated append for `filter(v, p)` /
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

// 3VL-aware predicate-gated map insert for conditional
// transformMap / transformMapEntry steps.  Mirrors
// cel_list_append_at_if_bool exactly:
//   - pred ERROR / UNKNOWN: propagate verbatim into the map slot
//     (aborts the comprehension).
//   - pred not CEL_BOOL: poison map with CEL_ERR_TYPE_MISMATCH.
//   - pred false: silent no-op.
//   - pred true: delegate to cel_map_insert_at (which performs
//     its own 3VL on key + value).
// The ERROR-predicate arm is what makes
// `{...}.transformMap(k, v, k=='baz' && 4/v==0, v)` (v==0 yields a
// divide-by-zero predicate) propagate the error instead of coercing
// it to a bool.
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
  // Per langdef §"Indexing": list index type is int.  When the
  // operand is dyn-typed (e.g. `[1,2,3][dyn(0.0)]`), cel-cpp's
  // runtime admits a CEL_UINT or an integral CEL_DOUBLE as an
  // index — the value must round-trip to an int64.  Non-integral
  // doubles error.  Pinned by oracle tests `ListIndexDoubleAgrees`,
  // `ListIndexUintAgrees`, `ListIndexNonIntegerDoubleAgrees`
  // (testdata/cel_cpp_oracle_test.cc) and conformance rows
  // `lists/index/zero_based_double`, `zero_based_uint`,
  // `zero_based_double_error`.
  int64_t i = 0;
  if (index->kind == CEL_INT) {
    i = index->payload.i;
  } else if (index->kind == CEL_UINT) {
    if (index->payload.u > (uint64_t)INT64_MAX) {
      poison(out, CEL_ERR_INDEX_OUT_OF_BOUNDS);
      return;
    }
    i = (int64_t)index->payload.u;
  } else if (index->kind == CEL_DOUBLE) {
    const double d = index->payload.d;
    // Integral check: must be finite, within int64 range, and
    // bit-equal to its int64 truncation.  cel-cpp's
    // ConvertDoubleToInt does the same range/integrality check.
    if (d != d || d > 9.2233720368547758e18 ||
        d < -9.2233720368547758e18) {  // NaN / inf / OOR
      poison(out, CEL_ERR_INVALID_ARGUMENT);
      return;
    }
    const int64_t trunc = (int64_t)d;
    if ((double)trunc != d) {
      // non-integral
      poison(out, CEL_ERR_INVALID_ARGUMENT);
      return;
    }
    i = trunc;
  } else {
    poison(out, CEL_ERR_TYPE_MISMATCH);
    return;
  }
  ArenaListHeader* hdr = arena_list_header(l);
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
// Aggregate-op kArena fast paths (size / in / eq / concat for
// lists, size / in / eq for maps).  No host trip; pure wasm.
// kHost trampolines + kDynamic dispatchers live below in this
// TU.  See `rewrite/map-list-dispatch.md` §2 for the three-path
// origin dispatch contract.
// =====================================================================

// 3VL absorbers + write_* writers come from cel_internal.h.

// Single-layer scalar equality matcher.  Any numeric pair (cross-kind
// included) routes through `numeric_compare_kernel` (defined in
// cel_compare.c, visible via cel_internal.h's extern) so `1 in [1.0]`
// / `dyn(3) in [1u, 3u]` return true per langdef §"List Membership
// (in)" + §"Equality".  The common scan-element kinds (numeric,
// string, bool) come first so a homogeneous `in`-list / map scan hits
// its arm with the fewest failed branches.
//
// Returns 1 (equal), 0 (unequal-or-different-shape).  Caller has
// already absorbed 3VL on the operands; this matcher does not
// surface ERROR / UNKNOWN.
//
// SCALARS ONLY: nested aggregates (CEL_LIST_*, CEL_MAP_*),
// CEL_MESSAGE, CEL_TYPE, and CEL_OPTIONAL return 0 here.  Aggregate
// walks must route such pairs through `deep_values_equal` below,
// which recurses into the polymorphic equality kernel (and from
// there into the cel_host trampolines for message / host-origin
// pairs).  Comparing them here would silently report `false` for
// equal values.
//
// Other TUs reach this via the `cel_value_eq` extern in
// cel_internal.h; the non-static definition is required for that.
// NOLINTNEXTLINE(misc-use-internal-linkage)
int cel_value_eq(const CelValue* a, const CelValue* b) {
  if (is_numeric_kind(a->kind) && is_numeric_kind(b->kind)) {
    return numeric_compare_kernel(a, b) == kCmpEqual;
  }
  if (a->kind == CEL_STRING && b->kind == CEL_STRING) {
    return spans_equal(a->payload.s, b->payload.s);
  }
  if (a->kind == CEL_BOOL && b->kind == CEL_BOOL) {
    return (a->payload.b != 0) == (b->payload.b != 0);
  }
  if (a->kind == CEL_BYTES && b->kind == CEL_BYTES) {
    return spans_equal(a->payload.s, b->payload.s);
  }
  if (a->kind == CEL_NULL && b->kind == CEL_NULL) {
    return 1;
  }
  // Duration / timestamp equality compares the (seconds, nanos) pair.
  // CEL_DURATION and CEL_TIMESTAMP are distinct kinds; cross-kind
  // returns 0 via the kind guards.
  if (a->kind == CEL_DURATION && b->kind == CEL_DURATION) {
    return a->payload.dur.seconds == b->payload.dur.seconds &&
           a->payload.dur.nanos == b->payload.dur.nanos;
  }
  if (a->kind == CEL_TIMESTAMP && b->kind == CEL_TIMESTAMP) {
    return a->payload.ts.seconds == b->payload.ts.seconds &&
           a->payload.ts.nanos == b->payload.ts.nanos;
  }
  if (a->kind == CEL_IP && b->kind == CEL_IP) {
    return net_ip_eq(a, b);
  }
  if (a->kind == CEL_CIDR && b->kind == CEL_CIDR) {
    return net_cidr_eq(a, b);
  }
  return 0;
}

// Polymorphic equality kernel — defined with the equality dispatcher
// section below; forward-declared here so the aggregate fast paths
// can recurse through it for non-scalar element pairs.
static void equality_kernel(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot);

// 1 if `kind` is fully comparable by `cel_value_eq` (the scalar
// matcher).  The complement — CEL_MESSAGE, CEL_LIST_*, CEL_MAP_*,
// CEL_TYPE, CEL_OPTIONAL, plus ERROR/UNKNOWN defensively — must go
// through the equality kernel.
static int is_scalar_eq_kind(uint32_t kind) {
  return kind == CEL_NULL || kind == CEL_BOOL || kind == CEL_INT ||
         kind == CEL_UINT || kind == CEL_DOUBLE || kind == CEL_STRING ||
         kind == CEL_BYTES || kind == CEL_DURATION || kind == CEL_TIMESTAMP ||
         kind == CEL_IP || kind == CEL_CIDR;
}

// Deep structural equality of the CelValues at byte offsets `a_off`
// and `b_off`.  Scalar pairs short-circuit through `cel_value_eq`;
// anything else recurses through the polymorphic equality kernel,
// which dispatches nested lists/maps back into the aggregate walks
// and routes CEL_MESSAGE / host-origin pairs through the cel_host
// trampolines — so `[Msg{x:1}] == [Msg{x:1}]` compares with the same
// MessageDifferencer semantics as a direct `msg1 == msg2`.
//
// The kernel's verdict lands in a lazily-allocated arena scratch cell
// (`*scratch`, 0 until first needed); a fresh cell — never the
// caller's out_slot — because workspace slot reuse can alias out_slot
// with an operand slot that later iterations still read.  A non-bool
// verdict (e.g. a not-comparable message pair, which the trampoline
// reports as a TYPE_MISMATCH error) compares UNEQUAL, matching the
// host-side walk's contract (eval/internal/cel_host.cc
// `ListEqElementEquals`).
//
// Returns 1 (equal), 0 (unequal-or-uncomparable), -1 (scratch
// allocation failed — caller must poison CEL_ERR_OVERFLOW, never
// degrade to a false verdict).
static int deep_values_equal(uint32_t* scratch, uint32_t a_off,
                             uint32_t b_off) {
  const CelValue* a = cv_at(a_off);
  const CelValue* b = cv_at(b_off);
  if (is_scalar_eq_kind(a->kind) && is_scalar_eq_kind(b->kind)) {
    return cel_value_eq(a, b);
  }
  if (*scratch == 0) {
    *scratch = arena_alloc((uint32_t)sizeof(CelValue));
    if (*scratch == 0) return -1;
  }
  equality_kernel(*scratch, a_off, b_off);
  const CelValue* verdict = cv_at(*scratch);
  return verdict->kind == CEL_BOOL && verdict->payload.b != 0;
}

// Byte offset of element `i` in an arena list's elements run.
static uint32_t arena_list_element_off(const ArenaListHeader* hdr, uint32_t i) {
  return hdr->elements_offset + (uint32_t)((size_t)kCelListEntryStride * i);
}

// Byte offsets of entry `i`'s key / value in an arena map's run.
static uint32_t arena_map_entry_key_off(const ArenaMapHeader* hdr, uint32_t i) {
  return hdr->entries_offset + (uint32_t)((size_t)kCelMapEntryStride * i);
}
static uint32_t arena_map_entry_val_off(const ArenaMapHeader* hdr, uint32_t i) {
  return arena_map_entry_key_off(hdr, i) + (uint32_t)sizeof(CelValue);
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

// Type-specialized `in`-list scans.  The needle's kind is decided once
// by the caller (`cel_list_in_arena`), so each per-element body is a
// single kind guard + payload compare — no re-dispatch through the
// `deep_values_equal` → `cel_value_eq` layers every element would
// otherwise pay.  Numeric scan keeps cross-kind
// promotion (`1 in [1.0, 2u]` is true per langdef §"Equality") by
// routing every numeric element through `numeric_compare_kernel`;
// non-matching-kind elements are simply skipped (a scalar is never
// equal to a different-typed value), which preserves the generic
// path's unequal verdict.  Return 1 if found, 0 otherwise.
static int arena_list_scan_numeric(const ArenaListHeader* hdr,
                                   const CelValue* needle) {
  // Same-kind elements — the overwhelmingly common homogeneous-list
  // shape — compare by a single inlinable payload test; only genuinely
  // cross-kind numeric elements (a uint/double in an int-needle scan,
  // etc.) fall to the out-of-line `numeric_compare_kernel`, which owns
  // the cross-type promotion ladder.  INT and UINT share the 64-bit
  // payload, so a bitwise `payload.u ==` decides equality for both;
  // DOUBLE must use IEEE `==` (NaN != NaN — a NaN is never `in` a
  // list; ±0.0 compare equal — matching `cmp_double`'s verdict).
  const uint32_t nk = needle->kind;
  for (uint32_t i = 0; i < hdr->count; ++i) {
    const CelValue* e = cv_at(arena_list_element_off(hdr, i));
    if (e->kind == nk) {
      const int eq = (nk == CEL_DOUBLE) ? (e->payload.d == needle->payload.d)
                                        : (e->payload.u == needle->payload.u);
      if (eq) return 1;
    } else if (is_numeric_kind(e->kind) &&
               numeric_compare_kernel(e, needle) == kCmpEqual) {
      return 1;
    }
  }
  return 0;
}

static int arena_list_scan_string(const ArenaListHeader* hdr,
                                  const CelValue* needle) {
  for (uint32_t i = 0; i < hdr->count; ++i) {
    const CelValue* e = cv_at(arena_list_element_off(hdr, i));
    if (e->kind == CEL_STRING && spans_equal(e->payload.s, needle->payload.s)) {
      return 1;
    }
  }
  return 0;
}

static int arena_list_scan_bool(const ArenaListHeader* hdr,
                                const CelValue* needle) {
  const int want = needle->payload.b != 0;
  for (uint32_t i = 0; i < hdr->count; ++i) {
    const CelValue* e = cv_at(arena_list_element_off(hdr, i));
    if (e->kind == CEL_BOOL && (e->payload.b != 0) == want) {
      return 1;
    }
  }
  return 0;
}

// Generic deep-equality scan for needle kinds the typed scans don't
// cover (bytes, temporal, net, null, or a non-scalar).  Routes every
// element through `deep_values_equal`, which recurses into nested
// aggregates and allocates a scratch cell for the equality kernel.
// Returns 1 (found), 0 (not found), -1 (scratch alloc failed — caller
// must poison CEL_ERR_OVERFLOW, never degrade to a false verdict).
static int arena_list_scan_generic(const ArenaListHeader* hdr,
                                   uint32_t value_slot) {
  uint32_t scratch = 0;
  for (uint32_t i = 0; i < hdr->count; ++i) {
    const int eq =
        deep_values_equal(&scratch, arena_list_element_off(hdr, i), value_slot);
    if (eq < 0) return -1;
    if (eq) return 1;
  }
  return 0;
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

  // Decide the scan once by needle kind.  The hot homogeneous needles
  // (numeric / string / bool) take a tight typed scan; every other
  // needle (bytes, temporal, net, null, or a non-scalar) falls to the
  // generic deep-equality scan, which handles cross-kind aggregate
  // recursion and the scratch allocation its kernel needs.
  if (is_numeric_kind(v->kind)) {
    write_bool(out, arena_list_scan_numeric(hdr, v));
    return;
  }
  if (v->kind == CEL_STRING) {
    write_bool(out, arena_list_scan_string(hdr, v));
    return;
  }
  if (v->kind == CEL_BOOL) {
    write_bool(out, arena_list_scan_bool(hdr, v));
    return;
  }
  const int found = arena_list_scan_generic(hdr, value_slot);
  if (found < 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
  write_bool(out, found);
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
  uint32_t scratch = 0;
  for (uint32_t i = 0; i < ha->count; ++i) {
    const int eq = deep_values_equal(&scratch, arena_list_element_off(ha, i),
                                     arena_list_element_off(hb, i));
    if (eq < 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return;
    }
    if (!eq) {
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
  uint32_t hdr_off = arena_alloc((uint32_t)sizeof(ArenaListHeader));
  if (hdr_off == 0) {
    poison(out, CEL_ERR_OVERFLOW);
    return 0;
  }
  uint32_t elements_off = 0;
  if (total > 0) {
    const uint32_t bytes =
        entries_bytes_checked((uint32_t)kCelListEntryStride, total);
    elements_off = bytes == 0 ? 0 : arena_alloc(bytes);
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
  // The u32 element-count add can wrap for adversarial counts; the
  // wrapped total would under-allocate and `copy_elements` would
  // scribble past the run.  Reject → poison, never wrap.
  if (hb->count > UINT32_MAX - ha->count) {
    poison(out, CEL_ERR_OVERFLOW);
    return;
  }
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
  // Hash-index fast path, mirroring cel_map_lookup_arena: usable only
  // when an index is built and the key does not force linear (§5.1).
  if (hdr->index_offset != 0 && !key_forces_linear(k)) {
    write_bool(out, cel_map_index_find(hdr, k) != UINT32_MAX ? 1 : 0);
    return;
  }
  for (uint32_t i = 0; i < hdr->count; ++i) {
    if (cel_value_eq(arena_map_entry_key(hdr, i), k)) {
      write_bool(out, 1);
      return;
    }
  }
  write_bool(out, 0);
}

// Returns 1 iff entry `i` of `ha` is structurally equal to some
// entry of `hb`, 0 if not, -1 on scratch-allocation failure inside
// the deep value compare.  Caller has confirmed both maps have the
// same entry count, so a missing match means the maps differ.  Keys
// are scalar by construction (`is_valid_map_key_kind`); VALUES can be
// messages or nested aggregates, so they go through the deep
// comparator.
static int arena_map_entry_matches(ArenaMapHeader* ha, uint32_t i,
                                   ArenaMapHeader* hb, uint32_t* scratch) {
  const CelValue* ka = arena_map_entry_key(ha, i);
  // Stored keys are always scalar (is_valid_map_key_kind: bool/int/uint/
  // string), never a double, so `key_forces_linear` never trips here and
  // the index on `hb` — when built — turns this O(n) inner scan into
  // O(1).  cel_map_index_find returns UINT32_MAX when hb has no index,
  // so the linear arm below still covers the unindexed case.
  if (hb->index_offset != 0) {
    const uint32_t j = cel_map_index_find(hb, ka);
    if (j == UINT32_MAX) return 0;
    return deep_values_equal(scratch, arena_map_entry_val_off(ha, i),
                             arena_map_entry_val_off(hb, j));
  }
  for (uint32_t j = 0; j < hb->count; ++j) {
    if (cel_value_eq(ka, arena_map_entry_key(hb, j))) {
      return deep_values_equal(scratch, arena_map_entry_val_off(ha, i),
                               arena_map_entry_val_off(hb, j));
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
  uint32_t scratch = 0;
  for (uint32_t i = 0; i < ha->count; ++i) {
    const int eq = arena_map_entry_matches(ha, i, hb, &scratch);
    if (eq < 0) {
      poison(out, CEL_ERR_OVERFLOW);
      return;
    }
    if (!eq) {
      write_bool(out, 0);
      return;
    }
  }
  write_bool(out, 1);
}

// =====================================================================
// kDynamic dispatchers + kHost extern decls for aggregate ops
// (size / in / eq / concat for lists; size / in / eq for maps),
// plus the polymorphic `cel_message_eq` host helper.  Each
// dispatcher mirrors the `cel_map_lookup` shape above: 3VL
// absorption → branch on operand kind → `__attribute__((musttail))`
// to either an arena fast path or a kHost trampoline.  The kHost
// arms link to `cel_host.cel_*` imports on the wasm build; the
// host build supplies weak no-op stubs that poison with
// TYPE_MISMATCH so an accidental host invocation surfaces at the
// assertion boundary.  See `rewrite/map-list-dispatch.md` §2.
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
  // Both arena → fast path; otherwise the host trampoline materialises
  // both sides via the appropriate backing methods.  Cross-origin
  // (one arena, one host) routes to the host trampoline, which
  // currently POISONs with TYPE_MISMATCH.
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
  // Mixed-origin or both-host: route to the host trampoline, which
  // currently POISONs with TYPE_MISMATCH (full materialisation is not
  // yet wired).  See CelListConcatImpl.
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
// Map-key iteration helpers (see cel_map.h).
//
// The iterator handle is the arena offset of a 16-byte state struct.
// Two shapes:
//
//   kind == 0 (ARENA):  walks ArenaMapHeader entries in place.
//     `payload` = header_ptr; `count` field unused (header carries it).
//
//   kind == 1 (HOST):   walks a flat snapshot the host trampoline
//     wrote into the arena at iter-open time.  Each snapshot entry is
//     a pair of CelValue cells (key, value) — total 48 bytes per entry.
//     `payload` = byte offset of entry 0; `count` = entry count.
//     String / bytes / message / map / list payloads inside snapshot
//     CelValues reference arena bytes (allocated by the trampoline),
//     so the snapshot is valid for the rest of the current Eval.
//
// `cursor` is the 1-based index of the "current" entry — i.e. the
// entry that the most recent `iter_next` returned 1 for.  `cursor == 0`
// is the pre-first state set by `iter_init`; key_at / value_at refuse
// to dereference it.
// =====================================================================

#define MAP_ITER_KIND_ARENA 0u
#define MAP_ITER_KIND_HOST 1u

// Per-entry stride for HOST snapshot entries: one CelValue key
// (24 bytes) directly followed by one CelValue value (24 bytes).
// Trampoline and `copy_iter_entry` MUST keep these in lockstep.
#define MAP_ITER_HOST_ENTRY_BYTES 48u

typedef struct {
  uint32_t kind;     // MAP_ITER_KIND_ARENA | MAP_ITER_KIND_HOST.
  uint32_t cursor;   // 1-based current entry; 0 = pre-first.
  uint32_t payload;  // ARENA: header_ptr; HOST: snapshot start offset.
  uint32_t count;    // HOST: entry count; ARENA: 0 (read from header).
} MapIterState;

_Static_assert(sizeof(MapIterState) == 16,
               "MapIterState must remain 16 bytes (iter ABI)");

// Resolve the iter-state struct from a handle, or NULL when the handle
// is the 0 sentinel (empty / poisoned / unsupported map kind).
// Centralising the deref keeps every caller's null-check identical.
static MapIterState* map_iter_state(uint32_t handle) {
  if (handle == 0) return (MapIterState*)0;
  return (MapIterState*)(cel_memory_base_() + handle);
}

// Return the iteration count for the supplied state — ARENA reads the
// header live (so a source that grows mid-iteration does not
// invalidate the iter); HOST uses the count cached at snapshot time.
static uint32_t map_iter_count(const MapIterState* state) {
  if (state->kind == MAP_ITER_KIND_ARENA) {
    return ((ArenaMapHeader*)(cel_memory_base_() + state->payload))->count;
  }
  return state->count;
}

// Host trampoline: walks a CEL_MAP_HOST source, allocates a flat
// snapshot in the arena via `cel_host`'s ArenaAllocator, encodes
// each (key, value) pair as two consecutive 24-byte CelValue cells,
// and writes the iter state's `{kind=HOST, cursor=0, payload, count}`
// fields at `state_offset`.  When the source is empty or
// snapshot-allocation fails, the trampoline sets `count = 0`.
#ifdef __wasm__
extern void cel_host_cel_map_iter_open(uint32_t state_offset, uint32_t map_slot)
    __attribute__((import_module("cel_host"),
                   import_name("cel_map_iter_open")));
#else
// Host build: weak no-op stub so unit tests link without the wasmtime
// trampoline.  Strong override registered in cel_host_wasmtime.cc.
__attribute__((weak)) void
cel_host_cel_map_iter_open(  // NOLINT(misc-use-internal-linkage)
    uint32_t state_offset, uint32_t map_slot) {
  (void)map_slot;
  MapIterState* s = (MapIterState*)(cel_memory_base_() + state_offset);
  s->kind = MAP_ITER_KIND_HOST;
  s->cursor = 0;
  s->payload = 0;
  s->count = 0;  // Empty: iter_init returns 0.
}
#endif

// Kind-dispatching count helper for map iter_range sources.  Used
// by codegen pre-sizing (`EmitLoadSourceCount` for map-source
// comprehensions) — the inline `payload+8` arena header read works
// for CEL_MAP_ARENA only.  CEL_MAP_HOST routes through the existing
// `cel_host.cel_map_size` trampoline (declared above for cel_size_at_*
// dispatch; reused here) which writes a CelValue; we unbox the int
// payload.  Returns 0 for empty / poisoned / unsupported kinds; the
// comprehension's pre-size then allocates a zero-capacity accu and
// the loop body never runs.
// cel:codegen-export
uint32_t cel_map_count(uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  if (m->kind == CEL_MAP_ARENA) {
    return arena_map_header(m)->count;
  }
  if (m->kind == CEL_MAP_HOST) {
    uint32_t tmp = arena_alloc((uint32_t)sizeof(CelValue));
    if (tmp == 0) return 0;
    cel_host_cel_map_size(tmp, map_slot);
    CelValue* out = cel_value_at(tmp);
    if (out->kind != CEL_INT || out->payload.i < 0) return 0;
    // Map sizes fit in u32 by construction (per-eval arena bytes
    // bound them well below 2^32); narrow safely.
    return (uint32_t)out->payload.i;
  }
  return 0;
}

// Map-source sibling of `vend_poison_list_view` (see the poisoned-
// comprehension-sources block comment above that function): vend a
// HOST-shaped one-entry iter whose key AND value both carry `*poison`
// so the loop body's 3VL absorption propagates it into the accu.
// Returns the MapIterState offset.  Fresh arena bytes when available;
// the emergency block's map half when exhausted; traps only when the
// block was never reserved.
static uint32_t vend_poison_map_iter(const CelValue* poison) {
  uint32_t state_off = arena_alloc(64u);
  if (state_off == 0) {
    if (arena_oom_block() == 0) __builtin_trap();
    state_off = arena_oom_block() + (uint32_t)kOomBlockMapIterStateOff;
  }
  const uint32_t entry_off = state_off + (uint32_t)sizeof(MapIterState);
  MapIterState* state = (MapIterState*)(cel_memory_base_() + state_off);
  state->kind = MAP_ITER_KIND_HOST;
  state->cursor = 0;
  state->payload = entry_off;
  state->count = 1;
  *cv_at(entry_off) = *poison;                               // key
  *cv_at(entry_off + (uint32_t)sizeof(CelValue)) = *poison;  // value
  return state_off;
}

// Arena-map arm of `cel_map_iter_init`.
static uint32_t map_iter_init_arena(const CelValue* m) {
  ArenaMapHeader* hdr = arena_map_header(m);
  // Empty map: skip the state alloc entirely — `iter_next(0)` returns
  // 0 immediately, so the comprehension loop exits without entering
  // the body.  Saves 16 arena bytes per empty-iter.
  if (hdr->count == 0) return 0;
  uint32_t state_off = arena_alloc((uint32_t)sizeof(MapIterState));
  if (state_off == 0) {
    // OOM: surface as an error iteration, never as an empty walk.
    CelValue oom;
    poison(&oom, CEL_ERR_OVERFLOW);
    return vend_poison_map_iter(&oom);
  }
  MapIterState* state = (MapIterState*)(cel_memory_base_() + state_off);
  state->kind = MAP_ITER_KIND_ARENA;
  state->cursor = 0;
  state->payload = m->payload.arena_map.header_ptr;
  state->count = 0;  // unused
  return state_off;
}

// Host-map arm of `cel_map_iter_init` — snapshots via the trampoline.
static uint32_t map_iter_init_host(uint32_t map_slot) {
  uint32_t state_off = arena_alloc((uint32_t)sizeof(MapIterState));
  if (state_off == 0) {
    CelValue oom;
    poison(&oom, CEL_ERR_OVERFLOW);
    return vend_poison_map_iter(&oom);
  }
  // Trampoline writes every field of the state struct + the
  // snapshot payload into the arena.  On empty / failure it
  // leaves count==0.
  cel_host_cel_map_iter_open(state_off, map_slot);
  MapIterState* state = (MapIterState*)(cel_memory_base_() + state_off);
  if (state->count == 0) return 0;
  return state_off;
}

uint32_t cel_map_iter_init(uint32_t map_slot) {
  CEL_LOG("enter");
  CelValue* m = cel_value_at(map_slot);
  if (m->kind == CEL_MAP_ARENA) return map_iter_init_arena(m);
  if (m->kind == CEL_MAP_HOST) return map_iter_init_host(map_slot);
  if (m->kind == CEL_ERROR || m->kind == CEL_UNKNOWN) {
    // Poisoned source (e.g. `{'a': 1/0}` — construction errors poison
    // the map value): propagate it through one error iteration
    // instead of the silent empty walk this used to take.
    return vend_poison_map_iter(m);
  }
  // Wrong kind: checker guarantees a map-typed iter_range — drift.
  CelValue mismatch;
  poison(&mismatch, CEL_ERR_TYPE_MISMATCH);
  return vend_poison_map_iter(&mismatch);
}

uint32_t cel_map_iter_next(uint32_t iter_handle) {
  CEL_LOG("enter");
  MapIterState* state = map_iter_state(iter_handle);
  if (state == (MapIterState*)0) return 0;
  const uint32_t count = map_iter_count(state);
  // `cursor` is the 1-based index of the *current* entry.  After the
  // last entry has been exposed (cursor == count), iteration is done
  // and every further call returns 0 without mutating state.
  if (state->cursor >= count) return 0;
  state->cursor++;
  return 1;
}

// Write the current entry's key/value into `out_slot`.  Routed through
// a shared helper so the key/value variants stay one-line dispatches.
static void copy_iter_entry(uint32_t out_slot, uint32_t iter_handle,
                            int want_value) {
  CelValue* out = cel_value_at(out_slot);
  MapIterState* state = map_iter_state(iter_handle);
  if (state == (MapIterState*)0 || state->cursor == 0) {
    // Codegen contract: a read without a preceding `iter_next` that
    // returned 1 is a generator bug.  Stamp an error rather than
    // dereferencing past the entries run; the eval surfaces it as
    // the comprehension's result via 3VL absorption upstream.
    poison(out, CEL_ERR_INDEX_OUT_OF_BOUNDS);
    return;
  }
  uint32_t i = state->cursor - 1;
  if (state->kind == MAP_ITER_KIND_ARENA) {
    ArenaMapHeader* hdr =
        (ArenaMapHeader*)(cel_memory_base_() + state->payload);
    *out = want_value ? *arena_map_entry_val(hdr, i)
                      : *arena_map_entry_key(hdr, i);
    return;
  }
  // HOST: snapshot entries are 48 bytes each — key at +0, value at +24.
  const uint32_t entry_off = state->payload + i * MAP_ITER_HOST_ENTRY_BYTES;
  const uint32_t cell_off = want_value ? entry_off + 24u : entry_off;
  *out = *(CelValue*)(cel_memory_base_() + cell_off);
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
// Polymorphic equality dispatcher.
//
// `cel_equals_at_vv` and `cel_not_equals_at_vv` resolve every cel-cpp
// `equals` / `not_equals` overload at runtime via operand-kind
// switch.  Per langdef §"Equality":
//
//   - Numeric kinds (int / uint / double) compare cross-type by
//     mathematical value via the cross-numeric ladder.
//   - Same-kind bool / string / bytes / null use the existing
//     `cel_*_eq_at_vv` helpers.
//   - Aggregate kinds (list / map) tail-call the kDynamic
//     dispatcher (`cel_list_eq` / `cel_map_eq`), which handles
//     arena vs host origin internally.
//   - CEL_MESSAGE values delegate to the kHost
//     `cel_host_cel_message_eq` import.
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
// CEL_TYPE × CEL_TYPE equality — byte compare on `payload.s`, the
// type-name string in linear memory (langdef §"Equality").  Extracted
// from `equality_kernel` to keep that function under the
// function-size gate.
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
  write_bool(out, cel_byteptr_equal_(base + a->payload.s.ptr,
                                     base + b->payload.s.ptr, la));
}

// The CEL_OPTIONAL arm recurses on inner CelValues through
// `equality_kernel` (forward-declared with the deep-equality helpers
// above).
// optional<T> == optional<U> per cel-cpp
// `OptionalValueInterface::Equal` + langdef §"Equality" optional
// addendum.  Both None → true; present mismatch → false; both
// Some(inner) → recurse on inner CelValues.  Extracted from
// `equality_kernel` to keep that switch under the
// `readability-function-size` gate.
static void optional_eq_at_vv(uint32_t out_slot, uint32_t a_slot,
                              uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  OptionalCell* ac = (OptionalCell*)cv_at(a->payload.opt);
  OptionalCell* bc = (OptionalCell*)cv_at(b->payload.opt);
  if (ac->present == 0 && bc->present == 0) {
    write_bool(out, 1);
    return;
  }
  if (ac->present != bc->present) {
    write_bool(out, 0);
    return;
  }
  const uint32_t a_inner =
      a->payload.opt + (uint32_t)offsetof(OptionalCell, inner);
  const uint32_t b_inner =
      b->payload.opt + (uint32_t)offsetof(OptionalCell, inner);
  equality_kernel(out_slot, a_inner, b_inner);
}

// Same-kind scalar dispatch.  Caller guarantees a->kind == b->kind ==
// `kind`.  Returns 1 if it published the result; 0 means the kind is
// an aggregate that needs the polymorphic arms in `equality_kernel`.
// Factored out so the parent stays under the readability-function-size
// gate.
static int equal_same_kind(uint32_t kind, uint32_t out_slot, uint32_t a_slot,
                           uint32_t b_slot) {
  switch (kind) {
    case CEL_NULL:
      cel_null_eq_at_vv(out_slot, a_slot, b_slot);
      return 1;
    case CEL_BOOL:
      cel_bool_eq_at_vv(out_slot, a_slot, b_slot);
      return 1;
    case CEL_STRING:
      cel_string_eq_at_vv(out_slot, a_slot, b_slot);
      return 1;
    case CEL_BYTES:
      cel_bytes_eq_at_vv(out_slot, a_slot, b_slot);
      return 1;
    case CEL_TYPE:
      type_eq_at_vv(out_slot, a_slot, b_slot);
      return 1;
    case CEL_OPTIONAL:
      optional_eq_at_vv(out_slot, a_slot, b_slot);
      return 1;
    case CEL_DURATION:
    case CEL_TIMESTAMP: {
      // (seconds, nanos) payload compare; `dur` and `ts` share the
      // same union arm.
      const CelValue* a = cel_value_at(a_slot);
      const CelValue* b = cel_value_at(b_slot);
      write_bool(cel_value_at(out_slot),
                 a->payload.dur.seconds == b->payload.dur.seconds &&
                     a->payload.dur.nanos == b->payload.dur.nanos);
      return 1;
    }
    case CEL_IP:
      write_bool(cel_value_at(out_slot),
                 net_ip_eq(cel_value_at(a_slot), cel_value_at(b_slot)));
      return 1;
    case CEL_CIDR:
      write_bool(cel_value_at(out_slot),
                 net_cidr_eq(cel_value_at(a_slot), cel_value_at(b_slot)));
      return 1;
    default:
      return 0;  // Aggregates fall through to the polymorphic arms.
  }
}

static void equality_kernel(uint32_t out_slot, uint32_t a_slot,
                            uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (absorb_3vl_binary(out, a, b)) return;
  if (is_numeric_kind(a->kind) && is_numeric_kind(b->kind)) {
    cel_numeric_eq_at_vv(out_slot, a_slot, b_slot);
    return;
  }
  if (a->kind == b->kind &&
      equal_same_kind(a->kind, out_slot, a_slot, b_slot)) {
    return;
  }
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
  // (NOT type-mismatch error).
  write_bool(out, 0);
}

// Wasm-exported via `-Wl,--export=cel_equals_at_vv` in
// runtime/BUILD.bazel; `misc-use-internal-linkage` is
// silenced because internal linkage would hide it from the wasm
// export table.
// NOLINTNEXTLINE(misc-use-internal-linkage)
// cel:codegen-export
void cel_equals_at_vv(uint32_t out_slot, uint32_t a_slot, uint32_t b_slot) {
  CEL_LOG("enter");
  equality_kernel(out_slot, a_slot, b_slot);
}

// Wasm-exported via `-Wl,--export=cel_not_equals_at_vv` — see
// `cel_equals_at_vv` above for the linkage rationale.
// NOLINTNEXTLINE(misc-use-internal-linkage)
// cel:codegen-export
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
