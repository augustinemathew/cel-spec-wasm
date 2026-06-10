# codegen-memory — design notes (undefined)

Component: the codegen memory pipeline — `ResolvePass`, `LayoutPass`,
`StaticMemoryBuilder`, `SlotAllocator` (all under `compiler/codegen/`),
plus the linear-memory map they define, verified against
`doc/implementation-plan/rewrite/memory-layout-design.md` and
`doc/implementation-plan/rewrite/map-list-dispatch.md`.

## 1. Verified architecture

### 1.1 Pipeline position and data flow

`ParseAndCheck` → `TypedAst` (annotations pre-seeded with `repr` from the
checker type_map and `field_number` on selects; `resolve_pass.cc:653-659`)
→ `ResolvePass(TypedAst) → ResolveOutput` (`resolve_pass.h:134-135`)
→ `LayoutPass(TypedAst, ResolveOutput, LayoutOptions) → StaticLayout`
(`layout_pass.h:155-157`) → `expr_lower` + `BuildCelAbi` consume
`StaticLayout`. Neither pass mutates the AST; both passes write only
into the side-table `WasmAnnotations` keyed by expr id.

### 1.2 ResolvePass — annotation stamping (resolve_pass.cc)

Runs 8 independent visitors in a fixed order over the checked AST
(`RunAnnotationVisitors`, `resolve_pass.cc:605-643`); only the last
(`DynPassthroughVisitor`) is order-sensitive — it must run after all
others so `dyn(x)`'s copied fields are final (`resolve_pass.cc:601-604`).

1. **KConstReprAudit** (`resolve_pass.cc:29-50`) — `ABSL_CHECK`s every
   kConst has a non-kUnknown `repr` (fail-loud against garbage rodata).
2. **ScopedIdentResolver** (`resolve_pass.cc:86-303`) — interns idents
   into a dense `variables` table (`local_index` 0,1,2,… first-seen;
   `resolve_pass.cc:135-140`). Maintains a comprehension scope stack:
   - `iter_range`/`accu_init` resolve in the OUTER scope; `loop_condition`
     + `loop_step` in an inner frame with iter (+iter2) + accu bound;
     `result` in an accu-only frame (`resolve_pass.cc:71-79, 233-258`).
     One frame covers cond+step (pushed at LOOP_CONDITION pre-visit,
     popped at LOOP_STEP post-visit; `resolve_pass.cc:260-274`).
   - Iter-var lifecycle table (`IterKindsFor`, `resolve_pass.cc:153-194`):
     single-iter list → iter = `kComprehensionIter` (moving pointer, no
     slot); map source → all iter vars `kComprehensionAccu` (slot,
     written by `cel_map_iter_*_at`); two-iter list → iter_var (index) =
     `kComprehensionAccu`, iter_var2 (value) = `kComprehensionIter`.
   - Per-comp binding indices are stamped on the comp node's annotation
     (`comp_iter_local_index` / `comp_accu_local_index` /
     `comp_iter2_local_index`, `resolve_pass.cc:224-229`) so codegen
     resolves by expr id, never by name — cel-cpp reuses `@result` at
     every nesting depth (`resolve_pass.cc:218-223`).
   - Iter/accu Reprs start `kUnknown` and resolve lazily on first kIdent
     reference; subsequent references CHECK-match (`resolve_pass.cc:116-126`).
   - `scope_id` = 1-based comprehension depth on scope-bound idents,
     0 on free idents (`resolve_pass.cc:127-128, 150`). Comprehension
     subexpression callbacks must be explicitly enabled on the traversal
     (`resolve_pass.cc:612-619`).
3. **AttributePathResolver** (`resolve_pass.cc:319-374`) — interns
   `(root_variable, qualifiers)` paths from kIdent+kSelect post-order;
   id 0 is the reserved sentinel pushed by the runner
   (`resolve_pass.cc:621`). Scope-bound idents get `attribute_id = 0` —
   not attribute roots (`resolve_pass.cc:331-339`). A select over a
   non-path-bearing operand stays 0 (`resolve_pass.cc:344-348`). The
   trampoline appends the select's own field from the OPERAND's id at
   `cel_get_field` time (`resolve_pass.cc:312-318`).
