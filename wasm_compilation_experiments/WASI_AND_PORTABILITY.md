# WASI parity, emscripten, and our memory model

Status: discussion doc — drafted 2026-05-16.  Not a plan yet;
this is the reasoning we'll lock down before picking a path in
`PLAN.md`.

## 1 What "WASI parity" actually means

WASI = WebAssembly System Interface.  It is **not** part of the
wasm core spec; it is a *set of imported functions* a wasm module
asks for when it wants OS-shaped operations (clock, files,
random, sockets, env).  Each WASI function shows up in the wasm
binary as `(import "wasi_snapshot_preview1" "<fn>" …)`.

A module is "WASI-clean" if it imports zero `wasi_*` functions.
A module is "WASI-needs-X" if it imports specific WASI functions.
Compatibility = does the host (the embedder running the wasm)
provide those imports?

There are three flavours of WASI in the wild:

  - **`wasi_snapshot_preview1`** — the de-facto standard; what
    wasi-sdk emits by default; what wasmtime, wasmer, node's
    `wasi` module, and the major browser shims implement.
  - **`wasi_unstable`** — older, deprecated, still supported by
    most runtimes.
  - **`wasi_preview2` / "component model" / "WIT"** — the
    future direction; ergonomically nicer, not yet uniformly
    supported.  We don't need to worry about this yet.

For our purposes, "WASI parity" means: **the wasm we ship must
either import nothing, or import only a small, well-known subset
of `wasi_snapshot_preview1` that every embedder we care about
implements.**

### The "good" subset (browser-shim-supported, basically universal)

These are the WASI imports that essentially every shim provides
and that are cheap to stub:

  - `fd_write` — used by stdlib `printf` / `iostream`.  Most
    shims wire it to `console.log` (browser) or stderr
    (servers).  Trivial.
  - `random_get` — `crypto.getRandomValues()` in browsers,
    `/dev/urandom` on servers.  Trivial.
  - `clock_time_get` — `Date.now()` in browsers, real clock
    on servers.  Trivial.
  - `environ_get` / `environ_sizes_get` — usually shimmed to
    return zero env vars.  Trivial.
  - `proc_exit` — shimmed to throw or set an exit code.
    Trivial.

If our binary imports only these, **the browser story is fine.**
`@bjorn3/browser_wasi_shim`, Bytecode Alliance's `js-wasi`,
`@wasmer/wasi`, and (since Node 18) Node's built-in
`node:wasi` module all handle this set.

### The "bad" subset (browser-unfriendly)

These imports usually come from a wasm binary that doesn't
realise it shouldn't be doing OS-y things, and break in
browsers:

  - `path_open`, `fd_readdir`, `path_filestat_get` — filesystem
    access.  Browsers don't have one (mountable virtual FS is
    possible but heavy).
  - `poll_oneoff`, `sock_*` — async/sockets.  Browser shims
    mostly stub these to `ENOTSUP`.
  - **Threading primitives** (`thread_spawn`, `atomic.wait`
    when used cross-thread) — needs `SharedArrayBuffer` and
    cross-origin isolation headers in browsers.  Painful.
  - `args_*` — argc/argv; programs that build a CLI surface
    pull this in.  Mostly harmless but smells like "this
    code thinks it's a CLI".

**The realistic risk for option B (vendor absl):** absl's
implementation of `absl::Now()` (called transitively from
some `ParseTime` error paths to stamp error messages, IIRC)
and absl's logging (uses `std::cerr` → `fd_write`) drag in
`clock_time_get` and `fd_write`.  Both are in the "good"
subset, so it works in browsers — but the binary will
look chunky.

**The realistic risk for option D (hand-roll C):**  None.
With `-nostdlib`, we can produce a `parse_handroll.wasm`
with **zero imports** other than `memory`.  This runs in
the most constrained possible host (no shim, no JS support
code) — a pure `WebAssembly.instantiate(bytes)` and then
call exports.

## 2 Where the runtime cannot run

Cataloguing the actual deployment envelopes and what each one
constrains:

| Host | wasm core | WASI preview1 | SharedArrayBuffer | Notes |
|------|-----------|----------------|---------------------|-------|
| **wasmtime** (Rust embedder) | yes | full | yes | reference embedder; runs everything |
| **wasmer** | yes | full | yes | same |
| **Node.js ≥18** | yes | full (`node:wasi`) | yes | server-side JS |
| **Browsers (Chrome/FF/Safari current)** | yes | **no native** — needs JS shim | yes (with COOP/COEP headers) | the only awkward one |
| **Cloudflare Workers** | yes | **partial** (no FS, no sockets) | no | edge functions |
| **Fastly Compute@Edge** | yes | most of preview1 | no | edge functions |
| **Envoy proxy-wasm** | yes | **none** — proxy-wasm has its own host ABI | no | specialised |
| **eBPF / kernel wasm** | partial | no | no | toy/research |

**The two real constraints to plan for** are:

  1. **Browser** — must be reachable.  Means: either zero
     WASI imports (D), or "good subset" only (B/C with care)
     plus the user accepts pulling a JS shim into their
     bundle (~5-15 KB minified).
  2. **Edge (Cloudflare / Fastly)** — these are the
     environments most likely to bite you.  Cloudflare
     Workers' WASI support is a moving target.  If our
     wasm imports `clock_time_get` we're fine; if we import
     `fd_write` it's *usually* fine but goes through their
     stderr; if we import anything filesystem-shaped we're
     dead.  Same "good subset" rule.

**Where it absolutely cannot run regardless of options:**

  - Environments with no wasm at all (legacy embedded, very
    old browsers).  Not our problem.
  - Environments with their own ABI (Envoy proxy-wasm).
    These would need a per-environment thin shim translating
    our `wasi_snapshot_preview1` imports to their ABI.
    That's host-author work; we can ship docs but can't
    solve it from the wasm side.

## 3 Emscripten vs wasi-sdk

You asked specifically about emscripten — important
distinction.  Both are clang-based toolchains targeting wasm,
but they emit fundamentally different binaries:

  - **emscripten** emits wasm + a **mandatory JS runtime
    glue file** (`*.js`).  The JS file provides the import
    surface (its own dialect of "POSIX in JS", not WASI).
    Binaries are tightly coupled to that JS runtime.  Good
    for "drop a C++ program into a webpage", bad for
    "ship a portable wasm module that any embedder can
    instantiate".
  - **wasi-sdk** emits wasm with WASI imports.  No JS glue
    required.  Any WASI-aware embedder (wasmtime, node,
    browser-with-shim) can run it.  This is the path for
    a portable library.

For our use case — a runtime that needs to work under at
least 4 hosts (cel-wasmc/CLI, Rust, JS/browser, future Go
or Python) — **wasi-sdk is the right tool, emscripten is
the wrong one.**  Emscripten would lock every embedder to
shipping the emscripten JS shim, which:
  - is large (50-100 KB minified)
  - is opinionated about how memory and the heap work
  - is JS-specific (no help for Rust / Go / Python hosts)

There's a narrower role for emscripten: if we *only*
cared about a browser host and wanted to compile a chunky
C++ library quickly without designing a clean ABI,
emscripten would be quicker to bring up.  But it'd be a
dead end for multi-host.

**Verdict:** emscripten is off the table for this
exploration.  wasi-sdk is the comparison point for B/C/D.

## 4 Our memory model and what it implies

The runtime today (per `cel_runtime/*.c` and the
`cel_runtime_wasm.wasm` build) uses a **single linear-memory
arena model**:

  - One wasm linear memory, grown via `memory.grow`.
  - A hand-rolled bump allocator (`cel_arena.c` /
    `cel_memory.c`) gives us slot-by-slot allocation
    backed by that linear memory.
  - All CEL values live in linear memory; pointers are
    32-bit offsets; the host reads/writes via wasmtime's
    memory accessors (or equivalent in any other embedder).
  - We do **not** use `malloc`/`free`; we don't link a C
    stdlib for that.  Our allocator is the only memory
    manager.

This matters for the option choice in several ways:

### 4.1 Option B / C (vendor absl, vendor a slice)

absl assumes a real `malloc` exists.  `wasi-sdk` provides
one (dlmalloc).  Linking absl pulls in dlmalloc, which:

  - Adds 5-10 KB to binary size.
  - Allocates from the *same* linear memory we manage with
    our bump allocator.

