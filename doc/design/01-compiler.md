# Compiler design — the pass pipeline

Status: current — authored 2026-06-10 from the design-rebuild notes
plus the post-merge code (slot-allocator free list, static-region
gates). Supersedes: compiler sections of
doc/implementation-plan/rewrite/design.md; memory-layout-design.md
(jointly with 03-abi-and-memory.md); map-list-dispatch.md;
m5-kcall-comprehensions.md and follow-on;
cross-numeric-ordering-plan.md; slice2-control-flow-plan.md;
dyn-passthrough-plan.md.

This doc describes the compiler as a chain of pass contracts: what
each pass consumes, what it produces, what invariant it establishes
for the next pass, and what breaks if it is reordered. Every
mechanism (the static-subset gate, the annotation side-table, the
slot allocator, the static memory builder, the overload table) is
explained at its spot in that chain. System-level context — the
Compiler/Program/Engine/Instance role split, link modes as an
architectural fork, threading — is `00-architecture.md`; byte-level
wire facts are `03-abi-and-memory.md`.

## 1. Pipeline overview

![The compile→eval pipeline](diagrams/pipeline.svg)

`Compiler::Compile(source, CompilerOptions)` (`compiler/compiler.h`)
maps the public options onto the internal `CompileOptions` and calls
the facade `celwasm::Compile` (`compiler/internal/compile.cc`), which
dispatches on `link_mode` and runs:

```
source ──ParseAndCheck──> TypedAst ──ResolvePass──> ResolveOutput
       ──LayoutPass──> StaticLayout ──[static-region gate]──>
       ──module bootstrap (per link mode)──>
       ──LowerExportAndFinalise──> CompiledArtifact (wasm bytes)
```

| Pass | Consumes | Produces | Invariant established | Breaks if reordered |
|---|---|---|---|---|
| Parse + check (cel-cpp wrap) | CEL source, `CheckOptions` | checked `cel::Ast` (`type_map`, `reference_map`) | every reachable node has a checker type; calls carry resolved overload ids | everything downstream reads `type_map`; nothing runs before it |
| Constant/type-ident rewrites | checked AST | same AST, idents for enum constants / type literals replaced by `kConstant` | downstream passes never see a kIdent for a resolved constant | type-ident rewrite keys off "Reference has no value", so it must follow the constant rewrite (`parse_and_check.cc:1416-1418`) |
| RejectDyn | rewritten AST | pass/fail | every surviving node is inside the static subset (five carve-outs, §2.3) | run before `PopulateAnnotations` so no dyn-typed repr survives except via carve-outs |
| PopulateAnnotations | AST + descriptor pool | `WasmAnnotations` seeded with `repr` + `field_number` → `TypedAst` | every node id has a `repr`; selects on messages carry field numbers | ResolvePass CHECKs `repr != kUnknown` on idents |
| ResolvePass | `TypedAst` | `ResolveOutput` (annotations + dense variables + intern tables) | `local_index`/`scope_id`/`attribute_id`/`message_type_id`/origins/`overload_id` populated | LayoutPass dereferences `local_index`; lowering dereferences `overload_id` |
| LayoutPass | `TypedAst` + `ResolveOutput` | `StatusOr<StaticLayout>` (rodata bytes + `.storage` per node + slot-exhaustion gate) | every storage-bearing node has a memory location; rodata+workspace fit the reserved window minus guard | lowering CHECKs `storage.kind != kNone` on the paths that need it |
| `ValidateExprStaticRegion` | `StaticLayout` | pass/fail | region end ≤ 8192 in BOTH link modes | the kStatic segment-install CHECK assumes it already ran |
| Module bootstrap | link mode | `WasmModule` (fresh + imports, or adopted runtime + rodata segment) | every call target lowering emits will resolve | `LowerToEvalFunction` requires imports installed first (`expr_lower.h`) |
| `LowerExportAndFinalise` | all of the above | wasm bytes + `cel.abi` | one shared tail for both modes; validate→optimize→serialize order | optimize before validate would let the optimizer mutate an unproven module |

Neither ResolvePass nor LayoutPass mutates the AST; both write only
into the side-table `WasmAnnotations` keyed by expr id
(notes/codegen-memory §1.1). The AST is mutated exactly twice, by the
two frontend rewrites.

## 2. Frontend: parse, check, and the static-subset gate

Component: `compiler/frontend/parse_and_check.{h,cc}` +
`compiler/frontend/status_tags.h`. Entry point:
`ParseAndCheck(expression, CheckOptions) ->
absl::StatusOr<TypedAst>`.

### 2.1 Parse + check (cel-cpp wrap)

The checker builder is configured with, unconditionally: the standard
library, the ComprehensionsV2 checker library, the strings / encoders
/ math extensions, `OptionalCheckerLibrary`, and hand-built network
extension decls (`net.IP` / `net.CIDR` opaque types — cel-cpp ships no
network library; `RegisterNetworkExtDecls`). Then the per-call
surface: `container`, variable specs, and custom-fn decls from
`CheckOptions::function_libraries` (one cel-cpp `FunctionDecl` per fn
name; cross-library overload-id collision filtering is the caller's —
`Compiler::Builder::Build` — contract). One `ParserOptions` value
(`DefaultParserOptions`: `enable_optional_syntax = true`,
`max_recursion_depth = 16384`) is shared between macro registration
and the parse call; divergence produces "macro not found" failures
(`parse_and_check.cc:1087-1111`).

**Schema overlay, not replacement.** `LoadDescriptorPool` resolves
message types against the process-wide `generated_pool()` by default;
a user schema (`.proto` source via an in-process
`google::protobuf::compiler::Parser`, or a binary `FileDescriptorSet`)
is merged OVER the generated pool via `MergedDescriptorDatabase`, so
well-known types always resolve (`parse_and_check.cc:160-195`).

Check failure returns InvalidArgument carrying the
`kUndeclaredReferencesUrl` payload (deduped root symbols of every
"undeclared reference" issue); the conformance runner classifies on
payloads, never message substrings (`status_tags.h`).

### 2.2 The two AST rewrites

The only AST mutations in the whole compiler, both idempotent, both
before RejectDyn so later passes see `kConstant` nodes uniformly:

1. **`InlineConstantReferences`** — every kIdent whose
   `reference_map` entry carries a value (enum-name constants like
   `TestAllTypes.NestedEnum.BAR`) becomes a kConstant carrying it.
   Re-implements cel-cpp's reference-resolver walk without pulling in
   FlatExprBuilder (`parse_and_check.cc:1266-1286`).
2. **`InlineTypeIdentifierReferences`** — every kIdent whose
   Reference is value-less AND whose `type_map` entry is
   `TypeType(inner)` becomes a kConstant whose `string_value` is the
   spec type name (`"int"`, `"google.protobuf.Int64Value"`,
   `"net.IP"`, …; `SpecTypeName`, cc:1366-1394). MUST run after
   rewrite 1: it keys off "Reference has no value", which depends on
   the constant rewrite having consumed the value-bearing ones.

The rewritten node's `type_map` entry stays `TypeType(inner)`, so
`ReprOf` stamps `Repr::kType` on the new constant — the hook
LayoutPass's rodata packer dispatches on (§5.2).

### 2.3 RejectDyn — the static-subset gate

Precondition: a checked AST (FailedPrecondition otherwise). A node
violates if its `type_map` entry is missing or `UnacceptableLabel`
(`parse_and_check.cc:352-379`) fires: `dyn`, `error`, function,
type-param, and unset specs are rejected, RECURSIVELY through
`list<...>` element types, `map<K,V>` key/value types, and any
`abstract<...>` parameter. So implicit dyn — bare `[]`,
heterogeneous `[1, "two"]`, `optional<dyn>` — rejects, not just
explicit `dyn(...)` (pinned by `e2e/m4_test.cc`
`BareEmptyListLiteralRejected` / `HeterogeneousListRejected`).

