# WASI / `malloc` migration

Branch: `wasi-malloc-migration` (forked from master @ `9685d72`).

This directory plans + tracks the migration of `compiler_v2`
from `--target=wasm32 -nostdlib -ffreestanding` with a custom
bump arena at fixed memory bytes 8/12, to `wasi-sdk`'s
`--target=wasm32-wasi` with a hand-rolled bump arena over
`malloc()`.

## Goal

**Simplify codegen and host** by removing:
  - The `cel_reset` codegen prologue + `arena_base` /
    `mem_size_bytes` threading.
  - The fixed cursor-slot at memory bytes 8/12.
  - The `host_string_arena` workaround (`api/instance.cc` ~110 LoC).
  - The inline-asm opacity barrier in `cel_memory.c`.
  - The 2-arg `cel_reset(base, limit)` ABI.
  - The `--import-memory=cel,memory` linker dance.

Side benefit: any C/C++ library (RE2, parts of absl) can be
vendored into the runtime without dual-allocator integration
pain.  Browser deployment via plain `WebAssembly.instantiate`
stays viable (zero WASI imports for pure-allocation code,
verified).

## What to read, in order

1. **[AUTHORITATIVE_PLAN.md](AUTHORITATIVE_PLAN.md)** — the build
   plan.  Resolved architectural decisions, per-Plan and
   per-Eval lifecycle, 12-slice work breakdown, acceptance
   criteria.  Single source of truth.
2. **[BASELINE_BENCH.md](BASELINE_BENCH.md)** — the numbers to
   beat.  Pre-migration `cel_pipeline_bench` measurements on
   the same branch + commit.  Static metrics (LoC, binary
   size, imports) too.
3. **[MEMORY_OPTIONS.md](MEMORY_OPTIONS.md)** — the experimental
   findings that justify the allocator + memory-layout choices
   the plan makes.  Every claim grounded in a running
   `experiments/*.wasm` binary.
4. **[ANALYSIS.md](ANALYSIS.md)** — reference material.
   Per-function inventory of what the migration touches
   (1,082 lines).  Source for the plan's diff sketches.
5. **[AGENT_ASSESSMENT.md](AGENT_ASSESSMENT.md)** — independent
   critical review of compiler_v2's current design.  Motivates
   the migration (the "three coupled decisions" finding).

## Experiments

Under [`experiments/`](experiments/):

  - `exp_a_rodata.c` — probes default + custom rodata layout
    under wasi-sdk.  Used to verify `--global-base=N` works.
  - `exp_b_mspace.c` — link test for dlmalloc's `mspace_*`
    API.  Fails — confirms stock wasi-libc doesn't expose it.
  - `exp_c_malloc.c` — pure-malloc wasm.  Verifies zero WASI
    imports (browser-shimmable without a shim).
  - `exp_d_arena_in_malloc.c` + `exp_d_driver.wat` — the
    recommended design.  47-LoC bump arena over a single
    `malloc()`, with hand-coded WAT driver proving reset
    semantics work cross-module.

To re-run the experiments, the symlink at
`experiments/wasi-sdk` points to wasi-sdk-25, installed at
`wasm_compilation_experiments/exp1_re2/wasi-sdk-25.0-arm64-macos/`.

## Status

  - [x] Branch cut (`wasi-malloc-migration`).
  - [x] Baseline benchmark captured.
  - [x] Memory-layout experimental questions answered.
  - [x] Architectural decisions resolved (5 of 5).
  - [x] Authoritative plan written.
  - [ ] **S1**: wasi-sdk in MODULE.bazel.
  - [ ] **S2**: runtime/BUILD.bazel switch to wasi-sdk.
  - [ ] **S3**: arena_init/alloc/reset implementation.
  - [ ] **S4**: kernel `cel_alloc` → `arena_alloc` (107 sites).
  - [ ] **S5**: kernel test fixtures port (21 files).
  - [ ] **S6**: codegen prologue swap.
  - [ ] **S7**: LayoutPass `arena_base` removal.
  - [ ] **S8**: Engine memory-ownership flip.
  - [ ] **S9**: Instance host_string_arena deletion.
  - [ ] **S10**: conformance debug → 1,144 PASS.
  - [ ] **S11**: post-migration bench + POST_MIGRATION_BENCH.md.
  - [ ] **S12**: Chrome smoke-test (string concat sample).

## Sentinel

[`CLAUDE_Do_NOT_DELETE_OR_REVERT_FILES_IN_THIS_DIR`](CLAUDE_Do_NOT_DELETE_OR_REVERT_FILES_IN_THIS_DIR)
is a sentinel for other agents — these files are owned by
the migration; don't include them in unrelated commits.