4. **MessageTypeIdVisitor** (`resolve_pass.cc:566-599`) — interns
   kStructExpr FQNs into `message_types` (sentinel at 0,
   `resolve_pass.cc:625-629`); CHECKs `name()` non-empty (empty-name
   struct literals must have lowered as kMapExpr at parse time,
   `resolve_pass.cc:577-580`).
5/6. **MapOriginVisitor / ListOriginVisitor** (`resolve_pass.cc:386-467`)
   — the implemented origin-inference table is exactly three rows per
   aggregate kind: kMapExpr/kListExpr → `kArena`; map/list-typed kIdent
   and kSelect with `scope_id == 0` → `kHost`; everything else stays the
   `kDynamic` default (`ir/annotations.h:104-105`). Comp-scope idents
   deliberately stay `kDynamic` (value may be arena-resident,
   `resolve_pass.cc:412-418, 461-463`).
7. **OverloadIdResolver** (`resolve_pass.cc:521-546`) — copies the first
   overload id from cel-cpp's `reference_map` onto each kCall as a
   `string_view` into cel-cpp-owned storage (lifetime = the TypedAst,
   `resolve_pass.cc:536-540`). Empty is legitimate for special-cased
   operators (`_[_]`, `_&&_`, `_||_`, `_?_:_`; `resolve_pass.cc:518-520`).
8. **DynPassthroughVisitor** (`resolve_pass.cc:478-507`) — for global
   1-arg `dyn(x)`, copies the arg's non-storage annotation fields onto
   the call node; storage forwarding happens later in LayoutPass
   (`resolve_pass.cc:475-477`).

Output (`ResolveOutput`, `resolve_pass.h:105-122`): annotations,
`variables` (free + comp-scope bindings, dense first-seen order;
`variables.size()` = the count of i32 wasm locals `$eval` declares),
`attributes` + `message_types` intern tables (sentinel entry 0),
`max_scope_id`.

### 1.3 LayoutPass — the five sub-passes (layout_pass.cc:375-437)

Inputs are moved out of `ResolveOutput` into the returned `StaticLayout`
(`layout_pass.cc:381-384`). `LayoutOptions` (`layout_pass.h:50-64`):
`debug_layout` (forwarded to `StaticLayout::debug_mode`) and
`rodata_base_override` (replaces the default `rodata_base = 16`,
`layout_pass.cc:386-388`).

- **Pass A — rodata** (`layout_pass.cc:390-400`): `ConstLayoutVisitor`
  packs every kConst into one `StaticMemoryBuilder` and stamps
  `{kStaticRodata, abs_offset}` (`layout_pass.cc:43-47`). Dispatch is on
  the Constant oneof, EXCEPT `Repr::kType` constants (rewrite targets of
  `InlineTypeIdentifierReferences`) which pack as CEL_TYPE via
  `AllocateType` (`layout_pass.cc:50-66`); an unrecognised variant is
  `ABSL_CHECK(false)` (`layout_pass.cc:74-79`). Then
  `SelectKeyRodataVisitor` lifts the field name of every kSelect whose
  OPERAND is `Repr::kOptional` into rodata as a CEL_STRING and stamps
  `select_key_rodata_offset` (`layout_pass.cc:133-153`); the kernel
  `cel_select_optional_field_at_vv` reads it as a CelValue key
  (consumed at `expr_lower.cc:243-248`). Both visitors share one
  builder so rodata is contiguous (`layout_pass.cc:390-394`).
