# Rewrite M4 — list literals + indexing (replays the M3 map shape)

Status: **shipped 2026-04-25.**

Summary of what landed (vs the as-written plan): every slice A–J
shipped under the as-written shape with one delta — the runtime
construction primitives became `cel_list_create(out, count)` +
`cel_list_set(list, index, elem)` rather than the planned
`create / append / grow` triple, since codegen always knows the
element count at lowering time (see the plan-vs-execution callout
below for the rationale).  The M4.A–E + G slices shipped first as
runtime + ABI; M4.F (codegen) + M4.H (Eval marshal/decoder) +
M4.I (conformance envelope) + M4.J (m4_test.cc) followed and
landed the e2e flows.  Conformance moved from 203 → 212 PASSes
(`lists.textproto` 0 → 4, `parse.textproto` 148 → 150,
`fields.textproto` 13 → 14).  One unplanned change: ResolvePass
now returns `Unimplemented` on any AST that contains a
`kComprehensionExpr` rather than crashing in the per-name Repr-
agreement CHECK — the M5 scope handler will replace that gate.

> Plan-vs-execution delta (2026-04-25): the runtime construction
> primitives diverged from the plan's `cel_list_create` +
> `cel_list_append` + `cel_list_grow` triple.  Per direct user
> direction ("the list is going to be of fixed length / we know what
> the list size is / we should have a way to set an element at an
> index / no grow"), the shipped surface is the simpler
> `cel_list_create(out, count)` (zero-fills `count` element slots up
> front) plus `cel_list_set(list, index, elem)` for codegen to write
> each known position.  No append, no grow.  Past-count `set` poisons
> with `CEL_ERR_OVERFLOW` — same shape as the map literal's
> past-capacity insert.  Comprehensions in M5 will need either
> `cel_list_clear` / `cel_list_set` over a pre-sized accumulator or
> a separate dynamic-list primitive; the M5 plan picks one.

## Slice progress (as of 2026-04-25)

| slice | status | notes |
|---|---|---|
| **M4.A** — `cel_data.h` ABI split | shipped | `CEL_LIST_ARENA = 7` (kept the slot — minimal-churn option from §8 risk #2), `CEL_LIST_HOST = 17`, `ArenaListHeader` + `ArenaListRef` payload, `kCelListEntryStride = 24`, `CEL_ERR_INDEX_OUT_OF_BOUNDS = 17`.  Static asserts pinned via `cel_data_test`. |
| **M4.B** — runtime arena primitives | shipped | New `cel_list.h` + `cel_list_create` / `cel_list_set` / `cel_list_at_arena` in `cel_runtime.c`.  `cel_list_test.cc` covers per-kind round-trip, OOB / negative / non-int index, set-past-count poisons, set-duplicate-index overwrites, ForEach, dispatcher routing. |
| **M4.C** — kDynamic dispatcher | shipped | `cel_list_at` with `__attribute__((musttail))` arms; `cel_host_cel_list_at` extern import; `wasm_imports.txt` += `cel_host_cel_list_at`; `compile.cc::InstallHostAbi` registers all five list imports.  `InstallHostAbi` was split into `InstallSelectImports`/`InstallMapImports`/`InstallListImports` to clear the function-size lint gate. |
| **M4.D** — host backings + Value::List | shipped | `HostList` (vector-backed) + `ProtoList` (proto reflection); `Value::List`/`Value::HostList`/`ListBacking`/`SharedListBacking` bodies in `cel_host.cc` (one-way dep matches `Value::Map`); `StructurallyEquals` kList arm = pointer-identity.  New `host_list_test.cc` + `proto_list_test.cc`.  `host_fixture_proto3.proto` extended with `rep_s/rep_b/rep_f64/rep_msg`. |
| **M4.E** — Layer 2 + Layer 3 wasmtime glue | shipped | `CelListAtImpl` Layer-2 trampoline body in `cel_host.cc`; `EncodeFieldResult` + `EncodeAggregateIfAny` factored to handle every aggregate kind (message/map/list) uniformly via interning into the matching externref namespace.  `HostExternrefTable::InternList`/`LookupList`.  `HostThreeArgTrampoline<Impl>` template extracted in `cel_host_wasmtime.cc` (shared with `CelMapLookupTrampoline`); `RegisterCelHostImports` adds `cel_list_at`.  New `cel_list_at_impl_test.cc` (15 tests).  Test fakes deduplicated into shared `cel_host_test_fakes.h` — previously each Layer-2 test re-implemented `FakeMemoryView` / `FakeExternrefTable` / `FakeArenaAllocator` and drifted as new namespaces (M3 maps, M4 lists) landed; now centralised. |
| **M4.F** — resolve + layout + codegen | shipped | `ListOriginVisitor` in `resolve_pass.cc` mirrors `MapOriginVisitor` (kListExpr → kArena, kIdent/kSelect with `Repr::kList` → kHost).  `MapStorageVisitor` generalised to `AggregateStorageVisitor` with a new `PostVisitList` arm (one workspace slot per kListExpr; element scratch slots released after `cel_list_set` consumes them).  `expr_lower.cc` gained `EmitKListExpr` (`cel_list_create` + per-element `cel_list_set`) and `ListAtCallTarget`; the `kCallExpr(_[_])` arm now dispatches on operand `repr` (kMap → MapLookupCallTarget, kList → ListAtCallTarget).  WAT 11–14 deferred (M3 maps shipped without WAT traces too — the byte-shape lock landed via `expr_lower_test`'s `BinaryenCallGetTarget` assertions).  An unplanned ResolvePass change: `kComprehensionExpr`-bearing programs return `Unimplemented` here so the conformance binary classifies them as SKIP rather than tripping the per-name Repr-agreement CHECK on cel-cpp's macro-expanded `@result` ident. |
| **M4.G** — `ProtoBacking::ReadField` REPEATED flip | shipped | REPEATED → `Value::HostList(ProtoList{...})` (was `kTypeUnsupported`).  Two existing tests flipped to assert HostList; m2 envelope test re-targeted at M4.F+H (e2e Eval needs codegen + decoder). |
| **M4.H** — activation marshaller + Eval decoder | shipped | `EncodeList` arm in `EncodeScalarValue` (interns `Value::List` / `Value::HostList` via `ExternrefTable::InternList` and writes `{CEL_LIST_HOST, payload.ref_slot}`).  `DecodeArenaListAt` reads `ArenaListHeader` + walks `count × 24B` and recursively decodes via `DecodeCelValueAt`; new `CEL_LIST_ARENA` arm in the top-level decoder.  `instance` build dep on `cel_host` added so `ExternrefTable::InternList` resolves.  m2_test's `SelectRepeatedFieldReturnsHostList` flipped from SKIP to a green `customer.tags[0] == "tag0"` assertion. |
| **M4.I** — conformance harness envelope | shipped | `IsInM3Envelope` → `IsInM4Envelope`; `IsAggregateMatcherKindForM3` → `IsAggregateMatcherKindForM4` admitting `kListValue`.  New `CompareList` mirrors `CompareMap` but is order-aware (lists are ordered per langdef § "List equality").  `CompareValue` factored: scalar arm extracted into a `CompareScalar` helper so the dispatcher stays under the function-size lint gate after the kListValue arm landed.  Conformance: 203 → 212 PASSes (`lists.textproto` 0 → 4 first PASS, `parse.textproto` 148 → 150, `fields.textproto` 13 → 14). |
| **M4.J** — m4_test.cc + doc reconcile | shipped | New `compiler_v2/e2e/m4_test.cc` (16 tests across `ListLiteralE2ETest`, `ProtoRepeatedE2ETest`, `ProtoRepeatedHostMsg3E2ETest`).  `Customer` proto fixture extended with `repeated string tags = 12`.  `scripts/run_full_suite.sh` MANUAL_TARGETS += `//compiler_v2/e2e:m4_test`.  This doc's status flipped to shipped + per-slice notes filled in. |

**Manual targets that gate close (per §6.4 / `per-component-test-coverage.md §5`):**
all 6 currently green — `cel_host_test`, `engine_test`, `instance_test`,
`m2_test` (one single-test SKIP for `SelectRepeatedFieldReturnsHostList`,
unblocks at M4.F+H), `cel_runtime_wasm_test`, `wat_runner_test`.

The runtime-binding sites that needed list-aware updates (not in the
original plan but surfaced during execution): `engine.cc::InstantiateRuntime`'s
`BindRuntimeExport` loop (added `cel_list_create` / `cel_list_set` /
`cel_list_at_arena` / `cel_list_at`), `cel_runtime_wasm_test.cc` linker
setup (no-op `cel_host.cel_list_at`), and `wat_runner.cc` linker setup
(same).  Codegen never decides to omit these imports based on AST
shape — the "always link the runtime fully" rule from CLAUDE.md
applies to the host runtime+linker side too.

---

(Original plan continues unchanged below.)

Parent: `design.md`.  Predecessors: `m3-map-literals.md` (shipped
2026-04-24, the maps half of the three-path dispatch contract);
`m2-ident-select-unknowns.md` (shipped 2026-04-25, kSelect / has /
PartialEval lit up end-to-end).  Authoritative design for list
dispatch: **`map-list-dispatch.md` §4.2 / §6 / §7 (list rows)** —
this doc implements the list half of that design and ticks the
remaining list bullets in `map-list-dispatch.md §11`.

**Scope rename note.**  What `m1-scalar-pipeline.md §10` originally
called "M6 — list + map literals" got split: M3 shipped maps; M4
ships lists.  The "kCall built-in overload set" (`size` / `in` /
`==` / `+` / arithmetic / comparisons) — originally numbered M3 in
the early plan — moves to M5, since the three-path dispatch they
reuse is now proven by M3 and M4.  (Comprehension lowering, once
bundled with M5, was further split into a follow-on milestone after
M5; see `m5-kcall-comprehensions.md`.)

## 0. Why lists now

  - **The dispatch design is locked.**  M3 proved out the three-
    path contract end-to-end (kArena fast path, kHost vtable
    path, kDynamic tail-call dispatcher with `__attribute__
    ((musttail))`).  Every M4 surface is a copy-paste of an M3
    surface with `map` renamed to `list` and the entry stride
    halved (24 B not 48 B).  Doing lists immediately after maps
    means the design rationale is still cached and the
    invariants stay aligned.
  - **The remaining M2 envelope boundary flips here.**
    `m2_test.cc::EnvelopeBoundaryE2ETest::SelectRepeatedFieldReturnsUnsupportedError`
    is currently `GTEST_SKIP`ped (left over from earlier
    envelope shifts); M4 either re-enables it as a green test
    that expects `Value::HostList(ProtoList{…})` (envelope
    flipped) or replaces it with an `_at_index` e2e test.  No
    silent skips after M4 closes (per
    `per-component-test-coverage.md` SKIP discipline rule).
  - **Conformance unlock.**  `lists.textproto` (39 tests) is
    fully gated on M4 — list literals + indexing.  Most of
    `proto2.textproto` / `proto3.textproto` `repeated_*` rows
    additionally graduate.  Rough ceiling: +60-80 PASSes once
    `IsInM4Envelope` admits `list_value:` matchers and
    `CompareValue::CompareList` lands.
  - **M5 is blocked on lists.**  Comprehensions
    (`x.exists(e, …)`, `x.map(e, …)`, `x.filter(e, …)`) iterate
    lists and accumulate into lists.  Without `cel_list_create`
    / `cel_list_append` on the runtime side, M5 can't ship.

## 1. Scope

### 1.1 What works end-to-end after M4

```cpp
auto compiler = *cel::Compiler::NewBuilder()
    .DeclareVariable("xs", CelType::List(CelType::Int()))
    .RegisterMessageType(Customer::descriptor())
    .Build();
```

Compiles + evaluates:

  - `[1, 2, 3]` — list literal (kArena).
  - `[1, 2, 3][1]` — literal + indexing → `2`.
  - `xs[0]` — indexing into bound list (kHost).
  - `customer.tags[2]` — proto repeated field via
    `ProtoList` backing (kHost).
  - `cond ? [1, 2] : xs` then `[.][...]` — mixed-branch
    conditional (kDynamic); runtime dispatcher routes.
  - `[…][i]` against `Activation::Bind("xs",
    Value::List({…}))` — round-trips through the
    activation-marshal kList encoder + the kHost `cel_list_at`
    trampoline + the host-side decoder.

### 1.2 Out of scope (deferred)

  - **`size(list)` / `x in list` / `list1 == list2` /
    `list1 + list2`** — ship with the M5 kCall built-in
    overload set.  All reuse M4's three-path origin dispatch.
  - **Comprehensions** — M5.  M4 ships
    `cel_list_create` / `cel_list_append` runtime exports so
    M5 can use them as comprehension side-effects without
    waiting for another milestone.
  - **Hash-table-style lookup for large arena maps**
    (carried forward from M3) — same status; no work in M4.
  - **List literals with non-uniform element types**
    (`[1, "a"]` — homogeneous-required at the type-checker per
    langdef).  The static subset rejects these; M4 doesn't
    relax that.
  - **String/bytes activation marshalling for kIdent** — still
    pending the host-arena-allocator work that M2 left open.
    `Activation::Bind("s", Value::String("..."))` continues to
    fail with `Unimplemented`; lists of strings as host
    bindings are gated on the same host-arena fix.

### 1.3 Envelope boundary probes

  - `[1, 2, 3][3]` → `CelValue{kind:CEL_ERROR,
    err:CEL_ERR_INDEX_OUT_OF_BOUNDS}` (new wire code; mirrors
    the M3 `CEL_ERR_NO_SUCH_KEY` story for maps).
  - `[1, 2, 3][-1]` → langdef-defined behaviour: per
    `doc/langdef.md`, list indices are `int` and negative
    indices are an error (not Python-style wrap-around).
    Lock with a test.
  - `[1u][1]` → cross-type numeric index: per langdef
    "Indexing", numeric indices on lists are `int` only —
    `uint` is a type-check error at the static-subset gate.
    Locked with a `RejectDyn` test.
  - `m3_test.cc::EnvelopeBoundaryE2ETest::SelectMapFieldReturnsHostMap`
    stays green; a new
    `SelectRepeatedFieldReturnsHostList` lands as its list
    counterpart.

## 2. Surfaces introduced in M4

All specified by `map-list-dispatch.md`; this is the landing
manifest for the list subset.

### 2.1 `runtime/cel_data.h` — `CelKind` split + `ArenaListHeader`

  - **`CelKind` renumber** — split the existing single
    `CEL_LIST = 7` into `CEL_LIST_ARENA = ?` and
    `CEL_LIST_HOST = ?`.  The numeric assignments depend on
    where M3 left the kind-enum holes; pick contiguous values
    after `CEL_MAP_HOST = 9` (i.e. `CEL_LIST_ARENA = 10`,
    `CEL_LIST_HOST = 11`) and renumber `CEL_MESSAGE` /
    `CEL_DURATION` / `CEL_TIMESTAMP` / `CEL_UNKNOWN` /
    `CEL_ERROR` accordingly.  Breaking ABI change; all
    consumers update in the M4.A commit.
  - **`CelValue.payload` union** — add `arena_list.header_ptr`
    (u32 offset).  `ref_slot` (already used by
    `CEL_MAP_HOST` / `CEL_MESSAGE`) is reused for
    `CEL_LIST_HOST`.  Slot stays 24 B.
  - **`ArenaListHeader`** struct (`map-list-dispatch.md §4.2`):
    `{count, capacity, elements_offset, _pad}` = 16 B.
    Elements stride 24 B (single CelValue back-to-back).
  - **New error codes** in the `CEL_ERR_*` enum:
      - `CEL_ERR_INDEX_OUT_OF_BOUNDS = 17` — list index
        outside `[0, count)`.  Mirrors `cel::ErrorCode`
        addition in `api/error.h`.
  - **Static asserts**: `sizeof(ArenaListHeader) == 16`,
    `sizeof(CelValue) == 24` (unchanged), `kCelListEntryStride
    == 24`.

### 2.2 `runtime/cel_runtime.{h,c}` — arena primitives + tail-call dispatcher

Per `map-list-dispatch.md §3 + §5`:

**Construction primitives** (called from codegen's
`kCreateList`):

```c
void cel_list_create(uint32_t out_slot, uint32_t initial_capacity);
void cel_list_append(uint32_t list_slot, uint32_t elem_slot);
// internal; called from cel_list_append on capacity miss
void cel_list_grow(uint32_t list_slot);
```

**Arena fast path** (called directly when codegen proved
`operand.list_origin == kArena`):

```c
void cel_list_at_arena(uint32_t out_slot,
                       uint32_t list_slot,
                       uint32_t index_slot);
```

Pure wasm; no host trip.  Reads `ArenaListHeader.count`,
checks `0 <= index < count`, copies element CelValue via
`memcpy`.  Out-of-bounds writes
`{CEL_ERROR, payload.err = CEL_ERR_INDEX_OUT_OF_BOUNDS}`.

**kDynamic dispatcher** (called when operand origin is
`kDynamic`):

```c
extern void cel_host_cel_list_at(uint32_t out, uint32_t l, uint32_t i)
    __attribute__((import_module("cel_host"),
                   import_name("cel_list_at")));

void cel_list_at(uint32_t out_slot, uint32_t list_slot,
                 uint32_t index_slot) {
  CelValue* l = cel_value_at(list_slot);

  // 3VL absorption — same path regardless of origin.
  if (l->kind == CEL_UNKNOWN || l->kind == CEL_ERROR) {
    *cel_value_at(out_slot) = *l;
    return;
  }

  if (l->kind == CEL_LIST_ARENA) {
    __attribute__((musttail))
    return cel_list_at_arena(out_slot, list_slot, index_slot);
  }
  if (l->kind == CEL_LIST_HOST) {
    __attribute__((musttail))
    return cel_host_cel_list_at(out_slot, list_slot, index_slot);
  }

  // Checker should have rejected; defence.
  cel_value_at(out_slot)->kind = CEL_ERROR;
  cel_value_at(out_slot)->payload.err = CEL_ERR_TYPE_MISMATCH;
}
```

Same `__attribute__((musttail))` invariant as M3 — any form
clang can't prove a tail call is a compile error.  Toolchain
flags already in place from M3 (`-mtail-call`,
`--enable-tail-call`, `wasmtime_config_wasm_tail_call_set`).

**Runtime toolchain requirements**: none new vs M3.  The
tail-call config flipped on at M3.C still applies.

  - `wasm_imports.txt`: one new line — `cel_host.cel_list_at`.

### 2.3 `api/internal/cel_host.{h,cc}` — concrete backings

M2 shipped `HostListBacking` as an abstract base (bodies
`ABSL_CHECK(false) << "stub until M3"` then bumped to "M4").
M4 keeps the abstract name and adds the two concrete
subclasses:

```cpp
// abstract — shipped M2, stays.
class HostListBacking {
 public:
  virtual ~HostListBacking() = default;
  virtual size_t Size() const = 0;
  virtual absl::StatusOr<cel::Value> At(
      size_t index, const cel::CelType& expected_element_type) = 0;
  virtual void ForEach(
      absl::FunctionRef<void(const cel::Value&)> visit) const = 0;
};

// new — vector-backed.  Used both for `Activation::Bind(
// Value::List(...))` and as the runtime's host-side
// representation of kHost lists.
class HostList final : public HostListBacking {
  std::vector<cel::Value> elements_;
  /* Size / At / ForEach implemented here */
};

// new — proto reflection-backed view over a single REPEATED
// proto field.  Held via the ExternrefTable when
// ProtoBacking::ReadField returns a repeated field.
class ProtoList final : public HostListBacking {
  const google::protobuf::Message* absl_nonnull owner_;
  const google::protobuf::FieldDescriptor* absl_nonnull field_;
  /* implementations via Reflection::FieldSize +
     Get{Repeated{Bool,Int32,Int64,…}} */
};
```

One abstract + two concretes.  The CelValue kind stays
single (`CEL_LIST_HOST`); codegen doesn't branch on which
concrete impl — Layer 2 does a virtual call and the impl
dispatches.  Same shape as M3.D.

### 2.4 `api/internal/cel_host.{h,cc}` — Layer 2 body

One new Impl for M4 scope:

```cpp
ABSL_MUST_USE_RESULT absl::Status CelListAtImpl(
    uint32_t out_slot,
    uint32_t list_slot,
    uint32_t index_slot,
    const TrampolineContext& ctx);
```

Reads `ref_slot` from `list_slot`, dereferences through
`ExternrefTable::LookupList` to a `HostListBacking*`, decodes
the `index_slot` CelValue (must be `CEL_INT`; out-of-range
or non-int → `CEL_ERR_TYPE_MISMATCH` for non-int,
`CEL_ERR_INDEX_OUT_OF_BOUNDS` for negative or
`>= Size()`), calls `backing->At(index)`, encodes the result
back into `out_slot` via `EncodeFieldResult` (the encoder
shipped at M2.C).

M4 does **not** ship `CelListSizeImpl` / `CelListContainsImpl`
— `size` and `in` are M5 work along with the rest of the
kCall built-in overload set.

### 2.5 `api/internal/cel_host.h` — `ExternrefTable` extension

Add a third namespace for list backings, mirroring `Map`:

```cpp
class ExternrefTable {
 public:
  // … existing Intern/Lookup, InternMap/LookupMap …
  virtual uint32_t InternList(
      std::shared_ptr<const HostListBacking> backing) = 0;
  virtual const HostListBacking* absl_nullable LookupList(
      uint32_t slot) const = 0;
};
```

`HostExternrefTable` (production impl) gains a
`list_backings_` vector + slot-0 sentinel, matching the map
pattern.

### 2.6 `api/internal/cel_host_wasmtime.{h,cc}` — Layer 3 glue

One new trampoline registration: `cel_host.cel_list_at` (3-arg,
i32×3 → void).  Registered in
`RegisterCelHostImports` alongside `cel_get_field` (4-arg),
`cel_has_field` (4-arg), `cel_map_lookup` (3-arg).

Reuses the existing `NI32sToVoid(arity)` helper.  No new
machinery above what M3 set up.

### 2.7 `api/value.{h,cc}` — `Value::List` body filled in

M2 shipped the stub; M4:

```cpp
// value.cc (or cel_host.cc, mirroring how Value::Map landed)
Value Value::List(std::vector<Value> elements) {
  return Value::HostList(std::make_shared<HostList>(
      std::move(elements)));
}

Value Value::HostList(std::shared_ptr<HostListBacking> backing) {
  Value v;
  v.kind_ = Kind::kList;
  v.payload_ = std::move(backing);
  return v;
}

absl::StatusOr<const HostListBacking*> Value::ListBacking() const;
absl::StatusOr<std::shared_ptr<const HostListBacking>>
    SharedListBacking() const;
```

`StructurallyEquals` gains the kList arm: pointer-identity at
M4 (matches M3 maps; richer comparator deferred to M5
comprehensions).  `Value::Payload` variant grows a
`std::shared_ptr<HostListBacking>` arm.

### 2.8 `codegen/resolve_pass.{h,cc}` — origin inference

New `ListOriginVisitor` populates
`NodeAnnotation.list_origin` bottom-up.  M4 table
(mirrors M3.F's MapOriginVisitor):

| node | `list_origin` | source |
|---|---|---|
| `kCreateList` | `kArena` | constructed in the arena by `cel_list_create` |
| `kSelect` on list-typed (REPEATED) field | `kHost` | `ProtoBacking::ReadField` returns `Value::HostList(ProtoList{…})` |
| `kIdent` declared `list<T>` | `kHost` | `Activation::Bind(Value::List)` → interned via `cel_refs` |
| `kCall` returning list (M5+ comprehensions) | `kHost` | deferred |
| `?:` / `\|\|` / `&&` over list branches | both agree → that origin; else `kDynamic` | `map-list-dispatch.md §2.1` |
| anything else with `repr == kList` | `kDynamic` | safe default |

M2 already populated `list_origin = kHost` on `kSelect` /
`kIdent` (stub path); M4 extends to `kCreateList` → `kArena`
+ the `?:` coalescing rule.

### 2.9 `codegen/layout_pass.{h,cc}` — slots for list ops

  - `kCreateList` gets a result slot at `workspace_base + n*24`.
  - Per-element scratch slot reused after each
    `cel_list_append`.
  - `kCallExpr(_[_])` on a list operand gets a result slot;
    operand's slot + index's slot are the call inputs.

Identical shape to the M3 map slot allocator — share the
SlotAllocator helper.

### 2.10 `codegen/expr_lower.{h,cc}` — two new arms + tail-call-aware dispatch

**`kCreateList` arm.**  Unconditional kArena — literals
always build in the arena:

```wat
(call $cel.cel_list_create (local.get $out_slot) (i32.const <N>))
(loop over elements:
  <lower element expression into scratch slot E>
  (call $cel.cel_list_append (local.get $out_slot) (local.get E)))
```

**`kCallExpr(_[_])` arm — extend the existing M3 dispatch.**
The arm currently dispatches on `operand.repr == kMap` →
`operand.map_origin`.  M4 extends with `operand.repr ==
kList` → `operand.list_origin`:

| `operand.list_origin` | emitted call |
|---|---|
| `kArena` | `call $cel.cel_list_at_arena` |
| `kHost` | `call $cel_host.cel_list_at` |
| `kDynamic` | `call $cel.cel_list_at` — the dispatcher |

The kCall arm continues to be narrowly scoped to `_[_]`
only; the broader kCall built-in overload set lands at M5.

### 2.11 `compile.cc::InstallHostAbi` — runtime imports

Adds five new imports to mirror the M3 map shape:

  - `cel.cel_list_create` (i32, i32 → void)
  - `cel.cel_list_append` (i32, i32 → void)
  - `cel.cel_list_at_arena` (i32, i32, i32 → void)
  - `cel.cel_list_at` (i32, i32, i32 → void)
  - `cel_host.cel_list_at` (i32, i32, i32 → void)

`Engine::Plan::InstantiateRuntime` extends its
`BindRuntimeExport` loop to include the four `cel_list_*`
runtime exports.  Per the project rule, no lazy import
gating — every emitted module declares all five regardless
of AST shape.

### 2.12 `api/instance.cc` — encoder + decoder arms

  - **`EncodeList`** — new arm in `EncodeScalarValue`
    handling `Repr::kList`.  Interns the bound
    `Value::List` (or `Value::HostList`) backing into the
    externref table via `InternList`, writes
    `{CEL_LIST_HOST, payload.ref_slot=slot}`.  Symmetric to
    `EncodeMessage` from M2.C.
  - **`DecodeArenaListAt`** — new helper that mirrors M3's
    `DecodeArenaMapAt`.  Reads `ArenaListHeader`, walks
    `count` × 24-byte CelValue elements, recursively decodes
    each, wraps in a vector-backed
    `Value::List(std::vector<Value>)`.
  - **`DecodeCelValueAt`** grows a `CEL_LIST_ARENA` arm
    calling `DecodeArenaListAt`.  `CEL_LIST_HOST` arm
    deferred — host-bound lists round-trip through the
    activation marshaller's encoder, never come back from
    Eval as a result (they pass through the trampolines but
    don't materialise as a host-side `Value::List` from
    Eval).

### 2.13 `api/internal/cel_host.cc` — `ProtoBacking::ReadField` on REPEATED

Currently returns
`Value::Error(kTypeUnsupported, field_name)`.  M4 replaces
that branch with:

```cpp
if (field->is_repeated() && !field->is_map()) {
  return cel::Value::HostList(
      std::make_shared<ProtoList>(msg_, field));
}
if (field->is_map()) {
  return cel::Value::HostMap(
      std::make_shared<ProtoMap>(msg_, field));
}
```

Map case from M3 unchanged; new repeated case lights up.

### 2.14 `abi/cel_abi.proto` + `cel_abi_emit.{h,cc}`

**No changes.**  List literals don't need a ctor table
(unlike proto literals, which will).  `variables[]` /
`fields[]` / `attributes[]` stay as M3 shipped them.

## 3. Source layout (M4 deliverables)

```
compiler_v2/
├── runtime/
│   ├── cel_data.h                       BREAKING: CEL_LIST split +
│   │                                    ArenaListHeader +
│   │                                    CEL_ERR_INDEX_OUT_OF_BOUNDS
│   ├── cel_data_test.cc                 + size asserts
│   ├── cel_runtime.h                    + cel_list_create/append/
│   │                                    at_arena/at exports
│   ├── cel_runtime.c                    + arena list primitives +
│   │                                    tail-call dispatcher +
│   │                                    cel_host.cel_list_at import
│   ├── cel_list.h                       NEW — public list API mirror
│   │                                    of cel_map.h
│   ├── cel_list_test.cc                 NEW — exhaustive list ops
│   ├── wasm_imports.txt                 + cel_host.cel_list_at
│   └── BUILD.bazel                      exports for new symbols
├── codegen/
│   ├── expr_lower.{cc,_test.cc}         + kCreateList arm +
│   │                                    kCall(_[_]) list × 3 origins
│   ├── resolve_pass.{cc,_test.cc}       + ListOriginVisitor
│   ├── layout_pass.{cc,_test.cc}        + slot assignment for
│   │                                    kCreateList + kCall on lists
│   └── runtime_link_test.cc             + expected exports grow
├── api/
│   ├── value.{cc,_test.cc}              Value::List / Value::HostList
│   │                                    bodies; StructurallyEquals
│   │                                    kList; SharedListBacking
│   └── internal/
│       ├── cel_host.{h,cc,_test.cc}     + HostList + ProtoList +
│       │                                    CelListAtImpl +
│       │                                    InternList/LookupList
│       ├── cel_host_wasmtime.{h,cc}     + cel_list_at trampoline
│       └── instance.cc                  + EncodeList +
│                                        DecodeArenaListAt
├── ir/
│   └── annotations.h                    (list_origin already ships;
│                                        M4 just writes new
│                                        non-sentinel values)
└── e2e/
    └── m4_test.cc                       NEW — list e2e suite
                                         (mirror of m3_test.cc shape;
                                         see §6.2 below)
```

New WAT traces (per CLAUDE.md WAT-first rule):

  - `11_list_literal.wat` — `[1, 2, 3]` (kArena).
  - `12_list_index_arena.wat` — `[1, 2, 3][1]` (direct
    `call $cel.cel_list_at_arena`).
  - `13_list_index_host.wat` — `xs[0]` on bound list
    (direct `call $cel_host.cel_list_at`).
  - `14_list_index_dynamic.wat` — `(cond ? [1, 2] : xs)[0]`
    (call into dispatcher; dispatcher emits `return_call` to
    either arena or host arm).
  - `15_proto_repeated_field.wat` —
    `customer.tags[2]` (kSelect on REPEATED field returns
    `Value::HostList(ProtoList{…})`; kCall(_[_]) emits
    `call $cel_host.cel_list_at`).

Each gets a walkthrough in `wat-traces.md`.

## 4. What gets ported verbatim from M3

Most of M4 is structural copy-paste.  Reused (rename `map`
→ `list`, halve entry stride, drop key handling):

  - The runtime arena primitives' bump-allocate-and-grow
    pattern (`cel_map_create` / `_insert` / `_grow` →
    `cel_list_create` / `_append` / `_grow`).
  - The kDynamic dispatcher with `__attribute__((musttail))`
    arms.  Tail-call toolchain config already on.
  - Three-layer `cel_host` split + the `ExternrefTable`
    independent-namespace pattern (M3 has `Intern`/`Lookup`
    and `InternMap`/`LookupMap`; M4 adds
    `InternList`/`LookupList`).
  - Layer-3 wasmtime trampoline registration via
    `RegisterCelHostImports` + `NI32sToVoid(arity=3)`.
  - `EncodeFieldResult` reuses for the list-element-result
    case (handles scalar / span / message inlining
    automatically).
  - `Engine::Plan::InstantiateRuntime`'s exhaustive-export
    loop just gets four more names.
  - `IsInM3Envelope` → `IsInM4Envelope` rename + admit
    `list_value:` matchers; `CompareList` mirrors
    `CompareMap`'s order-aware (lists are ordered, unlike
    maps) recursive compare.

## 5. Work breakdown (order of authoring)

Each slice: WAT → assemble + `wat_runner` (with stubs where
Layer 3 hasn't landed yet) → unit tests at every pipeline
stage touched → e2e through `Instance::Eval` → milestone
doc progress log updated.

1. **M4.A — `cel_data.h` ABI split.**  `CEL_LIST` →
   `CEL_LIST_ARENA` + `CEL_LIST_HOST`; `CelValue.payload`
   grows `arena_list`; `ArenaListHeader` struct;
   `CEL_ERR_INDEX_OUT_OF_BOUNDS = 17`.  All consumers update
   in-commit.  Static asserts on sizes + indices.  No
   functional change yet.

2. **M4.B — runtime arena primitives.**  `cel_list_create` /
   `cel_list_append` / `cel_list_grow` / `cel_list_at_arena`
   in `cel_runtime.c`.  `cel_list_test.cc` round-trips each
   element kind + boundary indices (0, count-1, count, -1
   via cast) + duplicate elements (lists allow them) +
   out-of-capacity growth.

3. **M4.C — tail-call dispatcher.**  `cel_list_at` with
   `__attribute__((musttail))` arms + extern import decl
   for `cel_host.cel_list_at`.  `wasm_imports.txt` grows by
   one.  `runtime_link_test.cc` expected exports + import
   list grow.

4. **M4.D — concrete backings + `Value::List` body.**
   `HostList` + `ProtoList` concretes land in
   `cel_host.{h,cc}`.  `Value::List` /
   `Value::HostList` /  `ListBacking` /
   `SharedListBacking` bodies filled in.
   `StructurallyEquals` kList arm (pointer-identity).
   `value_test` + `host_list_test` (new) + `proto_list_test`
   (new) extended.  No codegen yet.

5. **M4.E — Layer 2 + Layer 3 wasmtime glue.**
   `CelListAtImpl` in `cel_host.cc`; Layer-3 trampoline
   registration in `cel_host_wasmtime.cc` (extends
   `RegisterCelHostImports`).  `ExternrefTable::InternList`
   /`LookupList`.  `cel_host_test.cc` covers absorption
   (`UNKNOWN` / `ERROR`), invalid index (non-int, OOB,
   negative), invalid slot, both concrete backings.

6. **M4.F — resolve + layout + codegen.**
   `resolve_pass.cc` `ListOriginVisitor` (kCreateList →
   kArena; kSelect/kIdent list-typed → kHost; mixed ?: →
   kDynamic); `layout_pass.cc` slot allocation;
   `expr_lower.cc` kCreateList arm + kCallExpr(_[_]) × 3
   list-origin arms.  WAT 11 / 12 / 13 / 14 assembled and
   disassembly-matched.  E2E: literal list, bound list,
   conditional-origin list, all three operand types.

7. **M4.G — `ProtoBacking::ReadField` on REPEATED fields.**
   Returns `Value::HostList(std::make_shared<ProtoList>(
   msg, field))`.  Flips the M2 envelope for the repeated-
   field case (other M2 boundary tests stay at their
   already-flipped state).  Adds
   `SelectRepeatedFieldReturnsHostList` e2e counterpart;
   re-purposes the existing
   `EnvelopeBoundaryE2ETest::SelectRepeatedFieldReturnsUnsupportedError`
   (currently `GTEST_SKIP`ped) to the new green expectation
   (or removes it in favour of the new test, with the
   m2_test.cc comment block updated).  WAT 15.  Requires
   the testdata `Customer` to grow a `repeated string tags
   = …` field (small fixture change — same pattern as
   M3.G's `metadata` map field addition).

8. **M4.H — activation marshaller + Eval decoder.**
   `EncodeList` arm in `instance.cc::EncodeScalarValue`
   (interns bound `Value::List` via `InternList`).
   `DecodeArenaListAt` + `CEL_LIST_ARENA` arm in
   `DecodeCelValueAt`.  Tests in `instance_test.cc`:
   round-trip of literal `[…]` Eval result; round-trip of
   bound-list-then-index `xs[i]` Eval result; PartialEval
   with unknown patterns over list-bearing programs.

9. **M4.I — conformance harness envelope.**
   `IsInM3Envelope` → `IsInM4Envelope`.  Admit
   `list_value:` matcher; `CompareValue::CompareList`
   (order-aware, recursive — lists are ordered per
   langdef).  Re-run `run_conformance`; update
   `conformance/README.md` inventory + per-fixture rows
   that move (`lists.textproto`, parts of `proto2/3.textproto`).
   Conformance unlock target: +60-80 PASSes.

10. **M4.J — reconcile map-list-dispatch.md §11 (list rows)
    + design.md.**  Tick the list bullets in `§11`.  Fold
    `§4.2` / `§4.7.3` / `§5` (list construction primitives)
    / `§6` (list emit rules) / `§7` (list runtime imports)
    / `§8` (Plan order) into `design.md` matching how the
    map rows landed in M3.I.  `map-list-dispatch.md` header
    flips to "fully reconciled into design.md <date>."

Each slice leaves the test suite green (default + manual-
tagged); each can be reverted independently.

## 6. Test plan

### 6.1 Unit (per file — see `per-component-test-coverage.md §3`)

  - `cel_data_test` — `CelKind` renumber layout;
    `ArenaListHeader` size (16 B); `kCelListEntryStride`
    = 24; CelValue payload union size (stays 24 B);
    `CEL_ERR_INDEX_OUT_OF_BOUNDS` wire value pinned.
  - `cel_list_test` (new) — `cel_list_create` /
    `cel_list_append` / `cel_list_at_arena` round-trip for
    each element kind (bool / int / uint / double /
    string / bytes / message / map);
    out-of-bounds index → `CEL_ERR_INDEX_OUT_OF_BOUNDS`;
    negative index (passed as `i32` cast from -1)
    similarly errors; empty-list indexing; large-list
    growth via `cel_list_grow`; **tail-call test** (loop
    `cel_list_at` N times against an arena list, observe
    stack-pointer export doesn't grow — same shape as M3's
    map dispatcher tail-call test).
  - `value_test` (extended) — `Value::List(elements)`
    round-trip; `StructurallyEquals` × ordered / different-
    order / missing-element / mismatched-kind;
    `Value::HostList(backing)` preserves backing identity.
  - `host_list_test` (new) — vector-backed `HostList::At`
    against every element kind; `Size()` / `ForEach()`
    round-trip; OOB index returns
    `Value::Error(kIndexOutOfBounds)`.
  - `proto_list_test` (new) — `ProtoList::At` against a
    proto3 fixture's `repeated <each cpp_type>` field;
    `repeated message` element wraps as
    `Value::HostMessage(ProtoBacking)`; `repeated map` is
    not legal (descriptor.proto rule); empty repeated
    `Size() == 0`.
  - `cel_host_test` (extended) — `CelListAtImpl` against
    `HostList` and `ProtoList` via the fake externref
    table; absorption of `UNKNOWN`/`ERROR` on either
    operand; invalid index kind → `CEL_ERR_TYPE_MISMATCH`;
    OOB index → `CEL_ERR_INDEX_OUT_OF_BOUNDS`; missing
    list ref slot → infrastructure failure.
  - `expr_lower_test` (extended) — `kCreateList` emits
    WAT-11 byte-for-byte; `kCallExpr(_[_])` × 3 list-origin
    arms each matches WAT 12 / 13 / 14 disassembly.
  - `resolve_pass_test` (extended) — list-origin inference
    table from §2.8 locked per node kind; `?:` coalescing.
  - `layout_pass_test` (extended) — kCreateList result +
    per-element scratch slots at expected offsets; scratch
    reuse across appends.
  - `compile_test` (extended) — list-bearing programs
    (`[1,2,3]` literal, `xs[0]` bound, `c.tags[0]` proto)
    compile cleanly; module imports `cel.cel_list_*` and
    `cel_host.cel_list_at`; ABI carries `kList` repr on
    declared list variables.

### 6.2 E2E (`compiler_v2/e2e/m4_test.cc` — new)

Mirror of `m3_test.cc` shape (when it lands).  Three
fixtures:

  - `ListLiteralE2ETest` — pure literal flows.  One TEST
    per element kind (uniform).  One TEST for
    literal-then-index `[1,2,3][1] → 2`.  Boundary tests:
    OOB `[1,2,3][3]` → error; empty-list `[][0]` → error.
  - `ListBindingE2ETest` — `Activation::Bind("xs",
    Value::List({…}))` round-trips through the kHost
    arm.  Per element kind.
  - `ProtoRepeatedE2ETest` — `customer.tags[i]` flows
    end-to-end against the testdata `Customer` extended
    with a `repeated string tags` field (M4.G fixture
    addition).  Negative-index test asserts OOB error.
  - `DispatcherE2ETest` — `(cond ? [1,2] : xs)[0]` for
    both branches; verifies kDynamic dispatcher routes
    correctly.

Plus an updated `EnvelopeBoundaryE2ETest::SelectRepeatedFieldReturnsHostList`
that replaces (or flips the assertion of) the existing
`SelectRepeatedFieldReturnsUnsupportedError` test in
m2_test.cc.  Per per-component-test-coverage SKIP rule:
**no fixture-level GTEST_SKIPs** — every test in the
suite either runs green or has an explicit single-test
deferral with a tracked follow-up.

### 6.3 Conformance unlock

  - `IsInM3Envelope` → `IsInM4Envelope` admits `value:`
    matchers with `list_value` kind.
  - `CompareValue::CompareList` — element-wise, **order-
    aware** (unlike `CompareMap`), recursive.
  - Re-run `run_conformance`; update README inventory.
    Expected per-fixture moves:
      - `lists.textproto` 0 → most-of-39 PASSes (literal +
        indexing tests; `in` / `size` blockers stay at
        SKIP for M5).
      - `proto2/proto3.textproto` `repeated_*` rows that
        only need REPEATED-field reads should graduate; M7
        message-binding gap still blocks the rest.
      - `parse.textproto` and `basic.textproto` self-eval
        rows with list literals graduate.

### 6.4 Closeout gate

Per `per-component-test-coverage.md §5`, copy this block
into the M4 PR description:

```
M4 closeout

[ ] All per-component _test.cc files written (per §3 of
    per-component-test-coverage.md)
[ ] No fixture-level GTEST_SKIPs added to e2e/m4_test.cc
[ ] m2_test.cc::SelectRepeatedFieldReturnsUnsupportedError
    flipped (or removed in favour of new e2e test)
[ ] bazel test //compiler_v2/... passes
[ ] scripts/run_full_suite.sh passes (default + 6 manual
    targets)
[ ] m4_test runs green (no fixture skips)
[ ] bazel run //compiler_v2/conformance:run_conformance —
    README inventory refreshed; per-fixture moves documented
[ ] testing-checklist.md "Rewrite M4" rows ticked
[ ] m4-list-literals.md status header reflects shipping
[ ] map-list-dispatch.md §11 list bullets all ticked,
    header flipped to "fully reconciled"
[ ] WAT traces 11–15 exist and are exercised by
    wat_runner_test
[ ] Conformance harness updated (IsInM4Envelope,
    CompareList)
```

## 7. Rough size estimate

Modelling on M3 (which was ~9 slices, ~6-8 hours per slice
given the per-component test discipline):

  - M4.A–C (runtime data + primitives + dispatcher): half
    the work of M3 since the dispatcher pattern is
    already proven.  ~2-3 slices, smaller each.
  - M4.D–E (host backings + Layer 2/3): ~equal to M3.
  - M4.F (codegen): ~equal to M3.F — same pattern, one
    new arm (kCreateList) + extension of an existing arm
    (kCallExpr).
  - M4.G (REPEATED field flip): smaller than M3.G (the
    Layer-1 ProtoBacking already exists; one
    `is_repeated()` branch flips from error to
    `Value::HostList`).
  - M4.H (activation marshal + decoder): new vs M3 (we
    didn't ship list activation in M3; the M2 work
    sketched the encoder shape but never landed for
    `Repr::kList`).  Modest size.
  - M4.I–J (conformance + doc reconcile): smaller than
    M3.H–I; harness shape is now well-understood.

**Total: ~9 slices, ~5-7 working sessions.**  Smaller than
M3 in aggregate because the design is fully locked and the
test-coverage rigour is now baked in (no rediscovery).

## 8. Risks + open questions

  1. **Negative indices.**  Langdef on list indexing ("If
     the index value is negative or `>= size`, an error is
     raised") is unambiguous; M4 mirrors that.  No risk —
     locked via test.
  2. **`CEL_LIST` numeric value gap.**  The M3 enum split
     leaves a hole at `CEL_LIST = 7` (was the pre-M3
     value).  M4.A's split needs to either keep that
     numeric value (if no consumer renumbered to fill it)
     or pick fresh numbers.  Audit `cel_data.h` + every
     consumer at the start of M4.A; pick the option that
     minimises ABI churn.
  3. **`HostList::At` `expected_element_type`.**  Mirrors
     `HostMap::Get`'s `expected_value_type` — informational
     at M4 (no implicit coercion).  Document the contract;
     don't lean on it.
  4. **Comprehensions might want `cel_list_clear` /
     `cel_list_set`.**  M5 design call.  M4 ships
     `create` + `append` only; comprehension intermediate
     accumulators may need a `clear` if sub-expressions
     can fail mid-append.  Defer to M5 unless a concrete
     M4 test requires it.

## 9. Dependencies + sequencing

  - **Hard prereqs (already shipped):** M2 idents / kSelect
    / has / PartialEval (2026-04-25), M3 maps + tail-call
    dispatcher + cel_host three-layer scaffolding (2026-04-24).
  - **M5 unblocks:** `size` / `in` / `+` over lists;
    comprehensions iterating lists.
  - **M7 still blocked by message bindings** — independent
    of M4; the list e2e tests use scalar-list bindings
    only.
  - **Single-test SKIPs from M2:** `IdentE2ETest::String` /
    `Bytes` (host-arena allocator work).  M4 doesn't fix
    these; the corresponding list-of-string activation case
    inherits the same gap and stays a single-test skip
    until the host-arena work lands.

---

## Future work (will be appended at close)

  - **WAT traces 06–15 landed retroactively.**  M3 maps + M4
    lists originally shipped without WAT traces; both fully
    backfilled — `wat/06_map_literal.wat` through
    `wat/15_proto_repeated_field.wat` covering kArena fast paths,
    kHost trampolines, kDynamic dispatcher, and proto map/list
    field reads.  `wat-traces.md` extended with the
    walkthroughs.  `wat_runner.cc` extended to bind the map/list
    runtime exports and accept 3-arg `cel_host.*` stubs.
    `wat_runner_test` adds 12 new test cases (10 PASS + 2 SKIP);
    the 2 SKIPs are the kDynamic dispatcher arm tests, which
    panic the wasmtime c-api on the `return_call` →
    imported-host-function path (production e2e covers them
    through the full `wasmtime::Engine`).
  - **`ResolvePass` scope handler (M5 prereq).**  M4.F plugged a
    `ComprehensionDetector` early-exit so the resolver doesn't
    crash on cel-cpp's macro-expanded `@result` / `@iter` idents
    (which legitimately carry different Reprs across comprehension
    forms).  M5's comprehension lowering must replace this gate
    with a real scope-aware resolver (push a fresh scope on
    entering a `kComprehensionExpr`, pop on exit; intern names
    per-scope rather than globally).
  - **Negative-index error surface.**  M4.G's
    `cel_list_at_arena` returns `{CEL_ERROR,
    CEL_ERR_INDEX_OUT_OF_BOUNDS}` for negative + OOB indices and
    the m4_test asserts `Eval` fails — but the Eval-side decoder
    surfaces `CEL_ERROR` as a top-level decode error rather than a
    proper `Value::Error(kIndexOutOfBounds)` because the Error-
    matcher work is M4-error-surface-era.  When that lands, the
    OOB / negative-index tests in `m4_test.cc` should flip to
    asserting on the structured Error value.
  - **String / bytes / list-of-string activation marshalling.**
    Same host-arena gap that M2's `IdentE2ETest::String` /
    `Bytes` still SKIPs.  `Activation::Bind("xs",
    Value::List({Value::String(...), …}))` works at the encoder
    side — the kList encoder interns the backing — but the
    list elements that round-trip through `cel_list_at` still hit
    the `kString`/`kBytes` encoder path for the result and
    require the host arena to land first.
  - **Conformance ceiling fell short of the +60–80 estimate.**
    Lists landed +9 PASSes (3.5× under the rough estimate).  The
    bulk of `lists.textproto` is gated behind `size(list)` /
    `list1 == list2` / `x in list` / `+` (concatenation), which
    are M5 kCall built-ins.  Once M5 ships, expect the
    `lists.textproto` + `comparisons.textproto` rows to graduate
    in bulk.
  - **Empty list literal `[]`.**  Once M5's comprehensions
    surface a list type from the iter-expr type, comprehensions
    over an empty source list will exercise the `count == 0`
    branch of `cel_list_create`.  `cel_list_test` already covers
    that path at the runtime level.
  - **`RejectDyn` misses implicit-dyn from list literals.**
    ~~Open at M4 close.~~ **Closed by M5.A on 2026-04-25.**
    `UnacceptableLabel` in `frontend/parse_and_check.cc` now
    recurses through `list_type().elem_type()`,
    `map_type().{key,value}_type()`, and
    `abstract_type().parameter_types()`, so `[]`, `[1, "two"]`,
    and bare `{}` all reject at the gate.  Tests
    `BareEmptyListLiteralRejected` /
    `HeterogeneousListRejected` flipped from `EXPECT_TRUE` to
    `EXPECT_FALSE`.  Conformance dipped 212 → 207
    (5 rows that previously slipped the gate now correctly
    fail-compile); the M5.B-J slices grow the envelope back
    and well past the M4 ceiling.
