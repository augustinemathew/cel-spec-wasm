# Map and list dispatch: arena / host / dynamic

Status: **fully reconciled into design.md 2026-04-25.**  Map half
shipped 2026-04-24 (M3.A–H); list half shipped 2026-04-25 (M4.A–J).
The §11 reconciliation checklist below is fully ticked.  The
map-specific and list-specific sections both folded into
`design.md`; this doc remains as the dispatch-design retro for
both halves.

Parked as a standalone artifact so `design.md §4.7.2` / `§4.7.3` and
related sections can absorb this in one focused reconciliation pass
later.  This doc is the source of truth for the map/list dispatch
design until that reconciliation lands.

**Scope.** How `map<K,V>` and `list<X>` values flow through the
compiler, the wasm runtime, and the host adapter — specifically
how `_[_]`, `size()`, `in`, and related operations dispatch when
the operand could be either a CEL-constructed literal (living in
the arena) or a host-backed value (proto map field, JSON, custom
function return).  Consequence for codegen, `NodeAnnotation`,
the runtime module's import set, and the `CelValue` layout.

**Out of scope.** M2 doesn't ship maps or lists (shipped in M6;
proto literals in M7).  This doc defines the shape M6 will land
against; `m2-ident-select-unknowns.md` covers the M2 interface
stubs that reserve this design's extension points.

---

## 1. Problem

CEL's type system gives us **one** type: `map<K,V>`.  The runtime
has **two** plausible representations:

  - **Arena-resident** — a small header + entries packed into the
    expr module's linear memory by `cel_map_create` /
    `cel_map_insert`.  Fast to access from wasm (pure loads); no
    host trips.  The natural fit for CEL-constructed values
    (literals, comprehension results).
  - **Host-backed** — a `shared_ptr<HostMapBacking>` held in
    `cel_refs`, accessed via vtable dispatch.  The natural fit
    for proto map fields, JSON, and anything else the host hands
    us through `Activation::Bind` or a custom-function return.

A CEL expression like `m[k]` must work identically regardless of
which representation `m` has.  Three ways to reconcile:

  1. Always materialise into one form (copy on read) — rejected,
     violates the "no copy" invariant for host data.
  2. Always route through the vtable (Option 2 of earlier drafts)
     — clean dispatch, but every literal op pays a host trip.
  3. Keep both forms and dispatch; pay the vtable cost only when
     we can't prove arena origin at compile time.

This doc commits to **(3)**.  The static-inference path for
origin is strong enough that the kDynamic fallback is rare in
real expressions.

---

## 2. Origin inference (compile-time, ResolvePass)

A new `Origin` enum lives on `NodeAnnotation` for every node
whose `repr ∈ {kMap, kList}`:

```cpp
// ir/annotations.h
enum class Origin : uint8_t {
  kArena   = 0,   // statically proven: this expression built the value
  kHost    = 1,   // statically proven: value came from outside the expr
  kDynamic = 2,   // mixed / unknown; runtime discriminator required
};

struct NodeAnnotation {
  // … existing fields …
  Origin map_origin  = Origin::kDynamic;   // meaningful iff repr == kMap
  Origin list_origin = Origin::kDynamic;   // meaningful iff repr == kList
};
```

Defaulting to `kDynamic` is the safe choice: if ResolvePass
forgets to set it on some new node kind, codegen emits the
universal dispatcher — correct, just slower.  `kArena` is the
optimisation opt-in.

### 2.1 Inference rules

Bottom-up walk.  For a node `n` with `n.repr == kMap` (mirror
for `kList`):

| `n.kind()` | `n.map_origin` | Why |
|---|---|---|
| `kCreateMap` | `kArena` | `cel_map_create` writes the header into the arena; we own the bytes. |
| `kComprehension` folding into a map (e.g. `list.reduce(..., {})`) | `kArena` | CEL iteration builds the result in-arena. |
| `kSelect` reading a map-typed proto field | `kHost` | `cel_host.cel_get_field` returns a `CEL_MAP_HOST`. |
| `kIdent` declared as `map<K,V>` | `kHost` | Activation values enter via the prelude, which interns into `cel_refs` → `CEL_MAP_HOST`. |
| `kCall` returning a map | `kHost` | Custom-function return trampolines wrap returned `Value::Map` / `Value::HostMap` into a `HostMapBacking` and intern. |
| `?:` / `\|\|` / `&&` where the two branches have different origins | `kDynamic` | Can't pick at compile time. |
| `?:` / logical where both branches have the same origin | that origin | Easy coalesce. |

