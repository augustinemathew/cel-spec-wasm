# M-Mem: static layout pass (pre-codegen memory planning)

Status: **design — drafted 2026-04-20, not yet scheduled.**
Companion doc to `memory-ownership-flip.md` — the flip is the *enabling*
change (expr module owns memory); this pass is what cashes in on it.

## Why a separate pass

Today codegen is responsible for every memory decision as it walks the
AST:

  - bool / string / bytes / null literals go through `cel_make_*` runtime
    calls that allocate on every eval (`expr_lower.cc:576-590`);
  - scratch space for checked-arithmetic sret is a single slot pulled
    from `LoweringContext::GetScratchSlotLocal()` and shared across the
    whole function — with an inline tag-check branch after every op
    (`expr_lower.cc:621-…`);
  - int / uint / double literals lower to raw wasm `i64.const` /
    `f64.const`, creating two lowering worlds ("raw scalars" vs "boxed
    CelValues") that bridge awkwardly at 3VL sites.

This pass collapses all three into one compile-time plan.  Every AST
node is tagged with **where its result lives** — static `.rodata`,
pre-assigned workspace slot, ident binding, or dynamic `cel_alloc` —
before codegen starts.  Codegen becomes a mechanical map from
`Resolution` variants to wasm instructions.

## Three storage regimes

| Regime | Backing | Lifetime | Emission-time |
|---|---|---|---|
| **Static** | `.rodata` data segment inside expr module's memory | module load | pass writes bytes into `StaticMemoryBuilder` |
| **Workspace** | Pre-assigned slots in expr module's memory, between `.rodata` and the dynamic arena | one eval call | pass assigns offsets via `SlotAllocator` |
| **Dynamic** | Bump arena (`cel_alloc`) | one eval call | codegen emits runtime call |

Dynamic is the escape hatch for results whose size isn't known at
compile time: `string + string`, list builds, field-reads of
variable-length strings.  Everything else resolves statically.

## The `Resolution` table (answers the CLAUDE.md symbol-table debt)

Every `cel::Expr::id()` maps to a `Resolution` that says "this is what
your result is" in one of five shapes:

```cpp
namespace celwasm {

struct IdentBinding {
  // Wasm local index holding the CelValue offset handed in by the host.
  uint32_t local;
};

struct StaticCelValue {
  // Byte offset of a pre-baked CelValue in the .rodata segment.
  uint32_t offset;
};

struct ComputedSlot {
  // Byte offset of a pre-assigned workspace cell.  Multiple expr ids
  // may share an offset if their lifetimes do not overlap.
  uint32_t offset;
};

struct DynamicAlloc {
  // Bytes the runtime must cel_alloc for this result (may be an
  // upper bound; exact size computed at runtime when it varies).
  uint32_t size_hint;
};

struct FieldRef {
  uint32_t field_tag;     // proto wire tag
  uint32_t descriptor_id; // into the host's descriptor pool
};

struct OverloadRef {
  std::string helper_name; // e.g. "cel_int_add_v"
};

struct Resolution {
  cel::Type type;
  std::variant<IdentBinding, StaticCelValue, ComputedSlot,
               DynamicAlloc, FieldRef, OverloadRef>
      info;
};

}  // namespace celwasm
```

Rationale for the shape:

  - **Unified key.** Every `cel::Expr` in the tree has exactly one
    `Resolution`; codegen never consults `WasmAnnotations`,
    `LoweringContext::idents`, and `CheckOptions::variable_specs`
    separately the way it does today.
  - **Literals-are-not-symbols** but still fit the same table.
    `IdentBinding` covers what the old "symbol table" would have been;
    the other variants cover the compile-time facts codegen also needs.
  - **Answers the unresolved design debt in `CLAUDE.md`.**  The two
    options in the design doc's "Open questions" (A: promote to
    `SymbolTable` on `TypedAst`, B: side-table off cel-cpp's
    `reference_map`) are both subsumed: this *is* the side-table,
    generalised to cover more than just identifier refs.  When M-Mem
    ships, close that debt bullet.

