# Rewrite: memory layout, symbol table, codegen simplification

Status: **historical design baseline.**  Slices S1–S6 + S8 shipped
2026-04-22 → 2026-04-25; the remaining slice numbers were absorbed by
later milestone docs (S9 proto literals → m7, S7 custom functions →
m13/m21, S12 the compiler_v2 → top-level swap, landed 2026-05-25) or
not pursued (S10 Sethi–Ullman — the naive slot allocator stayed).
The as-shipped memory model is the §"Phase C delta" callouts below
plus [`wasi/DESIGN.md`](wasi/DESIGN.md) §4–§5 and
[`memory-layout-design.md`](memory-layout-design.md).
Drafted 2026-04-21.  This doc describes the end-state design; each
sub-section is annotated with shipping status where it has shipped
and a plan-vs-execution callout where the as-shipped shape diverged.

> **Phase C delta (shipped) — see [../wasi/DESIGN.md](../wasi/DESIGN.md) §4–§5.**
> The wasi-sdk migration (Phases A/B, shipped 2026-05-18) plus the
> Phase C library-vendoring work replaced the original
> "fixed-offset bump arena at bytes 8/12 + host-owned, expr-defined
> memory" shape this doc was drafted against.  The shipped model:
>
>   - **One shared linear memory, runtime-owned.**
>     `cel_runtime.wasm` is built on `wasm32-wasi-threads`, DEFINES
>     and exports its memory as **shared** (observed `(memory 4 1024
>     shared)` — min ~4 pages, max 1024 = 64 MiB; the min is baked by
>     wasm-ld and varies by build mode, the host's A13 invariant only
>     enforces a `>= CELWASM_INITIAL_MEMORY_PAGES = 2` floor).  The
>     expr module IMPORTS `cel.memory` with a matching shared shape
>     (`max_pages = 1024`, set in `compile.cc::InstallExprModuleImports`)
>     and no longer defines its own.  This is the *reverse* of the
>     pre-migration topology where the expr owned the memory and
>     the runtime imported it.  The host pulls the shared-memory
>     export off the runtime instance and binds it on the linker
>     as `cel.memory` (`engine.cc::BindRuntimeMemory`), reading
>     it via `wasmtime_sharedmemory_data()`.
>   - **Malloc-backed, per-Instance arena.**  The fixed bytes-8/12
>     cursor is gone.  `arena_init(cap_bytes)` is called once per
>     Instance by the host; on wasm it `malloc()`s the backing
>     buffer out of the dlmalloc heap, and the arena state lives
>     in a runtime BSS struct (`g_arena`), not at fixed memory
>     offsets.  `arena_reset()` takes no args (O(1) cursor zero);
>     `arena_alloc(n)` returns an absolute offset into the shared
>     memory.  `cel_alloc`/`cel_reset(base,limit)` are removed.
>   - **`--global-base=8192`.**  The runtime link reserves
>     `[0, 8192)` (= `CELWASM_RESERVED_LOW_MEMORY_BYTES`) for the
>     expr module's active data segments (rodata + workspace);
>     wasi-libc static data + stack + heap live above it.
>
> The corrected per-section descriptions are inline below
> (§3.2, §8.1–§8.3, §9.1–§9.2) with `> Phase C delta:` callouts.
> `wasi/DESIGN.md` §4–§5 is authoritative for the detailed runtime
> + host memory ABI and the asserted layout invariants (A1–A17);
> this doc carries the architectural overview.  Constants here are
> mirrored from `runtime/cel_layout.h` (the single
> source of truth).  Note `wasi/DESIGN.md` itself predates the
> shared-memory + wasm32-wasi-threads decision (it was drafted for
> a vanilla `wasm32-wasi`, host-imported `(memory 2)` target); the
> as-shipped shared-memory shape is the Phase C reality and is the
> one described in the callouts below.

**Shipping snapshot (2026-04-25):**

| design § | covered by | as-shipped | notes |
|---|---|---|---|
| §3.1 pipeline | S1 → S4 (M1 + M2) | shipped | parse → check → resolve → layout → emit; no M5 scope stack yet |
| §3.2 memory regions | S1 (M1), reshaped Phase C | shipped (Phase C) | runtime-owned **shared** `cel.memory`; expr imports it; expr rodata+workspace live in reserved `[0, 8192)`; arena is malloc-backed (see Phase C callout) |
| §4.1 `NodeAnnotation` | S3 (M1), extended at M2/M3/M4 | shipped | three new fields landed at M2/M3/M4 — see §4.1 update |
| §4.2 uniform call ABI | S5 (M5) | partial — M5.F shipped general kCall arm | general `kCall` arm landed M5.F (2026-04-25); 7 dispatcher names (`cel_list_size` / `cel_list_in` / `cel_list_eq` / `cel_list_concat` / `cel_map_size` / `cel_map_in` / `cel_map_eq`) have runtime exports + kHost trampolines shipped (M5.D step 2 host/runtime halves), but codegen's `kPendingRuntimeExports` guard in `expr_lower.cc` keeps emitting `Unimplemented` for them until step 2's flip-the-guard commit lands; control-flow `&&` / `||` / `?:` pending M5.G |
| §4.3 `OverloadTable` | S3 (M1), seeds at S5 | partial — M5.E + M5.B step 2 shipped seeds | `kBuiltinSeeds` populated with 80 entries (M5.E: 46; M5.B step 2: +34 cross-type numeric + bool/string/bytes ordering tail); `kExplicitlyUnimplementedIds` 86; coverage tripwire green |
| §4.6 custom functions | S7 (post-M5) | not shipped | per-function imports + `RegisterFunction` plumbing pending |
| §4.7.1 proto literals | S9 (M7) | not shipped | descriptor pool resolution + `cel_set_field` pending |
| §4.7.2 map literals | S8 — shipped as M3 | **shipped** | as-shipped uses **three-path dispatch** (`map-list-dispatch.md`) — see §4.7.2 callout |
| §4.7.3 list literals | S8 — shipped as M4 | **shipped** | same three-path dispatch + `create(out, count)` + `set(list, i, elem)` (no `append`/`grow`) — see §4.7.3 callout |
| §4.7.6 host field reads | S4 (M2) | shipped | `cel_get_field` + `cel_has_field` Layer-1/2/3 split landed |
| §5 ResolvePass | S3 → S4 | shipped + extended | M2 added `attribute_id`; M3/M4 added origin visitors; M4 added comprehension early-reject |
| §6 LayoutPass | S3 → S4 | shipped — naive | no Sethi–Ullman yet (S10); workspace slot per node |
| §7 codegen | S1 → S4 + S8 + partial S5 | partial | `kConst` / `kIdent` / `kSelect` / `kCallExpr(_[_])` / `kCreateMap` / `kCreateList` / general `kCall` (M5.F, 2026-04-25) arms green; `&&` / `||` / `?:` pending M5.G; `kCreateStruct`, `kComprehension` pending |
| §8 runtime | S1 + S8, reshaped Phase C | shipped + extended (Phase C) | **malloc-backed arena** (`arena_init`/`arena_alloc`/`arena_reset`, state in BSS — replaces bytes-8/12), map/list arena primitives, kDynamic dispatcher with `__attribute__((musttail))` |
| §9 host runtime (Engine/Instance) | S1 (M1), reshaped Phase C | shipped (Phase C) | two-phase instantiation; memory pulled from the runtime's shared export + bound as `cel.memory`; `arena_init` seeds the per-Instance arena |
| §10 future-milestone absorption | M2 ✓ M3 ✓ M4 ✓ M5 pending | partial | §10.1 (M2) ticked; §10.3 (M3) + §10.4 (M4) added below; §10.2 (comprehensions) deferred to a follow-on milestone after M5 — `m5-kcall-comprehensions.md` ships kCall + control flow + msg-eq only |
| §11.4 slice graph | S1–S4 + S8 done; S5 partial | partial | S5 partial — M5.A/B/C/D-step-1/E/F shipped 2026-04-25 (general kCall + arithmetic + comparison + string ops + aggregate kArena fast paths); S5 remainder (M5.D step 2 + M5.B step 2b + M5.G + M5.H) pending; S6 partial (`has` shipped, message-eq pending — lands with M5.D step 2); S7/S9/S10/S11/S12 pending |

**Two retroactive design docs landed alongside the milestone work:**

  - `two-phase-runtime-isolation.md` — Engine/Instance role split,
    host-allocated memory, parsed-runtime caching.  Reconciled into
    §9 + §3.2 as the runtime side of the design.
  - `map-list-dispatch.md` — three-path origin dispatch (kArena /
    kHost / kDynamic) for maps and lists.  **Supersedes §4.7.2 +
    §4.7.3's simple "empty-then-populate" model.**  M3 (maps) +
    M4 (lists) shipped this design.  Reconciled into §4.7 callouts;
    fully folded into design.md as of 2026-04-25 (see
    map-list-dispatch.md §11).

Supersedes `predecessor-m-mem-static-layout-pass.md` and
`predecessor-memory-ownership-flip.md` (both in this directory).
Subsumes the unified-symbol-table question: the extended
`WasmAnnotations` (§4.1) is the single per-node fact table that
`CheckOptions::variable_specs` / `TypedAst::variables()` /
`LoweringContext.idents` splintered across in v1.  M2 populates
`local_index` and `scope_id` on `NodeAnnotation`; M5 adds the
scope stack.  No `SymbolTable` class on `TypedAst` is planned.

**Companion doc — `cel-host-surface.md`** is authoritative for the
public user API (`Cel::Compiler` / `Cel::Program` / `Cel::Instance`
/ `Cel::RuntimeBindings` / `Cel::Value` / `Cel::Activation`), the
`cel.abi` custom-section schema, and every wasm callback signature
into the host. This doc owns runtime / codegen internals; where the
two touch, `cel-host-surface.md` wins. Sections here that describe
the custom-function flow (§4.6), the proto host imports (§4.7), or
the two-phase loader (§9) describe *implementation mechanics* that
realise surfaces defined there.

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

In v1, name/type/scope info is split across
`CheckOptions::variable_specs` (frontend),
`TypedAst::variables()` + `WasmAnnotations` (IR), and
`LoweringContext::idents` (codegen).  That worked for the flat,
scope-free subset v1 shipped; it breaks the moment M5
(comprehensions) or user functions need nested scopes.  Two
options were floated — promote to a `SymbolTable` on `TypedAst`,
or side-table off cel-cpp's `reference_map`.

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

### 3.2 Memory regions

> **Phase C delta (shipped).**  The original drafting (preserved
> below as historical context) had the expr module *define* its own
> linear memory with the arena bump region at the high end of that
> same memory.  The shipped model inverts memory ownership and moves
> the arena into the dlmalloc heap.  Read the as-shipped layout
> first; the original prose follows.

**As-shipped (Phase C).**  There is exactly one linear memory per
Instance, **defined and exported as shared by `cel_runtime.wasm`**
(built on `wasm32-wasi-threads`; observed shape `(memory 4 1024
shared)`).  The expr module IMPORTS it as `cel.memory` with a
matching shared shape (max 1024 pages = 64 MiB).
`-Wl,--global-base=8192` on
the runtime link forces wasi-libc to place its static data + stack +
heap above byte 8192, leaving `[0, 8192)` =
`CELWASM_RESERVED_LOW_MEMORY_BYTES` free for the expr module's active
data segments.

```
offset 0x00000  ── EXPR-RESERVED region [0, 8192) ─────────────────────
                   Active data segments install here at expr-module
                   instantiate time.
   [0, 16)          null sentinel (zero-kind CelValue → `off==0 ⇒ absent`)
   rodata_base=16   .rodata: constant CelValue headers + string/bytes
                    payloads, one per kStaticRodata node
   workspace_base   24-byte CelValue workspace slots, statically
   = RoundUp8(...)  assigned (kWorkspaceSlot nodes), incl. select +
                    aggregate scratch
   (arena_base       ← legacy layout field; no longer where the arena
    in StaticLayout)   physically lives — see below)

offset 0x02000  ── WASI-LIBC STATIC DATA + STACK ──────────────────────
                   ~static data + 64 KB stack.  __heap_base lands
                   above this (around 243568, varies by build mode).

offset ~__heap_base ── DLMALLOC HEAP ──────────────────────────────────
                   Per-Instance bump arena buffer is malloc'd here
                   once via arena_init (CELWASM_ARENA_CAPACITY_BYTES =
                   64 KiB).  Activation binding buffer + any
                   Plan-lifetime objects (RE2 regex cache, parsed
                   timestamps) also live here.
```

  - **Offset 0 is reserved** as the "absent" sentinel.  Every `_at`
    helper treats `out == 0` as absent and no-ops; `cel_value_at(0)`
    returns a well-formed NULL.
  - **`.rodata` starts at 16** (`rodata_base`, the first 8-aligned
    offset past the sentinel).  CelValue headers + span payloads for
    every `StorageKind::kStaticRodata` node.
  - **Workspace** holds pre-assigned 24-byte slots for
    `StorageKind::kWorkspaceSlot` nodes.  `workspace_base =
    RoundUp8(rodata_base + rodata.size())`.
  - **Arena** is the runtime's bump region for variable-length
    payloads (string-concat results, list/map bodies, host-decoded
    proto fields).  It is **malloc-backed** and lives in the dlmalloc
    heap, NOT contiguous with workspace inside `[0, 8192)`.
    `arena_alloc(n)` 8-aligns, bumps `g_arena.cursor`, and returns the
    absolute offset of `g_arena.base + cursor` in the shared memory;
    `arena_reset()` zeroes the cursor.  The `arena_base` field still
    on `StaticLayout` is a legacy artifact (codegen no longer consults
    it) — see §8.2.

The runtime module provides `arena_init` / `arena_alloc` /
`arena_reset` / 3VL / arithmetic helpers; the expr module imports
them as `cel.*`.  The `INITIAL_MEMORY_PAGES`,
`RESERVED_LOW_MEMORY_BYTES`, and `ARENA_CAPACITY_BYTES` constants live
in `runtime/cel_layout.h`, shared by codegen, host, and
runtime so the three can't drift; see `wasi/DESIGN.md` §5 for the full
asserted-invariant table (A1–A17).

---

**Original drafting (historical — pre-migration, expr-owned
memory).**  Under the (now-superseded) flip, the expr module defined
its own linear memory:

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

In that model the arena was the high end of the expr's own memory,
reached via `cel_alloc` and reset by a `cel_reset` the expr module
imported, with the cursor stored at fixed bytes 8/12.  The runtime
imported the expr's memory.  All three of those facts are reversed in
the as-shipped Phase C model above.

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
The header already flags the planned growth path (`attribute_id`,
`scope_depth`). Custom-function interning is no longer on that path —
customs are named imports resolved through the overload table
(§4.6.1), so `NodeAnnotation` needs no custom-specific field.

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

// As-shipped (post-M4) Origin enum used for map_origin / list_origin.
// See `map-list-dispatch.md`.
enum class Origin : uint8_t {
  kDynamic = 0,  // default — runtime dispatcher decides arena vs host
  kArena   = 1,  // arena-built (kCreateMap / kCreateList)
  kHost    = 2,  // host-backed (proto field / Activation::Bind)
};

struct NodeAnnotation {
  Repr             repr         = Repr::kUnknown;
  uint32_t         field_number = 0;            // SelectExpr (M2)
  // CallExpr's resolved cel-cpp overload id, e.g. "add_int64".
  // String_view points into cel-cpp's owned reference_map storage —
  // lifetime tied to the surrounding `TypedAst`.  Empty on non-call
  // nodes.  ResolvePass (M5.F `OverloadIdResolver`) stamps this from
  // `cel::Ast::reference_map().overload_id().front()`; codegen looks
  // it up in `OverloadTable::Lookup(string_view)` at emit time.
  absl::string_view overload_id = {};            // CallExpr (M5.F+)
  uint32_t         local_index  = 0;            // IdentExpr (M2)
  uint32_t         scope_id     = 0;            // comprehensions (M5)
  uint32_t         attribute_id = 0;             // SHIPPED M2 — interned
                                                  //   (root, qualifiers) path
                                                  //   id; 0 = none.  Read by
                                                  //   the cel_host trampoline
                                                  //   for unknown-pattern match.
  Storage          storage;
  // Forward-compat hooks added in M2; populated as of M3 (map_origin)
  // and M4 (list_origin).
  Origin           map_origin   = Origin::kDynamic;  // SHIPPED M3
  Origin           list_origin  = Origin::kDynamic;  // SHIPPED M4
};
```

ResolvePass writes `repr` / `field_number` / `overload_id` /
`local_index` / `scope_id` / `attribute_id` / `map_origin` /
`list_origin`. LayoutPass writes `storage`. Codegen reads
everything.

> **Plan-vs-execution delta — `overload_id` is a `string_view`, not
> an interned `uint32_t`.**  Original M1-locked schema named a
> `uint32_t overload_id` populated via
> `OverloadTable::InternOverloadId(...)` at ResolvePass and consumed
> via `OverloadTable::LookupById(uint32_t)` at codegen — the
> intern-id story §4.3 still describes for `LookupById` callers.
> M5.F shipped the resolver as a verbatim `string_view` carrying
> cel-cpp's owned overload-id string, looked up via the string-keyed
> `OverloadTable::Lookup(string_view)` at emit time.  The intern
> path (`InternOverloadId` / `LookupById`) still exists on the
> `OverloadTable` (used by `compile.cc::InstallOverloadImports` to
> walk every interned id and install the matching wasm import)
> but **no longer sits on the resolve→emit hot path**.  Trade-off:
> dropped a uint→string round-trip per call site at the cost of a
> string_view copy on the annotation; under realistic CEL the
> reference_map's id strings are short and the lookup is a
> single hash on the same string identity, so no measurable
> perf delta.

> **Plan-vs-execution delta — three new fields beyond the original
> §4.1 schema.**  Original M1-locked schema named `repr` /
> `field_number` / `overload_id` / `local_index` / `scope_id` /
> `storage`.  Three additional fields landed in subsequent
> milestones, each driven by a host-side runtime concern that
> couldn't be derived purely from the AST kind:
>
>   - **`attribute_id`** (M2) — the cel_host trampoline needs
>     a stable id-per-call-site so the unknown-pattern matcher
>     can distinguish two reads of the same field at different
>     syntactic positions.  Interned in `ResolveOutput::attributes`.
>   - **`map_origin` / `list_origin`** (M2 forward-compat,
>     populated M3 + M4) — codegen dispatches the kCallExpr(`_[_]`)
>     arm on operand origin (kArena → fast path, kHost → host
>     trampoline, kDynamic → runtime dispatcher); ResolvePass
>     stamps the origin from the operand's source node kind +
>     declared type.
>
> The §10.1.3 "no new fields" invariant was relaxed in-place at
> M2 close; the design's intent — *idents/selects/unknowns
> need no schema change* — is intact since none of these three
> fields encode ident/select/unknown facts.

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
  - **One dispatch rule in codegen.** No slot-out vs arena-out branch,
    and no built-in vs custom branch either — both are just rows in
    the overload table naming a specific wasm import
    `(module, helper_name)` with the same slot-out signature.

**Runtime implication.** `compiler/runtime/cel_runtime.c` owes every
helper this shape. Variable-length-return helpers that today return
an offset (e.g. `cel_string_concat`) get a new parameter `uint32_t
out_slot` and are migrated to write a `CelValue` there. The old
return-offset versions retire. This is a single runtime slice in the
rewrite plan (§11).

### 4.3 Overload table

Codegen needs `overload_id → (import_module, helper_name)`. Built-ins
are fixed at tool-compile time; custom functions are registered by
the embedder at compile time. Same lookup path for both, same row
shape for both — each row names one specific wasm import.

```cpp
// compiler/codegen/overload_table.h