**Key observation:** every non-CEL-built value enters the expr
module through some path (prelude for idents, trampoline for
selects, trampoline for call-returns) that interns into
`cel_refs` and produces `CEL_*_HOST`.  Arena residency is the
marker of "this eval built it."  Mixed-branch conditionals are
the only real source of `kDynamic`.

### 2.2 Pseudocode

```cpp
Origin InferMapOrigin(const cel::Expr& n, const WasmAnnotations& a) {
  switch (n.kind()) {
    case kCreateMap:      return Origin::kArena;
    case kComprehension:  return ComprehensionBuildsMap(n)
                                   ? Origin::kArena
                                   : InferFromResult(n, a);
    case kSelect:         return Origin::kHost;   // proto map field
    case kIdent:          return Origin::kHost;   // prelude-interned
    case kCall:           return Origin::kHost;   // trampoline-interned
    case kSelectOr: {     // ?: / || / && over map operands
      auto l = a.Find(n.lhs())->map_origin;
      auto r = a.Find(n.rhs())->map_origin;
      return (l == r) ? l : Origin::kDynamic;
    }
    default:              return Origin::kDynamic;
  }
}
```

---

## 3. Three dispatch paths

For an operation like `m[k]` (`_[_]` overload on a map operand),
codegen picks the emitted call based on `operand.map_origin`:

| Origin | Emitted call | Runtime path | Host trips |
|---|---|---|---|
| `kArena` | `call $cel.cel_map_lookup_arena` | Pure wasm: reads `ArenaMapHeader` in linear memory, scans entries. | 0 |
| `kHost` | `call $cel_host.cel_map_lookup` | One host trip: `cel_refs` lookup + `HostMapBacking::Get` via vtable. | 1 |
| `kDynamic` | `call $cel.cel_map_lookup` | Runtime module reads `CelValue.kind` from the operand; branches on arena → tail-call `cel_map_lookup_arena`, host → call `cel_host.cel_map_lookup`. | 0 (arena arm) or 1 (host arm) |

The kDynamic dispatcher **tail-calls** the arena fast path rather
than duplicating its body.  The host arm imports
`cel_host.cel_map_lookup` — the only place the runtime module
depends on cel_host.  One extern declaration, one call site.

### 3.1 Runtime dispatcher

```c
// compiler/runtime/cel_runtime.c

// Extern import — wasm-ld emits (import "cel_host" "cel_map_lookup" …)
// based on this attributed declaration.
extern void cel_host_cel_map_lookup(uint32_t out,
                                    uint32_t map,
                                    uint32_t key)
    __attribute__((import_module("cel_host"),
                   import_name("cel_map_lookup")));

// Universal dispatcher — codegen emits this for kDynamic operands.
void cel_map_lookup(uint32_t out_slot,
                    uint32_t map_slot,
                    uint32_t key_slot) {
  CelValue* m = cel_value_at(map_slot);

  // 3VL absorption, uniform across both paths.
  if (m->kind == CEL_UNKNOWN || m->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *m;
    return;
  }

  if (m->kind == CEL_MAP_ARENA) {
    cel_map_lookup_arena(out_slot, map_slot, key_slot);
    return;
  }
  if (m->kind == CEL_MAP_HOST) {
    cel_host_cel_map_lookup(out_slot, map_slot, key_slot);
    return;
  }

  // Checker should have rejected; defence.
  cel_value_at(out_slot)->kind = CEL_ERROR;
  cel_value_at(out_slot)->payload.err = CEL_ERR_TYPE_MISMATCH;
}

// Arena-only fast path — codegen emits this for kArena operands;
// also tail-called by cel_map_lookup's arena arm.  No imports.
void cel_map_lookup_arena(uint32_t out_slot,
                          uint32_t map_slot,
                          uint32_t key_slot) {
  CelValue* m = cel_value_at(map_slot);
  ArenaMapHeader* h =
      (ArenaMapHeader*)(void*)(uintptr_t)m->payload.arena_map.header_ptr;
  CelValue* key = cel_value_at(key_slot);
  for (uint32_t i = 0; i < h->count; ++i) {
    CelValue* entry_k = (CelValue*)(void*)(uintptr_t)
        (h->entries_offset + i * 48);
    if (cel_value_equal(entry_k, key)) {
      CelValue* entry_v = (CelValue*)(void*)(uintptr_t)
          (h->entries_offset + i * 48 + 24);
      *cel_value_at(out_slot) = *entry_v;
      return;
    }
  }
  cel_value_at(out_slot)->kind = CEL_ERROR;
  cel_value_at(out_slot)->payload.err = CEL_ERR_NO_SUCH_KEY;
}
```

