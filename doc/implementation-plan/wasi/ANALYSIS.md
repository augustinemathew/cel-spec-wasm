# Migrating compiler_v2 to WASI + `malloc`

Status: comprehensive analysis — drafted 2026-05-17.  Companion
to `../exp1_re2/` (which proved wasi-sdk + libraries-in-wasm
works end-to-end with 150 KB gzipped RE2 + 11 WASI imports).

## 1 Goal

Replace the bump arena
(`compiler_v2/runtime/cel_arena.{c,h}`) with wasi-libc's
`malloc` / `free`, switch the runtime build from
`--target=wasm32 -nostdlib` to wasi-sdk's `--target=wasm32-wasi`,
and align the host/wasm boundary on standard C library
allocation semantics — while **preserving per-Instance memory
isolation** (the user's hard requirement: each program
instantiation owns its own memory; instances do not share).

Why:

  - **Enables vendoring C/C++ libraries** (RE2, abseil, future
    parsers / formatters / regex / Unicode tables) without
    the dual-allocator integration tax called out in
    `../WASI_AND_PORTABILITY.md §4`.
  - **Removes the host-string-arena hack**
    (`api/instance.cc::EnsureHostStringArenaCapacity` and
    ~110 LoC of related plumbing) that exists *because* the
    bump arena rewinds on every eval, clobbering activation
    strings.
  - **Simplifies codegen** by removing the `cel_reset`
    prologue, the `arena_base` compile-time constant
    threading, and `mem_size_bytes` in `LoweringOptions`.
  - **Matches the user's prior compiler's memory model**
    where each compiled expression instance owned its own
    wasm memory and used `malloc` natively.

## 2 Current architecture — exhaustive inventory

This section is the catalogue.  Every function, every call
site, every file the migration touches.  Numbers are exact
line counts as of HEAD on 2026-05-17.

### 2.1 The arena module (`runtime/cel_arena.{c,h}`) — 105 LoC

Functions defined here:

| Function | Lines | Disposition |
|---|---:|---|
| `cel_reset(arena_base, arena_limit)` | 5 | **DELETED** |
| `cel_alloc(n)` | 11 | **DELETED** (callers move to `malloc`) |
| `cel_value_at(off)` | 5 | **KEPT** (becomes a thin macro/inline; just `(CelValue*)(mem_base + off)`) |
| `load_u32(off)` (static) | 3 | **DELETED** (cursor slot gone) |
| `store_u32(off, v)` (static) | 3 | **DELETED** (cursor slot gone) |
| `align_up(n, align)` (static) | 3 | **DELETED** (malloc handles alignment) |

Module-level state removed:
  - `kBumpOffset = 8`, `kLimitOffset = 12` enums — the fixed
    memory offsets for the arena cursor.
  - Inline-asm opacity barrier in `cel_memory.c` — load-bearing
    under `-nostdlib` because clang's wasm backend treated a
    cast-from-0 pointer as UB; **wasi-sdk's malloc-managed
    memory removes the need entirely**.

### 2.2 Memory accessors (`runtime/cel_memory.{c,h}`) — 104 LoC

| Function | Lines | Disposition |
|---|---:|---|
| `cel_memory_base_()` (wasm path) | 5 | **DROP inline-asm hack**; just return wasi-libc's view of memory base.  Or inline-replace with `__builtin_wasm_memory_base()` equivalent. |
| `cel_memory_size_()` (wasm path) | 4 | **DELETE in wasm** (wasi-libc's `__wasm_memory_size(0)` tracks this); KEEP in native build. |
| `cel_memory_base_()` (native path) | 3 | **KEEP** (host tests still need a backing buffer). |
| `cel_memory_size_()` (native path) | 3 | **KEEP** for native tests. |
| `cel_mem_base()` / `cel_mem_size()` | 6 | **KEEP** (public ABI for host tests). |
| `_Alignas(8) static uint8_t g_memory[…]` (native path) | 1 | **KEEP** — native test backing buffer. |

Net `cel_memory.c`: 68 LoC → ~30 LoC (−38).

### 2.3 Runtime kernel callers of `cel_alloc` — 107 references

Spread across these `.c` files in `compiler_v2/runtime/`:

| File | `cel_alloc` calls | Notes |
|---|---:|---|
| `cel_runtime.c` | 5 | Arena-list/map header + entry allocations |
| `cel_3vl.c` | 2 | Unknown-merge descriptor + id arrays |
| `cel_make.c` | 2 | CelValue + payload byte allocs |
| `cel_string_ops.c` | 1 | `concat` result payload |
| `cel_convert.c` | 1 | `int_to_string` etc. result payload |
| `cel_type.c` | 1 | `type()` result string payload |

Plus 95 occurrences in `_test.cc` files and comments.

**Migration**: each call site is mechanical —
`uint32_t off = cel_alloc(N)` becomes `uint32_t off =
(uint32_t)(uintptr_t)malloc(N)`.  The function signature on
the runtime kernel doesn't change; only the implementation
moves from bump to malloc.

Caveat: callers today treat **`cel_alloc` returning 0 as
OOM**.  `malloc` returning `NULL` is the natural equivalent;
no code change at call sites if we use the same convention.

### 2.4 Test surface — 21 test files, 55+ `cel_alloc` invocations

Every file under `compiler_v2/runtime/*_test.cc` that
exercises a kernel touches `cel_alloc` and `cel_reset`
directly.  Tests typically have:

```cpp
void SetUp() override {
  cel_reset(kArenaBase, kArenaLimit);
}
```

Migration: tests' `SetUp()` either becomes a no-op (Option A:
per-Instance malloc lifetime) or `mspace_create_with_base(...)`
(Option B: per-eval mspace).  Tests then use `malloc` /
`mspace_malloc` directly in place of `cel_alloc`.

Test files affected (with `cel_alloc` count):
  - `cel_runtime_wasm_test.cc` (13)
  - `cel_arena_test.cc` (11) — **entire test target deleted**
  - `cel_3vl_test.cc` (6)
  - `cel_arith_test.cc` (5)
  - `cel_compare_test.cc` (4), `cel_type_test.cc` (4)
  - `cel_aggregate_arena_test.cc` (3), `cel_convert_test.cc` (3)
  - `cel_string_ops_test.cc` (2), `cel_make_test.cc` (2)
  - `cel_time_test.cc` (1), `cel_list_test.cc` (1),
    `cel_map_test.cc` (1)