// Which wasm import module a helper comes from. The expr module imports
// from three modules today; only the first two are overload targets
// (cel_env is logging-only). Enumerated so a module rename is a one-
// line change in ImportModuleName() and a compile error everywhere.
enum class ImportModule : uint8_t {
  kCelRuntime = 0,  // "cel"      — runtime .wasm exports (cel_int_add_at_vv,
                    //              cel_and, cel_alloc, cel_string_concat, …)
  kCelHost    = 1,  // "cel_host" — host-provided helpers; each custom
                    //              function registers its own named import.
};
absl::string_view ImportModuleName(ImportModule m);  // → "cel" / "cel_host"

struct OverloadImpl {
  ImportModule     module = ImportModule::kCelRuntime;
  std::string_view name;        // wasm import name within `module`.
                                // Built-in: "cel_int_add_at_vv" (kCelRuntime).
                                // Custom:   "my_upper_string"   (kCelHost).
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

As-shipped (`compiler/codegen/overload_table.h`, M5.E + M5.F):

```cpp
struct Seed {
  absl::string_view overload_id = {};
  OverloadImpl impl = {};
};
// `compiler/codegen/overload_table.cc`: 80 entries today (M5.E +
// M5.B step 2 same-kind + cross-type numeric ladder).
constexpr std::array<Seed, 80> kBuiltinSeeds{ /* see §4.3.2 */ };

class OverloadTableBuilder {
 public:
  OverloadTableBuilder();  // seeds every row in kBuiltinSeeds.

  // Registers a custom host function. `helper_name` is the wasm
  // import name the expr module will reference — one import per
  // registered custom, no shared trampoline (§4.6.1). Fails with
  // AlreadyExists if `overload_id` is already present — either from
  // kBuiltinSeeds (user cannot shadow a built-in; CEL spec forbids
  // it and cel-cpp's FunctionRegistry would also reject) or from a
  // prior RegisterCustom call (two customs with the same id).
  ABSL_MUST_USE_RESULT absl::Status RegisterCustom(
      absl::string_view overload_id, ImportModule module,
      absl::string_view helper_name);

  OverloadTable Build() &&;

 private:
  // Parallel arrays.  std::deque storage stays valid under push_back
  // so the string_view keys in `index_` keep pointing at stable
  // bytes after a custom registration grows the table.
  std::deque<std::string> custom_ids_;
  std::deque<std::string> custom_helper_names_;
  std::vector<OverloadImpl> impls_;          // indexed by (interned_id - 1)
  absl::flat_hash_map<absl::string_view, uint32_t> index_;
  absl::flat_hash_set<absl::string_view> builtin_ids_;  // for collision msgs
};

class OverloadTable {
 public:
  // Codegen's hot-path lookup, called from `EmitGeneralCall` with the
  // string_view stamped onto `NodeAnnotation::overload_id` by
  // ResolvePass.  Returns nullptr if unregistered — codegen treats
  // this as Unimplemented and aborts the compile with the id in the
  // error message.
  const OverloadImpl* Lookup(absl::string_view overload_id) const;

  // Dense 1-based id, kept on the table for the *import-installer*
  // path — `InstallOverloadImports` walks `[1..size()]` and emits one
  // `AddFunctionImport` per kCelRuntime helper actually present.  Not
  // on the resolve→emit hot path: codegen never round-trips through
  // an interned id (it stamps the verbatim string_view on the
  // annotation and looks up by string at emit time).  Returns 0 if
  // `overload_id` is not registered.
  uint32_t InternOverloadId(absl::string_view overload_id) const;

  // Reverse of InternOverloadId — called only with ids the builder
  // itself assigned (see `compile.cc::InstallOverloadImports`'s walk
  // over `[1..size()]`).  CHECKs on out-of-range.
  const OverloadImpl& LookupById(uint32_t interned_id) const;

  // For import declaration: enumerate (module, helper_name) pairs
  // reached by the compiled expression. Codegen tracks the set of
  // interned ids it emitted and hands them back here.
  std::vector<std::pair<ImportModule, absl::string_view>> UsedImports(
      const absl::flat_hash_set<uint32_t>& used_ids) const;

  size_t size() const;

 private:
  // Parallel arrays mirror the builder's; keys in `index_` point
  // into the seed `constexpr` strings (built-ins) or the deques
  // (customs).
  std::deque<std::string>     custom_ids_;
  std::deque<std::string>     custom_helper_names_;
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
the builder copies into `custom_ids_` (a `std::deque<std::string>`
so existing string_view keys in `index_` aren't invalidated by
later pushes) so the frozen table survives registration-callsite
strings going out of scope.

**Why `Lookup` returns a pointer, not a reference.** Unresolved
overloads are a compile error (codegen emits Unimplemented), not a
crash. `LookupById(uint32_t)` — called only with ids the builder
itself assigned — returns a reference and CHECKs.

> **Plan-vs-execution delta — the interned `uint32_t` is a
> bookkeeping detail, not the resolve→emit pipe.**  The original
> M1-locked schema named `NodeAnnotation::overload_id : uint32_t`
> populated via `InternOverloadId` at ResolvePass and consumed via
> `LookupById` at codegen.  M5.F shipped the resolver as a verbatim
> `string_view` carrying cel-cpp's owned reference-map id, looked up
> via `Lookup(string_view)` at emit time (see §4.4 + §4.1's
> companion delta callout).  The dense id only matters now to
> `InstallOverloadImports` — `compile.cc` walks `[1..table.size()]`
> to emit one wasm import per shipped kCelRuntime helper.  The
> `InternOverloadId` / `LookupById` pair stayed on the class so
> the import-installer doesn't need a parallel walk over
> `kBuiltinSeeds`.

#### 4.3.2 Built-in seeds

`kBuiltinSeeds` is a `constexpr` array in `overload_table.cc`. Every
row names `ImportModule::kCelRuntime` explicitly; `kCelHost` only
appears at `RegisterCustom` sites.

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

> **Plan-vs-execution delta — Option B aggregate routing
> (M5.E, 2026-04-25).**  The as-written design treats every
> overload as routing to a single concrete helper.  Aggregate
> ops (`size_list`, `size_map`, `in_list`, `in_map`, `add_list`,
> aggregate `==`) instead seed the **kDynamic dispatcher**
> name (e.g. `cel_list_size`, NOT `cel_list_size_arena`); the
> dispatcher branches on the operand's runtime `kind` and
> `__attribute__((musttail))`-jumps to the arena fast-path or
> the kHost trampoline.  Trade-off: one extra runtime branch
> per aggregate-op call site, vs. either bloating the
> `OverloadTable` with `(id, origin)` pairs or special-casing
> aggregate ops in `expr_lower.cc` like `_[_]` does.  Option B
> won because it keeps the seed table flat and codegen's
> `EmitGeneralCall` arm a single lookup-and-emit (mirroring
> arithmetic / compare).
>
> The 7 dispatcher names (`cel_list_size`, `cel_list_in`,
> `cel_list_eq`, `cel_list_concat`, `cel_map_size`, `cel_map_in`,
> `cel_map_eq`) are surfaced as Unimplemented at codegen time
> until M5.D step 2 ships their runtime exports + kHost
> trampolines (§4.4.1).  See `overload_table.cc` head comment
> for the full reasoning.

### 4.4 Codegen dispatch

```cpp
const NodeAnnotation& a = *annotations.Find(expr.id());
switch (expr.kind_case()) {
  case kConstant: return EmitKConstLoad(mod, a);          // rodata
  case kIdentExpr: return EmitKIdentLoad(mod, a);         // local
  case kSelectExpr: return EmitKSelect(ctx, expr, sel, a);
  case kCallExpr: {
    // M5.F as-shipped (`expr_lower.cc::EmitGeneralCall`).  Special
    // arms first: `_[_]` (M3/M4 indexing — origin-aware), control
    // flow `_&&_` / `_||_` / `_?_:_` / `!_` (M5.G — branch-style,
    // 3VL short-circuit doesn't fit slot-out).  Everything else
    // reads `a.overload_id` (a string_view stamped by ResolvePass
    // from cel-cpp's reference_map) and looks it up directly:
    const OverloadImpl* impl = ctx.overload_table.Lookup(a.overload_id);
    if (impl == nullptr || IsPendingRuntimeExport(impl->name)) {
      return Unimplemented(...);
    }
    DCHECK(a.storage.kind == StorageKind::kWorkspaceSlot);
    // Uniform ABI (§4.2): `(out_slot, args…) -> void`.  Built-ins
    // and customs share one emitter — both are just a wasm
    // `call $import` naming `impl->module` / `impl->name`.
    return EmitGeneralCall(ctx, expr, call, a);
  }
  …
}
```

No built-in/custom branch: both rows look identical to codegen and
resolve to a named wasm import via `impl->module` + `impl->name`.
Node storage is deterministic from AST kind (literal → rodata, ident
→ local, everything else → workspace slot) so the dispatch table is
tiny.

**Import declaration is eager, walked off the table.** Today's
`compile.cc::InstallOverloadImports` walks `[1..table.size()]` and
emits one `AddFunctionImport` per kCelRuntime helper that ships now,
keyed by helper-name suffix to infer arity (`_at_vv` → 3 i32 args,
`_at_v` → 2; the 7 kDynamic dispatcher names in
`kPendingRuntimeExports` are skipped — see §4.4.1).  This is eager
rather than the originally planned `UsedImports(used_ids)` filter:
codegen never reports back the set of ids it actually emitted, and
the entire table is small enough that the wasm validator + linker
prunes unused imports at instantiation cost.  Customs (kCelHost)
install via M6 — outside the M5 slice.

##### 4.4.1 `kPendingRuntimeExports` guard (M5.F → M5.D step 2)

`compiler/codegen/expr_lower.cc` carries a 7-element
`kPendingRuntimeExports` set:

```cpp
constexpr std::array<absl::string_view, 7> kPendingRuntimeExports = {
    "cel_list_size", "cel_list_in", "cel_list_eq", "cel_list_concat",
    "cel_map_size",  "cel_map_in",  "cel_map_eq",
};
```

These are the kDynamic dispatcher names the M5.E seeds point at
(Option B aggregate routing — see §4.3.2 callout).  As of
2026-04-25 the **runtime exports** (`runtime/cel_runtime.c`
+ `BUILD.bazel`'s `--export=` list) and the **kHost trampolines**
(`eval/internal/cel_host_wasmtime.cc::RegisterCelHostImports`
binds all twelve `cel_host.*` entries including the seven
aggregate ops + `cel_message_eq`; corresponding `Cel*Impl` bodies
in `cel_host.cc`) **have shipped** — the M5.D-step-2 host /
runtime halves are done.  What remains is the codegen-side flip:
`EmitGeneralCall` still returns `Unimplemented` whenever the
resolved helper name is in this set, and `InstallOverloadImports`
mirrors the same skip list.  Until those two sites empty,
emitting an import for a dispatcher would link-fail against an
expr module whose imports list doesn't include them — the guard
keeps the two sites moving together.  Step 2's closing commit
(an "M5.D step 2 agent" task captured in
`m5d-step2-agent-prompt.md`) flips both lists to empty and adds
the e2e suite that walks the now-unblocked aggregate ops.

**Imports installer mirrors the guard.**  `compile.cc::InstallOverloadImports`
holds the same 7-name list and skips installing imports for them
— the codegen guard + installer skip list move together so the
emitted module + the linker-side imports stay coherent.

### 4.5 Coverage invariant

`overload_table_test::CoverageTripwire` iterates every `k*` member of
cel-cpp's `StandardOverloadIds` and asserts either
`OverloadTable::Lookup(k) != nullptr` (we have a helper) or
`OverloadTableIsExplicitlyUnimplemented(k) == true` (we know we
don't, codegen fails the expression with a clean Unimplemented
status citing the overload id).  A new constant added to cel-cpp
must be classified in one of those two buckets before this test
passes.  The "explicitly unimplemented" set lives in
`overload_table.cc::kExplicitlyUnimplementedIds` (86 entries today)
and is only readable through the free function
`OverloadTableIsExplicitlyUnimplemented(absl::string_view)` —
keeping the array internal so callers can't accidentally treat
"deferred" as a richer state than "rejected".

> **Plan-vs-execution delta — `CompileOptions::allowed_overloads`
> not shipped.**  The original design described embedder-side
> filtering (`langdef.md`'s "embedders may restrict which standard
> functions are available" rule) by adding `allowed_overloads` to
> `CompileOptions` and pruning the table before freezing.  As of
> 2026-04-25 (M5.F) `compiler/internal/compile.h::CompileOptions` carries
> only `check`, `mem_size_bytes`, `eval_internal_name`,
> `eval_export_name`, `validate`, `serialize` — no
> `allowed_overloads` field.  Both rejection routes
> (our-side `kExplicitlyUnimplementedIds` and the not-yet-shipped
> embedder filter) bottom out at the same `Lookup() == nullptr`
> path, so the design is forward-compat: a future M-something can
> add the field without touching codegen.  Until it ships, the
> only way to disable a built-in is to ship a fork that drops
> rows from `kBuiltinSeeds` — fine for our current single-tenant
> use, not fine for multi-tenant deployments.

### 4.6 Custom host functions

CEL embedders register functions through cel-cpp's checker API:

  - `TypeCheckerBuilder::AddFunction(const FunctionDecl&)`
    (`third_party/cel-cpp/checker/type_checker_builder.h`).
  - Each `FunctionDecl` carries one or more `OverloadDecl`s
    (`third_party/cel-cpp/common/decl.h`). Overload ids are either
    user-supplied via `MakeOverloadDecl("my_upper_string", string,
    string)` or auto-generated from arg types.

This compiler is AOT, and the public user surface splits
registration in two
(see `cel-host-surface.md` §2 — authoritative for the user API):

  - **Compile-time declaration** on `Cel::Compiler::Builder::
    RegisterFunction(FunctionDecl{...})`. `FunctionDecl` is
    signature-only: `name`, `overload_id`, `is_receiver`,
    `arg_types[]`, `return_type`. No impl. The Compiler forwards
    these to cel-cpp's `TypeCheckerBuilder::AddFunction` so the
    checker resolves calls normally.
  - **Eval-time impl binding** on `Cel::RuntimeBindings::
    AddFunction(overload_id, impl)`. Supplied at
    `program.Plan(bindings)` time; the Compiler never sees impls.

After the checker runs, `CheckedExpr.reference_map[id]` carries
the resolved overload id uniformly for built-ins and customs — no
IR distinction. Our frontend already consumes `reference_map`;
nothing about ResolvePass plumbing has to change.

Because the checker treats customs and built-ins the same way,
**customs are just dynamic entries in the overload table**. No
separate `NodeAnnotation` field, no separate dispatch path in codegen.

#### 4.6.1 Wasm ABI: one import per custom function

Each registered custom function becomes its own wasm import under
module `"cel_host"`, with the uniform slot-out signature (§4.2):

```
cel_host.<helper_name>(out_slot, arg0, arg1, …, argN-1) -> void
```

`helper_name` is the wasm import name the embedder supplies at
registration (typically matching the overload id, e.g.
`my_upper_string`). Arity is baked into the wasm signature — same
as every built-in helper; the host reads exactly `N` arg slots
directly off the wasm stack. No args-staging region, no `args_ptr`
indirection, no host-side fan-out table.

**Why per-function, not a shared trampoline.** Makes customs and
built-ins symmetric at every layer:

  - **Overload table.** Both rows are `(module, name)`; no
    pattern-id sidecar, no "is-custom" bit, no dispatch-time branch
    in codegen.
  - **Wasm ABI.** Both use the same
    `(out_slot, args…) -> void` signature; arity comes from the
    signature like any other import.
  - **Host binding.** The host binds one wasm import per embedder-
    registered function by name at `LoadEval`; no dispatcher table
    keyed by `pattern_id`; name collisions or missing bindings
    surface as ordinary wasm link-time errors from the runtime
    rather than a custom `FailedPrecondition` path.

**Tradeoff: imports list grows with the registry.** The expr
module's imports depend on which customs its expression references
(same rule as built-in helpers: `UsedImports` filters to the
subset). A registry change that adds or renames a custom requires
recompiling any expression module that references the affected
overload id — but that's already true under the shared-trampoline
design (the `pattern_id` was baked into the emitted wasm and would
need re-emission on renumbering). The only added coupling is that
renaming a custom's `helper_name` also requires recompiling
referencing modules. In an AOT world where compilation is cheap
relative to registration frequency, this is acceptable.

**No args-staging region.** LayoutPass reserves workspace slots
for each custom call's result; args are looked up in their callee
workspace slots exactly like built-ins. Nothing `StaticMemoryBuilder`-
adjacent is needed for customs.

(Alternatives considered: shared `cel_host_call_custom(pattern_id,
out_slot, args_ptr)` trampoline — smaller imports list, but forces
a host-side dispatch table, a staging region, and an asymmetric
codegen path, all to avoid an imports-list recompile that the
rest of the design already requires. Per-arity trampolines —
fixed-signature but duplicative across arities and still
asymmetric with built-ins. Both rejected in favour of per-function
imports, which collapse customs and built-ins onto the same ABI
surface.)

#### 4.6.2 Registration flow

Two phases. See `cel-host-surface.md` §5 for the user-facing API;
this subsection describes what happens inside the compiler.

**Phase A — compile time.** The embedder calls
`Compiler::Builder::RegisterFunction(FunctionDecl)` where
`FunctionDecl` is signature-only (no impl). The Compiler:

1. Forwards the declaration to cel-cpp's
   `TypeCheckerBuilder::AddFunction` (matching overload_id),
   so the checker resolves calls normally.
2. Calls `OverloadTableBuilder::RegisterCustom("my_upper_string",
   ImportModule::kCelHost, helper_name)`. `helper_name` defaults
   to the overload id; `FunctionDecl::helper_name` overrides.
   On collision with a built-in or with a prior custom, returns
   `AlreadyExists`.
3. At codegen, `UsedImports(used_overload_ids)` filters the
   registry to the subset this expression touches; each is
   emitted as a `CustomFunctionEntry` row into
   `cel.abi.functions.host_custom_imports[]` carrying the full
   signature (`function_name`, `overload_id`, `is_receiver`,
   `helper_name`, `arg_types[]`, `return_type`).

At ResolvePass, nothing special — the checker-supplied overload id
is interned exactly like a built-in's; only
`NodeAnnotation::overload_id` is written.

At codegen, the `kCall` arm is one unified emitter (§4.4); the
(module, helper_name) row in the overload table is emitted as a
`call $import` with the uniform slot-out signature. Built-ins and
customs are indistinguishable at this layer.

**Phase B — `program.Plan(bindings)` time.** The embedder
supplies a `RuntimeBindings` carrying `AddFunction(overload_id,
impl)` entries. `Plan` cross-checks (see
`cel-host-surface.md` §5.5): every `host_custom_imports[]` entry
must have a matching impl in `bindings.function_impls`. Missing →
`FailedPrecondition` citing `overload_id` + `function_name`.

#### 4.6.3 Host runtime

The bound impls live on the `Instance` that `Plan` constructed;
the ABI's `CustomFunctionEntry` drives the boxing/unboxing
trampoline shape:

```cpp
// Internal to api/internal/cel_host.cc — not user-visible.
struct BoundCustom {
  const abi::CustomFunctionEntry* entry;  // signature from ABI
  FunctionImpl                    fn;     // from RuntimeBindings
};
absl::flat_hash_map<std::string /*helper_name*/, BoundCustom> bound_;
```

At `LoadEval` (internal routine invoked by `Program::Plan`), the
host walks `cel.abi.functions.host_custom_imports[]` and, for
each entry, binds the wasm import `cel_host.<helper_name>` to a
trampoline that:

  1. Absorbs `UNKNOWN` / `ERROR` args into the out slot and
     returns (spec-mandated strict absorption, handled before the
     impl ever sees the args).
  2. Boxes each arg offset → `Value` per `entry->arg_types`.
  3. Invokes `fn(MakeConstSpan(values))`.
  4. Unboxes the returned `Value` into a `CelValue` at `out_slot`
     per `entry->return_type`. Return-kind mismatch → ERROR.

No arity cross-check at the registry level — the signature is
already in the ABI, and `Plan`'s cross-check runs against impls
only. The wasm linker additionally rejects signature-incompatible
bindings at instantiation time (belt + braces).

If the Program references a helper the bindings don't cover,
`Plan` returns `FailedPrecondition` at that call; `Program::
CheckCompatible(bindings)` runs the same audit without
instantiating wasmtime, for deployment preflight.

#### 4.6.4 Test strategy

  - **Unit**: `OverloadTableBuilder::RegisterCustom` collision
    rules: built-in id → `AlreadyExists`; duplicate custom →
    `AlreadyExists`. `InternOverloadId` returns the expected id.
  - **Unit**: `RuntimeBindings::AddFunction` collision with prior
    `AddFunction(same_overload_id, ...)` → `AlreadyExists`.
    `Find` returns the expected impl; `BoundOverloads` lists all.
  - **Integration**: fixture custom `my.upper(string) -> string`
    declared at compile, bound at Plan; `my.upper("abc") == "ABC"`
    round-trips e2e.
  - **Arity coverage**: one e2e test per arity in `{0, 1, 2, 3, 8}`,
    each with a distinct `overload_id` + matching wasm signature;
    args arrive at the impl in source order.
  - **Receiver style**: `x.upper()` with `is_receiver=true`
    binds against `(string) -> string` signature; args[0] is the
    receiver.
  - **Sibling calls don't collide**: `my.a(1, 2) + my.b("x")`
    each land in their own workspace slots and lower to distinct
    wasm imports with no shared state.
  - **Negative — missing impl at Plan**: compile referencing
    `my.upper`, `Plan` against empty bindings → `FailedPrecondition`
    citing `overload_id` + `function_name`.
  - **Negative — CheckCompatible preflight**: same setup, but
    `CheckCompatible` returns the same status without touching
    wasmtime.
  - **Import declaration**: `table.UsedImports(used_ids)` for an
    expression referencing only `my.a` produces one wasm import
    under `cel_host` named `my_a` — no extra imports for unused
    bound customs.
  - **Stdlib parity**: a bound `my.upper` cannot shadow a built-in;
    `RegisterCustom` on a built-in overload id fails.

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
  ;;    time against the descriptor pool (→ cel.abi.types[]).
  cel_host.cel_make_message(type_id, out_slot)         ;; (u32, u32) -> void

  ;; 2. Lower each field value into its own workspace slot.
  <emit expr_n>   -> slot_n   ;; CelValue (string) at slot_n
  <emit expr_a>   -> slot_a   ;; CelValue (int)    at slot_a

  ;; 3. Set each field.  field_ref_id is the intern id assigned at
  ;;    compile time (→ cel.abi.fields[] row carrying type_id +
  ;;    field_number + field_name + result_type).
  cel_host.cel_set_field(out_slot, field_ref_id_name, slot_n)  ;; (u32,u32,u32) -> void
  cel_host.cel_set_field(out_slot, field_ref_id_age,  slot_a)  ;; (u32,u32,u32) -> void
```

**Host surface (`api/internal/cel_host.cc`, full definition in
§4.7.6).** Four fixed host imports, regardless of message shape:

```
cel_host.cel_make_message(type_id: u32, out_slot: u32) -> void                                      (M7)
cel_host.cel_set_field   (msg_slot: u32, field_ref_id: u32, value_slot: u32) -> void                (M7)
cel_host.cel_get_field   (out_slot: u32, msg_slot: u32, field_ref_id: u32, attribute_id: u32) -> void   (M2 — §4.7.6)
cel_host.cel_has_field   (out_slot: u32, msg_slot: u32, field_ref_id: u32, attribute_id: u32) -> void   (M2 — §4.7.6)
```

Four i32s on the read/has side, not three: the trampoline needs
both the field intern id (resolves to FieldDescriptor) and the
attribute intern id (matches against the `PartialEval` unknown
pattern set, §4.7.6.3).  The `attribute_id = 0` sentinel means
"no attribute declared" — computed selects or non-ident-rooted
paths skip the unknown-pattern check.  `type_id` is interned at
compile time against the descriptor pool (recorded in
`cel.abi.types[]`); the host's dispatcher maps `type_id →
MessageFactory`.  `field_ref_id` is interned by `(type_id,
field_number)` — two call sites for the same field share a
row — and recorded in `cel.abi.fields[]` carrying `type_id +
field_number + field_name + result_type`.  `attribute_id` is
interned per call site (two reads of `c.name` at different
syntactic positions get distinct ids so pattern matching can
target them independently) and recorded in
`cel.abi.attributes[]`.  The host precomputes `field_ref_id →
(FieldDescriptor*, CelType)` and `attribute_id → AttributeEntry`
at `Plan` time so each trampoline invocation does two array
lookups on the hot path — no descriptor walk, no hash lookup.
See `cel-host-surface.md` §3.1 and §6 for the ABI schema.

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

#### 4.7.2 Map literals (`kCreateMap`) — SHIPPED M3 (2026-04-24) via three-path dispatch

> **Plan-vs-execution delta.**  The as-written §4.7.2 below describes
> a single `cel_map_create` + `cel_map_insert` + `cel_map_lookup`
> primitive set.  M3 shipped a richer **three-path origin dispatch**
> design: maps come in two flavours at the wire level
> (`CEL_MAP_ARENA` for literals, `CEL_MAP_HOST` for proto map
> fields + `Activation::Bind(Value::Map)`); indexing routes through
> `cel_map_lookup_arena` / `cel_host.cel_map_lookup` /
> `cel.cel_map_lookup` (the dispatcher) per operand origin.  See
> `map-list-dispatch.md` for the authoritative design and `m3-map-
> literals.md` for the slice retro.  This sub-section now reads as
> the M3 reconciliation; the empty-then-populate model is preserved
> only for the **construction** half (`cel_map_create` +
> `cel_map_insert`), which is per-arena and matches the original
> design.

**Representation (as-shipped).** Two CelKinds:

  - `CEL_MAP_ARENA` — a linear-memory `ArenaMapHeader` (16 B:
    `count, capacity, entries_offset, _pad`) with `count*48` B
    of `(key, value)` CelValue pairs in the arena.  Built by
    `cel_map_create` + `cel_map_insert`.
  - `CEL_MAP_HOST` — a host-side `HostMapBacking` (vector-backed
    `HostMap` or proto-reflection-backed `ProtoMap`) interned in
    the per-Instance `ExternrefTable`; the CelValue carries
    `payload.ref_slot=<slot>`.

**Codegen — `kCreateMap` empty-then-populate (kArena only).**
`{k1: v1, k2: v2}` lowers to:

```
  cel_map_create(out_slot, capacity)                 ;; (u32, u32) -> void
  <emit k1> -> slot_k1
  <emit v1> -> slot_v1
  cel_map_insert(out_slot, slot_k1, slot_v1)         ;; (u32,u32,u32) -> void
  <emit k2> -> slot_k2
  <emit v2> -> slot_v2
  cel_map_insert(out_slot, slot_k2, slot_v2)
```

**Codegen — `kCallExpr(_[_])` three-path dispatch.**  `m[k]` lowers
based on `m`'s `map_origin` (M3 `MapOriginVisitor`):

| `operand.map_origin` | emitted call |
|---|---|
| `kArena` (kCreateMap, ?: of arena arms) | `call $cel.cel_map_lookup_arena` |
| `kHost`  (kIdent[map<>], kSelect on map field) | `call $cel_host.cel_map_lookup` |
| `kDynamic` (mixed-origin ?:) | `call $cel.cel_map_lookup` (the dispatcher) |

The kDynamic dispatcher in `cel_runtime.c` tail-calls into the
arena or host arm via `__attribute__((musttail))` so the wasm
stack never grows.  Toolchain config: `-mtail-call` (clang),
`--enable-tail-call` (Binaryen), `wasmtime_config_wasm_tail_call_set`
(wasmtime).

**Runtime surface (as-shipped — `runtime/cel_runtime.{h,c}`).**

```c
// Construction (arena-side; called from kCreateMap codegen).
void cel_map_create(uint32_t out_slot, uint32_t initial_capacity);
void cel_map_insert(uint32_t map_slot, uint32_t key_slot,
                    uint32_t value_slot);
void cel_map_grow(uint32_t map_slot);  // internal

// Lookup — three paths.
void cel_map_lookup_arena(uint32_t out_slot, uint32_t map_slot,
                          uint32_t key_slot);   // pure wasm fast path
void cel_map_lookup      (uint32_t out_slot, uint32_t map_slot,
                          uint32_t key_slot);   // kDynamic dispatcher
extern void cel_host_cel_map_lookup(/*same shape*/)
    __attribute__((import_module("cel_host"),
                   import_name("cel_map_lookup")));  // kHost arm

// `size(m)` and `k in m` are M5 overload-table work; not in M3.
```

Duplicate keys at insert poison the map with `CEL_ERROR /
CEL_ERR_DUPLICATE_KEY` per langdef §"Map creation"; missing key
at lookup writes `CEL_ERROR / CEL_ERR_NO_SUCH_KEY` per
§"Indexing".

**Host backings (`api/internal/cel_host.{h,cc}`).**

```cpp
class HostMapBacking { /* abstract */ };
class HostMap final : public HostMapBacking { /* vector-backed */ };
class ProtoMap final : public HostMapBacking { /* reflection-backed */ };

absl::Status CelMapLookupImpl(
    uint32_t out_slot, uint32_t map_slot, uint32_t key_slot,
    TrampolineContext& ctx);   // Layer-2 trampoline body
```

`ProtoBacking::ReadField` on a MAP field returns
`Value::HostMap(std::make_shared<ProtoMap>(msg, field))`; the
Layer-2 trampoline interns this via `ExternrefTable::InternMap`
when assembling the `CEL_MAP_HOST` CelValue.

**Node storage.** `kWorkspaceSlot` on the `kCreateMap` node
(holds the `CEL_MAP_ARENA` CelValue); per-entry key/value slots
released after `cel_map_insert` consumes them.  `kCallExpr(_[_])`
gets its own workspace slot for the lookup result.

**Why not in OverloadTable.** `create_map` / `insert` / `lookup_*`
aren't cel-cpp overloads — they're codegen-side primitives.
`size(map)` / `k in m` / `m1 == m2` / `m1 + m2` **are** spec
overloads but don't ship until M5 (the kCall built-in overload
set), where they'll route through the OverloadTable.

#### 4.7.3 List literals (`kCreateList`) — SHIPPED M4 (2026-04-25) via three-path dispatch

> **Plan-vs-execution delta — two changes from the as-written design.**
>
>   1. **Three-path dispatch (mirror of M3 maps).**  Lists, like
>      maps, come in two wire flavours: `CEL_LIST_ARENA` (literals)
>      and `CEL_LIST_HOST` (proto repeated fields, vector-backed
>      `Activation::Bind(Value::List)`).  Indexing routes through
>      `cel_list_at_arena` / `cel_host.cel_list_at` / `cel.cel_list_at`
>      (the dispatcher) per `list_origin`.
>   2. **Construction primitive set is `create(out, count)` +
>      `set(list, index, elem)`, NOT `create / append / grow`.**
>      Per direct user direction during M4 ("the list is going to
>      be of fixed length / we know what the list size is / we
>      should have a way to set an element at an index / no grow").
>      Codegen always knows the element count at lowering time, so
>      `cel_list_create` zero-fills `count` element slots up front
>      and `cel_list_set(index, elem)` writes each known position.
>      Past-count `set` poisons the list with `CEL_ERR_OVERFLOW`.
>      M5 comprehensions will need either `cel_list_clear` /
>      `cel_list_set` over a pre-sized accumulator or a separate
>      dynamic-list primitive; the M5 plan picks one.
>
> See `m4-list-literals.md` for the slice retro and
> `map-list-dispatch.md §4.2 / §6 / §7` for the authoritative
> shared design.

**Representation (as-shipped).** Two CelKinds:

  - `CEL_LIST_ARENA` (= 7) — linear-memory `ArenaListHeader`
    (16 B: `count, capacity, elements_offset, _pad`) with
    `count*24` B of element CelValues.  Built by `cel_list_create`
    + `cel_list_set`.
  - `CEL_LIST_HOST` (= 17) — host-side `HostListBacking`
    (`HostList` vector-backed or `ProtoList` proto-reflection-
    backed) interned in `ExternrefTable::list_backings_`; the
    CelValue carries `payload.ref_slot=<slot>`.

**Codegen — `kCreateList` (kArena only).**  `[e0, e1, e2]` lowers to:

```
  cel_list_create(out_slot, 3)                        ;; (u32, u32) -> void
  <emit e0> -> slot_0;  cel_list_set(out_slot, 0, slot_0)
  <emit e1> -> slot_1;  cel_list_set(out_slot, 1, slot_1)
  <emit e2> -> slot_2;  cel_list_set(out_slot, 2, slot_2)
```

**Codegen — `kCallExpr(_[_])` three-path dispatch.**  Same shape
as maps; codegen branches on `operand.list_origin` (M4
`ListOriginVisitor`):

| `operand.list_origin` | emitted call |
|---|---|
| `kArena` | `call $cel.cel_list_at_arena` |
| `kHost` | `call $cel_host.cel_list_at` |
| `kDynamic` | `call $cel.cel_list_at` (the dispatcher) |

The dispatcher tail-calls into the arena or host arm via
`__attribute__((musttail))`.

**Runtime surface (as-shipped — `runtime/cel_list.h`).**

```c
// Construction.  All element slots zero-init to CEL_NULL; codegen
// follows up with cel_list_set per known index.  No grow/append.
void cel_list_create(uint32_t out_slot, uint32_t count);
void cel_list_set   (uint32_t list_slot, uint32_t index,
                     uint32_t elem_slot);

// Lookup — three paths.
void cel_list_at_arena(uint32_t out_slot, uint32_t list_slot,
                       uint32_t index_slot);   // pure wasm fast path
void cel_list_at      (uint32_t out_slot, uint32_t list_slot,
                       uint32_t index_slot);   // kDynamic dispatcher
extern void cel_host_cel_list_at(/*same shape*/)
    __attribute__((import_module("cel_host"),
                   import_name("cel_list_at")));  // kHost arm

// size / `in` / `==` / `+` are M5 overload-table work; not in M4.
```

Per langdef §"Indexing": list indices must be `CEL_INT` (uint /
double / bool indices are checker errors); negative indices and
indices `>= count` write `{CEL_ERROR, CEL_ERR_INDEX_OUT_OF_BOUNDS}`
into out_slot.  Non-int index writes `CEL_ERR_TYPE_MISMATCH`.

**Host backings (`api/internal/cel_host.{h,cc}`).**

```cpp
class HostListBacking { /* abstract — Size/At/ForEach */ };
class HostList final : public HostListBacking { /* vector-backed */ };
class ProtoList final : public HostListBacking { /* reflection-backed */ };

absl::Status CelListAtImpl(
    uint32_t out_slot, uint32_t list_slot, uint32_t index_slot,
    const TrampolineContext& ctx);   // Layer-2 trampoline body
```

`ProtoBacking::ReadField` on a REPEATED (non-map) field returns
`Value::HostList(std::make_shared<ProtoList>(msg, field))`.

**Node storage.** `kWorkspaceSlot` on the `kCreateList` node;
per-element scratch slots released after `cel_list_set`
consumes them.  `kCallExpr(_[_])` gets its own slot.

**Activation marshal + Eval decoder.** `EncodeList`
(`instance.cc`) interns a bound `Value::List` /
`Value::HostList` via `ExternrefTable::InternList` and writes
`{CEL_LIST_HOST, payload.ref_slot=<slot>}`; `DecodeArenaListAt`
walks `ArenaListHeader` + `count×24B` and recursively decodes via
`DecodeCelValueAt`.  `CEL_LIST_HOST` decode arm deferred —
host-bound lists round-trip through the activation marshaller
but never come back from Eval as a result.

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

#### 4.7.6 Host-side C++ interface — field reads (M2)

Canonical definition of `api/internal/cel_host.{h,cc}`.  M2 ships
the read side (`cel_get_field` + `cel_has_field`); M4 adds
`cel_message_eq`; M7 adds `cel_make_message` + `cel_set_field`.
Every later addition grows this same file.

Two-layer split, modelled on v1:

  - **Pure host logic** — runtime-agnostic free functions
    (`ReadField`, `HasField`).  No wasmtime dependency; unit-
    testable natively; reusable by any embedder that can hand
    over a `Message*` plus an allocator.
  - **Wasmtime trampoline layer** — a registration function plus
    a borrowed-state `CelHostBindings` struct.  Unwraps wasmtime
    types (i32 offsets, memory base, callback data), calls into
    the pure layer, writes the out slot.

The split is load-bearing: every host-side behaviour has one
canonical implementation in the pure layer.  The trampolines do
no CEL-semantic work — they only marshal.

##### 4.7.6.1 Pure host logic — `HostMessageBacking` virtual interface

As-shipped, Layer 1 is a virtual interface whose subclasses each
encapsulate a particular message provenance: `ProtoBacking` wraps a
`google::protobuf::Message*`; embedders subclass for JSON / XML / …
Two parallel hierarchies cover maps and lists (`HostMapBacking` →
`HostMap` / `ProtoMap`; `HostListBacking` → `HostList` / `ProtoList`).
Each backing returns a `cel::Value` (the public API leaf type, not a
runtime `CelValue`); Layer 2 marshals that `Value` into the wire
`CelValue` at `out_slot` using `MemoryView` + `ArenaAllocator` +
`ExternrefTable` from the `TrampolineContext`.

```cpp
// api/internal/cel_host.h (excerpt)
namespace celwasm {

class HostMessageBacking {
 public:
  virtual ~HostMessageBacking() = default;

