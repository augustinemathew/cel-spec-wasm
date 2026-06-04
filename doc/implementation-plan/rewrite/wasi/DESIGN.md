# WASI / `malloc` migration — design + work plan

Branch: `wasi-malloc-migration` (forked from master @ `9685d72`).

**Status: shipped 2026-05-18 (M1–M7 + B1–B6); Phase C shipped
later (RE2 + absl::time / cctz vendored).**  As-shipped deltas
annotated in §1 + §10.  Phase D (Chrome) remains open.

> **Phase C delta (shipped) — the threading/shared-memory flip.**
> Vendoring `absl::time` pulled in **cctz** (Google's civil-time /
> timezone library), which needs `<mutex>` for its timezone-database
> cache.  That forced the runtime onto the **`wasm32-wasi-threads`**
> toolchain — reversing **decision #4** (§3), which had picked
> `wasm32-wasi` vanilla.  `-threads` mandates a **shared** linear
> memory, so the as-shipped memory shape is **`(memory 4 1024
> shared)`**, defined + exported by `cel_runtime.wasm` and imported
> (matching shared shape) by the expr module.  The runtime also now
> imports `wasi_snapshot_preview1.*` (wasi-libc surface), satisfied
> host-side by `wasmtime_linker_define_wasi`.  Everything else in
> this doc — the malloc-backed arena (§4), `--global-base=8192`
> reserved region (§3 #3), zero-arg `arena_reset` (§1), runtime-owned
> memory (§4.2) — shipped as designed; only the *non-shared* framing
> (e.g. the `(memory 2)` in the §2.1 MVP `.wat`) and decision #4 are
> superseded.  The browser-deployment side benefit below is also
> weakened: the threads build is no longer "zero WASI imports."

> **Memory architecture consolidated 2026-05-24 into
> [`../memory-layout-design.md`](../memory-layout-design.md).** That doc
> is now the singular source of truth for memory organization (the §4
> layout + §5 invariant table + §6/§7 lifecycle are reproduced and kept
> current there). **This doc remains the historical build-plan** for the
> malloc-arena migration (Phase A/B work items §8, the baseline
> benchmarks §10, the bench workload §11, the risk register §12). Read
> `memory-layout-design.md` for the live memory design; read on here for
> the migration history + bench baselines.

**Single source of truth (for the migration plan).**  Supersedes the
previous trail of docs in this dir; deviations from this doc need an
explicit update here.

---

## 1 Goal

Replace `compiler/`'s freestanding wasm32 + bump-arena-at-
fixed-offsets architecture with **wasi-sdk's `wasm32-wasi` +
a hand-rolled bump arena over `malloc()`**.

**Goal is simplification, not WASI for its own sake.** What
gets removed (all eight items SHIPPED 2026-05-18):

  - ✅ The `cel_reset(arena_base, arena_limit)` codegen prologue
    — replaced with `(call $arena_reset)` (M5, commit `dfc366c`).
  - ✅ `LoweringOptions::mem_size_bytes` threading — removed
    from codegen plumbing (M5).
  - ✅ `LayoutPass::arena_base` field — no longer consulted by
    codegen (M5).
  - ✅ The fixed cursor slot at memory bytes 8/12 — bump cursor
    lives in BSS post-M3 + codegen no longer writes through
    offsets 8/12 post-M5.
  - ⚠ The inline-asm opacity barrier in `cel_memory.c` — still
    present; flagged in `cleanup-backlog.md #4` pending
    verification that wasi-sdk clang-19 doesn't need it.  Not
    blocking the merge; correctness-only audit.
  - ✅ The `host_string_arena` workaround in `api/instance.cc`
    — replaced with malloc'd activation buffer + `EnsureActivation
    Buffer` (M7, commit `5d8156a`).  Net ~50 LoC instead of
    ~110.
  - ✅ The 2-arg memory typing in `engine.cc`
    (`wasmtime_memorytype_new(min=2, ...)`) — deleted alongside
    the memory-ownership flip (M6, commit `208ddba`).
  - ✅ The `--import-memory=cel,memory` linker dance — dropped
    from runtime/BUILD.bazel (M6).

**Post-migration status (2026-05-18, post-M7):** seven of eight
"what gets removed" items have shipped.  The eighth (the
inline-asm opacity barrier) is a correctness audit only and
tracked in `cleanup-backlog.md`.  The migration's simplification
dividend is realized.

Side benefits:
  - Any C/C++ library (RE2, parts of absl) can be vendored
    into the runtime without dual-allocator pain.
  - Browser deployment via plain `WebAssembly.instantiate`
    stays viable — verified zero WASI imports for pure-malloc
    code in `experiments/exp_c_malloc.c`.

---

## 2 MVP: `"foo" + "bar"` end-to-end

The smallest workload that proves the architecture is
**string concatenation of two constants** evaluating to
`"foobar"` end-to-end (Compile → Plan → Eval → Decode), in
both wasmtime AND Chrome.

This MVP exercises:

  - Runtime built with wasi-sdk (S1, S2).
  - `arena_init` / `arena_alloc` / `arena_reset` working (S3).
  - One kernel (`cel_string_concat_at_vv`) migrated to use
    `arena_alloc` (S4 subset).
  - Codegen prologue emits `(call $arena_reset)` (S6).
  - Host marshalling: pulls runtime-owned memory, decodes a
    string CelValue from arena-allocated bytes (S8).
  - Cross-platform: same `cel_runtime.wasm` runs in wasmtime
    and Chrome.

**MVP scope is intentionally narrow** — kernels other than
concat stay on the old `cel_alloc` (we'll provide a backwards-
compat shim during the transition).  Once MVP works, the
remaining kernels migrate mechanically.

### 2.1 The MVP `.wat` (codegen target)

```wat
;; Codegen for: "foo" + "bar"
;;
;; Memory layout under WASI/malloc (see §4):
;;   [0, 8192)        FREE — installed as expr active data segments
;;     [16, 40)       CelValue{CEL_STRING, payload.s={ptr=64, len=3}} for "foo"
;;     [40, 64)       CelValue{CEL_STRING, payload.s={ptr=67, len=3}} for "bar"
;;     [64, 70)       payload bytes "foobar" (back-to-back rodata)
;;     [72, 96)       workspace slot for the concat result
;;   [8192, ~75K)     wasi-libc static data + 64 KB stack
;;   [~75K, ∞)        dlmalloc heap (arena buffer lives here)

(module
  ;; Phase C delta: the import is now a SHARED memory —
  ;;   (import "cel" "memory" (memory 2 1024 shared))
  ;; matching the runtime's exported `(memory 4 1024 shared)`.  The
  ;; non-shared form below is the original MVP target, kept for the
  ;; layout walkthrough; see the Phase C delta at the top of this doc.
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "cel_string_concat_at_vv"
          (func $concat (param i32 i32 i32)))

  ;; Two CelValue headers (24 bytes each) + their packed payloads.
  (data (i32.const 16)
        "\05\00\00\00" "\00\00\00\00"      ;; kind=CEL_STRING(5)
        "\40\00\00\00" "\03\00\00\00"      ;; ptr=64, len=3
        "\00\00\00\00" "\00\00\00\00")     ;; pad
  (data (i32.const 40)
        "\05\00\00\00" "\00\00\00\00"
        "\43\00\00\00" "\03\00\00\00"      ;; ptr=67, len=3
        "\00\00\00\00" "\00\00\00\00")
  (data (i32.const 64) "foobar")           ;; packed payload bytes

  (func $eval (result i32)
    (call $arena_reset)                    ;; cursor = 0 (~5 ns)
    (call $concat (i32.const 72)           ;; out_slot
                  (i32.const 16)           ;; "foo"
                  (i32.const 40))          ;; "bar"
    (i32.const 72))                        ;; return root offset

  (export "eval" (func $eval)))