- **Pass B — variable slots** (`ReserveVariableSlots`,
  `layout_pass.cc:341-365`): `workspace_base = RoundUp8(rodata_base +
  rodata.size())`; one 24-byte slot per variable EXCEPT
  `kComprehensionIter` vars, whose `slot_offset` stays 0 (sentinel —
  their wasm local holds a moving pointer; `layout_pass.cc:351-363`,
  `layout_pass.h:33-39`). Slots pack densely in allocation order (no
  holes for skipped iter vars). `static_assert(sizeof(CelValue) == 24
  && 24 % 8 == 0)` (`layout_pass.cc:343-345`).
- **Pass C — kIdent storage** (`IdentStorageVisitor`,
  `layout_pass.cc:94-120`): stamps `{kLocal, local_index}`; CHECKs the
  index is in range. The slot offset itself is NOT in the annotation —
  the `$eval` prelude `local.set`s each free-variable local to its
  `slot_offset` (`expr_lower.cc:158-174`), and comp-scope entries are
  skipped there and in the ABI emitter (`expr_lower.cc:168`,
  `cel_abi_emit.cc:27`).
- **Pass D — scratch slots** (`layout_pass.cc:410-420`): a single
  `SlotAllocator` based at `workspace_base + workspace_bytes` is shared
  by TWO SEQUENTIAL traversals: `SelectStorageVisitor` (every kSelect:
  release operand's workspace slot, acquire own;
  `layout_pass.cc:160-181`) then `AggregateStorageVisitor` (kMapExpr /
  kListExpr / kStructExpr / every kCall: release child workspace slots,
  acquire one result slot; `layout_pass.cc:203-301`). `dyn(x)` forwards
  the arg's storage instead of acquiring (`layout_pass.cc:252-268`).
  Receiver-form calls release the target's slot too
  (`layout_pass.cc:276-283`). Ternary branch-arm slots are allocated
  inside expr_lower, not here (`layout_pass.cc:269-277`). Aggregate
  entry/element scratch is the children's own slots — no per-entry
  pre-reservation (`layout_pass.cc:194-201`). After both walks,
  `workspace_bytes += slots.total_bytes()`; `peak_slots` recorded
  (`layout_pass.cc:419-420`).
- **arena_base** = `RoundUp8(workspace_base + workspace_bytes)`
  (`layout_pass.cc:422-423`) — computed and tested, but legacy: codegen
  no longer consults it (`layout_pass.h:82-84`; no non-comment consumer
  outside layout_pass — the arena lives in the wasi-libc dlmalloc heap
  and `arena_reset` is zero-arg, `expr_lower.cc:136-144`).
- **Pass E — comprehension aux locals** (`ComprehensionLocalsVisitor`,
  `layout_pass.cc:313-339, 425-434`): stamps `comp_aux_local_base` =
  `variables.size() + 3×(comp pre-order index)`; 3 locals per comp
  (end_off / cursor / index or source_addr; `layout_pass.h:129-146`).
  `total_wasm_locals = variables.size() + 3 × #comprehensions`.

NOT given storage by any visitor: kComprehensionExpr nodes (storage
stays `kNone`; the result flows via the accu slot in
`expr_lower_comprehension.cc:362-373, 599, 871-873`) — despite the
header comment, see §2.3.

### 1.4 StaticMemoryBuilder — rodata byte packing (static_memory_builder.cc)

- Frame = 24-byte CelValue: u32 kind LE @ +0, u32 pad @ +4, 16-byte
  payload @ +8 (`OpenFrame`, `static_memory_builder.cc:56-63`; CHECKs
  the cursor is 8-aligned). Every `Allocate*` CHECKs the frame is
  exactly 24 bytes and zero-fills unused payload.
- Returned offsets are ABSOLUTE wasm linear-memory offsets
  (`base_offset + local`; `static_memory_builder.h:10-16`), droppable
  into `i32.const`/CelSpan without arithmetic. Constructor CHECKs
  `base_offset % 8 == 0` (`static_memory_builder.cc:50-54`).
- string/bytes/type: CelSpan `{ptr = base+local+24, len}` in payload;
  payload bytes follow the frame; cursor padded back to 8
  (`AllocateSpan`, `static_memory_builder.cc:107-124`). `AllocateType`
  is byte-identical to a string except `kind = CEL_TYPE`
  (`static_memory_builder.cc:134-137`).