## Interfaces

### `StaticMemoryBuilder` — pack literals into `.rodata`

```cpp
class StaticMemoryBuilder {
 public:
  // Append a bool / int / uint / double / null CelValue (24 bytes,
  // no payload indirection).  Returns the offset.
  uint32_t AppendScalar(CelKind kind, CelScalarPayload payload);

  // Append a string / bytes CelValue plus its UTF-8 / byte payload.
  // The CelValue's `payload.span.offset` is backpatched to point at
  // the payload bytes emitted immediately after the header.
  // Returns the offset of the CelValue header.
  uint32_t AppendSpan(CelKind kind, absl::string_view bytes);

  // Reserved for list / map / message literals once those ship.
  // Recursive: child CelValues are appended first, then the parent's
  // pointer fields are backpatched.
  // uint32_t AppendList(absl::Span<const uint32_t> element_offsets);
  // uint32_t AppendMap(...);

  // Finalise: returns the byte buffer ready for
  // `BinaryenAddActiveSegment(mod, base=0, data)`.
  std::vector<uint8_t> Finalize();

  uint32_t size_bytes() const;
};
```

Alignment: CelValue is 24 bytes with 8-byte alignment; the builder
pads to `alignof(CelValue)` before each `AppendScalar`/`AppendSpan`
call and 1-byte-aligns span payloads.

Layout guarantee: **nothing inside `.rodata` points outside
`.rodata`** — spans carry `.offset` values that are linear-memory
absolute, and the `.rodata` segment is emitted at offset 0, so those
values are equal to the byte offset within the buffer.

### `SlotAllocator` — assign workspace offsets via Sethi–Ullman

```cpp
class SlotAllocator {
 public:
  explicit SlotAllocator(uint32_t workspace_base);

  // Allocate a workspace cell; returns its linear-memory offset.
  uint32_t Acquire();

  // Return a cell to the free-list.  Safe to call with the same offset
  // repeatedly across sibling subtrees — that's the whole point.
  void Release(uint32_t offset);

  uint32_t peak_slots() const;        // max concurrent
  uint32_t total_bytes() const;       // peak_slots * sizeof(CelValue)
};
```

Slots are `sizeof(CelValue) = 24` bytes each, 8-byte aligned.
`workspace_base` is `.rodata_end` rounded up to 8.

### `LayoutPass` — one-shot pre-codegen walker

```cpp
struct StaticLayout {
  std::vector<uint8_t> rodata;           // emitted as an active segment
  uint32_t workspace_base;               // rodata_end, aligned
  uint32_t workspace_bytes;              // SlotAllocator::total_bytes()
  uint32_t arena_base;                   // workspace_base + workspace_bytes
  absl::flat_hash_map<int64_t /*expr_id*/, Resolution> resolutions;
};

ABSL_MUST_USE_RESULT absl::StatusOr<StaticLayout> PlanStaticLayout(
    const TypedAst& ast);
```

The pass walks the typed AST once.  For each node:

  - **Literal** → `StaticMemoryBuilder::AppendX` → `StaticCelValue`
    resolution.
  - **Ident** → `IdentBinding{local}` (local index inherited from the
    variable declaration order in `TypedAst::variables()`).
  - **Select** → `FieldRef{tag, desc_id}` plus an output
    `ComputedSlot` for the result.
  - **Call** (arithmetic, logical, compare, string ops, etc.) →
    `OverloadRef{helper_name}` plus an output slot.  The slot is
    chosen by the Sethi–Ullman walk below; `OverloadRef` tells codegen
    which `_v` helper to call.
  - **Call that may produce variable-length output** (concat, list
    build, field-read of string) → `DynamicAlloc{size_hint}` instead
    of `ComputedSlot`.

Error modes — all detectable pre-codegen:
  - Literal too large to fit in remaining `.rodata` budget (if we ever
    set one): `ResourceExhausted`.
  - Repr has no static layout (e.g. a future dyn type that slipped
    past `RejectDyn`): `Unimplemented` with the expr id.