### 2.5 Codegen — explicit emit points

#### 2.5.1 `compiler_v2/codegen/expr_lower.cc`

| Lines | Construct | Disposition |
|---|---|---|
| 102-114 | `EmitCelResetCall` function | **DELETE** (12 LoC) |
| 117-130 | Comments explaining the cel_reset prologue | **DELETE** (14 LoC) |
| 1856-1861 | Inline doc block on `cel_reset` ordering | **DELETE** (6 LoC) |
| 1879 | Call site `EmitCelResetCall(mod, layout.arena_base, opts.mem_size_bytes)` | **DELETE** (1 LoC) |

Replacement: the eval prologue calls `malloc` once for the
workspace region (see §4.2 design) and stores the offset in
a wasm local; the epilogue calls `free` on the same local.
Net new lines: ~20.

#### 2.5.2 `compiler_v2/codegen/expr_lower.h`

| Lines | Construct | Disposition |
|---|---|---|
| 42 | `inline constexpr absl::string_view kCelResetInternalName = "cel_reset"` | **DELETE** |
| 137-148 | `LoweringOptions::mem_size_bytes` field | **DELETE** (12 LoC) |
| 164-167 | Inline doc snippet showing the `cel_reset` call | **UPDATE** to show the new prologue |
| 18 | Comment referencing `cel.cel_reset` function import | **DELETE** |
| 39 | Comment referencing kCelResetInternalName | **DELETE** |
| 7-12 | File header preamble naming cel_reset | **UPDATE** |

#### 2.5.3 `compiler_v2/codegen/layout_pass.{h,cc}`

| Lines | Construct | Disposition |
|---|---|---|
| `.h:67` | Comment about arena cursor at bytes 8/16 | **DELETE** |
| `.h:73-77` | Comment about `[arena_base..)` region | **REPLACE** with new memory map (§3 of this doc) |
| `.h:105` | `uint32_t arena_base = 0` field | **DELETE** |
| `.cc:390` | `layout.arena_base = RoundUp8(workspace_base + workspace_bytes)` | **DELETE** |

#### 2.5.4 `compiler_v2/codegen/module.{h,cc}`