---

## 4. Data structures

### 4.1 `ArenaMapHeader`

```c
// compiler/runtime/cel_data.h

// Header for a runtime-constructed map.  Lives in the arena at
//   CelValue{CEL_MAP_ARENA}.payload.arena_map.header_ptr.
// Entries are stored contiguously at entries_offset — each entry
// is a (key, value) pair of back-to-back CelValues (24B each → 48B
// per entry).  Linear scan at lookup time (maps in CEL are small
// in practice; hash-table optimisation is deferred).
typedef struct {
  uint32_t count;            // populated entries; 0 ≤ count ≤ capacity
  uint32_t capacity;         // entry slots allocated at entries_offset
  uint32_t entries_offset;   // arena offset to entry[0].key; entry[i].key
                             //   at (entries_offset + i*48), entry[i].value
                             //   at (entries_offset + i*48 + 24).
  uint32_t _pad;             // reserved; may hold a bucket-table offset
                             //   when hashing is added.
} ArenaMapHeader;
_Static_assert(sizeof(ArenaMapHeader) == 16, "fixed layout");
```

Memory layout for `{"a": 1, "b": 2}` with `arena_base = 1024`:

```
offset 1024  ArenaMapHeader { count=2, capacity=2,
                              entries_offset=1040, _pad=0 }

offset 1040  entry[0].key    = CelValue{CEL_STRING, {ptr=<"a">, len=1}}
offset 1064  entry[0].value  = CelValue{CEL_INT, payload.i=1}
offset 1088  entry[1].key    = CelValue{CEL_STRING, {ptr=<"b">, len=1}}
offset 1112  entry[1].value  = CelValue{CEL_INT, payload.i=2}

offset 1136  (arena bump continues here)
```

Workspace CelValue pointing at this map:
```
CelValue { kind = CEL_MAP_ARENA, payload.arena_map.header_ptr = 1024 }
```

### 4.2 `ArenaListHeader`

```c
typedef struct {
  uint32_t count;              // populated elements
  uint32_t capacity;           // slots at elements_offset
  uint32_t elements_offset;    // arena offset to element[0]; element[i]
                               //   at (elements_offset + i*24).
  uint32_t _pad;               // reserved.
} ArenaListHeader;
_Static_assert(sizeof(ArenaListHeader) == 16, "fixed layout");
```

Same pattern; elements are single `CelValue`s, stride = 24.

### 4.3 Revised `CelValue` payload

```c
// compiler/runtime/cel_data.h

typedef struct {
  uint32_t kind;
  uint32_t _pad;
  union {
    bool     b;
    int64_t  i;
    uint64_t u;
    double   d;
    struct { uint32_t ptr; uint32_t len; } s;    // CEL_STRING / CEL_BYTES
    uint32_t msg_slot;                            // CEL_MESSAGE → cel_refs
    struct { uint32_t header_ptr; } arena_map;   // CEL_MAP_ARENA → arena
    struct { uint32_t header_ptr; } arena_list;  // CEL_LIST_ARENA → arena
    uint32_t ref_slot;                            // CEL_MAP_HOST / CEL_LIST_HOST → cel_refs
    uint32_t err;                                 // CEL_ERROR (error code)
    uint32_t attr_id;                             // CEL_UNKNOWN (attribute id)
  } payload;
} CelValue;
_Static_assert(sizeof(CelValue) == 24, "fixed 24-byte slot");
```

### 4.4 `CelKind` enum additions

```c
typedef enum {
  CEL_NULL       = 0,
  CEL_BOOL       = 1,
  CEL_INT        = 2,
  CEL_UINT       = 3,
  CEL_DOUBLE     = 4,
  CEL_STRING     = 5,
  CEL_BYTES      = 6,
  CEL_MAP_ARENA  = 7,    // renamed from CEL_MAP; split on origin
  CEL_MAP_HOST   = 8,    // new
  CEL_LIST_ARENA = 9,    // renamed from CEL_LIST; split on origin
  CEL_LIST_HOST  = 10,   // new
  CEL_MESSAGE    = 11,
  CEL_DURATION   = 12,
  CEL_TIMESTAMP  = 13,
  CEL_UNKNOWN    = 14,
  CEL_ERROR      = 15,
} CelKind;
```

