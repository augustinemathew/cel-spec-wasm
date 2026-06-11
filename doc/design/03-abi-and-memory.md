# 03 — ABI & memory: the wire contracts

Status: current — authored 2026-06-10 from the design-rebuild notes
plus the post-merge code (chained arena, static-region gates). All
three §8 forks were settled AND fixed 2026-06-10: the error contract
(V4), 3VL precedence (V3), and the §8.2 unknown contract (V2 — the
descriptor-offset wire shipped end-to-end, host writers/readers
aligned). The same day the logic-op UNKNOWN-over-ERROR precedence
(the §8.2 adjacent finding) was oracle-settled and the kernel
aligned — see §8.3's scope note. Probe evidence inline in each
subsection. Supersedes: wire sections of
doc/implementation-plan/rewrite/cel-host-surface.md;
memory-layout-design.md; abi-refactor.md.

One telling for every byte-level fact the compiler, the runtime
kernel, and the evaluator must agree on. `00-architecture.md` names
the contracts; docs 01/02/04 describe the machinery on each side and
defer every byte here.

## 1. CelValue & kinds

The unit of data exchange everywhere is the 24-byte `CelValue`
(`runtime/cel_data.h`, `struct CelValue`): **u32 `kind` at +0, u32
`_pad` (zero) at +4, a 16-byte `payload` union at +8.**
`_Static_assert(sizeof(CelValue) == 24)` pins the size
(`cel_data.h:183`). Payload "pointers" are u32 byte offsets into the
one shared linear memory; **offset 0 is the universal absent
sentinel** (arena OOM, absent value, empty iterator handle, empty
span). A little-endian host is a build requirement (`#error` guard):
host↔wasm CelValue transfer is bitwise memcpy.

### 1.1 The CelKind table

Values are **wire-stable, append-only, never renumbered**
(`cel_data.h:26-30`):

| value | kind | payload arm |
|---|---|---|
| 0 | `CEL_NULL` | none |
| 1 | `CEL_BOOL` | `b` (i32) |
| 2 | `CEL_INT` | `i` (i64) |
| 3 | `CEL_UINT` | `u` (u64) |
| 4 | `CEL_DOUBLE` | `d` (f64) |
| 5 | `CEL_STRING` | `s` = `CelSpan{ptr, len}` |
| 6 | `CEL_BYTES` | `bytes` = `CelSpan{ptr, len}` |
| 7 | `CEL_LIST_ARENA` | `arena_list.header_ptr` (u32) |
| 8 | `CEL_MAP_ARENA` | `arena_map.header_ptr` (u32) |
| 9 | `CEL_MAP_HOST` | `ref_slot` (u32, externref table) |
| 10 | `CEL_MESSAGE` | `msg_slot` (u32, externref table) |
| 11 | `CEL_TYPE` | `s` = span of the spec type-name bytes |
| 12 | `CEL_DURATION` | `dur` = `CelDurTs{seconds:i64, nanos:i32}` |
| 13 | `CEL_TIMESTAMP` | `ts` = `CelDurTs` |
| 14 | `CEL_OPTIONAL` | `opt` (u32) |
| 15 | `CEL_UNKNOWN` | `unk` (u32 — UnknownSet descriptor offset, §8.2) |
| 16 | `CEL_ERROR` | `err` (u32, a `CEL_ERR_*` code, §8.1) |
| 17 | `CEL_LIST_HOST` | `ref_slot` (u32, externref table) |
| 18 | `CEL_IP` | `net_ref` (u32 arena offset → `NetIp`) |
| 19 | `CEL_CIDR` | `net_ref` (u32 arena offset → `NetCidr`) |

`CEL_TYPE` has no intern table — equality is `memcmp` on the payload
span bytes. The host-backed kinds (9, 10, 17) carry slots into three
*independent* host-side externref namespaces (map / message / list;
`ExternrefTable`, `eval/internal/cel_host.h`), each with slot 0 as
the nullptr sentinel and a per-Eval `Reset()`.

### 1.2 Aggregate headers & entry strides

Arena aggregates are a 16-byte header plus a contiguous entry run in
linear memory (`cel_data.h`):

- `ArenaListHeader {count, capacity, elements_offset, _pad}` — 16 B;
  element stride **`kCelListEntryStride = 24`** = `sizeof(CelValue)`
  (`_Static_assert`-tied, `cel_data.h:199`).
- `ArenaMapHeader {count, capacity, entries_offset, _pad}` — 16 B;
  entries are back-to-back `{key, val}` CelValue pairs, stride
  **`kCelMapEntryStride = 48`**.

The host map-iteration snapshot written by
`cel_host.cel_map_iter_open` uses the same 48 B/entry layout;
`MapIterState` is a 16-byte arena struct (`cel_runtime.c`). Entry
runs stride at 24 bytes — an arena/rodata fact; workspace slots
stride at 32 (§4.2).

## 2. The four kind-like enums

Four enums carry "what kind of value is this?" — distinct numberings
**by design**. All conversion is by explicit switch, never a cast,
with **one sanctioned cast-equivalence** (rule 4).

