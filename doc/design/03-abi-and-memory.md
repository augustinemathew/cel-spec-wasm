# ABI & memory — the wire contracts

One telling for every byte-level fact the compiler, the runtime kernel, and the evaluator must agree on. `00-architecture.md` names the contracts; docs 01/02/04 describe the machinery on each side and defer every byte here.

## 1. CelValue & kinds

The unit of data exchange everywhere is the 24-byte `CelValue` (`runtime/cel_data.h`, `struct CelValue`): **u32 `kind` at +0, u32 `_pad` (zero) at +4, a 16-byte `payload` union at +8.** `_Static_assert(sizeof(CelValue) == 24)` pins the size (`cel_data.h:183`). Payload "pointers" are u32 byte offsets into the one shared linear memory; **offset 0 is the universal absent sentinel** (arena OOM, absent value, empty iterator handle, empty span). A little-endian host is a build requirement (`#error` guard): host↔wasm CelValue transfer is bitwise memcpy.

### 1.1 The CelKind table

Values are **wire-stable, append-only, never renumbered** (`cel_data.h:26-30`):

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
| 15 | `CEL_UNKNOWN` | `unk` (u32 — UnknownSet descriptor offset, [08 §3.2](08-abi-wire-format.md#32-unknowns-the-descriptor-offset-contract)) |
| 16 | `CEL_ERROR` | `err` (u32, a `CEL_ERR_*` code, [08 §3.1](08-abi-wire-format.md#31-errors-the-bare-code-wire-per-layer)) |
| 17 | `CEL_LIST_HOST` | `ref_slot` (u32, externref table) |
| 18 | `CEL_IP` | `net_ref` (u32 arena offset → `NetIp`) |
| 19 | `CEL_CIDR` | `net_ref` (u32 arena offset → `NetCidr`) |

`CEL_TYPE` has no intern table — equality is `memcmp` on the payload span bytes. The host-backed kinds (9, 10, 17) carry slots into three *independent* host-side externref namespaces (map / message / list; `ExternrefTable`, `eval/internal/cel_host.h`), each with slot 0 as the nullptr sentinel and a per-Eval `Reset()`.

### 1.2 Aggregate headers & entry strides

Arena aggregates are a 16-byte header plus a contiguous entry run (`cel_data.h`):

- `ArenaListHeader {count, capacity, elements_offset, _pad}` — 16 B; element stride **`kCelListEntryStride = 24`** = `sizeof(CelValue)` (`_Static_assert`-tied, `cel_data.h:199`).
- `ArenaMapHeader {count, capacity, entries_offset, _pad}` — 16 B; entries are back-to-back `{key, val}` CelValue pairs, stride **`kCelMapEntryStride = 48`**.

The host map-iteration snapshot written by `cel_host.cel_map_iter_open` uses the same 48 B/entry layout; `MapIterState` is a 16-byte arena struct (`cel_runtime.c`). Entry runs stride at 24 bytes (arena/rodata); workspace slots stride at 32 (§4.2).

## 2. The four kind-like enums

Four enums carry "what kind of value is this?" — distinct numberings **by design**. All conversion is by explicit switch, never a cast, with one sanctioned cast-equivalence (rule 4).

| enum | home | job | numbering |
|---|---|---|---|
| `CelKind` | `runtime/cel_data.h` | the wire — CelValue.kind | §1.1; 0–19, append-only |
| `ir::Repr` | `compiler/ir/annotations.h` | per-node ABI repr; also the wire value of `cel.abi.VariableEntry.repr` | implicit: `kUnknown=0, kNull=1, kBool=2, kInt=3 … kType=14, kOptional=15` |
| `CelType::Kind` | `shared/type.h` | public declaration vocabulary | `kUnknown=0, kBool=1 … kMessage=9`, **gap at 10**, `kDuration=11, kTimestamp=12, kType=13` |
| `Value::Kind` | `eval/value.h` | decoded-result kind | `kNull=0 … kMap=8, kMessage=9`, gap at 10, `kDuration=11, kTimestamp=12, kType=13, kUnknown=14, kError=15` |

Alignment rules every cross-enum conversion site must honor:

1. **`CelKind` ≠ `Value::Kind` from value 9 up.** They agree on 0–8; at 9 the wire has `CEL_MAP_HOST` while `Value::Kind` has `kMessage`. A `static_cast` miscodes silently; conversion lives in the decode switches (`eval/instance.cc`, `host_call_context.cc`).
2. **`ir::Repr` matches nothing.** `kNull = 1` shifts every scalar by one relative to the wire (`Repr::kInt = 3` vs `CEL_INT = 2`); `kEnum = 11` has no CelKind twin. Repr exists only in compiler IR and `cel.abi.variables[].repr`; `DecodeRepr` (`abi_decode.cc:110-146`) is the one wire→enum map, clamping out-of-range and `kOptional` (15, no decode arm) to `kUnknown`, on which the activation marshal later fails loudly.
3. **`CelType::Kind` tracks `Value::Kind`** (same gap at 10, same `kType = 13`), deliberately narrower: no Null / Optional / Error / Unknown factories.
4. **The single sanctioned cast-equivalence:** `celwasm::ErrorCode` (`eval/error.h`) ↔ the wire `CEL_ERR_*` codes. The numerics MUST mirror 1:1 (`eval/error.h:19-22`); both append-only. The encode direction (`WireErrorCode`, `cel_host_error.cc:62-87`) is nonetheless a guarded switch: codes with no arm clamp to `CEL_ERR_TYPE_MISMATCH`.

!!! note "Open question (V6, V14/V15)"
    `ir::Repr` uses implicit enum numbering while `cel_abi.proto:48-50` promises wire stability; nothing pins the values — a mid-enum insertion would silently renumber every emitted `repr` without tripping `runtime_abi_version` (fix: explicit initializers + a pin test, [08 §5](08-abi-wire-format.md#5-change-discipline)). Also unprobed: whether an optional-typed free variable is reachable at all, and whether `Repr::kEnum` has any producer.

## 3. The calling convention

Every runtime kernel and every host trampoline shares one shape — the **uniform slot-out convention**:

```
void cel_<op>_at_v*(uint32_t out_slot, uint32_t arg_slot, ...)
```

All parameters are i32 linear-memory byte offsets of 24-byte CelValue cells; the callee reads operands at the argument offsets and writes the result at `out_slot`. No wasm return value. Same-slot aliasing (`out_slot == arg_slot`) is legal everywhere; callees read operands before writing `out` (test-pinned, `Layer2AliasingTest`).

The catalogue encodes the convention as data (`abi/runtime_catalogue.proto`): `num_args` is the exact i32 param count. `returns_i32` is **false for every `cel_host` / `cel_env` trampoline** (enforced by `runtime_catalogue_test.cc`) and true only for these catalogued exceptions:

| helper | returns | meaning of the i32 |
|---|---|---|
| `cel.arena_alloc(n)` | offset | allocated bytes' absolute offset; 0 = OOM |
| `cel.cel_list_arena_view(list_slot)` | slot | arena-view slot for iteration (falls back to the source slot) |
| `cel.cel_map_count(map_slot)` | count | entry count (0 on poison/OOM) |
| `cel.cel_map_iter_init(map_slot)` | handle | iteration state offset; 0 = empty/poisoned/OOM |
| `cel.cel_map_iter_next(handle)` | bool | 1 = advanced, 0 = exhausted |

Two further sanctioned deviations live in codegen: ternary `_?_:_` lowers as an inline `BinaryenIf`; `dyn(x)`'s identity arm forwards its operand's storage with no call (`01-compiler.md`). Corollary: **spec-level errors travel in-wire** as `CEL_ERROR` values in `out_slot`; a non-OK `absl::Status` from a trampoline means infrastructure failure → wasm trap (`02-evaluator.md` §4/§5).

## 4. The memory map

![Linear memory](diagrams/memory-map-light.svg#only-light)
![Linear memory](diagrams/memory-map-dark.svg#only-dark)

One shared linear memory serves both link modes. The runtime **defines and exports** it (`(memory 4 1024 shared)` observed); in dynamic mode the expr module imports it as `cel.memory`, in static mode the adopted module owns it outright. The host never creates it — it clones the export after instantiation (`engine.cc::BindHelpersInstance`).

### 4.1 The canonical region table

Authoritative constants: `runtime/cel_layout.h` and `compiler/memory_layout.h` (`MemoryLayout`), `static_assert`-tied.

| region | bytes | owner / content |
|---|---|---|
| `[0, 8)` | 8 | reserved null sentinel — makes "offset 0 == absent" well-defined runtime-wide |
| `[8, 16)` | 8 | reserved legacy arena-cursor slot; dead (arena state moved to BSS), kept so `rodata_base` stays 16 |
| `[16, rodata_end)` | layout-dependent | expr rodata: one 24-byte CelValue frame per `kConst`, span payload bytes trailing each frame, 8-aligned cursor (`compiler/codegen/static_memory_builder.cc`). Offsets are absolute — no relocation arithmetic in emitted wasm |
| `[workspace_base, +workspace_bytes)` | layout-dependent | workspace: `workspace_base = RoundUp16(rodata_end)`; 32-byte slots (§4.2) — referenced-variable slots first, then `SlotAllocator` scratch |
| guard band | 256 | `MemoryLayout::kGuardBytes` — slack the layout gate (§4.4) refuses to let workspace enter; catches the next allocator off-by-one before it spills |
| `[8192, __heap_base)` | build-dependent | runtime statics + wasi-libc shadow stack; pinned above the line by `-Wl,--global-base=8192` (`runtime/BUILD.bazel`) |
| `[__heap_base, …)` | grows | dlmalloc heap: the chained per-Instance arena (§5), the activation buffer (malloc'd, deliberately outside the arena), plan-lifetime objects |

The boundary constant is `CELWASM_RESERVED_LOW_MEMORY_BYTES = 8192` (`cel_layout.h:37`) — the **only** region the expression module may write. A write at or past byte 8192 lands in wasi-libc's address space: no trap, just corrupted malloc bookkeeping and a delayed death inside an unrelated helper (`memory_layout.h:10-20`).

### 4.2 Workspace slots: 32-byte stride, 16-aligned

Workspace cells stride at **`SlotAllocator::kSlotStride = 32`**, not 24 (`slot_allocator.h`; mirrored as `MemoryLayout::kSlotStride` with a tying `static_assert`). The CelValue stays 24 bytes; the trailing 8 are pad. The stride exists because the runtime is wasm32-wasi-threads: libc internals can emit `memory.atomic.*` ops, which require 16-byte alignment — a 24-byte stride leaves every other cell 8-aligned only. Hence also `workspace_base = RoundUp16(...)` and the allocator's 16-aligned-base CHECK.

Variable slots (one per referenced variable except comprehension iter vars, whose `slot_offset` stays 0 — their wasm local is a moving pointer) and scratch slots share the stride (`layout_pass.cc::ReserveVariableSlots`). The scratch allocator is a **LIFO free-list recycler**: `Release` returns a cell for reuse and `peak_slots()` is true peak liveness; `debug_layout = true` flips it to bump-only for layout dumps (`slot_allocator.cc`).

### 4.3 The page-count table

Four page numbers appear; consistent, related by ≤, different jobs:

| number | where | meaning |
|---|---|---|
| 2 pages (128 KiB) | `CELWASM_INITIAL_MEMORY_PAGES` (`cel_layout.h:29`); A13 check (`engine.cc::EnforceRuntimeMemoryInvariants`) | host-side **floor**: observed pages must be `>=` 2; not the actual size |
| 2 pages default | dynamic-mode `cel.memory` import min = `PagesForBytes(mem_size_bytes)` (`compile.cc`), from `CompilerOptions::mem_size_bytes` default 128 KiB | import minimum the expr module declares; must be ≤ the provided memory's size |
| 3–4 pages | wasm-ld auto-sizing of `cel_runtime.wasm` | actual initial size; varies by build mode and linked libraries |
| 1024 pages (64 MiB) | `MemoryLayout::kMaxMemoryBytes`; the export's max; `kSharedMaxPages` on the import | hard growth ceiling, shared by export and import declarations |

!!! note "Open question (V8)"
    A dynamic-mode `mem_size_bytes` above 256 KiB stamps an import minimum larger than the runtime's exported memory and plausibly fails instantiation at Plan; under default static mode the knob is a verified no-op. Probe pending; the knob may be deleted.

### 4.4 The static-region gates

Three layered gates plus CHECK tripwires enforce the §4.1 invariant. Before they existed, oversized expressions silently overwrote runtime statics — the true root cause of the "unaligned atomic" trap and the 10K-literal-list wasmtime panic (`04-runtime.md` §7).

1. **LayoutPass slot-exhaustion gate** (`layout_pass.cc`): `workspace_bytes` must not exceed `MemoryLayout::MaxWorkspaceBytes(rodata_base, rodata_size)` — the headroom left in `[16, 8192)` after rodata and the 256-byte guard band. Violation → `ResourceExhausted` (`kSlotExhaustedMessagePrefix`). The cap is dynamic in rodata (no constants: ~7.9 KiB of workspace; 3 KiB of string constants: ~4.9 KiB). Both link modes — LayoutPass is mode-blind.
2. **`ValidateExprStaticRegion`** (`compiler/internal/compile.cc`, from `RunFrontAndLayout`, both modes): rejects `workspace_base + workspace_bytes > 8192`, `ResourceExhausted`. Belt over the layout gate at the facade seam; skipped only when `LayoutOptions::rodata_base_override != 0` (a caller relocating the region owns its own budget; no production caller today).
3. **`ValidateAbiSlotExtents`** (`eval/engine.cc`, Plan time): rejects (`InvalidArgument`) any decoded `cel.abi` whose variable slot `slot_offset + sizeof(CelValue)` extends past 8192. The compiler never emits such a slot — a Program claiming one is corrupt, stale, or hand-crafted, and honoring it would have the marshal write over runtime statics. Plan is the earliest stage that sees the ABI: fail once, not per Eval.

Tripwires: `InstallExprRodataSegment` carries an `ABSL_CHECK_LE(rodata_end, 8192)` annotated "gate regressed" (`compile.cc`); at Plan the A13/A14 `ABSL_CHECK`s verify the page floor and `__heap_base >= 8192` (`engine.cc::EnforceRuntimeMemoryInvariants`); A15 (activation buffer at or above 8192) guards the marshal side (`instance.cc`).

The gates are status errors, not CHECKs: region size is embedder-input-dependent (a big literal list is legitimate CEL), and embedder input must never crash the process. A relocatable or growable static region is recorded future work (`04-runtime.md`).

## 5. The arena

The per-Eval allocator behind every kernel intermediate (`runtime/cel_arena.{h,c}`): a **chained-chunk grow-on-demand** bump arena.

- **State is a BSS struct** (`g_arena`: head/tail chunk pointers, `total_used`, `total_cap`, `initialized`) — not at fixed memory offsets; the pre-WASI cursor slots at bytes 8/12 are dead (§4.1).
- **`arena_init(cap_bytes)`** — once per Instance, host-called via wasm reentry at Plan with `CELWASM_ARENA_CAPACITY_BYTES` (64 KiB); mallocs the first chunk from dlmalloc. Same-capacity re-init is idempotent; different-size traps (A16: host-owned lifecycle).
- **`arena_alloc(n)`** — bump-allocates `RoundUp8(n)` zero-filled bytes from the tail chunk; returns the **absolute linear-memory offset**, or 0 on OOM. On overflow the wasm build mallocs a new chunk sized `pick_grow_size(prev, need)`: double the previous chunk, clamped to `[CEL_ARENA_MIN_GROW_BYTES = 4 KiB, CEL_ARENA_MAX_GROW_BYTES = 1 MiB]`, floored at the triggering request. malloc failure returns 0 — **OOM is a value** (`CEL_ERR_OVERFLOW` at every kernel consumer; audit in `notes/runtime-kernel.md` §1.7). Alloc before init traps. The bounds check is subtraction-form (`need > capacity - cursor`); the additive form wraps for near-`UINT32_MAX` requests (regression-pinned). **A9:** `arena_alloc(0)` returns a *valid* 8-byte slot — never 0 — so offset 0 stays unambiguous.
- **`arena_reset()`** — codegen-emitted as `$eval`'s first instruction — frees every chunk except the first and rewinds its cursor: O(extra-chunks), no per-Eval malloc churn when the initial sizing suffices. Reset before init is a harmless no-op.
- **The native twin does not chain** (no shared memory to grow into): first chunk carved from `g_memory[16…]`, OOM terminal. Wasm returns malloc'd absolute offsets; native returns `16 + local_off` — both satisfy `cel_mem_base() + ret == the allocated bytes`, the dual-build contract every kernel unit test relies on.
- **Relocation discipline:** `memory.grow` may move the linear-memory base; re-derive every raw pointer after any `arena_alloc`/`malloc` (`cel_3vl.c`'s merge is the canonical pattern; authoring rule, not mechanically checked — `04-runtime.md` §5).

Export split: `arena_alloc` / `arena_reset` are codegen imports; `arena_init` / `arena_capacity` / `arena_cursor` are host-reentry-only exports (`wasm_exports.txt` `[host-only]`). `arena_cursor()` reports the *first chunk's* cursor (back-compat); `arena_capacity()` the chain total, which changes across Evals.

!!! note "Open question (V44)"
    The host-side `WasmtimeArenaAllocator` (`cel_host_wasmtime.cc::Alloc`) maps an offset-0 return — and any trap/error — to `nullptr`, while `cel_host.h`'s `ArenaAllocator` contract promises a valid pointer for zero-byte allocs. Under A9 the wasm side never returns 0 for `n == 0`, so the divergence should be unreachable; no live path has been probed.

## 6. Where the wire formats live

The `cel.abi` descriptor, the runtime import catalogue, and the error/unknown wire contracts are [`08-abi-wire-format.md`](08-abi-wire-format.md).
