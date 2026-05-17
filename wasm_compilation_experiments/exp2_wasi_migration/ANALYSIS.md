# Migrating compiler_v2 to WASI + `malloc`

Status: analysis — drafted 2026-05-17.  Companion to
`../exp1_re2/` (which proved wasi-sdk + libraries-in-wasm works
end-to-end with 150 KB gzipped RE2).

## 1 Goal

Replace the bump arena (`compiler_v2/runtime/cel_arena.c`) with
wasi-libc's `malloc` / `free`, switch the runtime build from
`--target=wasm32 -nostdlib` to `--target=wasm32-wasi`, and align
the host/wasm boundary on standard C library allocation
semantics.

Why:

  - **Enables vendoring C/C++ libraries** (RE2, abseil, future
    parsers) without the dual-allocator integration tax described
    in `../WASI_AND_PORTABILITY.md §4`.  Today linking RE2 into
    `cel_runtime.wasm` would have RE2's dlmalloc and our bump
    arena both call `memory.grow` — heap corruption.
  - **Removes the host-string-arena hack** (`api/instance.cc`
    §"`EnsureHostStringArenaCapacity`", ~50 LoC).  That hack
    exists *because* the bump arena rewinds its cursor on
    every eval, clobbering activation strings the host
    pre-wrote into the arena.  Under malloc, the host calls
    `malloc` (reentering wasm) to allocate input strings;
    each lives until the host frees it.
  - **Simplifies codegen** — no `cel_reset` prologue, no
    `arena_base` compile-time constant, no `mem_size_bytes`
    threaded through `LoweringOptions`.
  - **Aligns lifecycle with the "expression-owned memory"
    model** the user used in the prior compiler: each
    `Instance` owns its wasm memory; allocations live until
    the Instance is dropped (or explicitly freed).

## 2 Current arena architecture — inventory

What we're replacing.

### 2.1 The runtime allocator (3 files, ~170 LoC)

  - `compiler_v2/runtime/cel_arena.c` (66 LoC) —
    `cel_alloc(n)` bump and `cel_reset(base, limit)` cursor
    reset.  Cursor lives at fixed memory offsets `[8..16)`.
  - `compiler_v2/runtime/cel_arena.h` (39 LoC) — ABI.
  - `compiler_v2/runtime/cel_memory.c` (68 LoC) — base/size
    accessors with the inline-asm opacity barrier needed to
    keep clang from optimizing away writes through a
    cast-from-0 pointer (see header comment in cel_memory.c).
    The barrier is load-bearing under `--target=wasm32
    -nostdlib`; under wasi-sdk it's unnecessary.

### 2.2 Call sites in the runtime kernel (107 occurrences)

`cel_alloc` is called from ~10 runtime `.c` files for every
non-rodata allocation: list payload bytes, map bucket arrays,
string concatenation results, time-value payloads, etc.
Every site is a direct `cel_alloc(size_t)` → offset call.
Mechanical search-and-replace target.

### 2.3 Codegen emission (2 sites)

  - `compiler_v2/codegen/expr_lower.cc:106-114`
    (`EmitCelResetCall`) — emits
    `call $cel_reset(arena_base, mem_size)` as the first
    instruction of every generated `$eval`.  Both arguments
    are compile-time constants baked in via `i32.const`.
  - `compiler_v2/codegen/expr_lower.cc:1879` — the call site
    that inserts `EmitCelResetCall` into the eval prologue.
  - `compiler_v2/codegen/layout_pass.cc:390` — computes
    `layout.arena_base = workspace_base + workspace_bytes`
    (rounded up to 8 bytes).
  - `compiler_v2/codegen/expr_lower.h:42` —
    `kCelResetInternalName = "cel_reset"`.

### 2.4 Host-side integration (3 areas)

  - **Host string arena** (`api/instance.cc:340-450`, ~110
    LoC).  Activation kString / kBytes encoder writes input
    bytes into linear memory ABOVE `arena_limit` so they
    survive `cel_reset`.  Includes `memory.grow` calls when
    the host arena fills up.
  - **`WasmtimeArenaAllocator`**
    (`api/internal/cel_host_wasmtime.cc:155-180`).
    Reentrant wasm-call into the `cel_alloc` export from
    inside Layer-3 trampolines.  Wraps the reentry with
    error-to-trap translation.  Used by 8+ trampoline call
    sites for output allocations.
  - **`WasmtimeMemoryView`** — wraps wasmtime's
    linear-memory accessor.  Mostly unaffected by the
    migration except for the "memory base may move on
    grow" rule (wasmtime invalidates the pointer after
    every `memory.grow`).  Already handled today; no
    change needed.

