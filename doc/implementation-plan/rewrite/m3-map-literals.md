# Rewrite M3 — map literals + indexing (with tail-call dispatch)

Status: **shipped 2026-04-24.**

What landed (vs the as-written plan): all nine slices A-I shipped
in order.  Two execution-time deltas worth flagging:

  - **M3.E.Layer3 wasmtime registration shipped during M3.H, not
    M3.E.**  The Layer-3 glue compiles standalone but only
    actually wires onto the linker once the conformance binary
    needed end-to-end map instantiation; that landed alongside
    the envelope bump.
  - **`Instance::Eval`'s decoder gained a `CEL_MAP_ARENA` arm
    in M3.H** to make `{...}` self-eval tests in the conformance
    corpus produce a `cel::Value::Map` host-side.  The plan
    didn't call this out explicitly under M3.F (the codegen
    slice); folded into M3.H since it's a host-surface
    extension.
  - **M2.C.0b layer-2 trampoline bodies (`CelGetFieldImpl` /
    `CelHasFieldImpl`) ship as Unimplemented-returning stubs
    (not the standard `ABSL_CHECK(false)` form) so the
    conformance harness can keep running while the real bodies
    land in their own slice.  See the Layer-2 stub note in
    `internal/cel_host.cc` for the rationale.

Parent: `design.md`.  Predecessor: `m2-ident-select-unknowns.md`
(shipped 2026-04-24).  Authoritative design for map dispatch:
**`map-list-dispatch.md`** — this doc implements the map half of
that design; list + proto literal construction slip to a later
milestone.

Scope rename vs the original roadmap: what `m1-scalar-pipeline.md
§10` called `M6` (list + map literals) now splits.  **M3 ships
maps only**; lists follow in the next iteration after M3 validates
the three-path dispatch contract end-to-end.  Proto literals stay
in a later milestone.

## 0. Why maps first, maps alone

  - **Three-path origin dispatch is the hard part.**  Shipping it
    for one aggregate kind — maps — lets us debug the tail-call
    wire-up, the `CelKind` ABI renumber, and the Binaryen tail-
    call emit before duplicating the pattern for lists.  Lists
    are nearly copy-paste of maps once the dispatch machinery
    proves out.
  - **Proto map field reads flip one M2 envelope boundary.**
    `ProtoBacking::ReadField` on MAP today returns
    `CEL_ERR_TYPE_UNSUPPORTED`.  Replacing that with
    `Value::HostMap(ProtoMap{…})` is the whole "proto
    field → host-backed aggregate" story in miniature.  Lists
    repeat the same pattern for REPEATED fields; shipping maps
    first validates the approach.
  - **`size` / `in` / `==` / `+` defer to next milestone.**  They
    live in the `kCall` + built-in overload set work and reuse
    the same three-path dispatch — but that's additive on top of
    M3, not a prerequisite.

## 1. Scope

### 1.1 What works end-to-end after M3

```cpp
auto compiler = *cel::Compiler::NewBuilder()
    .DeclareVariable("m", CelType::Map(CelType::String(), CelType::Int()))
    .RegisterMessageType(Customer::descriptor())
    .Build();
```

Compiles + evaluates:

  - `{"a": 1, "b": 2}` — map literal (kArena).
  - `{"a": 1}["a"]` — literal + indexing → `1`.
  - `m["k"]` — indexing into bound map (kHost).
  - `customer.metadata["env"]` — proto map field via
    `ProtoMap` backing (kHost).
  - `cond ? {"a": 1} : m` then `[.][...]` — mixed-branch
    conditional (kDynamic); runtime dispatcher routes.

### 1.2 Out of scope (deferred)

  - **Lists** — next iteration after M3, same dispatch pattern.
  - **Proto literals** — later milestone.
  - **`size(m)`, `k in m`, `m1 == m2`, `m1 + m2`** — ship with
    the `kCall` built-in overload set.  All reuse M3's three-
    path origin dispatch.
  - **Comprehensions** — M5.
  - **Hash-table lookup for large arena maps** — dispatch-doc
    §10 open question 1; linear scan stays until a bench
    motivates a bucket table.