  // `field_number == 0` means "resolve by name only" (non-proto
  // backings).  Spec-level errors (missing field, repeated at M2)
  // return `Value::Error(...)` inside the StatusOr's value;
  // infrastructure failures return non-OK Status.
  // `expected_type` comes from the FieldRefEntry's stamped CelType
  // and lets the backing coerce / validate before marshalling
  // (M2 ProtoBacking only validates; M5+ coercion paths reuse it).
  virtual absl::StatusOr<cel::Value> ReadField(
      int field_number, absl::string_view field_name,
      const cel::CelType& expected_type) const = 0;

  // True iff `msg.<field>` is set.  Proto2: explicit presence via
  // Reflection::HasField; proto3 singular scalar: true iff not at
  // type default; proto3 singular message: true iff set (implicit
  // presence).  Unknown field returns false — checker normally
  // rejects `has(msg.nope)`, this is defence-in-depth.
  virtual bool HasField(int field_number,
                        absl::string_view field_name) const = 0;
};

class ProtoBacking final : public HostMessageBacking {
 public:
  explicit ProtoBacking(const google::protobuf::Message* absl_nonnull msg);
  // ReadField + HasField bodies live in cel_host.cc.
  // ...
};
```

The CelValue shape `Value` boxes per `FieldDescriptor::CppType`:

| FieldDescriptor cpp_type | `cel::Value` |
|---|---|
| BOOL | `Value::Bool(...)` |
| INT{32,64} | `Value::Int(...)` |
| UINT{32,64} | `Value::Uint(...)` |
| FLOAT \| DOUBLE | `Value::Double(...)` |
| ENUM | `Value::Int(...)` (spec: enums → int) |
| STRING \| BYTES | `Value::String(...)` / `Value::Bytes(...)` |
| MESSAGE (singular) | `Value::HostMessage(std::make_shared<ProtoBacking>(submsg))` |
| MAP | `Value::HostMap(std::make_shared<ProtoMap>(msg, field))` (M3 envelope flip) |
| REPEATED non-map | `Value::HostList(std::make_shared<ProtoList>(msg, field))` (M4 envelope flip) |

Layer 2 takes the returned `Value`, calls `Encode...At(out_slot,
mem, refs, alloc)` on it, and writes the resulting `CelValue` bytes
through `MemoryView::WriteCelValue`.  String / bytes payloads hit
the arena via `ArenaAllocator::Alloc`; messages / maps / lists go
through the appropriate `ExternrefTable::Intern{,Map,List}` and the
returned `ref_slot` lands in the wire `CelValue`'s payload.

> **Plan-vs-execution delta — Layer 1 is virtual, not free
> functions.**  As-written §4.7.6.1 named two free functions
> `ReadField(msg, field_number, field_name, &out, alloc, intern)`
> and `HasField(msg, ...)` taking host-addressable callbacks for
> arena allocation and externref interning.  The shipped design
> hoisted those concerns into Layer 2 (`MemoryView` /
> `ArenaAllocator` / `ExternrefTable` on `TrampolineContext`) and
> made Layer 1 a polymorphic interface returning a runtime-
> agnostic `cel::Value`.  Why: the original free-function shape
> bound the host-arena / interner closures into Layer 1's
> callsites, which then required every embedder-supplied backing
> (JSON / XML / …) to also accept the same callbacks even though
> only ProtoBacking actually used them.  Pulling them out leaves
> backings purely declarative — they say what the field is, not
> how to ship it across the wasm boundary.

##### 4.7.6.2 Per-Plan state — split: runtime-agnostic vs wasmtime

As-shipped, the per-Plan host state is split across two structs in
two headers, so the runtime-agnostic Layer 2 (`cel_host.h`) is free
of wasmtime headers (the smoke test fakes can substitute
`MemoryView` / `ArenaAllocator` / `ExternrefTable` without linking
wasmtime), and Layer 3 (`cel_host_wasmtime.h`) holds the wasmtime
handles next to the linker-callback registration:

```cpp
// api/internal/cel_host.h — runtime-agnostic, used by Layer 2.

struct FieldRefEntry {
  uint32_t field_number = 0;  // 0 = "not proto-resolvable" sentinel
  std::string field_name;     // always populated
  // `FieldDescriptor*` is NOT cached here — the host resolves
  // against `msg.GetDescriptor()` on each call so the same
  // compiled expression works against multiple descriptor-
  // compatible message types.
};

struct AttributeEntry {
  std::string root_variable;              // "" = non-ident-rooted
  std::vector<std::string> qualifiers;    // e.g. ["name"] for c.name
};

struct CelHostBindings {
  // Decoded from cel.abi.fields[] / cel.abi.attributes[]; index
  // lookup in the trampoline (`entries[id - 1]`, id == 0 is
  // the sentinel).
  absl::Span<const FieldRefEntry> field_refs;
  absl::Span<const AttributeEntry> attributes;

