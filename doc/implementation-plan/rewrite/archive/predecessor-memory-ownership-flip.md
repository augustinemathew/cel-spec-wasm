# Memory ownership flip: expr module defines + exports memory

Status: **superseded 2026-04-21 by
[`rewrite-memory-layout-codegen.md`](rewrite-memory-layout-codegen.md).**
The rewrite doc adopts the flip (expr module owns memory; runtime
imports) with the arena cursor at fixed memory offsets 8..15 rather
than wasm globals. Kept here for historical context only — do not
treat this as the active plan.

---

Status (original): **open — investigation requested 2026-04-20.**
Not yet scheduled against a milestone.

## The proposal

Today:

  - `runtime.wasm` **defines** `memory` and `cel_alloc` /
    `cel_reset` / `cel_make_*` / 3VL helpers / checked-arithmetic
    helpers.  It `export`s `memory` and every `cel_*` function.
  - `expr.wasm` **imports** `memory` and every `cel_*` it uses from
    the `"cel"` namespace (rebound at link time from the runtime
    instance's exports).
  - One runtime instance is shared across many expr instances.  To
    keep evaluations independent, the host calls `cel_reset` on the
    runtime between `$eval` invocations to rewind the bump arena
    back to `g_static_end`.

The proposal: flip the memory ownership.

  - `expr.wasm` **defines + exports** its own `memory`.  Each eval
    module carries its own linear memory.
  - `runtime.wasm` **imports** `memory` from the expr module (or
    the runtime stops being a separate module altogether and its
    helpers are inlined / linked into every expr module).
  - `runtime.wasm` becomes **stateless** — no `g_memory[]`, no
    `g_static_end`, no arena cursor.  The allocator (if it stays
    in the runtime) becomes a pure function of an `(mem_base,
    cursor)` pair that the expr module owns.
  - Between calls, the expr instance is dropped (or the arena
    cursor in the expr module is rewound to a known boundary).
    There is no shared `cel_reset` to call.

## The rationale

From the user's framing (2026-04-20):

  1. **Clean memory per eval.**  Each expression evaluation gets
     fresh memory by construction.  No carry-over from a prior
     eval that forgot to wipe a buffer.  No cross-eval corruption
     window in hosts that reuse the runtime instance.
  2. **Truly stateless runtime.**  The runtime becomes a pure code
     module — deterministic functions over caller-supplied memory.
     This matches how most wasm runtimes think about "stateless"
     code, and removes a footgun: today the runtime's `g_memory`
     is mutable global state that every eval silently touches.
  3. **Thread-safety by default.**  §7.3.1 of the design doc
     enumerates today's "one instance per thread" constraint,
     driven by the shared `g_memory` array and the `g_alloc_ptr`
     global.  If memory moves to the expr module, two threads each
     instantiating their own expr module are naturally isolated —
     the "external mutex or per-thread instance" workaround
     disappears for the common case.

## Non-trivial questions the investigation needs to answer

This is not an obvious win — the runtime was designed with `g_memory`
deliberately owned, and the user is asking whether inverting that is
worth the cost.  The investigation should answer:

  1. **Does Binaryen / wasm actually let a compiled-C module
     import `memory` rather than define it?**  The runtime is
     built by cross-compiling `cel_runtime.c` with brew clang
     (`--target=wasm32`), and the compiler's output is currently
     assumed to include a `memory` definition.  Confirm the
     toolchain can emit an imported-memory variant, or identify
     what stands in the way.
  2. **Instance lifetime cost.**  What does "drop the expr
     instance between evals" cost under wasmtime / other runtimes
     vs. today's `cel_reset` fast-path?  Appendix of the bench
     suite should get a row for both shapes before a choice is
     made.  If instantiation is >10× the cost of `cel_reset`, the
     flip only makes sense if the user workload already
     instantiates fresh per eval (which many hosts do — the
     design-doc §7.3 thread-safety story implicitly assumes this).
  3. **Does the runtime genuinely become stateless, or does some
     state move with it?**  Today the runtime holds:
       - `g_memory[]` — the linear memory itself.
       - `g_alloc_ptr` — the bump cursor.
       - Interned literal data segments emitted by codegen into
         the runtime's data section (today's layout).
       - Runtime-side "reserve-on-first-call" buffers (none today
         but trivially could exist tomorrow).
     Enumerate which of these move with memory, which stay, and
     which need a new home (e.g. interned literals should move to
     the expr module's data segment anyway — the codegen path
     arguably already assumes this).
  4. **How does this interact with `cel_refs`?**  The externref
     table today is a runtime global.  Does it move with memory,
     or does the proposal leave it on the runtime side and accept
     that "runtime holds table state but no linear memory"?  If
     the latter, the "truly stateless" claim needs a footnote.
  5. **Host loader changes.**  `compiler/host/host_loader.{h,cc}`
     assumes "one runtime instance, many eval instances, rebind
     runtime exports into the 'cel' namespace on the eval linker."
     Sketch the new loader flow: who instantiates first, how the
     memory import resolves, and whether runtime instantiation
     still exists as a concept.
  6. **3VL / partial-eval impact.**  The sret slot allocation in
     codegen (`cel_alloc(24)` at eval entry) crosses the
     memory-owner boundary today.  If memory lives in the expr
     module but the allocator lives in the runtime, `cel_alloc`
     becomes `(module.memory, module.cursor_global) → i32`.  Is
     that a useful abstraction (the allocator is generic and
     reusable) or a complication worth avoiding (just inline the
     allocator into every expr module)?
  7. **Binaryen emission path.**  `compiler/codegen/module.cc`
     emits the expr module today with an imported memory.
     Flipping to defined-and-exported is a small Binaryen API
     change; the `cc_wasm_library` toolchain for the runtime is
     the riskier piece.