| Lines | Construct | Disposition |
|---|---|---|
| `module.h:5-15` | Comments naming cel_reset / cel_alloc | **UPDATE** |
| `module.h:80-94` | `AddMemoryImport(...)` API | **DELETE** (90% likely — under the new architecture expr modules import memory FROM the runtime module which now owns it via wasi-sdk's default export, see §3) |
| `module.h:63-67` | `SetMemory(...)` API | **KEEP**.  Codegen still calls this to define + export memory in the runtime path, but the expr-module path changes |

#### 2.5.5 `compiler_v2/compile.cc`

| Lines | Construct | Disposition |
|---|---|---|
| 300 | `mod.AddFunctionImport(kCelResetInternalName, "cel", "cel_reset", reset_params, none)` | **DELETE** |
| 303 | `mod.AddFunctionImport("cel_alloc", "cel", "cel_alloc", alloc_params, i32)` | **REPLACE** with `mod.AddFunctionImport("malloc", "cel", "malloc", malloc_params, i32)` and `mod.AddFunctionImport("free", "cel", "free", free_params, none)` |

#### 2.5.6 `compiler_v2/compile.h`

| Lines | Construct | Disposition |
|---|---|---|
| 11-12 | File header text about cel_reset/cel_alloc imports | **UPDATE** to malloc/free |
| 44-49 | `LoweringOptions.mem_size_bytes` reference | **DELETE** |

#### 2.5.7 `compiler_v2/tools/wat_runner/wat_runner.{cc,h}`

| Lines | Construct | Disposition |
|---|---|---|
| `.cc:34-35` | `"cel_reset", "cel_alloc"` entries in the runtime export bind list | **REPLACE** with `"malloc"`, `"free"` |
| `.cc:714` | Comment ("M1 baseline: cel_reset + cel_alloc") | **UPDATE** |
| `.cc:751` | Comment ("cel_reset runs on top of whatever we wrote") | **UPDATE** to reflect malloc semantics |
| `.h:19-20` | Doc comment naming cel_reset/cel_alloc | **UPDATE** |

### 2.6 Codegen tests

| File | Affected lines | Disposition |
|---|---:|---|
| `codegen/expr_lower_test.cc` | ~50 references | **UPDATE** — fixtures install `malloc`/`free` imports instead of `cel_reset`/`cel_alloc`; expected-emission checks become "expect call to malloc not cel_reset" |
| `codegen/module_test.cc` | line 160 — `m.AddFunctionImport("cel_reset", ...)` | **UPDATE** |
| `codegen/layout_pass_test.cc` | ~20 references | **UPDATE** — remove arena_base assertions |
| `codegen/expr_lower_test.cc:145` / 154 / 289 / 490 | Specific `cel_reset` references | **UPDATE** |

### 2.7 Host integration

#### 2.7.1 `compiler_v2/api/engine.cc`

| Lines | Construct | Disposition |
|---|---|---|
| 76-103 | `InitStoreAndMemory` allocates 2-page host-owned memory | **MAJOR REWRITE**: host stops allocating memory; memory comes from the runtime module's exported memory (wasi-sdk default).  See §3.3. |
| 138-156 | `BindRuntimeExport` helper | **KEEP** |
| 165-227 | `kRuntimeExports[]` array — entries `"cel_reset", "cel_alloc"` | **DELETE** these two entries; add `"malloc"`, `"free"` |
| 250 | `BindAllRuntimeExports` loop | unchanged |
| 264-294 | `InstantiateRuntime` — `wasmtime_instance_export_get(cel_alloc)` lookup | **REPLACE** lookup target with `"malloc"` |
| 296-330 | `InstantiateExpr` | **UPDATE** — expr module imports memory FROM runtime, not from a separately-allocated host memory.  See §3.3. |
| 67-68, 83-84, 173, 275 | Comments referencing cel_reset/cel_alloc | **UPDATE** |

Net change: ~80 LoC restructured (mostly comments + a few
real wiring changes), no Plan/Instance API change.

#### 2.7.2 `compiler_v2/api/instance.cc`

| Lines | Construct | Disposition |
|---|---|---|
| 340-450 | `EnsureHostStringArenaCapacity` + helpers — the **host string arena** that lives above `arena_limit` because cel_reset would clobber it | **DELETE WHOLESALE** (~110 LoC).  Under malloc, activation marshalling calls `malloc(len)` via wasm reentry to get a fresh region; that region survives until the host explicitly frees it. |
| 416-450 | `EncodeStringOrBytes` + `HostStringArena` struct | **REPLACE** with simpler `malloc`-based encoder (~30 LoC) |
| 93-130 | `DecodeArenaListAt` / `DecodeArenaMapAt` | unchanged — these read CelValue bytes from memory regardless of allocator origin |
| 312 | Comment about "per-Eval arena lifetime" | **UPDATE** to mention malloc/free lifecycle |

#### 2.7.3 `compiler_v2/api/internal/instance_impl.h`

| Lines | Construct | Disposition |
|---|---|---|
| 38-48 | `host_string_arena_floor` / `host_string_arena_capacity` fields | **DELETE** |
| 19-29 | `wasmtime_memory_t memory{}` field | **KEEP** but its initialization moves: it's no longer host-allocated; it comes from `wasmtime_instance_export_get(runtime_instance, "memory", ...)`. |
| 31-36 | `host_env` field | **KEEP**; `host_env.cel_alloc_fn` → `host_env.malloc_fn` rename |

#### 2.7.4 `compiler_v2/api/internal/cel_host_wasmtime.{cc,h}`

| Lines | Construct | Disposition |
|---|---|---|
| 27-50 | `HostExternrefTable` | unchanged |
| 155-180 | `WasmtimeArenaAllocator::Alloc` — calls `wasmtime_func_call(cel_alloc_fn)` | **RENAME** to `WasmtimeMallocAllocator`; calls `wasmtime_func_call(malloc_fn)` instead.  Same signature, same return semantics (NULL/0 → OOM). |
| ~8 trampoline call sites | `WasmtimeArenaAllocator alloc(ctx, env->cel_alloc_fn, env->memory)` | mechanical rename |

Net: ~30 LoC changed, no semantic shift; the allocator
abstraction holds.

#### 2.7.5 `compiler_v2/api/internal/cel_host.{cc,h}`

The Layer-2 implementations (3,215 LoC) take an `Allocator&`
parameter that they call into for output payloads.  **Zero
changes** — the abstraction over the allocator survives the
migration.

#### 2.7.6 `compiler_v2/bench/kernel_bench.cc`

| Lines | Construct | Disposition |
|---|---|---|
| 59 | `cel_reset(16u, cel_mem_size())` in setup | **REPLACE** with `malloc`-based scratch alloc |
| 64, 361, 363, 377-378, 550, 568 | `cel_alloc(...)` in bench inner loops | **REPLACE** with `malloc` |

### 2.8 Build system

#### 2.8.1 `MODULE.bazel`

Today:
  - `http_archive(binaryen)` — pinned tarball
  - `http_archive(wasmtime_darwin_arm64)` — pinned tarball

New:
  - `http_archive(wasi_sdk_darwin_arm64)` — wasi-sdk 25 release
  - `http_archive(wasi_sdk_darwin_x86_64)` — same, x86_64
  - `http_archive(wasi_sdk_linux_x86_64)` — same, linux
  - `http_archive(wasi_sdk_linux_arm64)` — same, linux arm
  - Platform select rule: pick the right archive based on
    `@platforms//os` × `@platforms//cpu`.

Estimated +40 LoC in MODULE.bazel.

#### 2.8.2 `compiler_v2/runtime/BUILD.bazel`

Today, the `cel_runtime_wasm_file` genrule uses:

```
/opt/homebrew/opt/llvm/bin/clang
--target=wasm32
-ffreestanding -nostdlib
-O3 -flto -mtail-call
-Wl,--no-entry
-Xlinker --import-memory=cel,memory
-Wl,--export=cel_alloc
-Wl,--export=cel_reset
...30 more --export lines
```

Migration:

```diff
- /opt/homebrew/opt/llvm/bin/clang
+ $(execpath @wasi_sdk//:bin/clang)
- --target=wasm32
+ --target=wasm32-wasi
- -ffreestanding
- -nostdlib
+ # wasi-sdk provides libc + dlmalloc; default startup is fine
+ -nostartfiles
+ -Wl,--no-entry
- -Xlinker --import-memory=cel,memory
+ # runtime now OWNS the memory and EXPORTS it for expr_module to import
+ -Wl,--export-memory
- -Wl,--export=cel_alloc
- -Wl,--export=cel_reset
+ -Wl,--export=malloc
+ -Wl,--export=free
  # Keep all the other --export lines (kernels stay exported).
```

Brew's `/opt/homebrew/opt/llvm/bin/clang` goes away entirely —
that path was the source of much pain (`compile_commands.json`
references it 63 times; CI machines with a different brew
prefix have to massage it).

Net change: ~10 lines.

#### 2.8.3 `scripts/lint.sh` + `scripts/refresh_compile_db.sh`

These scripts assume `compile_commands.json` references brew's
clang.  After migration, the runtime's compile_commands paths
shift to point at wasi-sdk's clang.  This affects:
  - The PCH cache (`.lint-cache/lint_pch.h.pch`) is keyed on
    the clang version; switching wasi-sdk's clang 19 (vs
    brew's clang 21) invalidates the PCH.  First run after
    migration recompiles the PCH (~10s).
  - clang-tidy on runtime `.c` files now sees wasi-libc
    headers, which has different macros and built-in
    functions.  Most lints are still relevant; a small
    handful of `cppcoreguidelines-*` checks may fire
    differently on wasi-libc's `assert.h` macros.  Spot-fix
    expected; not a blocker.

Net change: 0 LoC, possible 0.5 day of lint fix-up post-migration.

#### 2.8.4 `compile_commands.json`

Auto-regenerated by `scripts/refresh_compile_db.sh` from
bazel's `aquery` output.  No manual edit.

### 2.9 Documentation

| File | Lines mentioning cel_reset/cel_alloc/arena | Disposition |
|---|---:|---|
| `doc/wasm-compiler-design.md` | 18 | **UPDATE** §8.2 memory layout, §6 ABI, §6.1 codegen prologue |
| `doc/implementation-plan/rewrite/design.md` | ~12 | **UPDATE** |
| `doc/implementation-plan/rewrite/cel-runtime-c-split-plan.md` | mentions cel_arena.c as P3 | **UPDATE** epilogue to note the migration superseded P3 |
| `doc/implementation-plan/rewrite/m1-scalar-pipeline.md` | ~3 | **UPDATE** |
| `doc/implementation-plan/rewrite/m2-ident-select-unknowns.md` | ~5 | **UPDATE** memory map |
| `doc/implementation-plan/rewrite/m3-map-literals.md` | refs arena_map | **UPDATE** if behaviour shifts |
| `compiler_v2/conformance/README.md` | mentions arena lifetime in a couple places | **UPDATE** |
| `CLAUDE.md` | mentions wasm32 cross-compilation via brew llvm | **UPDATE** to wasi-sdk |
| WAT traces (`doc/implementation-plan/rewrite/wat/*.wat`) | ~30 files reference `cel_reset` / `cel_alloc` | **UPDATE** every one (mechanical) |

Estimated ~150 lines of docs need touching.

### 2.10 Net code change

| Bucket | Removed | Added | Net |
|---|---:|---:|---:|
| `cel_arena.{c,h}` | 105 | 0 | −105 |
| `cel_memory.{c,h}` simplification | 50 | 20 | −30 |
| Runtime kernel call sites (mechanical rename) | 0 | 0 | 0 |
| Runtime test files (mechanical) | 0 | 0 | 0 |
| `cel_arena_test.cc` (deleted) | ~150 | 0 | −150 |
| Codegen `EmitCelResetCall` + scaffolding | 35 | 0 | −35 |
| Codegen new malloc/free prologue + epilogue | 0 | 45 | +45 |
| LayoutPass `arena_base` | 12 | 0 | −12 |
| `LoweringOptions::mem_size_bytes` | 15 | 0 | −15 |
| Codegen tests | ~50 changed in place | — | 0 |
| Host string arena (`instance.cc`) | 110 | 30 | −80 |
| `WasmtimeArenaAllocator` rename | 0 | 0 | 0 |
| Trampoline allocator threading | 0 | 0 | 0 (same Allocator& shape) |
| `engine.cc` memory-init rewrite | 30 | 25 | −5 |
| `instance_impl.h` fields | 8 | 0 | −8 |
| `kernel_bench.cc` | 5 | 5 | 0 |
| `MODULE.bazel` wasi-sdk archives | 0 | 40 | +40 |
| `runtime/BUILD.bazel` flag swap | 5 | 5 | 0 |
| `tools/wat_runner/wat_runner.cc` | 2 | 2 | 0 |
| Docs | ~150 changed in place | — | 0 |
| **Total** | **577** | **172** | **−405 LoC** |

The net is bigger than the previous estimate because this
analysis caught the `cel_arena_test.cc` deletion (150 LoC),
the deeper `instance.cc` cleanup, and the documentation
updates.  Net is a **−405 LoC simplification**.

## 3 Memory model — the architectural change

This is the section the previous draft buried.  The user's
hard requirement: **each instantiation of the library has
its own memory model.**  We meet that.

### 3.1 Today's architecture (per Instance)

```
Engine (shared, process-global)
  └─ wasmtime_engine_t  (Cranelift state, compiled module cache)
  └─ runtime_module     (parsed `cel_runtime.wasm` bytes)

Instance ×N (one per Engine::Plan(program) call)
  ├─ wasmtime_store_t   (per-Instance Cranelift execution state)
  ├─ wasmtime_linker_t  (per-Instance linker)
  ├─ wasmtime_memory_t  (HOST-OWNED, 2 pages = 128 KB initial, grows)
  ├─ runtime_instance   (wasmtime instance of runtime_module)
  ├─ expr_module        (parsed `program.wasm` bytes)
  ├─ expr_instance      (wasmtime instance of expr_module)
  ├─ host_env           (cel_host trampoline payload)
  └─ host_string_arena  (offset/capacity into the memory above)

Memory layout (inside the per-Instance memory):
  [0, 8)       sentinel
  [8, 16)      bump cursor + limit
  [16, ?)      rodata (active data segment from expr_module)
  [?, ?)       workspace slots
  [?, ?)       bump arena
  [arena_limit, ∞)  host string arena (above the bump arena)

Memory is HOST-ALLOCATED (wasmtime_memory_new) and IMPORTED
by both runtime_instance and expr_instance.  Both see the
same bytes.
```

Critical property today: **every Instance has its own
linear memory**.  Each `Engine::Plan(program)` call creates
a fresh `wasmtime_store_t` + `wasmtime_memory_t` pair.
Instances are isolated.  ✓

### 3.2 Post-migration architecture (per Instance)

```
Engine (shared, process-global) — UNCHANGED
  └─ wasmtime_engine_t
  └─ runtime_module     (now built with wasi-sdk; includes dlmalloc)

Instance ×N — STRUCTURE UNCHANGED, ALLOCATOR CHANGED
  ├─ wasmtime_store_t   (per-Instance, unchanged)
  ├─ wasmtime_linker_t  (per-Instance, unchanged)
  ├─ wasmtime_memory_t  (NOW OWNED BY runtime_instance, NOT host)
  ├─ runtime_instance   (wasmtime instance of runtime_module;
                         instantiation runs wasi-libc's _initialize
                         and lazy-inits dlmalloc state)
  ├─ expr_module        (unchanged)
  ├─ expr_instance      (imports `cel.memory` from runtime_instance)
  ├─ host_env           (now holds malloc_fn handle)
  └─ (host_string_arena DELETED)

Memory layout (inside the runtime-owned memory):
  [0, __data_end)        wasi-libc + cel_runtime static data
                          (compiler-generated; ~few KB)
  [__data_end, sp_init)  wasm stack (grows down from $stack_pointer)
  [sp_init, sbrk)        dlmalloc heap (grows up via memory.grow)
  [sbrk, mem_size)       unused; memory.grow extends here on demand
```

Critical property post-migration: **every Instance STILL
has its own linear memory**, because every Instance still
gets its own runtime_instance which still owns one memory.
The owner changes (host → runtime_module) but isolation
is preserved.  ✓

### 3.3 How the runtime+expr modules share memory

Today: host creates memory; both modules import it.

Post-migration: runtime_module DEFINES and EXPORTS memory;
expr_module IMPORTS it.

```c
// runtime_module's manifest (effectively):
(memory $mem (export "memory") 1 65536)

// expr_module's manifest:
(import "cel" "memory" (memory 1))
```

Engine wiring:
```cpp
// In InitLinker (post-migration):
// - DON'T allocate a host memory.
// - INSTANTIATE runtime_instance first (this creates memory[0]).
// - DEFINE the exported `memory` from runtime_instance onto the
//   linker under (cel, memory).
// - INSTANTIATE expr_instance, which imports cel.memory and is
//   now wired to the runtime_instance's memory.
```

Two wasm instances share one linear memory — same as today,
just with the ownership chain inverted.

### 3.4 Where rodata + workspace live now

The expr_module installs its rodata as an **active data
segment** at a chosen offset.  Today, the offset is 16
(immediately after the cursor slot).

Post-migration, the offset must be **above wasi-libc's
static data + stack reservation**.  Two options:

**Option I — fixed offset.**  Hard-code an offset like
`0x10000` (64 KB into memory) that's safely above any
wasi-libc init.  Codegen bakes this in.

  - Pro: simplest.
  - Con: brittle — if wasi-libc's static data grows past
    that, breakage is silent.

**Option II — runtime-allocated offset.**  At eval start,
expr_module calls `malloc(rodata_size + workspace_size)`,
gets a base offset back, uses that base for both regions.

  - Pro: bulletproof — wasi-libc decides where; nothing
    can collide.
  - Con: requires runtime relocation — all offsets become
    `base + relative_off` rather than absolute constants.

**Option III — runtime-module reserves scratch.**
runtime_module declares a static `uint8_t scratch[64KB]`
array; exports its base as a wasm global; codegen reads
that global at instantiation to get the rodata + workspace
base.

  - Pro: clean separation; runtime owns the layout
    decision.
  - Con: 64 KB is now bss in every Instance even if the
    expression doesn't need it.

**Recommended: Option II + workspace-malloc'd-per-eval.**
Eval prologue:
```wasm
(local.set $base (call $malloc (i32.const RODATA+WS_BYTES)))
;; Active data segment installed bytes already AT a fixed
;; expected offset; we just copy from there to $base.
;; Actually simpler: skip active data segments. Codegen
;; emits `memcpy($base + ofs, rodata_const, size)` for each
;; constant.  Rodata is generated lazily, not pre-installed.
```

Codegen impact: kIdent emits
`(i32.add (local.get $base) (i32.const var_offset))` instead
of `(i32.const absolute_offset)`.  ~50 LoC added across
expr_lower's kIdent and kConst arms.

Trade-off summary:

| Option | Init cost | Per-eval cost | Codegen complexity |
|---|---|---|---|
| I — fixed offset | 0 | 0 | low (no change vs today) |
| II — malloc'd base | 0 | ~50ns (one malloc) | medium (relative addressing) |
| III — static scratch | ~64 KB bss/Instance | 0 | low |

Pick **Option II** for the prototype; revisit if perf data
suggests Option III is worth the bss cost.

### 3.5 Per-eval allocator strategy (pick one)

Three options for tracking allocations within an eval call:

**A. Global malloc, no per-eval tracking.**  Every
`cel_alloc(n)` → `malloc(n)`.  Caller (host) is responsible
for calling `free` on the result offset when done reading.

  - Pro: simplest.  Allocations are explicit.
  - Con: every CelValue containing a span (string, list,
    map) needs an explicit free call from the host.  Today
    that's implicit (cel_reset cleans everything).

**B. Per-eval `mspace_create_with_base` + `mspace_destroy`.**
At eval start, create an mspace; allocations go into it;
at eval end, destroy the whole mspace.  Mass-free.  Same
semantics as today's bump+reset.

  - Pro: bump-arena-like perf; no per-allocation cleanup.
  - Pro: matches today's lifecycle 1:1.
  - Con: needs `mspace.h` exposed; depends on dlmalloc-
    specific API.

**C. Per-Instance lifetime.**  Every allocation lives until
the Instance is dropped.  No per-eval reset.

  - Pro: zero overhead between evals.
  - Con: long-lived Instance + many evals = unbounded
    growth.  Effectively a memory leak.

**Recommended: B.** wasi-libc bundles dlmalloc and exposes
`mspace_*` via `<dlmalloc.h>` or a configure-time flag.
Each eval opens its own mspace, allocates from it,
destroys it on return.  Equivalent to the bump arena's
"fresh per eval" semantics, with full malloc API
compatibility for vendored libraries.

The mspace handle becomes a wasm global (idiomatic);
trampolines read it on demand.  No fixed-memory-offset
hacks.

### 3.6 The expression-owned-memory model preserved

Per the user's "each instantiation needs its own memory
model": this is preserved naturally.  Each Instance still
has its own runtime_instance → its own memory → its own
dlmalloc state → its own mspace.  Instances cannot leak
state into each other.

If anything, the migration **strengthens** the isolation:
today's host_string_arena hack writes activation strings
into shared linear memory above arena_limit; under malloc,
each string is a discrete allocation that the host can
free explicitly.  No shared region.

## 4 Instantiation cost analysis

This is the section the previous draft skipped.

### 4.1 Today's measured numbers (from `cel_pipeline_bench.cc`)

  - `BM_Engine_Build`: ~167 µs (parse `cel_runtime.wasm`,
    create wasmtime engine).  One-time per process.
  - `BM_Compile`: ~hundreds of µs (parse → check → resolve
    → layout → module → lower → assemble).  Per-source.
  - `BM_Plan_Hot`: ~12 µs across all simple inputs
    (per-Plan cost: new store + memory + linker + bind
    cel.memory + instantiate runtime + bind runtime
    exports + parse expr bytes + instantiate expr + lookup
    eval).  Per-Instance.
  - `BM_Eval`: ~tens of ns (per-eval, wasmtime call
    dispatch + decode).

### 4.2 Post-migration cost decomposition

Engine::Build:
  - `wasmtime_module_new(runtime_bytes)` — proportional to
    binary size.  Today's runtime is ~80 KB; post-migration
    with wasi-libc + dlmalloc is ~120-150 KB.  Parse cost
    scales linearly: ~167 µs → ~280-330 µs.
  - **Delta: +110-160 µs one-time per process.**

Engine::Plan (cost per Instance):
  - `wasmtime_store_new` — unchanged (~1 µs).
  - `wasmtime_memorytype_new` + `wasmtime_memory_new` —
    **DELETED** post-migration; runtime_module owns memory.
    Saves ~2-3 µs.
  - `wasmtime_linker_new` — unchanged (~0.5 µs).
  - `RegisterCelLog` + `RegisterCelHostImports` — unchanged
    (~1 µs).
  - `linker_define(cel.memory)` — **DELETED**; memory is
    defined by runtime_instance now.  Saves ~0.5 µs.
  - `linker_instantiate(runtime_module)` — this is the cost
    that changes.  Today: ~3-4 µs.  Post-migration:
    - wasm-instance creation: ~3-4 µs (unchanged).
    - wasi-libc's `_initialize` function runs implicitly,
      which:
      - Initializes dlmalloc's global state (~5-8 µs first
        time per Instance; just zero-fills a few hundred
        bytes).
      - Sets up the stack pointer.
      - No malloc calls yet — dlmalloc is lazy.
    - **Delta: +5-10 µs per Instance.**
  - `BindAllRuntimeExports` loop — unchanged (~1 µs).
  - `wasmtime_instance_export_get(malloc, ...)` lookup +
    bind — adds ~1 µs.
  - `wasmtime_module_new(expr_bytes)` — unchanged.
  - `linker_instantiate(expr_module)` — slightly slower
    because expr_module now does relative addressing
    (Option II in §3.4); active data segment install is
    replaced by `memcpy` calls in the eval prologue.  This
    means **first eval** is slightly slower (one extra
    malloc + memcpy ~100 ns).  Subsequent evals reuse the
    base.
  - `wasmtime_instance_export_get(eval, ...)` — unchanged.

