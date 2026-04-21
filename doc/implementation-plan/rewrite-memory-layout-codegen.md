# Rewrite: memory layout, symbol table, codegen simplification

Status: **design — drafted 2026-04-21, not yet scheduled.**

Supersedes `m-mem-static-layout-pass.md` and `memory-ownership-flip.md`.
Closes the "Unified symbol table" bullet in `CLAUDE.md` on completion.

## 0. What this is

Three threads of work that have accumulated in the implementation plan
are actually one rewrite:

  - **Memory ownership flip** — move the wasm `memory` from the runtime
    module to the expr module.
  - **Static memory pass** — compile-time decide where every AST node's
    result lives (`.rodata`, workspace slot, arena).
  - **Unified symbol table** — the five small tables (§4) that together
    tell codegen everything it needs to know: type, wire shape, storage,
    operator overload, ident binding, field access. Each table is
    keyed either by `expr_id` (per-node facts) or is a static lookup
    compiled into the tool (the overload table).

Together they replace today's "codegen does everything as it walks"
model with a layered pipeline whose codegen step is a mechanical
translation: *walk the AST, switch on node kind, read pre-computed
facts, emit wasm.* That collapses ~40% of `expr_lower.cc`, deletes
whole runtime helper families (`_at_ii`, `_at_uu`, most `cel_make_*`
call sites from codegen), and — not incidentally — gives us
per-instance memory isolation for free.

This is scoped as a **rewrite**, not an incremental patch. It is
sliceable (see §11), but the end-state interfaces are designed
up front.

## 1. Problem statement

### 1.1 Today's codegen does three jobs, poorly factored

`compiler/codegen/expr_lower.cc` is ~1500 lines. A single walk of the
AST:

  - resolves idents (`LoweringContext::idents`, populated inline by
    `BuildParamList`);
  - decides how literals travel (raw `i64.const`/`f64.const` for
    numeric, `cel_make_bool/int/uint/double/null` calls for boxed,
    `cel_alloc` + `cel_make_string_view` for strings);
  - decides where results live (a single shared scratch slot via
    `GetScratchSlotLocal()`, inline-branching `EmitCheckedArithmetic`
    to check tags after every op);
  - picks runtime helpers by hand-written `if (lhs == kInt && rhs ==
    kInt) emit cel_int_add_at_ii else …` ladders scattered across ten
    visitor methods.

Each job has a different natural shape, so wedging them into one
walker produces the "two lowering worlds" footgun: raw scalars and
boxed CelValues coexist and bridge awkwardly at 3VL sites
(Slice F Step 4 spent half its engineering budget on exactly this
bridging).

### 1.2 The runtime is not stateless, and it matters

`cel_runtime.c:57` defines `static uint8_t g_memory[CELWASM_ARENA_BYTES]`
in BSS — the "one instance per thread" constraint in
`wasm-compiler-design.md` §7.3.1 is driven by this single array. The
runtime exports `memory`; every expr module imports it. This works
for the common case (one runtime instance, many evals, `cel_reset`
between calls) but:

  - two evals on different threads need two runtime instances or an
    external mutex — not natural for a policy-engine embedding;
  - there is no clean place to put compile-time-known bytes (the
    `.rodata` segment the static layout pass wants to emit);
  - `cel_reset` is a contract between the host and the runtime that
    every embedder must get right.

### 1.3 The symbol-table debt

`CLAUDE.md` tracks an unresolved decision: name/type/scope info is
split across `CheckOptions::variable_specs` (frontend),
`TypedAst::variables()` + `WasmAnnotations` (IR), and
`LoweringContext::idents` (codegen). Two options were floated —
promote to `SymbolTable` on `TypedAst`, or side-table off
cel-cpp's `reference_map`. Comprehensions (M5) land before this
decision forces itself.

This rewrite takes the side-table approach and extends it. `WasmAnnotations`
(`compiler/ir/annotations.h`) is already the per-expr_id side map. It
grows new fields — `overload_id`, `local_index`, `scope_id`, `storage`
— and becomes the single answer to "what does codegen need to know
about expr_id X?". `LoweringContext::idents` goes away.

## 2. Design goals

These are the invariants the design must preserve; they drive every
interface choice.

  1. **Codegen is a pure translation.** Given a fully-populated
     `WasmAnnotations` plus the frozen `OverloadTable`, emitting
     wasm is a `switch (expr.kind())` with no decision-making, no
     memory-allocation choices, no type-based helper-name derivation.
  2. **Every expr_id in the checked AST has a NodeAnnotation
     populated with the fields its kind cares about.** No parallel
     tables to keep consistent; codegen does
     `annotations.Find(expr.id())` and everything it needs is there.
     Fields irrelevant to a kind (e.g. `overload_id` on an
     `ident_expr`) stay at their zero sentinel.
  3. **Post-mortem debuggability.** A `--debug-layout` mode preserves
     per-expr memory distinctness so the CEL debugger can walk the
     arena and recover intermediate values. Production builds reuse
     slots aggressively; the two modes produce identical observable
     semantics.
  4. **Comprehension-ready.** M5 adds scopes; `scope_id` on
     `NodeAnnotation` accommodates nested bindings without a schema
     change.
  5. **Per-instance memory isolation.** Two expr modules instantiated
     against one runtime share no linear-memory state. Today's
     "one thread" constraint dissolves.
  6. **All literals in `.rodata`.** Every `Const` node lands in
     `.rodata` unconditionally — there is no cap, no fallback path,
     no runtime-initialised-literal variant. The uniform slot-out ABI
     (§4.2) already guarantees non-literal CelValues land in a
     workspace slot; rodata is literals only and grows with the
     program. Wasm limits (linear-memory size) are the only ceiling,
     and hitting them is a codegen bug, not a design concern.
  7. **Testable in layers.** `ResolvePass`, `LayoutPass`,
     `StaticMemoryBuilder`, `SlotAllocator`, and `OverloadTable` are
     unit-testable in isolation. Pipeline integration is an
     additional layer of tests, not a substitute for unit coverage.
  8. **Operator lookup is a flat table, not a visitor ladder.** Every
     CEL overload is one entry in `OverloadTable`. Adding a new
     runtime helper = one seed row (built-ins) or one
     `RegisterCustom` call (customs); codegen needs no edits.

## 3. Architecture

### 3.1 New pipeline

```
source → parse → check → TypedAst  (types; cel-cpp reference_map)
                              │
                              ▼
                     ┌────────────────┐
                     │  ResolvePass   │  scope stack, name binding,
                     │                │  overload lookup
                     └────────┬───────┘
                              │  ResolveOutput:
                              │    WasmAnnotations (repr + field_number
                              │      + overload_id + local_index + scope_id)
                              │    local_types, max_scope_id
                              ▼
                     ┌────────────────┐
                     │   LayoutPass   │  .rodata, slots, arena, scope stack
                     └────────┬───────┘
                              │  StaticLayout:
                              │    annotations (same map, now with
                              │      .storage filled for every node)
                              │    rodata bytes, workspace/arena bases
                              ▼
                     ┌────────────────┐
                     │ codegen/lower  │  switch (expr.kind()), read
                     │                │  annotations + OverloadTable
                     └────────┬───────┘
                              │
                              ▼
                     Binaryen module (defines memory, active data segment)
                              │
                              ▼
                     Host loader (two-phase instantiation)
```

Two reasons for splitting ResolvePass and LayoutPass:

  - **Name/overload binding is structural; layout is strategic.**
    Resolve answers "which binding does this `x` refer to, and which
    overload does this `+` resolve to" — properties of the
    checker-emitted AST, not of codegen choices. Layout answers
    "where does the result live" — driven by `SlotAllocator` strategy,
    subject to debug/production switches. Separating them lets Layout
    be re-run with different policies without reparsing meaning.
  - **Comprehensions need a scope stack during Resolve.** Keeping
    that logic out of the memory planner keeps each pass shorter than
    the lint's 60-line function ceiling.

### 3.2 Memory regions in the expr module

Under the flip, the expr module defines its own linear memory.
Layout:

```
offset 0                                                      memory end
┌────────────┬───────────┬──────────────┬──────────────────────────────┐
│ reserved   │  .rodata  │  workspace   │          arena               │
│   (16B)    │  literals │  slots       │  cel_alloc bump region       │
└────────────┴───────────┴──────────────┴──────────────────────────────┘
     ▲             ▲             ▲                    ▲
     │             │             │                    └─ grows ↑
     │             │             └ fixed at LayoutPass (SlotAllocator::total_bytes)
     │             └ fixed at LayoutPass (StaticMemoryBuilder::size_bytes)
     └ null sentinel at offset 0 (a zero-kind CelValue so
       `off == 0 ⇒ absent` stays well-defined; 8 bytes header +
       8 bytes padding to align .rodata to 16)
```

  - **Offset 0 is reserved** as the "absent" sentinel. Every `_at`
    helper already treats `out == 0` as a caller bug and no-ops;
    the sentinel is a proper CelValue-shaped region so
    `cel_value_at(0)` returns a well-formed NULL.
  - **`.rodata` starts at 16** (first 8-byte-aligned offset past
    the sentinel). Contains CelValue headers + span payloads for
    every node annotated `StorageKind::kStaticRodata`.
  - **Workspace** holds pre-assigned 24-byte slots for nodes annotated
    `StorageKind::kWorkspaceSlot`. Slot count = peak Sethi–Ullman
    number (typically 1–3 for realistic CEL).
  - **Arena** is the runtime-internal bump region for variable-length
    payloads — the bytes a string-concat result points at, the body
    of a list/map, the payload of a host-decoded proto field. Not a
    node-storage kind; every CEL node's `CelValue` itself lives in
    `.rodata` / a workspace slot / a local. The arena is only reached
    via `cel_alloc` called inside runtime helpers. Grows forward from
    `arena_base`; reset by `cel_reset` exported from the expr module.

The runtime module still provides `cel_alloc` / `cel_reset` / 3VL /
arithmetic helpers. It imports the expr module's memory; its own
`g_cel_arena` (two u32s) lives at a fixed offset inside the reserved
region (§8.2) that both sides agree on by convention.

## 4. Symbol table and overload table

Codegen needs five facts per node: type (→ `Repr`), storage location,
ident binding, overload binding, proto field. They live in two places:
per-node facts on the existing `WasmAnnotations` side map, plus a
single immutable `OverloadTable` built once per compilation.

### 4.0 The five questions

| # | Question | Answered by |
|---|---|---|
| 1 | What is this node's CEL type? | checker (`TypedAst::TypeOf(id)`) — drives `Repr` |
| 2 | Where do its result bytes live? | `LayoutPass` → `NodeAnnotation::storage` |
| 3 | If it's an `ident`, which binding? | `ResolvePass` (scope stack) → `local_index` / `scope_id` |
| 4 | If it's a `call`, which overload → helper? | `ResolvePass` interns the checker's overload id; `OverloadTable` maps id → `OverloadImpl` |
| 5 | If it's a `select`, which proto field? | `ResolvePass` (already M3 G2) → `field_number` |

### 4.1 Per-node facts on `NodeAnnotation`

Extend the existing `WasmAnnotations` in `compiler/ir/annotations.h`.
The header already flags these three new fields as the planned growth
path (`attribute_id`, `pattern_id`, `scope_depth`).

```cpp
enum class StorageKind : uint8_t {
  kNone = 0,         // default / not yet populated
  kStaticRodata,     // CelValue in .rodata at known offset
  kWorkspaceSlot,    // CelValue in a pre-assigned 24B workspace cell
  kLocal,            // wasm local (ident_expr only)
};
struct Storage {
  StorageKind kind = StorageKind::kNone;
  uint32_t    payload = 0;   // rodata offset | slot offset | local idx
};
struct NodeAnnotation {
  Repr     repr           = Repr::kUnknown;   // existing
  uint32_t field_number   = 0;                 // existing, M3 G2
  uint32_t overload_id    = 0;                 // new, call_expr
  uint32_t local_index    = 0;                 // new, ident_expr
  uint32_t scope_id       = 0;                 // new, ident_expr
  Storage  storage;                             // new, all kinds
};
```

ResolvePass writes `repr` / `field_number` / `overload_id` /
`local_index` / `scope_id`. LayoutPass writes `storage`. Codegen reads
everything.

Zero sentinels cover "field irrelevant for this kind" — `overload_id`
is 0 on non-call nodes, `local_index` / `scope_id` are 0 on non-ident
nodes. A per-kind audit at ResolvePass end DCHECKs the populated-ness
pattern per kind, catching missed population once and for all. A
single audit function (~20 lines, kind-switch + per-kind field checks)
gives us the guarantee a `std::variant` would give compile-time, at
the test level.

### 4.2 Uniform call ABI

**Every overload — built-in or custom — has wasm signature
`(i32 out_slot, i32 arg0, i32 arg1, …) -> void`.** All args and the
output are `i32` offsets to `CelValue` cells in linear memory. The
helper reads inputs from the arg offsets, writes a fully-formed
`CelValue` into `out_slot`, and returns nothing.