## Deliverable

A decision doc (update this file) that either:

  - **Recommends the flip.**  Includes a migration plan: runtime
    changes, codegen changes, host-loader changes, bench baselines
    to clear, and a test plan that covers the "fresh memory per
    eval" invariant directly (not by proxy via `cel_reset`).
  - **Recommends keeping the status quo.**  Identifies the blocker
    (toolchain, instantiation cost, externref table ownership, …)
    and — if the blocker is surmountable later — flags when to
    revisit.  The "truly stateless runtime" property may still be
    worth pursuing piecemeal (e.g. move interned literal data to
    expr-side even if memory stays runtime-side).

Either way, close the open item in `doc/wasm-compiler-design.md`
§7.3 once a decision lands — today that section ambiguously
describes the runtime's instance ownership.

## Findings and recommendation

**Summary.** Recommendation: **phased — do the flip, but only after
a throwaway feasibility build proves the wasm32 runtime link works
with `-Wl,--import-memory`, and only after an eval-instantiation
cost measurement (not currently captured by the bench suite) stays
within the same order of magnitude as today's `cel_reset` fast
path.**  The flip is small on the codegen side,
toolchain-supported on the runtime side, and cleanly resolves the
§7.0.1 "single-threaded contract" footgun.  The externref table
(Question 4) turns out to be a non-issue — it's already
per-eval-module — and interned literal data segments (Question 3)
don't exist today, which simplifies the migration.  The honest
framing: flip is a worthwhile cleanup plus a real
thread-safety-by-default win, not a correctness fix for a bug
anyone is hitting today.

### Per-question answers

**1. Does the toolchain support an imported-memory runtime?**  Yes.
`/opt/homebrew/bin/wasm-ld --help` lists
`--import-memory=<module>,<name>` and a no-arg
`--import-memory` (defaults to `env.memory`).  The current
runtime genrule at `compiler/runtime/BUILD.bazel:50-61` passes
`-Wl,--no-entry -Wl,--export-all`; replacing `--export-all` with
`--import-memory=cel,memory` plus an explicit `--export=cel_*`
allow-list is a mechanical change.  I did **not** build this to
confirm — the claim "wasm-ld accepts the flag" is strong; the
claim "the resulting module instantiates cleanly against a
wasmtime-provided memory, preserving `static uint8_t
g_memory[…]` semantics" needs a throwaway build to be sure.
Risk: `static uint8_t g_memory[CELWASM_ARENA_BYTES]` at
`compiler/runtime/cel_runtime.c:57` lives in BSS, which
`--import-memory` lays out into the imported memory at
`--global-base`.  That *should* work, but the
`memset(g_memory, 0, sizeof(g_memory))` in
`ensure_initialized()` at `cel_runtime.c:83` now zeroes a region
of the expr module's memory — worth a smoke test.