**Total per-Plan delta: +6-11 µs**, taking BM_Plan_Hot from
~12 µs → ~18-23 µs.  ~50-90% relative increase.

Per-Eval:
  - `wasmtime_func_call($eval)` — same dispatch cost.
  - Eval prologue work:
    - Today: `cel_reset(arena_base, arena_limit)` =
      2 wasm stores, ~2 ns.
    - Post-migration: `malloc(workspace_bytes)` + remember
      offset.  dlmalloc malloc is ~30-50 ns for a hot
      fast-path allocation.
  - Eval body: identical (kernels return the same offsets).
  - Eval epilogue:
    - Today: nothing.
    - Post-migration: `free(workspace_base)` + (if Option B)
      `mspace_destroy(per_eval_mspace)`.  Combined ~50-100 ns.

**Total per-Eval delta: +80-150 ns**, taking BM_Eval from
~10-30 ns (literal eval) → ~100-200 ns.  3-10× relative
increase.

### 4.3 Where the per-Eval slowdown matters

  - **Literal eval** (`42` returns 42) — most affected.
    Today's eval is "i32.const 42; cel_reset; return".
    Post-migration adds the malloc+free.  But literal eval
    is rare in real CEL — usually you have at least an
    ident.
  - **Field access** (`request.user.id`) — small fixed
    overhead.  Today: cel_reset + bump for the host
    trampoline output. Post-migration: malloc for the
    trampoline output.  Net: ~50-100 ns extra per eval.
    Not a deal-breaker.
  - **Comprehension** (`list.exists(v, p)`) — N evals of
    the predicate, each allocates 0-few cells.  If each
    iteration does 1 malloc that's 50 ns × N.  For N=100
    that's 5 µs.  Bench it.
  - **String/regex** (`s.matches(re)`) — once RE2 is
    vendored, the regex match itself dominates (1-10 µs);
    our 100 ns prologue/epilogue is in the noise.

