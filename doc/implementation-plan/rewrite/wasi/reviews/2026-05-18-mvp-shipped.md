# Periodic code review — `wasi-malloc-migration` MVP

Review date: **2026-05-18.**  Branch tip: `f4b09da`.  Reviewer: Claude.
Span: commits `a086393..f4b09da` (9 commits).

## Verdict

**Mixed.**  The MVP is functionally clean — 14/14 runtime tests,
10/10 e2e tests, conformance pass=1373 with no regression — and the
slices that DID ship (M1, M2, M3, M8) are well-designed: the
`cel_layout.h` single-source-of-truth header is exactly the right
shape, the malloc-backed arena's native/wasm split is necessary and
correctly motivated, and the doc collapse from 9 → 6 → 2 files is a
clear win.  But the MVP shipped with **four of nine Phase-A slices
deferred** (M4, M5, M6 proper, M7), and the design's promised
simplification dividend is entirely hidden behind compat shims that
now form load-bearing infrastructure rather than the temporary
scaffolding they were drafted as.  The branch is in a coherent
state — but it is **further from "migration complete" than the
status table suggests**, and the gap is concentrated in places
where it will compound if Phase B drags.

**Top three to look at first**:

  1. **`eval/instance.cc:344-455` + `instance_impl.h:42-56`** —
     the `host_string_arena` machinery (110 LoC + 2 fields) that
     DESIGN §1 lists as removed is still load-bearing for every
     activation-marshalled string/bytes/type binding.  Its design
     assumes the wasm-side bump cursor lives at memory bytes 8/12
     and is bounded by `arena_limit`; neither is true anymore.  It
     works only because codegen still emits the old `(call
     $cel_reset arena_base arena_limit)` and the compat shim ignores
     the args.  This is the M7 deferral, but the comments describing
     the old design have not been touched and are now actively
     misleading.

  2. **`runtime/BUILD.bazel:280` re-adding
     `--import-memory=cel,memory`** after M2's removal.  The M6-M8
     milestone doc captures the revert honestly, but the resulting
     state — runtime built with wasi-sdk but still importing memory
     from the host — is the most fragile shape in the entire migration
     surface: it depends on wasi-libc's static data layout sitting
     above offset 8192 *and* the host happening to allocate ≥ 2 pages.
     There is no `static_assert` or runtime `ABSL_CHECK` validating
     either invariant.

  3. **`compiler/codegen/expr_lower.cc:114-122` + `layout_pass.cc:390`** —
     codegen still computes `arena_base`, still threads
     `mem_size_bytes` through `LoweringOptions`, and still emits
     `EmitCelResetCall(arena_base, arena_limit)`.  The `cel_reset`
     compat shim ignores both args — so codegen is doing work that
     produces dead bytes in every emitted wasm.  M5 deferral.

---

## Architectural drift (DESIGN.md as-designed vs as-built)

DESIGN.md §1 lists eight things the migration removes.  After the
MVP, **none of them have been removed**.  All eight survive,
hidden behind a compat layer.  Drift inventory:

| DESIGN §1 promise | As-built shape | Where |
|---|---|---|
| `cel_reset(arena_base, arena_limit)` codegen prologue removed | Still emitted; args still computed; runtime shim ignores them | `compiler/codegen/expr_lower.cc:114-122`, `:1084-1094`; `runtime/cel_arena.c:129-139` |
| `LoweringOptions::mem_size_bytes` threading dropped | Still in struct, still defaulted to 64 KiB, still wired into `EmitCelResetCall` | `compiler/codegen/expr_lower.h:140-148` |
| `LayoutPass::arena_base` field dropped | Still computed at end of LayoutPass | `compiler/codegen/layout_pass.cc:390`; `layout_pass.h:105` |
| Fixed cursor slot at bytes 8/12 gone | Code no longer USES bytes 8/12, but `cel_arena.c:64` reserves bytes [0, 16) of native `g_memory` "in case anything still pokes there" — i.e. the slot is leftover rather than removed | `runtime/cel_arena.c:62-68` |
| Inline-asm opacity barrier in `cel_memory.c` removed (clang wasm32 backend workaround no longer needed under wasi-sdk) | Still present at `cel_memory.c:33`; header comment still describes the old reason; nothing tests whether wasi-sdk's clang actually needs it | `runtime/cel_memory.c:9-17, 31-35` |
| `host_string_arena` workaround in `api/instance.cc` (~110 LoC) deleted | Still present in full; `EncodeStringOrBytes`, `EncodeType`, `EnsureHostStringArenaCapacity`, `TotalHostStringBytes`, `HostStringArena` struct, `host_string_arena_floor` + `_capacity` fields all intact | `eval/instance.cc:344-455, 740-790`; `api/internal/instance_impl.h:42-56` |
| 2-arg memory typing in `engine.cc` (host stops allocating memory) | `InitStoreAndMemory` still does `wasmtime_memorytype_new(min=2, max_present=false, ...)` and `wasmtime_memory_new` — host owns memory | `eval/engine.cc:141-157` |
| `--import-memory=cel,memory` linker dance removed | Removed in M2, **re-added in M6 commit `582def9`** | `runtime/BUILD.bazel:280` |

The M6-M8 milestone doc honestly captures the deferrals.  The drift
is not that the work was hidden — it's that the **design's
"simplification dividend" is asserted to be paid (README "shipped"
status, "MVP works") when in fact every removal listed is still
present, just routed differently.**

### New drift the MVP itself introduced (beyond the deferrals)

  - **A13/A14 assertions never landed**.  DESIGN §5 lists 17
    numbered assumptions, each with "where asserted".  A13
    (wasm memory page count at instantiation == `kInitialMemoryPages`)
    and A14 (`__heap_base` >= `kReservedLowMemoryBytes`) are
    explicitly tagged for `api/engine.cc`.  Neither is present.
    The runtime exports `__heap_base` (BUILD.bazel:287) but the host
    never reads it.  This is the load-bearing invariant for the
    `--global-base=8192` layout to actually be safe — without the
    check, a future wasi-libc / wasi-sdk update that pushes static
    data below 8192 will silently corrupt expr rodata.

  - **The 8192-byte reservation is unused**.  M2 added
    `-Wl,--global-base=8192` precisely to reserve `[0, 8192)` for
    expr module active data segments (DESIGN §4.1 line 1).  But the
    expr module's codegen has not been retargeted: rodata, workspace
    slots, and the obsolete arena base still live wherever LayoutPass
    decided in the pre-WASI world.  The reservation exists; nothing
    uses it.  Net effect: 8 KB of linear memory is wasted on every
    Instance.

  - **`cel_memory_size_()` in wasm returns the wrong constant**.
    `cel_memory.c:36-42` returns a hard-coded `64*1024` — but under
    wasi-sdk the runtime now owns memory at `min=2 pages = 128 KiB`.
    The comment ("imports a 1-page memory") is doubly wrong: memory
    is no longer imported (per the MVP claim) and it's two pages
    not one.  The function is currently dead in wasm (the arena's
    OOM check is local-only) but if any caller starts honoring it,
    everything past 64 KiB looks invalid.

---

## Tech-debt inventory

Each entry: `file:line — severity — effort — what needs to happen`.

