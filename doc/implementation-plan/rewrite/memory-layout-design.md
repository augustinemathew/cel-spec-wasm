# Memory layout design (singular source of truth)

Status: consolidated 2026-05-24. **This is the single authoritative
description of how linear memory is organized at runtime.** It
consolidates the memory architecture previously split across:

- [`wasi/DESIGN.md`](wasi/DESIGN.md) §4–§7 (the malloc-arena migration —
  now a historical build-plan; its memory model lives here),
- [`phase-c-design.md`](phase-c-design.md) (the threads/shared-memory
  flip + in-runtime libraries — historical build-plan),
- [`modules-and-ffi.md`](modules-and-ffi.md) §2 (the substrate it now
  references here instead of duplicating),
- [`design.md`](design.md) §3.2 (the uber-design keeps the basics +
  points here for depth).

Anything that needs to change about memory organization changes **here
first**; the others reference this doc.

Cross-references:
- **FFI / how data crosses module boundaries** → [`modules-and-ffi.md`](modules-and-ffi.md).
- **Uber design / pipeline** → [`design.md`](design.md) §3.2.
- **Constants + invariants in code** → `runtime/cel_layout.h`.

---

## 1. The one shared linear memory (post-Phase-C)

There is **exactly one linear memory per Instance**, **defined and
exported as shared by `cel_runtime.wasm`**. Everything else imports it.

- The runtime is built `wasm32-wasi-threads` (forced by vendoring
  `absl::time`/cctz, which needs `<mutex>` — see Phase C note §7).
  `-threads` mandates a **shared** memory; the observed shape is
  `(memory 4 1024 shared)` (initial 4 pages, max 1024 = 64 MiB).
- `-Wl,--global-base=8192` on the runtime link forces wasi-libc to put
  its static data + stack + heap **above** byte 8192, leaving
  `[0, 8192)` free for the expr module.
- The **expr module imports** `cel.memory` with a matching shared shape
  (`(memory N 1024 shared)`); wasmtime rejects a non-shared import
  against a shared export.
- A **CEL-defined library** (when compiled as its own module — not the
  v1 single-module model, see §5) would likewise import `cel.memory`.
- A **foreign library** does NOT share this memory — it has its own;
  the host marshals across (`modules-and-ffi.md` §5).

Ownership inverted vs. the original pre-migration drafting (where the
expr module defined memory and the arena sat at its high end). The
as-shipped model: **runtime owns memory; arena lives in the dlmalloc
heap.** (`wasi/DESIGN.md` §4.2 historical; `design.md` §3.2.)

---

## 2. The three regions and three lifetimes

```
 shared cel.memory (defined by cel_runtime.wasm; imported by expr [+ any lib module])
 0                                  8192                 ~__heap_base (≈243568)
 ├───────────────────────────────────┼─────────────────────┼─────────────────────────▶ grows
 │  RESERVED LOW REGION [0, 8192)     │ wasi-libc statics + │  DLMALLOC HEAP            │
 │  CELWASM_RESERVED_LOW_MEMORY_BYTES │ 64 KB shadow stack  │  ┌────────────────────┐  │
 │                                    │ (--global-base=8192)│  │ per-Eval bump arena│  │
 │  [0,16)  null sentinel             │                     │  │ per-Instance       │  │
 │  16      .rodata (const CelValue   │  off-limits to      │  │   activation buffer│  │
 │          headers + str/bytes bytes)│  codegen            │  │ Plan-lifetime objs │  │
 │  ws_base 24B workspace slots       │                     │  │   (RE2 cache, …)   │  │
 │          (vars + SlotAllocator     │                     │  │ [future] separate  │  │
 │           scratch)                 │                     │  │   lib __memory_base│  │
 └─────────────────────────────────────────────────────────┴──┴────────────────────┴──┘
   lifetime: the module             lifetime: process     lifetime: Eval / Instance / Plan
```

### 2.1 Reserved low region `[0, 8192)` — compile-time static slots
Lifetime = the module. Installed as **active data segments** at
module-instantiate time. Holds:

- `[0,16)` — **null sentinel**: a zero-kind CelValue so `off==0 ⇒
  absent` is well-defined. Every `_at` helper treats `out==0` as a
  no-op; `cel_value_at(0)` returns a well-formed NULL.
- `rodata_base = 16` — `.rodata`: constant CelValue headers + packed
  string/bytes payload bytes, one region per `kStaticRodata` node.
- `workspace_base = RoundUp8(rodata_base + rodata.size())` — 24-byte
  CelValue workspace slots for `kWorkspaceSlot` nodes (free variables +
  select/aggregate/call scratch). The `SlotAllocator` **recycles**
  workspace cells across non-overlapping lifetimes and tracks
  `peak_slots`.