### 4.4 Optimization opportunities for the per-Eval path

If the bench delta turns out to be unacceptable:

1. **Per-Instance mspace cached**: instead of
   create+destroy per eval, the Instance owns one
   long-lived mspace; eval calls `mspace_reset(msp)` (a
   dlmalloc extension that frees all allocations without
   destroying the mspace).  ~5 ns per eval — back to bump-
   arena territory.
2. **Eval prologue elision**: if the expr has zero
   dynamic allocations (e.g. pure literal, pure bool
   comparison), codegen detects this and skips the
   workspace malloc entirely.
3. **Stack-based scratch**: small allocations
   (< 256 bytes) go on the wasm stack via `__builtin_alloca`
   instead of malloc.  Already supported by clang.

Recommend bench first; optimize if needed.  The 100 ns
order-of-magnitude is comfortable under most usage
patterns.

### 4.5 Memory footprint per Instance

Today:
  - 2 wasm pages = **128 KB** baseline per Instance.

Post-migration:
  - wasi-libc + dlmalloc static data: ~16 KB.
  - Initial heap: dlmalloc allocates first page on demand
    (1 page = 64 KB).
  - Workspace + rodata: ~8-32 KB depending on expr.
  - **Baseline ~80-110 KB per Instance**; grows on demand
    via `memory.grow`.