The numeric values shift relative to today's `CelKind` — a
breaking ABI change.  Acceptable since the rewrite is an ABI
boundary; all consumers (`api/value.cc`, `cel_host`, checker
`Repr` mapping) update in the same commit that lands this.

---

## 5. Construction primitives (runtime-owned, no host trips)

```c
// cel_runtime.h

// Create an empty map with room for `initial_capacity` entries.
// Codegen passes the exact entry count for literals; comprehensions
// that can't pre-size use a conservative default (e.g. 4).
void cel_map_create(uint32_t out_slot, uint32_t initial_capacity);

// Append one entry.  Duplicate-key → poison the map with CEL_ERROR
// per langdef.md §"Map creation".  Grows via cel_map_grow (bump-
// allocate 2x region, copy entries, leak old region — reclaimed
// at cel_reset).
void cel_map_insert(uint32_t map_slot,
                    uint32_t key_slot,
                    uint32_t value_slot);

// List counterparts.
void cel_list_create(uint32_t out_slot, uint32_t initial_capacity);
void cel_list_append(uint32_t list_slot, uint32_t elem_slot);
```

`cel_map_size` / `cel_list_size`: trivial `ArenaMapHeader.count` /
`ArenaListHeader.count` reads.  Follow the origin-dispatch rules
(§3) — kArena reads the header directly; kHost goes through
`cel_host.cel_map_size`; kDynamic branches.

---

## 6. Codegen emit rules

For each map/list operation, codegen looks up
`operand.map_origin` / `list_origin` in `NodeAnnotation` and
emits one of three fully-qualified import names:

| Op | kArena | kHost | kDynamic |
|---|---|---|---|
| `_[_]` (map lookup) | `cel.cel_map_lookup_arena` | `cel_host.cel_map_lookup` | `cel.cel_map_lookup` |
| `size(map)` | `cel.cel_map_size_arena` | `cel_host.cel_map_size` | `cel.cel_map_size` |
| `k in map` | `cel.cel_map_contains_arena` | `cel_host.cel_map_contains` | `cel.cel_map_contains` |
| `_[_]` (list indexing) | `cel.cel_list_at_arena` | `cel_host.cel_list_at` | `cel.cel_list_at` |
| `size(list)` | `cel.cel_list_size_arena` | `cel_host.cel_list_size` | `cel.cel_list_size` |
| `x in list` | `cel.cel_list_contains_arena` | `cel_host.cel_list_contains` | `cel.cel_list_contains` |

OverloadTable gains three rows per op (one per origin).
Codegen's `kCall` arm picks the row via `operand.map_origin` /
`operand.list_origin` before calling `OverloadTable::LookupById`.

---

## 7. Runtime module's import + export surface

**New runtime-module imports (M6+):**
```
(import "cel_host" "cel_map_lookup"     …)
(import "cel_host" "cel_map_size"       …)
(import "cel_host" "cel_map_contains"   …)
(import "cel_host" "cel_list_at"        …)
(import "cel_host" "cel_list_size"      …)
(import "cel_host" "cel_list_contains"  …)
```

Six new imports.  Each is called only from the kDynamic
dispatcher in the runtime's C source; kArena paths never touch
them.

**New runtime-module exports (M6+):**
```
cel_map_create / cel_map_insert / cel_map_grow
cel_map_lookup_arena / cel_map_lookup
cel_map_size_arena / cel_map_size
cel_map_contains_arena / cel_map_contains
(list counterparts — cel_list_*)
```

**`compiler/runtime/wasm_imports.txt`** grows by six lines to
whitelist the new host-side imports during cross-compile
(`--allow-undefined-file` consults this).

---

## 8. Plan / instantiation order (`Engine::Plan`)

Invariant the current loader already satisfies (design.md §9.1);
documenting here for completeness:

```
1. Create store + linker.
2. Register on the linker, BEFORE any module instantiates:
     - cel.memory
     - cel_env.cel_log
     - cel_host.cel_get_field, cel_host.cel_has_field    (M2)
     - cel_host.cel_map_lookup, …, cel_host.cel_list_at, …  (M6)
     - custom-function trampolines                        (M5)
3. Instantiate the runtime module.  Its imports (cel.memory,
   cel_env.cel_log, cel_host.cel_map_*) are all satisfied.
4. Bind runtime exports (cel_alloc, cel_reset, cel_map_*,
   cel_list_*, scalars, …) onto the linker as cel.*
5. Instantiate the expr module.  Its imports are satisfied.
```