### 2.5 Fixed memory layout

```
[ 0,   8)   reserved sentinel (offset 0 == absent)
[ 8,  12)   u32 bump cursor
[12,  16)   u32 limit
[16,  ?)    rodata (compile-time .data segment)
[?,   ?)    workspace slots (LayoutPass-assigned)
[?,   ?)    bump arena ([arena_base, arena_limit))
[?,   ∞)    host string arena (above arena_limit)
```

Everything below `arena_limit` is rewound on every
`cel_reset`.  This is the cause of the host-string-arena
positioning hack.

### 2.6 Test surface

55 occurrences of `cel_alloc` across 14 runtime test files.
Tests directly invoke the runtime kernels via the native
build (which compiles the same `.c` files into a host-side
shared lib for fast iteration).  Most tests `cel_reset()`
once in `SetUp()` then exercise the kernel.  Migration:
mechanical — replace `cel_reset` with no-op (or
mspace-create) and `cel_alloc` with `malloc`.

### 2.7 The `cel_runtime.wasm` build target

`compiler_v2/runtime/BUILD.bazel:244-280` invokes brew's
LLVM clang with:
  - `--target=wasm32`
  - `-ffreestanding -nostdlib`
  - `-O3 -flto -mtail-call`
  - `-Wl,--no-entry`
  - `-Xlinker --import-memory=cel,memory`
  - ~30 `-Wl,--export=...` lines for the public runtime ABI.

This produces a 64 KB-page module with imported memory and
no libc.  Migration target: switch to wasi-sdk's clang with
`--target=wasm32-wasi` (or `wasm32-wasi-threads` if we want
`std::mutex` etc. for future C++ deps); drop `-ffreestanding
-nostdlib`; drop `--import-memory` (wasi-sdk provides its
own); keep the `--export=` lines but remove `cel_alloc` /
`cel_reset`.

## 3 Allocator strategy — three options