### 1.3 Envelope boundary probes

  - `{"a":1}["b"]` → `CelValue{kind:CEL_ERROR, err:CEL_ERR_NO_SUCH_KEY}`.
  - Duplicate-key construction poisons the map to `CEL_ERROR`
    per langdef §"Map creation".
  - Cross-type numeric key equality: `{1:"x"}[1u]` returns
    `"x"` per langdef § map-key equality ladder.
  - M2's `EnvelopeBoundaryE2ETest::SelectRepeatedFieldReturnsUnsupportedError`
    stays green (lists still error out); a new
    `SelectMapFieldReturnsHostMap` test replaces its map-field
    counterpart.

## 2. Surfaces introduced in M3

All specified by `map-list-dispatch.md`; this is the landing
manifest for the map subset.

### 2.1 `runtime/cel_data.h` — `CelKind` split + `ArenaMapHeader`

  - **`CelKind` renumber** (dispatch-doc §4.4).  Partial — we
    add `CEL_MAP_ARENA` + `CEL_MAP_HOST` now, leave `CEL_LIST`
    as the existing single kind (its split waits for the lists
    slice).  Breaking ABI change; all consumers update
    in the M3.A commit.
  - **`CelValue.payload` union** — add `arena_map.header_ptr`
    (u32 offset) and `ref_slot` (u32, shared with `CEL_MESSAGE`
    already).  Slot stays 24 B.
  - **`ArenaMapHeader`** struct (dispatch-doc §4.1): `{count,
    capacity, entries_offset, _pad}` = 16 B.  Entries stride 48 B
    (key+value CelValues back to back).

### 2.2 `runtime/cel_runtime.{h,c}` — arena primitives + tail-call dispatcher

Per dispatch-doc §3 + §5:

**Construction primitives** (called from codegen's `kCreateMap`):
```c
void cel_map_create(uint32_t out_slot, uint32_t initial_capacity);
void cel_map_insert(uint32_t map_slot, uint32_t key_slot, uint32_t value_slot);
void cel_map_grow(uint32_t map_slot);  // internal; called from cel_map_insert
```

**Arena fast path** (called directly when codegen proved
`operand.map_origin == kArena`):
```c
void cel_map_lookup_arena(uint32_t out_slot, uint32_t map_slot, uint32_t key_slot);
```
Pure wasm; no host trip.  Linear scan of 48 B entries via
`cel_value_equal`.

**kDynamic dispatcher** (called when operand origin is
`kDynamic`):
```c
extern void cel_host_cel_map_lookup(uint32_t out, uint32_t m, uint32_t k)
    __attribute__((import_module("cel_host"),
                   import_name("cel_map_lookup")));

void cel_map_lookup(uint32_t out_slot, uint32_t map_slot, uint32_t key_slot) {
  CelValue* m = cel_value_at(map_slot);

  // 3VL absorption — same path regardless of origin.
  if (m->kind == CEL_UNKNOWN || m->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *m;
    return;
  }

  // Branch on actual kind; tail-call into the right arm.
  if (m->kind == CEL_MAP_ARENA) {
    __attribute__((musttail))
    return cel_map_lookup_arena(out_slot, map_slot, key_slot);
  }
  if (m->kind == CEL_MAP_HOST) {
    __attribute__((musttail))
    return cel_host_cel_map_lookup(out_slot, map_slot, key_slot);
  }

  // Checker should have rejected; defence.
  cel_value_at(out_slot)->kind = CEL_ERROR;
  cel_value_at(out_slot)->payload.err = CEL_ERR_TYPE_MISMATCH;
}
```

`__attribute__((musttail))` forces wasm `return_call` /
`return_call_indirect` emission.  Any form clang can't prove is
a tail call is a compile error — that's the whole point.
Dispatcher frames never grow the call stack.  The tail call
into the imported `cel_host_cel_map_lookup` becomes
`return_call $import_index` at the wasm level.

**Runtime toolchain requirements** (new):

  - clang wasm32 cross-compile: add `-mtail-call` to the flags
    in `compiler_v2/runtime/BUILD.bazel`'s genrule.
  - Binaryen: pass `--enable-tail-call` through
    `wasm-opt` / `BinaryenModuleValidate` / our codegen emit
    pipeline.
  - wasmtime: enable tail calls via
    `wasmtime::Config::wasm_tail_call(true)` in
    `api/internal/wasmtime_engine_state.cc::Initialize`.
  - `wasm_imports.txt`: one new line — `cel_host.cel_map_lookup`.

### 2.3 `api/internal/cel_host.{h,cc}` — two concrete backings

M2 shipped `HostMapBacking` as an abstract base (bodies
`ABSL_CHECK(false) << "stub until M3"`).  M3 keeps the abstract
name and adds the two concrete subclasses you asked for:

```cpp
// abstract — shipped M2, stays.
class HostMapBacking {
 public:
  virtual ~HostMapBacking() = default;
  virtual uint32_t Size() const = 0;
  virtual std::optional<Value> Get(const Value& key) const = 0;
  virtual bool ContainsKey(const Value& key) const = 0;
  virtual void ForEach(
      absl::FunctionRef<void(const Value&, const Value&)>) const = 0;
};

// new — the "HostMap" concrete.  Vector-backed; used both for
// Activation::Bind(Value::Map(...)) user bindings AND as the
// runtime's host-side representation of kHost maps.  (Arena
// literals stay in wasm memory; they never reach this class.)
class HostMap final : public HostMapBacking {
  std::vector<std::pair<Value, Value>> entries_;
  /* Size / Get / ContainsKey / ForEach implemented here */
};

// new — the "proto map" concrete.  Reflection-backed view over
// a proto map field.  Held via the ExternrefTable when
// ProtoBacking::ReadField returns a map field.
class ProtoMap final : public HostMapBacking {
  const google::protobuf::Reflection* absl_nonnull refl_;
  const google::protobuf::FieldDescriptor* absl_nonnull field_;
  const google::protobuf::Message* absl_nonnull owner_;
  /* implementations read map entries via
     Reflection::GetRepeatedMessage + the synthetic `key`/`value`
     sub-field descriptors on the map entry type */
};
```

One abstract + two concretes.  The CelValue kind stays single
(`CEL_MAP_HOST`); codegen doesn't branch on which concrete
impl — Layer 2 does a virtual call and the impl dispatches.

No `VectorMapBacking` / `ArenaMapBacking` / etc. proliferation.

### 2.4 `api/internal/cel_host.{h,cc}` — Layer 2 body

One new Impl for M3 scope:

```cpp
ABSL_MUST_USE_RESULT absl::Status CelMapLookupImpl(
    uint32_t out_slot,
    uint32_t map_slot,
    uint32_t key_slot,
    TrampolineContext& ctx);
```

Reads ref_slot from `map_slot`, dereferences through
`ExternrefTable` to a `HostMapBacking*`, decodes `key_slot`
into a `cel::Value`, calls `backing->Get(key)`, encodes the
result back into `out_slot`.  One virtual call on the hot path;
one host memcpy for string keys/values (dispatch-doc §10 open
question 5 is the future-work mitigation).

M3 does **not** ship `CelMapSizeImpl` / `CelMapContainsImpl` /
`CelMapCreateImpl` / `CelMapInsertImpl` — creation + growth
happen arena-side in the runtime module; `size` / `in` ship
with the kCall overload set next milestone.

### 2.5 `api/internal/cel_host_wasmtime.{h,cc}`

One new trampoline registration: `cel_host.cel_map_lookup`.
Uses the same `HostFieldTrampoline<Impl>` template M2 uses for
`cel_get_field` / `cel_has_field`.  No new machinery above that.

wasmtime config update in `wasmtime_engine_state.cc::Initialize`:

```cpp
wasmtime_config_wasm_tail_call_set(config_.get(), true);
```

### 2.6 `api/value.{h,cc}` — `Value::Map` body filled in

M2 shipped the stub.  M3:

```cpp
// value.cc
Value Value::Map(std::vector<std::pair<Value, Value>> entries) {
  return Value::HostMap(std::make_shared<HostMap>(std::move(entries)));
}

Value Value::HostMap(std::shared_ptr<HostMapBacking> backing) {
  Value v;
  v.kind_ = Kind::kMap;
  v.payload_ = std::move(backing);
  return v;
}
```

`StructurallyEquals` gains the kMap arm: unordered-pairs
element-wise compare per langdef § map equality.

### 2.7 `codegen/resolve_pass.{h,cc}` — origin inference (map-only)

New visitor populates `NodeAnnotation.map_origin` bottom-up.
M3 table (`list_origin` changes deferred until the lists slice):

| node | `map_origin` | source |
|---|---|---|
| `kCreateMap` | `kArena` | constructed in the arena by `cel_map_create` |
| `kSelect` on map-typed field | `kHost` | `ProtoBacking::ReadField` returns `Value::HostMap(ProtoMap{…})` |
| `kIdent` declared `map<K,V>` | `kHost` | `Activation::Bind(Value::Map)` → interned via `cel_refs` |
| `kCall` returning map (M5+ comprehensions) | `kHost` (stub path; check not exercised at M3) | deferred |
| `?:` / `\|\|` / `&&` over map branches | both agree → that origin; else `kDynamic` | dispatch-doc §2.1 |
| anything else with `repr == kMap` | `kDynamic` | safe default |

M2 already populated `map_origin = kHost` on `kSelect` / `kIdent`
(stub path); M3 extends to `kCreateMap` → `kArena` + the `?:`
coalescing rule.

### 2.8 `codegen/layout_pass.{h,cc}` — slots for map ops

  - `kCreateMap` gets a result slot at `workspace_base + n*24`.
  - Per-entry scratch slots (key + value) reused after each
    `cel_map_insert`.
  - `kCallExpr(_[_])` on a map operand gets a result slot;
    operand's slot + key's slot are the call inputs.

### 2.9 `codegen/expr_lower.{h,cc}` — two new arms + tail-call-aware dispatch

**`kCreateMap` arm.**  Unconditional kArena — literals always
build in the arena:

```wat
(call $cel.cel_map_create (local.get $out_slot) (i32.const <N>))
(loop over entries:
  <lower key expression into scratch slot K>
  <lower value expression into scratch slot V>
  (call $cel.cel_map_insert (local.get $out_slot) (local.get K) (local.get V)))
```

**`kCallExpr(_[_])` arm (narrow).**  Dispatches on operand's
`map_origin`:

| `operand.map_origin` | emitted call |
|---|---|
| `kArena` | `call $cel.cel_map_lookup_arena` |
| `kHost` | `call $cel_host.cel_map_lookup` |
| `kDynamic` | `call $cel.cel_map_lookup` — the dispatcher |

The full kCall arm that ships next milestone subsumes this;
the narrow arm is literally a switch on function name `_[_]`
with operand `repr == kMap`.

### 2.10 `abi/cel_abi.proto` + `cel_abi_emit.{h,cc}`

**No changes.**  Map literals don't need a ctor table (unlike
proto literals, which will).  `variables[]` / `fields[]` /
`attributes[]` stay as M2 shipped them.