- Infallible by design — no cap, no StatusOr
  (`static_memory_builder.h:18-22`); the budget gate lives downstream
  (§1.6).
- `AllocateList`/`AllocateMap` are `ABSL_CHECK(false)` stubs
  (`static_memory_builder.cc:139-153`) — aggregates are built at eval
  time in the arena, never in rodata.
- No deduplication: every kConst gets its own frame, even equal
  literals (one `Pack` per PostVisitConst, `layout_pass.cc:43-47`;
  distinctness pinned by `layout_pass_test.cc:217-232`).

### 1.5 SlotAllocator — currently a monotonic bump (slot_allocator.cc)

`Acquire()` = `base + peak_slots_ * 24`, increment; `Release()` is a
NO-OP ("Naive path … M10 flips on the free list",
`slot_allocator.cc:18-28`). Therefore `peak_slots()` actually returns
TOTAL acquires, not peak simultaneous liveness, and `debug_mode` has no
behavioural effect today (`slot_allocator_test.cc:27-47` pins both
facts). Constructor CHECKs 8-aligned base (`slot_allocator.cc:13-15`).
The header documents the intended release discipline (post-order:
release every workspace child after its single read, then acquire;
`slot_allocator.h:11-21`) and a free-list trace that does not match
current behaviour (`slot_allocator.h:60-77` — explicitly labelled as
the M10 future).

### 1.6 The linear-memory map and its enforcement

Layout (verified against `layout_pass.h:78-95`, `runtime/cel_layout.h`,
`compile.cc:507-541`, `engine.cc:351-377`, `instance.cc:413-420`):

```
[0, 8)         reserved null sentinel
[8, 16)        reserved (legacy arena cursor slot; unused by codegen)
[16, …)        rodata  (rodata_base default 16; overridable)
ws_base        RoundUp8(rodata_base + rodata.size()); 24B cells:
               referenced-variable slots, then SlotAllocator scratch
[8192, heap)   wasi-libc statics + 64KB shadow stack (runtime-owned;
               --global-base = CELWASM_RESERVED_LOW_MEMORY_BYTES = 8192,
               cel_layout.h:37)
[heap, …)      dlmalloc heap: per-Instance bump arena
               (CELWASM_ARENA_CAPACITY_BYTES = 64 KiB, cel_layout.h:41),
               activation buffer, plan-lifetime objects
```

Enforcement as actually implemented:
- Rodata budget: `InstallExprRodataSegment` returns ResourceExhausted if
  `rodata_base + rodata.size() > 8192` — STATIC link mode only, rodata
  only (`compile.cc:524-536`; pinned by
  `compile_test.cc:356-374`). Dynamic mode has no budget check
  (`compile_test.cc:376-385` pins that oversized rodata COMPILES).
- Workspace is bounded NOWHERE (grep: no consumer of
  `workspace_base/workspace_bytes` performs a limit check; the only
  RESERVED_LOW checks are compile.cc:527, engine.cc:374 (`__heap_base`
  ≥ 8192, A14), instance.cc:418 (activation buffer ≥ 8192, A15)).
- A13 is a `>=` page floor, not `==` (`engine.cc:357-360`,
  `cel_layout.h:18-29`).
- `cel.abi` carries per-variable `slot_offset` only — no region bases
  (`cel_abi.proto:60`; emitter filters to `kFreeVariable`,
  `cel_abi_emit.cc:27`).

### 1.7 Key invariants worth restating in the new doc

- CelValue is 24 bytes, 8-aligned; 24 % 8 == 0 keeps contiguous cells
  aligned from any 8-aligned base (`layout_pass.cc:343-345`,
  `slot_allocator.cc:11-15`, `static_memory_builder.cc:18-23`).
- Offset 0 is the universal "absent" sentinel: iter vars'
  `slot_offset == 0` (`layout_pass.cc:351-363`), `attribute_id == 0`,
  `message_type_id == 0`, `select_key_rodata_offset == 0` — all rely on
  real offsets/ids starting ≥ 16 / ≥ 1.