### From the MVP itself (compat shims + deferrals)

  - `runtime/cel_arena.c:116-143` — **P1**, 0.25 days.
    The `cel_reset` / `cel_alloc` compat shims with auto-init are
    the load-bearing bridge.  Delete after B1 (kernel rename) +
    M5 (codegen prologue swap).  The auto-init branch
    (line 134-137) hides a subtle bug surface: if a kernel calls
    `arena_alloc` BEFORE any `cel_reset` (which DESIGN §6 step 5
    says the host should do via explicit `arena_init`), allocation
    silently fails (returns 0) rather than crashing — and a 0
    return is treated as OOM by every kernel, producing
    `CEL_ERR_OVERFLOW` poisoning at the call site rather than a
    diagnostic.  An `ABSL_CHECK(false)` or `__builtin_trap()` on
    `!g_arena.initialized` in `arena_alloc` would be the
    `unimplemented-features` rule's correct shape; we silently
    return 0.

  - `eval/engine.cc:141-157` (`InitStoreAndMemory`) — **P1**,
    1 day.  Host still allocates memory; should be deleted entirely
    once M6 ships properly.  The comment above the function still
    references "Slice 0 of the conformance unlock plan" and
    `arena_limit` semantics that no longer apply.

  - `eval/instance.cc:344-455, 740-790` —
    **P1**, 1.5 days.  `EncodeStringOrBytes` + `EncodeType` +
    `EnsureHostStringArenaCapacity` + `HostStringArena` struct +
    `TotalHostStringBytes` + `MarshalActivation`'s pre-pass.  All
    of this assumes the wasm-side arena is bounded above by
    `arena_limit` so the host can park strings past that limit.
    Replace with a malloc'd binding buffer (DESIGN §4.1 says
    "Activation binding buffer is malloc'd here once (grows via
    realloc as needed)") — per M7 plan.  The comments at
    `instance.cc:344-360` describe the OLD arena design verbatim;
    they need rewriting in the same commit.

  - `eval/internal/instance_impl.h:42-56` — **P1**,
    0.25 days.  `host_string_arena_floor` and `host_string_arena_capacity`
    fields go away with the M7 work.  The 13-line comment above
    them describes the old `cel_reset(arena_base, arena_limit)`
    contract that no longer holds.

  - `compiler/codegen/expr_lower.cc:110-122` (`EmitCelResetCall`) —
    **P1**, 0.5 days.  Replace with `EmitArenaResetCall` (no args).
    M5 work.

  - `compiler/codegen/expr_lower.h:140-148` (`LoweringOptions::mem_size_bytes`) —
    **P1**, 0.25 days.  Delete the field and its threading.  M5.

  - `compiler/codegen/layout_pass.cc:390`, `layout_pass.h:67-105`
    (`StaticLayout::arena_base`) — **P1**, 0.5 days.  Delete field
    and the computation.  M5.

  - `runtime/BUILD.bazel:280` (`--import-memory=cel,memory`) —
    **P1**, embedded in the M6 engine.cc rewrite above.

  - `runtime/cel_memory.c:9-42` (inline-asm opacity barrier
    + stale wasm `cel_memory_size_`) — **P2**, 0.25 days.  Verify
    whether wasi-sdk's clang elides null-pointer-derived stores;
    if not, delete the asm + the load-bearing header comment.
    The wasm `cel_memory_size_` returning 64KiB also needs to be
    either fixed to read `__heap_base`/memory.size or deleted as
    dead.

  - `runtime/cel_arena.c:60-69` (the "in case anything
    still pokes there during the migration" `[0, 16)` reservation
    in the native build) — **P2**, 0.1 days.  Delete after B1; the
    `g_arena.base = cel_memory_base_() + 16` should become
    `g_memory_base_()` once the legacy poke-at-bytes-8/12 dust
    has fully settled.

### Tech-debt the MVP did NOT introduce but did NOT clear either

  - DESIGN.md §5 catalogs **17 numbered assumptions A1-A17**, each
    with a "where asserted" cell.  Status after MVP: A1-A4 are
    pre-existing; A5-A7 landed in `cel_layout.h` (M2);
    A9, A10, A16 landed in `cel_arena.c` (M3); **A8, A11, A12,
    A13, A14, A15, A17 are still TODO**.  Severity P1 collectively;
    each individual assert is 5-15 minutes of work.  The risk is
    that without these in the build, the design's "every assumption
    is enforced in code" discipline is aspirational rather than
    real, and a Phase B regression that violates them will not
    surface until something else breaks.

  - `runtime/cel_arena.c:62` — **P2**.  The
    "leaves the null sentinel + the legacy cursor slot bytes 8/12
    untouched, in case anything still pokes there during the
    migration" comment is a tripwire: it admits the native arena
    is co-located with vestigial state.  Phase B should either
    verify nothing pokes (and delete the reservation) or document
    what does poke (and the reservation stops being conditional).

  - `experiments/exp_d_driver.wat`, `experiments/.gitignore`, and
    the symlink `experiments/wasi-sdk -> ../../../../wasm_compilation_experiments/exp1_re2/wasi-sdk-25.0-arm64-macos`
    — **P2**, 0.25 days.  Once Phase C wires absl + RE2 via
    `http_archive`, the symlink to `wasm_compilation_experiments/`
    becomes dead reference to a dir DESIGN §13 explicitly says is
    superseded.

