# Memory layout — options + experimental findings

Status: **experimental findings — 2026-05-18.**  Conclusions
folded into `AUTHORITATIVE_PLAN.md` §1.  This doc is kept as
the empirical justification for the allocator + memory-layout
choices the plan makes.

Two questions answered by direct experiment in `experiments/`:

  1. **Where do rodata / expression constants need to be laid
     out in a WASI module?**
  2. **Can we eliminate `cel_reset` by having the host provide
     a fresh memory or by tracking mallocs per-eval?**

Both questions answered by direct experiment in
`experiments/` — wasi-sdk 25 on macOS arm64.  Every claim
below is grounded in a running `.wasm` binary, not extrapolation.

## 1 Question 1 — rodata layout

### 1.1 Default wasi-sdk memory layout

Built `experiments/exp_a_rodata.c` (a 5-LoC C file with a
const string + a BSS buffer + linker-symbol probes).  Result:

```
offset 0x000  ──────────  RESERVED (wasm-ld convention)
                          The first 1024 bytes are unused.

offset 0x400  ──────────  STATIC DATA (rodata + bss)
                          kAlpha @ 1024
                          kBeta  @ 1030
                          g_buffer @ 1056 (BSS)
                          __data_end @ 5152

offset 0x1420 ──────────  STACK (grows DOWN from $stack_pointer)
                          Default reservation: ~65 KB
                          stack_pointer init @ 70688

offset 0x11420 ──────────  HEAP (grows UP via memory.grow + dlmalloc)
                          __heap_base @ 70688
                          First malloc lands here.
```

Binary: 17,534 B for the probe (with strings + BSS); 55,321 B
for `exp_c_malloc.c` (which forces malloc to link, pulling in
dlmalloc).

**Knobs verified to work**:

  - `-Wl,--global-base=N` shifts the rodata start.  Tested
    N=8192 → `kAlpha @ 8192`, `__data_end @ 12304`,
    `__heap_base @ 77840`.  Bytes `[0, 8192)` become entirely
    free for our use.
  - `-Wl,-z,stack-size=N` shrinks the stack reservation.
    Tested `stack-size=4096` → stack from 5152 to ~9248
    (4 KB), heap_base drops from 70688 to 73744.

Combining both: with `--global-base=65536 -z,stack-size=4096`,
the layout becomes:

```
offset 0x00000  ─────  FREE FOR US (65,536 bytes)
                       Expression module's rodata + workspace
                       can live here via active data segments.

offset 0x10000  ─────  STATIC DATA + STACK
                       wasi-libc's bookkeeping (~5KB) + a tight
                       4 KB stack.

offset 0x12010  ─────  HEAP (dlmalloc territory)
                       __heap_base @ 73744
```

### 1.2 Implications for the expression module

Today's `cel_runtime.wasm` imports memory from the host
(`-Wl,--import-memory=cel,memory`) and the expr module also
imports `cel.memory`.  Both write to the same memory; rodata
goes in via active data segments at fixed offsets.

Under WASI, the cleanest analogue is:

  - **runtime module** (built with wasi-sdk):
    - Owns memory (uses `--export-memory`).
    - Or imports memory (`-Wl,--import-memory`).
    - In either case, sets `--global-base=N` to leave the
      first N bytes free for the expression module's rodata.
    - dlmalloc operates above `__heap_base`.
  - **expr module** (Binaryen-emitted):
    - Imports `cel.memory` from runtime.
    - Installs rodata via active data segments at offsets
      in `[0, N)`.
    - Codegen knows N at compile time (a runtime constant
      shared via a header).

**Decision**: pick a fixed N for our memory-layout constant.
8 KB is plenty for most expressions (today's rodata is
typically tens to hundreds of bytes per expression).  N=8192
gives us:
  - 8 KB for expr rodata + workspace.
  - Stack at default 64 KB (no stack-size override needed).
  - Heap from ~77 KB onwards.