```

**This is identical in shape to today's
`doc/implementation-plan/rewrite/wat/18_string_concat.wat`**
except:
  - `cel_reset(c1, c2)` → `arena_reset()` (no args).
  - Memory is imported, not host-allocated; runtime owns it.

### 2.2 What the kernel does (runtime C, post-migration)

```c
// runtime/cel_string_ops.c
void cel_string_concat_at_vv(uint32_t out_slot,
                             uint32_t a_slot,
                             uint32_t b_slot) {
  CelValue* out = cel_value_at(out_slot);
  const CelValue* a = cel_value_at(a_slot);
  const CelValue* b = cel_value_at(b_slot);
  if (span_op_prelude(out, a, b, CEL_STRING)) return;

  uint32_t total = a->payload.s.len + b->payload.s.len;
  uint32_t off = arena_alloc(total);      // ← was cel_alloc
  if (off == 0) { poison(out, CEL_ERR_OVERFLOW); return; }

  uint8_t* dst = cel_memory_base_() + off;
  memcpy(dst, cel_memory_base_() + a->payload.s.ptr, a->payload.s.len);
  memcpy(dst + a->payload.s.len,
         cel_memory_base_() + b->payload.s.ptr, b->payload.s.len);
  out->kind = CEL_STRING;
  out->payload.s.ptr = off;
  out->payload.s.len = total;
}
```

The kernel signature stays identical.  The only delta is
`cel_alloc` → `arena_alloc` — a mechanical rename across
107 call sites.

### 2.3 What the host decoder does

```cpp
// eval/internal/abi_decode.cc
cel::Value DecodeStringCelValue(const MemoryView& mem, uint32_t slot_off) {
  const CelValue* cv = mem.Read<CelValue>(slot_off);
  if (cv->kind == CEL_STRING) {
    const char* bytes = (const char*)(mem.base() + cv->payload.s.ptr);
    return cel::Value::String(absl::string_view(bytes, cv->payload.s.len));
  }
  // ... other kinds
}
```

Unchanged from today.  The `ptr` is an offset in linear
memory; under WASI, that offset can land in the arena
buffer (in the dlmalloc heap region) — the decoder doesn't
need to care.

### 2.4 Per-layer tests for the MVP

| Layer | Test | File | What it proves |
|---|---|---|---|
| Runtime C kernel | `arena_init(64K)` + write two CelValues into mock memory + call `cel_string_concat_at_vv` + verify result CelValue + payload bytes | `runtime/cel_string_ops_test.cc` (extend existing) | Kernel works against the new arena API. |
| Arena module | `arena_init` + sequence of `arena_alloc` + `arena_reset` + re-alloc returns the same offset | `runtime/cel_arena_test.cc` (rewrite) | Bump arena over malloc has correct semantics. |
| WAT trace | Hand-coded `.wat` from §2.1 runs through `wat_runner`, returns offset 72; host decodes CelValue at offset 72 and verifies `{kind=STRING, payload.s={ptr=arena_base, len=6}}` | `doc/implementation-plan/rewrite/wasi/experiments/mvp_concat.wat` (NEW) | Codegen ABI shape locked. |
| Codegen | Compile `'foo' + 'bar'` through `compiler_v2`; assert emitted wasm matches the WAT byte-for-byte (modulo Binaryen-assigned names) | `compiler/codegen/expr_lower_test.cc` (extend) | Codegen produces the right output. |
| Host integration | Build, instantiate, eval; result is the `cel::Value::String("foobar")` | `e2e/mvp_concat_test.cc` (NEW) | Engine + Instance + decoder all work. |
| Chrome | Same MVP wasm loaded via `WebAssembly.instantiate` in headless Chrome (Puppeteer or similar); JS reads memory at returned offset and verifies `"foobar"` | `doc/implementation-plan/rewrite/wasi/experiments/mvp_concat_chrome/` (NEW) | Browser target works. |

---

## 3 Resolved architectural decisions

Five questions, all answered with experimental evidence in
`experiments/`.

| # | Question | Decision | Evidence |
|---|---|---|---|
| 1 | Tree strategy | **In-place migration in `compiler/`** | User direction 2026-05-17. |
| 2 | Allocator strategy | **Hand-rolled bump arena over a single `malloc()`** | `experiments/exp_b_mspace.c` (mspace_* not in wasi-libc); `experiments/exp_d_arena_in_malloc.c` (47-LoC arena works, end-to-end). |
| 3 | Memory layout | **`--global-base=8192` on the runtime build.  Expr rodata in `[0, 8192)`.** | `experiments/exp_a_rodata.c` (linker flag honored; bytes `[0, N)` left free). |
| 4 | Threading target | ~~**`wasm32-wasi` vanilla, no `-threads`**~~ → **SUPERSEDED in Phase C: `wasm32-wasi-threads` + shared memory** (cctz needs `<mutex>`; see the Phase C delta at the top of this doc). | `experiments/exp_c_malloc.c` (the original "zero WASI imports for pure malloc" finding held only until absl::time/cctz were vendored). |
| 5 | M5 comprehensions ordering | **Ships first; this migration starts after** | Both touch `expr_lower.cc` and `layout_pass.cc`. |

---

## 4 Memory architecture

### 4.1 Linear memory layout per Instance

```
offset 0x00000  ─────  EXPR MODULE RESERVED (8 KB)
                       Active data segments install here at
                       module instantiate time.
                       Codegen places rodata starting at offset 16
                       (skipping the first 16 bytes for null-sentinel
                       parity with today).
                       Workspace slots also in this region.