This is a contract with the runtime: every helper the overload table
names must follow this shape. Today's `_at_vv` helpers already do;
the variable-length helpers (`cel_string_concat`, and future
`cel_list_build` / `cel_map_build`) migrate from return-offset to
slot-out. Internally they still `cel_alloc` variable-length payload
(the string bytes, the list body) — but the `CelValue` header, which
holds the offset-to-payload, lands in the caller-provided `out_slot`.

Consequences:

  - **Every call expression's node storage is `kWorkspaceSlot`.** There
    is no `kArena` storage kind; the arena is a runtime-internal
    allocator for variable-length payloads, not a node-storage choice.
    LayoutPass assigns workspace slots for calls, `.rodata` for
    literals, wasm locals for idents.
  - **Polymorphic overloads look identical at the call site.**
    `cel_int_add_at_vv(out, a, b)` and `cel_string_concat(out, a, b)`
    have the same wasm signature — the caller doesn't need to know
    which it's emitting to set up the call.
  - **One dispatch rule in codegen.** No slot-out vs arena-out branch;
    the only split is built-in vs custom (the latter prepends a
    `pattern_id` arg).

**Runtime implication.** `compiler/runtime/cel_runtime.c` owes every
helper this shape. Variable-length-return helpers that today return
an offset (e.g. `cel_string_concat`) get a new parameter `uint32_t
out_slot` and are migrated to write a `CelValue` there. The old
return-offset versions retire. This is a single runtime slice in the
rewrite plan (§11).

### 4.3 Overload table

Codegen needs `overload_id → (import_module, helper_name, pattern_id)`.
Built-ins are fixed at tool-compile time; custom functions are
registered by the embedder at compile time. Same lookup path for
both, one table.

```cpp
// compiler/codegen/overload_table.h

// Which wasm import module a helper comes from. The expr module imports
// from three modules today; only the first two are overload targets
// (cel_env is logging-only). Enumerated so a module rename is a one-
// line change in ImportModuleName() and a compile error everywhere.
enum class ImportModule : uint8_t {
  kCelRuntime = 0,  // "cel"      — runtime .wasm exports (cel_int_add_at_vv,
                    //              cel_and, cel_alloc, cel_string_concat, …)
  kCelHost    = 1,  // "cel_host" — host trampolines (cel_host_call_custom)
};
absl::string_view ImportModuleName(ImportModule m);  // → "cel" / "cel_host"

struct OverloadImpl {
  ImportModule     module = ImportModule::kCelRuntime;
  std::string_view name;        // wasm import name within `module`.
                                // Built-in: "cel_int_add_at_vv" (kCelRuntime).
                                // Custom:   "cel_host_call_custom"   (kCelHost).
  uint32_t pattern_id = 0;      // 0 for built-ins.
                                // Non-zero for customs — prepended as the
                                // first call arg by codegen.
};
```

**Polymorphism: one row per overload, not per function.** `size`
accepts string / bytes / list / map; `+` accepts int / uint / double
/ string / bytes / list / (timestamp, duration) / (duration, duration).
Each overload the checker resolves produces a distinct overload id;
each id maps to its own `OverloadImpl`.

**Canonical id list: `third_party/cel-cpp/common/standard_definitions.h`.**
cel-cpp defines every standard overload id as a
`constexpr absl::string_view k*` constant there (~212 constants:
`kAddInt`, `kAddString`, `kSizeString`, `kEqualsInt`,
`kLessEqualsIntUint`, …). Our table must cover every entry or mark
it explicitly unimplemented (§4.5).

**Runtime parity with cel-cpp.** Every helper named in the table has
a semantic counterpart inside cel-cpp (the interpreter's built-in
function implementations live at `third_party/cel-cpp/runtime/standard/`;
each `kAdd*` / `kSize*` / etc. lives there as a C++ function). Our
`cel_runtime.h` helpers **must mirror their cel-cpp counterparts
bit-for-bit in semantics** — int overflow behaviour, div-by-zero
error vs NaN, string concat encoding, duration arithmetic precision,
every 3VL edge. When a helper diverges, the divergence is either a
bug (fix it) or a documented spec-allowed choice (recorded in
`cel_runtime.h` at the declaration site, with a pointer to the
cel-cpp source file it diverges from). The coverage test (§4.5) is
name-level; the semantic-parity check is per-helper (cross-reference
cel-cpp impl in a comment on the helper declaration).

#### 4.3.1 Builder → frozen table

The table can't be a raw `constexpr` array because custom functions
need to join it at compile time. It also can't be a global mutable
registry — each compilation has its own embedder-supplied function
set. The shape is a builder that seeds built-ins unconditionally,
accepts custom registrations with a hard collision check, and freezes
into an immutable table.

```cpp
struct Seed { absl::string_view overload_id; OverloadImpl impl; };
constexpr Seed kBuiltinSeeds[] = { /* see 4.3.2 */ };

class OverloadTableBuilder {
 public:
  OverloadTableBuilder();  // seeds every row in kBuiltinSeeds.

  // Registers a custom host function. `pattern_id` must be non-zero.
  // Fails with AlreadyExists if `overload_id` is already present —
  // either from kBuiltinSeeds (user cannot shadow a built-in; CEL
  // spec forbids it and cel-cpp's FunctionRegistry would also reject)
  // or from a prior RegisterCustom call (two customs with the same id).
  ABSL_MUST_USE_RESULT absl::Status RegisterCustom(
      absl::string_view overload_id, ImportModule module,
      absl::string_view helper_name, uint32_t pattern_id);

  OverloadTable Build() &&;

 private:
  absl::flat_hash_map<std::string, OverloadImpl> entries_;
  absl::flat_hash_set<absl::string_view> builtin_ids_;  // for collision msgs
};

class OverloadTable {
 public:
  // Returns nullptr if the overload isn't registered — codegen treats
  // this as Unimplemented and aborts the compile with the id in the
  // error message.
  const OverloadImpl* Lookup(absl::string_view overload_id) const;

  // Dense 1-based id for fitting into NodeAnnotation.overload_id
  // (a uint32_t). Zero reserved for "unresolved". Assigned at Build()
  // time: built-ins first in kBuiltinSeeds order, then customs in
  // registration order. Returns 0 if not registered.
  uint32_t InternOverloadId(absl::string_view overload_id) const;

  // Reverse of InternOverloadId — called only with ids the builder
  // itself handed out; returns a reference and DCHECKs.
  const OverloadImpl& LookupById(uint32_t interned_id) const;

  // For import declaration: enumerate (module, helper_name) pairs
  // reached by the compiled expression. Codegen tracks the set of
  // interned ids it emitted and hands them back here.
  std::vector<std::pair<ImportModule, absl::string_view>> UsedImports(
      const absl::flat_hash_set<uint32_t>& used_ids) const;

 private:
  // Parallel arrays, indexed by (interned_id - 1):
  std::vector<std::string>    ids_;      // overload_id strings, owned
  std::vector<OverloadImpl>   impls_;
  absl::flat_hash_map<absl::string_view, uint32_t> index_;
};
```