### Slot-assignment algorithm (inside `PlanStaticLayout`)

Two-phase.  Phase 1 computes slot-Strahler bottom-up; phase 2 walks
top-down, visiting the higher-Strahler child first at each internal
node, and issues `Acquire` / `Release` calls.

```cpp
// Phase 1: slot-Strahler.  Leaves that are ident bindings or static
// literals count as 0 (they don't consume workspace).
uint32_t SlotStrahler(const cel::Expr& e);

// Phase 2: emit Resolutions.  At an op with inputs of strahler s_l, s_r:
//   - Visit the higher-strahler subtree first (its slot will become
//     the output slot — we alias output-over-input).
//   - Visit the other subtree (its slot dies after the op reads it).
//   - Output slot = the input slot belonging to whichever side was
//     visited first.  Release the other input's slot.
void AssignSlots(const cel::Expr& e, SlotAllocator& alloc,
                 absl::flat_hash_map<int64_t, Resolution>& out);
```

Aliasing safety relies on the ABI invariant that every `cel_*_v`
helper loads both inputs before writing output — documented in
`cel_runtime.h`, enforced by a runtime test that calls
`cel_int_add_v(x, x, x)` and checks `x == 2*x_in`.

Peak slot count equals root's slot-Strahler, which for realistic CEL
(`a && b && c && …`, `x.y.z == "literal"`) is 1–3.

## Codegen integration

`LowerToEvalFunction` takes a `StaticLayout` alongside the `TypedAst`:

```cpp
ABSL_MUST_USE_RESULT absl::StatusOr<LoweredFunction> LowerToEvalFunction(
    const TypedAst& ast, const StaticLayout& layout,
    absl::string_view func_name, WasmModule& mod);
```

Inside, `LowerExpr` becomes a switch on
`layout.resolutions.at(expr.id()).info`:

| Variant | Emitted |
|---|---|
| `IdentBinding{local}` | `local.get local` |
| `StaticCelValue{offset}` | `i32.const offset` |
| `ComputedSlot{offset}` | `(call $helper (i32.const offset) <child_exprs>…)` |
| `DynamicAlloc{hint}` | `(call $helper (call $cel_alloc (i32.const hint)) <child_exprs>…)` |
| `FieldRef{tag,desc}` | `(call $cel_host_get_field (i32.const out_slot) <msg_expr> (i32.const tag) (i32.const desc))` |
| `OverloadRef{name}` | used to pick the helper name; output slot comes from the surrounding `ComputedSlot`/`DynamicAlloc` |

`LoweringContext::GetScratchSlotLocal` disappears;
`EmitCheckedArithmetic` disappears (the `_v` helpers own the sret-and-
tag-check protocol, and 3VL absorption carries ERROR through the chain
without codegen branching).  The `.rodata` segment is emitted once at
the end of the pass via `WasmModule::AddActiveDataSegment(0, …)`.

## Sequencing