**2. Instance-lifetime cost.**  Not measurable from the bench
suite as it stands.  `compiler/bench/bench_fixture.cc:24-43`
calls `LoadEval` **once** per compile; the hot loop at
`compiler/bench/eval_bench.cc:105-115` spins on `CallEval`,
which in turn (`compiler/host/host_loader.cc:406-452`) is
`cel_reset` + `cel_alloc(24)` + `eval(…)` + `cel_mem_base()` —
no instantiation.  Today's benchmarks measure the
**steady-state** cost of the current model and compare it to
nothing.  Expectation, unverified: a
`wasmtime_linker_instantiate` on a pre-compiled module is
~µs-range; `cel_reset` is a single store to
`g_cel_arena.bump` — nanoseconds.  A 100–1000× gap is
plausible.  **Precondition for the flip:** add a bench row that
re-instantiates per iteration before locking the decision in.
If wasmtime instantiation is cheap because the module is
cached, the gap narrows; if not, the flip only makes sense for
hosts that *already* instantiate fresh per eval.

**3. Does the runtime genuinely become stateless?**  Mostly,
yes.  Inventory of mutable state in `cel_runtime.c`:
  - `g_memory[CELWASM_ARENA_BYTES]` (line 57) — moves with
    memory.
  - `g_cel_arena` (line 67) — a struct of two `uint32_t`s;
    `--import-memory` places it in the imported memory's BSS, so
    it physically rides along.
  - `g_singleton_*_off`, `g_static_end` (lines 61-65) —
    initialised once in `ensure_initialized`, never written
    after.  Read-only after init; per-instance semantics fine.
  - **No codegen-emitted data segments.**  `Grep
    "BinaryenAddDataSegment"` across the tree returns zero hits;
    `BinaryenSetMemory` at `compiler/codegen/module.cc:90,150`
    is called with `numSegments=0`.  String literals are
    constructed at runtime via `cel_alloc` +
    `cel_make_string_view`
    (`compiler/codegen/expr_lower.cc:366-371,469`), so there
    is no interned-literal data segment to migrate.  The
    original "interned literals should move to expr side" worry
    is already moot — nothing lives in a data segment on either
    side today.
  - **`cel.abi` custom section** is already in the expr module
    (parsed at `compiler/host/host_loader.cc:226-253`).
Net: the flip cleanly moves `g_memory` and `g_cel_arena`.  The
runtime `.wasm` becomes pure code plus `__wasm_call_ctors` init.

**4. `cel_refs` externref table.**  Non-issue.  The table is
**already** per-expr-module (`compiler/codegen/cel_refs.h:60-62`
emits `$cel_refs` into each eval via
`AddCelRefsTableAndHelpers`), and `doc/wasm-compiler-design.md`
§7.1 explains why: the wasm32 C cross-compile can't author
`table.set`/`table.get` on externref, so the table was lifted
to Binaryen-emit-time in codegen.  The runtime never held a
refs table.  No table-migration work; the "truly stateless
runtime" claim survives intact on this axis.

**5. Host loader changes.**  `compiler/host/host_loader.cc`
does three steps today: (a) compile both modules, instantiate
runtime bare (`InitEngineStoreAndCompile`, lines 468-492);
(b) define runtime instance under `"cel"` on a linker and
instantiate eval against it (`SetupLinkerAndInstantiateEval`,
494-552); (c) bind `cel_ref_intern` off the eval instance back
onto `CelHostEnv` (line 547).  After the flip, the ordering
inverts and there is a circular-import risk: runtime imports
`cel.memory` from expr, expr imports `cel.cel_*` from runtime.
Two resolutions:
  1. **Two-phase instantiation.**  Instantiate expr first
     (imports are now only `cel.cel_alloc`, `cel.cel_make_*`,
     etc. — which we resolve with *lazy trampolines* on the
     linker that will delegate to the runtime once it's
     instantiated); then instantiate runtime with expr's memory
     (pulled off the fresh instance via
     `wasmtime_instance_export_get` + `wasmtime_linker_define`);
     then rewire `CelHostEnv` to resolve its three trampoline
     deps (`cel_alloc`, `cel_mem_base`, `memory`) off the new
     runtime instance.  Fiddly but wasmtime supports the
     primitives.
  2. **Inline runtime into every eval module** via the "merge
     step" §7.0 describes and explicitly punted on: read
     `kCelRuntimeWasmBytes` via `BinaryenModuleRead` at codegen
     time, splice functions/globals into the expr module, emit
     one monolith.  Kills the "tiny per-expression modules"
     benefit (~2 KB runtime × N evals instead of amortised), but
     the host loader collapses to a single instantiation.
Option (1) preserves the current module topology; option (2) is
simpler at the host-loader level but more expensive per compiled
module.  My weak preference: option (1) — it keeps the shipped
`.wasm` size story the §7.0 doc sells.