  // Only populated by `Instance::PartialEval(activation, patterns)`.
  // Empty span for plain `Eval` — trampoline skips the check
  // without branching.
  absl::Span<const cel::AttributePattern> unknown_patterns;
};

// Per-eval context bundled into one struct to stay under the
// 6-param lint gate.  `alloc` is unused by has() but shared for
// signature uniformity — Layer 2's entry points all take this.
struct TrampolineContext {
  const CelHostBindings& bindings;
  MemoryView& mem;
  ExternrefTable& refs;
  ArenaAllocator& alloc;
};
```

```cpp
// api/internal/cel_host_wasmtime.h — Layer 3, wasmtime handles +
// the production ExternrefTable + per-Instance backing storage.

class HostExternrefTable final : public ExternrefTable { /* ... */ };

struct CelHostCallbackEnv {
  // Storage for the bindings spans.  `bindings` references these.
  std::vector<FieldRefEntry> field_refs_storage;
  std::vector<AttributeEntry> attrs_storage;
  CelHostBindings bindings;

  // Per-eval externref table — Reset() between Evals.
  HostExternrefTable refs;

  // Filled by Engine::Plan after the runtime + expr instances are
  // ready.  `memory` is the runtime-owned SHARED-memory handle both
  // modules share (read via wasmtime_sharedmemory_data); `arena_alloc_fn`
  // is the runtime's arena_alloc export, `malloc_fn` its dlmalloc
  // malloc — both bound onto the linker.  (Phase C: was a
  // `wasmtime_memory_t` + `cel_alloc_fn` pre-migration.)
  wasmtime_sharedmemory_t* memory = nullptr;
  wasmtime_func_t arena_alloc_fn = {};
  wasmtime_func_t malloc_fn = {};
};
```

`CelHostCallbackEnv` is passed by pointer as wasmtime callback-data
(not copied per call) so the address must outlive the store —
`InstanceImpl` owns it.  The Layer-3 callback bodies build a
shared-memory-backed `MemoryView` and `ArenaAllocator` per call from
`env->memory` + `env->arena_alloc_fn`, bundle them with `env->refs`
+ `env->bindings` into a stack-local `TrampolineContext`, and call
into the appropriate Layer-2 `Cel*Impl` entry point.

> **Plan-vs-execution delta — `cel_ref_intern` is gone.**  The
> as-written `CelHostBindings` cached a `wasmtime_func_t
> cel_ref_intern` for the trampoline to call back into wasm to
> intern a host message into the expr module's externref table.
> The shipped design pulls externref interning host-side: the
> trampoline calls `env->refs.Intern(backing)` directly (an
> in-process method on `HostExternrefTable`), no wasm round-trip.
> The expr module never gets to see the slot index until the
> trampoline writes the result `CelValue` back through
> `MemoryView::WriteCelValue`.  Why: avoids a host→wasm→host
> re-entry per message field read, simplifies the wasm runtime
> surface (no `cel_ref_intern` export needed), and keeps
> `HostExternrefTable` reset semantics tied to Plan/Eval lifetime
> instead of trying to coordinate with wasm-side state.

##### 4.7.6.3 Wasmtime trampoline registration

```cpp
// api/internal/cel_host_wasmtime.h
namespace celwasm {

// Registers cel_host.{cel_get_field, cel_has_field, cel_map_lookup,
// cel_list_at} on `linker`.  Lands as four imports today; M5 (msg-
// equality) and M7 (cel_make_message + cel_set_field) grow this
// list.  Must be called before
// `wasmtime_linker_instantiate(linker, expr_module)`.  `env` is
// borrowed; caller (`Engine::Plan`) keeps it alive for the
// lifetime of the Instance via ownership on `InstanceImpl`.
ABSL_MUST_USE_RESULT absl::Status RegisterCelHostImports(
    wasmtime_linker_t* absl_nonnull linker,
    CelHostCallbackEnv* absl_nonnull env);

}  // namespace celwasm
```

The signature takes a `CelHostCallbackEnv*` (Layer 3's combined
bindings + wasmtime handles + ExternrefTable bundle), not a bare
`CelHostBindings&`, because every trampoline body needs the
externref table + memory + cel_alloc handles in addition to the
bindings spans.

##### 4.7.6.4 Trampoline body (`cel_get_field`)

The `cel_get_field` trampoline is the load-bearing M2 code path.
Executed per select-expression evaluation.  Layer-3
(`cel_host_wasmtime.cc`) decodes wasmtime args and assembles a
`TrampolineContext`; Layer-2 (`cel_host.cc::CelGetFieldImpl`)
runs the body in a runtime-agnostic shape:

```
CelGetFieldImpl(out_slot, msg_slot, field_ref_id, attribute_id, ctx):

  1. Read CelValue at msg_slot via ctx.mem.ReadCelValue.
  2. Absorb UNKNOWN / ERROR:
       if msg.kind == CEL_UNKNOWN: write UNKNOWN to out_slot; return OK.
       if msg.kind == CEL_ERROR:   write ERROR to out_slot;   return OK.
     (Strict absorption per langdef.md — downstream `cel_unknown_merge`
     calls in generated code are redundant for this specific edge
     but cheap; the trampoline absorbing here saves a backing
     dispatch.)

  3. Type guard:
       if msg.kind != CEL_MESSAGE: write ERROR(kTypeMismatch); return OK.
     (Checker should have rejected; runtime guard is defence.)

  4. Unknown-pattern check:
       if attribute_id != 0 and !ctx.bindings.unknown_patterns.empty():
         attr = ctx.bindings.attributes[attribute_id - 1]
         for pattern in ctx.bindings.unknown_patterns:
           if pattern.Matches(attr):
             write CelValue{CEL_UNKNOWN, attribute_id} to out_slot;
             return OK.
     Short-circuits the backing dispatch entirely on UNKNOWN — this is
     why the unknown check lives at the trampoline, not at codegen.

  5. Field-ref resolution:
       field = ctx.bindings.field_refs[field_ref_id - 1]
       backing = ctx.refs.Lookup(msg.payload.msg_slot)
       if backing == nullptr:
         write ERROR(kHostAdapterError); return OK.

  6. Dispatch:
       absl::StatusOr<cel::Value> result = backing->ReadField(
           field.field_number, field.field_name, expected_type);
       if (!result.ok()) return result.status();   // infra failure

  7. Marshal `*result` into a 24B CelValue at out_slot via
     ctx.mem.WriteCelValue (scalars inline, spans through
     ctx.alloc.Alloc, sub-messages through ctx.refs.Intern).
```

Steps 1–3 and 7 are trampoline-side (pure marshal); step 6 is the
entire CEL semantics.  Steps 4–5 are the M2-new layer (the unknown
gate and the per-Plan field-ref table lookup).  Non-OK Status is
reserved for infrastructure failures (memory-view OOB, missing
backing, reflection error); spec-level errors (field-not-found,
wrong-type access) travel inside the marshalled CelValue at
`out_slot` so the caller's `cel_unknown_merge` chain absorbs them
uniformly.

##### 4.7.6.5 Trampoline body (`cel_has_field`)

Identical shape, with step 6 replaced by:

```
       bool present = HasField(*msg_ptr, field.field_number,
                               field.field_name);
       out_staging = {CEL_BOOL, present ? 1 : 0};
```

and with step 4's match-writing `UNKNOWN` instead of `false` —
`has(c.name)` where `c.name` matches an unknown pattern must
produce UNKNOWN, not a spuriously-confident boolean.

##### 4.7.6.6 Invariants

  - **Aliasing safety.** `msg_slot` and `out_slot` may be the
    same workspace cell (Sethi–Ullman may alias them at M8).
    `ReadField` must read the msg's `CelValue` fully before
    writing `*out` — the trampoline copies it to a local
    `out_staging` and writes at step 7 after the pure call
    returns, so aliasing is safe by construction.
  - **No allocation on the UNKNOWN / ERROR paths.**  Neither
    absorption (step 2) nor pattern-match (step 4) calls
    `alloc_callback` or `intern_callback` — both short-circuit
    before the pure helpers can.  An expression with every
    ident declared unknown never touches the expr module's
    arena.
  - **Bindings lifetime.** `CelHostBindings` address captured
    as wasmtime callback-data.  Held by `InstanceImpl`; lives
    as long as the Instance.  Re-invoking `PartialEval` on
    the same Instance with a different pattern set mutates
    `bindings.unknown_patterns` in place — no re-registration.
  - **cel_refs opacity.** The trampoline treats `cel_refs` as
    an opaque externref table: it asks the table "give me the
    Message\* at slot `k`" without caring how the table got
    populated (ident prelude materialises; inner `cel_get_field`
    on a message-typed field adds via `intern_callback`).  The
    table's own invariants live with the codegen emitter.

##### 4.7.6.7 Testing

Mandatory coverage in `api/internal/cel_host_test.cc`:

  - `ReadField` × every `FieldDescriptor::CppType` landing on a
    realistic Customer fixture (bool / int32 / int64 / uint32 /
    uint64 / float / double / enum / string / bytes / message).
  - `ReadField` × repeated / map field → `CEL_ERROR`.
  - `ReadField` × unresolvable field number + unresolvable name
    → `CEL_ERROR`.
  - `HasField` × proto2 explicit-presence (set vs unset).
  - `HasField` × proto3 scalar default vs non-default.
  - `HasField` × proto3 singular message unset vs set.
  - **Trampoline absorption**: `msg_slot`'s CelValue is UNKNOWN
    / ERROR → out_slot gets that kind without invoking
    `ReadField`.
  - **Trampoline pattern match**: `bindings.unknown_patterns`
    contains a pattern matching `attribute_id`'s entry →
    out_slot gets UNKNOWN; no allocator or interner call.
  - **Trampoline aliasing**: `msg_slot == out_slot` with a
    message CelValue at that cell produces the correct read.
  - **Multiple-pool descriptor**: two `Customer` messages built
    against distinct `DescriptorPool` instances both read
    correctly (each call resolves against the *passed*
    message's descriptor, not a cached pool).

E2E parity (`e2e/eval_test.cc`) covers the full
pipeline; the unit tests above lock the host interface itself.

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

class StaticMemoryBuilder {
 public:
  explicit StaticMemoryBuilder(uint32_t base_offset);

  // Returns the CelValue frame's linear-memory offset. Infallible —
  // the builder grows as needed. Literals always land in rodata
  // (§4.2); there is no cap, no fallback path, no runtime-
  // initialised-literal variant.
  uint32_t AllocateNull();
  uint32_t AllocateBool(bool v);
  uint32_t AllocateInt(int64_t v);
  uint32_t AllocateUint(uint64_t v);
  uint32_t AllocateDouble(double v);
  uint32_t AllocateString(absl::string_view s);
  uint32_t AllocateBytes(absl::string_view b);

  // Signature-final stubs until M5/M6; body ABSL_CHECK(false)s so
  // any M1 caller crashes loudly.
  uint32_t AllocateList(absl::Span<const uint32_t> element_offsets);
  uint32_t AllocateMap(absl::Span<const uint32_t> key_offsets,
                       absl::Span<const uint32_t> value_offsets);

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

As-shipped (`compiler/codegen/slot_allocator.h`):

```cpp
namespace celwasm {

class SlotAllocator {
 public:
  // base_offset must be 8-byte aligned (CelValue alignment).
  SlotAllocator(uint32_t base_offset, bool debug_mode);