**No-override rule.** `RegisterCustom` returns `AlreadyExists` when a
custom registration collides with a built-in id. This is CEL's
spec-level rule (users can't shadow standard functions) reflected at
the table level. cel-cpp's `FunctionRegistry` enforces the same rule
at the checker level; we double-check here so the failure cites the
overload id and points at the table, not at a deeper checker internal.
Collision against a previous custom is also rejected (prevents
silent last-one-wins when an embedder builds the registry in a loop).

**Ownership of overload_id strings.** `kBuiltinSeeds` string_views
point into cel-cpp's `constexpr` constants — stable for the process
lifetime. `RegisterCustom` may receive a caller-owned string_view;
the builder copies into `ids_` so the frozen table survives
registration-callsite strings going out of scope. `index_` keys
point into `ids_`, which is never resized after `Build()`.

**Why `Lookup` returns a pointer, not a reference.** Unresolved
overloads are a compile error (codegen emits Unimplemented), not a
crash. `LookupById(uint32_t)` — called only with ids the builder
itself assigned — returns a reference and DCHECKs.

#### 4.3.2 Built-in seeds

`kBuiltinSeeds` is a `constexpr` array in `overload_table.cc`. Every
row names `ImportModule::kCelRuntime` explicitly; `kCelHost` only
appears at `RegisterCustom` sites. `pattern_id` is omitted (0).

```cpp
constexpr auto kRT = ImportModule::kCelRuntime;

constexpr Seed kBuiltinSeeds[] = {
  // ── arithmetic: add ────────────────────────────────────────────
  {StandardOverloadIds::kAddInt,              {kRT, "cel_int_add_at_vv"}},
  {StandardOverloadIds::kAddUint,             {kRT, "cel_uint_add_at_vv"}},
  {StandardOverloadIds::kAddDouble,           {kRT, "cel_double_add_at_vv"}},
  {StandardOverloadIds::kAddString,           {kRT, "cel_string_concat_at_vv"}},
  {StandardOverloadIds::kAddBytes,            {kRT, "cel_bytes_concat_at_vv"}},
  {StandardOverloadIds::kAddList,             {kRT, "cel_list_concat_at_vv"}},
  {StandardOverloadIds::kAddDurationDuration, {kRT, "cel_duration_add_at_vv"}},
  {StandardOverloadIds::kAddTimestampDuration,{kRT, "cel_timestamp_add_dur_at_vv"}},

  // ── size (polymorphic) ─────────────────────────────────────────
  {StandardOverloadIds::kSizeString, {kRT, "cel_string_size_at_v"}},
  {StandardOverloadIds::kSizeBytes,  {kRT, "cel_bytes_size_at_v"}},
  {StandardOverloadIds::kSizeList,   {kRT, "cel_list_size_at_v"}},
  {StandardOverloadIds::kSizeMap,    {kRT, "cel_map_size_at_v"}},

  // ── equality (polymorphic) ─────────────────────────────────────
  {StandardOverloadIds::kEqualsInt,    {kRT, "cel_int_eq_at_vv"}},
  {StandardOverloadIds::kEqualsString, {kRT, "cel_string_eq_at_vv"}},
  {StandardOverloadIds::kEqualsBytes,  {kRT, "cel_bytes_eq_at_vv"}},
  // … one row per equality overload id.

  // ── logical / 3VL ──────────────────────────────────────────────
  {StandardOverloadIds::kLogicalAnd, {kRT, "cel_and_at_vv"}},
  {StandardOverloadIds::kLogicalOr,  {kRT, "cel_or_at_vv"}},
  {StandardOverloadIds::kLogicalNot, {kRT, "cel_not_at_v"}},

  // … every remaining constant from standard_definitions.h.
};
```

Keys are the exact `string_view` constants from cel-cpp's
`common/standard_definitions.h`, not ad-hoc strings. A rename upstream
is a compile error here, not a silent dispatch miss. Helper names use
the `_at_v` / `_at_vv` suffix uniformly — slot-out convention, §4.2.

### 4.4 Codegen dispatch

```cpp
const NodeAnnotation& a = *annotations.Find(expr.id());
switch (expr.kind()) {
  case kConst:  return EmitStorageLoad(ctx, a.storage);   // rodata
  case kIdent:  return BinaryenLocalGet(ctx.mod, a.storage.payload, ...);
  case kSelect: return EmitSelect(ctx, expr, a, child_refs[0]);
  case kCall: {
    const OverloadImpl& h = table.LookupById(a.overload_id);
    DCHECK(a.storage.kind == StorageKind::kWorkspaceSlot);
    // Uniform ABI (§4.2): (out_slot, args…) -> void.
    // Customs prepend pattern_id; built-ins pass args directly.
    return h.pattern_id != 0
        ? EmitCallCustom(ctx, h, a.storage.payload, child_refs)
        : EmitCallBuiltin(ctx, h, a.storage.payload, child_refs);
  }
  …
}
```

One dispatch rule: `h.pattern_id != 0` → custom, else built-in. Both
use the slot-out ABI. Node storage is deterministic from AST kind
(literal → rodata, ident → local, everything else → workspace slot)
so the dispatch table is tiny.

**Import declaration follows the table.** After codegen finishes
walking, `table.UsedImports(used_ids)` returns the unique
`(module, helper_name)` pairs the expr module references; the driver
emits one `AddFunctionImport` per pair. The old `expr_lower.cc`
pattern of `AddFunctionImport(name, "cel", …)` per-call disappears.
A helper mis-classified as `kCelRuntime` when it actually lives on
the host fails at link time (`--allow-undefined-file` rejects it);
the table is the single source of truth.

### 4.5 Coverage invariant

A unit test iterates every `k*` member of cel-cpp's
`StandardOverloadIds` (via a generated list — see `overload_table_test.cc`
in §11) and asserts either `InternOverloadId(k) != 0` (we have a
helper) or `kExplicitlyUnimplemented.contains(k)` (we know we don't,
codegen fails the expression with a clean Unimplemented status citing
the overload id). A new constant added to cel-cpp must be classified
in one of those two buckets before this test passes.

**What CEL permits vs forbids.** CEL's spec (`langdef.md`) says
embedders **may restrict which standard functions are available** — a
deployment may disallow `string.matches`, for instance. Our compile-
time equivalent: `CompileOptions::allowed_overloads` filters the
`OverloadTable` before freezing. A filtered-out overload behaves the
same as an unimplemented one — `Lookup` returns nullptr, codegen
emits Unimplemented with the overload id. `kExplicitlyUnimplemented`
is our-side; `allowed_overloads` is embedder-side; both routes wind
up at the same `nullptr` from `Lookup`, so codegen has one rejection
path.

### 4.6 Custom host functions

CEL embedders register functions through cel-cpp's checker API:

  - `TypeCheckerBuilder::AddFunction(const FunctionDecl&)`
    (`third_party/cel-cpp/checker/type_checker_builder.h`).
  - Each `FunctionDecl` carries one or more `OverloadDecl`s
    (`third_party/cel-cpp/common/decl.h`). Overload ids are either
    user-supplied via `MakeOverloadDecl("my_upper_string", string,
    string)` or auto-generated from arg types.

This compiler is AOT, so custom functions are declared at compile
time (the tool reads `CompileOptions` alongside the expression).
After the checker runs, `CheckedExpr.reference_map[id]` carries the
resolved overload id uniformly for built-ins and customs — no IR
distinction. Our frontend already consumes `reference_map`; nothing
about ResolvePass plumbing has to change.

Because the checker treats customs and built-ins the same way,
**customs are just dynamic entries in the overload table**. No
separate `NodeAnnotation` field, no separate dispatch path in codegen.

#### 4.6.1 Wasm ABI: single generic trampoline

One host import, regardless of arity:

```
cel_host.cel_host_call_custom(pattern_id, out_slot, args_ptr) -> void
```

`args_ptr` is a linear-memory offset to a contiguous `uint32_t[]` of
CelValue offsets — the `N` args, in order. `N` is fixed per
`pattern_id` (the host's dispatch table carries the arity), so the
host reads exactly that many slots and does not need a length field.

**Why single, not per-arity.** Per-arity trampolines push arity into
the wasm signature — every new max-arity grows the import set, and
the import-and-binding plumbing duplicates. A single trampoline
decouples the wasm ABI from the arity distribution of the embedder's
function library; the host does the fan-out. The small cost is the
`args_ptr` staging buffer — cheap and allocation-free (see below).

**`args_ptr` has no `cel_alloc` cost.** LayoutPass reserves one
**args-staging region** per expr module, sized to the maximum custom
arity observed in the AST (`max_arity * 4` bytes, 4-byte aligned).
Every custom call writes its args into this region at a known
offset and passes that offset as `args_ptr`. Because args-staging is
consumed synchronously by `cel_host_call_custom` — the host reads
the args and returns before emitting the next call — one region
serves every custom call in the expression. No arena traffic, no
per-call allocation; the cost is one `StaticMemoryBuilder`-adjacent
reservation at compile time.

**Out-slot and args follow the uniform ABI (§4.2).** Every arg cell
and the `out_slot` cell are pre-allocated `CelValue` offsets into
workspace. The host reads arg `i` as `*(CelValue*)(mem + *(uint32_t*)
(mem + args_ptr + i*4))` and writes the result into
`*(CelValue*)(mem + out_slot)`.

(Alternatives considered: per-function imports — baked into the
module, rebuild on every registry change, rejected. Per-arity
trampolines — fixed but duplicative; rejected because the single-
trampoline + staging region design has no measurable hot-path cost
and one fewer coupling point.)

#### 4.6.2 Registration flow

When the embedder registers a function at compile time:

1. `CompileOptions::RegisterFunction(name, arg_types, return_type, fn_ptr)`
   allocates a fresh `pattern_id` (monotonic `uint32_t`, 1-based).
2. The call is forwarded to cel-cpp's `FunctionRegistry` so the
   checker resolves calls normally; the checker assigns it an
   overload id (e.g. `"my_upper_string"`).
3. `OverloadTableBuilder::RegisterCustom("my_upper_string",
   ImportModule::kCelHost, "cel_host_call_custom", pattern_id)` is
   called. Arity is not encoded in `name` — one import name serves
   every custom, and the host's dispatch table carries arity per
   `pattern_id`. On collision with a built-in, the embedder gets a
   clean `AlreadyExists` citing the overload id.
4. The `cel.abi` custom section records `pattern_id → (function
   name, arg types, return type)` so the host dispatcher can wire
   `fn_ptr`s at instantiation time.

At ResolvePass, nothing special — the checker-supplied overload id
is interned exactly like a built-in's; only
`NodeAnnotation::overload_id` is written.

At codegen, the `kCall` arm is one branch (§4.4); `h.pattern_id != 0`
picks the custom-call emitter which prepends `pattern_id`.

#### 4.6.3 Host runtime

The host installs one dispatcher table at `LoadEval`:

```cpp
struct CustomEntry {
  uint8_t    arity;
  CustomFn   fn;  // void(*)(uint32_t out_slot, absl::Span<const uint32_t> args)
};
std::vector<CustomEntry> dispatcher;  // indexed 1..N by pattern_id

// Single wasm import bound to this one function:
void cel_host_call_custom_impl(uint32_t pattern_id,
                               uint32_t out_slot,
                               uint32_t args_ptr) {
  const CustomEntry& e = dispatcher[pattern_id];
  const uint32_t* args =
      reinterpret_cast<const uint32_t*>(memory_base + args_ptr);
  e.fn(out_slot, absl::MakeConstSpan(args, e.arity));
}
```

At instantiation, the host reads `cel.abi.custom_functions` and
wires each registered `pattern_id → {arity, fn_ptr}`. If the expr
module references a `pattern_id` the host has not bound, `LoadEval`
returns `FailedPrecondition` — detected at link time, not at eval.

Arity mismatch between the expr module's expectations and the host's
binding is caught at `LoadEval` too: the checker already recorded
the arg count against the overload id, and the `cel.abi` section
records it; the host cross-checks against its registered `arity`
field before returning from `LoadEval`.

#### 4.6.4 Test strategy

  - **Unit**: `RegisterCustom` appends one row; `InternOverloadId`
    returns the expected id; `RegisterCustom` with a built-in id
    returns `AlreadyExists`; `RegisterCustom` with a duplicate custom
    returns `AlreadyExists`.
  - **Integration**: fixture custom `my.upper(string) -> string`
    called from `my.upper("abc") == "ABC"` round-trips e2e.
  - **Arity coverage**: one e2e test per arity in `{0, 1, 2, 3, 8}`,
    each calling the single `cel_host_call_custom` trampoline; the
    test fixture registers a function at each arity and asserts the
    staging region layout (args read in order) matches the
    embedder-visible C++ signature.
  - **Args-staging reuse**: two custom calls in the same expression
    (e.g. `my.a(1, 2) + my.b("x")`) share the staging region; the
    second call overwrites the first's bytes before the host reads
    them — confirmed via a test that orders the calls and inspects
    staging bytes mid-eval.
  - **Negative — unbound pattern**: compile against a registry
    containing a function, instantiate without binding it; confirm
    `LoadEval` returns `FailedPrecondition` citing the unbound
    `pattern_id`.
  - **Negative — arity mismatch**: compile with a registry that
    declares `my.fn(int, int)`; at bind time, register a 1-arg
    `my.fn`; `LoadEval` returns `FailedPrecondition` citing the
    arity mismatch.

### 4.7 Proto messages and struct literals

CEL permits two kinds of aggregate construction:

  - **Proto message literals** — `my.pkg.Customer{name: "a", age: 3}`.
    Spec: `langdef.md` §"Message creation". Requires a proto
    descriptor registered with the checker; field names / types are
    checked against the descriptor. Runtime result: a fully-formed
    proto message (today: kMessage `externref`; §4.7.1).
  - **Struct (map) literals** — `{"name": "a", "age": 3}`. Spec:
    `langdef.md` §"Map creation". Checker result type is `map<K, V>`.
    Runtime result: a CEL map (kMap linear-memory header; §4.7.2).

cel-cpp's AST distinguishes them at parse time: `StructExpr` with a
non-empty `message_name` is a proto literal; empty `message_name` is
a map literal. Both land on `kCreateStruct` / `kCreateMap` in the
expr kind enum.

**Construction model.** None of the four aggregate kinds —
proto message, map, list, or struct-literal-as-map — is pre-packed
into linear memory at compile time. Codegen emits an **empty-then-
populate** sequence: a `create_*` call allocates an empty container
and writes its `CelValue` into `out_slot`; one `insert_*`/`push_*`/
`set_field_*` call per entry mutates the container in place. Each
entry's key and value are themselves `CelValue` offsets — first-
class operands, not baked-in layout bytes.

This keeps the ABI narrow: every aggregate call has the same shape
as every scalar call (slot-in, slot-out), and the runtime/host owns
all layout decisions. It also means aggregates with non-literal
keys or values (`{"name": user.first_name + user.last_name}`,
`[f(x), g(x)]`) lower through exactly the same emit sequence as
fully-literal aggregates — no branch, no special case.

#### 4.7.1 Proto message literals (`kCreateStruct` with `message_name`)

**Representation.** Same as other proto messages: `Repr::kMessage`,
an externref slot indexed by an i32 in `$cel_refs`. Construction
lives on the host side — the compiler doesn't link a proto runtime
into wasm.

**Codegen — empty-then-populate.** The expression
`Customer{name: expr_n, age: expr_a}` lowers to:

```
  ;; 1. Allocate an empty Customer. type_id is interned at compile
  ;;    time against the descriptor pool.
  cel_host.cel_make_message(type_id, out_slot)         ;; (u32, u32) -> void

  ;; 2. Lower each field value into its own workspace slot.
  <emit expr_n>   -> slot_n   ;; CelValue (string) at slot_n
  <emit expr_a>   -> slot_a   ;; CelValue (int)    at slot_a

  ;; 3. Set each field. field_id is the proto field-number.
  cel_host.cel_set_field(out_slot, field_id_name, slot_n)  ;; (u32,u32,u32) -> void
  cel_host.cel_set_field(out_slot, field_id_age,  slot_a)  ;; (u32,u32,u32) -> void
```

**Host surface (`compiler/host/cel_host.cc`, §4.7.5).** Four fixed
host imports, regardless of message shape:

```
cel_host.cel_make_message(type_id: u32, out_slot: u32) -> void
cel_host.cel_set_field   (msg_slot: u32, field_id: u32, value_slot: u32) -> void
cel_host.cel_get_field   (out_slot: u32, msg_slot: u32, field_id: u32) -> void   (read side — reused from M3 G2)
cel_host.cel_has_field   (out_slot: u32, msg_slot: u32, field_id: u32) -> void   (from M3 G3)
```

`type_id` is interned at compile time against the descriptor pool;
the host's dispatcher maps `type_id → MessageFactory` and
`(type_id, field_id) → MessageFieldSetter`. `cel_make_message`
creates an empty, mutable proto; `cel_set_field` writes a field
(host reads the CelValue from `value_slot` and calls the
descriptor's setter). Field IDs are proto field numbers — the same
`field_number` the checker already populates on `SelectExpr` nodes
(§3 `NodeAnnotation::field_number`) and that M3 Slice G2 wired for
reads.

**No `MessagePattern` table.** Earlier drafts proposed a
`(descriptor, field-assignment-shape) → pattern_id` side table; that
optimises for the fully-literal case and breaks the uniform
empty-then-populate model above. Dropped. The compile-time side
data is just `type_id → descriptor` (one entry per referenced
message type) and `(type_id, field_id) → setter` (looked up on
demand at bind time).

**Node storage.** `kWorkspaceSlot` — `out_slot` holds the
`Repr::kMessage` CelValue.

**Where in the OverloadTable.** Not in it. `cel_make_message`,
`cel_set_field`, `cel_get_field`, `cel_has_field` are the fixed
host surface (§7.4), declared in `DeclareHostImports`. Compilers
for code that uses proto literals always wire all four — no AST-
gated import (per `feedback_no_lazy_imports`).

#### 4.7.2 Map literals (`kCreateMap`)

**Representation.** `Repr::kMap`, a linear-memory `CelMap` header
with a heap of key-value entries in the arena. Pure runtime — no
host trip.

**Codegen — empty-then-populate.** `{k1: v1, k2: v2}` lowers to:

```
  cel_map_create(out_slot)                           ;; (u32) -> void
  <emit k1> -> slot_k1
  <emit v1> -> slot_v1
  cel_map_insert(out_slot, slot_k1, slot_v1)         ;; (u32,u32,u32) -> void
  <emit k2> -> slot_k2
  <emit v2> -> slot_v2
  cel_map_insert(out_slot, slot_k2, slot_v2)
```

**Runtime surface (`compiler/runtime/cel_runtime.h`).**

```c
void     cel_map_create(uint32_t out_slot);
void     cel_map_insert(uint32_t map_slot, uint32_t key_slot,
                        uint32_t value_slot);
void     cel_map_lookup(uint32_t out_slot, uint32_t map_slot,
                        uint32_t key_slot);   // out = ERROR if missing
uint32_t cel_map_size  (uint32_t map_slot);   // plain i32 for size overload
```

Each name is a near-mirror of cel-cpp's `runtime/standard/map_*.cc`
so the parity invariant (§4.3) is trivial to uphold. Duplicate keys
at insert are a spec error (`langdef.md`): the runtime writes an
`ERROR` CelValue to `out_slot` on the second `cel_map_insert` with
a duplicate key, and subsequent `cel_map_insert` on that now-ERROR
map is a no-op (ERROR is absorbing).

**Node storage.** `kWorkspaceSlot` on the `kCreateMap` node itself
(holds the map `CelValue`). Per-entry key/value slots follow Sethi–
Ullman and die at the end of the containing expression.

**Why not in OverloadTable.** `create_map` and `insert_*` aren't
cel-cpp overloads — they're codegen-side primitives. `size(map)`
**is** a spec overload and *is* in the OverloadTable (routing to
`cel_map_size`).

#### 4.7.3 List literals (`kCreateList`)

Same pattern as maps — empty-then-populate, pure runtime:

```
  cel_list_create(out_slot)                          ;; (u32) -> void
  <emit elem_i> -> slot_i
  cel_list_append(out_slot, slot_i)                  ;; (u32,u32) -> void
  ;; …repeated per element
```

**Runtime surface.**

```c
void     cel_list_create(uint32_t out_slot);
void     cel_list_append(uint32_t list_slot, uint32_t elem_slot);
void     cel_list_at    (uint32_t out_slot, uint32_t list_slot,
                          uint32_t index_slot);       // out = ERROR if OOB
uint32_t cel_list_size  (uint32_t list_slot);
```

Lists are homogeneous at the type-checker level — the codegen emits
no per-element type tag. Out-of-bounds `cel_list_at` writes an
ERROR CelValue.

#### 4.7.4 Struct literals without `message_name`

CEL's parser produces `StructExpr` with an empty `message_name`
for plain map literals (`{"a": 1}`). They lower identically to map
literals (§4.7.2). No codegen-level distinction.

Typed structs (`foo.Bar{x: 1}`) always carry a non-empty
`message_name` and flow through §4.7.1 as proto literals. The
checker rejects `message_name` referring to a non-proto descriptor
before we see it.

#### 4.7.5 Spec-allowed function set & host declaration

`langdef.md` and cel-cpp together define which construction
operations are spec-legal:

  - Proto message literals require a registered descriptor —
    `CompileOptions::descriptor_pool` must contain `message_name`,
    and each field in the literal must be declared on the descriptor.
    Violations fail compilation with `InvalidArgument`.
  - Map keys must be comparable (int, uint, bool, string). Checker
    enforces; codegen trusts it.
  - Every construction primitive (`cel_map_create`, `cel_map_insert`,
    `cel_map_lookup`, `cel_map_size`, `cel_list_*`,
    `cel_host.cel_make_message`, `cel_host.cel_set_field`,
    `cel_host.cel_get_field`, `cel_host.cel_has_field`) carries a
    cel-cpp parity pointer in its declaration comment, per §4.3.

**Declared once, up front.** Per `feedback_no_lazy_imports`, the
expr module imports the full set of aggregate primitives regardless
of whether the AST uses them. `DeclareRuntimeImports` + `DeclareHostImports`
(§7.4) name each primitive unconditionally.

### 4.8 Scope stack (ResolvePass internal)

Pushed during ResolvePass when entering a comprehension body; popped
on exit. Records iter/accu locals so ident references inside the body
resolve to them. The stack is a compile-time artefact — only `scope_id`
(written into `NodeAnnotation`) reaches codegen.

```cpp
// Internal to resolve_pass.cc
struct ScopeFrame {
  uint32_t scope_id;        // fresh per PushScope
  absl::flat_hash_map<std::string, uint32_t> name_to_local;
};
```


## 5. ResolvePass — `compiler/codegen/resolve_pass.{h,cc}`

### 5.1 Interface

```cpp
namespace celwasm {

// WasmAnnotations with `repr`, `field_number`, `overload_id`,
// `local_index`, `scope_id` populated. `storage` is still zero-
// initialised — LayoutPass fills it next.
struct ResolveOutput {
  WasmAnnotations annotations;
  std::vector<BinaryenType> local_types;  // declared in the lowered $eval
  uint32_t max_scope_id = 0;              // 0 if no comprehensions present
};

ABSL_MUST_USE_RESULT absl::StatusOr<ResolveOutput> ResolvePass(
    const TypedAst& ast);

}  // namespace celwasm
```

### 5.2 What it does

Walks the `TypedAst` once. For every node:

  - Writes `repr` from the node's static type (today's
    `PopulateAnnotations` logic; moved here).
  - For `ident_expr`: looks up the name in the scope stack
    (innermost-wins), writes `{local_index, scope_id}`.
  - For `call_expr`: reads the cel-cpp checker's overload choice
    (`reference_map[id].overload_ids()`), runs it through
    `InternOverloadId`, writes `overload_id`. An overload that isn't
    in `OverloadTable` yet returns `overload_id = 0`, which codegen
    later rejects with `Unimplemented`.
  - For `select_expr`: writes `field_number` (already today's M3 G2
    logic; moved here).
  - For `comprehension_expr`: pushes a `ScopeFrame` before visiting
    the body, pops after. Iter/accu names go into the frame's
    `name_to_local` with pre-assigned local indices.

Nested `[1].exists(x, [2].exists(x, x == 2))` produces two distinct
`scope_id`s for the two `x` bindings; the inner `x == 2` binds to the
inner scope (spec-mandated shadowing).

### 5.3 What it does NOT do

  - No memory decisions. No slot assignment. No .rodata emission.
    (That's LayoutPass.)
  - No `Storage` writes. Every node leaves ResolvePass with
    `storage.kind == kNone`.
  - No runtime helper string lookup. Only overload **id** interning;
    the name-to-helper mapping is `OverloadTable`'s job, consulted
    at codegen time.

This pass is O(nodes), touches only the scope stack.

## 6. LayoutPass — `compiler/codegen/layout_pass.{h,cc}`

### 6.1 Interface

```cpp
namespace celwasm {

struct StaticLayout {
  WasmAnnotations annotations;     // every node now has .storage filled

  std::vector<uint8_t> rodata;     // active data segment @ offset 16
  uint32_t rodata_base = 16;
  uint32_t workspace_base = 0;     // rodata_base + rodata.size(), 8-aligned
  uint32_t workspace_bytes = 0;    // SlotAllocator::total_bytes()
  uint32_t arena_base = 0;         // workspace_base + workspace_bytes

  std::vector<BinaryenType> local_types;   // carried from ResolveOutput

  uint32_t peak_slots = 0;
  bool debug_mode = false;
};

struct LayoutOptions {
  bool debug_layout = false;       // disable slot reuse
};

// Takes ResolveOutput by value; extends `annotations` in place by
// writing `.storage` for every node.
ABSL_MUST_USE_RESULT absl::StatusOr<StaticLayout> LayoutPass(
    const TypedAst& ast, ResolveOutput resolved,
    const LayoutOptions& opts = {});

}  // namespace celwasm
```

### 6.2 Sub-components

#### 6.2.1 `StaticMemoryBuilder` — `.rodata` packing

```cpp
namespace celwasm {

struct CelScalarPayload {
  CelKind kind;
  union { int32_t b; int64_t i; uint64_t u; double d; } value;
};

class StaticMemoryBuilder {
 public:
  explicit StaticMemoryBuilder(uint32_t base_offset);

  // Returns the CelValue's linear-memory offset. Infallible — the
  // builder grows as needed. Literals always land in rodata
  // (§4.2); there is no cap, no fallback path, no runtime-
  // initialised-literal variant.
  uint32_t AppendScalar(const CelScalarPayload& p);
  uint32_t AppendSpan(CelKind kind, absl::string_view bytes);

  // Future work (M5): AppendList, AppendMap.

  std::vector<uint8_t> Finalize() &&;
  uint32_t size_bytes() const { return buf_.size(); }

 private:
  std::vector<uint8_t> buf_;
  uint32_t base_offset_;
};

}  // namespace celwasm
```

CelValue is 24 bytes, 8-byte aligned. Span payloads are 1-byte aligned
but the next CelValue pads back to 8.

Rodata is unconditional — no cap, no `StatusOr` plumbing. By §4.2
every CelValue header is either a literal (→ rodata) or a call/ident
result (→ workspace slot / local); the alternatives table we chose
not to have doesn't exist, so there is no fallback to maintain.
Wasm's 4 GiB linear-memory limit is the absolute ceiling, and any
expression that approaches it is a codegen bug worth surfacing
loudly — not a design concern.

#### 6.2.2 `SlotAllocator` — workspace assignment

```cpp
namespace celwasm {

class SlotAllocator {
 public:
  SlotAllocator(uint32_t base_offset, bool debug_mode);

  uint32_t Acquire();                      // byte offset of a 24B cell
  void Release(uint32_t offset);           // no-op in debug_mode

  // M5: comprehension-scope semantics (slots acquired inside a
  // PushScope/PopScope pair all die at PopScope).
  void PushScope();
  void PopScope();

  uint32_t peak_slots() const { return peak_slots_; }
  uint32_t total_bytes() const { return peak_slots_ * 24; }
};

}  // namespace celwasm
```

Debug mode: `Release` is a no-op. Peak slots then equals the number
of `kWorkspaceSlot` nodes in the tree. Memory cost: 24 B × nodes,
bounded by expression size (realistic worst case: ~12 KB for 500
nodes).

### 6.3 Slot-Strahler walk

Phase 1 computes slot-Strahler bottom-up:

```cpp
// 0 for leaves whose storage is kStaticRodata / kLocal (no workspace need).
// Otherwise: max(l, r) + (l == r) over children that consume workspace.
uint32_t SlotStrahler(const cel::Expr& e, const WasmAnnotations& anno);
```

Phase 2 walks top-down and writes `Storage`:

  1. Leaf `const_expr` → `StaticMemoryBuilder::AppendX` → `storage =
     {kStaticRodata, offset}`. Rodata is unconditional (§6.2.1) —
     no cap, no fallback path.
  2. Leaf `ident_expr` → `storage = {kLocal, local_index}` (copied
     from `NodeAnnotation::local_index`).
  3. Every internal node (call, select, has, list build, map build,
     comprehension result) → `storage = {kWorkspaceSlot, out_offset}`.
     Uniform slot-out ABI (§4.2) means there is no per-node-kind
     branch here; the allocator runs the same way for every computed
     CelValue:
     a. Visit the higher-Strahler subtree first. Its output slot
        aliases with the parent's output slot.
     b. Visit the other subtree.
     c. Release the non-aliased input's slot.
     d. Write `{kWorkspaceSlot, out_offset}` on the parent.

Aliasing safety: every `_at_vv` helper reads both inputs before
writing output — documented in `cel_runtime.h` and enforced by a
runtime test (see §8.4).

Peak slot count = root's slot-Strahler. Realistic CEL bottoms out
at 1–3 slots (a `&&` chain is 1; `x.y.z == "lit"` is 1; `(a+b)*(c+d)`
is 2). Pathological (full balanced tree of depth N) hits `O(log N)`.

## 7. Codegen rewrite — `compiler/codegen/expr_lower.{h,cc}`

### 7.1 Interface

```cpp
namespace celwasm {

ABSL_MUST_USE_RESULT absl::StatusOr<LoweredFunction> LowerToEvalFunction(
    const TypedAst& ast, const StaticLayout& layout,
    absl::string_view func_name, WasmModule& mod);

}  // namespace celwasm
```

`LoweringContext` shrinks to a handful of fields: the module pointer,
param indices, current local count, a reference to
`layout.annotations`, a reference to the frozen `OverloadTable`.
Gone: `idents`, `scratch_slot`, `prologue_setups`,
`EmitCheckedArithmetic`, per-visitor helper-string plumbing. All of
that data now lives in `NodeAnnotation` + `OverloadTable`.

### 7.2 `LowerExpr` — switch on kind, read annotations, emit

The walk itself is unchanged: post-order, gather children, emit the
current node using children's refs as operands. What changes is *what
happens at each node*: today a nested `if (expr.kind() == LITERAL)
… else if (expr.kind() == CALL && is_arith) …` picks the emission
shape from the AST kind and re-derives types. Tomorrow the walker
dispatches on `expr.kind()` and looks up the pre-computed facts.

```cpp
// Pseudocode — real version lives in expr_lower.cc.
BinaryenExpressionRef LowerExpr(LoweringContext& ctx,
                                const cel::Expr& expr) {
  std::vector<BinaryenExpressionRef> child_refs;
  for (const cel::Expr* c : Children(expr)) {
    child_refs.push_back(LowerExpr(ctx, *c));
  }
  const NodeAnnotation& a = *ctx.annotations.Find(expr.id());
  switch (expr.kind()) {
    case kConst:
      // Bool/int/uint/double/null/string/bytes all end up here.
      // Storage is always kStaticRodata — rodata is unconditional,
      // there is no fallback path (§4.2, §6.2.1).
      return EmitStorageLoad(ctx, a.storage);

    case kIdent:
      // Storage is kLocal; value is the CelValue offset held in the local.
      return BinaryenLocalGet(ctx.mod, a.storage.payload,
                              BinaryenTypeInt32());

    case kSelect:
      // a.field_number tells us which field; a.storage.payload is the
      // workspace slot the select's CelValue lands in.
      return EmitSelect(ctx, expr, a, child_refs[0]);

    case kCall: {
      const OverloadImpl& h = ctx.overloads.LookupById(a.overload_id);
      DCHECK(a.storage.kind == StorageKind::kWorkspaceSlot);
      // Uniform ABI (§4.2): helper(out_slot, args…) -> void.
      // Custom calls prepend pattern_id; built-ins don't. No storage-
      // kind branch — every call result is a workspace slot.
      return h.pattern_id != 0
          ? EmitCallCustom(ctx, h, a.storage.payload, child_refs)
          : EmitCallBuiltin(ctx, h, a.storage.payload, child_refs);
    }

    case kList:
      // Empty-then-populate (§4.7.3): cel_list_create(out_slot) then
      // one cel_list_append per element. Each element's value is
      // already in its own workspace slot by post-order.
      return EmitListBuild(ctx, expr, a, child_refs);

    case kCreateMap:
      // Empty-then-populate (§4.7.2): cel_map_create(out_slot) then
      // one cel_map_insert(out_slot, key_slot, value_slot) per entry.
      return EmitMapBuild(ctx, expr, a, child_refs);

    case kCreateStruct:
      // Proto message literal (§4.7.1): cel_host.cel_make_message
      // (type_id, out_slot) then one cel_host.cel_set_field per
      // field. Empty-then-populate like maps/lists; no pre-staged
      // args_ptr, no MessagePattern.
      return EmitMakeMessage(ctx, expr, a, child_refs);

    case kComprehension:
      return EmitComprehension(ctx, expr, a, child_refs);   // see §10
  }
}
```

Each emitter (`EmitStorageLoad`, `EmitSelect`, `EmitHelperCallSlotOut`,
…) is short — under the 60-line lint ceiling — and does one thing.

### 7.3 What disappears

Deletions (by file, for commit-message clarity):

  - `expr_lower.cc`:
    - `LoweringContext::idents` field and its builder.
    - `LoweringContext::scratch_slot` + `GetScratchSlotLocal()`.
    - `LoweringContext::prologue_setups`.
    - `EmitCheckedArithmetic` (~60 lines).
    - `BuildParamList`'s boxing-setup emission (moves to ResolvePass
      + LayoutPass).
    - Every `cel_make_{bool,int,uint,double,null}` call site in
      literal lowering.
    - Raw-scalar arith paths (`BinaryenBinary(IntAdd, …)` etc.) —
      all arith goes through `cel_*_at_vv`.
    - Every hand-written "which helper to call" ladder; replaced by
      `OverloadTable::LookupById(a.overload_id)`.
  - `cel_runtime.h` / `cel_runtime.c`:
    - `cel_int_{add,sub,mul,div,mod}_at_ii` and siblings.
    - `cel_uint_{add,sub,mul,div,mod}_at_uu` and siblings.
    - Potentially `cel_bool_from_value`, `cel_int_from_value`,
      `cel_uint_from_value`, `cel_double_from_value` if no codegen
      path remains (audit during Slice 7).

Scalar `cel_make_*` stay in the header — needed by host code boxing
user-supplied idents at the boundary, and by `cel_runtime_test`. But
they become dead code on the codegen path.

### 7.4 What stays

  - Import declarations — the scaffolding stays but the driver
    changes. A new helper `DeclareImportsFromTable(mod, used_overloads,
    table)` iterates each `overload_id` the codegen emitted, calls
    `table.UsedImports(...)` to get unique `(ImportModule, name)`
    pairs, and emits `AddFunctionImport(name, ImportModuleName(module),
    name, …)` for each. The expr module ends up importing from two
    modules: `"cel"` for runtime helpers and `"cel_host"` for custom
    trampolines + the fixed host functions (`get_field`, `has_field`,
    `message_eq`, `cel_make_message`). The old `ImportCel2` /
    per-helper-name hand-imports are gone.
  - `DeclareHostImports` for the fixed host surface: `cel_get_field`,
    `cel_has_field`, `cel_set_field`, `cel_message_eq`,
    `cel_make_message`, `cel_host_call_custom` — still declared once
    up front (per `feedback_no_lazy_imports`), not driven by the
    table, because the table doesn't know about them (they aren't
    overloads). Module name is hard-coded `"cel_host"` at this one
    site.
  - The `cel_env` logging import (`cel_log`) — one site in codegen,
    unchanged; not an overload, not a host function surface.
  - The `cel.abi` custom section (M3), now extended with
    `custom_functions[]` (§4.6).
  - `cel_refs` table emission (already per-expr, unchanged).

## 8. Runtime changes — `compiler/runtime/*`

### 8.1 Build flags — `compiler/runtime/BUILD.bazel`

```
-Wl,--import-memory=cel,memory
-Wl,--allow-undefined-file=$(location :wasm_imports.txt)
-Wl,--export=cel_alloc
-Wl,--export=cel_and
-Wl,--export=cel_or
…
```

`--export-all` retires. Every exported symbol is explicit so new
additions are visible in code review.

### 8.2 Arena cursor — fixed memory-base offsets

Today `g_cel_arena` is a `static` struct at `cel_runtime.c:67`.
After the flip, the runtime cannot store per-instance mutable state
in C globals inside its own memory (it no longer owns the memory).

**Decision: in-memory at a fixed offset.** The arena cursor lives
at bytes `8..15` of linear memory — two `uint32_t`s (bump pointer
at offset 8, high watermark at offset 12). Bytes `0..7` are the
memory-base zero sentinel (kept free of CelValue headers so a
`0` offset always reads as an obvious invalid value).

Both modules see the cursor through plain `i32.load` / `i32.store`
at known constant offsets; no wasm-globals machinery. Rationale:

  - **No wasm-ld dependency.** Avoids relying on `wasm-ld` correctly
    emitting exported mutable globals from `__attribute__((visibility("default")))`
    C globals. That path works today, but it's a moving part that
    breaks on toolchain upgrades for no benefit.
  - **`cel_reset` is one `i32.store offset=8`.** Trivial in both
    C (`*(uint32_t*)8 = base`) and Binaryen-emitted wasm.
  - **Full memory dumps are self-contained.** Dumping linear
    memory to a file captures the entire allocator state —
    cursor, watermark, and every CelValue header and span payload
    live side by side. Debug-layout walkers (§11 Slice 11) get
    this for free.
  - **Cross-instance isolation holds.** Each expr instance has its
    own memory → its own bytes 8..15 → its own arena. The runtime
    module's C code reads the cursor via `memory_base + 8`, which
    resolves to the imported memory, not to any shared runtime
    state.

```c
// compiler/runtime/cel_runtime.c — illustrative
#define CEL_ARENA_CURSOR_OFFSET  8u
#define CEL_ARENA_LIMIT_OFFSET   12u

static inline uint32_t cel_arena_cursor(void) {
  return *(uint32_t*)CEL_ARENA_CURSOR_OFFSET;
}
static inline void cel_arena_set_cursor(uint32_t v) {
  *(uint32_t*)CEL_ARENA_CURSOR_OFFSET = v;
}
// Analogous for limit at offset 12.

void cel_reset(uint32_t arena_base, uint32_t arena_limit) {
  cel_arena_set_cursor(arena_base);
  *(uint32_t*)CEL_ARENA_LIMIT_OFFSET = arena_limit;
}

uint32_t cel_alloc(uint32_t nbytes) {
  uint32_t cur = cel_arena_cursor();
  uint32_t next = cur + ((nbytes + 7u) & ~7u);
  if (next > *(uint32_t*)CEL_ARENA_LIMIT_OFFSET) return 0;
  cel_arena_set_cursor(next);
  return cur;
}
```

**Codegen side.** Binaryen emits `i32.load offset=8` /
`i32.store offset=8` directly — no import, no helper call. One
constant per site.

### 8.3 `cel_reset` ownership

Today: runtime exports `cel_reset`; host calls it before every
`CallEval`.

Tomorrow: **expr module exports `cel_reset`**, which resets the arena
cursor (byte 8 of reserved region) and any workspace bookkeeping.
Host calls the expr's `cel_reset` between evals, or — if it wants
fresh memory — reinstantiates the expr module (which gives it
per-instance isolation for free).

Runtime-side `cel_reset` retires.

### 8.4 Runtime test for aliasing ABI

Slot aliasing in `SlotAllocator` depends on the invariant that every
`_at_vv` helper reads both inputs before writing output. Existing
comment in `cel_runtime.h` documents this; add an explicit test:

```cpp
// compiler/runtime/cel_runtime_test.cc
TEST(CelRuntime, VvHelpersLoadInputsBeforeWritingOutput) {
  uint32_t x = cel_make_int(7);
  cel_int_add_at_vv(x, x, x);              // aliased: out == a == b
  EXPECT_EQ(cel_value_at(x)->payload.i, 14);
  // … one case per _vv helper.
}
```

This test is load-bearing evidence that SlotAllocator's aliasing is
safe. Must run under `bazel test //compiler/...` (default suite, not
`manual`).

### 8.5 Error provenance (optional, Slice 11)

Extend `CelValue.payload.err` to carry the expr_id that produced the
error:

```c
// cel_runtime.h
typedef struct {
  uint16_t code;      // CEL_ERR_*
  uint16_t _pad;
  uint32_t expr_id;   // originating expr.id()
  uint32_t msg_off;
  uint32_t msg_len;
} CelErrorPayload;
_Static_assert(sizeof(CelErrorPayload) <= 16, "fits in CelValue payload");
```

`cel_make_error` gains an `expr_id` param. Codegen supplies the
literal `expr_id` at each call site; the host diagnostic includes
source position via the `expr_id → parse location` map the checker
already maintains (`CheckedExpr.source_info`).

Survives slot reuse because the ERROR CelValue travels through slots
— its payload is copied, not reconstructed.

## 9. Host loader — `compiler/host/host_loader.{h,cc}`

### 9.1 Two-phase instantiation

```cpp
// Pseudocode.
absl::StatusOr<EvalInstance> LoadEval(const CompiledEval& compiled) {
  // Phase 1: instantiate the expr module. It defines + exports memory.
  // Its imports are cel.cel_alloc, cel.cel_and, etc. Satisfy with
  // trampolines that rebind in phase 2.
  Trampolines t = InstallTrampolines(linker);
  wasmtime_instance_t expr = wasmtime_linker_instantiate(linker, expr_mod);

  // Phase 2: instantiate the runtime module. Its only import is
  // cel.memory — provide expr's exported memory.
  wasmtime_memory_t expr_mem = wasmtime_instance_export_get(expr, "memory");
  wasmtime_linker_define_memory(linker, "cel", "memory", expr_mem);
  wasmtime_instance_t rt = wasmtime_linker_instantiate(linker, rt_mod);

  // Phase 3: rebind trampolines to the real runtime exports.
  BindTrampolines(t, rt);

  return EvalInstance{expr, rt, ...};
}
```

### 9.2 Deletions

  - `cel_alloc(24)` call for the sret slot at `host_loader.cc:429` —
    the sret slot is a `kWorkspaceSlot` offset known at compile time;
    the host passes it as the out-slot param (or `eval(0)` means "use
    the default output slot", which the expr reads from its layout).
  - `cel_reset` call pre-`CallEval` — replaced by the expr's own
    `cel_reset` export.

## 10. Comprehensions (M5) — how this design absorbs them

### 10.1 What M5 adds

  - Comprehension macros (`all`, `exists`, `exists_one`, `map`,
    `filter`) — already rewritten to explicit AST by cel-cpp's macro
    expander.
  - Nested scopes with inner-wins shadowing (`langdef.md`).
  - Per-iteration bindings for iter var and accu var.

### 10.2 How the annotations absorb it

  - **Scope push.** ResolvePass pushes a `ScopeFrame` on comprehension
    entry, allocates two locals (iter, accu), assigns a fresh
    `scope_id`. Idents inside the body resolve against the frame;
    their `NodeAnnotation` gets `{local_index, scope_id}` of the
    bound name. Outer references keep the outer `scope_id` —
    shadowing is free.
  - **Scope pop.** On body exit, pop the frame. Already-assigned
    `scope_id`s persist on their nodes.
  - **Accumulator storage.** The accu var lives in a wasm local for
    the comprehension's lifetime. LayoutPass reserves workspace slots
    with `PushScope` / `PopScope` semantics (§6.2.2) so per-iteration
    intermediates reuse across iterations.
  - **The comprehension node itself.** Its `storage` equals the
    accu's `kLocal` — the comprehension "aliases" its result through
    the accu. No new `StorageKind` needed.

### 10.3 Future-compat invariants

M5 must preserve:

  - **No new `NodeAnnotation` fields, no new `StorageKind`.**
    Comprehensions fit in today's fields.
  - **`scope_id` is write-once.** Once ResolvePass assigns it, no
    later pass changes it.
  - **Debug layout still works.** PushScope/PopScope are no-ops when
    `debug_mode = true` (fresh slots per node, no release).

If M5 cannot meet these, the design is wrong and this doc updates
before M5 lands, not after.

## 11. Implementation plan

### 11.1 Strategy: build `compiler_v2/` end-to-end, swap at the end

The rewrite touches new passes (`ResolvePass`, `LayoutPass`), a new
ABI (uniform slot-out), a new memory-ownership model, and runtime
surgery. Migrating `compiler/` in place means every slice has to
keep v1's ABI and v2's ABI coexisting in the same translation
units — which is where complexity actually lives, not in the new
code itself.

**We build a parallel `compiler_v2/` tree instead.** `compiler/`
remains untouched (and shipping) throughout the rewrite. Each slice
grows `compiler_v2/` by a vertical capability — a new expression
kind that works end-to-end from parse → check → resolve → layout →
emit → run. The first slice ships v2 evaluating `42`; the last
slice ships v2 at feature parity with today's M3 tip, passes the
same e2e fixtures, and renames `compiler_v2/` → `compiler/`
(deleting v1).

**Why this is better for an LLM-driven rewrite.**

  - **New code is cheaper than migration.** Each slice is "write
    v2 file X" with no in-place diffs to v1. No maintaining an
    ABI bridge. No keeping deleted-in-v2 code alive for one more
    slice. The LLM's context budget spends on the design, not on
    reconciling two in-flight designs.
  - **v1 stays runnable throughout.** `bazel test //compiler/...`
    is always green because it never moves. The user can ship bug
    fixes against v1 while v2 is being built (low probability, but
    the option costs us nothing).
  - **Each slice is end-to-end executable.** The v2 tree is a
    standalone compiler from Slice 1 onward: `bazel run
    //compiler_v2/cli:celwasmc_v2 -- -e "42"` works. Slices add
    capability, not ABI compatibility. This is the load-bearing
    property of the plan — no slice leaves v2 in a half-built
    state.
  - **Deletions land for free at the swap.** The "what disappears"
    list in §7.3 is implicit — those v1 surfaces never appear in
    v2. One commit at the swap (`git rm -r compiler/ && git mv
    compiler_v2 compiler`) retires them wholesale.

**Tradeoffs we accept.**

  - **No incremental CLI replacement.** Users calling `celwasmc`
    before the swap get v1; after the swap, v2. There is no "flag
    to opt into v2" — that's a feature flag we'd have to maintain,
    and the whole point of this strategy is not to.
  - **Disk cost.** A second tree sits in `compiler_v2/` for the
    lifetime of the rewrite (~3–4 weeks). Worth it for the
    cognitive cost it saves.
  - **Reference-copy risk.** v2 files that copy v1 literally (e.g.
    M3 proto-select host code that's proven) must be transcribed,
    not symlinked. Each slice's description calls out what gets
    copied from v1 and cites the exact source path — no guessing.

### 11.2 `compiler_v2/` directory layout

Mirrors `compiler/` one-for-one. New file names carry no `_v2`
suffix internally (the directory already tags them); at the swap,
rename is a pure `git mv`.

```
compiler_v2/
├── BUILD.bazel                      # root; re-exports //compiler_v2/... as a filegroup
├── ir/
│   ├── annotations.h                # NodeAnnotation (§3, §4.1)
│   ├── typed_ast.h                  # (copied from compiler/ir/typed_ast.h verbatim in Slice 1)
│   └── BUILD.bazel
├── frontend/
│   ├── parse.{h,cc}                 # thin wrapper over cel-cpp parser (copy from compiler/frontend)
│   ├── check.{h,cc}                 # thin wrapper over cel-cpp checker
│   └── BUILD.bazel
├── codegen/
│   ├── overload_table.{h,cc,_test.cc}       # §4.3
│   ├── resolve_pass.{h,cc,_test.cc}         # §5
│   ├── layout_pass.{h,cc,_test.cc}          # §6
│   ├── static_memory_builder.{h,cc,_test.cc}# §6.2.1
│   ├── slot_allocator.{h,cc,_test.cc}       # §6.2.2
│   ├── expr_lower.{h,cc,_test.cc}           # §7, annotation-driven
│   ├── module.{h,cc,_test.cc}               # §8, two-phase wiring
│   └── BUILD.bazel
├── runtime/
│   ├── cel_runtime.{h,c,_test.cc}   # slimmed helper set, uniform slot-out ABI
│   ├── cel_runtime.wasm             # cross-compiled under §8.1 flags
│   ├── cel_runtime_wasm_bytes.{h,cc}
│   ├── wasm_imports.txt
│   └── BUILD.bazel
├── host/
│   ├── host_loader.{h,cc,_test.cc}  # §9, two-phase instantiation
│   ├── cel_host.{h,cc,_test.cc}     # get_field/set_field/has_field/make_message/call_custom
│   ├── cel_log.{h,cc,_test.cc}      # copied from compiler/host (already-new surface)
│   └── BUILD.bazel
├── cli/
│   ├── celwasmc_v2.cc               # CLI entry point; temp name until swap
│   └── BUILD.bazel
├── e2e/
│   ├── eval_test.cc                 # v2 e2e; grows per slice
│   ├── testdata/
│   │   └── customer.proto           # copied from compiler/testdata
│   └── BUILD.bazel
└── bench/
    └── eval_bench.cc                # v2 bench; smoke-tests parity vs v1
```

**Reuse from v1.** The following are copied verbatim (or nearly
so) in Slice 1 and not rewritten — they're already-new code for
our purposes:

  - `compiler/ir/typed_ast.h` and `typed_ast.cc`
  - `compiler/frontend/parse.*` and `check.*`
  - `compiler/host/cel_log.*` (the log-skill host surface)
  - `compiler/testdata/customer.proto` (and any other .proto fixtures)
  - Most of `compiler/host/host_loader.*`'s wasmtime boilerplate
    (error mapping, function-import wiring) — reimagined in Slice 1.

**Rewritten from scratch.** Everything under `compiler_v2/codegen/`
and `compiler_v2/runtime/`. The ABI and memory model is new; there
is no value in a file-level diff against v1.

### 11.3 Invariants that hold across every slice

Non-negotiable — violations mean the slice is misscoped and needs
to split:

  - **v2 is always end-to-end executable.** Every slice ships a
    `compiler_v2/cli/celwasmc_v2` binary that compiles **at least**
    the expression families the slice claims, runs them under the
    v2 runtime+host, and returns the right answer. No slice leaves
    v2 half-built.
  - **v2 tests are always green on HEAD.** `bazel test //compiler_v2/...`
    passes after every slice. No trailing "will fix" commits.
  - **v1 tests are always green on HEAD.** `bazel test //compiler/...`
    passes after every slice. The v2 slices do not touch v1 files.
    If a slice wants to share code, it copies — not symlinks, not
    `cc_library` cross-references.
  - **Every slice ticks ≥ 1 testing-checklist row.** Rows are
    retargeted at v2 paths (e.g. `compiler_v2/codegen/...` instead
    of `compiler/codegen/...`). Post-swap, the checklist is
    re-based to the renamed paths in one commit.
  - **Each slice is one squashable commit.** Revert = `git revert
    <sha>` (which deletes the new v2 files and stops the CLI at the
    previous slice's capability). v1 is unaffected.

### 11.4 Dependency graph (v2 vertical slices)

Each slice adds capability to the v2 tree. Earlier slices are
strict prerequisites — a later slice's e2e tests depend on the
infrastructure shipped by earlier ones. The graph is deliberately
compact: micro-slices that don't change executing behavior are
merged into their dependency-neighbours to keep PR overhead low.

```
  S1. Bootstrap (runtime + two-phase loader + int literal e2e)
         │    [runtime with bytes-8/12 arena, directory skeleton,
         │     minimal codegen: -e "42" prints 42]
         ▼
  S2. All scalar literals (bool/int/uint/double/null/string/bytes)
         │
         ▼
  S3. Symbol table & pipeline scaffolding
         │  [NodeAnnotation schema + empty OverloadTable +
         │   ResolvePass (ident/field populating only) +
         │   LayoutPass scaffold. Codegen-inert; behavior unchanged.]
         ▼
  S4. Idents + SelectExpr reads (M3 G2 parity in v2)
         │  [resolve_pass populates local_index / field_number;
         │   kSelect → cel_host.cel_get_field]
         ▼
  S5. OverloadTable seeding + full built-in overload set + uniform ABI
         │  [kBuiltinSeeds covers every StandardOverloadIds::k*;
         │   every helper slot-out shape from day one; kCall wired;
         │   coverage tripwire live]
         ▼
  S6. has(msg.field) + message equality (M3 G3/G4 parity)
         │
         ▼
  S7. Custom functions (single `cel_host_call_custom` trampoline)
         │
         ▼
  S8. Map + list literals (runtime empty-then-populate primitives)
         │
         ▼
  S9. Proto literals (host empty-then-populate primitives)
         │
         ▼
  S10. Sethi–Ullman slot allocation + debug layout + error provenance
         │   [DEFERRED optimisation; lands after all features e2e-green]
         ▼
  S11. v1 M3 tip parity audit + bench parity
         │
         ▼
  S12. Swap — `git mv compiler_v2 compiler`
```

**Critical path is linear (S1 → S12).** No fan-out; v2 grows as a
single trunk. The only parallelisable work is authoring within a
slice (e.g. two authors splitting S5's overload seeds by type
family while a third writes the aliasing test for S10).

**Why these merges vs an earlier split.**
  - S3 bundles NodeAnnotation + empty OverloadTable + ResolvePass
    + empty LayoutPass because none of them change executing
    behavior — codegen ignores them. Splitting would be four tiny
    PRs that all have to land before any user-visible capability
    moves. Merging holds zero risk to `master` (v2 tests green;
    v1 untouched) and cuts PR overhead 4×.
  - S5 bundles the full built-in overload set with the uniform-
    ABI wiring. In v2 the uniform-slot-out shape is the only shape
    — there's no return-offset helper to migrate off of — so the
    uniform-ABI "migration" is just "implement helpers correctly
    from day one". Splitting "wire the table" from "fill in the
    helpers" would produce an intermediate state where kCall
    dispatches into a half-populated table.
  - Sethi–Ullman (S10) stays standalone and late — see the note
    below.

**Naive slot allocator up front, Sethi–Ullman at S10.** Slices 1–9
use a naive allocator that hands out a fresh 24-byte slot per node
and never releases (functionally equivalent to `debug_layout = true`).
This is correct from Slice 1 on — the only cost is workspace size,
bounded linearly in AST size. Sethi–Ullman with aliasing-safety
tests lands as S10 once every feature is e2e-green, so if the
optimisation breaks something subtle we bisect against a known-
good feature-complete v2 — not an in-flight feature set.

### 11.5 Slice-by-slice plan

Each slice is described as: **scope** (what lands), **e2e check**
(observable-from-CLI behavior that proves it works), **tests**,
**risk** and **mitigation**, **effort** (optimistic days). Entry
criterion is always "previous slice shipped"; exit criterion is
always "e2e check runs green and tests pass".

---

#### Slice 1 — Bootstrap: `-e "42"` end-to-end (2 days)

**Scope.** Enough v2 to evaluate `42` under the v2 CLI.

  - `compiler_v2/` directory structure (§11.2).
  - `compiler_v2/runtime/cel_runtime.{h,c}`: `CelValue` layout,
    `cel_alloc`, `cel_reset`, `cel_make_int`. **Arena cursor at
    fixed memory-base bytes 8/12** (§8.2) — no wasm globals.
    Build flags per §8.1.
  - `compiler_v2/host/host_loader.{h,cc}`: two-phase instantiation
    (§9.1).
  - `compiler_v2/frontend/{parse,check}.{h,cc}`: copied verbatim
    from v1 (already thin wrappers over cel-cpp).
  - `compiler_v2/codegen/expr_lower.{h,cc}`: minimal — `kConst` for
    `int64` only, via `.rodata`.
  - `compiler_v2/codegen/static_memory_builder.{h,cc}` with
    `AppendScalar` for int.
  - `compiler_v2/cli/celwasmc_v2.cc`: CLI entry.
  - `compiler_v2/e2e/eval_test.cc`: `EvalInt("42", 42)`.

**E2E check.** `bazel run //compiler_v2/cli:celwasmc_v2 -- -e "42"`
prints `42`.

**Tests.**
  - `cel_runtime_test`: arena-at-offset-8 round-trip; `cel_alloc`
    + `cel_make_int`.
  - `host_loader_test`: two-phase instantiation with memory shared
    via bytes 8/12 cursor.
  - `static_memory_builder_test::AppendScalar` int byte layout.
  - `expr_lower_test`: emits `i32.const <offset>` for `kConst` int.
  - `e2e/eval_test`: `EvalInt("42", 42)`.

**Risk.** The bootstrap concentrates scaffolding. **Mitigation:**
accept this is the longest slice — pay the cost once.

**Effort.** 2 days.

---

#### Slice 2 — All scalar literals (1 day)

**Scope.** `StaticMemoryBuilder::AppendScalar` for bool / uint /
double / null; `AppendSpan` for string / bytes (header + payload +
alignment pad). `expr_lower.cc` `kConst` arm per kind.

**E2E check.** `-e "true"`, `-e "3.14"`, `-e "\"hello\""`,
`-e "b\"x\""`, `-e "null"` each print the expected value.

**Tests.** Per-kind byte layout; per-kind emission; per-kind e2e.

**Risk.** Span-payload alignment pad off by one byte.
**Mitigation:** explicit test asserting offset of next header
after a span payload.

**Effort.** 1 day.

---

#### Slice 3 — Symbol table & pipeline scaffolding (1.5 days)

**Scope.** Merge that prepares the pipeline but doesn't change
executing behavior — codegen ignores the new fields until S4.

  - `compiler_v2/ir/annotations.h` extends `NodeAnnotation` with
    `overload_id`, `local_index`, `scope_id`, `storage` (§4.1).
  - `compiler_v2/codegen/overload_table.{h,cc}` with
    `OverloadTableBuilder` / `OverloadTable` / `kBuiltinSeeds[]`
    seeded with an **empty list** (real entries land in S5).
    `RegisterCustom` + `AlreadyExists` collision rule work now.
  - `compiler_v2/codegen/resolve_pass.{h,cc}` lands; populates
    `local_index` (ident) and `field_number` (SelectExpr) only.
    Overload interning is stubbed to "lookup returns nullptr".
  - `compiler_v2/codegen/layout_pass.{h,cc}` lands as no-op —
    `kStaticRodata` for consts (consumed by S2), everything else
    unset.
  - Pipeline driver: `parse → check → resolve → layout → emit`.
    Codegen still only handles `kConst`.

**E2E check.** S1–S2 fixtures remain green through the new
pipeline. No new expression capability.

**Tests.** `annotations_test` field round-trip;
`overload_table_test` builder + `AlreadyExists`; `resolve_pass_test`
on fixture ASTs (ident + select); `layout_pass_test` no-op.

**Risk.** None — pass-through refactor.

**Effort.** 1.5 days.

---

#### Slice 4 — Idents + SelectExpr reads (M3 G2 parity) (2 days)

**Scope.**

  - `compiler_v2/host/cel_host.{h,cc}` with `cel_get_field` and
    `cel_has_field` (transcribed from v1 host/cel_host — proven
    code). Fixed host imports declared up front per
    `feedback_no_lazy_imports`.
  - `expr_lower.cc` grows arms for `kIdent` (returns local-held
    offset) and `kSelect` (non-`test_only` dispatches to
    `cel_host.cel_get_field` using `NodeAnnotation::field_number`).
  - LayoutPass assigns `kWorkspaceSlot` for `kSelect` (naive
    allocator — fresh slot, no release).

**E2E check.** Against a `Customer` proto fixture copied to
`compiler_v2/e2e/testdata/`: `-e "x.name" -V x=<Customer>`
returns the string; `-e "x.age"` returns the int; `-e "x.order.total"`
(nested) works.

**Tests.** `resolve_pass_test` populated on `Customer` ASTs;
`expr_lower_test` for `kIdent`/`kSelect`; `e2e/eval_test` with
the `Customer` fixture.

**Risk.** Descriptor pool lifetime — `field_number` must be
resolved while the pool is live (M3 Slice G2 lesson).
**Mitigation:** transcribed from v1; same invariants.

**Effort.** 2 days.

---

#### Slice 5 — Full built-in overload set + kCall wired (3 days)

**Scope.** The big slice. The uniform slot-out ABI is the only ABI
that exists in v2, so there is no "migration" — every helper is
born slot-out.

  - `cel_runtime.h` adds the full helper set in one go:
    int/uint/double/bool arithmetic + comparisons
    (`_add/sub/mul/div/mod/neg/lt/le/gt/ge/eq/ne_at_vv`);
    string/bytes ops (`_concat/_eq/_contains/_starts_with/_ends_with/_matches_at_vv`);
    `cel_size_{string,bytes,list,map}`; 3VL helpers from M4 Slice A
    (already exist in v1, transcribed).
  - Each helper carries a cel-cpp parity comment citing the
    source-of-truth in `third_party/cel-cpp/runtime/standard/`.
  - `OverloadTable::kBuiltinSeeds` becomes non-empty — one row per
    `StandardOverloadIds::k*` (or explicit unimplemented list).
    Coverage tripwire in `overload_table_test`.
  - `ResolvePass` now populates `overload_id` on `kCall` nodes.
  - `expr_lower.cc` grows `kCall` arm: `OverloadTable::LookupById(a.overload_id)`,
    emit `helper(out_slot, args…)` — uniform shape.
  - `CompileOptions::allowed_overloads` filter lands.

**E2E check.** Full arithmetic + comparison + string/bytes ops
all round-trip. `-e "(1 + 2) * 3 - 4"` → `5`; `-e "10 / 0"` → ERROR;
`-e "\"a\" + \"b\""` → `"ab"`; `-e "size(\"hello\")"` → `5`;
`-e "\"abc\".contains(\"b\")"` → `true`.

**Tests.**
  - `cel_runtime_test`: every new helper + aliasing proof
    (deferred Sethi–Ullman means aliasing isn't triggered yet, but
    the test locks the invariant so S10 can't regress it).
  - `overload_table_test`: coverage tripwire — every
    `StandardOverloadIds::k*` mapped or explicitly unimplemented.
  - `e2e/eval_test`: spec-cited fixtures per cel-cpp for each
    overload family; a 10k randomised-fixture run cross-checked
    against cel-cpp's interpreter.

**Risk.** cel-cpp parity drift at scale. **Mitigation:** the
randomised fixture run is mandatory; parity comments enforce the
manual audit trail.

**Effort.** 3 days.

---

#### Slice 6 — `has(msg.field)` + message equality (M3 G3/G4 parity) (1 day)

**Scope.** `expr_lower.cc` `kSelect` dispatches on `test_only`
(reads → `cel_get_field`; tests → `cel_has_field`). `kCall` for
`_==_` on messages lowers to `cel_host.cel_message_eq`.
Transcribed from v1 M3 slices G3/G4. `cel_host` gets `cel_message_eq`.

**E2E check.** `has(x.name)`, `has(x.order)`, `x == y` (proto-to-
proto), `has(x.order.items)`.

**Tests.** Transcribed G3/G4 e2e tests under `compiler_v2/e2e/eval_test.cc`.

**Risk.** Low — straight port.

**Effort.** 1 day.

---

#### Slice 7 — Custom functions (single trampoline) (2 days)

**Scope.**

  - `OverloadTableBuilder::RegisterCustom` (§4.3) + the single
    `cel_host.cel_host_call_custom(pattern_id, out_slot, args_ptr)`
    host import (§4.6.1). Args-staging region reserved by
    LayoutPass; codegen writes arg offsets into it before the
    call.
  - `CompileOptions::RegisterFunction` plumbing through frontend
    to `OverloadTableBuilder::RegisterCustom`.
  - `cel.abi` custom section extended with `custom_functions[]`
    (arity per pattern).

**E2E check.** Fixture `my.upper(string) -> string` at arity 1
wires register → compile → load → call → return. Per-arity e2e in
`{0, 1, 2, 3, 8}`, all on the single trampoline with different
`pattern_id`s.

**Tests.** `overload_table_test`: `RegisterCustom` collision /
no-override; `e2e/eval_test`: per-arity + unbound-pattern negative
+ arity-mismatch negative; `cel_host_test`: args-staging decode.

**Risk.** Args-staging overwrite between sibling custom calls.
**Mitigation:** LayoutPass emits arg-writes immediately before
each call with no interleaved calls; unit test verifies emit order.

**Effort.** 2 days.

---

#### Slice 8 — Map + list literals (runtime primitives) (2 days)

**Scope.** `cel_runtime` gains `cel_map_create`, `cel_map_insert`,
`cel_map_lookup`, `cel_map_size`; `cel_list_create`,
`cel_list_append`, `cel_list_at`, `cel_list_size` (§4.7.2, §4.7.3).
`expr_lower.cc` `kCreateMap` / `kList` arms: empty-then-populate
emit sequence. Overload arms for `size`, `list[i]`, `map[k]`,
`k in map`, `v in list`.

**E2E check.** `{"a": 1, "b": 2}.size() == 2`; `[1,2,3][1] == 2`;
`"a" in {"a": 1}`; `2 in [1,2,3]`. Duplicate-key map → ERROR.

**Tests.** `cel_runtime_test`: every map/list helper + duplicate
key + OOB index; `e2e/eval_test`: spec-cited fixtures.

**Risk.** Map semantics parity with cel-cpp. **Mitigation:** mirror
`runtime/standard/map_functions.cc` exactly; parity comment cites
source.

**Effort.** 2 days.

---

#### Slice 9 — Proto literals (host primitives) (2 days)

**Scope.**

  - `cel_host` adds `cel_make_message(type_id, out_slot)` and
    `cel_set_field(msg_slot, field_id, value_slot)` (§4.7.1).
  - Compile-time `type_id` interning against the descriptor pool;
    `(type_id, field_id) → setter` resolution at bind time.
  - `expr_lower.cc` `kCreateStruct` with `message_name` arm.

**E2E check.** `Customer{name: "a", age: 3}.name == "a"`;
`Customer{name: x.first + x.last, age: 30}` (non-literal values
work via the same emit sequence).

**Tests.** `cel_host_test` on `cel_make_message` + `cel_set_field`;
`e2e/eval_test` with proto literal (literal values); proto literal
(computed values); unregistered descriptor → compile-time
`InvalidArgument`; unknown field_id → ERROR.

**Risk.** Unknown field on a known descriptor should fail at
*compile time* (checker rejects it). **Mitigation:** fail fast
in frontend; host only handles checker-validated descriptors.

**Effort.** 2 days.

---

#### Slice 10 — Sethi–Ullman + debug layout + error provenance (3 days)

**Scope.** The deferred optimisation and quality-of-life features.
Bundled because they all touch `LayoutPass` / `SlotAllocator`.

  - `SlotAllocator::Release()` stops being a no-op; LayoutPass
    Phase-2 walk computes slot-Strahler and emits aliasing visit
    order (§6.3). `LayoutOptions::debug_layout = true` restores
    the naive path.
  - `CelErrorPayload` grows `expr_id`; `cel_make_error` takes it;
    codegen fills the literal `expr_id` at emit sites; host
    diagnostics resolve it to source position via
    `CheckedExpr.source_info`.

**E2E check.**
  - `celwasmc_v2 --dump-workspace -e "(a+b)*(c+d)"` drops from 5
    slots (naive) to 2 (Sethi–Ullman).
  - `--debug-layout` and production mode produce identical
    evaluation results on the full e2e suite.
  - `(1 << 62) + (1 << 62)` returns an error whose `expr_id`
    resolves to the `+` node's source position.

**Tests.** `slot_allocator_test`: slot-Strahler unit
(`((a+b)+c)+d` → 1, `(a+b)*(c+d)` → 2); `cel_runtime_test`:
aliasing proof per `_at_vv` helper (§8.4); `e2e/eval_test`:
debug-vs-production parity on all prior fixtures + the 10k
randomised corpus; error-provenance on overflow / div-by-zero /
type mismatch.

**Risk.** **Highest-risk slice in the rewrite.** Aliasing bugs in
`_at_vv` helpers are silent until a specific AST shape triggers
them. **Mitigation:**
  - Per-helper aliasing test (§8.4) mandatory.
  - The randomised fixture generator from S5 re-runs under both
    `debug_layout` and production; byte-for-byte identical results
    required.
  - Scheduled *after* S1–S9 — we bisect against a feature-complete
    known-good v2, not an in-flight one.

**Effort.** 3 days.

---

#### Slice 11 — v1 M3 tip parity audit + bench parity (2 days)

**Scope.** No new features — close gaps.

  - Copy `compiler/e2e/eval_test.cc` into `compiler_v2/e2e/` (drop
    v1-specific fixtures only if they're genuinely untranslatable),
    run, fix regressions.
  - Wire `compiler_v2/bench/eval_bench.cc` from v1; assert v2 is
    within 20% of v1 on existing microbenchmarks (expect v2 faster
    on most; flag slowdowns > 20%).
  - `scripts/lint.sh` clean for all `compiler_v2/` files. Any
    `// NOLINT` gets a bullet in
    `doc/implementation-plan/lint-backlog.md`.

**E2E check.** `bazel test //compiler_v2/... //compiler/...` all
green; v2's e2e fixture count ≥ v1's.

**Tests.** Parity audit — no new test cases.

**Risk.** Gap discovery. **Mitigation:** exhaustive by
construction.

**Effort.** 2 days.

---

#### Slice 12 — Swap: `compiler_v2/` → `compiler/` (0.5 day)

**Scope.** One commit; no code changes, only renames.

  - `git rm -r compiler/`
  - `git mv compiler_v2 compiler`
  - `git mv compiler/cli/celwasmc_v2.cc compiler/cli/celwasmc.cc`
    (+ BUILD.bazel target rename).
  - Sweep `//compiler_v2/...` → `//compiler/...` in any remaining
    references (docs, checklist, CLAUDE.md if any).
  - Update `doc/wasm-compiler-design.md` §7.0 / §7.3.
  - Close CLAUDE.md's symbol-table debt bullet.
  - Rename `m-mem-static-layout-pass.md` +
    `memory-ownership-flip.md` → `.obsolete.md`.
  - Mark this doc `Status: shipped` at the top.

**E2E check.** `bazel test //compiler/...` green; `rg 'compiler_v2'`
returns zero matches.

**Risk.** Missed path reference. **Mitigation:** pre-commit
`rg 'compiler_v2'` sweep.

**Effort.** 0.5 day.

---

**Total.** ~22 days optimistic critical-path. Realistically
4–5 calendar weeks with reviews, flakes, and randomised-fixture
debugging (especially S10).

### 11.6 Per-slice test strategy

| Slice | Unit | Integration / lowering | E2E |
|---|---|---|---|
| 1 | Arena-at-offset-8; `AppendScalar` int; `host_loader` two-phase | `kConst` int emits `i32.const <offset>` | `celwasmc_v2 -e "42"` → `42` |
| 2 | `Append{Scalar,Span}` per kind | `kConst` emission per kind | Scalar literal e2e per kind |
| 3 | `annotations_test`; `overload_table` builder + `AlreadyExists`; `resolve_pass`; `layout_pass` no-op | Pipeline refactor-only | All prior fixtures |
| 4 | `resolve_pass` populates `local_index`/`field_number`; `cel_host` read ops | `kIdent` / `kSelect` emission | `Customer` field reads |
| 5 | Every helper + aliasing test (locked); `overload_table` coverage tripwire | `kCall` dispatch | Full built-in overload set + 10k randomised vs cel-cpp |
| 6 | — | `kSelect.test_only` dispatch | `has(msg.field)` + message equality |
| 7 | `RegisterCustom` collision; args-staging decode | — | Per-arity customs; unbound / arity-mismatch negatives |
| 8 | `cel_map_*` / `cel_list_*` + duplicate / OOB | `kCreateMap` / `kList` emit | Map + list literal fixtures |
| 9 | `cel_make_message` / `cel_set_field` host tests | `kCreateStruct` emit | Proto literal fixtures; unregistered-descriptor negative |
| 10 | Slot-Strahler; `_at_vv` aliasing per §8.4; `CelErrorPayload.expr_id` | Debug vs prod slot count | Debug-vs-prod parity on 10k corpus; error provenance |
| 11 | — | — | v1 M3 fixtures green under v2; bench within 20% |
| 12 | — | — | `bazel test //compiler/...` green post-swap |

### 11.7 Testing-checklist updates

Add these rows to `doc/implementation-plan/testing-checklist.md`:

  - Static-literal lowering × each CEL scalar type (bool, int, uint, double, null)
  - Static-literal lowering × string
  - Static-literal lowering × bytes
  - Slot allocation × fixture tree shapes (linear chain, balanced tree, ladder)
  - Slot aliasing × each `_at_vv` helper
  - No-`cel_alloc` × static-only eval
  - Debug-layout parity × production layout (same inputs, same outputs, different memory)
  - Scope-id assignment × nested comprehension
  - Scope-id assignment × outer binding shadowed
  - Two-phase instantiation × fresh memory per eval module
  - Overload-table coverage × every `StandardOverloadIds::k*` from cel-cpp `common/standard_definitions.h` (mapped or explicitly unimplemented)
  - Overload-table lookup × size_{string, bytes, list, map} (polymorphic family)
  - Overload-table lookup × add_{int, uint, double, string, bytes, list, duration_duration, timestamp_duration}
  - Overload-table lookup × equals_{int, uint, double, bool, string, bytes, …}
  - Overload-table lookup × unknown cel_id (negative → 0)
  - Helper-name audit × every mapped runtime helper name is declared as a wasm import in the expr module
  - Import-module audit × every `kCelRuntime` helper is actually exported by `cel_runtime.wasm`; every `kCelHost` helper is bound by the host loader
  - Import-module dispatch × a call to a `kCelRuntime` helper emits `AddFunctionImport(… "cel" …)`; a call to a `kCelHost` custom emits `AddFunctionImport(… "cel_host" …)`
  - Custom-function registration × `RegisterCustom` collides with a built-in id → `AlreadyExists`
  - Custom-function registration × `RegisterCustom` collides with a prior custom → `AlreadyExists`
  - Spec-allowed function set × `CompileOptions::allowed_overloads` filter rejects a disallowed standard function with Unimplemented
  - Runtime parity × each mapped helper in `cel_runtime.h` has a cel-cpp source pointer in its declaration comment
  - Runtime parity × fixture expressions (`(1 << 62) * 4`, `"a" + "b"`, `duration("1h") + duration("30m")`, `timestamp("2020-01-01T00:00:00Z") + duration("24h")`) produce identical CelValues on our runtime and under cel-cpp's interpreter
  - Uniform ABI audit × every helper named by `kBuiltinSeeds` has wasm signature `(i32…) -> void` ending in `_at_v` / `_at_vv` / `_at_vvv`
  - Error provenance × arithmetic overflow
  - Error provenance × div-by-zero
  - Error provenance × type mismatch
  - Proto message literal × construction of `Customer{name: "a", age: 3}` matches a fixture decoded through cel-cpp
  - Proto message literal × unregistered descriptor → compile-time `InvalidArgument`
  - Map literal × `{"a": 1, "b": 2}` survives round-trip, `.size() == 2`
  - Map literal × duplicate key produces ERROR CelValue per `langdef.md`
  - List literal × `[1, 2, 3]` round-trips; `size([1,2,3]) == 3`

### 11.8 Rollout and backout

**Rollout.** Each slice lands as one squashed commit on `master`
with `[rewrite]` in the subject. No feature flag — the rewrite is
an internal restructure with no user-visible surface change
(until Slices 8–9 add aggregate literal support). A slice is "live"
the moment its commit is on `master`.

**Backout.** If a slice is found to have a regression after merge,
the default response is `git revert <sha>`. The dependency graph
(§11.2) defines what else must revert with it: reverting Slice 8
also requires reverting Slices 9 and later. This is why the graph
is the plan's load-bearing artefact.

**Pre-merge gates (per slice).**
  1. `scripts/lint.sh` — clean (zero clang-tidy warnings in touched
     files).
  2. `bazel test //compiler/...` — green locally and on any CI
     that's wired up.
  3. The testing-checklist row(s) this slice ticks are ticked in
     the same commit.
  4. The slice's milestone doc (this file) marks the slice `[x]`
     in §14.
  5. A terse commit body citing: which slice (by §11.3 number),
     which exit criteria were met, which checklist rows flipped.

### 11.9 Exit criteria for the whole rewrite

The rewrite is done when:

  - [ ] Slices 1–12 all `[x]` in §14.
  - [ ] Zero references to `LoweringContext::scratch_slot`,
        `prologue_setups`, `EmitCheckedArithmetic`, `_at_ii`,
        `_at_uu` remain in `compiler/`.
  - [ ] `rg cel_make_bool|cel_make_int|cel_make_uint|cel_make_double|cel_make_null compiler/codegen` returns zero hits (runtime/host boxing paths excluded).
  - [ ] Every `cel_*` helper in `cel_runtime.h` has a cel-cpp parity
        comment pointing at the source-of-truth impl.
  - [ ] `doc/wasm-compiler-design.md` §7.0 / §7.3 reflect the new
        pipeline (ResolvePass → LayoutPass → emit).
  - [ ] The CLAUDE.md "Unresolved design debt" bullet about the
        unified symbol table is closed (the extended
        `WasmAnnotations` subsumes it).
  - [ ] `m-mem-static-layout-pass.md` and `memory-ownership-flip.md`
        are renamed `.obsolete.md` with a pointer to this file.
  - [ ] This file has a `Status: shipped` stanza at the top with
        the final ship date.

## 12. Open questions

**1. Inline runtime into every expr module (option-2 from the flip doc)?**
Massive codegen simplification (all `cel` imports collapse to direct
calls), but requires either `wasm-merge` at compile time or
wasm-ld-based linking. Recommend: **not in this rewrite.** Import-
based shape works and keeps toolchain deps stable.

**2. Arena cursor location.** **Decided: bytes 8..15 of linear
memory, not wasm globals.** Both modules share the same memory
instance, so a fixed offset is the natural primitive. Wasm-globals
would add a `wasm-ld` dependency (correctly emitting exported
mutable globals from C `__attribute__((visibility("default")))`) for
zero real benefit. Bonus: a full memory dump now captures the
entire allocator state (cursor + watermark + every CelValue +
every span payload) in one blob — debug-layout walkers get
self-contained snapshots for free. See §8.2 for the layout.

**3. Boxing policy for doubles.** Accepted 2026-04-20 (box doubles
into `.rodata`; f64.div doesn't model CEL's div-by-zero as ERROR).
Re-confirm before Slice 6 starts.

**4. `.rodata` cap.** **Decided: no cap.** Literals go in rodata
unconditionally. `StaticMemoryBuilder::Append*` return plain
`uint32_t`, not `StatusOr`. No fallback path, no embedder-tunable
`rodata_cap`. Wasm's 4 GiB linear-memory limit is the absolute
ceiling, and approaching it is a codegen bug, not a design concern.

**5. Externref table (`cel_refs`).** Already per-expr-module.
Unchanged by this rewrite.

**6. Map / list / message literal *static* packing.** Dynamic
construction via runtime / host calls ships in Slice 13 (§4.7). Static
packing into `.rodata` (compile-time-known lists / maps whose keys
and values are all literals) is out of scope for this rewrite;
`StaticMemoryBuilder` leaves `AppendList` / `AppendMap` stubbed with
an explicit `Unimplemented` return path. Addition when (if) a profiling
need emerges.

**7. `MessagePatternTable` vs `OverloadTable` unification.** Proto
construction lives in its own side table (§4.7.1) keyed by descriptor
pointer, not overload id. One `CallTargetTable` that interleaves both
is possible — same lookup surface, same pattern_id space — but the
keys are disjoint and merging them complicates the builder's
collision-check rules (built-in seeds vs customs vs message patterns,
three partitions instead of two). Keep split until there's a concrete
reason to merge.

**8. Do we also store the expr_id of the "primary slot-acquiring
node" on `Storage` for debugging?** Would make arena walkers
print "slot 0x40 = result of expr_id 17 (a + b)" but adds a field
to hot memory. Recommend: leave out; debug-layout mode already
preserves per-expr distinctness.

## 13. What this rewrite buys us, at completion

A one-paragraph summary for the post-completion CLAUDE.md update:

  > The codegen pipeline became: parse → check → resolve → layout →
  > emit. Each stage has one job, testable in isolation. Per-node
  > facts (repr, storage, overload_id, local_index, scope_id,
  > field_number) live on the existing `NodeAnnotation`; operator
  > dispatch lives in the frozen `OverloadTable`, built from a
  > `constexpr` seed list for built-ins and extended via
  > `RegisterCustom` for embedder-supplied host functions. Every
  > overload — built-in or custom — has the same wasm ABI:
  > `(out_slot, args…) -> void`, with all operands as i32 offsets to
  > `CelValue` cells. Literals live in `.rodata`; computed CelValues
  > live in workspace slots pre-assigned by Sethi–Ullman; the arena
  > is a runtime-internal allocator for variable-length payloads.
  > Memory is per-expr-module, so two evals in parallel are
  > naturally isolated. `EmitCheckedArithmetic`,
  > `GetScratchSlotLocal`, the `_at_ii`/`_at_uu` helper families, and
  > most `cel_make_*` call sites in codegen are gone. Every helper
  > in `cel_runtime.h` carries a cel-cpp-parity pointer in its
  > declaration comment. The extended `WasmAnnotations` is the
  > symbol table the CLAUDE.md bullet asked for; it handles scoped
  > bindings so M5's comprehensions slot in without schema changes.

## 14. Deliverables checklist

On completion, these are the observable artefacts:

  - [ ] `compiler/ir/annotations.h` — extended `NodeAnnotation` +
        `StorageKind` / `Storage`
  - [ ] `compiler/codegen/overload_table.{h,cc,_test.cc}`
  - [ ] `compiler/codegen/resolve_pass.{h,cc,_test.cc}`
  - [ ] `compiler/codegen/layout_pass.{h,cc,_test.cc}`
  - [ ] `compiler/codegen/static_memory_builder.{h,cc,_test.cc}`
  - [ ] `compiler/codegen/slot_allocator.{h,cc,_test.cc}`
  - [ ] `compiler/codegen/expr_lower.{h,cc}` — annotation-driven
  - [ ] `compiler/codegen/module.{h,cc}` — `SetMemory` + active segment
  - [ ] `compiler/runtime/cel_runtime.{h,c}` — slimmed helper set, §8 changes
  - [ ] `compiler/runtime/BUILD.bazel` — `--import-memory`, explicit exports
  - [ ] `compiler/runtime/wasm_imports.txt` — already exists (cel_log)
  - [ ] `compiler/host/host_loader.{h,cc}` — two-phase instantiation
  - [ ] `doc/wasm-compiler-design.md` §7.0 / §7.3 — rewritten
  - [ ] `doc/implementation-plan/testing-checklist.md` — new rows
  - [ ] `CLAUDE.md` — symbol-table debt bullet closed
  - [ ] This doc — marked `shipped`; `m-mem-static-layout-pass.md` +
        `memory-ownership-flip.md` renamed `.obsolete.md` with a
        one-line pointer to this doc.