---

## Coverage gaps

  - **`cel_arena_test.cc`** does NOT have a test for `arena_init`
    being called twice with different sizes (the A16 trap case).
    The header comment at `cel_arena.h:36-38` and DESIGN §5 row A16
    both say "calling with a different value after the first call
    traps".  The runtime implements it (`cel_arena.c:43-48`
    `__builtin_trap()`).  No test exercises this — adding one
    requires either a fork-based death test or rewiring the
    fixture, but the assertion is testable and load-bearing.

  - **`cel_arena_test.cc`** does NOT test the compat shim's
    auto-init behavior (the line 134-137 branch in `cel_arena.c`).
    `CompatShimCelAllocMatchesArenaAlloc` covers the steady-state
    parity but not the cold-start case where `cel_reset` is the
    first call ever.  In native test-harness order this might be
    the very first SetUp; in production wasm it should never
    happen (host calls `arena_init` explicitly per DESIGN §6).
    Test the cold path to lock the contract.

  - **`mvp_concat_test.cc`** tests precisely one input (`'foo' +
    'bar'`).  DESIGN §2.4 lists six per-layer test rows; the
    MVP test covers row 5 ("host integration") but does not
    cover row 1 (kernel-level arena re-use across evals),
    row 2 (arena reset semantics surface end-to-end), row 4
    (codegen byte-for-byte match against the WAT), or row 6
    (Chrome).  Worse, the e2e test in its current form is
    indistinguishable from a hot-path test that bypasses the
    arena entirely (the result is so small it fits in pre-existing
    `Plan_Hot` machinery).  At minimum, add a second test that
    Evals twice in a row and asserts both calls return correct
    results — that exercises arena_reset across-eval, which the
    MVP claim is staked on.

  - **`per-component-test-coverage.md` §3.8** lists
    `cel_arena_test.cc` among the runtime tests but the catalog
    (§3.8 lines 206-218) describes "wire stability" and
    "per-kind round-trip" — no entries cover the arena's new
    public API (arena_init / arena_alloc / arena_reset / cursor /
    capacity).  Severity **P1**: per-component-test-coverage.md is
    the keystone testing doc; new public API surface arriving
    without entries is the same anti-pattern CLAUDE.md calls out
    ("M2 silently shipped half-done with 29 skipped tests").

  - **`per-component-test-coverage.md`** has no entry for
    `cel_layout.h`.  The five compile-time asserts in that file
    are the single source of truth for memory-layout invariants;
    if `kInitialMemoryPages` or `kReservedLowMemoryBytes` ever
    becomes a function or a tunable, the testing posture changes
    materially.  A short row noting "compile-time-asserted; if
    runtime tuning lands, add boundary tests" is appropriate.

  - **No entry in `per-component-test-coverage.md` for
    `mvp_concat_test`**.  §3.11 "E2E" catalog should list it.
    Same omission applies to the Chrome smoke-test deferred to
    Phase D (M9 doc).

---