Five carve-outs ADMIT otherwise-dyn shapes, each a narrow
shape-matched predicate (`CheckSubsetNode`, cc:611-630):

1. **`dyn(x)` passthrough** — a global 1-arg `dyn` call admits iff
   the arg is itself a 1-arg `dyn` call (recursive collapse) or the
   arg's checker type `has_primitive() || has_null() || has_type()`.
   Aggregate / message / dyn-typed args reject. `dyn` is the
   identity function: there is no CEL_DYN runtime kind; ResolvePass +
   LayoutPass forward the arg's annotations and storage onto the call
   node, and codegen emits the argument (§6.2).
2. **Select-through-Any** — a kSelect typed `dyn` admits if its
   operand types as `google.protobuf.Any`, transitively
   (`msg.single_any.x.y`). The runtime unwrap is
   `ProtoBacking::ReadField`'s job on the eval side.
3. **`math.@min` / `math.@max`** — the `math.greatest`/`least` macro
   expansions admit their checker-assigned dyn result, and the
   macro-built mixed-numeric list-literal arg is skipped.
4. **`<target>.format([list-literal])`** — receiver-style `format`
   whose single arg is a kListExpr admits the args subtree without
   recursion (the runtime renderer dispatches per element kind);
   `list<dyn>`-typed *variables* stay rejected.
5. **`cel.bind` shape** — a comprehension whose iter_range is an
   empty list literal and whose loop_condition is `kConst(false)`
   skips checking the (dyn-typed) iter_range and the (unreachable)
   loop_step; accu_init / loop_condition / result are still checked.
   Shape-matched, not macro-provenance-matched.

Violations are accumulated, not first-fail, and reported as
InvalidArgument tagged `kStaticSubsetViolationUrl` with the offending
expr ids in the payload. The public header's one-line summary of the
gate omits the carve-outs (register row R11); this section is the
authoritative telling.

> **Open question (V17):** four frontend probes are pending — is the
> "no type_map entry" arm reachable on a real checked AST; can a
> non-`cel.bind` macro expansion match `IsCelBindShape`; does
> select-through-Any have a frontend unit test (today it is pinned
> only via e2e); does `"%s".format([msg_var])` produce a clean
> runtime error as the carve-out comment asserts; what repr does a
> wrapper-FQN variable spec (`w:google.protobuf.Int64Value`) get.

## 3. IR & annotations

Component: `compiler/ir/typed_ast.{h,cc}` +
`compiler/ir/annotations.h`. `TypedAst` is deliberately NOT a heavier
IR: a move-only bundle of `unique_ptr<cel::Ast>` + `WasmAnnotations`
+ `vector<Variable>`; downstream passes read `type_map`,
`reference_map`, and the annotations simultaneously.

`WasmAnnotations` is a `flat_hash_map<int64_t, NodeAnnotation>` — ONE
per-node fact table, zero sentinels for "not applicable", no parallel
tables (the invariant held across the project's lifetime even as the
schema grew). The full schema (`annotations.h:80-139`):

| Field | Written by | Meaning |
|---|---|---|
| `repr` | frontend (`PopulateAnnotations`) | wire-representation kind from the checker type (`Repr`, 16 enumerators incl. `kType`, `kOptional`, `kEnum`) |
| `field_number` | frontend (`FieldNumberVisitor`) | kSelect proto field number; 0 = resolve-by-name / not a message field |
| `overload_id` | ResolvePass | cel-cpp resolved overload string; a `string_view` into cel-cpp-owned storage — lifetime tied to the TypedAst |
| `local_index` | ResolvePass | kIdent's dense wasm-local index |
| `scope_id` | ResolvePass | 1-based comprehension depth on scope-bound idents; 0 = free |
| `attribute_id` | ResolvePass | interned attribute path id for partial eval; 0 = none |
| `message_type_id` | ResolvePass | kStructExpr's dense index into `cel.abi.types[]`; 0 = none |
| `storage` | LayoutPass | `{kind, payload}`: kStaticRodata (rodata offset), kLocal (local index), kWorkspaceSlot (slot offset), kNone |
| `map_origin` / `list_origin` | ResolvePass | three-path dispatch origin (kDynamic default / kArena / kHost) |
| `comp_aux_local_base` | LayoutPass | first of 3 per-comprehension auxiliary wasm locals |
| `comp_iter_local_index` / `comp_accu_local_index` / `comp_iter2_local_index` | ResolvePass | per-comp variable bindings by id, never by name (cel-cpp reuses `@result` at every nesting depth) |
| `select_key_rodata_offset` | LayoutPass | rodata offset of a kSelect's field name packed as a CEL_STRING CelValue, for optional-select and map-field-sugar lowering |