Slightly lower baseline; same dynamic growth model.
Memory.grow's amortized cost per page is ~10 µs (wasmtime
allocates host-side and zero-fills).

## 5 Build system — comprehensive changes

### 5.1 `MODULE.bazel`

Add 4 `http_archive` blocks (one per platform/arch), plus
platform-select rules.  Total: ~50 LoC.

```python
http_archive(
    name = "wasi_sdk_darwin_arm64",
    sha256 = "...",
    urls = ["https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-arm64-macos.tar.gz"],
    build_file = "//third_party/wasi_sdk:BUILD.external.bazel",
    strip_prefix = "wasi-sdk-25.0-arm64-macos",
)

# Similar for darwin_x86_64, linux_x86_64, linux_arm64.

# Platform-aware alias:
alias(
    name = "wasi_sdk",
    actual = select({
        "@platforms//os:macos+@platforms//cpu:arm64": "@wasi_sdk_darwin_arm64",
        "@platforms//os:macos+@platforms//cpu:x86_64": "@wasi_sdk_darwin_x86_64",
        "@platforms//os:linux+@platforms//cpu:x86_64": "@wasi_sdk_linux_x86_64",
        "@platforms//os:linux+@platforms//cpu:arm64": "@wasi_sdk_linux_arm64",
    }),
)
```