- Unreferenced declared variables get no slot, no local, no ABI entry
  (`resolve_pass.h:96-99`; `layout_pass_test.cc:343-351`).
- Comp-scope variables are invisible to the prelude and the ABI
  (`expr_lower.cc:168`, `cel_abi_emit.cc:27`); they are set by the
  comprehension loop prologue.
- `string_view` annotation fields (`overload_id`) alias cel-cpp-owned
  storage; the TypedAst must outlive codegen (`resolve_pass.cc:536-540`).

## 2. Doc-vs-code discrepancies

1. **P0 — "Bounded" layout claim is unimplemented.**
   `memory-layout-design.md:87-93` ("LayoutPass fails with
   ResourceExhausted if rodata + workspace would overrun the region
   (`LayoutOptions::reserved_region_limit_bytes`, default
   CELWASM_RESERVED_LOW_MEMORY_BYTES)") and the §4 invariant rows A11/A12
   ("layout_pass.cc (ResourceExhausted)", `memory-layout-design.md:173-174`).
   Code: `LayoutOptions` has no such field (`layout_pass.h:50-64`);
   `layout_pass.cc` contains no ResourceExhausted and no limit check.
   The only gate is rodata-only, static-mode-only, in
   `compile.cc:524-536`. Workspace (`workspace_base + workspace_bytes`)
   is unbounded in every mode — and because `SlotAllocator::Release` is
   a no-op, workspace grows by 24B per kSelect/kCall/aggregate node, so
   ~340 such nodes push workspace stores past 8192 into the runtime's
   static data at eval time, with no compile-time or run-time tripwire.
2. **P1 — SlotAllocator "recycles" / peak_slots semantics.**
   `memory-layout-design.md:83-85` ("The SlotAllocator **recycles**
   workspace cells across non-overlapping lifetimes and tracks
   peak_slots") and `slot_allocator.h:102-104` ("Peak number of 24-byte
   cells live simultaneously"). Code: `Release` is a no-op and
   `peak_slots_` increments per Acquire (`slot_allocator.cc:18-28`) —
   it is total-acquires, never peak-liveness. Safe (over-allocation)
   but the doc/header describe an allocator that doesn't exist;
   `slot_allocator_test.cc:27-37` pins the no-op.
3. **P1 — layout_pass.h claims kComprehensionExpr gets storage.**
   `layout_pass.h:72-74` lists `kComprehensionExpr` among nodes whose
   `.storage` is written by SlotAllocator. No visitor in
   `layout_pass.cc` handles comprehension storage (only
   `ComprehensionLocalsVisitor`, which stamps `comp_aux_local_base`,
   `layout_pass.cc:313-339`); comp nodes keep `storage.kind == kNone`
   and the result flows through the accu slot
   (`expr_lower_comprehension.cc:599, 871-873`).
4. **P1 — `rodata_base_override` cites a nonexistent consumer.**
   `layout_pass.h:57-62` says it is "Used by
   `compiler/celfn/library_module.cc`". That file does not exist —
   `compiler/celfn/` contains only `library_module.h`, whose declared
   `CompileLibraryBodies` has no implementation, no BUILD target, and
   no caller (repo-wide grep). `rodata_base_override` has zero
   production users and zero tests; the multi-band `[0,8192)` scheme in
   `memory-layout-design.md:190-196` ("cumulative rodata_base_override
   + per-band reserved_region_limit_bytes") is entirely unbuilt.
5. **P1 — map-list-dispatch.md §2.1 inference table is wider than the
   shipped code.** Doc rows `kCall returning a map → kHost`,
   `kComprehension folding into a map → kArena`, and the same-origin
   branch coalesce (`map-list-dispatch.md:91-99, 111-127`) are NOT
   implemented: `MapOriginVisitor`/`ListOriginVisitor` stamp only
   kMapExpr/kListExpr (kArena) and scope-free kIdent/kSelect (kHost);
   everything else stays kDynamic (`resolve_pass.cc:386-467`; the
   fall-through is acknowledged at `resolve_pass.cc:383-385`). Safe by
   design (kDynamic is the correct-but-slower default,
   `map-list-dispatch.md:81-84`), but the doc presents the table as
   shipped.
6. **P2 — dangling section reference.** `resolve_pass.cc:380` and
   `:425` cite "`rewrite/map-list-dispatch.md` §2.6 inference table";
   the doc has no §2.6 (the table is §2.1).
7. **P2 — map-list-dispatch.md §4.4 CelKind numbering is stale.** Doc
   (`map-list-dispatch.md:299-317`): MAP_ARENA=7…ERROR=15, no
   CEL_TYPE/CEL_OPTIONAL. Code (`runtime/cel_data.h:32-49`):
   LIST_ARENA=7, MAP_ARENA=8, MAP_HOST=9, MESSAGE=10, TYPE=11,
   OPTIONAL=14, UNKNOWN=15, ERROR=16, LIST_HOST=17. §12's retro
   partially corrects the map values; §4.4 stands wrong.
8. **P2 — A13 wording.** `memory-layout-design.md:176` says
   "instantiated memory page count == initial"; code asserts `>=` a
   floor (`engine.cc:357-360`), and `cel_layout.h:18-29` explains why
   (link-time auto-sizing varies 2-4 pages).
9. **P2 — "[0,16) null sentinel".** `memory-layout-design.md:63,76-78`
   describes the whole 16 bytes as the null sentinel;
   `layout_pass.h:80-83` splits it: `[0,8)` null sentinel, `[8,16)`
   legacy arena cursor/limit slot no longer consulted.
10. **P2 — expr_lower.h still documents the two-arg arena_reset.**
    `expr_lower.h:7` and `:201` show
    `call $arena_reset(<arena_base>, <arena_limit>)`; the emitted call
    is zero-arg (`expr_lower.cc:136-144`;
    `abi/runtime_catalogue_test.cc:175` pins 0 args).
    `memory-layout-design.md:109-110` has it right.
11. **P2 — stale "stub until M5/M6/M10" milestones.**
    `AllocateList`/`AllocateMap` say "stub until M5"/"until M6"
    (`static_memory_builder.cc:142,151`) and SlotAllocator says "M10
    flips on the free list" (`slot_allocator.cc:25-26`,
    `slot_allocator.h:77`) — those milestone numbers shipped long ago
    carrying different scope; none of these bodies ever landed. The
    stubs are still correct as tripwires, but the "until <milestone>"
    pointers are dead.

## 3. Validation items

1. **Does an over-8KiB rodata segment corrupt the runtime in kDynamic
   mode?** `compile_test.cc:376-385` pins that it compiles, with a
   comment claiming "no runtime static data below it" — but the dynamic
   expr module imports the same shared `cel.memory`
   (`compile.cc:345-350`) whose owner is linked `--global-base=8192`,
   so the active data segment should overwrite runtime statics at
   instantiate. Probe: e2e test, `link_mode = kDynamic`, expression
   `"<9000 x's>" + "tail"`; Plan + Eval and assert both the literal
   round-trips AND a subsequent arena-using op (string concat) still
   works. (`bazel test` an added case in `e2e/program_roundtrip_test.cc`.)
2. **Workspace overrun past 8192 (the missing A11).** Generate an
   expression with >340 slot-acquiring nodes, e.g.
   `python3 -c 'print("1" + "+1"*400)'` piped to the CLI
   (`bazel run //tools/cel -- --expr "$(python3 -c ...)"`), static
   mode. Expect either a wrong answer / corrupted runtime (confirming
   P0 #1's danger) or some unidentified guard. Result decides whether
   the fix is the doc's promised LayoutPass ResourceExhausted gate.
3. **Map-typed kSelect rooted at a comp-scope ident gets Origin::kHost
   even though the value is arena-resident.** `scope_id` is only
   stamped on kIdent nodes (`resolve_pass.cc:127-128`), so
   `StampHostIfMapTyped`'s `scope_id != 0` skip never fires for
   kSelect (`resolve_pass.cc:409-419`). Probe:
   `[{'a': {'b': 1}}].exists(m, m.a['b'] == 1)` end-to-end — if codegen
   routes the `['b']` lookup through the host trampoline on an
   arena-kind CelValue, it should error or misbehave; if green,
   document why (e.g. select-on-map lowers via a kind-dispatching
   kernel).
4. **Is Pass D's two-traversal structure compatible with real slot
   reuse?** `SelectStorageVisitor` then `AggregateStorageVisitor` run
   as separate walks over one allocator (`layout_pass.cc:413-418`),
   but the release discipline in `slot_allocator.h:11-21` assumes one
   post-order walk: during the select walk, a kSelect whose operand is
   a kCall sees `storage.kind == kNone` (call slots aren't assigned
   yet) and releases nothing, while the aggregate walk later releases
   select slots that another select's parent already reasoned about.
   Settle by: implement a free-list behind `!debug_mode`, run
   `layout_pass_test` + e2e on `c.f[x].g + c.h` shapes, and check for
   slot aliasing between a live select result and a call result.
5. **Is `rodata_base_override` (and `library_module.h`) live or dead?**
   `git log -S CompileLibraryBodies` / check feature branches for the
   m13 CEL-defined-fn module producer; if the v1 single-module design
   landed elsewhere without it, delete the option + header or mark the
   design future-work explicitly.

## 4. Test coverage observations

Pinned well:
- ResolvePass: full kConst Repr matrix; full declarable-type matrix
  (scalars + duration/timestamp + list/map element-kind variations +
  messages) at the kIdent layer; dense first-seen indexing; same-slot
  sharing across nasty shapes (`arr[arr[0]]`, message in its own index
  expr); unreferenced-decl exclusion; field_number preservation
  (`resolve_pass_test.cc:167-584`).
- Comprehension scope: iter/accu kinds per macro (exists/all/map/
  exists_one accu Reprs), nested same-name shadowing produces two
  distinct iter entries, scope-bound idents excluded from attribute
  table (`resolve_pass_test.cc:707-876`); layout side pins iter
  slot_offset==0 sentinel, accu gets a real slot, free-var/accu
  non-collision, two-iter index-as-accu lifecycle
  (`layout_pass_test.cc:700-800`).
- Layout regions: rodata_base==16, workspace 8-alignment,
  arena_base == workspace_base+workspace_bytes (×3 shapes), exact
  byte/slot totals for select chains, map/list literals, indexing,
  control-flow ops (`layout_pass_test.cc:86-676`).
- StaticMemoryBuilder: byte-exact frames per kind incl. INT64_MIN/MAX,
  UINT64_MAX, -0.0 bit pattern, embedded-NUL bytes, empty string,
  7-byte pad, base_offset applied to both frame and span ptr; stub
  death tests (`static_memory_builder_test.cc`).
- SlotAllocator: monotonic bump, no-op release, unaligned-base death
  (`slot_allocator_test.cc`).
- Rodata budget: under/over/dynamic-mode trio (`compile_test.cc:345-385`).

Gaps:
- **`rodata_base_override` has zero tests** (no layout_pass_test case
  sets it; no caller exists) — the only LayoutOptions knob without
  coverage.
- **No origin coverage for map/list-typed kSelect → kHost** in
  resolve_pass_test (§9/§10 cover literal→kArena, ident→kHost,
  non-map default only); the comp-scope-select hazard of validation
  item 3 is untested.
- **No workspace-budget negative test** (mirror of
  RodataOverBudgetReturnsResourceExhausted) — cannot exist until the
  guard does.
- **No test that equal literals get distinct frames** (dedup-freedom
  is implicit); `DistinctLiteralsGetDistinctOffsets` covers distinct
  values only.
- Map-source and `cel.bind`-shape comprehensions have no layout-level
  tests (iter-kind table rows for map_source are exercised only via
  e2e); `AllocateType`/`Repr::kType` packing is untested at the
  builder level (only via layout's optional-select test indirectly —
  actually not at all; `AllocateType` has no direct unit test).
- `total_wasm_locals` / `comp_aux_local_base` values are not asserted
  in layout_pass_test (covered indirectly by expr_lower/e2e).

## 5. Design decisions worth preserving

- **Annotations are a side table; the AST is never mutated.** Every
  pass communicates through `WasmAnnotations[expr_id]`; sentinel-zero
  ids/offsets mean "absent". New node kinds default to safe values
  (`Origin::kDynamic`, `StorageKind::kNone`) so a forgotten visitor
  degrades to slower-but-correct or a loud expr_lower CHECK, never a
  silent miscompile (`map-list-dispatch.md:81-84`, `layout_pass.h:75-76`).
- **By-id, not by-name, comprehension binding lookup** — cel-cpp reuses
  `@result` at every nesting depth; name-based lookup conflates nested
  accu vars (probe-confirmed 2026-05-17, `resolve_pass.cc:218-223`).
- **Iter vars own no workspace cell.** A list iter local is a moving
  element pointer; map iter vars are accu-lifecycle slots written by
  `cel_map_iter_*_at`; the two-iter list index is an accu-lifecycle
  slot rewritten `{CEL_INT, i=idx}` per iteration
  (`resolve_pass.cc:153-194`, `layout_pass.cc:351-363`). slot_offset 0
  is the load-bearing sentinel.
- **Absolute offsets everywhere.** StaticMemoryBuilder bakes
  `base_offset` into both the returned frame offset and the CelSpan
  ptr, so emitted wasm needs no relocation arithmetic
  (`static_memory_builder.h:50-53`). Consequence: rodata is
  position-DEPENDENT — relocating a module requires the
  `rodata_base_override`-style band scheme plus pointer rewriting, the
  stated reason `__memory_base` libraries are future work
  (`memory-layout-design.md:196-201`).
- **Rodata is infallible at the builder, bounded at the linker seam.**
  Packing has no failure mode; the 8192 budget is enforced where the
  segment meets the runtime's `--global-base` (static mode), and it is
  a Status (embedder input must not crash the process), not a CHECK
  (`compile.cc:514-517`).
- **Aggregates never pack into rodata.** `AllocateList`/`AllocateMap`
  stay CHECK-stubs; list/map literals are built per-eval in the arena
  via `cel_list_create`/`cel_map_insert`, with the workspace holding
  only the 24B handle (`static_memory_builder.h:29-37`,
  `layout_pass_test.cc:448-459, 570-582`).
- **The two reserved low slots.** `[0,8)` makes `offset 0 == absent`
  well-defined runtime-wide; `[8,16)` is fossil (pre-WASI arena
  cursor) kept so rodata_base stays 16 and the ABI doesn't churn
  (`layout_pass.h:80-84`).
- **arena_base is computed but legacy** — keep emitting it only if the
  new doc decides the field earns its keep; the arena moved to the
  dlmalloc heap and `arena_reset` is zero-arg
  (`memory-layout-design.md:103-110`, `expr_lower.cc:136-144`).
- **Rejected alternative (recorded in map-list-dispatch.md §1):**
  always-host vtable dispatch for aggregates was rejected for the
  per-op host-trip cost; always-materialise was rejected for violating
  no-copy on host data. The arena/host/dynamic three-path split with
  compile-time origin inference is the keeper, with kDynamic as the
  safe default and `kArena` the opt-in fast path.
- **Origin/storage forwarding for `dyn(x)` is split across the two
  passes on purpose**: ResolvePass copies non-storage fields (runs
  before slots exist), LayoutPass forwards storage
  (`resolve_pass.cc:475-477`, `layout_pass.cc:252-268`) — the arg's
  slot lifetime continues through the call, so no release/acquire.