The two-phase order doesn't change — we just add more names to
the registration step in (2).

---

## 9. Milestone fit

| Milestone | Scope relative to this doc |
|---|---|
| **M2** (idents + proto field reads + unknowns) — shipped 2026-04-24 | Signature-final stubs land in `api/internal/cel_host.h`: `HostMapBacking` + `HostListBacking` interfaces declared; bodies `ABSL_CHECK(false) << "M6"` (now `M3`).  `ProtoBacking::ReadField` on MAP/REPEATED returns `CEL_ERR_TYPE_UNSUPPORTED` — the envelope boundary (locked by `EnvelopeBoundaryE2ETest::SelectRepeatedFieldReturnsUnsupportedError`).  `NodeAnnotation.map_origin` + `list_origin` fields added but only `kSelect` / `kIdent` declared map/list populate them (always to `kHost`). |
| **M3** (map literals only) — map half of this design lands here | `CEL_MAP_ARENA` / `CEL_MAP_HOST` split; `ArenaMapHeader`; runtime-side `cel_map_*` arena helpers; kDynamic dispatcher with `return_call` tail calls (`__attribute__((musttail))` in `cel_runtime.c`); `HostMap` + `ProtoMap` concrete backings; narrow `kCall(_[_])` arm across all three map dispatch paths.  Envelope boundary drops the M2 `CEL_ERR_TYPE_UNSUPPORTED` for map fields only; `ProtoBacking::ReadField` on MAP returns `Value::HostMap(ProtoMap{…})`.  REPEATED stays erroring until the lists iteration.  Reconciliation of the **map** §11 bullets into `design.md` ticked in this milestone. |
| **M3-follow-up (lists)** | Replay the M3 pattern for lists: `CEL_LIST_ARENA` / `CEL_LIST_HOST` split; `ArenaListHeader`; `HostList` + `ProtoList` concretes; `kCreateList` + `kCallExpr(_[_])` on list × 3 origins; envelope flip for REPEATED fields. Ticks the list bullets in §11. |
| **Next milestone** (`kCall` + built-in overload set) | `size`, `in`, `==`, `+` on maps + lists reuse M3's three-path origin dispatch per §6.  `OverloadTable::kBuiltinSeeds` populated.  No new data-structure work. |
| **Later milestone** (proto literals) | `cel_host.cel_make_message` trampoline + `cel.abi.message_ctors[]` + `kCreateStruct` codegen arm.  Orthogonal to this doc's map/list dispatch; no changes to the three-path design. |
| **M5** (comprehensions + customs + 3VL) | Origin rule for `kComprehension` folding into a map/list → `kArena`.  `cel_map_insert` / `cel_list_append` exercised as comprehension side effects.  Custom-function return trampoline's map/list wrapping defined; `kCall.map_origin = kHost`. |

M2's stub work is what keeps M3 additive rather than
structural — the interface is declared before the body is
implemented.

---

## 10. Open questions

  1. **Hash table for larger maps.**  Linear scan in
     `cel_map_lookup_arena` is fine for ≤ 20 entries; past that,
     lookup goes quadratic across repeated indexing.  Open
     `ArenaMapHeader._pad` is reserved for a bucket-table
     offset if a bench motivates adding one.  Decision deferred
     to M6 with a bench target.
  2. **Comprehensions that produce maps of unknown initial
     capacity.**  `list.reduce(acc, x, acc + {x.k: x.v}, {})`
     starts empty and grows.  `cel_map_create(out, 4)` + growth
     on each `+` — but `+` on maps is map-merge, not in-place
     insert.  Probably want a comprehension-internal
     `cel_map_merge` that mutates in place.  Decision deferred
     to M5.
  3. **`==` on maps with different origins.**  `{"a":1} ==
     proto.some_map` — both sides conform to `HostMapBacking`
     (kArena wraps implicitly? or we special-case the
     `kArena`+`kHost` pair in `cel_map_eq`?).  Spec says order-
     independent deep equality.  Simplest: `cel_map_eq` always
     iterates both sides via whichever access path fits, entry-
     by-entry, up to size.  Decision deferred to M6.
  4. **Large arena consumption for wide maps.**  A 1000-entry
     map is 1000 × 48B entries + header + string payloads ≈ 60–
     100 KB.  Our default `mem_size_bytes = 128 KiB` would
     exhaust quickly.  Codegen should probably fall back to
     `kHost` construction (build a `MaterialisedMapBacking` on
     the host, intern) past a threshold — but detecting "this
     comprehension will produce many entries" at compile time is
     hard.  Punt to M8 (bench-driven).
  5. **`cel_refs` intern cost on every PartialEval.**  An
     expression with 5 map reads against different proto paths
     interns 5 backings per eval.  Cheap individually (~50 ns
     each) but adds up under hot loops.  Benchmark and decide
     whether to cache interned slots across evals (would
     require `cel_reset` to be selective instead of global —
     breaks simplicity).