  uint32_t Acquire();                      // byte offset of a 24B cell
  void Release(uint32_t offset);           // no-op in debug_mode

  uint32_t peak_slots() const;
  uint32_t total_bytes() const;            // peak_slots * 24
  uint32_t base_offset() const;
  bool debug_mode() const;
};

}  // namespace celwasm
```

Debug mode: `Release` is a no-op. Peak slots then equals the number
of `kWorkspaceSlot` nodes in the tree. Memory cost: 24 B × nodes,
bounded by expression size (realistic worst case: ~12 KB for 500
nodes).

> **Plan-vs-execution delta — comprehension scope methods deferred
> to M5.**  The original §6.2.2 promised `PushScope` / `PopScope`
> on `SlotAllocator` so per-iteration intermediate slots could die
> at `PopScope` instead of leaking across iterations.  These
> methods don't exist yet — `compiler/codegen/slot_allocator.h`
> ships only `Acquire` / `Release` / `peak_slots` / `total_bytes`.
> Comprehension lowering moved to a follow-on milestone after M5
> (`m5-comprehensions-followon.md`); the scope-aware allocator is
> part of that scope.  Until it lands, the naive Sethi–Ullman path
> (S10 still pending) means `Release` is the only mechanism for
> slot reuse and `Acquire` monotonically bumps the cursor.

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

As-shipped (`compiler/codegen/expr_lower.h`, M5.F):

```cpp
namespace celwasm {

struct LoweringOptions {
  // mem_size_bytes flows to the cel_reset(arena_base, arena_limit)
  // call emitted at the top of every $eval body.  64 KiB default
  // (one wasm page); compile.cc raises to 128 KiB to match the
  // runtime's 2-page imported-memory minimum.
  uint32_t mem_size_bytes = 64u * 1024u;
};

struct LoweredFunction {
  BinaryenFunctionRef absl_nonnull func;        // () -> i32
  std::vector<FieldRefRow> field_refs;          // serialised into cel.abi
};

ABSL_MUST_USE_RESULT absl::StatusOr<LoweredFunction> LowerToEvalFunction(
    const TypedAst& ast, const StaticLayout& layout,
    absl::string_view func_name, WasmModule& mod,
    const OverloadTable& overload_table,
    const LoweringOptions& opts = {});

}  // namespace celwasm
```

The internal `EmitCtx` (in `expr_lower.cc`) carries: the module
reference, layout reference, overload-table reference, and the
running field-refs intern table populated as `kSelect` arms emit.
Gone from the v1 `LoweringContext`: `idents` (now read off
`StaticLayout::variables` + `NodeAnnotation::storage` for
kIdent), `scratch_slot` + `GetScratchSlotLocal` (slots come from
`NodeAnnotation::storage` allocated by LayoutPass), `prologue_setups`
(replaced by a fixed `cel_reset(arena_base, arena_limit)` + the
ident workspace prelude), `EmitCheckedArithmetic` (deleted whole),
per-visitor helper-string plumbing (replaced by
`OverloadTable::Lookup(ann.overload_id)` in `EmitGeneralCall`).

> **Phase C delta (shipped) — `$eval` prologue is `(call
> $arena_reset)`, no args.**  The `mem_size_bytes` comment above
> describes the pre-migration `cel_reset(arena_base, arena_limit)`
> prologue.  As-shipped, codegen emits a zero-arg
> `(call $arena_reset)` (§8.3) and `LoweringOptions::mem_size_bytes`
> no longer feeds a reset call — it now only sizes the *minimum
> page count* of the imported shared `cel.memory`
> (`compile.cc::InstallExprModuleImports` → `PagesForBytes`).

> **Plan-vs-execution delta — `LowerToEvalFunction` takes an
> `OverloadTable&` + `LoweringOptions` shipped with M5.F.**  The
> as-written §7.1 signature was `(ast, layout, func_name, mod)`.
> M5.F threaded `const OverloadTable&` into the call so
> `EmitGeneralCall` could resolve `ann.overload_id` without a
> back-channel (the table is built once in `compile.cc` from
> `OverloadTableBuilder().Build()`); `LoweringOptions` arrived
> earlier (M2) so codegen could pick the `cel_reset` arena
> limit at emit time.  Both additions are signature-final; the
> deltas above don't ripple further.

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

    case kCallExpr: {
      // Special arms first (origin-aware indexing, branch-style
      // control flow); see expr_lower.cc::Emit for the full set.
      if (call.function() == "_[_]") return EmitKIndexCall(...);
      if (IsControlFlow(call.function())) return Unimplemented(...);
      const OverloadImpl* h = ctx.overload_table.Lookup(a.overload_id);
      if (h == nullptr || IsPendingRuntimeExport(h->name)) {
        return Unimplemented(...);
      }
      DCHECK(a.storage.kind == StorageKind::kWorkspaceSlot);
      // Uniform ABI (§4.2): helper(out_slot, args…) -> void. Built-
      // ins and customs share one emitter — both resolve to a named
      // wasm import via (h->module, h->name). No storage-kind branch,
      // no built-in/custom branch — every call result is a workspace
      // slot, every helper is an import.
      return EmitGeneralCall(ctx, expr, call, a);
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
      // field. Empty-then-populate like maps/lists; no
      // MessagePattern side table.
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
    changes.  As-shipped (`compiler/internal/compile.cc`,
    `InstallOverloadImports` + the per-feature `Install*Imports`
    helpers): `InstallExprModuleImports` first declares the imported
    shared `cel.memory` (with the rodata active data segment) +
    `cel.arena_reset` + `cel.arena_alloc` (Phase C — these replaced
    `cel.cel_reset` + `cel.cel_alloc`) + the fixed host surfaces
    (`InstallSelectImports` for cel_get_field / cel_has_field;
    `InstallMapImports` for cel_map_create / insert / lookup_arena
    / lookup + the `cel_host_cel_map_lookup` re-export trampoline;
    `InstallListImports` mirrors for lists).  Then
    `InstallOverloadImports` walks `[1..table.size()]` and for each
    kCelRuntime helper not already installed (and not in
    `kPendingRuntimeExports` — §4.4.1), infers arity from the
    `_at_v` / `_at_vv` suffix and emits one `AddFunctionImport`
    under module `"cel"`.  The expr module ends up importing from
    three modules today: `"cel"` (runtime helpers + arena + lookup
    primitives), `"cel_host"` (cel_get_field / cel_has_field /
    cel_map_lookup / cel_list_at), and `"cel_env"` (cel_log).
    The old v1 `ImportCel2` / per-helper-name hand-imports are gone.
  - `DeclareHostImports` for the fixed host surface — split into
    `InstallSelectImports` / `InstallMapImports` /
    `InstallListImports` in `compile.cc` (one site each).  Module
    name `"cel_host"` is hard-coded at those sites.  Per
    `feedback_no_lazy_imports`, the imports are declared up front
    even when the AST doesn't reference them — the wasm validator
    elides unused imports at instantiate time.  Custom functions
    (M6) are NOT declared here — each registered custom will become
    an overload-table row routed through `InstallOverloadImports`
    like any built-in.
  - The `cel_env` logging import (`cel_log`) — one site in codegen,
    unchanged; not an overload, not a host function surface.
  - The `cel.abi` custom section (M2.B.2), serialised proto with
    `variables[]`, `attributes[]`, `fields[]`, and `memory.*`
    (rodata_base / workspace_base / arena_base) populated today;
    M6 will extend with `host_custom_imports[]` (§4.6); M7 with
    `types[]` (§4.7.1).
  - `cel_refs` (the externref table) is host-side as of M3 (lives
    on `HostExternrefTable` in `cel_host_wasmtime.h`), reset per
    Eval — there is no in-wasm `cel_refs` table to emit anymore.

> **Plan-vs-execution delta — `cel_message_eq` /
> `cel_make_message` / `cel_set_field` not yet declared.**  The
> as-written §7.4 listed all four extension imports as "still
> declared once up front."  Today only `cel_get_field` +
> `cel_has_field` (M2) + `cel_map_lookup` (M3) + `cel_list_at`
> (M4) ship under `cel_host`.  Message equality lands with the
> M5 kCall built-in overload set (it routes through the
> OverloadTable, not the fixed host surface — see §6 / Slice 6
> remainder); `cel_make_message` + `cel_set_field` land at M7
> (proto literals).

## 8. Runtime changes — `compiler/runtime/*`

### 8.1 Build flags — `runtime/BUILD.bazel`

> **Phase C delta (shipped).**  `--import-memory` is gone: the
> runtime now DEFINES + exports its (shared) memory rather than
> importing it.  The export list is no longer hand-written inline —
> it's driven by `wasm_exports.txt` → a wasm-ld response file.
> `cel_alloc` is gone; `arena_init`/`arena_alloc`/`arena_reset`
> replace it.  As-shipped flags on `cel_runtime_wasm.bin`:

```
# wasm32-wasi-threads cross-compile via //third_party/wasi_sdk
-nostartfiles
-Wl,--no-entry
-Wl,--global-base=8192                    # reserve [0, 8192) for expr data segments
-Wl,--allow-undefined-file=$(location wasm_imports.txt)
-Wl,@$(location :wasm_export_args)        # one --export=<name> per wasm_exports.txt entry
-mtail-call                               # musttail dispatchers lower as return_call
```

`--export-all` is retired.  Every exported symbol is explicit
(`wasm_exports.txt`, the single source of truth, cross-checked
against `celwasm::abi::CelRuntimeHelpers()` by
`//abi:runtime_catalogue_consistency_test`) so new
additions are visible in code review.

**Original drafting (historical):** the pre-migration freestanding
build imported memory (`-Wl,--import-memory=cel,memory`) and exported
`cel_alloc` among an inline `--export=` list.

### 8.2 Arena cursor — fixed memory-base offsets

> **Phase C delta (shipped) — the fixed bytes-8/12 cursor is gone.**
> The arena is now **malloc-backed and per-Instance**, with its
> state in a runtime BSS struct, not at fixed memory offsets.  See
> `runtime/cel_arena.c` and `wasi/DESIGN.md` §4.  The
> as-shipped ABI:
>
> ```c
> // runtime/cel_arena.c
> typedef struct {
>   uint8_t* base;        // malloc'd buffer base in linear memory
>   uint32_t capacity;    // total bytes (CELWASM_ARENA_CAPACITY_BYTES = 64 KiB)
>   uint32_t cursor;      // next free byte, relative to base
>   uint32_t initialized; // 0 or 1
> } CelArena;
> static CelArena g_arena;  // BSS — zero-init at instantiation
>
> void arena_init(uint32_t cap_bytes);  // once per Instance; malloc()s base
> uint32_t arena_alloc(uint32_t n);     // 8-aligns, bumps; returns ABSOLUTE
>                                       //   offset (cel_mem_base()+ret resolves
>                                       //   on both wasm + host); 0 on OOM;
>                                       //   traps if !initialized
> void arena_reset(void);               // O(1): cursor = 0 (no args)
> uint32_t arena_cursor(void);
> uint32_t arena_capacity(void);
> ```
>
> On the **wasm** build `arena_init` `malloc()`s the backing buffer
> from the dlmalloc heap (high in memory, above `__heap_base`); the
> returned malloc pointer IS an absolute offset in the shared memory,
> so `arena_alloc` returns it directly.  On the **native** build the
> arena is backed by a slice of the test `g_memory[]` so unit tests
> exercise the same byte layout.  The host seeds the arena once per
> Instance (`engine.cc::SeedRuntimeArena` →
> `arena_init(CELWASM_ARENA_CAPACITY_BYTES)`); `arena_alloc` traps if
> called before init (CLAUDE.md "Unimplemented features" rule).  The
> `StaticLayout::arena_base` field is now a legacy artifact —
> codegen no longer emits the old `cel_reset(base, limit)` prologue
> that consumed it (§8.3).
>
> The original fixed-offset design follows as historical context.

**Original drafting (historical — fixed bytes 8/12).**
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

As-shipped (`runtime/cel_runtime.c`, M1):

```c
enum {
  kBumpOffset  = 8u,
  kLimitOffset = 12u,
};

// Aligned pointer load/store rather than memcpy: clang's wasm32
// backend lowered memcpy(dst, &v, 4) as three byte-stores (high 3
// bytes only) which left the cursor's LSB stale on every reset.
// Explicit `*(uint32_t*)p = v` compiles to a single `i32.store`.
static uint32_t load_u32(uint32_t off) {
  return *(const uint32_t*)(cel_memory_base_() + off);
}
static void store_u32(uint32_t off, uint32_t v) {
  *(uint32_t*)(cel_memory_base_() + off) = v;
}

void cel_reset(uint32_t arena_base, uint32_t arena_limit) {
  store_u32(kBumpOffset,  arena_base);
  store_u32(kLimitOffset, arena_limit);
}

uint32_t cel_alloc(uint32_t n) {
  uint32_t need = align_up(n, 8u);
  if (need == 0) need = 8u;
  uint32_t bump  = load_u32(kBumpOffset);
  uint32_t limit = load_u32(kLimitOffset);
  if (bump + need > limit) return 0;
  store_u32(kBumpOffset, bump + need);
  memset(cel_memory_base_() + bump, 0, need);
  return bump;
}
```

`cel_memory_base_()` resolves to offset 0 of the imported
`cel.memory` on wasm32 (via an inline-asm opacity barrier — see the
file's head comment for why `(uint8_t*)0` is otherwise UB-elided
under clang's wasm32 backend) and to a `static uint8_t g_memory[]`
on the host build, so native tests exercise the same byte layout
that the wasm runtime sees.

**Codegen side.** Binaryen emits `i32.load offset=8` /
`i32.store offset=8` directly — no import, no helper call. One
constant per site.

### 8.3 `cel_reset` ownership

> **Phase C delta (shipped).**  `cel_reset(base, limit)` is removed.
> Codegen emits a single `(call $arena_reset)` (the zero-arg
> `cel.arena_reset` import) as the first instruction of every `$eval`
> body.  The "emit the reset in the prologue, not from the host"
> decision below still holds — the only change is that the reset is
> now argument-free (the arena base/capacity are owned by the
> runtime's `g_arena`, established once at `arena_init`), so there
> are no compile-time `i32.const base`/`i32.const limit` operands.
> The original argument-carrying form follows as historical context.

**Original drafting (historical).**
Today: runtime exports `cel_reset`; host calls it before every
`CallEval`.

Tomorrow: **codegen emits a `cel_reset(<rodata_size>, <mem_size>)`
call as the first instruction of every `$eval` body**, using
compile-time-constant offsets from the layout pass. The runtime
still *provides* the `cel_reset` implementation (the expr module
imports it like any other `cel.*` helper); what goes away is the
host-side init phase. The host instantiates expr + runtime,
invokes `$eval`, and reads the result — no pre-`$eval` reset
call, no runtime-private state that needs priming.

Rationale: arena_base and arena_limit are compile-time constants
of the module (rodata_size determines base; page-count-times-64K
determines limit). Calling `cel_reset` from the host is an
extra round-trip that buys nothing over emitting two `i32.const`s
and a `call` at the top of `$eval`. Between-eval isolation is
still trivial — each `$eval` entry re-resets the cursor, so
carry-over from a prior call is impossible by construction.

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
safe. Must run under `bazel test //...` (default suite, not
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

## 9. Host runtime — `eval/engine.{h,cc}`, `eval/instance.{h,cc}`

The runtime side of the host surface lives across two classes:
`cel::Engine` (process-shared wasmtime fixture, owns the engine +
parsed runtime module) and `cel::Instance` (per-Plan execution
handle, owns store + memory + linker + instances).  `Plan` is the
public entry point: `cel::Engine::Plan(program)` — see
`cel-host-surface.md` §2.2.5.  Plan parses the program's ABI,
spins up the wasmtime store, instantiates both modules, and
returns an Instance ready for `Eval`.

`eval/host/host_loader.{h,cc}` was deleted in the runtime-
isolation work.  Its earlier role (single-class wasmtime
boilerplate) is now split for clarity: Engine is the "what's
shared across all Plans" half; Instance is the "what's owned per
Plan" half.

> **Plan-vs-execution delta — `Plan(program)` ships before
> `Plan(program, bindings)`.**  The original §9 + §4.6.2 named
> `Engine::Plan(program, bindings)` so customs / descriptor pools
> could be wired at Plan time.  As of 2026-04-25 (M5.F)
> `engine.h::Engine::Plan` takes only a `const Program&`;
> `cel::RuntimeBindings` is documented in `cel-host-surface.md`
> §2.4 as a future surface.  The current `Plan` carries enough
> for M1–M5: it decodes the wasm `cel.abi` custom section
> (variables / fields / attributes), builds a `CelHostBindings`,
> and wires the cel_host trampolines via `RegisterCelHostImports`
> against `Engine::Builder()` defaults (the global descriptor pool
> at construction time).  M6 (customs) + M7 (proto literals)
> introduce the second `Plan(program, bindings)` overload; the
> existing zero-bindings overload stays for backward compat.

### 9.1 Two-phase instantiation (runtime-owned shared memory)

> **Phase C delta (shipped) — the host no longer allocates memory.**
> The runtime instance owns + exports the (shared) memory; the host
> pulls it off `helpers_instance` (renamed from `runtime_instance` in
> m28, 2026-06-08, to cover both dynamic and static link modes), clones
> the shared-memory handle,
> and binds it on the linker as `cel.memory` BEFORE the expr module
> instantiates (`engine.cc::BindRuntimeMemory`).  It then seeds the
> per-Instance arena via `arena_init`.  This reverses the
> "host-allocated `cel.memory`, both modules import" model the
> original §9.1 (below) described.

As-shipped (`eval/engine.cc::Engine::Plan` →
`InstantiateRuntime`):

```cpp
absl::StatusOr<Instance> Engine::Plan(const Program& program) const {
  auto impl = std::make_unique<celwasm::InstanceImpl>();

  // 1. Per-Plan store + a sandboxed WASI config (absl/cctz pull in
  //    wasi-libc env/stdio/clock imports; wasmtime's reference WASI
  //    impl resolves them).  No host memory allocation here.
  if (auto s = InitStore(wasmtime_.get(), impl.get()); !s.ok()) return s;

  // 2. Linker setup: bind cel_env.cel_log; register cel_host
  //    trampolines against impl->host_env; define the WASI stubs.
  //    `cel.memory` is NOT bound here — the runtime owns it.
  if (auto s = InitLinker(wasmtime_.get(), impl.get()); !s.ok()) return s;

  // 3. Phase 1: instantiate cel_runtime.wasm, then:
  //      BindRuntimeMemory  — pull the SHARED `memory` export, clone
  //                           the handle, bind it as cel.memory.
  //      EnforceRuntimeMemoryInvariants — A13 page floor + A14
  //                           __heap_base ≥ reserved low region.
  //      BindAllRuntimeExports — bind every cel.* helper named by
  //                           the ABI catalogue (single source of
  //                           truth — see callout below).
  //      BindRuntimeFuncHandles — cache arena_alloc + malloc handles
  //                           for the cel_host trampolines.
  //      SeedRuntimeArena   — arena_init(CELWASM_ARENA_CAPACITY_BYTES).
  if (auto s = InstantiateRuntime(wasmtime_.get(), impl.get());
      !s.ok()) return s;

  // 4. Phase 2: parse + instantiate the expr module against the
  //    now-complete linker (it imports the bound cel.memory +
  //    cel.arena_* + cel.* helpers); cache the eval export.
  if (auto s = InstantiateExpr(wasmtime_.get(), impl.get(),
                               program.wasm_bytes());
      !s.ok()) return s;

  // 5. Decode the cel.abi custom section, populate CelHostBindings
  //    from cel.abi.{fields, attributes}, and park on the Instance.
  //    NotFound is tolerated for synthetic WAT fixtures.
  // ...
  return Instance(wasmtime_, std::move(impl));
}
```

The wiring order side-steps the expr↔runtime "circular import" the
predecessor design wrestled with: `cel.memory` + the runtime's
helper exports are bound on the linker after the runtime
instantiates but before the expr module does, so the expr's imports
all resolve.

> **Plan-vs-execution delta — catalogue-driven `BindAllRuntimeExports`,
> not lazy `UsedImports` (and no longer a hand-maintained list).**
> The as-written design imagined the host binding only the runtime
> exports the expr module actually imports, driven by
> `OverloadTable::UsedImports(used_ids)`.  As-shipped,
> `BindAllRuntimeExports` (`engine.cc`) iterates
> `celwasm::abi::CelRuntimeHelpers()` — the `cel`-namespace span of
> the ABI catalogue (`abi/runtime_catalogue`) — and binds
> every helper unconditionally.  This is the single source of truth:
> codegen's import-declaration pass (`compile.cc`) consumes the same
> catalogue, so the bind set and the import set cannot drift, and
> `wasm_exports.txt` is cross-checked against the catalogue by
> `runtime_catalogue_consistency_test`.  (The earlier hand-maintained
> `kRuntimeExports` array was removed 2026-05-22.)  `arena_alloc` +
> `malloc` are additionally cached as raw wasmtime func handles on
> `host_env` (`BindRuntimeFuncHandles`) so cel_host trampolines can
> allocate without round-tripping the linker.

### 9.2 Deletions

  - `cel_alloc(24)` call for the sret slot at `host_loader.cc:429` —
    the sret slot is a `kWorkspaceSlot` offset known at compile time;
    the host passes it as the out-slot param (or `eval(0)` means "use
    the default output slot", which the expr reads from its layout).
  - Host-side arena priming pre-`CallEval` — codegen emits a
    `(call $arena_reset)` as the first instruction of `$eval`, so the
    host no longer primes the arena per-Eval (§8.3).  The one-time
    `arena_init` at Plan time (`SeedRuntimeArena`) is the only
    host-driven arena call.

> **Phase C delta:** `host_loader.cc` itself was deleted in the
> runtime-isolation work (its role split into Engine + Instance —
> see the §9 preamble); the bullets above describe the *behaviour*
> that retired, not live line numbers.  `cel_alloc` / `cel_reset`
> no longer exist as runtime exports (replaced by `arena_*`, §8.2).

## 10. Future-milestone absorption

Two milestones have effects that reach into the rewrite's
shape: M2 (idents + proto field reads + `Activation` + unknown
attributes) and M5 (comprehensions).  Both must slot into the
M1 skeleton *without* a schema change on `NodeAnnotation` or a
new `StorageKind`.  The sub-sections below enumerate what
each milestone adds and how the existing design absorbs it.

### 10.1 Unknowns and `Activation` (M2) — how this design absorbs them

**Status: shipped 2026-04-24.**  See
`rewrite/m2-ident-select-unknowns.md` for the close-out pass.

Scope change vs the original M1-plan "After M1" list: unknown
propagation / partial evaluation was originally slated for M4
alongside 3VL + message equality.  It moved to M2 because
`Activation` is the natural home for attribute patterns
(declaring "this path is unknown"), and `Activation` already
ships in M2 as the vehicle for idents.  M4 now owns just 3VL
+ the error surface.  See `m1-scalar-pipeline.md §10` for the
milestone list and `conformance/README.md` for the
conformance forecast this unlocks.

#### 10.1.1 What M2 adds

  - `kIdent` + `kSelect` arms in `expr_lower` (§7.2).
  - `api/internal/cel_host.{h,cc}` (Layer 1 backing semantics +
    Layer 2 runtime-agnostic trampoline bodies) +
    `api/internal/cel_host_wasmtime.{h,cc}` (Layer 3 wasmtime
    glue) — transcribed from v1 M3 G2/G3.  Lives at
    `api/internal/` (not `host/`) because it's an implementation
    detail of `Engine::Plan` — the only caller that registers
    these imports on the linker — not a public-host surface.
    Precedent: `instance_impl.{h,cc}` +
    `wasmtime_engine_state.{h,cc}` already there.
    > **Plan-vs-execution delta:** as-written §10.1.1 named a
    > single `cel_host.{h,cc}` file.  As-shipped split Layer 3
    > into its own TU so runtime-agnostic code (Layers 1–2) is
    > free of wasmtime headers and the smoke-test harness can
    > substitute fakes without linking wasmtime.
  - `cel::Activation` — user-facing class on the public API
    (`cel-host-surface.md` §2.6).  Carries `Bind(name, Value)`;
    `BindLazy(...)` is signature-final but body-stubbed until a
    later milestone exercises lazy resolution.
  - `Instance::PartialEval(activation, absl::Span<const
    AttributePattern>)` — second entry point on `Instance`
    (see `cel-host-surface.md` §2.3).  `AttributePattern`s are
    parsed once per call and matched against `AttributeId`s the
    resolver produced from `ident` + `select` chains.
  - Runtime `cel_unknown_merge` helper — already landed in v1 as
    M4 Slice A (`runtime/cel_runtime.{h,c}`, per the existing
    checklist).  M2 wires it into the M2-new lowering paths:
    every `kSelect` that resolves against an unknown-patterned
    root attribute traps through `cel_host.cel_get_field` →
    `cel_host` returns a `CelValue{kind:CEL_UNKNOWN,
    payload.attr_id:...}` → downstream ops absorb via
    `cel_unknown_merge`.
  - `Customer` proto fixture port from `compiler/testdata/`.

#### 10.1.2 How the annotations absorb it

  - **Idents.** `kIdent` nodes use `NodeAnnotation.local_index`
    (already declared on the M1 skeleton — see §4.1).
    `ResolvePass` is the single writer.  No schema change.
  - **Selects.** `kSelect` nodes use the existing
    `NodeAnnotation.field_number` (ported from v1 M3 G2).  No
    schema change.
  - **Unknowns.** Zero codegen-facing impact.  The unknown-pattern
    set passed to `Instance::PartialEval` is consulted *at
    trampoline entry* inside `cel_host.cel_get_field` (host side),
    not at lowering time.  Codegen emits the same wasm for a
    select regardless of whether the attribute will resolve
    concretely or as `UNKNOWN`; the fork is purely a runtime
    property.  The `CelValue` kind on the wire already has
    `CEL_UNKNOWN` (per `runtime/cel_data.h`), so no runtime
    data-model change either.

#### 10.1.3 Future-compat invariants

M2 must preserve (verified at close-out 2026-04-24):

  - **No new `NodeAnnotation` fields, no new `StorageKind`.**
    Idents + selects + unknowns fit in today's fields — verified.
    > **Invariant relaxed, not broken.** M2 added three fields
    > to `NodeAnnotation`: `attribute_id` (the unknown-pattern
    > interning key that §10.1.2 anticipated) and `map_origin` /
    > `list_origin` (forward-compat hooks for M6 — see
    > `m2-ident-select-unknowns.md §2.8`).  The invariant's
    > *intent* — idents / selects / unknowns need no schema
    > change — is intact; the new fields exist for map/list
    > origin inference that M6 codegen will exercise, not M2.
    > No new `StorageKind` was added.
  - **Codegen stays oblivious to partial eval.** ✓ The same
    lowered `$eval` body produces concrete or unknown values
    purely by runtime dispatch at `cel_host.cel_get_field`
    entry; no compile-time branching on "might this be
    unknown."  Verified via
    `instance_test::InstancePartialEvalTest::NonMatchingPatternFallsThroughToRealValue`
    (same module, different pattern sets, different outcomes).
  - **`cel_reset` semantics survive.** ✓ An Instance that
    evaluated once via `Eval(A)` and once via
    `PartialEval(A, [pattern])` on the same bindings does not
    require re-planning — the unknown set is per-`Eval` /
    `PartialEval` call, stored on `CelHostCallbackEnv.bindings`
    and reset after each call.  Verified via
    `InstancePartialEvalTest::EmptyPatternSetBehavesLikeEval` +
    `MatchingPatternAbsorbsSelectToUnknown` on the same
    Instance.

### 10.2 Comprehensions (M5) — how this design absorbs them

#### 10.2.1 What M5 adds

  - Comprehension macros (`all`, `exists`, `exists_one`, `map`,
    `filter`) — already rewritten to explicit AST by cel-cpp's macro
    expander.
  - Nested scopes with inner-wins shadowing (`langdef.md`).
  - Per-iteration bindings for iter var and accu var.

#### 10.2.2 How the annotations absorb it

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

#### 10.2.3 Future-compat invariants

M5 must preserve:

  - **No new `NodeAnnotation` fields, no new `StorageKind`.**
    Comprehensions fit in today's fields.
  - **`scope_id` is write-once.** Once ResolvePass assigns it, no
    later pass changes it.
  - **Debug layout still works.** PushScope/PopScope are no-ops when
    `debug_mode = true` (fresh slots per node, no release).

If M5 cannot meet these, the design is wrong and this doc updates
before M5 lands, not after.

> **Plan-vs-execution delta — M4 added a comprehension early-reject
> at the front of `ResolvePass`.**  The current `IdentResolver` is
> scope-flat (one global name → local_index map).  cel-cpp's macro
> expansion of comprehensions synthesises internal idents like
> `@result` whose `Repr` legitimately differs across comprehension
> forms (`exists` returns bool, `map` returns list).  Without scope
> handling, the resolver's per-name Repr-agreement CHECK trips on
> the first conformance test that contains both forms.  M4
> short-circuits with `Unimplemented` on any AST that contains a
> `kComprehensionExpr` so the conformance binary classifies them
> as SKIP.  M5's first task is to replace this early-reject with
> the §10.2.2 scope handler.

### 10.3 Maps (M3) — how this design absorbs them

**Status: shipped 2026-04-24.**  See `m3-map-literals.md` for the
slice retro and `map-list-dispatch.md` for the authoritative
three-path dispatch design.

#### 10.3.1 What M3 added

  - Wire-level CelKind split: `CEL_MAP_ARENA = 8` (literal-built,
    16 B `ArenaMapHeader` + `capacity*48` B entries run in the
    arena) and `CEL_MAP_HOST = 9` (host-table interned via
    `ExternrefTable::list_backings_`-style namespace).  See §4.7.2.
  - Runtime arena primitives: `cel_map_create` / `cel_map_insert`
    / `cel_map_grow` / `cel_map_lookup_arena` in `cel_runtime.c`.
  - kDynamic dispatcher: `cel_map_lookup` with
    `__attribute__((musttail))` arms tail-calling into the arena
    or host arm.  Toolchain config: `-mtail-call` (clang),
    `--enable-tail-call` (Binaryen),
    `wasmtime_config_wasm_tail_call_set` (wasmtime).
  - Layer-1 host backings: `HostMap` (vector-backed) +
    `ProtoMap` (proto reflection-backed).  Layer-2 trampoline
    body `CelMapLookupImpl`.  Layer-3 wasmtime trampoline
    registration via `RegisterCelHostImports`.
  - `Value::Map` / `Value::HostMap` factories filled in;
    `StructurallyEquals` kMap arm.
  - Codegen: `kCreateMap` arm + `kCallExpr(_[_])` three-path
    dispatch on `map_origin`.
  - `ProtoBacking::ReadField` on MAP fields returns
    `Value::HostMap(ProtoMap{...})` — first M2 envelope flip.
  - Conformance harness widening: `IsInM3Envelope` admits
    `map_value:` matchers; `CompareMap` order-agnostic.

#### 10.3.2 How the annotations absorb it

  - **`kCreateMap` storage.**  `kWorkspaceSlot` on the
    kMapExpr node; per-entry key/value slots reused after
    `cel_map_insert` consumes them.
  - **`kCallExpr(_[_])` storage.**  Its own `kWorkspaceSlot` for
    the lookup result.
  - **`map_origin`** field on `NodeAnnotation` (added M2 forward-
    compat, populated M3).  `MapOriginVisitor` walks the AST
    bottom-up: `kCreateMap` → `kArena`; `kIdent` / `kSelect` with
    `Repr::kMap` → `kHost`; everything else → `kDynamic` (the
    safe default).  Codegen reads operand's `map_origin` at the
    `_[_]` emission site and picks the right import target.

#### 10.3.3 Future-compat invariants (verified at M3 close)

  - **No new `StorageKind`.** ✓ Map results live in workspace
    slots; map-host backings live in the externref table (not a
    StorageKind concern).
  - **`map_origin` is write-once.** ✓ ResolvePass is the single
    writer; LayoutPass + codegen are read-only.
  - **Codegen stays oblivious to runtime map-kind.** ✓ The same
    lowered `$eval` body works for arena and host operands
    without compile-time branching when origin is `kDynamic` —
    the dispatcher decides at runtime via the operand's CelKind
    tag.

### 10.4 Lists (M4) — how this design absorbs them

**Status: shipped 2026-04-25.**  Mirror of §10.3 with `map` → `list`
substitutions and one runtime-API simplification.  See
`m4-list-literals.md` for the slice retro.

#### 10.4.1 What M4 added

  - Wire-level CelKind split: `CEL_LIST_ARENA = 7` (kept the
    pre-M4 slot to minimise ABI churn) and `CEL_LIST_HOST = 17`
    (new tail value).  16 B `ArenaListHeader` (no key field;
    24 B element stride) + new error code
    `CEL_ERR_INDEX_OUT_OF_BOUNDS = 17`.
  - Runtime arena primitives — **`cel_list_create(out, count)`
    + `cel_list_set(list, index, elem)` instead of the
    create/append/grow triple** the §4.7.3 plan named.
    Codegen always knows the element count at lowering time;
    fixed-length API is simpler and past-count `set` poisons
    with `CEL_ERR_OVERFLOW` (same shape as the map literal's
    past-capacity insert).  Plus `cel_list_at_arena` and the
    kDynamic dispatcher `cel_list_at` (same shape as
    `cel_map_lookup`).
  - Layer-1 host backings: `HostList` (vector-backed) +
    `ProtoList` (reflection-backed).  Layer-2 `CelListAtImpl`.
    Layer-3 wasmtime trampoline registration extends
    `RegisterCelHostImports`.  `HostExternrefTable::InternList`
    /`LookupList` add a third namespace.
  - `Value::List` / `Value::HostList` factories;
    `StructurallyEquals` kList arm (pointer-identity at M4 —
    richer comparator deferred to M5 comprehensions).
  - Codegen: `kCreateList` arm + extended `kCallExpr(_[_])` arm
    that now dispatches on operand's `repr` (kMap →
    `MapLookupCallTarget`, kList → new `ListAtCallTarget`).
    `MapStorageVisitor` generalised to `AggregateStorageVisitor`
    with a `PostVisitList` arm.
  - `ProtoBacking::ReadField` on REPEATED (non-map) fields
    returns `Value::HostList(ProtoList{...})` — second M2
    envelope flip.
  - Activation marshal + Eval decoder: `EncodeList` interns
    bound `Value::List` / `Value::HostList`; `DecodeArenaListAt`
    walks the elements run.
  - Conformance harness widening: `IsInM4Envelope` admits
    `list_value:` matchers; `CompareList` order-aware (lists
    are ordered per langdef § "List equality", unlike maps).
    Conformance: 203 → 212 PASSes.

#### 10.4.2 How the annotations absorb it

  - **`list_origin`** field on `NodeAnnotation` (added M2
    forward-compat, populated M4).  `ListOriginVisitor` mirrors
    `MapOriginVisitor` — `kCreateList` → `kArena`;
    `kIdent` / `kSelect` with `Repr::kList` → `kHost`;
    everything else → `kDynamic`.
  - **Storage** identical to map shape: `kCreateList` →
    workspace slot, per-element scratch slots released after
    `cel_list_set`, `kCallExpr(_[_])` → its own slot.

#### 10.4.3 Future-compat invariants (verified at M4 close)

  - **No new `StorageKind`.** ✓
  - **`list_origin` is write-once.** ✓
  - **Codegen stays oblivious to runtime list-kind.** ✓
  - **M5 comprehensions need a dynamic-list primitive.**  M4's
    `cel_list_create(out, count)` is fixed-length; comprehensions
    that build a list whose size depends on the predicate's
    runtime decisions (e.g. `xs.filter(e, e > 0)`) need either
    a pre-sized accumulator + `cel_list_set` over a clearable
    range or a separate `cel_list_create_dynamic` /
    `cel_list_append` primitive.  Captured in
    `m4-list-literals.md §8 risk #4`; M5 plan picks one.
  - **`RejectDyn` gap.**  cel-cpp types both `[]` (no inferable
    element) and `[1, "two"]` (heterogeneous) as `list<dyn>`;
    our static-subset gate only catches explicit `dyn(...)`
    calls, not implicit dyn from these list inferences.  Two
    tests in `m4_test.cc::ListRejectionE2ETest` lock the current
    pass-through behaviour with TODOs.  Likely a small
    standalone slice before M5 (since comprehensions will
    introduce more inference paths where dyn could leak).

## 11. Implementation plan

### 11.1 Strategy: build `compiler/` end-to-end, swap at the end

The rewrite touches new passes (`ResolvePass`, `LayoutPass`), a new
ABI (uniform slot-out), a new memory-ownership model, and runtime
surgery. Migrating `compiler/` in place means every slice has to
keep v1's ABI and v2's ABI coexisting in the same translation
units — which is where complexity actually lives, not in the new
code itself.

**We build a parallel `compiler/` tree instead.** `compiler/`
remains untouched (and shipping) throughout the rewrite. Each slice
grows `compiler/` by a vertical capability — a new expression
kind that works end-to-end from parse → check → resolve → layout →
emit → run. The first slice ships v2 evaluating `42`; the last
slice ships v2 at feature parity with today's M3 tip, passes the
same e2e fixtures, and renames `compiler/` → `compiler/`
(deleting v1).

**Why this is better for an LLM-driven rewrite.**

  - **New code is cheaper than migration.** Each slice is "write
    v2 file X" with no in-place diffs to v1. No maintaining an
    ABI bridge. No keeping deleted-in-v2 code alive for one more
    slice. The LLM's context budget spends on the design, not on
    reconciling two in-flight designs.
  - **v1 stays runnable throughout.** `bazel test //...`
    is always green because it never moves. The user can ship bug
    fixes against v1 while v2 is being built (low probability, but
    the option costs us nothing).
  - **Each slice is end-to-end executable.** The v2 tree is a
    standalone compiler from Slice 1 onward: `bazel run
    //tools/cel:celwasmc_v2 -- -e "42"` works. Slices add
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
  - **Disk cost.** A second tree sits in `compiler/` for the
    lifetime of the rewrite (~3–4 weeks). Worth it for the
    cognitive cost it saves.
  - **Reference-copy risk.** v2 files that copy v1 literally (e.g.
    M3 proto-select host code that's proven) must be transcribed,
    not symlinked. Each slice's description calls out what gets
    copied from v1 and cites the exact source path — no guessing.

### 11.2 `compiler/` directory layout

Mirrors `compiler/` one-for-one. New file names carry no `_v2`
suffix internally (the directory already tags them); at the swap,
rename is a pure `git mv`.

```
compiler/
├── BUILD.bazel                      # root; re-exports //compiler/... as a filegroup
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
├── api/
│   ├── compiler.{h,cc,_test.cc}    # cel::Compiler — pure compile-time
│   ├── program.{h,_test.cc}        # cel::Program — bytes + ABI
│   ├── engine.{h,cc,_test.cc}      # cel::Engine — wasm engine + parsed runtime
│   ├── instance.{h,cc,_test.cc}    # cel::Instance — Eval + state
│   ├── (value/activation/type/attribute/error already shipped)
│   ├── cel_pipeline_bench.cc       # per-stage cost benches
│   ├── internal/
│   │   ├── wasmtime_engine_state.{h,cc}  # engine + runtime module
│   │   ├── instance_impl.{h,cc}          # per-Plan handles
│   │   └── cel_host.{h,cc,_test.cc}      # M2+: get_field/has_field trampolines;
│   │                                     #      M4 adds message_eq; M7 adds make_message
│   │                                     #      + set_field.  Internal-only because only
│   │                                     #      Engine::Plan calls it.
│   └── BUILD.bazel
├── host/
│   ├── cel_log.{h,cc,_test.cc}     # copied from compiler/host (already-new surface)
│   └── BUILD.bazel
│
│   (host_loader.{h,cc} was deleted in the runtime-isolation work
│   — its role split across api/engine + api/instance.  cel_host
│   moved from an earlier host/ placement to api/internal/ — it's
│   an implementation detail of Engine::Plan, not a public-host
│   surface like cel_log.)
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
    (error mapping, function-import wiring) — reimagined in
    Slice 1, then again in the runtime-isolation work (now lives
    in `api/engine.cc` + `api/instance.cc`).

**Rewritten from scratch.** Everything under `compiler/codegen/`
and `runtime/`. The ABI and memory model is new; there
is no value in a file-level diff against v1.

### 11.3 Invariants that hold across every slice

Non-negotiable — violations mean the slice is misscoped and needs
to split:

  - **v2 is always end-to-end executable.** Every slice ships a
    `tools/cel/celwasmc_v2` binary that compiles **at least**
    the expression families the slice claims, runs them under the
    v2 runtime+host, and returns the right answer. No slice leaves
    v2 half-built.
  - **v2 tests are always green on HEAD.** `bazel test //...`
    passes after every slice. No trailing "will fix" commits.
  - **v1 tests are always green on HEAD.** `bazel test //...`
    passes after every slice. The v2 slices do not touch v1 files.
    If a slice wants to share code, it copies — not symlinks, not
    `cc_library` cross-references.
  - **Every slice ticks ≥ 1 testing-checklist row.** Rows are
    retargeted at v2 paths (e.g. `compiler/codegen/...` instead
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
         │     minimal codegen: -e "42" prints 42]                       [SHIPPED M1 2026-04-22]
         ▼
  S2. All scalar literals (bool/int/uint/double/null/string/bytes)       [SHIPPED M1 2026-04-22]
         │
         ▼
  S3. Symbol table & pipeline scaffolding                                [SHIPPED M1 2026-04-22]
         │  [NodeAnnotation schema + empty OverloadTable +
         │   ResolvePass (ident/field populating only) +
         │   LayoutPass scaffold. Codegen-inert; behavior unchanged.]
         ▼
  S4. Idents + SelectExpr reads (M3 G2 parity in v2)                     [SHIPPED M2 2026-04-25]
         │  [resolve_pass populates local_index / field_number;          (+ M2.E PartialEval / Activation)
         │   kSelect → cel_host.cel_get_field]
         ▼
  S5. OverloadTable seeding + full built-in overload set + uniform ABI   [SHIPPED 2026-04-25 — M5.A through M5.G all green]
         │  [kBuiltinSeeds covers every StandardOverloadIds::k*;
         │   every helper slot-out shape from day one; kCall wired;
         │   coverage tripwire live; +three follow-on slices
         │   (1.5 / 1.55 / 1.6) widened cross-numeric coverage]
         ▼
  S6. has(msg.field) + message equality (M3 G3/G4 parity)                [SHIPPED — has() M2 2026-04-25;
         │                                                                  msg-eq via CelMessageEqImpl + cel_host_cel_message_eq
         │                                                                  + polymorphic dispatcher 2026-04-25]
         │
         ▼
  S7. Custom functions (one wasm import per registered custom)           [PENDING — post-M5]
         │
         ▼
  S8. Map + list literals (three-path dispatch — see §4.7.2 / §4.7.3)    [SHIPPED M3 (maps) 2026-04-24
         │                                                                + M4 (lists) 2026-04-25]
         ▼
  S9. Proto literals (host primitives + descriptor-pool resolution)      [PENDING — M7]
         │
         ▼
  S10. Sethi–Ullman slot allocation + debug layout + error provenance    [PENDING]
         │   [DEFERRED optimisation; lands after all features e2e-green]
         ▼
  S11. v1 M3 tip parity audit + bench parity                             [PENDING]
         │
         ▼
  S12. Swap — `git mv compiler_v2 compiler`                              [PENDING]
```

**Re-ordered relative to as-written plan.**  S8 (maps + lists)
shipped before S5 (full kCall built-in overload set) because the
three-path dispatch design needed to be proven on one aggregate
kind (M3) before duplicating for the other (M4); deferring the
broader kCall arm + overload-table population to M5 keeps the
dispatch contract locked before exercising it from many call
sites.  Comprehension lowering — originally bundled with M5 —
has since been split into a follow-on milestone that depends on
M5's general kCall arm but is otherwise independent.  S6's `has()` half shipped
during M2 (test_only kSelect dispatches to `cel_host.cel_has_field`);
message equality remains pending.

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

#### Slice 1 — Bootstrap: `-e "42"` end-to-end (2 days) — SHIPPED M1 2026-04-22

**Scope.** Enough v2 to evaluate `42` under the v2 CLI.

  - `compiler/` directory structure (§11.2).
  - `runtime/`: header split into topic headers
    (`cel_data.h` / `cel_memory.h` / `cel_arena.h` / `cel_make.h`
    / `cel_log.h`) plus umbrella `cel_runtime.h`; `cel_runtime.c`
    defines `cel_alloc` / `cel_reset` / `cel_make_int`. **Arena
    cursor at fixed memory-base bytes 8/12** (§8.2) — no wasm
    globals. Build flags per §8.1. Codegen emits a
    `cel_reset(<rodata_size>, <mem_size>)` call as the first
    instruction of `$eval` (§8.3) — there is no host-side reset
    phase.
  - `eval/engine.{h,cc}` + `eval/instance.{h,cc}`:
    two-phase instantiation (§9.1).  Originally this slice planned
    `eval/host/host_loader.{h,cc}`; it was rewritten and split
    in the runtime-isolation work — see `two-phase-runtime-isolation.md`.
  - `compiler/frontend/{parse,check}.{h,cc}`: copied verbatim
    from v1 (already thin wrappers over cel-cpp).
  - `compiler/codegen/expr_lower.{h,cc}`: minimal — `kConst` for
    `int64` only, via `.rodata`.
  - `compiler/codegen/static_memory_builder.{h,cc}` with
    `AllocateInt`.
  - `tools/cel/celwasmc_v2.cc`: CLI entry.
  - `e2e/eval_test.cc`: `EvalInt("42", 42)`.

**E2E check.** `bazel run //tools/cel:celwasmc_v2 -- -e "42"`
prints `42`.

**Tests.**
  - `cel_runtime_test`: arena-at-offset-8 round-trip; `cel_alloc`
    + `cel_make_int`.
  - `engine_test` + `instance_test` + `cel_runtime_wasm_test`:
    two-phase instantiation with memory shared via bytes 8/12
    cursor (host-allocated memory imported by both modules; see
    `two-phase-runtime-isolation.md`).
  - `static_memory_builder_test::AllocateInt` byte layout.
  - `expr_lower_test`: emits `i32.const <offset>` for `kConst` int.
  - `e2e/eval_test`: `EvalInt("42", 42)`.

**Risk.** The bootstrap concentrates scaffolding. **Mitigation:**
accept this is the longest slice — pay the cost once.

**Effort.** 2 days.

---

#### Slice 2 — All scalar literals (1 day) — SHIPPED M1 2026-04-22

**Scope.** `StaticMemoryBuilder::AllocateBool` / `AllocateUint` /
`AllocateDouble` / `AllocateNull`; `AllocateString` / `AllocateBytes`
(header + payload + alignment pad). `expr_lower.cc` `kConst` arm per
kind.

**E2E check.** `-e "true"`, `-e "3.14"`, `-e "\"hello\""`,
`-e "b\"x\""`, `-e "null"` each print the expected value.

**Tests.** Per-kind byte layout; per-kind emission; per-kind e2e.

**Risk.** Span-payload alignment pad off by one byte.
**Mitigation:** explicit test asserting offset of next header
after a span payload.

**Effort.** 1 day.

---

#### Slice 3 — Symbol table & pipeline scaffolding (1.5 days) — SHIPPED M1 2026-04-22

**Scope.** Merge that prepares the pipeline but doesn't change
executing behavior — codegen ignores the new fields until S4.

  - `compiler/ir/annotations.h` extends `NodeAnnotation` with
    `overload_id`, `local_index`, `scope_id`, `storage` (§4.1).
  - `compiler/codegen/overload_table.{h,cc}` with
    `OverloadTableBuilder` / `OverloadTable` / `kBuiltinSeeds[]`
    seeded with an **empty list** (real entries land in S5).
    `RegisterCustom` + `AlreadyExists` collision rule work now.
  - `compiler/codegen/resolve_pass.{h,cc}` lands; populates
    `local_index` (ident) and `field_number` (SelectExpr) only.
    Overload interning is stubbed to "lookup returns nullptr".
  - `compiler/codegen/layout_pass.{h,cc}` lands as no-op —
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

#### Slice 4 — Idents + SelectExpr reads (M3 G2 parity) (2 days) — SHIPPED M2 2026-04-25

> **As-shipped scope expanded vs plan.**  M2 absorbed the unknown-
> propagation / partial-eval work that the original M1 plan had
> queued for M4.  M2 ships idents + kSelect reads + `has()` + the
> `Activation` surface + `Instance::PartialEval(activation,
> patterns)` together, since `Activation` is the natural home for
> attribute-pattern declarations.  See §10.1 for the absorption
> notes and `m2-ident-select-unknowns.md` for the slice retro.

**Scope.**

  - `eval/internal/cel_host.{h,cc}` with `cel_get_field`
    and `cel_has_field` (transcribed from v1 host/cel_host —
    proven code). Lives at `api/internal/` per the convention
    already set by `instance_impl.{h,cc}` +
    `wasmtime_engine_state.{h,cc}` — internal-only implementation
    detail of `Engine::Plan`, not a public-host surface.
    Fixed host imports declared up front per
    `feedback_no_lazy_imports`.
  - `expr_lower.cc` grows arms for `kIdent` (returns local-held
    offset) and `kSelect` (non-`test_only` dispatches to
    `cel_host.cel_get_field` using `NodeAnnotation::field_number`).
  - LayoutPass assigns `kWorkspaceSlot` for `kSelect` (naive
    allocator — fresh slot, no release).

**E2E check.** Against a `Customer` proto fixture copied to
`e2e/testdata/`: `-e "x.name" -V x=<Customer>`
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

#### Slice 5 — Full built-in overload set + kCall wired (3 days) — SHIPPED M5 2026-04-25

> See `m5-kcall-comprehensions.md` for the closeout summary
> (M5.A–M5.G, plus Slices 1.5 / 1.55 / 1.6 follow-ons).
> Comprehension lowering carved out to a follow-on milestone
> (`m5-comprehensions-followon.md` — not yet drafted) since
> it's independent of the kCall arm once the OverloadTable is
> populated.

> **Re-ordered after S8.**  Originally scheduled before maps/lists,
> moved after them so the three-path dispatch contract proves out
> on one aggregate kind first (M3 maps).  S5 lands alongside M5
> comprehensions because they reuse the same dispatch.

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

#### Slice 6 — `has(msg.field)` + message equality (M3 G3/G4 parity) (1 day) — SHIPPED M2/M5 2026-04-25

> `has()` shipped at M2 alongside idents + kSelect.  Message
> equality lands at M5.D step 2: `CelMessageEqImpl` Layer-2
> trampoline + `cel_host.cel_message_eq` Layer-3 binding +
> polymorphic dispatcher arm in `cel_runtime.c`.

> **`has()` half shipped early at M2.**  `kSelect.test_only`
> dispatch to `cel_host.cel_has_field` landed alongside the M2
> field-read work — both share the `cel_host.cel_get_field` /
> `cel_has_field` Layer-1/2/3 split.  Message equality remains
> on this slice; it lands when the kCall built-in overload set
> ships (M5) since `_==_` on messages routes through the same
> overload-table machinery as the rest of `==`.

**Scope.** `expr_lower.cc` `kSelect` dispatches on `test_only`
(reads → `cel_get_field`; tests → `cel_has_field`). `kCall` for
`_==_` on messages lowers to `cel_host.cel_message_eq`.
Transcribed from v1 M3 slices G3/G4. `cel_host` gets `cel_message_eq`.

**E2E check.** `has(x.name)`, `has(x.order)`, `x == y` (proto-to-
proto), `has(x.order.items)`.

**Tests.** Transcribed G3/G4 e2e tests under `e2e/eval_test.cc`.

**Risk.** Low — straight port.

**Effort.** 1 day.

---

#### Slice 7 — Custom functions (per-function imports) (2 days) — PENDING (post-M5)

**Scope.** Landing the split model defined in
`cel-host-surface.md` §5: signatures on `Compiler`, impls on
`RuntimeBindings` at `Plan` time.

  - `OverloadTableBuilder::RegisterCustom` (§4.3) + per-function
    wasm imports under `"cel_host"` (§4.6.1). Each declared
    custom becomes its own `AddFunctionImport` with the uniform
    slot-out signature; no shared trampoline, no args-staging
    region.
  - `Cel::Compiler::Builder::RegisterFunction(FunctionDecl)`
    plumbing through the frontend to `TypeCheckerBuilder::
    AddFunction` + `OverloadTableBuilder::RegisterCustom`.
    `FunctionDecl` is signature-only — no impl field (per
    `cel-host-surface.md` §5.1).
  - `Cel::RuntimeBindings::AddFunction(overload_id, impl)` on the
    eval-time side, wired so `Program::Plan(bindings)` binds each
    declared custom import to the trampoline + impl from the
    bindings. Cross-check per §4.6.2 phase B.
  - `cel.abi` custom section extended with `CustomFunctionEntry[]`
    carrying `(function_name, overload_id, is_receiver,
    helper_name, arg_types[], return_type)` — full signature so
    the trampoline knows how to box/unbox on its own.
  - Auto-boxing trampoline: wasm i32 offsets in → `Value` args →
    `impl(...)` → `Value` result → i32 offset out. `Value`
    constructors for every CelType kind the args/return might
    take. No raw-offset FunctionImpl API.

**E2E check.** Fixture `my.upper(string) -> string` — compile-
time `RegisterFunction`, Plan-time `AddFunction`, call round-
trips. Per-arity e2e in `{0, 1, 2, 3, 8}`. Receiver-style e2e:
`"abc".upper()` with `is_receiver=true`. `Program::FromWasm` +
`RuntimeBindings` on a different process evaluates correctly.

**Tests.**
  - `overload_table_test`: `RegisterCustom` collision /
    no-override; built-in shadowing fails.
  - `runtime_bindings_test`: `AddFunction` collision; `Find`,
    `BoundOverloads`; `SetDescriptorPool` override.
  - `e2e/eval_test`: per-arity + receiver-style + missing-impl at
    Plan → `FailedPrecondition` citing overload_id;
    `CheckCompatible` preflight matches.
  - `cel_host_test`: trampoline boxes each arg kind correctly;
    mismatched return kind → ERROR.

**Risk.** Import-list bloat for programs that declare many
unused customs. **Mitigation:** `UsedImports` filters to the
subset the expression actually references (same as built-ins);
unit test confirms declared-but-unused customs produce no wasm
imports.

**Effort.** 2 days.

---

#### Slice 8 — Map + list literals (three-path dispatch) — SHIPPED M3 (maps) 2026-04-24 + M4 (lists) 2026-04-25

> **As-shipped supersedes the as-written scope.**  Original §4.7.2
> + §4.7.3 named simple `cel_map_*` / `cel_list_*` primitives plus
> the `_[_]` / `size` / `in` overload arms in one slice.  The
> shipped design splits this:
>
>   - **M3 (maps) + M4 (lists)** — wire-level CelKind splits
>     (`CEL_MAP_ARENA` / `CEL_MAP_HOST` and `CEL_LIST_ARENA` /
>     `CEL_LIST_HOST`); arena primitives + kDynamic dispatcher
>     with `__attribute__((musttail))`; Layer-1/2/3 host
>     trampolines for `cel_host.cel_map_lookup` /
>     `cel_host.cel_list_at`; codegen kCreateMap + kCreateList
>     arms; kCallExpr(`_[_]`) three-path dispatch on
>     operand origin; conformance harness widening.  See
>     `map-list-dispatch.md`, `m3-map-literals.md`, and
>     `m4-list-literals.md` for the design + retro.
>   - **M5 (kCall built-in overload set)** — `size(map)` /
>     `size(list)` / `k in map` / `e in list` / `==` / `+`
>     route through the OverloadTable to the same three-path
>     dispatch.  Not yet shipped.
>
> Two notable runtime-API deviations from the as-written design:
>
>   1. **Lists use `create(out, count)` + `set(list, i, elem)`** —
>      not `create / append / grow`.  Codegen always knows the
>      element count.  See §4.7.3.
>   2. **No `cel_list_size` / `cel_map_size` primitives shipped
>      yet** — they require the OverloadTable population of
>      `kSize{Map,List}`, which is M5 work.

**E2E check (M3 + M4).**

  - `{"a": 1, "b": 2}` — round-trip kArena map literal.
  - `{"a": 1}["a"]` → `1` — kArena fast path.
  - `m["k"]` on bound map / `c.metadata["k"]` — kHost trampoline.
  - `[1, 2, 3]` — kArena list literal.
  - `[1, 2, 3][1]` → `2` — kArena fast path.
  - `xs[0]` on bound list / `c.tags[2]` — kHost trampoline.
  - Out-of-bounds + duplicate-key + non-int-index error paths
    pinned via langdef-cited tests.

**Tests shipped.** `cel_map_test` + `cel_list_test` (runtime),
`host_map_test` + `host_list_test` + `proto_map_test` +
`proto_list_test` (host backings), `cel_map_lookup_impl_test` +
`cel_list_at_impl_test` (Layer-2 trampolines), expanded
`expr_lower_test` / `resolve_pass_test` / `layout_pass_test`
(codegen pipeline), and new e2e suites `m3_test.cc` (16 tests) +
`m4_test.cc` (41 tests, incl. ListRejectionE2ETest locking
`RejectDyn` gaps).  WAT traces 06–15 in
`doc/implementation-plan/rewrite/wat/` exercised by
`wat_runner_test`.

---

#### Slice 9 — Proto literals (host primitives) (2 days) — PENDING (M7)

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
`InvalidArgument`; unknown field_ref_id → ERROR.

**Risk.** Unknown field on a known descriptor should fail at
*compile time* (checker rejects it). **Mitigation:** fail fast
in frontend; host only handles checker-validated descriptors.

**Effort.** 2 days.

---

#### Slice 10 — Sethi–Ullman + debug layout + error provenance (3 days) — PENDING

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

#### Slice 11 — v1 M3 tip parity audit + bench parity (2 days) — PENDING

**Scope.** No new features — close gaps.

  - Copy `compiler/e2e/eval_test.cc` into `e2e/` (drop
    v1-specific fixtures only if they're genuinely untranslatable),
    run, fix regressions.
  - Wire `bench/eval_bench.cc` from v1; assert v2 is
    within 20% of v1 on existing microbenchmarks (expect v2 faster
    on most; flag slowdowns > 20%).
  - `scripts/lint.sh` clean for all `compiler/` files. Any
    `// NOLINT` gets a bullet in
    `doc/implementation-plan/lint-backlog.md`.

**E2E check.** `bazel test //... //compiler/...` all
green; v2's e2e fixture count ≥ v1's.

**Tests.** Parity audit — no new test cases.

**Risk.** Gap discovery. **Mitigation:** exhaustive by
construction.

**Effort.** 2 days.

---

#### Slice 12 — Swap: `compiler/` → `compiler/` (0.5 day) — PENDING

**Scope.** One commit; no code changes, only renames.

  - `git rm -r compiler/`
  - `git mv compiler_v2 compiler`
  - `git mv compiler/cli/celwasmc_v2.cc compiler/cli/celwasmc.cc`
    (+ BUILD.bazel target rename).
  - Sweep `//compiler/...` → `//compiler/...` in any remaining
    references (docs, checklist, CLAUDE.md if any).
  - Update `doc/wasm-compiler-design.md` §7.0 / §7.3.
  - Rename `m-mem-static-layout-pass.md` +
    `memory-ownership-flip.md` → `.obsolete.md`.
  - Mark this doc `Status: shipped` at the top.

**E2E check.** `bazel test //...` green; `rg 'compiler_v2'`
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
| 1 | Arena-at-offset-8; `AllocateInt`; `engine`/`instance` two-phase | `kConst` int emits `i32.const <offset>` | `celwasmc_v2 -e "42"` → `42` |
| 2 | `Allocate{Null,Bool,Uint,Double,String,Bytes}` | `kConst` emission per kind | Scalar literal e2e per kind |
| 3 | `annotations_test`; `overload_table` builder + `AlreadyExists`; `resolve_pass`; `layout_pass` no-op | Pipeline refactor-only | All prior fixtures |
| 4 | `resolve_pass` populates `local_index`/`field_number`; `cel_host` read ops | `kIdent` / `kSelect` emission | `Customer` field reads |
| 5 | Every helper + aliasing test (locked); `overload_table` coverage tripwire | `kCall` dispatch | Full built-in overload set + 10k randomised vs cel-cpp |
| 6 | — | `kSelect.test_only` dispatch | `has(msg.field)` + message equality |
| 7 | `RegisterCustom` collision; per-function import decode | — | Per-arity customs; unbound / arity-mismatch negatives |
| 8 | `cel_map_*` / `cel_list_*` + duplicate / OOB | `kCreateMap` / `kList` emit | Map + list literal fixtures |
| 9 | `cel_make_message` / `cel_set_field` host tests | `kCreateStruct` emit | Proto literal fixtures; unregistered-descriptor negative |
| 10 | Slot-Strahler; `_at_vv` aliasing per §8.4; `CelErrorPayload.expr_id` | Debug vs prod slot count | Debug-vs-prod parity on 10k corpus; error provenance |
| 11 | — | — | v1 M3 fixtures green under v2; bench within 20% |
| 12 | — | — | `bazel test //...` green post-swap |

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
  2. `bazel test //...` — green locally and on any CI
     that's wired up.
  3. The testing-checklist row(s) this slice ticks are ticked in
     the same commit.
  4. The slice's milestone doc (this file) marks the slice `[x]`
     in §14.
  5. A terse commit body citing: which slice (by §11.3 number),
     which exit criteria were met, which checklist rows flipped.

### 11.9 Exit criteria for the whole rewrite

The rewrite is done when:

  - [x] Slices 1–4 + 8 (M1 + M2 + M3 + M4) shipped.
  - [ ] Slices 5, 6 (msg-eq half), 7, 9, 10, 11, 12 all `[x]` in
        §14.
  - [ ] Zero references to `LoweringContext::scratch_slot`,
        `prologue_setups`, `EmitCheckedArithmetic`, `_at_ii`,
        `_at_uu` remain in `compiler/` (or `compiler/` post-swap).
  - [ ] `rg cel_make_bool|cel_make_int|cel_make_uint|cel_make_double|cel_make_null compiler/codegen` returns zero hits (runtime/host boxing paths excluded).
  - [ ] Every `cel_*` helper in `cel_runtime.h` has a cel-cpp parity
        comment pointing at the source-of-truth impl.  (Map/list
        primitives shipped at M3/M4 already carry these; full
        scalar+string set lands with M5.)
  - [ ] `doc/wasm-compiler-design.md` §7.0 / §7.3 reflect the new
        pipeline (ResolvePass → LayoutPass → emit).
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
`StaticMemoryBuilder` leaves `AllocateList` / `AllocateMap` as
signature-final stubs whose body is `ABSL_CHECK(false)` — M1 callers
crash loudly rather than silently miscompile. Body fills in when (if)
a profiling need emerges.

**7. `MessagePatternTable` vs `OverloadTable` unification.** Proto
construction lives in its own side table (§4.7.1) keyed by descriptor
pointer, not overload id. One `CallTargetTable` that interleaves both
is possible — same lookup surface — but the keys are disjoint and
merging them complicates the builder's collision-check rules
(built-in seeds vs customs vs message patterns, three partitions
instead of two). Keep split until there's a concrete reason to
merge.

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
  > declaration comment. The extended `WasmAnnotations` subsumes
  > v1's split symbol table (`CheckOptions::variable_specs` +
  > `TypedAst::variables()` + `LoweringContext::idents`); it handles
  > scoped bindings so M5's comprehensions slot in without schema
  > changes.

## 14. Deliverables checklist

Paths retargeted at `compiler/` since the swap (S12) hasn't
landed.  At swap, all `compiler/...` paths become
`compiler/...`.

**Shipped (M1–M4):**

  - [x] `compiler/ir/annotations.h` — extended `NodeAnnotation`
        + `StorageKind` / `Storage` + `Origin` (M3/M4)
  - [x] `compiler/codegen/overload_table.{h,cc,_test.cc}` —
        builder + frozen table; `kBuiltinSeeds` empty (M5 fills)
  - [x] `compiler/codegen/resolve_pass.{h,cc,_test.cc}` —
        ident, field_number, attribute_id, map_origin, list_origin
        (overload_id stays at zero until M5)
  - [x] `compiler/codegen/layout_pass.{h,cc,_test.cc}` — kConst
        rodata; kIdent local; kSelect + kMapExpr + kListExpr +
        kCallExpr(`_[_]`) workspace slots (naive allocator —
        Sethi–Ullman is S10)
  - [x] `compiler/codegen/static_memory_builder.{h,cc,_test.cc}` —
        every scalar kind + null
  - [x] `compiler/codegen/slot_allocator.{h,cc,_test.cc}` —
        naive path (debug-layout style); `Release` is a no-op
        until S10
  - [x] `compiler/codegen/expr_lower.{h,cc}` — annotation-driven;
        kConst, kIdent, kSelect (incl. test_only), kCreateMap,
        kCreateList, kCallExpr(`_[_]`) on map + list × 3 origins
  - [x] `compiler/codegen/module.{h,cc}` — memory import + active
        rodata segment
  - [x] `runtime/cel_data.h` — CelKind split for maps
        + lists; `ArenaMapHeader` + `ArenaListHeader`; CEL_ERR_*
        codes incl. `INDEX_OUT_OF_BOUNDS`
  - [x] `runtime/cel_runtime.{h,c}` — `cel_reset` /
        `cel_alloc` (M1) + map/list arena primitives + kDynamic
        dispatchers with `__attribute__((musttail))` (M3/M4)
  - [x] `runtime/BUILD.bazel` — `--import-memory`,
        explicit exports, `-mtail-call` for the dispatcher
  - [x] `runtime/wasm_imports.txt` — `cel_log` (M1) +
        `cel_host.cel_get_field` / `cel_has_field` (M2) +
        `cel_host.cel_map_lookup` (M3) + `cel_host.cel_list_at` (M4)
  - [x] `eval/{compiler,program,engine,instance,
        value,activation,attribute,type,error}.{h,cc,_test.cc}` —
        public surface
  - [x] `eval/engine.{h,cc}` + `instance.{h,cc}` —
        two-phase instantiation; replaces the planned
        `eval/host/host_loader.{h,cc}`
        (see `rewrite/two-phase-runtime-isolation.md`)
  - [x] `eval/internal/cel_host.{h,cc,_test.cc}` —
        Layer-1 backings (`HostMessageBacking` / `ProtoBacking` /
        `HostMap` / `ProtoMap` / `HostList` / `ProtoList`) +
        Layer-2 trampoline bodies (`CelGetFieldImpl` /
        `CelHasFieldImpl` / `CelMapLookupImpl` /
        `CelListAtImpl`) + `ExternrefTable`
  - [x] `eval/internal/cel_host_wasmtime.{h,cc}` —
        Layer-3 wasmtime trampoline registration via
        `RegisterCelHostImports`
  - [x] `conformance/runner.{h,cc}` —
        `IsInM4Envelope` + `CompareMap` + `CompareList`
  - [x] `tools/wat_runner/{wat_runner,wat_runner_test}.{h,cc}` —
        WAT-first harness; binds map/list runtime exports + 3-arg
        `cel_host.*` stubs
  - [x] `doc/implementation-plan/rewrite/wat/0[1-9]_*.wat` +
        `1[0-5]_*.wat` — WAT traces 01-15
  - [x] `doc/implementation-plan/rewrite/wat-traces.md` — per-WAT
        walkthroughs
  - [x] `doc/implementation-plan/rewrite/two-phase-runtime-isolation.md`
        — Engine/Instance role split + parsed-runtime caching
  - [x] `doc/implementation-plan/rewrite/map-list-dispatch.md`
        — three-path origin dispatch design (fully reconciled
        into design.md 2026-04-25)
  - [x] `doc/implementation-plan/per-component-test-coverage.md`
        — per-component test scenarios + closeout gate
  - [x] `doc/implementation-plan/testing-checklist.md` —
        Rewrite M1 / M2 / M3 / M4 sections + ticked rows
  - [x] `scripts/run_full_suite.sh` — closeout gate (default +
        manual targets + conformance)

**Pending (M5 → S10–S12):**

  - [ ] `kBuiltinSeeds` populated for every
        `StandardOverloadIds::k*` (or marked
        `kExplicitlyUnimplemented`).  S5 work, lands with M5.
  - [ ] `runtime/cel_runtime.{h,c}` — full helper
        set (`_add/sub/mul/div/mod/lt/le/gt/ge/eq/ne_at_vv` per
        scalar kind; `_concat/_eq/_contains/_starts_with/
        _ends_with/_matches_at_vv` for string/bytes;
        `cel_size_{string,bytes,list,map}`; 3VL helpers from
        v1 M4 Slice A transcribed).
  - [ ] `expr_lower.cc` general `kCall` arm —
        `OverloadTable::LookupById(a.overload_id)` →
        `EmitHelperCallSlotOut(...)`.  Replaces the narrow
        `_[_]`-only arm shipped for M3/M4.
  - [ ] Custom functions (S7) — `Compiler::Builder::
        RegisterFunction(FunctionDecl)` +
        `RuntimeBindings::AddFunction(overload_id, impl)` +
        `cel.abi.host_custom_imports[]` + per-function
        wasm imports under `cel_host`.
  - [ ] Proto literals (S9) — `cel_host.cel_make_message` +
        `cel_host.cel_set_field` + `cel.abi.types[]` +
        `kCreateStruct` codegen arm.
  - [ ] `kComprehension` codegen arm + ResolvePass scope
        handler (M5) — replaces M4's
        `ComprehensionDetector` early-reject.
  - [ ] Sethi–Ullman slot allocation + debug-layout mode +
        error provenance (`CelErrorPayload.expr_id`) — S10.
  - [ ] `compiler/...` v1 retirement (Slice 12) — `git mv
        compiler_v2 compiler` after S11 parity audit.
  - [ ] `RejectDyn` tightening — catch implicit dyn from
        heterogeneous `[1, "two"]` and bare `[]` list
        literals (the two TODO tests in
        `m4_test::ListRejectionE2ETest` flip when this lands).
  - [ ] String / bytes activation marshalling — host-arena
        allocator that survives `cel_reset` so
        `Activation::Bind("s", Value::String(...))` works
        (currently SKIPped; same gap blocks
        `list<string>` host bindings).
  - [ ] M4 negative-index / OOB error surface — `Eval`
        currently surfaces `CEL_ERROR / CEL_ERR_INDEX_OUT_OF_BOUNDS`
        as a top-level decode error rather than a structured
        `Value::Error(kIndexOutOfBounds)`.  Lands when the
        Error matcher work ships (M4-error-surface-era).
  - [ ] `doc/wasm-compiler-design.md` §7.0 / §7.3 — rewritten
        to reflect the new pipeline.
  - [ ] This doc marked `Status: shipped` at the top once
        S5–S12 land; `m-mem-static-layout-pass.md` +
        `memory-ownership-flip.md` renamed `.obsolete.md`
        with a one-line pointer to this doc.