**Bounded.** `LayoutPass` fails with `ResourceExhausted` if
`rodata + workspace` would overrun the region
(`LayoutOptions::reserved_region_limit_bytes`, default
`CELWASM_RESERVED_LOW_MEMORY_BYTES`). Beyond 8192 lies the runtime's
own static data — an overrun would corrupt the runtime at instantiate
(the data segment) or at eval (workspace stores), so the guard is a
hard compile-time tripwire, not a runtime check.

### 2.2 Runtime statics + shadow stack `[8192, __heap_base)`
Lifetime = process. Owned by `cel_runtime.wasm` (wasi-libc static data
+ a 64 KB shadow stack). `__heap_base` lands above this (~243568,
varies by build mode). **Off-limits to codegen.**

### 2.3 dlmalloc heap `[__heap_base, …)`
Lifetime varies by sub-allocation. `malloc`-backed. Holds:

- **Per-Eval bump arena** — `arena_init(CELWASM_ARENA_CAPACITY_BYTES =
  64 KiB)` `malloc`s the buffer **once per Instance**; `arena_alloc(n)`
  8-aligns, bumps `g_arena.cursor`, and returns the **absolute** offset
  of `g_arena.base + cursor`; `arena_reset()` (zero-arg) rewinds the
  cursor to 0. Holds string-concat results, list/map bodies,
  host-decoded proto fields, and lifted foreign results. Reset between
  Evals. The legacy `StaticLayout::arena_base` field is **no longer
  consulted** by codegen.
- **Per-Instance activation buffer** — `malloc`'d once per Instance
  (grows via realloc), holds bound string/bytes payloads. **Not the
  arena** — `arena_reset` would wipe bindings. Its offset is `>=
  CELWASM_RESERVED_LOW_MEMORY_BYTES` (invariant A15).
- **Plan-lifetime objects** — RE2 regex cache, parsed timestamps, etc.
- **[future]** separately-instantiated library modules' `__memory_base`
  regions (§5, not v1).

---

## 3. The CelValue and the slot model

The unit every intra-memory helper passes is the **24-byte CelValue**
(`runtime/cel_data.h`):

```c
struct CelValue {        // sizeof == 24, _Alignof == 8
  uint32_t kind;         // CelKind @ +0
  uint32_t _pad;         // @ +4
  union { ... } payload; // @ +8, 16 bytes
};
```

- Scalars (`bool`/`int`/`uint`/`double`/`null`) inline in `payload`.
- `string`/`bytes` carry `{ptr, len}` — `ptr` is an **absolute** offset
  into the shared memory (rodata bytes, activation buffer, or arena).
- Aggregates (`list`/`map`) carry an arena header pointer / `ref_slot`.
- `CEL_MESSAGE` is a host-resident handle (`msg_slot`) — valid only in
  shared memory; it may not cross into a foreign module
  (`modules-and-ffi.md` §5.8).

A **slot** is the byte offset of one such cell. Wasm values flowing
through codegen are i32 slot offsets, not payloads. Every intra-memory
helper has the shape `(func $h (param $out_slot i32) (param $arg_slot
i32) … (result))` — reads `*argN_slot`, writes `*out_slot`, void
return.

---

## 4. Constants + asserted invariants

Single source of truth: **`runtime/cel_layout.h`**, included
by codegen, host, and runtime so the three can't drift.

| Constant | Value | Meaning |
|---|---|---|
| `CELWASM_INITIAL_MEMORY_PAGES` | (runtime-defined; observed init 4) | initial shared-memory pages |
| `CELWASM_RESERVED_LOW_MEMORY_BYTES` | 8192 | the `--global-base`; reserved low region size |
| `CELWASM_ARENA_CAPACITY_BYTES` | 64 KiB | per-Instance arena buffer size |
| `kWasmPageSize` | 64 KiB | wasm page |

Every sizing/layout assumption is asserted at the point it matters
(from `wasi/DESIGN.md` §5; `static_assert` where static, `ABSL_CHECK`
where dynamic):

| # | Invariant | Where |
|---|---|---|
| A1–A4 | `sizeof(CelValue)==24`, `_Alignof==8`, `offsetof(kind)==0`, `offsetof(payload)==8` | `cel_data.h` static_assert |
| A5 | arena capacity is power-of-2 | `cel_layout.h` |
| A7 | reserved-low == 8192 (the `--global-base`) | `cel_layout.h` |
| A8 | CelKind enum agrees codegen↔runtime | `cel_data.h` / codegen |
| A9–A10 | `arena_alloc(0)` safe; OOM returns 0, never a partial offset | `cel_arena.c` |
| A11 | `workspace_base + workspace_bytes < reserved_region_limit` | `layout_pass.cc` (`ResourceExhausted`) |
| A12 | `rodata_base + rodata.size() <= workspace_base` | `layout_pass.cc` |
| A13 | instantiated memory page count == initial | `engine.cc` |
| A14 | runtime `__heap_base` >= reserved-low | `engine.cc` Plan-time |
| A15 | activation buffer offset >= reserved-low (never overlaps `[0,8192)`) | `instance.cc` |
| A16 | `arena_init` called exactly once per Instance | `cel_arena.c` |
| A17 | codegen emits no `cel_alloc`/`cel_reset` (legacy) imports | `compile.cc` |