The frontend stamps exactly two fields (`repr`, `field_number`);
everything else is codegen-side. `repr` mapping: wrappers reuse the
wrapped primitive's repr (nullness tracked elsewhere); `kAny →
kMessage`; the abstract type named exactly `"optional_type"` →
`kOptional` (deliberately exact-name, so future cel-cpp abstract
types don't silently light up optional codegen); dyn / error /
function / param / unset → `kUnknown` for the downstream audits to
flag. `field_number` is re-resolved by the frontend while the
descriptor pool is live because cel-cpp's `reference_map` cannot
supply it.

`Variable{name, repr}` entries are captured in `variable_specs`
order, with repr stamped at spec-parse time rather than re-derived
from `type_map` — unreferenced variables never appear in `type_map`
but still shape the eval-function signature.

> **Open question (V14/V15):** is an optional-typed free variable
> reachable on the wire at all (expected: frontend-rejected,
> unreachable by construction), and is `Repr::kEnum` producible by
> the compiler or wire-format-only (the `ReprOf(TypeSpec)` overload
> has no enum arm; the `cel::Type` overload's kEnum arm has one
> caller whose parser can't produce enums)?

## 4. ResolvePass

Component: `compiler/codegen/resolve_pass.{h,cc}`. Runs 8 visitors in
a fixed order (`RunAnnotationVisitors`); only the last is
order-sensitive.

1. **KConstReprAudit** — `ABSL_CHECK`s every kConst has a
   non-kUnknown repr (fail-loud against garbage rodata).
2. **ScopedIdentResolver** — interns idents into the dense
   `variables` table (`local_index` 0,1,2,… first-seen). Maintains a
   comprehension scope stack: `iter_range`/`accu_init` resolve in the
   OUTER scope; `loop_condition` + `loop_step` in an inner frame with
   iter (+iter2) + accu bound; `result` in an accu-only frame. The
   iter-var lifecycle table (`IterKindsFor`): single-iter list source
   → iter is `kComprehensionIter` (a moving pointer, no workspace
   slot); map source → all iter vars are `kComprehensionAccu` (slot,
   written by `cel_map_iter_*_at`); two-iter list → the index var is
   accu-lifecycle, the value var is iter-lifecycle. Per-comp binding
   indices are stamped on the comp node's annotation so codegen
   resolves bindings by expr id, never by name. Leading-dot idents
   (`.y`, the namespace-disambiguation form) skip the scope lookup
   entirely and intern under the dot-stripped canonical name,
   mirroring cel-cpp's `LookupLocalIdentifier`
   (`resolve_pass.cc::PostVisitIdent`; pinned by the
   `namespace_shadowing` conformance rows).
3. **AttributePathResolver** — interns `(root_variable, qualifiers)`
   paths from kIdent + kSelect post-order; entry 0 is the reserved
   sentinel. Scope-bound idents get `attribute_id = 0` (not attribute
   roots). The host trampoline appends a select's own field from the
   OPERAND's id at `cel_get_field` time.
4. **MessageTypeIdVisitor** — interns kStructExpr FQNs into
   `message_types` (sentinel at 0); CHECKs the name is non-empty.
5. /6. **MapOriginVisitor / ListOriginVisitor** — the implemented
   origin-inference table is exactly three rows per aggregate kind:
   literal kMapExpr/kListExpr → `kArena`; map/list-typed kIdent and
   kSelect with `scope_id == 0` → `kHost`; everything else stays the
   `kDynamic` default. The historical design table's wider rows
   (kCall→kHost, comprehension-fold→kArena, same-origin coalescing)
   were never implemented — kDynamic is the correct-but-slower
   default, so a missing row degrades to a runtime kind-branch, never
   a miscompile (register row R18; the unshipped rows are §10 future
   work). Comp-scope idents deliberately stay kDynamic (the value may
   be arena-resident).
6. (cont.)
7. **OverloadIdResolver** — copies the first overload id from
   cel-cpp's `reference_map` onto each kCall. Empty is legitimate for
   the special-cased operators (`_[_]`, `_&&_`, `_||_`, `_?_:_`).
8. **DynPassthroughVisitor** — for global 1-arg `dyn(x)`, copies the
   arg's non-storage annotation fields onto the call node; must run
   last so the copied fields are final. Storage forwarding happens in
   LayoutPass (slots don't exist yet).

Output (`ResolveOutput`): the annotations; the dense `variables`
table (free variables + comp-scope bindings in first-seen order —
`variables.size()` is the count of i32 wasm locals `$eval` declares);
the `attributes` and `message_types` intern tables; `max_scope_id`.
Unreferenced declared variables get no entry, no slot, no ABI row.

> **Open question (V7):** `cel_abi.proto` claims `variables[]` is
> positional by `local_index`, but the emitter skips comp-scope
> entries while ResolvePass interleaves them in the same dense index
> space — a free variable first referenced inside a comprehension
> should break the claim. Consumers iterate by name, so no runtime
> bug; the wire-comment fix (or re-densification) is pending.

## 5. LayoutPass and the memory map

Component: `compiler/codegen/layout_pass.{h,cc}`,
`compiler/codegen/static_memory_builder.{h,cc}`,
`compiler/codegen/slot_allocator.{h,cc}`, and the constants header
`compiler/memory_layout.h`. Signature: `LayoutPass(TypedAst,
ResolveOutput, LayoutOptions) -> absl::StatusOr<StaticLayout>` — the
StatusOr is load-bearing: layout can now FAIL (§5.4).

![Linear memory](diagrams/memory-map.svg)

### 5.1 The memory map and its single source of truth

`compiler/memory_layout.h` is the compiler-side single source of
truth for every layout constant, cross-checked against the runtime's
`runtime/cel_layout.h` macros by `static_assert`s in the header
itself — a drifted constant fails the build, not the eval.

```
[0, 8)        reserved null sentinel (offset 0 == "absent",
              runtime-wide)