**6. 3VL / sret allocation.**  Cleanest part of the flip.
Today `compiler/codegen/expr_lower.cc:1305,1531` and
`compiler/host/host_loader.cc:429` call `cel_alloc(24)` for
sret slots.  After the flip, `cel_alloc` still resolves to the
runtime's implementation — which already is
`g_cel_arena.bump += need` against a `g_memory` that's "whatever
linear memory I was linked into" (see
`compiler/runtime/cel_runtime.c:135-146`).  The C source doesn't
change; the wasm linker just wires the same bump logic against
the expr module's memory.  No new abstraction needed; the
allocator is already memory-generic because C + wasm-ld make it
so.

**7. Binaryen emission path.**  Trivial.  `WasmModule` already
exposes both APIs: `SetMemory(initial, max, export_name)` at
`compiler/codegen/module.h:63` / `module.cc:76-105` (defines +
optionally exports) and `AddMemoryImport(mod, base, initial,
max)` at `module.h:90` / `module.cc:129-169` (imports).
`expr_lower.cc:246-249` currently calls the latter; the flip
calls the former with `export_name="memory"`.  One-line change.

### Migration sketch (if greenlit)

Four slices, each independently testable:

1. **Feasibility spike (~1 day).**  Edit
   `compiler/runtime/BUILD.bazel:50-61`: replace
   `-Wl,--export-all` with `-Wl,--import-memory=cel,memory
   -Wl,--export=cel_*`.  Build with `bazel build
   //compiler/runtime:cel_runtime_wasm_file`.  Confirm
   Binaryen/wasmtime decode the resulting module.  If it
   fails, recommendation inverts to "don't flip" — file the
   reason in this doc.
2. **Bench baseline (~0.5 day).**  Add
   `BM_InstantiatePerCall` to `compiler/bench/eval_bench.cc`
   that re-runs `LoadEval` per iteration; add a cheaper
   `BM_LinkerInstantiatePerCall` that reuses the compiled
   `eval_mod_` but re-runs `wasmtime_linker_instantiate`.
   Compare both to `BM_IntLiteral`.  If the gap is >100× for
   the linker variant, surface to user before proceeding.
3. **Codegen + loader (~2-3 days).**  Flip
   `DeclareRuntimeImports`
   (`compiler/codegen/expr_lower.cc:243-257`) from
   `AddMemoryImport` to `SetMemory("memory")`; invert
   instantiation order in
   `compiler/host/host_loader.cc:SetupLinkerAndInstantiateEval`
   to instantiate expr first, then runtime with
   `wasmtime_linker_define` of expr's memory.  Rewire
   `CelHostEnv::Init`
   (`compiler/host/cel_host_wasmtime.h:79`) to resolve
   `cel_alloc` / `cel_mem_base` / `memory` off the expr
   instance (or the runtime instance, if option-1
   two-phase).  Drop the pre-call `cel_reset` in
   `host_loader.cc:418-422` — the replacement is either
   "reinstantiate eval" (clean-per-call) or "keep `cel_reset`
   on the expr instance" (same semantics as today, just
   per-expr rather than shared).
4. **Tests + design doc (~1 day).**  Add a regression test
   that instantiates two expr modules against one runtime
   stub, interleaves `CallEval` on both on one thread with
   distinct inputs, and verifies no arena cross-talk.  Update
   `doc/wasm-compiler-design.md` §7.0 / §7.0.1 to reflect the
   new shared-code/independent-memory story; close the open
   bullet.  Update `doc/implementation-plan/testing-checklist.md`
   with a "memory-isolation" row.

### Open questions for the user

  - **Is the loss of "one `cel_reset` wipes all shared state"
    an acceptable behavioural break?**  Under the flip, each
    expr instance still exports `cel_reset`, but there's no
    cross-instance equivalent.  §7.0.1 implies today's common
    pattern is "one eval instance per runtime, `cel_reset`
    between calls" — that pattern continues to work per-instance
    under the flip.  Confirm nothing in the roadmap assumes
    cross-expr reset.
  - **Is a slight per-module byte increase acceptable** if
    option-2 (inline runtime into every eval) turns out to be
    the cleaner path?  §7.0 sells "tiny per-expression modules"
    as a feature; option-1 preserves it, option-2 doesn't.
  - **Does wasmtime support the two-phase instantiation order**
    (expr first, then runtime importing expr's memory via
    `wasmtime_linker_define_memory`)?  The primitives exist but
    I didn't write the code to confirm they compose.  The
    feasibility spike in Slice 1 should exercise this before
    Slice 3 starts.