---

## 5. Where custom-function memory fits

Two backends, two memory stories (full mechanics in
`modules-and-ffi.md`):

- **CEL-defined ("fully defined" — body in the `.celfn`)** — **v1
  single-module**: the body is compiled into the **same** expr module
  as an internal wasm function; it shares the one `[0,8192)` reserved
  region. Each compiled function (expr + each body) gets a **disjoint
  static band** within `[0,8192)` (cumulative `rodata_base_override` +
  per-band `reserved_region_limit_bytes`), so a callee's workspace
  never clobbers a caller's live cells. **No separate module, no
  `__memory_base`, no heap region** in v1.
  - **`__memory_base` is future work**, needed only if CEL-defined
    libraries later become separately-instantiated/reusable modules
    sharing `cel.memory` (then each needs a disjoint heap region whose
    base is bound at instantiate; see `modules-and-ffi.md` §5-future).
    String/aggregate-literal pointer-relocation (`ptr = base + K`) is a
    `__memory_base`-only concern and does not arise in v1 (base is 0).
- **Foreign (TinyGo/Rust/clang)** — **own memory**, cross-memory FFI;
  the host marshals args in / results out via a fixed canonical-ABI
  seam. Isolated — cannot touch `[0,8192)`. See `modules-and-ffi.md`
  §5.

---

## 6. Lifecycle

### Per-Plan (cold path, ~once per `Engine::Plan`)
1. `wasmtime_store_new` + `wasmtime_linker_new` + register host
   trampolines + `wasmtime_linker_define_wasi`.
2. Instantiate `cel_runtime.wasm` → memory created here (runtime owns
   it); wasi-libc `_initialize` runs.
3. Pull from runtime: memory → bind as `cel.memory`; `malloc`/`free`/
   `arena_*`/every `cel_*` kernel → bind as `cel.*`; assert A14.
4. `arena_init(64 KiB)` — bookends dlmalloc lazy-init + arena buffer
   alloc here, off the Eval hot path. Assert A13/A16.
5. Instantiate the expr module → active data segments install rodata
   into `[16,…)` of the shared memory.
6. Malloc the activation buffer (wasm reentry); cache its offset; assert
   A15.

### Per-Eval (hot path — Instance lives across many Evals)
Host: marshal each bound activation value into its workspace slot
(scalars inline; string/bytes copied into the activation buffer, slot
stamped with `{ptr,len}`). Call `eval()` → root offset. Decode the
CelValue at the root offset.

Inside `$eval` (codegen-emitted): `(call $arena_reset)` → expression
body → return root offset.

---

## 7. Phase C threading note

The original migration (`wasi/DESIGN.md` decision #4) picked
`wasm32-wasi` vanilla ("zero WASI imports"). Vendoring `absl::time`
pulled in **cctz**, which needs `<mutex>`, forcing
`wasm32-wasi-threads` → **shared** memory (`(memory 4 1024 shared)`) +
`wasi_snapshot_preview1.*` imports (satisfied host-side by
`wasmtime_linker_define_wasi`). Everything else — malloc-backed arena,
`--global-base=8192`, zero-arg `arena_reset`, runtime-owned memory —
shipped as designed. The "zero WASI imports / plain
`WebAssembly.instantiate`" browser side-benefit is correspondingly
weakened (threads build is not import-free).

---

## 8. Status of the consolidated docs

- **`wasi/DESIGN.md`** — historical build-plan for the malloc-arena
  migration (M1–M9, B1–B6) + the baseline benchmarks (§10) + the risk
  register. Its memory model (§4) and invariant table (§5) are
  reproduced + kept current **here**; read this doc for the live memory
  design, that doc for the migration history + bench baselines.
- **`phase-c-design.md`** — historical build-plan for in-runtime
  parsers + RE2/`matches()`. Its threads/shared-memory consequence is
  captured in §1 + §7 here.
- **`design.md` §3.2** — the uber-design's memory basics; points here
  for depth.
- **`modules-and-ffi.md`** — references §2 here as the substrate;
  owns the FFI / data-crossing mechanics.