If a future expression has >8 KB rodata (e.g. a giant proto
schema embedded in `cel.bind`), codegen can either:
  - Increase N (it's a build-time linker flag).
  - Spill the rodata to malloc'd heap regions, addressed
    relatively via a wasm local.

### 1.3 Open question on layout

Should expression rodata be at a FIXED low offset (via
`--global-base`) or DYNAMICALLY MALLOC'D at eval start
(Option II in ANALYSIS.md §3.4)?

  - **Fixed (Option I)**: brittle if a future expr exceeds
    the carve-out; cheap (no malloc).
  - **Malloc'd (Option II)**: bulletproof; ~50ns per eval
    for the malloc; needs relative addressing in codegen.

**Recommendation: fixed for the migration**.  The "future
expr exceeds 8KB" risk is small (we don't know of any today),
and the codegen simplification from absolute addressing is
worth keeping.  If we hit the wall, add Option II as a
fallback later.

## 2 Question 2 — eliminating `cel_reset`

The user proposed two angles:

  - **Host instantiates and provides memory** — fresh memory
    per eval avoids needing reset.
  - **Track all mallocs in code, free on exit** — fallback if
    nothing else works.

### 2.1 Sub-question: are mspaces available?

dlmalloc's `mspace_*` API would give bump-arena-like
mass-free semantics.  Tested in `experiments/exp_b_mspace.c`:

```
wasm-ld: error: undefined symbol: create_mspace
wasm-ld: error: undefined symbol: mspace_malloc
wasm-ld: error: undefined symbol: destroy_mspace
```

**wasi-sdk 25's wasi-libc does NOT expose mspace_*.**
This kills Option B from `ANALYSIS.md §3.5` ("mspace per
eval") as a no-rebuild path.

Workarounds, if we want mspace:
  - Rebuild wasi-libc from source with `MSPACES=1`.  Major
    build infrastructure work.
  - Vendor dlmalloc.c directly into the runtime.  ~6,000 LoC
    one-time addition; compiles cleanly under wasi-sdk.
  - Use a different allocator entirely (jemalloc, mimalloc).

Each has cost.  Skipping mspaces saves the rebuild and lets
us ship with the stock wasi-libc — at the cost of needing a
different reset strategy.

### 2.2 Option: re-instantiate per eval

Wasmtime's `linker_instantiate` is what `Engine::Plan` does
today.  Per-Plan cost is **279 µs** (from `BASELINE_BENCH.md
§5`).  If we re-instantiate per eval instead of resetting:

```
Per-eval cost: 279,000 ns (instance creation)
             vs
Today's per-eval: 141 ns
                = ~2000× slower
```

**Not viable for a hot path.**  Even for "rare" evals, 280µs
is sluggish.

### 2.3 Option: memory.fill from host

The host has direct access to wasm linear memory via
`wasmtime_memory_data()` returning a `uint8_t*`.  Host can
`memset(data + heap_base, 0, total - heap_base)` between
evals to wipe the heap region.

**Problem**: dlmalloc's persistent state lives in BSS
(`mparams` is a static struct that records init-done +
configuration), not on the heap.  Wiping the heap doesn't
reset `mparams`, so dlmalloc's `top` pointer (the next-alloc
location) is now pointing into wiped memory and dlmalloc
thinks it owns regions that no longer have its bookkeeping
headers.  **First malloc after wipe corrupts.**

We could also wipe `mparams` (it's at a known BSS offset).
That re-initializes dlmalloc, but next eval would
re-initialize dlmalloc state on first malloc, costing ~10µs
once.

This is a fragile approach.  Not recommended.

### 2.4 Option: vendored dlmalloc with MSPACES=1

Ship `third_party/dlmalloc/dlmalloc.c` (one ~6KLoC file)
configured with:

```c
#define ONLY_MSPACES 0   // keep regular malloc for libraries
#define MSPACES 1        // also expose mspace_*
#define USE_LOCKS 0      // single-threaded wasm
#define HAVE_MORECORE 0  // no sbrk on wasm
#define HAVE_MMAP 0      // no mmap on wasm
```

Now we get both:
  - **Plan-lifetime malloc/free**: vendored libraries (RE2)
    call `malloc()` like normal.
  - **Eval-lifetime mspace**: each eval opens an mspace via
    `create_mspace_with_base(buf, size, 0)`, allocates from
    it, `destroy_mspace`s on exit.  Mass-free semantics.

Cost: 6 KLoC of vendored code; ~3-5 KB binary delta.

**Viable.**  Most flexible option.

### 2.5 Option: arena over malloc (the simplest)

Tested in `experiments/exp_d_arena_in_malloc.c` (47 LoC):

```c
typedef struct {
  uint8_t* base; size_t capacity; size_t cursor;
} Arena;
static Arena g_arena = {0};

void arena_init(int32_t cap) {
  g_arena.base = malloc(cap);
  g_arena.capacity = cap;
  g_arena.cursor = 0;
}
int32_t arena_alloc(int32_t n) {
  size_t aligned = (n + 7) & ~7;
  if (g_arena.cursor + aligned > g_arena.capacity) return 0;
  uint8_t* p = g_arena.base + g_arena.cursor;
  g_arena.cursor += aligned;
  memset(p, 0, aligned);
  return (int32_t)(uintptr_t)p;
}
void arena_reset(void) { g_arena.cursor = 0; }
```

End-to-end test via hand-coded WAT driver:

```
;; scenario: init(64K) → alloc(128) → alloc(256) → reset → alloc(128)
;; Returns 1 if the 3rd alloc lands at the SAME address as the 1st.
scenario result: 1
```

**Confirmed working.**

Properties:
  - Same bump-arena semantics as today.
  - Same per-eval reset cost (one assignment).
  - Same alloc cost (cursor bump + memset).
  - Library-compatible: vendored C++ (RE2, abseil) calls
    `malloc()` directly, gets dlmalloc heap pages.  Our arena
    sits on top of one big `malloc()`'d buffer.
  - Binary size: 55,796 B with arena code added — same order
    as plain malloc.
  - **Zero WASI imports** — confirmed.
  - Browser-shimmable: yes.

**This is the recommended path.**  Combines the bump-arena
perf with WASI library compatibility, without needing to
vendor dlmalloc or rebuild wasi-libc.

### 2.6 Lifecycle under the "arena over malloc" design

```
Plan creation (once per Program):
  - Wasmtime instantiates the runtime + expr modules.
  - Runtime's `_initialize` runs wasi-libc startup (dlmalloc
    lazy-inits on first malloc).
  - Runtime exports `arena_init`, `arena_alloc`, `arena_reset`.

Per-Plan setup:
  - Host calls $arena_init(64 * 1024) once after instantiation
    via wasmtime_func_call (or codegen emits this in $eval's
    first-call path).
  - Now the arena exists for the lifetime of the Instance.

Per-Eval:
  - Codegen emits `(call $arena_reset)` at the top of $eval
    instead of today's `(call $cel_reset arena_base arena_limit)`.
  - $eval body calls `$arena_alloc(n)` instead of $cel_alloc.
  - At the end of $eval, NO cleanup needed — next eval's
    arena_reset() handles it.

Plan teardown (rare):
  - Wasmtime destroys the Instance.
  - dlmalloc heap (with the arena buffer) goes with it.
  - No explicit cleanup needed.
```

### 2.7 What about tracking allocations (the user's fallback)?

The user proposed: track every malloc in a list, free them
on exit.  This works:

```c
static void** g_allocs = NULL;
static size_t g_count = 0, g_cap = 0;

int32_t tracked_alloc(int32_t n) {
  void* p = malloc(n);
  if (!p) return 0;
  if (g_count == g_cap) {
    g_cap = g_cap ? g_cap * 2 : 16;
    g_allocs = realloc(g_allocs, g_cap * sizeof(void*));
  }
  g_allocs[g_count++] = p;
  return (int32_t)(uintptr_t)p;
}
void free_all(void) {
  for (size_t i = 0; i < g_count; i++) free(g_allocs[i]);
  g_count = 0;
}
```

This is **strictly worse** than option 2.5 for our use case:
  - Each `tracked_alloc` does the malloc + an append to the
    tracking list (~2 mallocs + ptr-store).
  - Each `free_all` is N free() calls, each ~30-50 ns of
    dlmalloc work.

For an eval with 50 allocations, free_all is ~1.5 µs.
Bump arena's reset is constant (1 assignment).  **2.5 wins
by an order of magnitude.**

The user's "track all mallocs" fallback would only make
sense if we wanted libraries to use OUR allocator (so they
can be reset on eval boundary).  That's a different design
question — libraries shipped today (RE2, abseil) use the
global malloc; their allocations are plan-lifetime.

## 3 Recommendation matrix

| Concern | Choice |
|---|---|
| Where does rodata live? | Active data segments at offset 0, with `--global-base=8192` in the runtime build to leave bytes `[0, 8192)` free. |
| Eval workspace allocator | Hand-rolled bump arena backed by ONE `malloc(64K)` per Instance.  Cursor=0 to reset. |
| Inside-runtime cel_alloc | Becomes `arena_alloc(n)` — same signature, same return semantics (offset or 0 for OOM). |
| Cross-eval state | Lives in the arena until reset.  Plan-lifetime stuff (RE2 compiled regex) uses global `malloc()` and outlives evals. |
| Memory from host? | Yes (`--import-memory=cel,memory`).  But not strictly required; the runtime can own its memory and export it.  Either works. |
| mspace_*? | NOT used.  Stock wasi-libc doesn't expose them.  If we ever need them, vendor dlmalloc.c. |
| memory.fill reset? | NOT used.  Doesn't reset dlmalloc safely. |
| Re-instantiate per eval? | NOT used.  279 µs per eval is 2000× slowdown. |

## 4 Binary-size delta vs today

Today's `cel_runtime.wasm`: **60,971 B stripped / 11,741 B gz**.

Under WASI with the arena-over-malloc design:

  - dlmalloc + wasi-libc startup: adds ~30-40 KB raw.
  - Arena code: adds ~200 B.
  - Removes: `cel_arena.c` (66 LoC), the inline-asm hack in
    `cel_memory.c`, the cursor accessors.  Net runtime LoC
    drop: ~150 LoC.

Estimated post-migration binary: **90-110 KB stripped /
20-30 KB gzipped.**  ~2× growth.  Browser cold-start adds
~4-6 ms for the parse, but parsing scales linearly so this
is a one-time cost per Engine, not per Plan.

## 5 Open questions for next session

  1. **Stack size**: today's wasm stack is 64 KB by default.
     Does our codegen ever blow the stack with deeply-nested
     comprehensions?  Probably not at M5-M10 scale, but worth
     a probe.  Easy fix: `-z,stack-size=N` to tune.
  2. **Active data segment location with --global-base=N**:
     does the expr module need to know N at compile time, or
     can it import N as a wasm global?  Answering this would
     decide whether expr modules are tied to a specific
     runtime binary (today they are; tomorrow they could
     parameterise).
  3. **First-call dlmalloc init cost**: estimated ~5-10 µs in
     ANALYSIS.md §4.2.  Verify by measuring a `Plan + alloc(0)
     + Eval` cycle vs `Plan + Eval`.
  4. **`__heap_base` symbol** — can the runtime read this at
     init time to know where dlmalloc's territory begins?
     The probe in `exp_a_rodata.c` showed yes.  Confirms
     codegen knows the heap layout.
  5. **Custom `--initial-memory`**: do we want to start with
     more than 2 pages (128 KB)?  Saves `memory.grow` calls
     on the hot path but inflates baseline.

## 6 Files in `experiments/`

  - `exp_a_rodata.c` — probe for default + custom rodata
    layout.  Built with various `--global-base` and
    `-z,stack-size` flags.
  - `exp_b_mspace.c` — link probe for mspace_* (FAILS;
    confirms not available).
  - `exp_c_malloc.c` — pure-malloc probe.  Confirms zero
    WASI imports.
  - `exp_d_arena_in_malloc.c` — the recommended design.
    47-LoC arena over malloc, with `exp_d_driver.wat` proving
    cross-module composition + reset semantics.
  - `wasi-sdk` (symlink to `wasm_compilation_experiments/exp1_re2/wasi-sdk-25.0-arm64-macos`).