dlmalloc (wasi-libc's default) is full-featured but each
`malloc`/`free` cycle has overhead.  Our current bump+reset
is ~10ns/alloc; dlmalloc is ~50-100ns.  Per-eval allocation
count varies wildly — small expressions allocate 0-5 times,
big comprehensions allocate hundreds.

### Option A: Per-Instance lifetime + `malloc` directly

Every Instance owns its memory.  Allocations live until the
Instance is dropped.  No reset between evals.

  - Pro: simplest.  No reset mechanism, no lifecycle
    tracking.
  - Con: allocations accumulate across evals.  For a
    long-lived Instance evaluating millions of CEL programs,
    this is a memory leak.
  - Use case: one-shot programs.  CLI mode, batch processing.

### Option B: `mspace` per eval (dlmalloc sub-arena)

dlmalloc supports `mspace_create_with_base` / `mspace_destroy`
— create a sub-arena, allocate from it via
`mspace_malloc(msp, n)`, then discard the whole thing.

  - Per-eval: `mspace_create_with_base(buf, size)` →
    `mspace_malloc` 100 times → `mspace_destroy`.
  - Pro: bump-arena-like perf (no per-allocation overhead;
    one wholesale free at the end).
  - Pro: still uses standard C library.
  - Pro: C++ libraries vendored later (e.g. RE2) can use
    their own `mspace` or the global `malloc`.
  - Con: needs `dlmalloc.h` exposed; not portable to non-dl
    allocators.  Wasi-libc ships dlmalloc so we're fine.
  - **Recommended.**  Matches the user's "memory owned by
    the expression" mental model — each eval's memory is
    fully isolated and freed wholesale.

### Option C: Reference-counted CelValues with explicit free

Each CelValue carries a refcount; allocations live until
their refcount hits 0.  Standard C++ ownership model.

  - Pro: precise; no leaks even with long-lived Instances.
  - Con: complex.  Every kernel needs ownership annotations.
    Cycles (list-of-list) require tracing.
  - Verdict: overkill for our use case.  Not recommended.

**Pick B.**  Each `$eval` opens a per-call mspace; the
generated wasm threads the mspace handle through every
allocation; on return, the mspace is destroyed.

## 4 Codegen impact

### 4.1 What goes away

  - `EmitCelResetCall` (8 LoC in `expr_lower.cc`).
  - `kCelResetInternalName` (1 LoC in `expr_lower.h`).
  - `arena_base` field in `StaticLayout`
    (`layout_pass.h:105` + the line computing it in
    `layout_pass.cc:390`).
  - `mem_size_bytes` field in `LoweringOptions`
    (`compile.h:44-49`).
  - The 2nd arg of `EmitCelResetCall` (the `mem_size_bytes`
    arena_limit) — no longer needed.

### 4.2 What replaces it

  - **mspace prologue** — at the top of every `$eval`, call
    `mspace_create_with_base(buf, size)` where `buf` is a
    per-eval scratch region.  Two variants:
    - **(a) Static scratch**: codegen reserves a fixed
      `EVAL_SCRATCH_BYTES` of bss memory; mspace lives there.
      Fast but caps eval memory.
    - **(b) Dynamic scratch**: `$eval` calls `malloc` once
      for the scratch region, then mspace-owns it.  More
      flexible, one extra malloc per eval.
  - **Threading the mspace handle**: every codegen site that
    today calls `cel_alloc(n)` now calls
    `mspace_malloc(msp, n)`.  This means **every host
    trampoline needs the mspace pointer threaded into it** —
    the kCall arm emits `call $cel_host_*(out_slot, ...,
    mspace)` instead of just `(out_slot, ...)`.
  - **mspace epilogue** — at the end of `$eval`, call
    `mspace_destroy(msp)`.  Returns control to the host with
    the result slot's contents still valid only if the host
    copies them out first (see §5.1).

### 4.3 Lifetime issue at the boundary

The current eval shape returns an offset into the arena
that the host then decodes (via
`api/internal/abi_decode.cc`).  If the mspace is destroyed
at eval end, the offset is dangling when the host reads it.

Two ways out:

  - **Decode-in-wasm**: the eval doesn't return an offset;
    it writes the result into a host-supplied buffer (a
    "result region" malloc'd by the host before each eval).
    Decoder runs on that buffer.  Adds a copy.
  - **Defer destroy**: eval returns the mspace handle along
    with the offset; host decodes; host calls
    `cel_destroy_mspace(msp)` after.  No copy.

**Prefer "defer destroy"** — same perf as current, just one
extra trampoline call.

## 5 Host-side impact

### 5.1 Activation marshalling — strings and bytes

Today's "host string arena" hack
(`api/instance.cc::EnsureHostStringArenaCapacity` + helpers,
~110 LoC) goes away entirely.

Replacement: the activation encoder calls
`mspace_malloc(activation_msp, len)` (via wasm reentry) for
each kString / kBytes input value; writes bytes; stamps the
offset into the CelValue payload.  Activation's mspace
lives for the lifetime of one eval call, same as the eval's
own mspace.  Or both share one mspace.

  - Net code delta: −110 LoC + ~30 LoC for the new encoder.

### 5.2 `WasmtimeArenaAllocator` → `WasmtimeMallocAllocator`

`api/internal/cel_host_wasmtime.cc:155-180`'s reentry
allocator becomes a thin wrapper around `mspace_malloc`
instead of `cel_alloc`.  Same ABI (size in, offset out),
same error handling (OOM → trap), just a different export
name.

  - Net code delta: ~0 LoC; rename + signature update.

### 5.3 `cel_host_*` trampoline signatures

Every Layer-3 trampoline that allocates output
(e.g. `cel_host_cel_list_concat` returns a new list) needs
the per-eval mspace handle threaded in.  Three options:

  - **(i) Explicit mspace parameter** — each trampoline
    takes an extra i32 (the mspace handle).  Codegen sees
    the mspace local in scope at the call site (e.g. as a
    `local.tee` from the eval prologue), passes it in.
    Cleanest at the type level.
  - **(ii) Implicit via TLS** — store the active mspace in a
    fixed memory slot (analogous to the current bump cursor
    at offset 8).  Trampolines read it.  Less verbose but
    re-introduces the "fixed offset" hack we're trying to
    remove.
  - **(iii) Implicit via wasm global** — store the mspace in
    a mutable wasm global.  Slightly cleaner than (ii) but
    same semantics.

  - **Verdict: pick (iii)**.  Wasm globals are the idiomatic
    way to thread per-instance state.  One global, set by
    eval prologue, read by trampolines.  No layer changes.

  - Net code delta: ~50 LoC across the 8+ trampolines that
    currently call `cel_alloc`.

### 5.4 Memory access — wasmtime memory view

Wasmtime's `memory.grow` invalidates the host-side pointer
into linear memory.  Today our `WasmtimeMemoryView` already
re-derives the base before each access.  Under malloc this
becomes more important because dlmalloc grows memory
opportunistically.  No new code; existing safety holds.

## 6 Build system changes

### 6.1 `compiler_v2/runtime/BUILD.bazel`

Switch the `cel_runtime_wasm_file` rule from brew's clang
to wasi-sdk:

```diff
- "/opt/homebrew/opt/llvm/bin/clang",
- "--target=wasm32",
- "-ffreestanding",
- "-nostdlib",
+ "$(WASI_SDK)/bin/clang",
+ "--target=wasm32-wasi",
+ "-D_WASI_EMULATED_SIGNAL",     # if signal/etc. wanted
+ "-D_WASI_EMULATED_MMAN",
```

The right plumbing for `$(WASI_SDK)` per repo memory
(`feedback_runtime_cross_platform.md`): an
`http_archive` of wasi-sdk in `MODULE.bazel`.  Hermetic,
cross-platform — meets the "no `brew install
wasi-runtimes`" rule.

Drop `-Wl,--export=cel_alloc` and `-Wl,--export=cel_reset`.
Add `-Wl,--export=malloc` / `-Wl,--export=free` (for host
reentry; if the host doesn't need direct access, omit them
and let wasm-ld dead-code-strip).  Add
`-Wl,--export=cel_mspace_create` /
`-Wl,--export=cel_mspace_destroy` for the per-eval
lifecycle.

  - Net BUILD delta: ~10 lines changed.

### 6.2 New `MODULE.bazel` dependency

```python
http_archive(
    name = "wasi_sdk",
    urls = ["https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-arm64-macos.tar.gz"],
    ...
)
```

Plus a `wasi_sdk_arm64` / `wasi_sdk_x86_64` platform select
so Linux CI machines pick up the right tarball.  Standard
http_archive idiom; the repo's existing
`http_archive` calls are the template.

  - Net `MODULE.bazel` delta: ~20 lines.

### 6.3 Native test build (host-side)

The runtime `.c` files compile under both the native host
toolchain (for fast tests) and the wasm cross-compile.
Native build uses libc's `malloc` directly — same call
sites, same semantics.  No fork.

  - Net delta: 0 LoC.

## 7 Total work estimate

### 7.1 Per-area LoC delta

| Area | LoC removed | LoC added | Net |
|---|---:|---:|---:|
| Runtime arena (`cel_arena.{c,h}`) | 105 | 0 | −105 |
| Runtime memory (`cel_memory.{c,h}` simplification) | 50 | 20 | −30 |
| Runtime kernel `cel_alloc` call sites | 0 | 0 | 0 (rename) |
| Runtime test files | 0 | 0 | 0 (rename) |
| Codegen `EmitCelResetCall` etc. | 30 | 0 | −30 |
| Codegen mspace prologue/epilogue | 0 | 40 | +40 |
| LayoutPass arena_base | 10 | 0 | −10 |
| `compile.h` / `LoweringOptions` | 15 | 5 | −10 |
| Host string arena (`instance.cc`) | 110 | 30 | −80 |
| `WasmtimeArenaAllocator` rename | 0 | 0 | 0 |
| Trampoline mspace threading | 0 | 50 | +50 |
| BUILD.bazel + MODULE.bazel | 5 | 30 | +25 |
| New mspace lifecycle helpers | 0 | 60 | +60 |
| **Total** | **325** | **235** | **−90 LoC** |

Net is slightly smaller — surprising because we're adding
malloc — because the host-string-arena hack and the bump
arena's bespoke machinery were carrying real weight.

### 7.2 Session estimate

  - **Slice 1: wasi-sdk in MODULE.bazel + runtime builds
    against it** — 0.5 session.  No semantic change; just
    swap the build target and confirm the kernels link.
  - **Slice 2: replace `cel_alloc` with `malloc` everywhere
    in the runtime + tests** — 0.5 session.  Mechanical.
  - **Slice 3: codegen — drop `cel_reset` prologue, drop
    `arena_base` and `mem_size_bytes` from
    LoweringOptions** — 0.5 session.  Affects most
    expr_lower_test.cc fixtures; will need re-baselining.
  - **Slice 4: mspace prologue/epilogue codegen + lifecycle
    threading via wasm global** — 1 session.  This is the
    interesting design slice; locks in the per-eval scope
    semantics.
  - **Slice 5: rewrite the host string arena as
    malloc-on-activation** — 0.5 session.  Mostly deletion.
  - **Slice 6: WasmtimeArenaAllocator → MallocAllocator;
    trampoline mspace param threading** — 0.5 session.
  - **Slice 7: conformance + e2e + bench regression check
    + closeout** — 0.5 session.
  - **Slice 8: doc updates (CLAUDE.md, design.md, host
    surface doc)** — 0.25 session.

  - **Total: ~4.25 sessions** for the full migration.

  - Minimum-viable variant: Slices 1–4 only (~2.5
    sessions).  Defers host-side cleanup; conformance keeps
    passing because the wasm interface stabilizes first.
    Slice 5–6 ship as a follow-up.

### 7.3 Risk gates

  - **R1 (mid-Slice 1)**: confirm wasi-sdk's
    `--target=wasm32-wasi` actually links our kernels
    without thread/mutex pulls.  Our `cel_*.c` doesn't use
    threads; should be clean.  If it isn't, drop to
    `wasm32-wasi-threads` (adds `-pthread`, requires
    `--shared-memory`; documented in `../exp1_re2/`).
  - **R2 (mid-Slice 4)**: confirm the per-eval mspace
    approach passes the bench regression budget.  Today's
    `kernel_bench` shows ~10K eval/s on simple expressions.
    mspace-per-eval should be within 20%; if not, fall back
    to Option A (no per-eval reset; rely on Instance drop).
  - **R3 (mid-Slice 6)**: trampoline mspace threading via
    wasm global must survive nested host→wasm→host calls.
    Comprehension lowering (M5 follow-on) introduces such
    nesting (host can be called from inside a comprehension
    body).  Test before locking in.

## 8 What the prototype should produce

Following the user direction, this exploration produces
prototype code in this directory, not a milestone plan doc.
Files to be added here (over the prototyping arc):

  - `prototype_runtime.c` — a tiny standalone "C runtime"
    that uses `mspace` per-eval.  Demonstrates the
    allocation pattern end-to-end against a hand-coded
    `$eval` function.
  - `prototype_driver.wat` — calls into `prototype_runtime`
    multiple times, verifying that each eval's mspace is
    freed cleanly and memory doesn't leak.
  - `bench_compare.sh` — micro-bench harness comparing
    bump-arena (today's runtime) and mspace-based runtime
    on a synthetic workload.  Numbers go into RESULTS.md.
  - `RESULTS.md` — empirical numbers + a final go/no-go
    recommendation.

The prototype runs *separately* from the main `compiler_v2/`
build; it does not modify the existing runtime.  Lessons go
into a follow-up milestone plan after the user reviews
RESULTS.md.

## 9 What doesn't change

  - Host trampoline ABI (the inter-process boundary the
    embedder programs against).  Shape stays the same; just
    the allocation implementation under the hood changes.
  - CEL evaluation semantics.  Identical conformance row
    pass count expected — this is a refactor, not a feature
    change.
  - `cel-cpp` integration (parser + checker).  Unaffected.
  - The 24-byte `CelValue` layout.  Unaffected.
  - The `cel_host_*` import set (the runtime's external
    surface).  Same names, same signatures.
  - Browser-portability story.  wasi-sdk's malloc has zero
    WASI imports for pure allocation (proved in
    `../exp1_re2/`: 7.4 KB malloc-only wasm with 0
    imports).  So nothing breaks for browser embedders.

## 10 Comparison vs. status-quo for vendoring libraries

If we adopt this migration:

| Concern | Status quo | Post-migration |
|---|---|---|
| Vendoring RE2 | Heap corruption (dual allocators). 1-2 sessions of integration plumbing. | Just works.  `libre2.a` links cleanly. |
| Vendoring abseil pieces | Same dual-allocator issue. | Works; absl uses malloc. |
| Vendoring a parser (e.g. for timestamp/duration) | Either hand-roll in C (option D in PLAN.md) or pay integration tax. | Vendor freely. |
| Adding new host trampoline | Must use `WasmtimeArenaAllocator` reentry. | Use `MallocAllocator` reentry (~identical code). |
| Adding new runtime helper | Call `cel_alloc(n)`. | Call `malloc(n)`. |
| Closing out a milestone | Bench, conformance, lint. | Same, plus check that allocations don't leak across evals (added assertion in test harness). |
| Per-eval perf | Bump cursor: ~10 ns / alloc. | mspace: ~30-50 ns / alloc, mass-destroy at end. |
| Memory per Instance | Static (mem_size_bytes baked in). | Dynamic (grows as needed). |
| Browser compat | 0 WASI imports. | 0 WASI imports for pure allocation; new imports only if we vendor libs that need them (RE2 added 11; tiny parsers add 0). |

The big win: **the same architecture that ships the bump
arena ships RE2-in-runtime for free**.

## 11 Open questions

  1. **mspace vs. global malloc**?  Per-eval mspace is more
     work to set up but matches today's "fresh arena per
     eval" semantics.  Global malloc + manual free is
     simpler but requires care.  *Recommendation: mspace
     for the prototype; revisit after seeing bench numbers.*
  2. **wasi-sdk target: vanilla or threads**?  Vanilla
     `wasm32-wasi` is 0 WASI imports for our kernels (none
     use threading).  If we later vendor RE2 + absl,
     `wasm32-wasi-threads` becomes necessary.  Decide once,
     pick threads if we plan to vendor in the next 6
     months; vanilla otherwise.  Pivoting later is medium
     effort.
  3. **Static scratch region size**?  Option (a) in §4.2:
     reserve N KB of bss for eval scratch.  Today's
     `mem_size_bytes` default is 64 KB.  Match that for
     drop-in compatibility.  Programs that need more
     overflow into `malloc` heap dynamically.
  4. **Do we lose anything by deleting `cel_reset`?**  Yes
     — the inline-asm opacity barrier in `cel_memory.c`
     (load-bearing today) goes with it.  But wasi-sdk's
     `malloc` doesn't need that hack.  Net simplification.
  5. **Migration ordering with M5 comprehensions follow-on
     work**?  The two are technically independent.
     Comprehensions touches LayoutPass + codegen; this
     migration touches LayoutPass + codegen + runtime.
     Doing them in parallel = merge conflicts in
     `expr_lower.cc` and `layout_pass.cc`.  *Recommendation:
     ship comprehensions first (it's planned and scoped),
     then this migration.  Or block on neither — pick one
     to do solo, schedule the other for after.*

## 12 Recommendation

**Yes, migrate.**

The estimate is bounded (~4.25 sessions) and the migration
is mostly mechanical (~325 LoC removed, ~235 added).
The biggest unknowns — wasi-sdk integration and mspace
semantics — are both prototypable in this directory before
touching the main runtime, so the risk is contained.

The architectural payoff is large: any C/C++ library can
ship in the runtime; the host-side string arena hack
disappears; the codegen prologue simplifies.  And the
post-migration runtime works the way the user's old
compiler did, which is the model the user has already
validated in production.

Suggested next step: build the prototype standalone in this
directory, measure the bench delta, then write the
migration milestone doc as
`doc/implementation-plan/rewrite/m-wasi-malloc-migration.md`
after the user signs off on the empirical numbers.