offset 0x02000  ─────  WASI-LIBC STATIC DATA + STACK
                       (~5 KB static + 64 KB stack by default;
                       not user-tunable per Instance.)

offset ~0x12000 ─────  DLMALLOC HEAP
                       __heap_base lives here.
                       Per-Instance arena buffer is malloc'd here once
                       (default 64 KB).
                       Activation binding buffer is malloc'd here once
                       (grows via realloc as needed).
                       Plan-lifetime allocations (future RE2 regex
                       objects, absl-parsed timestamps) also here.
```

### 4.2 Module + Instance relationships

```
Engine (process-global):
  ├─ wasmtime_engine_t
  └─ runtime_module       (cel_runtime.wasm bytes parsed once)

Instance (per Engine::Plan):
  ├─ wasmtime_store_t
  ├─ wasmtime_linker_t
  ├─ runtime_instance     (instantiated runtime_module)
  │   ├─ exports memory   ← runtime owns memory now; Phase C: SHARED
  │   │                      `(memory 4 1024 shared)` (wasm32-wasi-threads)
  │   ├─ exports malloc, free
  │   ├─ exports arena_init, arena_alloc, arena_reset
  │   └─ exports all cel_* kernels (unchanged signatures)
  ├─ expr_module          (program.wasm bytes parsed per Plan)
  ├─ expr_instance        (imports cel.memory + cel.arena_* + cel.cel_*)
  ├─ host_env             (cel_host trampoline payload — holds the malloc fn handle)
  └─ activation_buf_off   (one i32: cached offset of malloc'd binding buffer)
```

**Host does NOT allocate memory.**  Runtime owns it.

### 4.3 Runtime module exported surface (post-migration)

```c
// NEW exports the migration adds:
void   arena_init(uint32_t cap_bytes);   // one-time per Instance
uint32_t arena_alloc(uint32_t n);        // returns offset (or 0 = OOM)
void   arena_reset(void);                // cursor = 0
uint32_t arena_capacity(void);
uint32_t arena_cursor(void);
void*  malloc(size_t);                   // wasi-libc; surfaced for host reentry
void   free(void*);                      // wasi-libc

// REMOVED exports:
//   cel_alloc(uint32_t)
//   cel_reset(uint32_t, uint32_t)

// All other cel_* kernels unchanged.
```

---

## 5 Asserted assumptions

**Every sizing or layout assumption the design embeds MUST be
asserted in code at the point it matters.**  Static where
possible; runtime check where the value is dynamic.

| # | Assumption | Where asserted | Mechanism |
|---|---|---|---|
| A1 | `sizeof(CelValue) == 24` | `runtime/cel_data.h` (already there for native; verify wasm too) | `static_assert` |
| A2 | `_Alignof(CelValue) == 8` | `runtime/cel_data.h` | `static_assert` |
| A3 | `offsetof(CelValue, kind) == 0` | `runtime/cel_data.h` | `static_assert` |
| A4 | `offsetof(CelValue, payload) == 8` | `runtime/cel_data.h` | `static_assert` |
| A5 | `kArenaCapacityBytes == 65536` (64 KB default) | NEW `runtime/cel_arena.h` | `#define` + `static_assert` for power-of-2 |
| A6 | `kInitialMemoryPages == 2` (= 128 KB) | NEW `runtime/cel_layout.h` | `static_assert` |
| A7 | `kReservedLowMemoryBytes == 8192` (the `--global-base`) | NEW `runtime/cel_layout.h` | `static_assert` |
| A8 | CelValueKind enum values match between codegen and runtime (`CEL_STRING == 5`, etc.) | `codegen/cel_value_layout.h` (NEW or extend existing) | `static_assert` against runtime header |
| A9 | `arena_alloc(0)` is safe (returns valid offset or 0) | `runtime/cel_arena.c` | runtime check + test |
| A10 | `arena_alloc(n)` returns 0 on OOM, NOT a partial offset | `runtime/cel_arena.c` | runtime check |
| A11 | Codegen-assigned `workspace_base + workspace_bytes < kReservedLowMemoryBytes` | `codegen/layout_pass.cc` | `ABSL_CHECK` at end of LayoutPass |
| A12 | Codegen-assigned `rodata_base + rodata.size() <= workspace_base` | `codegen/layout_pass.cc` | `ABSL_CHECK` |
| A13 | Wasm memory page count at instantiation == `kInitialMemoryPages` | `api/engine.cc` after runtime_instance instantiation | `ABSL_CHECK` reading `wasmtime_memory_size` |
| A14 | `__heap_base` symbol from runtime ≥ `kReservedLowMemoryBytes` | `api/engine.cc` Plan-time | `ABSL_CHECK` (pull `__heap_base` export, compare) |
| A15 | Activation buffer offset never overlaps `[0, kReservedLowMemoryBytes)` | `api/instance.cc` | `ABSL_CHECK` after malloc |
| A16 | `arena_init` called exactly once per Instance | `runtime/cel_arena.c` | `ABSL_CHECK(g_arena.base == NULL)` on entry |
| A17 | Codegen does NOT emit `cel_alloc` or `cel_reset` imports post-migration | `codegen/compile.cc` | `ABSL_CHECK_NE(import_name, "cel_alloc")` etc. |

### 5.1 The constants header

A single source of truth for the magic numbers:

```c
// runtime/cel_layout.h  (NEW)
#define kInitialMemoryPages    2u
#define kReservedLowMemoryBytes 8192u   // == --global-base on the runtime build
#define kArenaCapacityBytes    (64u * 1024u)
#define kWasmPageSize          (64u * 1024u)

static_assert(kReservedLowMemoryBytes <
              kInitialMemoryPages * kWasmPageSize,
              "reserved region must fit inside initial memory");
static_assert((kReservedLowMemoryBytes & 7) == 0,
              "reserved region size must be 8-aligned");
static_assert((kArenaCapacityBytes & (kArenaCapacityBytes - 1)) == 0,
              "arena capacity should be power-of-2 (helps growth math)");
```

Codegen + host + runtime all `#include` this.  Drift between
the three is caught at compile time.

---

## 6 Per-Plan lifecycle (cold path)

Runs once per `Engine::Plan(program)`.  Estimated total:
**~60 µs** (faster than today's 279 µs — see §10).

```
1.  wasmtime_store_new                                    ~1 µs
2.  wasmtime_linker_new + register cel_log + cel_host_*  ~1 µs
3.  wasmtime_linker_instantiate(runtime_module)         ~10 µs
    → runtime_instance
    - Memory created here (runtime owns it now).
    - wasi-libc _initialize runs (stack pointer set;
      dlmalloc lazy-init deferred to first malloc).
4.  Pull from runtime_instance:                          ~2 µs
    - memory     → bind on linker as cel.memory
    - malloc, free, arena_*  → bind as cel.*
    - every cel_* kernel     → bind as cel.<name>
    - __heap_base export     → ASSERT (A14) >= kReservedLowMemoryBytes
5.  wasmtime_func_call(arena_init, kArenaCapacityBytes) ~5 µs
    - First malloc: dlmalloc lazy-init (~3 µs) + arena
      buffer allocation (~1 µs).  Bookended in arena_init
      so all initialization cost lands here, not on the
      first Eval.
6.  ASSERT (A13) wasm memory page count == kInitialMemoryPages
7.  wasmtime_module_new(expr_bytes)                     ~30 µs
8.  wasmtime_linker_instantiate(expr_module)            ~10 µs
    → expr_instance
    - Active data segments install expr rodata into
      [16, ...) of the shared memory.
9.  Pull expr_instance.eval → eval_fn                    ~1 µs
10. Malloc activation buffer (~1 µs) via wasm reentry.
    Cache offset in InstanceImpl.activation_buf_off.
    ASSERT (A15) offset >= kReservedLowMemoryBytes.
```

---

## 7 Per-Eval lifecycle (hot path)

**We do NOT instantiate per Eval.**  The Instance lives across
many Evals.  Estimated per-Eval cost: **~145 ns** (vs 141 ns
baseline — +4 ns, within the 5× budget by a factor of 35).

```
Instance::Eval(activation) — host:
  1.  For each (name, value) in activation:
      - Look up workspace slot offset for `name`.
      - If value is string/bytes:
        - Grow activation buffer if needed (realloc via wasm reentry).
        - memcpy payload bytes into buffer.
        - Stamp CelValue{kind, payload.s={ptr, len}} into the slot.
      - Else (scalar): stamp CelValue directly.
  2.  wasmtime_func_call(eval_fn, []) → root_offset
  3.  Decode CelValue at root_offset.

Inside $eval (codegen-emitted wasm):
  1.  (call $arena_reset)             ;; cursor = 0
  2.  <expression body — unchanged from today>
  3.  return root_offset
```

Per-Eval cost decomposition:

| Step | Today | Post-migration | Δ |
|---|---:|---:|---:|
| Host marshal (no bindings) | ~0 ns | ~0 ns | 0 |
| `wasmtime_func_call` dispatch | ~100 ns | ~100 ns | 0 |
| $eval prologue (cel_reset / arena_reset) | ~5 ns (2 i32.store) | ~5 ns (1 i32.store) | 0 |
| Expression body | varies | varies (kernels do the same work) | 0 |
| CelValue decode | ~30 ns | ~30 ns | 0 |
| **Total (literal scalar)** | **141 ns** | **~145 ns** | **+~4 ns** |

---

## 8 Work items — MVP first, then full migration

### Phase A — MVP (`"foo" + "bar"` works end-to-end)

Aggressive: ~5 working days.  Goal is a runnable demo on
both wasmtime AND Chrome before any of the long-tail kernel
migration starts.

| Slice | Work | Acceptance | Days |
|---|---|---|---:|
| **M1** | wasi-sdk in `MODULE.bazel` (4 platforms) + `third_party/wasi_sdk/BUILD.external.bazel`. | `bazel build @wasi_sdk//:clang` works. | 0.5 |
| **M2** | `runtime/BUILD.bazel`: switch to wasi-sdk, `--target=wasm32-wasi`, drop `-ffreestanding -nostdlib`, add `-Wl,--global-base=8192`.  Add `cel_layout.h` (§5.1) + all `static_assert`s (A1-A8).  Add temporary `cel_alloc`/`cel_reset` shims that route to `arena_alloc`/`arena_reset` so old kernels still build. | `cel_runtime.wasm` builds and existing runtime tests pass. | 1.0 |
| **M3** | Implement new `cel_arena.c` (~50 LoC: arena_init/alloc/reset over malloc).  Replace inline-asm in `cel_memory.c` with wasi-sdk-friendly version.  Unit-test the arena module. | `bazel test //runtime:cel_arena_test` passes; A9-A10 + A16 asserts active. | 0.5 |
| **M4** | Migrate ONE kernel: `cel_string_concat_at_vv` from `cel_alloc` → `arena_alloc`.  Update its unit test. | `bazel test //runtime:cel_string_ops_test` passes. | 0.5 |
| **M5** | Codegen prologue: replace `EmitCelResetCall(arena_base, mem_size)` with `EmitArenaResetCall()`.  Drop `arena_base` from `StaticLayout`.  Drop `mem_size_bytes` from `LoweringOptions`.  Add A11-A12 asserts in LayoutPass.  Add A17 assert in compile.cc. | Compile `"foo" + "bar"`; emitted wasm matches the MVP `.wat` byte-for-byte. | 1.0 |
| **M6** | Engine: stop host-allocating memory; pull runtime_instance.memory.  Add A13-A14 asserts.  Bind `arena_*` + `malloc` + `free` on the linker.  Call `arena_init` once per Instance. | `Engine::Plan(program)` succeeds for the MVP program. | 0.5 |
| **M7** | Instance: rewrite `EncodeStringOrBytes` over malloc'd binding buffer.  Delete `EnsureHostStringArenaCapacity` and helpers (~110 LoC).  Add A15 assert. | Activation marshal works for the MVP test. | 0.5 |
| **M8** | E2E test: `e2e/mvp_concat_test.cc`.  Asserts `Compile("'foo' + 'bar'")` → `Plan` → `Eval` → `cel::Value::String("foobar")`. | Test passes. | 0.25 |
| **M9** | Chrome smoke-test: `experiments/mvp_concat_chrome/` with the compiled MVP `.wasm` + a small `index.html` driver that calls `WebAssembly.instantiate`, calls `eval()`, reads memory at offset returned, decodes string, asserts `"foobar"`. | Loads + evaluates in Chrome via Puppeteer or manual confirmation. | 0.5 |
| **MVP total** | | | **5.25 days** |

### Phase B — finish the migration

| Slice | Work | Days |
|---|---|---:|
| **B1** | Migrate the remaining 106 `cel_alloc` call sites across 5 kernel `.c` files.  Drop the temporary compat shim from M2. | 1.0 |
| **B2** | Migrate the remaining 20 test files' `SetUp()` from `cel_reset` to `arena_reset`. | 0.5 |
| **B3** | Codegen test fixture rebaseline (50 sites in `codegen/*_test.cc`). | 1.0 |
| **B4** | Conformance debug: re-run, find divergences, fix. Target: **1,144 PASS**. | 1-2 |
| **B5** | Post-migration bench harness against the §11 workload.  Write `POST_MIGRATION_BENCH.md` with deltas. | 0.5 |
| **B6** | Doc closeout: update `doc/wasm-compiler-design.md`, `doc/implementation-plan/rewrite/design.md`, conformance README's headline; flip this doc's status to "shipped". | 0.5 |
| **Phase B total** | | **4.5-5.5 days** |

### Phase C — RE2 / `absl::ParseTime` vendoring (post-MVP, post-migration)

The migration unlocks library vendoring.  Phase C ships
**regex** and **timestamp formatting/parsing** as proof of
the architectural payoff.

| Slice | Work | Days |
|---|---|---:|
| **C1** | Vendor `abseil-cpp` into `third_party/abseil-cpp_wasm/` via `http_archive` + `BUILD.external.bazel`.  Cross-compile via wasi-sdk + apply our `absl-wasm.patch` (from `wasm_compilation_experiments/exp1_re2/`). | 1.0 |
| **C2** | Vendor `re2` similarly.  Build against the vendored absl. | 0.5 |
| **C3** | New runtime function `cel_matches_at_vv(out, target, regex)` calling `RE2::PartialMatch`.  Per-Instance regex cache (an `absl::flat_hash_map<string, RE2*>`, malloc-backed; persists across evals).  CelHost trampoline pattern stays. | 1.0 |
| **C4** | New runtime function `cel_timestamp_parse_at_v(out, str)` calling `absl::ParseTime`.  Same shape; no caching needed (parse is fast). | 0.5 |
| **C5** | Conformance — the `string.textproto::matches/*` rows (9 SKIPs today) flip to PASS.  `timestamps.textproto` parse-dependent rows also flip. | 0.5 |
| **C6** | Binary size + bench delta measurement.  Check against the user's stated requirement that this runs in Chrome. | 0.5 |
| **Phase C total** | | **4.0 days** |

### Grand total

**MVP + full migration: ~10 days.  + RE2/absl vendoring: ~14 days.**

---

## 9 Acceptance criteria

The migration **ships** when ALL of:

  - [ ] `bazel test //...` green.
  - [ ] `bazel run //conformance:run_conformance` → **1,144 PASS** (matches baseline).
  - [ ] Per-Eval cost ≤ **5× baseline**:
    - Scalar Eval: ≤ 705 ns (today 141 ns).
    - String Eval: ≤ 785 ns (today 157 ns).
  - [ ] Per-Plan cost ≤ **1.5× baseline** (≤ 419 µs).  Expected: drops.
  - [ ] Memory baseline ≤ **1.5× baseline** (≤ 192 KB initial).  Expected: ~150-180 KB.
  - [ ] `cel_runtime.wasm` ≤ **2× baseline** (≤ 122 KB stripped).  Expected: 90-110 KB.
  - [ ] **MVP runs in Chrome** (see §2.4 row 6).
  - [ ] All `static_assert`s + `ABSL_CHECK`s from §5 are active in the build.

---

## 10 Baseline numbers (the "before" to compare against)

Captured 2026-05-18 on this branch @ `9685d72`, before any
migration code lands.  See `git log` for the bench command;
machine was Apple M1 with light load.

| Static metric | Value |
|---|---:|
| compiler_v2 production C/C++ LoC | 25,823 |
| `cel_runtime.wasm` stripped | 60,971 B |
| `cel_runtime.wasm` gzipped | 11,741 B |
| Imports | 14 (1 cel_log + 12 cel_host + memory) |
| Exports | 162 (incl. memory) |
| Initial memory | 2 pages = 131,072 B |

| Dynamic metric (mean of 1 rep @ `--benchmark_min_time=2s`) | Wall ns |
|---|---:|
| `BM_Compiler_Build` | 3 |
| `BM_Engine_Build` | 5,730,165 (5.73 ms) |
| `BM_Compile` (avg) | 294,000 (294 µs) |
| `BM_Plan_Hot` (avg) | 279,000 (279 µs) |
| `BM_Eval` (scalar) | 141 |
| `BM_Eval` (string) | 157 |
| `BM_Pipeline_Cold` | 6,516,125 (6.52 ms) |
| `BM_Pipeline_WarmEngine` | 586,940 (587 µs) |
| `BM_Pipeline_WarmProgram` | 288,650 (289 µs) |
| `BM_Pipeline_HotEval` | 141 |

The bench header in `cel_pipeline_bench.cc` claims "Engine
Build ≈ 167 µs" and "Eval ≈ tens of ns" — both are stale
(34× and 5× off respectively).  Trust this table, not the
header.

To reproduce:
```sh
bazel run -c opt //bench:cel_pipeline_bench -- \
  --benchmark_min_time=2s
```

---

## 11 Bench workload (post-migration comparison)

S11 / B5 runs `cel_pipeline_bench` against this 10-row
workload set.  Each row produces a golden `cel::Value`
auto-asserted on every run.

| # | Source | Bound | Exercises | Milestone gating |
|---|---|---|---|---|
| 1 | `42` | — | kConst, pure rodata | M1 |
| 2 | `true && false` | — | kCall(`_&&_`), short-circuit | M5 |
| 3 | `x` | `x: int` | kIdent, scalar activation | M2 |
| 4 | `x + y` | `x,y: int` | kCall arith | M5 |
| 5 | `'foo' + 'bar'` | — | string concat (the MVP) | M5C |
| 6 | `s.contains('hello')` | `s: string` | receiver kCall, string ops | M5C |
| 7 | `msg.field` | `msg: Customer` | kSelect, proto trampoline | M3 |
| 8 | `[1, 2, 3].size()` | — | list size | M4 |
| 9 | `{'a':1,'b':2}.size()` | — | map size | M3 |
| 10 | `int(msg.f) > 0` | `msg: Customer` | conversion + arith | M10 |

Comprehension rows omitted — M5 follow-on sequenced before
this migration anyway; revisit when it ships.

Methodology:
  - Same machine, same load profile, same bazel `-c opt`.
  - `--benchmark_min_time=2s --benchmark_repetitions=3`.
  - Capture mean + p99 + stddev per row.
  - Compare to §10 deltas; pass/fail per §9.

---

## 12 Risk register

  - **R1.  dlmalloc lazy-init cost lands on Plan, not Eval.**
    Plan setup now triggers it via the explicit `arena_init`
    call.  ~5 µs one-time per Plan.  If this exceeds budget,
    we could lazy-init via a no-op `malloc(0)` in
    `arena_init`.
  - **R2.  Activation buffer fragmentation.**  If activations
    have wildly different binding sizes, dlmalloc may
    fragment.  Mitigation: realloc strategy doubles the
    buffer; binding offsets within it come from a single
    host-side cursor (same pattern as today's
    `host_string_arena`, just with malloc'd backing).
  - **R3.  Codegen test rebaselining (B3).**  ~50 fixtures
    assert specific call targets.  Budget 1 day, not 0.5.
  - **R4.  Chrome smoke-test surprises (M9).**  Wasmtime ≠
    Chrome in subtle ways.  Our zero-WASI-imports design
    should run cleanly, but verify early.
  - **R5.  M5 comprehensions parallel work.**  If that branch
    lands on master mid-migration, rebase against the new
    `expr_lower.cc` shape.  Mitigation: don't start M5
    (codegen prologue swap) until comprehensions has merged
    or paused.

---

## 13 Files in this directory

```
DESIGN.md                            ← this doc; the build plan
CLAUDE_Do_NOT_DELETE_OR_REVERT...    ← sentinel for other agents
experiments/
  exp_a_rodata.c                     ← rodata layout probe
  exp_b_mspace.c                     ← mspace_* link test (fails)
  exp_c_malloc.c                     ← pure-malloc probe (zero imports)
  exp_d_arena_in_malloc.c            ← the recommended design
  exp_d_driver.wat                   ← cross-module driver
  .gitignore                         ← *.wasm + symlink
  wasi-sdk → ../../../../wasm_compilation_experiments/exp1_re2/wasi-sdk-25.0-arm64-macos
```

The previous trail of docs (HANDOFF.md, WORK_PLAN.md,
BENCHMARK_DESIGN.md, ANALYSIS.md, MEMORY_OPTIONS.md,
BASELINE_BENCH.md, AGENT_ASSESSMENT.md, README.md) is
**superseded by this doc** and removed.  Git history
preserves them if needed.

---

## 14 Status checklist (live)

  - [x] Branch cut (`wasi-malloc-migration` @ master `9685d72`).
  - [x] Baseline benchmark captured (§10).
  - [x] Memory layout + reset experiments answered (§3).
  - [x] Architectural decisions resolved (§3).
  - [x] Design + work plan consolidated (this doc).
  - [ ] Phase A (MVP, M1-M9): not started.
  - [ ] Phase B (full migration, B1-B6): not started.
  - [ ] Phase C (RE2 + absl::ParseTime, C1-C6): not started.