[8, 16)       reserved legacy arena-cursor slot (dead; kept so
              rodata_base stays 16 and the ABI doesn't churn)
[16, …)       rodata — kConst CelValue frames + payload bytes
              (rodata_base = MemoryLayout::kRodataBaseMin = 16,
              overridable)
ws_base       RoundUp16(rodata_base + rodata.size()); 32-byte
              cells: referenced-variable slots, then SlotAllocator
              scratch
[…, 7936)     free headroom up to the guard band
[7936, 8192)  kGuardBytes = 256 guard band — workspace must not
              enter it (gate, §5.4)
[8192, heap)  wasi-libc statics + stack (runtime-owned;
              --global-base = kReservedLowMemoryBytes = 8192)
[heap, …)     dlmalloc heap: the per-Instance bump arena
              (64 KiB cap), activation buffer, plan-lifetime
              objects
```

The first 8 KiB is the ONLY region the expr module may write: the
runtime build pins wasi-libc's static data, heap, and shadow stack
above `--global-base=8192`, so a byte written at or past that line
corrupts libc state with no wasm trap — the process limps along and
traps minutes later inside an unrelated helper
(`memory_layout.h:10-20`). Everything in §5.4 exists to make that
impossible to reach.

`arena_base` is still computed (`RoundUp8` past workspace) and
tested, but legacy: codegen no longer consults it — the arena lives
in the wasi-libc dlmalloc heap and the emitted `arena_reset` call is
zero-arg (`expr_lower.cc::EmitArenaResetCall`). The canonical
region/page tables live in `03-abi-and-memory.md`.

### 5.2 The five sub-passes

`LayoutPass` moves the `ResolveOutput` into the returned
`StaticLayout` and runs (`layout_pass.cc:468-557`):

- **Pass A — rodata.** `ConstLayoutVisitor` packs every kConst into
  one `StaticMemoryBuilder` and stamps `{kStaticRodata,
  abs_offset}`. Dispatch is on the Constant oneof EXCEPT
  `Repr::kType` constants (the §2.2 rewrite targets), which pack as
  CEL_TYPE via `AllocateType`; an unrecognised variant is
  `ABSL_CHECK(false)`. Then `SelectKeyRodataVisitor` lifts the field
  name of every kSelect whose operand is `Repr::kOptional` **or
  `Repr::kMap`** into rodata as a CEL_STRING CelValue and stamps
  `select_key_rodata_offset` — the optional-select kernel reads its
  key from a CelValue slot, and kSelect on a map operand is CEL
  sugar for `m[field]`, lowered through the map-lookup family with
  the rodata string as key (§6.3). Both visitors share one builder
  so rodata is contiguous.

  **StaticMemoryBuilder** (its spot in the spine: it exists only for
  Pass A): packs 24-byte CelValue frames — u32 kind at +0, pad at
  +4, 16-byte payload at +8 — returning ABSOLUTE linear-memory
  offsets (`base_offset` baked into both the frame offset and any
  CelSpan payload pointer), so emitted wasm needs no relocation
  arithmetic. string/bytes/type payloads follow the frame, cursor
  padded back to 8. Infallible by design — no cap, no StatusOr; the
  budget gate lives downstream (§5.4). No deduplication: equal
  literals get distinct frames. `AllocateList`/`AllocateMap` are
  `ABSL_CHECK(false)` stubs — aggregates are built at eval time in
  the arena, never in rodata.
- **Pass B — variable slots** (`ReserveVariableSlots`):
  `workspace_base = RoundUp16(rodata_base + rodata.size())`; one
  32-byte cell (`SlotAllocator::kSlotStride`) per referenced
  variable EXCEPT `kComprehensionIter` vars, whose `slot_offset`
  stays 0 (their wasm local holds a moving element pointer, not a
  slot address). Slots pack densely in allocation order.
- **Pass C — kIdent storage** (`IdentStorageVisitor`): stamps
  `{kLocal, local_index}` with a range CHECK. The slot offset itself
  is NOT in the annotation — the `$eval` prelude `local.set`s each
  free-variable local to its `slot_offset`; comp-scope entries are
  skipped there and in the ABI emitter (set by loop prologues).
- **Pass D — scratch slots.** One `SlotAllocator` based at
  `workspace_base + workspace_bytes` is shared by two sequential
  traversals: `SelectStorageVisitor`, then
  `AggregateStorageVisitor`. The release/acquire discipline is
  §5.3. Afterwards `workspace_bytes += slots.total_bytes()` and
  `peak_slots` is recorded.
- **Pass E — comprehension aux locals**
  (`ComprehensionLocalsVisitor`): stamps `comp_aux_local_base` =
  `variables.size() + 3 × (comp pre-order index)`; 3 locals per comp
  (end-pointer / cursor / index or source-addr);
  `total_wasm_locals = variables.size() + 3 × #comprehensions`. Its
  post-visit hook ALSO stamps the kComprehensionExpr's `storage`
  with the storage of its `result` sub-expression — the comp block's
  emitted value is the slot the result actually lives in, and for
  `exists_one` that is a fresh Bool slot distinct from the Int accu
  (mirroring the accu slot here was the cleanup-backlog #32/#33
  bug pinned by `KnownBugs.PbtExistsOneInTernaryCondBytes`).

`LayoutOptions`: `debug_layout` (pins the allocator to bump-only,
§5.3) and `rodata_base_override` (relocates the whole region; a
non-zero value also skips the facade's static-region gate — the
caller owns its own budget). The override's documented consumer,
`compiler/celfn/library_module.cc`, does not exist; the knob has no
production caller today (register row R3; the `@native` library-module
fork is `05-custom-functions.md`'s decision).

### 5.3 SlotAllocator — free-list reuse (as merged)

`compiler/codegen/slot_allocator.{h,cc}`. The allocator hands out
CelValue cells in the workspace region; callers `Acquire` a cell per
computed result and `Release` it once every read is done.

**Cell stride is 32 bytes, not 24.** Every slot must be 16-byte
aligned from a 16-aligned base: the runtime is compiled under
wasm32-wasi-threads, and any helper that touches an `__atomic_*`
builtin emits `memory.atomic.*` ops against the workspace. A 24-byte
stride from an 8-aligned base puts every other slot at `% 16 == 8`,
and the first atomic on such an address traps with `wasm trap:
unaligned atomic` — the symptom pinned by
`e2e/known_bugs_test::LongArith_2000Terms_NoUnalignedAtomicTrap`.
CelValue stays 24 bytes; the trailing 8 bytes of each cell are pad.
The stride is mirrored as `MemoryLayout::kSlotStride` with a
`static_assert` pinning the two together (`slot_allocator.h:207`).

**Two modes** (`SlotAllocator(base_offset, debug_mode)`):

- `debug_mode == false` (production): `Release` pushes the cell onto
  a LIFO free list; `Acquire` pops the most-recently-released cell
  first (cache-hot, keeps the bump pointer low) and bumps a fresh
  cell only when the list is empty. `live_slots_` tracks
  acquired-minus-released; `peak_slots()` is the true high-water mark
  of simultaneously-live cells — the workspace size the module needs
  (`total_bytes() = peak_slots() * kSlotStride`). Releasing more than
  was acquired is an `ABSL_CHECK`.
- `debug_mode == true`: bump-only — every Acquire is a fresh cell,
  Release is a no-op, `peak_slots` equals total acquires. Preserves
  per-expr slot distinctness for layout dumps.

**The release discipline, by node kind** (the load-bearing design;
`slot_allocator.h` preamble + `AggregateStorageVisitor`'s class
comment):

- **kSelect / kCall: release operands, then acquire (PostVisit).**
  Every runtime helper backing these reads its operand slots BEFORE
  writing the result slot (`cel_int_add_at_vv(out, lhs, rhs)`,
  `cel_get_field`, `cel_map_lookup`, …), so parent↔operand aliasing
  is safe; the LIFO list hands the just-vacated operand cell back as
  the parent's result. Consequence: a left-associative `+`-chain or
  a `c.a.b.c.d` select chain peaks at ~1 slot regardless of length
  (`slot_allocator_test::LeftAssocAdditionChainAfterReleaseFix`
  pins peak == 1 at N = 2000; a balanced tree peaks at its depth).
- **kListExpr / kMapExpr / kStructExpr: acquire at PreVisit, pin
  across the subtree, and NEVER release operands into the parent's
  own lifetime.** Aggregate codegen writes the parent FIRST
  (`cel_list_create(parent)`), then lowers each element and appends —
  so the parent's slot must not alias ANY descendant's. Acquiring at
  PreVisit guarantees no descendant Acquire can be handed the
  parent's cell; PostVisit releases each operand cell so SIBLING
  subtrees of the aggregate's parent can reuse them, and the
  aggregate's own slot is released by its consumer. Surfaced
  historically by the m4/m7 cross-aggregate aliasing bugs; pinned by
  the `e2e/slot_aliasing_test.cc` battery (observed-correctness
  table: nested aggregates of every kind pair, chains of every
  associativity, mixed kCall+aggregate, comprehensions over all of
  it).
- `dyn(x)` forwards the arg's storage instead of acquiring — the
  arg's lifetime continues through the call, no release/acquire.
  Ternary branch-arm storage is the node's own PostVisit slot; both
  arms copy into it at runtime (§6.4).

This resolved the former P0: with the old no-op `Release`, workspace
grew 24 bytes per acquire, so ~340 slot-acquiring nodes pushed
workspace writes past 8192 into runtime statics — the actual root
cause of both the long-arith "unaligned atomic" trap and the
10K-literal-list wasmtime panic (cleanup-backlog #16 had blamed the
runtime layer). With reuse, a 2000-term arithmetic chain compiles
AND evaluates correctly; the remaining ceilings are rodata-bound
(§5.4).

The two Pass-D traversals (selects, then aggregates) run sequentially
over one allocator, so cross-walk interactions exist by construction
(a kSelect whose operand is a kCall sees `kNone` during the select
walk and releases nothing). The merged discipline above is built so
no cross-walk aliasing is unsafe, and `e2e/slot_aliasing_test.cc` is
the standing empirical pin (this answers validation item V46's
premise; the residual question below remains).

> **Open question (V46, residual):** the two-traversal structure
> over-holds some cells (a select's operand-call slot is released
> only in the second walk). Whether collapsing Pass D into one
> post-order walk would tighten `peak_slots` further — without
> re-opening the aggregate aliasing class — is unmeasured.

### 5.4 The static-region gates (new)

Three gates, one per pipeline stage, all derived from
`MemoryLayout`:

1. **LayoutPass slot-exhaustion gate** (`layout_pass.cc:529-554`).
   After Pass E: `workspace_bytes` must not exceed
   `MemoryLayout::MaxWorkspaceBytes(rodata_base, rodata_size)` =
   `kReservedLowMemoryBytes − rodata_end − kGuardBytes` (clamped at
   0). The cap is dynamic in rodata: an expression with no constants
   gets ~7.9 KiB of workspace headroom; 3 KiB of string constants
   leaves ~4.9 KiB. Overflow returns ResourceExhausted prefixed
   `kSlotExhaustedMessagePrefix` with a remediation hint (split the
   expression; move literals into bound variables). The
   `kGuardBytes = 256` band (≥ one slot stride by `static_assert`)
   exists to trip the gate on the NEXT slot-allocator off-by-one
   before it spills into libc.
2. **`ValidateExprStaticRegion`** (`compile.cc:57-75`, called from
   `RunFrontAndLayout` in BOTH link modes, skipped only when
   `rodata_base_override != 0`): rejects any layout whose region end
   (`workspace_base + workspace_bytes`) exceeds
   `CELWASM_RESERVED_LOW_MEMORY_BYTES`, with ResourceExhausted — a
   status, never a CHECK, because region size is embedder input.
   This covers the rodata-dominated case the workspace gate's
   clamped-to-zero arm also reaches, and it covers kDynamic: the
   dynamic expr module's `cel.memory` import resolves to the
   runtime's exported memory, so an oversized rodata segment would
   overwrite runtime state at instantiate time exactly as in
   kStatic. (Pinned both modes:
   `compile_test::RodataOverBudgetReturnsResourceExhausted{,InDynamicMode}`,
   `WorkspaceOverBudgetReturnsResourceExhaustedBothModes`;
   end-to-end by
   `KnownBugs.LiteralIntListInScanRejectedAtCompileAt10K` and the
   N=327/328 boundary pair.)
3. **kStatic segment-install tripwire**
   (`InstallExprRodataSegment`): an `ABSL_CHECK_LE(rodata_end,
   8192)` — by this point the gate has run, so reaching the CHECK
   means the gate regressed.

The eval side adds a fourth, Plan-time gate outside this doc's scope
(`eval/engine.cc::ValidateAbiSlotExtents`): a Program whose `cel.abi`
declares a variable slot extending past the window is rejected as
corrupt/hand-crafted before the marshal can write through it.

The boundary numbers in the tests (literal `x in [0..N-1]` fits at
N = 327, overflows at 328; nested-list depth 246/247) are recorded
as re-probed 2026-06-10 in the test comments; they shift if
`kGuardBytes`, the stride, or rodata framing changes — the tests are
the pin.

## 6. Lowering

Component: `compiler/codegen/expr_lower.{h,cc}`,
`expr_lower_internal.h`, `expr_lower_comprehension.cc`,
`overload_table.{h,cc}`, `module.{h,cc}`. Per the repo's WAT-first
rule, every arm was designed as an executable `.wat` under
`doc/implementation-plan/rewrite/wat/` first; this section cites
those files and never inlines listings (the on-disk corpus is
maintained and runnable; inline copies rot).

`LowerToEvalFunction(ast, layout, name, module, overload_table,
opts)` adds a nullary `() -> i32` `$eval` to a caller-prepared module
— the caller must have installed memory + every function import
first. The returned i32 is the root expression's CelValue offset.
The body shape: `(block i32: <local.set per free variable> (call
$arena_reset) <root>)`; comp-scope variables are skipped in the
prelude (set by loop prologues). `LoweringOptions::mem_size_bytes`
is vestigial and the parameter is entirely unread
(`LowerToEvalFunction(..., const LoweringOptions& /*opts*/)`).

`Emit(EmitCtx&, Expr&)` is the single dispatcher
(`expr_lower.cc:1271`); every arm returns an i32-valued Binaryen
expression whose runtime value is the node's CelValue offset (a
rodata `i32.const` for kConst, a `local.get` for kIdent, a
`(block (call …) (i32.const out_slot))` for everything else).

**Storage-aware addressing.** `EmitSlotBaseAddress(ctx, Storage)`
is the one place a `Storage` becomes an address expression: kLocal →
`local.get` (the local holds the cell's byte offset), kStaticRodata
/ kWorkspaceSlot → literal `i32.const`, kNone → CHECK. Every slot
probe and `cel_copy_slot` routes through it; reading
`storage.payload` directly was the bug class where a kIdent's local
INDEX was treated as a byte offset
(`KnownBugs.PbtTernaryInsideIntSubtract`, fixed 2026-06-05).

### 6.1 The kCall dispatch ladder

In order (`Emit`'s kCallExpr arm):

1. **`dyn(x)`** — identity: emit the argument
   (`dyn-passthrough-plan.md` heritage; no `cel_to_dyn` helper
   exists).
2. **`_[_]`** — `EmitKIndexCall`: origin-aware three-path dispatch
   (§6.3). `Repr::kOptional` operands route to
   `cel_select_optional_field_at_vv`'s index cousin
   (`wat/m14_optional_select_field.wat` for the key-slot ABI).
3. **`_?_:_`** — `EmitConditional`: nested `BinaryenIf` (§6.4;
   `wat/33_conditional.wat`). The ONLY operator where laziness is
   load-bearing.
4. **Everything else** — `EmitGeneralCall`: look up
   `ann.overload_id` in the OverloadTable (Unimplemented naming the
   id if unstamped or unregistered — fail-loud), flatten a receiver
   target to args[0], emit one uniform slot-out call
   `(out_slot, arg_slot…) -> void`. `_&&_` / `_||_` / `!_` take THIS
   arm — eager evaluation of both operands, with non-strict 3VL
   absorption living entirely inside `cel_and` / `cel_or` /
   `cel_not` (`wat/30_logical_and.wat`..`32_logical_not.wat`).
   Spec-equivalent because CEL is side-effect-free; the historical
   plan's claim that explicit branching was "the only correct
   lowering" was wrong as-shipped, and the eager shape is also a
   perf fact (both operands always evaluate) that the bench docs
   state.

Per-arm WAT references: kConst `wat/01_literal_42.wat`; kIdent
`wat/02_ident_x.wat`, `03_two_idents.wat`; kSelect
`wat/04_select_c_name.wat` (proto path) and `10_proto_map_field.wat`;
aggregates `wat/06_map_literal.wat`, `11_list_literal.wat`; indexing
`07`–`09` and `12`–`14` (`*_arena` / `*_host` / `*_dynamic` triples);
arithmetic/compare/concat `16`–`18`; `size`/`in` `21`–`22`; proto
literals `40`–`41` (`wat/m20_set_field_poison.wat` for the poison
contract); time `50`–`55`; wrapper unwrap `56`; optionals `m14_*`;
extensions `m16_*` / `m17_*` / `m18_*`.

### 6.2 kSelect — three branches

`EmitKSelect` dispatches on the OPERAND's repr:

- **`Repr::kOptional`** → `cel_select_optional_field_at_vv`, key
  from `select_key_rodata_offset` (§5.2).
- **`Repr::kMap`** → `EmitKSelectMapBranch`: map field-selection
  sugar (`m.field` ≡ `m['field']`, langdef §"Field selection") lowers
  to `cel_map_lookup` (value form) or `cel_map_in` (the `has()` /
  test_only form, note its `(out, key, map)` arg order) with the
  rodata key. It uses the kDynamic DISPATCHER unconditionally rather
  than `MapLookupCallTarget(origin)`: ResolvePass stamps `kHost` on
  every map-typed kSelect, but a nested selector
  (`{'c': {...}}.c.d`) yields a CEL_MAP_ARENA value at runtime that
  the host trampoline rejects; the dispatcher's runtime kind-branch
  routes correctly at any nesting depth (closes the cleanup-backlog
  #9 selection gap).
- **otherwise (proto path)** → `EmitKSelectProtoBranch`: appends a
  `FieldRefRow{field_number, name, owner_fqn}` (index 0 is a
  reserved sentinel; `field_number = 0` means the host resolves the
  FieldDescriptor by name) and emits
  `cel_host.cel_get_field(out, msg, field_ref_id, attribute_id)` —
  or `cel_has_field` for test_only. `attribute_id` is the OPERAND's
  id; the trampoline appends the field at runtime for
  unknown-pattern matching.

Relatedly, `PickIndexCallTarget` (the `_[_]` arm) forces the
kDynamic dispatcher whenever the operand is a kSelectExpr, for the
same #9 reason — the stamped kHost origin can be wrong about a
nested select's runtime kind. The static `kArena` fast path is still
taken for literal-origin operands; the test suite asserts both the
positive (arena emits no dispatcher/host call) and the negative
direction.

### 6.3 Three-path aggregate dispatch

Map/list operations pick their call target from the operand's origin
annotation: `kArena` → `cel.*_arena` pure-wasm fast path; `kHost` →
`cel_host.*` trampoline; `kDynamic` → the runtime dispatcher, which
kind-branches once and `musttail`-calls the right arm. Seeds for
aggregate ops in the OverloadTable point at the kDynamic dispatcher
names so the table stays a flat id→name map; only the bespoke `_[_]`
and kSelect arms exploit compile-time origin (the WAT triples
`07`–`09` / `12`–`14` are the three shapes side by side).

### 6.4 Ternary

`EmitConditional` (`wat/33_conditional.wat`): outer probe
`kind == CEL_BOOL` on the cond slot — any non-bool cond (UNKNOWN /
ERROR, and under dyn potentially other kinds) is copied verbatim to
out_slot; inner probe `payload != 0` selects the arm. Each arm's
eval expression is nested INSIDE its if-arm, so only the chosen arm
executes; `BuildConditionalArm` copies from the arm's emitted value
expression — not from `storage.payload` — via the Storage-aware
`cel_copy_slot`, which is what makes kLocal-resident arms correct.

> **Open question (V10):** for a dyn-typed non-bool cond
> (`dyn(1) ? 2 : 3`) the copy-cond-verbatim probe returns the cond
> value where cel-cpp presumably errors ("no matching overload").
> Oracle + e2e comparison pending; statically-typed non-bool conds
> are checker-rejected, so the difference is dyn-only.

### 6.5 Cross-numeric comparison re-pick (documented Option B)

`MaybeRepickCrossNumericOverload`, called from `EmitGeneralCall`:
when the function is one of `_<_ _<=_ _>_ _>=_` and the operand
Reprs span a numeric cross-pair (int/uint/double), codegen OVERRIDES
the ResolvePass-stamped overload id with the cross-numeric id
(`cel_numeric_<op>_at_vv` family) from four hand-written switch
tables.

This is not an accident or a layering violation — it is the
documented Option B from `cross-numeric-ordering-plan.md`, justified
by an executed probe (2026-04-25, recorded in that doc's
"Failure-mode probe" section): cel-cpp's reference map carries
**exactly one** candidate per call — the same-kind overload of the
non-dyn operand (`dyn(1) < 2u` → `[less_uint64]`) — and cel-cpp
rejects non-dyn cross-numeric forms at the checker entirely. So
there is no candidate list for a resolve-time pick (the doc's
original Option A) to choose from; the id must be synthesized from
operand Reprs, which are accurate only post-annotation-forwarding.
Any future "move it to ResolvePass" proposal must re-confirm that
probe first.

Residual debt that IS real: the 24 id strings duplicate rows of
`kBuiltinSeeds` (including cel-cpp's `_uint`-not-`_uint64` typo
`greater_equals_uint_double`, mirrored verbatim at both sites — the
tripwire does byte-equal lookups, so "fixing" the string would
regress) with no tripwire tying the two tables together. A drifted
string fails LOUDLY (Unimplemented naming the id), not silently.

> **Open question (V13):** only 2 of the 24 cross-pair cells are
> unit-tested at the codegen layer; the behavioral load rides the
> 72-row e2e matrix (`e2e/m5_test.cc::CrossNumericOrderingE2ETest`).
> A parameterized codegen test over all 4 ops × 6 pairs is pending.

> **Open question (V47):** can `EmitGeneralCall` see a pre-stamped
> cross-numeric id with same-kind Reprs (re-pick returns empty → id
> kept; the helper is polymorphic so likely benign), e.g. under
> parse-only/`disable_check` flows? Unprobed.

### 6.6 Comprehension lowering

One TU (`expr_lower_comprehension.cc`); entry
`LowerComprehension` emits prologue + `(block exit (loop continue
…))` + result. The authoritative shapes are
`wat/60_comprehension_exists_list.wat` through
`67_three_arg_list_exists.wat` plus `65_celbind_degenerate.wat` and
`66_nested_comprehension.wat`.

- `CompContext` binds iter/iter2/accu by the ResolvePass-stamped
  per-comp indices, never by name (`@result` is reused at every
  nesting depth); exit/continue labels are expr_id-suffixed so
  nested same-name comps pass Binaryen's label validator.
- List sources are normalised through `cel_list_arena_view` (arena
  passthrough / host snapshot) so the inline 24-byte-stride pointer
  walk is origin-uniform; map sources go through
  `cel_map_iter_init/next/key_at/value_at`, which kind-dispatch
  internally (`wat/64_comprehension_exists_map.wat`).
- The loop step is classified ONCE into a closed shape set by AST
  structure (macro names are erased by the expander):
  kListAppend(If) / kMapInsert(If) / kMapMerge(If) / kGeneric.
  Collection accumulators are PRE-SIZED (`cel_list_create` /
  `cel_map_create` with capacity = `iter_range count × per-iter`)
  and the runtime append/insert traps on overflow — the pre-size +
  trap pair is the regression tripwire for a codegen sizing bug.
  The accu-copy paths route through the Storage-aware
  `EmitCelCopySlot`, so a bare-ident accu_init or a bare
  `kIdent(@result)` loop step (the cel.bind pass-through) is read
  via `local.get`, not by misusing the local index as an offset.
- The loop-cond peephole admits a closed set of four shapes (kConst
  true/false, `@not_strictly_false(@result)`,
  `@not_strictly_false(!@result)`); anything else fails compile with
  Unimplemented rather than emitting a wrong loop.
- Unsupported sub-shapes crash loudly per repo rule — and one is a
  live, pinned crash: a `transformMapEntry` whose entry expr is not
  a literal kMapExpr (e.g. a ternary) hits `ABSL_CHECK(false)` and
  aborts the compiler instead of returning a status
  (`KnownBugs.TransformMapEntryComputedEntryCrash`, kept skipped
  because running it aborts the test binary).

**Known bug (pinned, open).** The loop-cond peephole reads the
accu's bool-payload bits at offset 8 WITHOUT checking
`accu.kind == CEL_BOOL`, so an ERROR accumulator (non-zero error
code in the same slot) trips `exists`'s early exit:
`[0, 2].exists(x, 2/x == 1)` returns the division error where the
spec (error absorbed by a later matching element) says `true`.
Pinned as `KnownBugs.ExistsAbsorbsErrorAccumulator`
(`e2e/known_bugs_test.cc:426`); the fix is a kind check ahead of the
payload probe.

> **Open question (V18):** the optional `or`/`orValue` overloads are
> seeded as eager slot-out helpers (`cel_optional_or_at_vv`,
> `cel_optional_or_value_at_vv`), so the RHS always evaluates — the
> wat-traces M14.4 short-circuit requirement
> (`optional.of(1).orValue(1/0)` must not produce a spurious error)
> is unverified against this lowering. Oracle + e2e probe pending.

### 6.7 OverloadTable

`kBuiltinSeeds`: 271 rows (count pinned by
`overload_table_test.cc::kBuiltinSeedCount`) mapping cel-cpp
overload-id strings — copied verbatim, typos included — to
`(ImportModule, helper_name)`. `kExplicitlyUnimplementedIds` is down
to 6 ids, all special-cased in the emitter (conditional,
not_strictly_false ×2, index_list/map, to_dyn). The coverage
tripwire partitions every `cel::StandardOverloadIds::k*` between the
two sets and rejects overlap — a new cel-cpp standard id fails the
test until it is seeded or explicitly excluded. Built-in arity comes
from the generated ABI catalogue (`abi::FindBuiltinHelper`); a seed
missing from the catalogue CHECK-fails at Build(). `RegisterCustom`
copies caller strings into deque-stable storage, rejects builtin
shadowing and duplicates; interned ids are 1-based, 0 = unresolved.

> **Open question (V48):** which seed satisfies the tripwire for
> `kTimestampToDate` / `WithTz` (presumably the
> `timestamp_to_day_of_month_1_based*` family) — confirm and note in
> the seed table comment.

### 6.8 WasmModule rules and the ABI-constant gap

`WasmModule` is RAII over `BinaryenModuleRef`. Rules that are easy
to get wrong and are therefore test-pinned:

- `Adopt(existing)` takes the feature-set UNION (narrowing trips
  Binaryen's feature-dependency asserts) — static mode adopts the
  stripped runtime this way.
- `CodegenLoad`/`CodegenStore` always pass `memoryName = nullptr` so
  loads bind to whatever memory exists: static mode's adopted memory
  is named `"0"`, dynamic mode's `"memory"`; hard-coding either
  silently breaks the other mode. `AddActiveDataSegment` takes the
  memory name for the same reason.
- `BinaryenSetMemory` must precede `BinaryenAddMemoryImport`;
  reversed order silently emits a non-imported memory.
- `Optimize(level)`: level 0 is a guaranteed byte-identical no-op
  (golden-test contract); ShrinkLevel is pinned 0 (the wasm is
  Cranelift input — size is irrelevant, smaller-but-slower is the
  wrong trade); the optimize knobs are process-global (the threading
  consequence is `00-architecture.md` §5).

> **Open question (V11):** codegen hand-copies CelValue wire
> constants (CEL_BOOL=1, CEL_INT=2, offsets 0/8, the 24-byte list
> stride in the comprehension pointer walk) with no compile-time tie
> to `runtime/cel_data.h` — the `:expr_lower` target does not dep
> `//runtime:cel_runtime` even though `:static_memory_builder`
> already does. A CelValue layout change would compile green through
> all of `//compiler/codegen` and fail only at e2e. Fix: add the dep
> + replace literals or static_assert them.

## 7. Module finalization and link modes

The facade `celwasm::Compile` (`compile.cc:673-690`) dispatches on
`CompileOptions::link_mode`; both arms share `RunFrontAndLayout`
(parse → check → resolve → layout → static-region gate) at the front
and `LowerExportAndFinalise` at the back, so the arms cannot
silently diverge — one codegen path, two bootstraps.

**kDynamic bootstrap.** Fresh `WasmModule`;
`InstallExprModuleImports` installs the shared `cel.memory` import
(initial pages = `PagesForBytes(mem_size_bytes)`, shared, max pages
from `MemoryLayout::kMaxMemoryBytes`) with the rodata data segment
attached, plus the FULL runtime import surface regardless of AST
shape — zero-arg `arena_reset`, `arena_alloc`, every `cel_host.*`
trampoline, the map family including iteration helpers, the list
family including the `*_if_present` kernels. Per the standing
no-lazy-imports rule: unused imports are harmless; AST-gated imports
are a silent-breakage vector.

**kStatic bootstrap (the default).** `AdoptStrippedRuntime`
(`BinaryenModuleRead` over the embedded wrapper-stripped runtime
bytes) becomes the base module; `InstallExprRodataSegment` adds the
rodata as an active segment on the adopted memory (named `"0"`),
behind the §5.4 CHECK tripwire; `InstallCelHostImports` installs the
`cel_host.*` imports under codegen's canonical internal names — the
adopted runtime already imports the same `(cel_host, X)` pairs but
under wasm-ld-assigned names (`$cel_host_cel_get_field`) codegen
never targets. Wasm allows duplicate imports of the same
`(module, base, type)` triple and wasmtime resolves both to one
trampoline; skipping the parallel set was a considered-and-rejected
"cleanup". `InstallExprModuleImports` is skipped entirely — every
`cel.*` name is a defined function in the adopted module.

**`LowerExportAndFinalise`** (the shared tail): build the
OverloadTable (built-in seeds + each `function_libraries` decl as a
`RegisterCustom` row — `kHost`/`kForeignComponent` route via the
`cel_fn` import module, `kCelDefined` via per-module `kUserModule`);
`InstallOverloadImportsExport`, which probes `BinaryenGetFunction`
per entry and self-skips names already defined in the adopted
runtime (so under kStatic only custom `cel_fn.*` imports land — the
per-entry probe is a hash lookup, microseconds total, deliberately
not gated on link mode) and unconditionally installs `cel_copy_slot`
(emitted directly by ternary lowering, not table-seeded; arity from
the ABI catalogue); `LowerToEvalFunction`; export under
`eval_export_name`; `AttachCelAbiSection` (the serialized
`celwasm.abi.CelAbi` proto, stamped `LINK_MODE_STATIC`/`_DYNAMIC` —
embedder-tooling metadata and a Plan-time tripwire only, never a
routing input; `00-architecture.md` §3); then `FinaliseModule` —
validate FIRST (prove well-formedness before the optimizer mutates
IR), optimize only when `optimize_level > 0`, serialize.

Shape invariants pinned by `compile_test.cc`: a kStatic Program has
ZERO `"cel"`-module function imports, retains `cel_host.*` imports,
exports `eval`, and is >10× the size of its dynamic twin.

The dead surface to know about: `CompiledArtifact.library_modules` /
`CompileLibraryBodies` (the `@native` CEL-defined-fn library-module
producer) is declared and documented in `compile.h` /
`celfn/library_module.h` but has no implementation, no BUILD target,
and no caller — a compiled `@native` call currently emits an
unresolvable import (register rows R2/R3). The fork decision
(inline vs bundled module vs reject-at-Compile) belongs to
`05-custom-functions.md`; this doc only records that
`rodata_base_override` (§5.2) is the layout seam reserved for the
bundled-module option.

## 8. Public surface and options

`Compiler` is built via `Compiler::Builder` (rvalue-consuming
`Build() &&`): `DeclareVariable` (validation deferred to Build —
empty name, `kUnknown` kind, empty Message FQN, duplicate names each
InvalidArgument), `AddLibrary`, and `AddFunction(celfn_source)`
(eager parse, deferred error, FIRST failure wins). Cross-library
overload-id collisions are caught at Build. Declarations cross into
the frontend as `"name:Type"` spec strings. `Program` is pure bytes;
one Compiler mints many Programs (`00-architecture.md` §2).

`CompilerOptions`, knob by knob, with each knob's REAL effect:

| Knob | Claimed | Real effect |
|---|---|---|
| `mem_size_bytes` (default 128 KiB) | "raise for a larger arena" | kDynamic: initial page count of the `cel.memory` import only. The arena is dlmalloc-sized at runtime; the `LoweringOptions` copy is unread. kStatic (default): **no effect at all** (the adopted runtime defines its own memory). The header's arena advice is wrong (register rows R6/R8). |
| `container` | checker name-resolution container | Forwarded verbatim to `CheckOptions::container` → cel-cpp `set_container`. Checker-only; no codegen/layout effect. |
| `optimize_level` (default 0) | "outside [0,3] rejected" | `FinaliseModule` gates on `> 0`, so **negative levels silently compile as level 0**; only > 3 reaches the range check and rejects (register row R7). Level 0 is byte-identical (golden-test contract). Process-global Binaryen state: serialize Compile calls when > 0. |
| `link_mode` (default kStatic) | selects the compile arm | §7. Forwarded by blind `static_cast` between the public and internal enums. |

Internal-only knobs stay on `CompileOptions` (`eval_internal_name`,
`eval_export_name`, `validate`, `serialize`) — the public struct
carries only what tunes an expression's lowering.

> **Open question (V8):** `mem_size_bytes` under kDynamic above
> 256 KiB stamps an import minimum larger than the runtime's
> exported memory and plausibly fails instantiation at Plan; the
> triple probe (static no-op byte-identity, dynamic page stamping,
> dynamic >256 KiB Plan behavior) decides fix-or-delete for the
> option and the CLI flag. The existing
> `MemSizeBytesLargerThanOnePageGrowsPageCount` test asserts only
> validity, not the page count its name claims.

> **Open question (V23):** `container` is the only public knob with
> no test through `Compiler::Compile` or the facade; its behavior is
> verified only by reading `parse_and_check.cc`.

> **Open question (V24):** whichever contract the `optimize_level`
> fix chooses (reject negatives vs document clamp-to-0) needs a
> facade test pinning it; today neither path is pinned.

> **Open question (V25/V26):** nothing locks the three LinkMode
> enums' values together (see `00-architecture.md` §3), and
> double-`Build()` on a moved-from Builder silently succeeds with
> empty state — accept-empty vs reject is undecided.

## 9. Rejected alternatives

Recorded so they are not re-proposed without new evidence (sources:
design-heritage notes; the closed-out plan docs):

- **Sethi–Ullman / Strahler slot pre-assignment.** The original
  design's §6.3 planned compile-time slot aliasing by Strahler
  numbers; never pursued. The shipped answer is release-based LIFO
  reuse (§5.3), which achieves the same peak-≈-depth behavior with a
  far simpler invariant (read-before-write helpers) and an
  observed-correctness e2e battery instead of a proof obligation.
- **`MessagePattern` table for proto literals.** Rejected in the
  original design; shipped shape is `message_type_id` interning +
  `cel_make_message`/`cel_set_field` empty-then-populate calls, which
  keeps aggregate call sites shaped like scalar call sites.
- **Always-host vtable dispatch for aggregates** (per-op host-trip
  cost) and **always-materialise** (violates no-copy on host data) —
  both rejected for the three-path origin split with kDynamic as the
  safe default (§6.3).
- **Explicit branching for `&&`/`||`.** The m5 plan asserted it was
  the only correct lowering; CEL's side-effect freedom makes eager
  slot-out + kernel-side 3VL absorption spec-equivalent and simpler
  (§6.1). Ternary remains the one lazy operator.
- **Resolve-time cross-numeric pick (Option A).** Non-viable — the
  probe showed cel-cpp's reference map has no candidate list (§6.5).
- **Interned-uint32 overload ids.** Replaced by borrowed
  `string_view`s into cel-cpp storage; accepted lifetime coupling to
  the TypedAst in exchange for dropping a round-trip per call site.
  Any future TypedAst serialization must re-own the views.
- **Skipping `InstallCelHostImports` in static mode.** The runtime's
  wasm-ld import names don't match codegen-canonical names; the dual
  import set is wasm-spec-correct and resolves to one trampoline
  (§7).
- **Inlining the runtime "not in this rewrite".** The original
  recommendation lost to measurement; static linking is now the
  default, with the old reasoning still governing kDynamic
  (`00-architecture.md` §3 carries the flip rationale).
- **Rodata caps / deduplication / runtime-initialised literals.**
  Deliberate non-features: the builder is infallible and every
  literal gets its own frame; the budget is enforced at the region
  gate, not the packer (§5.2, §5.4).
- **AST-gated ("lazy") imports.** Standing rule: the full runtime
  surface installs regardless of AST shape (§7).

## 10. Future work

- **Loop-cond peephole kind check** — fix
  `ExistsAbsorbsErrorAccumulator` (§6.6); and convert the
  `transformMapEntry` computed-entry `ABSL_CHECK` aborts into status
  errors.
- **`@native` fork resolution** — `CompileLibraryBodies` /
  `library_modules` / `rodata_base_override` either get their
  producer or get deleted; decision in `05-custom-functions.md`
  (V5).
- **ABI-constant tie** — dep `//runtime:cel_runtime` from
  `:expr_lower`, replace the hand-copied CelValue literals (V11).
- **Cross-numeric table tripwire + full cell coverage** (V13) and a
  seed↔re-pick-table consistency test.
- **Origin-inference growth** — the unshipped rows (kCall→kHost,
  comp-fold→kArena, same-origin coalescing) remain valid
  optimizations now that lowering routes nested selects through the
  dispatcher; measure before building.
- **Single-walk Pass D** — collapse select+aggregate traversals if
  the peak-slot win justifies re-auditing the aliasing classes
  (V46 residual).
- **Per-module Binaryen optimization** — `BinaryenModuleRunPasses`
  instead of the process-global level setter, removing the §8
  serialize-Compile-calls caveat.
- **Relocatable / growable static region** — the rodata-bound
  ceilings (literal `in`-list at ~327 ints) are legitimate
  expressions; lifting them needs the multi-band relocation scheme
  (`rodata_base_override` + pointer rewriting) or rodata spill into
  the heap. The known-bugs boundary tests flip back to value checks
  when it lands.
- **Option-contract pins** — V8 (mem_size triple probe), V23
  (container), V24 (optimize_level), V25 (enum lock), V26 (Builder
  double-build).
- **Header-comment rot cleanup riding the next code commits** — the
  stale `compile.h` docblock (M1 module shape, two-arg
  `arena_reset`), `compiler.h`'s `function_libraries_`
  "storage-only" claim and stale optimize/mem_size text,
  `expr_lower.h`'s kConst-only contract, `overload_table.h`'s stale
  reasons-list, `slot_allocator.h`'s trailing "M1 ships the no-op
  form" sentence (the free list shipped), and
  `annotations.h::select_key_rodata_offset`'s kOptional-only claim
  (kMap operands also lift keys now).

<!-- diagram-wanted: pass-contract chain — one box per pass with its
     consumes/produces edge labels and the three gate diamonds
     (RejectDyn, slot-exhaustion, ValidateExprStaticRegion) -->
<!-- diagram-wanted: three-path dispatch decision tree — operand
     repr → origin annotation → forced-dynamic overrides (select
     operand, map-dot sugar) → call target -->
<!-- diagram-wanted: slot-allocator timeline for
     `(a + b) + [c, d][0]` showing PreVisit-pin vs
     release-then-acquire and the LIFO free list state -->

## History

This doc supersedes (each gets an archive banner pointing here):

- `doc/implementation-plan/rewrite/design.md` — compiler sections
  (§3–§8, §10–§14); architecture content went to
  `00-architecture.md`.
- `doc/implementation-plan/rewrite/memory-layout-design.md` —
  jointly with `03-abi-and-memory.md`; its A11/A12 bounded-layout
  rows described a gate that did not exist when written and is now
  shipped in a different shape (§5.4).
- `doc/implementation-plan/rewrite/map-list-dispatch.md` — §6.3
  here; its §2.1 inference table is wider than shipped (§4).
- `doc/implementation-plan/rewrite/m5-kcall-comprehensions.md` and
  follow-on — §6.1/§6.6; its `&&`/`||` mechanism claim is corrected
  in §9.
- `doc/implementation-plan/rewrite/cross-numeric-ordering-plan.md` —
  §6.5 carries the probe findings and the Option-B verdict.
- `doc/implementation-plan/rewrite/slice2-control-flow-plan.md` —
  §6.4; the shipped ternary probe is `kind == CEL_BOOL`, not the
  planned `kind >= 15`.
- `doc/implementation-plan/rewrite/dyn-passthrough-plan.md` —
  §2.3/§4/§6.1; the admission summary there omits the `has_type()`
  arm.

The WAT corpus under `doc/implementation-plan/rewrite/wat/` and the
per-arm walkthroughs in `wat-traces.md` remain the maintained
lowering reference; this doc cites, never copies, them.