---

## 11. Reconciliation checklist

When folding this into `design.md`:

- [x] `§4.1` `NodeAnnotation` gains `map_origin` + `list_origin`.
  `Origin` enum added to `ir/annotations.h`.  *(map_origin shipped
  M3.A; list_origin populated M4.F.)*
- [x] `§4.7.2` rewritten around the three-path dispatch.
  `ArenaMapHeader` defined.  Construction + lookup helpers
  listed per path.  *(M3.A–C.)*
- [x] `§4.7.3` rewritten for lists.  `ArenaListHeader` defined
  (16 B `{count, capacity, elements_offset, _pad}`; entries
  stride 24 B).  *(M4.A.)*
- [x] `§4.7.6.1` Layer-1 interface grows `HostMapBacking` +
  `HostListBacking`.  `HostList` (vector-backed) + `ProtoList`
  (proto reflection) bodies live in `api/internal/cel_host.cc`.
  *(M3.D for maps; M4.D for lists.)*
- [x] `§5` ResolvePass contract gains the origin-inference rule
  (this doc's §2).  *(M3.F maps; M4.F lists.)*
- [x] `§7.2` codegen `kCreateMap` / `kCreateList` / `_[_]` arms
  specify origin-dependent emit.  *(M3.F maps; M4.F lists.)*
- [x] `§8.1` runtime build flags: wasm_imports.txt gains
  `cel_host.cel_map_lookup` + `cel_host.cel_list_at`; exports
  list gains `cel_map_create` / `cel_map_insert` /
  `cel_map_lookup_arena` / `cel_map_lookup` plus
  `cel_list_create` / `cel_list_set` / `cel_list_at_arena` /
  `cel_list_at`.  *(M3.B–C maps; M4.B–C lists.)*
- [x] `cel-host-surface.md` adds map + list dispatch to the
  adapter's method set (mirror the field-read story).
  *(M3.D maps; M4.D–E lists.)*

Both map and list rows are reconciled.  Fully reconciled into
design.md 2026-04-25.

## 12. Map half — what shipped (M3 retro)

  - `CelKind` split: `CEL_MAP_ARENA = 8`, `CEL_MAP_HOST = 9` (other
    kinds renumbered down).  `ArenaMapHeader` is 16 B with
    `{count, capacity, entries_offset, _pad}`; entries stride 48 B.
  - Runtime exports: `cel_map_create`, `cel_map_insert`,
    `cel_map_lookup_arena`, `cel_map_lookup` (the
    `__attribute__((musttail))` dispatcher).  Cross-toolchain
    tail-call config (`-mtail-call`, Binaryen
    `--enable-tail-call`, wasmtime `wasm_tail_call(true)`) all
    flipped on.
  - Host import: one new line in `wasm_imports.txt` —
    `cel_host.cel_map_lookup`.  Three-arg ABI (`out_slot`,
    `map_slot`, `key_slot`) distinct from the four-arg
    `cel_get_field` / `cel_has_field`.
  - `Value::Map(...)` / `Value::HostMap(...)` constructors;
    `StructurallyEquals` kMap arm uses pointer-identity at M3
    (deferred to a richer comparator when comprehensions land).
  - `HostMap` (vector-backed) + `ProtoMap` (proto-reflection-
    backed) concrete `HostMapBacking` impls share the
    map-key-equality + invalid-kind helpers.
  - `ProtoBacking::ReadField` on MAP fields wraps in
    `Value::HostMap(std::make_shared<ProtoMap>(...))`.

The lists slice repeats this shape for `CEL_LIST_ARENA` /
`CEL_LIST_HOST` + REPEATED-field reads; `cel_list_lookup` /
`cel_host.cel_list_at` follow the same three-path dispatch.