Four slices, each independently testable.  Depends on
`memory-ownership-flip.md` slices 1-3 shipping first (the static data
segment belongs in the expr module's memory).

  1. **`Resolution` + ident-only** (~1 day).  Introduce the header,
     the `StaticLayout` struct, and a minimal `PlanStaticLayout` that
     only emits `IdentBinding` entries (drop-in replacement for
     `LoweringContext::idents`).  Codegen consults the table; no
     behavioural change.  Tests: every existing lowering test still
     passes.
  2. **Static literals** (~2 days).  Add `StaticMemoryBuilder`, emit
     bool / int / uint / double / null / string / bytes literals into
     `.rodata`, replace `cel_make_*` call sites with
     `i32.const offset`.  At this point int/uint/double also start
     travelling as boxed CelValue offsets rather than raw
     `i64`/`f64` — this is the ABI unification discussed in the
     parent thread (see "Boxing policy" open question below).
     Tests: lowering tests cover each literal kind; an e2e test
     instantiates the module and confirms `CallEval` results match a
     no-static-literals baseline.
  3. **Workspace slots** (~2-3 days).  Add `SlotAllocator`, implement
     slot-Strahler numbering, wire `ComputedSlot` through.
     `EmitCheckedArithmetic` deleted; `_ii`/`_uu` scalar arithmetic
     helpers deleted (dead after #2 collapses raw scalars).  Tests:
     unit test the slot-Strahler computation on a fixture set of
     tree shapes; lowering test that
     `((a+b)+c)+d` uses 1 slot and `(a+b)*(c+d)` uses 2.
  4. **Dynamic alloc escape hatch** (~1 day).  Tag string concat,
     list build, and any variable-length field-read with
     `DynamicAlloc` so `cel_alloc` is called only where needed.
     Tests: e2e concat still works; static-only evals do not call
     `cel_alloc` (smoke-test by setting a trap on `cel_alloc` in the
     host and running a literal-only expression).

Each slice updates `doc/implementation-plan/testing-checklist.md`
with the rows it flips.

## Open questions

  - **Boxing policy.**  Slice 2 proposes boxing int/uint/double into
    `.rodata` CelValues.  The immediate cost is that arithmetic
    helpers (`cel_int_add_v`, etc.) become the only arithmetic path;
    the `_ii`/`_uu` variants retire.  Accepted trade in the parent
    design discussion (2026-04-20): "box doubles too, since IEEE
    `f64.div` doesn't model CEL's div-by-zero-as-ERROR and UNKNOWN
    has no f64 encoding."  Confirm before Slice 2 starts.
  - **Slot reuse under UNKNOWN / ERROR.**  If a `_v` helper writes
    ERROR into its output slot and the surrounding op's absorption
    rule reads-then-overwrites, the output stays ERROR — correct.
    Verify this holds for every `_v` helper pair; add a
    slot-reuse-under-3VL test matrix in Slice 3.
  - **Upper bound on `.rodata` size.**  Unbounded `.rodata` grows
    the module byte-size; a very literal-heavy expression could blow
    past wasmtime's default max module size.  Cap at
    `kMaxStaticBytes` (say 1 MiB); on overflow, fall back to
    `DynamicAlloc` for further literals.  Not a Slice 2 blocker;
    file as follow-up.
  - **Map / list / message literals.**  Not covered here; handled
    when M5 (collections) lands.  `StaticMemoryBuilder` leaves
    `AppendList` / `AppendMap` stubbed.
  - **Interaction with comprehensions (M5).**  Comprehension
    accumulators need per-iteration slots — a workspace region that's
    *reused across iterations*, not freed between them.
    `SlotAllocator` will need a "scope" concept (push/pop) by M5;
    design it in at Slice 3 if it's cheap, else document and defer.

## Deliverables at M-Mem completion

  - `compiler/codegen/static_layout.{h,cc}` — `StaticLayout`,
    `PlanStaticLayout`.
  - `compiler/codegen/static_memory_builder.{h,cc}` — literal packing.
  - `compiler/codegen/slot_allocator.{h,cc}` — Sethi–Ullman allocator.
  - `compiler/codegen/resolution.h` — the `Resolution` variant.
  - `compiler/codegen/expr_lower.{h,cc}` — switched to
    `StaticLayout`-driven lowering; `EmitCheckedArithmetic` and
    `GetScratchSlotLocal` removed.
  - `compiler/runtime/cel_runtime.{h,c}` — `_ii`/`_uu` arithmetic
    variants removed; `cel_make_{bool,int,uint,double,null,
    string_view,bytes_view}` become optional (still callable from
    codegen paths that need them — e.g. user-supplied idents being
    boxed at the host boundary).
  - `compiler/host/host_loader.cc` — `cel_alloc(24)` for sret slot
    removed; the sret slot is a `StaticLayout`-known offset in the
    expr module's workspace.
  - Testing-checklist rows: "static-literal lowering" × each CEL
    type; "slot allocation" under the standard Strahler fixtures;
    "no `cel_alloc` on static-only evals"; "slot reuse preserves
    3VL absorption".

Close the CLAUDE.md "Unified symbol table" bullet when this doc's
deliverables ship.