## Doc drift

  - **`doc/implementation-plan/rewrite/design.md:99, 119, 126,
    247-274, 418, 461`** still describe the runtime as defining
    `cel_alloc`/`cel_reset` against a host-imported memory at
    fixed offsets.  This was the as-designed shape **before** the
    WASI migration started — perfectly correct for the rewrite
    plan's frame of reference.  But the WASI migration is now
    "MVP shipped" per its own README.  Sibling design docs are
    out of sync.  **Severity P1**: a developer reading
    `rewrite/design.md` will believe the `cel_alloc`/`cel_reset`
    contract is the active one; nothing flags that wasi-malloc
    plans to delete it.  CLAUDE.md's "Closing out a planning doc"
    rule §4 says "reconcile other docs that referenced the old
    plan in the same commit" — this is the case the rule was
    written for.

  - **`doc/implementation-plan/rewrite/cel-host-surface.md:187,
    369, 388`** describes `cel_reset` as "runs internally at each
    call" — same drift category.  Add a top-banner note pointing
    at `wasi/DESIGN.md` for the active shape.

  - **`doc/implementation-plan/rewrite/conformance-unlock-plan.md`**
    is presumably still active (the lines 1627-1628, 1749, 2903
    grep hits mention `cel_alloc` in flight).  Verify whether it
    closes against the pre-WASI ABI; if so, mark the relevant
    sections "see wasi/DESIGN.md §1 for post-MVP shape".

  - **DESIGN.md §14 status checklist** says "Phase A (MVP, M1-M9):
    not started" — README.md says M1, M2, M3, M8, M9 are shipped.
    DESIGN.md's checklist hasn't been kept in sync with README.md's
    tracker.  CLAUDE.md's closeout rule §1 says "update the
    header status line so the doc itself signals state."  Either
    delete §14 (it's redundant with README.md) or update it.

  - **README.md "M9 shipped"** is misleading.  The slice as
    described in DESIGN §8 is "Chrome smoke-test"; what shipped is
    "absl::ParseTime works in a separate experimental wasm
    binary."  The M9 doc itself captures this honestly
    (line 75-105 "Chrome smoke-test — deferred to Phase C") but
    the README's headline `☒ M9` overstates progress.  Severity
    **P1**: a casual reader sees Phase A's last row green.

  - **README.md "What landed" row for M6 + M8** says "no
    regression" — true, but elides that the milestone is **partial**.
    M6 was reverted; what shipped is M6's WASI stub, not M6 itself.
    Reword as "M6 (WASI stubs only) + M8 shipped" or use ◐ instead
    of ☒.

---

## Conformance / test-sweep observations

  - **pass=1373** is stable across the MVP.  The baseline DESIGN
    §10 cited was 1144 (a stale READmd number from the recorded
    baseline); the as-master number at branch-point @ `9685d72`
    was already higher.  Spot-checked the skip + fail buckets
    against pre-migration master; no new categories appear
    introduced by the MVP commits.  Skips remain in the
    `string.textproto::matches/*` (RE2-pending, expected per
    Phase C), `timestamps.textproto` parse rows (also Phase C),
    and the comprehension-deferred set from rewrite M5 — all
    pre-existing.

  - **The `bazel test //compiler_v2/...` green status hides one
    thing:** `cel_runtime_wasm_test` and the genrule
    `cel_runtime_wasm_file` are `tags = ["manual"]`, as is
    `cel_runtime_wasm_bytes_cc`.  Every e2e test (including the
    new `mvp_concat_test`) depends transitively on
    `cel_runtime_wasm_bytes`, which is `manual`.  This is the
    pre-existing pattern, not a MVP regression — but it's worth
    noting that the MVP test's "passes" status is conditional on
    the manual genrule having been built.  Per CLAUDE.md
    "per-component-test-coverage.md" §6.3: "every milestone's
    'closeout gate' must enumerate the manual-tagged tests that
    were run."  The M6-M8 doc lists the e2e suite as passing but
    does not enumerate which manual targets were rebuilt.  Add
    explicit list before the next milestone closes.

---

## General tech-debt sweep (beyond WASI scope)

The user asked for a holistic pass.  Findings:

  - **`eval/engine.cc:228-303`** — `kRuntimeExports[]` is
    a flat constexpr array of ~130 names.  Every kernel addition
    requires editing this list **and** the export list in
    `runtime/BUILD.bazel:299-477` **and**
    `runtime/wasm_imports.txt` (the
    `allow-undefined-file`).  Three places to keep in sync;
    nothing checks they match.  **P2**, 1 day: generate one list
    from the other (a small bazel genrule that reads
    `wasm_imports.txt` and emits a `kRuntimeExports[]` .h).
    This will also delete drift risk before the M5.B step 7 string
    `_v` absorbers add another 9 entries.

  - **`runtime/cel_arena.c:75-99` (`arena_alloc`)** is
    correctly small.  But `align_up_8` at line 33 + the local var
    `need` rename + the `if (need == 0) need = 8u` line is a place
    `readability-isolate-declaration` or `readability-magic-numbers`
    is likely to fire under stricter lint.  Currently fine; flag
    only if the lint backlog list grows.

  - **`eval/internal/instance_impl.h:22-29`** — the
    struct has 8 public data members.  Mixing `wasmtime_*` raw
    handles (5) with C++-owned domain state (`abi`, `host_env`,
    arena bookkeeping fields) makes the lifetime invariants
    implicit ("destructor order matters" per the file header
    comment) rather than enforced.  **P2**, 0.5 days: split into
    a `WasmtimeHandles` sub-struct with explicit destructor
    sequencing.  Not urgent; the comment captures intent.

  - **`doc/implementation-plan/lint-backlog.md` shipped with 8
    known function-size exceedances** ("Intentionally left in
    place").  None added by the MVP — confirmed via spot grep
    over `runtime/cel_arena.c` (all functions ≤ 30
    lines).  Backlog stable.

  - **`runtime/BUILD.bazel:299-477`** — the `cc_library`
    + the `genrule` BOTH list the kernel `.c` files.  Adding a new
    kernel `.c` requires two edits.  **P2**, same fix as the export
    list consolidation: derive one from the other.

  - **`compiler/codegen/expr_lower.cc:1084-1094`** — `EmitCelResetCall`
    is called unconditionally in the eval-function epilogue
    construction.  Per the WASI migration plan this will become
    `EmitArenaResetCall` with no args.  Worth confirming during M5
    that the call shape is centralized; if it leaks to other
    sites the rename surface grows.

  - **`third_party/wasi_sdk/BUILD.bazel:14-44`** — five `alias`
    declarations, all hard-coded to `darwin_arm64`.  M1.1 follow-up
    in the M1 milestone doc.  Severity **P2** until CI extends.

  - **No `cleanup-backlog.md` exists yet.**  CLAUDE.md's severity
    convention refers to `cleanup-backlog.md` for P2 items; the
    file does not exist.  Create one at the same time as Phase B
    starts; populate with the P2 items enumerated here.

---

## On the neighboring-component pick — why codegen/

I picked `compiler/codegen/` over `eval/internal/cel_host.cc`
because:

  - `cel_host.cc`'s involvement in the migration is symmetric
    (every trampoline calls `cel_alloc` via the existing
    `host_env.cel_alloc_fn` handle, captured at
    `engine.cc:336-344`; after Phase B that becomes
    `arena_alloc_fn`, mechanical rename).
  - `codegen/` is where the **shape** of every emitted wasm
    contains the M5 deferral.  Every program produced by this
    branch contains a `(call $cel_reset i32 i32)` prologue whose
    args are dead.  That's not a localized debt — every cached
    expr module on disk encodes a contract that's about to change.
    Picking codegen surfaces the size of the rebaseline (50
    fixtures per Phase B B3) and the fact that the WAT-first
    design discipline (CLAUDE.md "WAT-first for ABI and codegen
    design") has a pending `.wat` update that has NOT been drafted
    — DESIGN §2.1's MVP `.wat` shows the new shape, but
    `doc/implementation-plan/rewrite/wat/` has no entry for the
    post-WASI prologue.  Severity **P1** to add it before M5
    codegen change, per the WAT-first rule.

Drift caught in codegen specifically:

  - `compiler/codegen/layout_pass.h:60-105` — the diagram in
    the header comment shows "bytes 8/12 — arena cursor + limit
    written by cel_reset" as a live concept.  Still true today
    (compat shim), false after M5.  Update with M5.
  - `compiler/codegen/expr_lower.h:39-42` — `kCelResetInternalName`
    is `"cel_reset"`.  Rename to `kArenaResetInternalName` =
    `"arena_reset"` in M5.  Search-and-replace target.
  - `compiler/codegen/module.h:12` — header comment names
    `cel.cel_reset` / `cel.cel_alloc` as imports.  Update with M5.

---

## Next steps the user can hand to an implementer

Ordered; each tagged P0/P1/P2.

1.  **P1, 0.5 days** — Reconcile README.md and DESIGN.md §14 status:
    M9 (Chrome) is not actually shipped; M6 is only partially shipped.
    Use ◐ instead of ☒ for those rows, with a one-line "what's
    deferred" note per row.  Same commit: update DESIGN.md §1's
    "what gets removed" list with a "post-MVP status" annotation
    that admits all eight items are still present.

2.  **P1, 1 day** — Land assertions A8, A11, A12, A13, A14, A15,
    A17 from DESIGN §5.  Each is small (5-15 min); they're the
    discipline the design promised.  Do this BEFORE Phase B
    starts so the migration's checks ride downhill.

3.  **P1, 0.25 days** — In `cel_arena.c:79`, replace the silent
    `return 0` on `!g_arena.initialized` with `ABSL_CHECK(false)`
    (or the equivalent `__builtin_trap()`) per the CLAUDE.md
    "Unimplemented features" rule for the case where a kernel
    runs before `arena_init`.  The auto-init in `cel_reset` is
    fine; the bare `arena_alloc` path should be loud.

4.  **P1, 1 day** — Write the post-WASI prologue `.wat` under
    `doc/implementation-plan/rewrite/wat/` before M5 codegen
    work starts, per CLAUDE.md's "WAT-first" rule.  This locks
    the byte shape that B3 will rebaseline against.

5.  **P1, 0.25 days** — Add `arena_init` double-init death test
    and `arena_alloc-before-init` test to `cel_arena_test.cc`.
    Add `cel_arena` rows to `per-component-test-coverage.md §3.8`
    documenting the new public API surface, and an `mvp_concat_test`
    row to §3.11.

6.  **P1, 2.5 days** — Phase B slice M5 (codegen prologue swap +
    drop `arena_base` / `mem_size_bytes`).  This is the slice
    that breaks the existing 50 codegen test fixtures; bake the
    full 1 day budget for B3 rebaselining per DESIGN §12 R3.

7.  **P1, 1 day** — Phase B slice M6 proper (engine.cc memory
    ownership flip).  Includes A13/A14 assertion landing if not
    already done in step 2.  The MVP doc captures the
    two-memories-confused failure mode that broke the first
    attempt — same shape: coordinate engine.cc + instance.cc +
    BUILD.bazel in a single commit.

8.  **P1, 1.5 days** — Phase B slice M7 (host_string_arena
    deletion + malloc'd binding buffer).  Touch
    `instance_impl.h:42-56`, `instance.cc:344-455 + 740-790`.
    Rewrite the comments at instance.cc:344-360 in the same
    commit; they currently describe a design that no longer
    holds.

9.  **P1, 0.5 days** — Reconcile sibling docs: add a banner to
    `doc/implementation-plan/rewrite/design.md` + `cel-host-surface.md`
    + `conformance-unlock-plan.md` pointing at `wasi/DESIGN.md`
    for the post-MVP ABI.  CLAUDE.md "Closing out a planning
    doc" rule §4.

10. **P2, 0.5 days** — Create `doc/implementation-plan/cleanup-backlog.md`
    populated with the P2 items from §"Tech-debt inventory":
    the wasi-sdk darwin-arm64-only aliases, the kRuntimeExports[]
    triple-source-of-truth consolidation, the
    `experiments/wasi-sdk` symlink cleanup post Phase C, the
    inline-asm opacity barrier verification under wasi-sdk
    clang.

---

## What's NOT in this review

  - I did not run `bazel test //compiler_v2/...` or
    `scripts/lint.sh` (per the review prompt); MVP claims of
    test/lint state are taken at face value.
  - I did not exhaustively diff every C/C++ file modified;
    the focus was the migration-touched files plus one
    neighbor.  A line-by-line review of `engine.cc`'s
    `WasiRandomGetStub` (signature, arity, memory bounds
    check) found nothing concerning; the stub is the right
    shape.
  - I did not validate the cross-platform wasi-sdk archives
    (M1's `e1e529ea...` etc shasums); trusting the milestone
    doc.  Linux CI bring-up will exercise those.