## 3. Source layout (M3 deliverables)

```
compiler_v2/
├── runtime/
│   ├── cel_data.h                       BREAKING: CEL_MAP split +
│   │                                    ArenaMapHeader
│   ├── cel_data_test.cc                 + size asserts
│   ├── cel_runtime.h                    + cel_map_create/insert/
│   │                                    lookup/lookup_arena exports
│   ├── cel_runtime.c                    + arena map primitives +
│   │                                    tail-call dispatcher +
│   │                                    cel_host.cel_map_lookup import
│   ├── cel_runtime_test.cc              + cel_map_* unit coverage
│   ├── wasm_imports.txt                 + cel_host.cel_map_lookup
│   └── BUILD.bazel                      genrule +=  -mtail-call
├── codegen/
│   ├── expr_lower.{cc,_test.cc}         + kCreateMap arm +
│   │                                    kCall(_[_]) × 3 origin arms
│   ├── resolve_pass.{cc,_test.cc}       + MapOriginVisitor
│   ├── layout_pass.{cc,_test.cc}        + slot assignment for
│   │                                    kCreateMap + kCall on maps
│   └── runtime_link_test.cc             + expected exports grow +
│                                    validate module with tail-call
│                                    feature enabled
├── api/
│   ├── value.{cc,_test.cc}              Value::Map / Value::HostMap
│   │                                    bodies; StructurallyEquals kMap
│   └── internal/
│       ├── cel_host.{h,cc,_test.cc}     + HostMap + ProtoMap +
│       │                                    CelMapLookupImpl
│       ├── cel_host_wasmtime.{h,cc}     + cel_map_lookup trampoline
│       └── wasmtime_engine_state.cc     + tail-call feature enabled
├── ir/
│   └── annotations.h                    (map_origin field already ships;
│                                        M3 just writes new non-sentinel
│                                        values)
└── e2e/
    └── m3_test.cc                       NEW — map e2e suite
```