The cleanest fix: configure dlmalloc to allocate from a
**dedicated region** of linear memory, separate from our
arena.  Two options:
  - (a) Reserve the first N MB for our arena, give dlmalloc
    a fixed offset to start `sbrk`-ing from above that.
  - (b) Run dlmalloc on a separate wasm memory (wasm
    supports multiple linear memories since the
    multi-memory proposal; widely available).

Either works.  Option (a) is simpler and what wasi-sdk
expects out of the box — just set `--initial-memory` and
let our arena live below the data segment offset that
clang places, with dlmalloc taking the rest.

There's a subtler concern: **memory growth.**  Our arena
grows by `memory.grow`.  dlmalloc also grows by
`memory.grow`.  If both call `memory.grow` independently,
the heap is corrupted — neither sees the other's
allocations.  The fix is a single shared allocator
underneath, but that means substituting our bump for
dlmalloc, *or* gating all `memory.grow` calls through one
of the two.  Real engineering work — probably 1-2 days.

### 4.2 Option D (hand-roll)

Zero new allocator.  The hand-rolled parser writes to a
caller-supplied output buffer in linear memory; allocation
is done by our existing arena before calling.  No
integration story needed.  This is the single biggest
operational advantage of D.

### 4.3 Implication for `WASI_AND_PORTABILITY` decisions

If B/C win on parity/size grounds, **we still pay an
allocator integration tax** of 1-2 days that doesn't
show up in any of the §3 experiments as currently
scoped.  We should add an Exp 5b — "does linking
dlmalloc + absl + our arena into one wasm module work
without corrupting either heap?"  That's where B/C
either lives or dies in practice.

### 4.4 Browsers and our memory

Browsers don't constrain our memory choices — they
provide `WebAssembly.Memory` and we use it.  The only
extra wrinkle:

  - `SharedArrayBuffer` (needed if we ever go
    multi-threaded inside the wasm runtime) requires
    COOP/COEP headers in the embedder page.  Annoying
    but not a runtime problem.
  - Memory64 (64-bit linear memory) is not yet baseline
    in browsers; we're on memory32 (max 4 GB linear
    memory).  Plenty for CEL evaluation.

## 5 Updated decision matrix

Adding the WASI/portability axis to the option grid:

|  | WASI imports | Browser-direct (no shim) | Edge-runtime OK | Memory integration |
|--|---------------|--------------------------|------------------|---------------------|
| **A** host trampoline | depends on host | yes | yes | no change |
| **B** vendor absl | likely `fd_write`, `clock_time_get` | needs shim | usually OK | dlmalloc + our arena merge |
| **C** vendor slice | maybe none if pruned | maybe | yes | maybe-merge needed |
| **D** hand-roll C | none | yes | yes | no change |
| **E** hybrid | mix of A's + D's import sets | yes | yes | no change |

The "browser-direct" column is the swing factor.  Only D
runs in a stripped-down browser with no JS shim.  Every
other option pulls at least `clock_time_get` /
`fd_write` and thus needs ~10 KB of JS shim alongside.
That's not a deal-breaker but it's a real per-embedder
cost.

## 6 What this changes about the experiment plan

The §3 plan in `PLAN.md` already covers most of this, but
two additions:

  - **Add Exp 5b** — "allocator integration": link
    `parse_absl.wasm`'s dlmalloc against our arena
    allocator in a single test binary; assert no heap
    corruption under churn.  This is the load-bearing
    risk for B/C beyond binary size.
  - **Capture WASI import lists explicitly** for each
    wasm artefact — not just count, but full names and
    whether each is in the "good subset" from §1.
    Decision against the matrix in §2 is mechanical
    after that.

## 7 Net guidance ahead of running experiments

  - If you want the runtime to run in a **plain
    `WebAssembly.instantiate(bytes)` call with no JS
    shim**, only D delivers that.
  - If "JS shim of ~10 KB is fine on browser hosts", B,
    C, and E are all back in scope.
  - Emscripten is not the answer for any of our cases.
  - Memory-model integration is the silent tax on B/C
    that the size-comparison doesn't surface.  Either
    we wear it (1-2 days of real plumbing) or we go D.

These are the constraints to confirm before running the
expensive experiments (Exp 1, 2, 7).  Exp 3 (hand-roll)
is cheap enough we can just do it regardless.