New file: `third_party/wasi_sdk/BUILD.external.bazel` —
exports clang / clang++ / ar / ranlib as filegroups so
genrules can reference them with `$(execpath ...)`.  ~30 LoC.

Net `MODULE.bazel + third_party/wasi_sdk/` delta: +90 LoC.

### 5.2 `compiler_v2/runtime/BUILD.bazel`

The `cel_runtime_wasm_file` genrule changes:

  - `tools = ["@wasi_sdk//:clang"]` (was hard-coded
    `/opt/homebrew/...`).
  - `cmd` invokes `$(execpath @wasi_sdk//:clang)` instead
    of the brew path.
  - Compiler flags: `--target=wasm32-wasi` (was `wasm32`),
    drop `-ffreestanding -nostdlib`, drop
    `--import-memory=cel,memory`, drop `--export=cel_alloc`
    and `--export=cel_reset`, add `--export=malloc` and
    `--export=free`, add `--export-memory` to expose memory.

Net BUILD.bazel delta: ~15 LoC changed (no net add).

### 5.3 `compiler_v2/runtime/BUILD.bazel` — native test build

The native build (host-side test infrastructure) compiles
runtime `.c` files with the system compiler against
`g_memory[]` (the static array in `cel_memory.c`).  This
path **stays the same** — native compiler has malloc out of
the box; we just call it.  No BUILD changes.

### 5.4 `compiler_v2/api/BUILD.bazel`

The API targets compile against wasmtime + our own runtime
bytes.  No build flag changes; only the runtime bytes
binary changes shape (now contains dlmalloc).

### 5.5 `scripts/lint.sh` + `scripts/refresh_compile_db.sh`

`compile_commands.json` will reference wasi-sdk's clang
after the runtime BUILD change.  Two effects:
  - PCH cache (`.lint-cache/lint_pch.h.pch`) invalidates;
    next lint rebuilds it (~10 s one-time).
  - clang-tidy on runtime `.c` files sees wasi-libc
    headers — a small handful of lints (mostly around
    `assert.h` macros) may fire differently.

Net change: 0 LoC in the scripts; ~0.5 day of lint
follow-up post-migration.

### 5.6 CI

Per repo memory `feedback_runtime_cross_platform.md`, no
`brew install` deps for the runtime build.  Wasi-sdk via
`http_archive` meets this.  CI sees the new MODULE.bazel
on first build; bazel downloads the platform-appropriate
tarball; build succeeds.

### 5.7 Cross-platform build cost

The wasi-sdk tarball is 200 MB compressed, 1.5 GB
uncompressed per platform.  First fetch on a clean CI
machine: ~30 s download + ~15 s extract.  Subsequently
cached.