New WAT traces (per CLAUDE.md WAT-first rule):

  - `06_map_literal.wat` — `{"a":1, "b":2}` (kArena).
  - `07_map_index_arena.wat` — `{"a":1}["a"]` (direct
    `call $cel.cel_map_lookup_arena`).
  - `08_map_index_host.wat` — `m["a"]` on bound map (direct
    `call $cel_host.cel_map_lookup`).
  - `09_map_index_dynamic.wat` — `(cond ? {"a":1} : m)["a"]`
    (call into dispatcher; dispatcher emits `return_call` to
    either arena or host arm).
  - `10_proto_map_field.wat` — `customer.metadata["env"]`
    (kSelect on map field returns `Value::HostMap(ProtoMap{…})`;
    kCall(_[_]) emits `call $cel_host.cel_map_lookup`).

Each gets a walkthrough in `wat-traces.md`.

## 4. What gets ported verbatim from v1

Nothing.  v1 never shipped map dispatch codegen.  Portions
reused:

  - The CelArray / CelMap payload slots in `cel_data.h`
    (renamed / extended in-place).
  - The three-layer cel_host pattern (M2.C).
  - The existing `HostMapBacking` abstract base signature (M2
    stub).

## 5. Work breakdown (order of authoring)

Each slice: WAT → assemble + `wat_runner` (with stubs where
Layer 3 hasn't landed yet) → unit tests at every pipeline stage
touched → e2e through `Instance::Eval` → milestone doc progress
log updated.

1. **M3.A — `cel_data.h` ABI split.**  `CEL_MAP` →
   `CEL_MAP_ARENA` + `CEL_MAP_HOST`; `CelValue.payload` grows;
   `ArenaMapHeader` struct.  All consumers update in-commit.
   Static asserts on sizes.  No functional change yet (the old
   `CEL_MAP` kind was stubbed everywhere).
2. **M3.B — runtime arena primitives.**  `cel_map_create` /
   `cel_map_insert` / `cel_map_grow` / `cel_map_lookup_arena`
   in `cel_runtime.c`.  `cel_runtime_test.cc` round-trips each
   scalar key kind + cross-type numeric equality ladder +
   duplicate-key poison + out-of-capacity growth.  Enable
   `-mtail-call` flag on the genrule in preparation for M3.C.
3. **M3.C — tail-call dispatcher.**  `cel_map_lookup` with
   `__attribute__((musttail))` arms + extern import decl for
   `cel_host.cel_map_lookup`.  Enable wasmtime tail-call feature
   in `wasmtime_engine_state.cc::Initialize`.  Grow Binaryen
   feature set.  `runtime_link_test.cc` expected exports grow;
   module-validator now enforces tail-call feature.
   `wasm_imports.txt` grows by one.
4. **M3.D — concrete backings + `Value::Map` body.**  `HostMap`
   + `ProtoMap` concretes land in `cel_host.{h,cc}`.
   `Value::Map` / `Value::HostMap` bodies filled in.
   `StructurallyEquals` kMap arm (unordered-pairs).
   `value_test` + `cel_host_test` extended.  No codegen yet.
5. **M3.E — Layer 2 + Layer 3 wasmtime glue.**
   `CelMapLookupImpl` in `cel_host.cc`; Layer-3 trampoline
   registration in `cel_host_wasmtime.cc`.  `cel_host_test.cc`
   covers both concrete backings against the fake externref
   table.
6. **M3.F — resolve + layout + codegen.**
   `resolve_pass.cc` `MapOriginVisitor` (kCreateMap → kArena;
   kSelect/kIdent map-typed → kHost; mixed ?: → kDynamic);
   `layout_pass.cc` slot allocation; `expr_lower.cc` kCreateMap
   arm + kCallExpr(_[_]) × 3 origin arms.  WAT 06 / 07 / 08 / 09
   assembled and disassembly-matched.  E2E: literal map, bound
   map, conditional-origin map, all three operand types.
7. **M3.G — `ProtoBacking::ReadField` on MAP fields.**  Returns
   `Value::HostMap(std::make_shared<ProtoMap>(reflection, field,
   owner))`.  Flips the M2 envelope for the map-field case
   (list/REPEATED still errors).  Adds
   `SelectMapFieldReturnsHostMap` e2e counterpart to the
   existing `SelectRepeatedFieldReturnsUnsupportedError`.  WAT
   10.  Requires the M2 testdata `Customer` to grow a
   `map<string, string> metadata` field (small fixture change).
8. **M3.H — conformance harness envelope.**  `IsInM2Envelope`
   → `IsInM3Envelope`.  Admit `map_value` matcher;
   `CompareValue::CompareMap` (order-agnostic, langdef
   semantics).  Re-run `run_conformance`; update
   `conformance/README.md` inventory + testing-checklist.md
   "Rewrite M3" rows.
9. **M3.I — reconcile map-list-dispatch.md §11 (map rows).**
   Fold the map-specific bullets into `design.md §4.7.2` / §5 /
   §7.2 / §8.1.  List rows in §11 stay unticked (next
   milestone's job).  `map-list-dispatch.md` header flips to
   "maps reconciled into design.md <date>; lists still pending."

Each slice leaves the test suite green; each can be reverted
independently.

## 6. Test plan

### 6.1 Unit (per file)

  - `cel_data_test` — `CelKind` renumber layout; `ArenaMapHeader`
    size (16 B); CelValue payload union size (stays 24 B).
  - `cel_runtime_test` (extended) — `cel_map_create` /
    `cel_map_insert` / `cel_map_lookup_arena` round-trip for
    each scalar key kind; duplicate-key poisons the map;
    linear-scan finds entries; cross-type numeric key equality
    per langdef; `cel_map_grow` fires when capacity exceeded.
    **Tail-call test**: a unit module that calls
    `cel_map_lookup` in a loop N times verifies the wasm stack
    doesn't grow (observable via a stack-pointer export).
  - `value_test` (extended) — `Value::Map(entries)` round-trip;
    `StructurallyEquals` × ordered / unordered / mismatched-
    kind; `Value::HostMap(backing)` preserves backing identity.
  - `cel_host_test` (extended) — `CelMapLookupImpl` against
    `HostMap` and `ProtoMap` via the fake externref table;
    `ProtoMap::Get` on a `Customer.metadata` fixture;
    missing-key path returns `CEL_ERR_NO_SUCH_KEY`.
  - `expr_lower_test` (extended) — `kCreateMap` emits WAT-06
    byte-for-byte; `kCallExpr(_[_])` × 3 origin arms each
    matches WAT 07 / 08 / 09 disassembly (modulo Binaryen-
    assigned names).
  - `resolve_pass_test` (extended) — origin-inference table
    from §2.7 locked per node kind; `?:` coalescing rule.
  - `layout_pass_test` (extended) — kCreateMap result + per-
    entry scratch slots at expected offsets; scratch reuse.

### 6.2 E2E (`compiler_v2/e2e/m3_test.cc` — new)

  - Map literal × each scalar key/value kind (null/bool/int/
    uint/double/string/bytes combinations that are spec-legal
    as keys — bool/int/uint/string; value can be anything).
  - Bound map indexing: `m["k"]` over `Activation::Bind("m",
    Value::Map({…}))`.
  - Proto map field: `customer.metadata["env"]`.
  - Conditional-origin (kDynamic): `(cond ? {"a":1} : m)["a"]`
    — hits the dispatcher → `return_call` into correct arm;
    test both branches.
  - Boundaries: missing key → `CEL_ERR_NO_SUCH_KEY`; duplicate
    key at construction → poisons to `CEL_ERROR`.
  - Cross-type numeric keys: `{1: "a"}[1u]` → `"a"`; `{1u:
    "a"}[1.0]` → `"a"`.
  - PartialEval × map indexing with unknown key operand →
    `Value::Unknown(attribute_id)`.

### 6.3 Conformance unlock

  - `IsInM3Envelope` admits `value:` matchers with
    `map_value` kind.
  - `CompareValue::CompareMap` — element-wise, order-agnostic,
    recursive.
  - Expected PASSes: the map-indexing rows in `fields.textproto`
    (9+ of them); any fixture-wide test that only needs
    map-indexing + scalars to succeed.  Rough ceiling: +20–50
    PASSes.

### 6.4 Testing-checklist rows

Flip on `testing-checklist.md §"Rewrite M3"`:

  - [ ] `kCreateMap` × each scalar key/value combo
  - [ ] `kCallExpr(_[_])` on map × kArena
  - [ ] `kCallExpr(_[_])` on map × kHost
  - [ ] `kCallExpr(_[_])` on map × kDynamic (tail-call dispatcher)
  - [ ] Proto map field read via `ProtoMap`
  - [ ] Missing-key / duplicate-key error paths
  - [ ] Cross-type numeric key equality
  - [ ] `Value::Map` round-trip through `Activation::Bind`
  - [ ] `CelKind` split: all old `CEL_MAP` consumers updated
  - [ ] Tail-call feature enabled end-to-end (wasmtime config +
        Binaryen opts + clang genrule flag)

## 7. Exit criteria

  - [ ] `bazel test //compiler_v2/...` green.
  - [ ] `bazel run //compiler_v2/conformance:run_conformance`
        shows no `kFail` regressions vs the post-M2 +
        binding-marshaller snapshot.  New PASSes appear in
        map-indexing rows.
  - [ ] `conformance/README.md` inventory refreshed.
  - [ ] `testing-checklist.md §"Rewrite M3"` rows all ticked.
  - [ ] This doc flips to `Status: shipped <date>` with a
        one-paragraph "what landed" summary.
  - [ ] `map-list-dispatch.md §11` map bullets ticked into
        `design.md`; list bullets stay open for the next
        milestone.
  - [ ] `design.md §10.2` invariants re-verified (no new
        `NodeAnnotation` fields; codegen stays oblivious to
        partial eval; `cel_reset` semantics survive).
  - [ ] `cel-host-surface.md §3` reconciled with `HostMap` +
        `ProtoMap` concretes + `CelMapLookupImpl`.
  - [ ] WAT walkthroughs 06–10 in `wat-traces.md`; byte-level
        equivalence in `wat_runner_test.cc`.

## 8. Risk register (M3-specific)

  - **Breaking `CelKind` ABI renumber.**  Every in-flight
    consumer (value.cc, cel_host.cc, checker Repr mapping,
    CLI decoders, benches) updates in the M3.A commit.
    **Mitigation:** grep for every `CEL_MAP` reference before
    the renumber; atomic commit; full `bazel test` before
    merging.
  - **Tail-call support gap.**  wasmtime supports tail calls
    since v11; Binaryen supports them with `--enable-tail-call`;
    clang emits `return_call` with `-mtail-call`.  If any of
    those fail, the dispatcher body grows the wasm stack
    linearly with call count — correctness survives (just
    slower, possibly stack-overflow on pathological cases).
    **Mitigation:** the tail-call unit test (§6.1) observes
    the wasm SP explicitly; fails loudly if TCO doesn't stick.
    Fallback plan: if tail calls don't pan out, the dispatcher
    gets a manual trampoline (no worse than today's M2 cost
    for select chains).
  - **`__attribute__((musttail))` compilation.**  The attribute
    is clang-only; our wasm build already uses clang.  If the
    attribute can't be placed on a return-of-imported-function
    call (the `cel_host_cel_map_lookup` arm), we have three
    fallbacks: (1) wrap the import in a local thunk and tail-
    call that; (2) write the dispatcher in hand-rolled WAT
    under `doc/…/wat/`; (3) drop tail-call on the kHost arm
    and accept the frame cost (host imports trap on entry
    anyway — the stack cost is dwarfed).
  - **Arena reclamation for bound aggregates.**  User holds a
    `Value::HostMap` or reads `customer.metadata` via
    `ProtoMap`; on next Eval, `cel_reset` rewinds the arena
    but neither backing lives there — `HostMap` is host-side,
    `ProtoMap` views the Customer's live memory.  Still the
    M2 invariant holds: caller keeps the backing alive for
    the Value's lifetime.  **Mitigation:** document in
    `value.h`; add `value_test::HostMapBackingOutlivesNextEval`
    lock.
  - **Map-key cross-type equality ladder.**  Langdef mandates
    `1 == 1u == 1.0` on map keys.  The comparator in
    `cel_runtime.c` must traverse numeric coercion.
    **Mitigation:** unit-test against every pair in the
    langdef ladder before the lookup arm lands.
  - **Descriptor pool identity across Plan + Instance.**
    `ProtoMap` holds a raw `const FieldDescriptor*`.  Same
    mitigation as M2's `fields[]` — the shared_ptr-to-pool
    capture extends naturally.
  - **Binaryen opt passes vs tail calls.**  Some opts (notably
    `--inlining`) can unfold tail calls into regular calls if
    mishandled.  **Mitigation:** pin the opt pass list
    explicitly in `module.cc`; exclude passes known to disturb
    tail-call shape until verified.  `runtime_link_test.cc`
    disassembles the output and asserts `return_call`
    presence.

## 9. After M3

  - **Next milestone (lists).**  Replay the M3 pattern for
    lists: `CEL_LIST_ARENA` / `CEL_LIST_HOST` split;
    `ArenaListHeader`; `HostList` + `ProtoList` concretes;
    three-path dispatch for `_[_]` on lists.  Envelope boundary
    flip for REPEATED fields.  Near-copy of M3 with list
    semantics substituted.
  - **kCall + built-in overload set.**  Arithmetic, comparison,
    string ops.  Also: `size(m)`, `k in m`, `m1 == m2`,
    `m1 + m2` (map merge) — all reuse M3's three-path
    dispatch per dispatch-doc §6.  Full `OverloadTable`
    population.
  - **Proto literals.**  `cel_host.cel_make_message` trampoline;
    `cel.abi.message_ctors[]`; `kCreateStruct` codegen arm.
  - **M5** — comprehensions + customs + 3VL + error surface.
  - **Beyond** — perf (hash-map key lookup past the linear-scan
    threshold, externref zero-copy for bound aggregates), proto
    wrappers, enums, timestamps.

Each gets its own `m<n>-*.md` plan doc.