<!-- diagram-wanted: four-enum alignment table as a graphic — one row
     per semantic kind, four columns of numeric values, divergence
     points (Repr's +1 shift; wire-vs-Value split at 9) highlighted -->

| enum | home | job | numbering |
|---|---|---|---|
| `CelKind` | `runtime/cel_data.h` | the wire — CelValue.kind | §1.1; 0–19, append-only |
| `ir::Repr` | `compiler/ir/annotations.h` | per-node ABI repr; also the wire value of `cel.abi.VariableEntry.repr` | implicit: `kUnknown=0, kNull=1, kBool=2, kInt=3 … kType=14, kOptional=15` |
| `CelType::Kind` | `shared/type.h` | public declaration vocabulary | `kUnknown=0, kBool=1 … kMessage=9`, **gap at 10**, `kDuration=11, kTimestamp=12, kType=13` |
| `Value::Kind` | `eval/value.h` | decoded-result kind | `kNull=0 … kMap=8, kMessage=9`, gap at 10, `kDuration=11, kTimestamp=12, kType=13, kUnknown=14, kError=15` |

Alignment rules (every cross-enum conversion site must honor them):

1. **`CelKind` ≠ `Value::Kind` from value 9 up.** They agree on 0–8;
   at 9 the wire has `CEL_MAP_HOST` while `Value::Kind` has
   `kMessage`. The `value.h` "kept in sync" comment is wrong above
   8 — a `static_cast` miscodes silently; conversion lives in the
   decode switches (`eval/instance.cc`, `host_call_context.cc`).
2. **`ir::Repr` matches nothing.** `kNull = 1` shifts every scalar
   by one relative to the wire (`Repr::kInt = 3` vs `CEL_INT = 2`);
   `kEnum = 11` has no CelKind twin. Repr exists only in compiler IR
   and `cel.abi.variables[].repr`; `DecodeRepr`
   (`abi_decode.cc:110-146`) is the one wire→enum map, clamping
   out-of-range and `kOptional` (15, no decode arm) to `kUnknown`,
   on which the activation marshal later fails loudly.
3. **`CelType::Kind` tracks `Value::Kind`** (same gap at 10, same
   `kType = 13`), deliberately narrower: no Null / Optional / Error /
   Unknown factories.
4. **The single sanctioned cast-equivalence:** `celwasm::ErrorCode`
   (`eval/error.h`) ↔ the wire `CEL_ERR_*` codes. The numerics MUST
   mirror 1:1 ("`static_cast`-equivalent", `eval/error.h:19-22`);
   both append-only. The encode direction (`WireErrorCode`,
   `cel_host_error.cc:62-87`) is nonetheless a guarded switch: codes
   with no arm (`kUnknownType = 30`, `kCustomFnFailed = 40`,
   `kTimeout = 50`, and currently `kDuplicateKey = 16`, which *has*
   a wire twin) clamp to `CEL_ERR_TYPE_MISMATCH`.

> **Open question (V6, V14/V15):** `ir::Repr` uses implicit enum
> numbering while `cel_abi.proto:48-50` promises wire stability;
> nothing pins the values — a mid-enum insertion would silently
> renumber every emitted `repr` without tripping
> `runtime_abi_version` (fix: explicit initializers + a pin test,
> §10). Also unprobed: whether an optional-typed free variable
> (emitter would stamp 15; `DecodeRepr` clamps to `kUnknown`) is
> reachable at all, and whether `Repr::kEnum` has any producer.

## 3. The calling convention

Every runtime kernel and every host trampoline shares one shape — the
**uniform slot-out convention**:

```
void cel_<op>_at_v*(uint32_t out_slot, uint32_t arg_slot, ...)
```

All parameters are i32 linear-memory byte offsets of 24-byte CelValue
cells; the callee reads operands at the argument offsets and writes
the result CelValue at `out_slot`. No wasm return value. Same-slot
aliasing (`out_slot == arg_slot`) is legal everywhere; callees read
operands before writing `out` (test-pinned, `Layer2AliasingTest`).

The catalogue encodes the convention as data
(`abi/runtime_catalogue.proto`): `num_args` is the exact i32 param
count — no out-slot semantics layered on. `returns_i32` is **false
for every `cel_host` / `cel_env` trampoline** (enforced by
`runtime_catalogue_test.cc`) and true only for these catalogued
exceptions (the `uint32_t`-returning marker-exported helpers):

| helper | returns | meaning of the i32 |
|---|---|---|
| `cel.arena_alloc(n)` | offset | allocated bytes' absolute offset; 0 = OOM |
| `cel.cel_list_arena_view(list_slot)` | slot | arena-view slot for iteration (falls back to the source slot) |
| `cel.cel_map_count(map_slot)` | count | entry count (0 on poison/OOM) |
| `cel.cel_map_iter_init(map_slot)` | handle | iteration state offset; 0 = empty/poisoned/OOM |
| `cel.cel_map_iter_next(handle)` | bool | 1 = advanced, 0 = exhausted |

Two further sanctioned deviations live in codegen: ternary `_?_:_`
lowers as an inline `BinaryenIf`; `dyn(x)`'s identity arm forwards
its operand's storage with no call (`01-compiler.md`). Corollary:
**spec-level errors travel in-wire** as `CEL_ERROR` values in
`out_slot`; a non-OK `absl::Status` from a trampoline means
*infrastructure failure* → wasm trap (`02-evaluator.md` §4/§5).

## 4. The memory map

![Linear memory](diagrams/memory-map.svg)

One shared linear memory serves both link modes. The runtime
**defines and exports** it (`(memory 4 1024 shared)` observed); in
dynamic mode the expr module imports it as `cel.memory`, in static
mode the adopted module owns it outright. The host never creates
it — it clones the export after instantiation
(`engine.cc::BindHelpersInstance`).

### 4.1 The canonical region table

Authoritative constants: `runtime/cel_layout.h` and
`compiler/memory_layout.h` (`MemoryLayout`), `static_assert`-tied.

| region | bytes | owner / content |
|---|---|---|
| `[0, 8)` | 8 | reserved null sentinel — makes "offset 0 == absent" well-defined runtime-wide |
| `[8, 16)` | 8 | reserved legacy arena-cursor slot; dead (arena state moved to BSS), kept so `rodata_base` stays 16 |
| `[16, rodata_end)` | layout-dependent | expr rodata: one 24-byte CelValue frame per `kConst`, span payload bytes trailing each frame, 8-aligned cursor (`compiler/codegen/static_memory_builder.cc`). Offsets are absolute — no relocation arithmetic in emitted wasm |
| `[workspace_base, +workspace_bytes)` | layout-dependent | workspace: `workspace_base = RoundUp16(rodata_end)`; 32-byte slots (§4.2) — referenced-variable slots first, then `SlotAllocator` scratch |
| guard band | 256 | `MemoryLayout::kGuardBytes` — slack the layout gate (§4.4) refuses to let workspace enter; catches the next allocator off-by-one before it spills |
| `[8192, __heap_base)` | build-dependent | runtime statics + wasi-libc shadow stack; pinned above the line by `-Wl,--global-base=8192` (`runtime/BUILD.bazel`) |
| `[__heap_base, …)` | grows | dlmalloc heap: the chained per-Instance arena (§5), the activation buffer (malloc'd, deliberately outside the arena), plan-lifetime objects |

The boundary constant is `CELWASM_RESERVED_LOW_MEMORY_BYTES = 8192`
(`cel_layout.h:37`) — the **only** region the expression module may
write. A write at or past byte 8192 lands in wasi-libc's address
space: no trap, just corrupted malloc bookkeeping and a delayed
death inside an unrelated helper (`memory_layout.h:10-20`).

### 4.2 Workspace slots: 32-byte stride, 16-aligned

Workspace cells stride at **`SlotAllocator::kSlotStride = 32`**, not
24 (`slot_allocator.h`; mirrored as `MemoryLayout::kSlotStride` with
a tying `static_assert`). The CelValue stays 24 bytes; the trailing
8 bytes are pad no codepath touches. The stride exists because the
runtime is wasm32-wasi-threads: libc internals can emit
`memory.atomic.*` ops, which require 16-byte alignment — a 24-byte
stride leaves every other cell 8-aligned only. Hence also
`workspace_base = RoundUp16(...)` and the allocator's
16-aligned-base CHECK (16 suffices; rodata packs immediately ahead).

Variable slots (one per referenced variable except comprehension iter
vars, whose `slot_offset` stays 0 — their wasm local is a moving
pointer) and scratch slots share the stride
(`layout_pass.cc::ReserveVariableSlots`). The scratch allocator is a
**LIFO free-list recycler**: `Release` returns a cell for reuse and
`peak_slots()` is true peak liveness; `debug_layout = true` flips it
to bump-only for layout dumps (`slot_allocator.cc`). Older "24B
cells" / no-op-Release comments predate this; the `.cc` wins.

### 4.3 The page-count table

Four page numbers appear; consistent, related by ≤, different jobs:

| number | where | meaning |
|---|---|---|
| 2 pages (128 KiB) | `CELWASM_INITIAL_MEMORY_PAGES` (`cel_layout.h:29`); A13 check (`engine.cc::EnforceRuntimeMemoryInvariants`) | host-side **floor**: observed pages must be `>=` 2; not the actual size |
| 2 pages default | dynamic-mode `cel.memory` import min = `PagesForBytes(mem_size_bytes)` (`compile.cc`), from `CompilerOptions::mem_size_bytes` default 128 KiB | import minimum the expr module declares; must be ≤ the provided memory's size |
| 3–4 pages | wasm-ld auto-sizing of `cel_runtime.wasm` | actual initial size; varies by build mode and linked libraries |
| 1024 pages (64 MiB) | `MemoryLayout::kMaxMemoryBytes`; the export's max; `kSharedMaxPages` on the import | hard growth ceiling, shared by export and import declarations |

> **Open question (V8):** a dynamic-mode `mem_size_bytes` above
> 256 KiB stamps an import minimum larger than the runtime's
> exported memory and plausibly fails instantiation at Plan; under
> default static mode the knob is a verified no-op. Probe pending;
> the knob may be deleted end-to-end.

### 4.4 The static-region gates

Three layered gates plus CHECK tripwires enforce the §4.1 invariant.
Historically nothing bounded the workspace, and oversized
expressions silently overwrote runtime statics — the true root cause
of the "unaligned atomic" trap and the 10K-literal-list wasmtime
panic (`04-runtime.md` §7).

1. **LayoutPass slot-exhaustion gate** (`layout_pass.cc`):
   `workspace_bytes` must not exceed
   `MemoryLayout::MaxWorkspaceBytes(rodata_base, rodata_size)` — the
   headroom left in `[16, 8192)` after rodata and the 256-byte guard
   band. Violation → `ResourceExhausted`
   (`kSlotExhaustedMessagePrefix`). The cap is dynamic in rodata
   (no constants: ~7.9 KiB of workspace; 3 KiB of string constants:
   ~4.9 KiB). Both link modes — LayoutPass is mode-blind.
2. **`ValidateExprStaticRegion`** (`compiler/internal/compile.cc`,
   from `RunFrontAndLayout`, both modes): rejects
   `workspace_base + workspace_bytes > 8192`, `ResourceExhausted`.
   Belt over the layout gate at the facade seam; skipped only when
   `LayoutOptions::rodata_base_override != 0` (a caller relocating
   the region owns its own budget — the seam reserved for
   bundled-library rodata banding; no production caller today).
3. **`ValidateAbiSlotExtents`** (`eval/engine.cc`, Plan time):
   rejects (`InvalidArgument`) any decoded `cel.abi` whose variable
   slot `slot_offset + sizeof(CelValue)` extends past 8192. The
   compiler never emits such a slot — a Program claiming one is
   corrupt, stale, or hand-crafted, and honoring it would have the
   activation marshal write over runtime statics. Plan is the
   earliest stage that sees the ABI: fail once, not per Eval.

Tripwires: `InstallExprRodataSegment` carries an
`ABSL_CHECK_LE(rodata_end, 8192)` annotated "gate regressed"
(`compile.cc`); at Plan the A13/A14 `ABSL_CHECK`s verify the page
floor and `__heap_base >= 8192`
(`engine.cc::EnforceRuntimeMemoryInvariants`); A15 (activation
buffer at or above 8192) guards the marshal side (`instance.cc`).

The gates are status errors, not CHECKs: region size is
embedder-input-dependent (a big literal list is legitimate CEL), and
embedder input must never crash the process. Compile-time rejection
is the *current* honest behavior, not the end state — a relocatable
or growable static region is recorded future work (`04-runtime.md`).

## 5. The arena

The per-Eval allocator behind every kernel intermediate
(`runtime/cel_arena.{h,c}`). As merged it is a **chained-chunk
grow-on-demand** bump arena; the fixed-capacity single-buffer arena
older docs (and the notes) describe is gone.

- **State is a BSS struct** (`g_arena`: head/tail chunk pointers,
  `total_used`, `total_cap`, `initialized`) — NOT at fixed memory
  offsets; the pre-WASI cursor slots at bytes 8/12 are dead (§4.1).
- **`arena_init(cap_bytes)`** — once per Instance, host-called via
  wasm reentry at Plan with `CELWASM_ARENA_CAPACITY_BYTES` (64 KiB);
  mallocs the first chunk from dlmalloc. Same-capacity re-init is
  idempotent; different-size traps (A16: host-owned lifecycle).
- **`arena_alloc(n)`** — bump-allocates `RoundUp8(n)` zero-filled
  bytes from the tail chunk; returns the **absolute linear-memory
  offset**, or 0 on OOM. On overflow the wasm build mallocs a new
  chunk sized `pick_grow_size(prev, need)`: double the previous
  chunk, clamped to `[CEL_ARENA_MIN_GROW_BYTES = 4 KiB,
  CEL_ARENA_MAX_GROW_BYTES = 1 MiB]`, floored at the triggering
  request. malloc failure returns 0 — **OOM is a value**
  (`CEL_ERR_OVERFLOW` at every kernel consumer; audit in
  `notes/runtime-kernel.md` §1.7). Alloc before init traps. The
  bounds check is subtraction-form (`need > capacity - cursor`); the
  additive form wraps for near-`UINT32_MAX` requests
  (regression-pinned). **A9:** `arena_alloc(0)` returns a *valid*
  8-byte slot — never 0 — so offset 0 stays unambiguous.
- **`arena_reset()`** — codegen-emitted as `$eval`'s first
  instruction — frees every chunk except the first and rewinds its
  cursor: O(extra-chunks), no per-Eval malloc churn when the initial
  sizing suffices. Reset before init is a harmless no-op.
- **The native twin does not chain** (no shared memory to grow
  into): first chunk carved from `g_memory[16…]`, OOM terminal. Wasm
  returns malloc'd absolute offsets; native returns `16 + local_off`
  — both satisfy `cel_mem_base() + ret == the allocated bytes`, the
  dual-build contract every kernel unit test relies on.
- **Relocation discipline:** `memory.grow` may move the linear-memory
  base; re-derive every raw pointer after any `arena_alloc`/`malloc`
  (`cel_3vl.c`'s merge is the canonical pattern; authoring rule, not
  mechanically checked — `04-runtime.md` §5).

Export split: `arena_alloc` / `arena_reset` are codegen imports;
`arena_init` / `arena_capacity` / `arena_cursor` are
host-reentry-only exports (`wasm_exports.txt` `[host-only]`). The
diagnostics pair drifted with the chaining: `arena_cursor()` reports
the *first chunk's* cursor (back-compat), `arena_capacity()` the
chain total, which changes across Evals.

> **Open question (V44):** the host-side `WasmtimeArenaAllocator`
> (`cel_host_wasmtime.cc::Alloc`) maps an offset-0 return — and any
> trap/error — to `nullptr`, while `cel_host.h`'s `ArenaAllocator`
> contract promises a valid pointer for zero-byte allocs. Under A9
> the wasm side never returns 0 for `n == 0`, so the divergence
> should be unreachable; no live path has been probed.

## 6. The `cel.abi` custom section

One serialized `celwasm.abi.CelAbi` proto (`abi/cel_abi.proto`) in a
wasm custom section named `"cel.abi"`. Producer:
`compile.cc::AttachCelAbiSection` → `BuildCelAbi`
(`abi/cel_abi_emit.cc`); consumer:
`abi_decode.cc::DecodeCelAbiFromWasm`, first in `Engine::Plan`
(raw-bytes walk; no wasmtime state). Emit and decode agree **by
construction** — both link the same generated proto; the only
hand-rolled wire code is the decode side's custom-section framing
walk (the evaluator cannot link Binaryen). Tolerance: `NotFound` (no
section) → empty abi; variable-free Eval works, the link-mode label
goes unvalidated, synthetic WAT fixtures load. `InvalidArgument`
(bad magic / wasm version ≠ 1 / truncated LEB128 / section overrun /
parse failure) propagates.

### 6.1 Field-by-field

| # | field | emitted from | consumed by |
|---|---|---|---|
| 1 | `version` — schema version of the proto itself, constant 1 (`cel_abi_emit.cc::kCelAbiVersion`) | always | **nobody** (V43 below) |
| 2 | `variables[]` — `VariableEntry{name, local_index, slot_offset, repr}`, free variables only (comp-scope locals skipped, `EmitVariables`) | `StaticLayout::variables` | activation marshal (`instance.cc`), iterating linearly by name |
| 3 | `fields[]` — `FieldEntry{id, field_number, name, owner_fqn}`; row 0 = sentinel | codegen's `FieldRefRow` span | `cel_get_field`/`cel_set_field` trampolines via `BuildCelHostBindings` |
| 4 | `attributes[]` — `AttributeEntry{id, variable, qualifiers[]}`; row 0 = sentinel | `layout.attributes` | partial-eval unknown-pattern matcher |
| 5 | `types[]` — `TypeEntry{id, fully_qualified_name}`; row 0 = sentinel | `layout.message_types` | Plan-time FQN → `Descriptor*` resolution for `cel_make_message` |
| 6 | `runtime_abi_version` — `abi::kRuntimeAbiVersion` (currently 2) | constant | `CheckRuntimeAbiVersion` at Plan |
| 7 | `link_mode` — `LINK_MODE_DYNAMIC = 0` / `LINK_MODE_STATIC = 1` | the compile arm taken | `ValidateLinkModeLabel` — validation only, never routing |

Wire-design facts, one telling each. **Sentinel row 0 everywhere:**
`id == 0` means "no id"; the emitter writes a real placeholder row 0
so host tables index 1:1 with the ids codegen burned into the wasm
(`FieldEntry.field_number == 0`, not `id == 0`, is the separate "not
proto-resolvable" marker). **Minimal wire:** only the numeric `repr`
crosses for variables — `repr` alone picks the host encoder; a full
`CelType` is a reserved additive slot; no `CheckedExpr` on the wire.
**Attribute granularity is field-path only, by design:** `.field`
selects extend `qualifiers`, `[k]`/`[i]` never do, and
`AttributePattern::Parse` rejects bracket qualifiers rather than
accept patterns it cannot honor (`cel_abi.proto:130-169` carries the
full produce/propagate model, and the FieldEntry-vs-AttributeEntry
split).

> **Open question (V7/R30):** `cel_abi.proto`'s comment claims
> `variables[]` is positional by `local_index`, but the emitter
> skips comp-scope locals while ResolvePass interleaves them in the
> same dense index space — a free variable first referenced inside a
> comprehension should break the claim. Consumers iterate by name —
> no runtime bug. Probe: `xs.map(i, i + y)`, check
> `variables(1).local_index()`.

### 6.2 The two versions, and the never-enforced one

**`CelAbi.version` (field 1)** is the proto message's own schema
version, constant 1 since inception. **`runtime_abi_version`
(field 6)** is the helper-catalogue version (§7) — the one actually
**enforced**, by `CheckRuntimeAbiVersion`
(`abi/runtime_catalogue.cc`): `prog_v == engine_v` → OK;
`prog_v == 0` AND an empty surface (no variables / fields /
attributes / types) → OK, so hand-written fixtures need no stamped
version; `prog_v == 0` with a non-empty surface →
`FailedPrecondition` "predates ABI versioning; recompile"; otherwise
→ `FailedPrecondition` naming **both** versions. Hard rejection is
deliberate: the alternative is wasmtime's opaque type-mismatch trap
at first call into a renamed helper.

> **Open question (V43):** `CelAbi.version` is read by no non-test
> code (grep-verified); "schema version exists and is never checked"
> was never recorded as deliberate policy. Probe: a section with
> `version = 99` plus one variable — Plan will load it. Decide:
> tolerate-forever, or reject unknown majors at Plan.

### 6.3 The link-mode label

`LinkMode` is an enum, not a bool, so future modes land additively.
`LINK_MODE_DYNAMIC = 0` is load-bearing: pre-label Programs carry no
field-7 tag and decode (proto3 default) to the shape they actually
have — dynamic-mode sections are **byte-identical** to legacy ones
(byte-pinned by `cel_abi_emit_test.cc`). The label is metadata +
tripwire, never routing: the engine routes on import introspection
(`is_static` ⇔ zero `cel.*` imports, `module_imports.cc`);
`ValidateLinkModeLabel` (`engine.cc`) rejects a label/shape
contradiction with `FailedPrecondition`; unknown future enum values
skip validation (open wire set).

## 7. The runtime catalogue & import namespaces

`abi/runtime_catalogue.{h,cc}` is the single source of truth for
every wasm import an expr module may declare; the entry type is the
**generated proto** `celwasm::abi::CelRuntimeFunction`
(`{name, module, num_args, returns_i32}`) — no hand-defined POD.

<!-- diagram-wanted: import-namespace map — expr module in the center,
     four arrows out (cel → cel_runtime.wasm exports; cel_host /
     cel_env → wasmtime trampolines; cel_fn → user registrations),
     annotated with the static-mode collapse (cel arrow disappears,
     kernel inlined) and the 12-name runtime-side allowlist subset -->

### 7.1 The four namespaces

| module | contents | catalogued? |
|---|---|---|
| `cel` | pure-wasm helpers exported by `cel_runtime.wasm` (kernels, dispatchers, arena, iteration) | yes — **derived** from `// cel:codegen-export` markers on the C declarations; membership from the marker, arity/return shape from the `void`/`uint32_t` C signature (clang lowers it 1:1 to the wasm type). `//bazel:gen_runtime_catalogue` |
| `cel_host` | wasmtime host trampolines (field access, aggregate ops, proto construction, WKT) | yes — hand-maintained rows in `abi/runtime_host_env.textproto` (20 rows; no C export to derive from), guarded by the startup bijection CHECK in `cel_host_wasmtime.cc` (trampolines ⊆ catalogue AND catalogue ⊆ trampolines) |
| `cel_env` | host environment helpers — today only `cel_log` | yes — hand-maintained (1 row); **not** covered by the cel_host bijection check (drift note below) |
| `cel_fn` | user custom-fn implementations | **no, by design** — `FindBuiltinHelper(kCelFn, …)` returns nullptr; arities come from per-compile registration. Open set |

The composed textproto (hand-maintained host/env rows + appended
derived `cel` rows) is embedded as a string literal —
`CelRuntimeHelpers()` is a pure in-process call. Cross-namespace
name collisions are intentional and test-pinned (`cel.cel_list_at`,
the kDynamic dispatcher, tail-calls `cel_host.cel_list_at`); lookups
are `(module, name)`-keyed. Linker-export-list mechanics, and the
deleted-as-tautological consistency test, are `04-runtime.md` §6
(residual audit: V42).

### 7.2 The 12-vs-20 import-set table

Two correct counts for two artifacts — a subset, not a conflict:

| set | size | contents |
|---|---|---|
| `cel_host` catalogue = what an **expr module** may import | 20 | `cel_get_field`, `cel_has_field`, `cel_map_lookup`, `cel_map_iter_open`, `cel_list_iter_open`, `cel_list_at`, `cel_list_size`, `cel_list_in`, `cel_list_eq`, `cel_list_concat`, `cel_map_size`, `cel_map_in`, `cel_map_eq`, `cel_message_eq`, `cel_make_message`, `cel_set_field`, `resolve_message_type_name`, `cel_timestamp_tz_accessor`, `cel_wkt_unwrap_time`, `cel_wkt_unwrap_wrapper` |
| `runtime/wasm_imports.txt` = what **cel_runtime.wasm itself** imports | 12 (+ `cel_log`) | the kDynamic dispatcher tail-call targets plus resolve/tz: `cel_map_lookup`, `cel_list_at`, `cel_list_size`, `cel_list_in`, `cel_list_eq`, `cel_list_concat`, `cel_map_size`, `cel_map_in`, `cel_map_eq`, `cel_message_eq`, `resolve_message_type_name`, `cel_timestamp_tz_accessor` |

The other 8 catalogue rows are imported only by expr modules, never
by the kernel. A static-mode Program retains its `cel_host.*` /
`cel_env.*` imports while importing nothing from `"cel"` — the host
boundary does not collapse with the link mode (V12).

Observed drift, recorded rather than silently inherited: the
`cel_env.cel_log` catalogue row says `num_args: 4`, but both the C
import declaration (`runtime/cel_log.h:48-50`) and the wasmtime
registration (`cel_log.cc::CelLogType`) are **9×i32 → void**
(file/fn spans, line, fmt span, argv ptr+count; argv slots are
16 bytes: u32 tag, u32 pad, u64 payload). No consumer of the env
row's arity was found, and the bijection check does not cover
`cel_env` — latent, but the row is wrong as written; fix it and
extend the cross-check to the env namespace.

## 8. Errors & unknowns on the wire — THE FORK SECTION

> **Status update (2026-06-10):** all three forks are RESOLVED and
> FIXED. §8.1 (errors) — the bare-code wire was crowned and the
> divergent readers fixed (V4). §8.3 (3VL precedence) — the
> error-dominates rule was oracle-confirmed for STRICT ops and the
> losing host implementation aligned (V3); the LOGIC ops carry the
> opposite, oracle-confirmed UNKNOWN-over-ERROR rule (scope note in
> §8.3). §8.2 (unknown `payload.unk` contract, V2) — the
> **descriptor-offset contract is crowned AND shipped end-to-end**:
> every host writer mints descriptors, both host decoders
> dereference them, `Value::Unknown` carries the full attribute-id
> set, and `absorb_3vl_binary` merges both-unknown operands.
> Resolution details are inline in each subsection.

This section documents the per-layer behavior (docs 02 §8 and 04 §3
defer here).

### 8.1 Errors: the bare-code wire, per layer

**Verified:** the on-wire error is `kind = CEL_ERROR`,
`payload.err = CEL_ERR_*` — a bare u32 code (§2 rule 4); every
kernel (`poison`) and host writer (`WriteWireError`) emits it.

| layer | behavior on the error path | citation |
|---|---|---|
| host → wasm encode | `EncodeValue`'s kError arm writes ONLY `WireErrorCode(code)`; **`ErrorPayload.message` and `expr_id` are discarded** (cleanup-backlog #31). Every Layer-1 message (`FieldNotFound(name)`, the out-of-bounds range text, …) dies here. `WireErrorCode` maps all 14 named `ErrorCode` values (it used to collapse kDuplicateKey/kUnknownType/kCustomFnFailed/kTimeout to TYPE_MISMATCH) and passes an out-of-enum numeric through unchanged | `eval/internal/cel_host.cc`, kError arm of `EncodeValue`; `cel_host_error.cc::WireErrorCode` |
| wasm → host decode (Instance) | `DecodeCelError` synthesizes a generic message from the code (`ErrorCodeName`); its switch covers every named `ErrorCode` **including `kInvalidArgument`** (the arm it historically omitted, so wire 18 used to surface as `kHostAdapterError` / "runtime error code 18"); an unrecognized wire byte degrades to `kHostAdapterError` / "runtime error code N". The full code matrix is pinned by `instance_test.cc::ErrorCodeRoundTripTest` (one case per named code + the out-of-enum byte) | `eval/instance.cc::DecodeCelError` |
| wasm → host decode (host-call ctx) | `DecodeWireError` — same arm set as its Instance sibling; the two decoders agree | `eval/host_call_context.cc` |
| in-guest formatting | `cel_log`'s `%v` error formatter reads the production bare-code wire: `payload.err` IS the `CEL_ERR_*` code, rendered as `error(code=N)`. (It previously implemented a never-shipped descriptor-struct shape — `payload.err` as an offset to a 16-byte `(code, msg_ptr, msg_len, pad)` struct — rendering garbage on real errors, with tests pinning fixture memory in the dead shape) | `eval/host/cel_log.cc::FormatError`; `cel_log_test.cc::ValueErrorKindBareCode` |
| host-callback trap path | a non-OK Status from an embedder callback becomes a wasm trap; the message survives, the status CODE is lost | `eval/engine.cc`, `TrapFromStatus`; V19 |

> **RESOLVED (V4, 2026-06-10):** the bare-code wire was crowned and
> the readers were fixed: `%v` rewritten to the bare-code shape,
> `DecodeCelError` gained the missing `kInvalidArgument` arm (wire 18
> now decodes as `kInvalidArgument` / "invalid_argument" — example 08
> and the examples smoke assertion updated with it), and the
> encode-side `WireErrorCode` gap the exhaustive round-trip test
> exposed (4 named codes collapsing to TYPE_MISMATCH) was closed,
> with `CEL_ERR_UNKNOWN_TYPE/CUSTOM_FN_FAILED/TIMEOUT` appended to
> `cel_data.h` in lockstep per §10. The message-carrying
> `{code, expr_id, msg_off, msg_len}` wire upgrade was NOT taken;
> message loss remains the documented contract (cleanup-backlog #31
> stays open for that decision).

### 8.2 Unknowns: the descriptor-offset contract — SHIPPED

**The one contract, every layer:** `CEL_UNKNOWN`'s `payload.unk` is
a u32 byte offset to a 2-word `{ids_off, len}` **UnknownSet
descriptor**; `ids_off` points at a contiguous u32 array of
attribute ids in sorted, deduplicated order. `payload.unk == 0` is
the legal empty set (no recorded provenance) — production writers
never mint it. Real attribute ids are intern ids in `[1, N]` (row 0
of `cel.abi.attributes[]` is the no-attribute sentinel,
`abi/cel_abi.proto:99-101`); the function-origin sentinel
`kFunctionUnknownSentinel = 0xFFFFFFFF` travels inside a descriptor
like any other id.

| layer | behavior | citation |
|---|---|---|
| kernel merge | `cel_unknown_merge` dereferences both descriptors and mints a fresh sorted-deduped union in the bump arena; OOM → `CEL_ERR_OVERFLOW` | `runtime/cel_3vl.c::merge_unknown_descriptors` |
| strict-op absorption | `absorb_3vl_binary` routes both-UNKNOWN operands through `cel_unknown_merge` (after the error scan) — neither side's provenance drops | `runtime/cel_internal.h`; `cel_arith_test.cc::BothUnknownMergesAttributeIdSets` |
| host writers | all mint descriptors via `EncodeUnknownSet` (`eval/internal/cel_host.{h,cc}`): the `cel_get_field` trampoline on a FULL pattern match (`RunFieldPrelude`) and `HostCallContext::{ReturnUnknown,ReturnValue}` allocate in the **guest bump arena** (in-eval; survives until the next $eval's `arena_reset`); the activation marshal (`EncodeUnknownVariable`, `eval/instance.cc`) allocates in the **activation buffer** — the marshal runs BEFORE $eval, whose prelude resets the arena, so an arena-minted descriptor would be zero-filled (the same lifetime argument as string payload bytes) | `eval/internal/cel_host.cc`; `eval/host_call_context.cc`; `eval/instance.cc` |
| host readers | both decoders (`Instance::Eval`/`PartialEval` result decode and the host-call arg decode) dereference `{ids_off, len}` and surface EVERY id | `eval/instance.cc::DecodeUnknownSetAt`; `eval/host_call_context.cc::DecodeUnknownSet` |
| user surface | `Value::Unknown` holds the attribute-id SET (sorted, deduped): `Unknown(vector<AttributeId>)` + `UnknownAttributes()` span accessor; the single-id `UnknownAttribute()` works for one-element sets and returns FailedPrecondition on merged sets (silently picking a winner would hide provenance) | `eval/value.{h,cc}` |
| in-guest formatting | `cel_log`'s `%v` unknown formatter dereferences the descriptor (unchanged — it was already on the winning shape) | `eval/host/cel_log.cc::FormatUnknown` |

> **RESOLVED + FIXED (V2, 2026-06-10).** History: the host side
> shipped a conflicting "raw attribute id in `payload.unk`"
> interpretation. The probe demonstrated it live — `PartialEval` of
> `a && b` with both bare variables FULL-matched returned `kUnknown`
> with `AttributeId{360296}`, an arena descriptor offset misdecoded
> as an attribute id (silent garbage, no trap), and strict ops
> left-biased (`a + b` both-unknown dropped `b`'s identity). The
> reference demands a merged SET: cel-cpp routes both-unknown
> operands through `AttributeUtility::MergeUnknowns` (cel-cpp
> `eval/eval/logic_step.cc`, `attribute_utility.cc:107-130`; strict
> fns merge after the error scan, `function_step.cc:219`;
> `AttributeSet::Merge` is a sorted-set union), oracle-pinned by
> `testdata/cel_cpp_oracle_unknown_payload_test.cc` (and/or/add ×
> dotted/bare → both identities; same attribute both sides
> deduplicates). The descriptor contract won — it is the only shape
> that can carry the merged set — and the 5-point fix shipped as one
> unit: host writers mint descriptors (`EncodeUnknownSet`), both
> decoders dereference, `Value::Unknown` grew the set surface,
> `absorb_3vl_binary` merges, and the false comments
> (`cel_3vl.c`/`cel_3vl.h` "host mints `payload.unk == 0`",
> `attribute.h` "0-based ids") were corrected. E2E pins:
> `e2e/m2_partial_eval_test.cc::MergedUnknownProvenanceTest` (a&&b /
> a||b / a+b both-unknown decode BOTH identities; dedup;
> single-unknown regression; dotted `a.age && b.age`), both link
> modes.
>
> Known residual (recorded, out of V2 scope): a custom host fn
> called with SEVERAL unknown args propagates the FIRST arg's set
> un-merged (`eval/engine.cc::AbsorbUnknownOrErrorArg`) where
> cel-cpp's function_step would union them; each arg's own set
> survives intact. Provenance granularity at a `.field` select is
> the OPERAND's interned attribute (bare root), so `c.age && c.name`
> both-unknown dedupes to one id where cel-cpp reports two dotted
> paths.

### 8.3 3VL precedence: TWO rules — per op class

**Scope note (settled 2026-06-10):** the precedence between UNKNOWN
and ERROR operands is **per-op-class**, not global:

  - **STRICT ops** (arithmetic, comparisons, every dispatched
    function): **ERROR dominates UNKNOWN** across operands,
    left-bias within each class; both-UNKNOWN merges.
  - **LOGIC ops** (`_&&_`, `_||_`): the absorbing bool dominates
    everything (`false && X = false`, `true || X = true`), then
    **UNKNOWN dominates ERROR** — cel-cpp's
    `LogicalOpStep::Calculate` (cel-cpp `eval/eval/logic_step.cc`)
    merges unknowns BEFORE scanning for errors, because the
    resolved unknown may later short-circuit the error away.
    Oracle-pinned both orders for both ops
    (`UnknownPayloadOracle.{And,Or}{UnknownLeftErrorRight,
    ErrorLeftUnknownRight}IsUnknown`,
    `testdata/cel_cpp_oracle_unknown_payload_test.cc`), plus the
    absorbing-bool controls (`false && (1/0==1)` → false,
    `true || (1/0==1)` → true, `unknown && false` → false).
    Kernel aligned in `runtime/cel_3vl.c::{cel_and,cel_or}`
    (truth-table pinned in `cel_3vl_test.cc`; e2e in
    `e2e/m5_test.cc::ControlFlowUnknownErrorPrecedenceE2ETest`).

For a strict operation over an (unknown, error) operand pair, all
three absorption implementations carry the same precedence rule:
**ERROR dominates UNKNOWN across operands**, left-bias within each
class. The rule is cel-cpp's: `NoOverloadResult`
(cel-cpp `eval/eval/function_step.cc:202-223`) scans the args for an
`ErrorValue` and returns the first one found BEFORE merging unknowns;
langdef §"Evaluation" leaves multi-error propagation order
unspecified, so the reference implementation's behavior is the
contract.

| layer | rule | citation |
|---|---|---|
| runtime kernel `absorb_3vl_binary` (strict ops) | error dominates (both ERROR checks precede both UNKNOWN checks), left-bias within each class; both-UNKNOWN merges via `cel_unknown_merge` (§8.2) | `runtime/cel_internal.h`; pinned both orders in `cel_arith_test.cc::{UnknownLeftErrorRightPropagatesError, ErrorLeftUnknownRightPropagatesError}` |
| cel_host trampolines `AbsorbBinary` (strict ops) | error dominates — aligned to the kernel (it was first-operand-wins: UNKNOWN(a) beat ERROR(b)) | `eval/internal/cel_host_error.cc`; pinned both orders in `cel_host_error_test.cc::AbsorbBinaryTest` |
| host-call trampoline `AbsorbUnknownOrErrorArg` (strict: custom fns) | error dominates (scans all args with an explicit `!have_error` guard on the unknown arm) | `eval/engine.cc` |
| runtime kernel `cel_and` / `cel_or` (logic ops) | absorbing bool > UNKNOWN (merged) > ERROR, left-bias within each class | `runtime/cel_3vl.c`; `cel_3vl_test.cc` 4×4 matrices |

> **RESOLVED (V3, 2026-06-10):** the probe ran exactly as specified —
> `testdata/cel_cpp_oracle_test.cc` gained
> `PartialEvalOracle.{UnknownPlusErrorIsError,ErrorPlusUnknownIsError}`
> (`x + (1/0)` and `(1/0) + x` with `x` attribute-unknown through
> `PartialEvalWithCelCpp`): cel-cpp returns the ERROR in **both**
> orders. The losing host-side `AbsorbBinary` was aligned;
> `cel_host_error.h`'s self-contradicting comment was rewritten. The
> kernel and `eval/engine.cc` were already correct and are unchanged.
> Conformance held at 1966 in both modes. (The logic-op
> UNKNOWN-over-ERROR scope note above was settled later the same
> day, when the §8.2 probe surfaced the divergence; conformance held
> at 1973/0 both modes through that fix.)

## 9. The component boundary (WIT vocabulary)

Foreign Component-Model functions speak a parallel, typed contract —
not the CelValue slot ABI. Two layers:

**The shared dynamic vocabulary** — `abi/wit/cel.wit`
(`package cel:value@0.2.0`): the complete CEL value model as a WIT
`resource value`. WIT forbids recursive variants, so aggregates nest
through handles; proto messages cross as
`record message {type-name, wire: list<u8>}` (serialized bytes,
never a handle); map keys are restricted via
`variant map-key {bool, int, uint, string}`. A `custom-fn` interface
(`invoke(name, args) -> result<value, eval-error>`) and two worlds
complete it. Reserved for the dynamic/variadic path; no first-party
caller dispatches through it.

**The common concretely-typed path** — per-function typed WIT plus
the canonical-ABI lift/lower bridge in
`eval/internal/cel_component.{h,cc}`; the authoritative per-CEL-type
mapping table is `cel_component.h:12-34`. Highlights: bytes ↔
`list<u8>`; duration/timestamp ↔ a `{seconds, nanos}` record;
`map<K,V>` ↔ `list<tuple<K,V>>`; `proto(fqn)` ↔
`SerializePartialToString` bytes re-materialised via the descriptor
pool; `optional<T>` permanently rejected both directions; kType
lifts as a type-name string (its Lower arm is an Unimplemented
stub — unreachable: kType is rejected at library Build). 3VL
absorption is the **caller's** job: lift/lower never see
Error/Unknown (`02-evaluator.md` §9). Naming seam: the wasm import
stays snake_case (`cel_fn.<overload_id>`); the Component-Model
identifier grammar rejects snake_case, so the engine kebab-cases
consumer-side (`OverloadIdToKebab`).

## 10. Change discipline

**Bumps `runtime_abi_version`** (currently 2): renaming/removing a
helper, changing an arity, a return shape, or a namespace. **Does
not bump:** adding a helper, adding a proto field, appending a
`LinkMode` / `CelKind` / `CEL_ERR_*` / `Repr` value. Append-only
enums, and what enforces each:

| surface | rule | enforcement today |
|---|---|---|
| `CelKind` | append at tail, never renumber | comment + downstream fixtures; no static pin |
| `CEL_ERR_*` ↔ `ErrorCode` | append in lockstep, stay cast-equivalent | comment ("MUST mirror"); `cel_host_error_test.cc` enumerates the mapping |
| `ir::Repr` (wire via `VariableEntry.repr`) | values stable on wire | **nothing** — the V6 gap. Implicit numbering, no `= N`, no pin test; a mid-enum insertion silently desynchronizes old Programs from new engines *without* tripping `runtime_abi_version`. Fix: explicit initializers + per-member `EXPECT_EQ(static_cast<uint32_t>(...))` |
| `LinkMode` (×3: public option, internal option, proto) | additive; forwarded by blind `static_cast` | **nothing locks them together** (V25) — a value added to one enum only would miscompile silently; a static_assert pin is pending |
| layout constants | compiler/runtime parity | `static_assert`s in `compiler/memory_layout.h` tie `MemoryLayout` to the `CELWASM_*` macros; `slot_allocator.h` ties `kSlotStride` |

Known un-tied seams (gaps a wire change can fall through): codegen
hand-copies a few wire literals (`CEL_BOOL = 1`, `CEL_INT = 2`, the
24-byte stride) into `expr_lower.cc` instead of including
`cel_data.h` — a CelValue layout perturbation would pass
`//compiler/codegen/...` green (the V11 magic-number probe); the
built wasm's export section vs the catalogue has no automated check
(V42); the `cel_env` rows sit outside the cel_host bijection CHECK
(§7.2's `cel_log` arity drift is the live instance).

Byte-compat pins that must keep passing: dynamic-mode `cel.abi`
sections serialize with no field-7 tag; legacy bytes decode as
DYNAMIC; unknown `LinkMode` values parse and survive
re-serialization (`cel_abi_emit_test.cc`); the empty-surface
carve-out keeps versionless synthetic fixtures loading.

**The §8.2 unknown wire is settled contract** (descriptor offset,
shipped 2026-06-10 as one coordinated unit — host writers mint
descriptors via `EncodeUnknownSet`, both readers dereference,
`Value::Unknown` carries the set, `absorb_3vl_binary` merges).
Any new unknown producer/consumer MUST speak the descriptor shape;
a raw id in `payload.unk` re-opens the fork. (§8.1 and §8.3 were
resolved 2026-06-10 with the V3/V4 evidence recorded inline; the
error wire and both 3VL precedence rules are settled contract.)

## History

Supersedes, for wire and memory content:
`doc/implementation-plan/rewrite/cel-host-surface.md` (wire sections;
the message-carrying error wire it specifies never shipped),
`…/memory-layout-design.md` (its bounded-layout promise shipped late
and differently — §4.4; its fixed 64 KiB arena and 24-byte workspace
cells are superseded by §5 and §4.2), and `…/abi-refactor.md` (its
hand-written `AbiHelper` POD was replaced by the generated proto;
its consistency test was deleted as tautological, §7).

Source notes: `doc/design/notes/{abi-shared,runtime-kernel,
codegen-memory,eval-internal,90-abi-memory-consistency,
91-contract-coherence}.md`, code-verified 2026-06-10. Where the
notes describe the pre-merge tree — fixed-capacity arena, no-op
`SlotAllocator::Release`, 24-byte workspace cells, no workspace
gate — this doc is the corrected telling, re-verified against
`runtime/cel_arena.c`,
`compiler/codegen/{slot_allocator,layout_pass}.cc`,
`compiler/memory_layout.h`, `compiler/internal/compile.cc`, and
`eval/engine.cc` as merged.