## 6 What absolutely doesn't change

  - **CEL evaluation semantics** — identical conformance
    pass count expected.  This is a refactor.
  - **cel-cpp integration** (parser + checker) — unchanged.
  - **24-byte `CelValue` layout** — unchanged.
  - **The `cel_host_*` import set** — same names, same
    signatures (Allocator& abstraction holds).
  - **Engine / Instance public C++ API** — unchanged.
  - **Programs compiled before the migration** — would NOT
    work post-migration (they import `cel.cel_reset` and
    `cel.cel_alloc`, which are removed).  Programs are
    typically re-compiled per-Source per-deploy, so this is
    fine; not a wire-format break.
  - **Conformance row pass count** — unchanged (semantic
    equivalence is the migration's correctness gate).
  - **Browser-compat story** — wasi-libc's malloc has 0
    WASI imports for pure allocation (verified in
    `../exp1_re2/probe_malloc.wasm`: 7.4 KB malloc-only
    wasm with 0 imports).  Browser embedders are unaffected
    until they call something that uses WASI (file/clock).

## 7 What's load-bearing in today's design that goes away

Catalogue of clever hacks the migration removes:

  1. **The inline-asm opacity barrier in cel_memory.c**
     (lines 13-15) — load-bearing because clang sees
     `(uint8_t*)0` as a C null pointer and elides stores
     through it.  Under wasi-sdk, the memory pointer comes
     from `__builtin_wasm_memory_base()` or wasi-libc
     internals, which clang treats as a real pointer.
     No barrier needed.
  2. **The fixed cursor slot at bytes 8/12** —
     load-bearing because cel_reset writes the arena state
     there and every cel_alloc reads it.  Gone; dlmalloc
     keeps its state inside its own private bookkeeping.
  3. **The host string arena positioning hack** —
     load-bearing because cel_reset would clobber anything
     below `arena_limit`.  Gone.
  4. **The 2-arg `cel_reset(base, limit)` ABI** — load-
     bearing because codegen baked compile-time-known
     constants into the prologue.  Gone.
  5. **`LoweringOptions::mem_size_bytes` plumbing** —
     load-bearing because codegen needs to know the
     arena limit at compile time.  Gone; runtime
     decides via dlmalloc's own bookkeeping.
  6. **Two-arg memory typing
     (`wasmtime_memorytype_new(min, max_present=false,
     max=0, ...)`)** in `engine.cc` — load-bearing
     because the host has to choose initial+max.  Gone;
     runtime_module declares its own min/max.

## 8 Migration slice plan

8 slices, each independently committable, ~4.5 sessions
total.

### Slice 1 — wasi-sdk in MODULE.bazel (0.5 session)

Add the 4 http_archives + the alias + the
`third_party/wasi_sdk/BUILD.external.bazel`.  No compiled
code touched yet.  Verify `bazel build @wasi_sdk//:clang`
succeeds on macOS arm64 (which is what dev box has) and
that the platform-select rule resolves correctly.

### Slice 2 — Switch runtime build to wasi-sdk (0.5 session)

`compiler_v2/runtime/BUILD.bazel`: swap the genrule's
clang + flags.  Keep `cel_reset` / `cel_alloc` as no-op
shims temporarily (we'll remove later).  Verify the new
`cel_runtime.wasm` binary is bigger but still loads under
wasmtime and existing tests pass.

### Slice 3 — Replace `cel_alloc` with `malloc` in runtime kernels (0.5 session)

Mechanical search-replace across 10 `.c` files + 21 test
files.  Keep `cel_reset` for now (we still need an mspace
reset).

### Slice 4 — Codegen: switch the eval prologue to malloc-based workspace (1 session)

This is the design-heavy slice.

  - Remove `EmitCelResetCall`.
  - Replace with: at eval start, emit
    `(local.set $ws_base (call $malloc (i32.const ws_bytes)))`.
  - Adjust kIdent codegen to emit relative offsets:
    `(i32.add (local.get $ws_base) (i32.const var_off))`.
  - Adjust kConst codegen similarly.
  - Remove `arena_base` from `StaticLayout`.
  - Remove `mem_size_bytes` from `LoweringOptions`.
  - Add `cel_msp` (the per-eval mspace handle) as a wasm
    global; codegen reads it from the prologue and threads
    it through trampoline calls.

Cross-feed conformance to confirm zero semantic delta.

### Slice 5 — mspace per eval (0.5 session)

Add mspace lifecycle helpers in the runtime:
  - `cel_eval_begin()` → opens mspace, stores in global.
  - `cel_eval_end()` → destroys mspace.

Codegen emits these as eval prologue/epilogue.  Allocations
inside the eval (including via trampolines) use the global
mspace.

### Slice 6 — Delete host_string_arena, rewrite activation marshalling (0.5 session)

`api/instance.cc`: remove `EnsureHostStringArenaCapacity`
and all references.  Activation string marshalling
becomes:
```cpp
uint32_t off = host_malloc(ctx, malloc_fn, str.size());
WriteBytes(mem, off, str);
SetCelValue(slot, kString, off, str.size());
```

`InstanceImpl`: drop the 2 host_string_arena fields.

### Slice 7 — Engine memory ownership flip (0.5 session)

`api/engine.cc`: stop allocating memory host-side.
Instantiate runtime first; pull its exported memory and
bind it under `cel.memory` on the linker; then instantiate
expr against that wired-up memory.

### Slice 8 — Closeout (0.5 session)

  - Re-run `scripts/run_full_suite.sh`.
  - Re-run conformance; expect identical PASS count.
  - Re-run `BM_Engine_Build`, `BM_Plan_Hot`, `BM_Eval` —
    capture deltas, fold into RESULTS.md.
  - Update docs (§2.9 list).
  - Rebuild lint PCH; fix any new lint warnings.

## 9 Risk register

  - **R1**: wasi-libc's dlmalloc may not expose `mspace_*`
    by default (some builds gate them behind a configure
    flag).  Mitigation: verify in Slice 5; if not exposed,
    either rebuild wasi-libc with the flag, or fall back to
    Option C (Instance-lifetime, no per-eval reset).
  - **R2**: Per-eval cost regression (Eval ~10ns → ~150ns)
    may matter for hot-path embedders (proxy filters).
    Mitigation: §4.4 optimisations; ship Slice 8 with
    measured numbers before deciding on optimisation.
  - **R3**: Memory.grow during eval invalidates wasmtime
    host-side memory pointers.  Today this is handled by
    re-deriving the base before each access; same
    discipline holds.  No new code, but worth verifying
    in Slice 7's e2e tests.
  - **R4**: clang-tidy lint warnings on wasi-libc headers.
    Mitigation: 0.5 day of fix-up; could be deferred to a
    follow-up commit after the migration.
  - **R5**: CI cold-start cost (wasi-sdk tarball download,
    ~30s) on first build per CI worker.  Mitigation:
    cache key on the http_archive sha256; subsequent
    builds hit cache.
  - **R6**: M5 comprehensions follow-on milestone work
    (planned, scoped) and this migration both touch
    LayoutPass + expr_lower.  Mitigation: ship
    comprehensions first (its plan is locked); start this
    migration only after comprehensions lands.  Order
    matters; merge conflicts in `expr_lower.cc` are real.

## 10 What goes into the prototype directory

This directory will accumulate:

  - `ANALYSIS.md` (this doc).
  - `prototype_runtime.c` — a tiny standalone runtime
    using `mspace_*` per-eval.
  - `prototype_runtime_driver.wat` — hand-coded driver
    showing the eval prologue/epilogue shape.
  - `bench_compare.sh` — micro-bench harness comparing
    bump-arena (today) and mspace-based runtime on
    representative workloads.  Numbers feed into the
    Slice 8 closeout.
  - `RESULTS.md` — empirical numbers + final go/no-go
    recommendation.
  - `CLAUDE_Do_NOT_DELETE_OR_REVERT_FILES_IN_THIS_DIR` —
    the sentinel that tells other agents to leave files
    here alone.

Prototype work runs *separately* from the main
`compiler_v2/` build.  Lessons go into a follow-up
milestone plan (`doc/implementation-plan/rewrite/
m-wasi-malloc-migration.md`) after the user reviews
the prototype's RESULTS.md.

## 11 Final recommendation

**Yes — proceed with the migration.**

Summary:

  - Net code change: **−405 LoC** (large reduction; the
    bump arena and host string arena were carrying real
    weight).
  - Migration scope: 8 incremental slices, ~4.5 sessions.
  - Per-Engine instantiation cost: **+110-160 µs** one-time
    (larger runtime binary).
  - Per-Instance instantiation cost: **+6-11 µs** (~50-90%
    relative increase; absolute still small).
  - Per-Eval cost: **+80-150 ns** (3-10× relative; absolute
    still well under 1 µs for typical workloads).
  - **Per-Instance memory isolation preserved** — every
    Instance still owns its own memory; isolation is
    actually strengthened by removing the shared
    host_string_arena region.
  - Architectural payoff: libraries (RE2, abseil, any
    future C++ dep) ship in the runtime with zero
    integration tax; codegen simplifies; host
    activation-marshalling simplifies; the inline-asm
    hack in cel_memory.c goes away.

Sequencing: ship M5 comprehensions follow-on first (it's
already scoped and locked); start this migration after.
Don't run them in parallel — both touch `expr_lower.cc`
and `layout_pass.cc`; merge conflicts are guaranteed.

Suggested next step: build `prototype_runtime.c` in this
directory; measure the bench delta against a synthetic
workload; write RESULTS.md.  Then draft the formal
milestone plan as
`doc/implementation-plan/rewrite/m-wasi-malloc-migration.md`
once the empirical numbers are in.
