# Cleanup backlog

P2 items surfaced by periodic code reviews (see CLAUDE.md
"Periodic code review") and ad-hoc tech-debt observations.  P0
and P1 items live in the active milestone's "pre-close cleanup"
section, not here.

Entries are dated by the review or commit that surfaced them so
the reader can trace context.  When a commit addresses an entry,
the commit message cites `cleanup-backlog #N` and the entry is
struck through or removed.

## Format

```
- [ ] **#<N>** — <one-line description>.
      Surfaced: <YYYY-MM-DD review name or commit hash>.
      Files: <main paths>.
      Why P2: <reason this isn't P0/P1>.
```

## Open

- [ ] **#1** — `third_party/wasi_sdk/BUILD.bazel` flat-aliases
      `//third_party/wasi_sdk:clang` → `@wasi_sdk_darwin_arm64//:clang`;
      Linux + macOS-x86_64 will fail at the bazel-analyzing stage.
      Surfaced: 2026-05-18 MVP review (P2 in §"Tech-debt
      inventory").
      Files: `third_party/wasi_sdk/BUILD.bazel`, `MODULE.bazel`.
      Why P2: works for the dev box today; CI matrix expansion is
      a Phase B / M1.1 follow-up, not a merge blocker.

- [ ] **#2** — `kRuntimeExports[]` in `compiler_v2/api/engine.cc`
      is the third source of truth for the runtime's exported
      kernel names — the other two are
      `compiler_v2/runtime/BUILD.bazel`'s `-Wl,--export=…` flags
      and `wasm_imports.txt`.  Nothing checks they match;
      regressing one of the three lights up at instantiate-time
      as `unknown import: cel::<name>`.
      Surfaced: 2026-05-18 MVP review (P2).
      Files: `compiler_v2/api/engine.cc`, `compiler_v2/runtime/BUILD.bazel`.
      Why P2: every new kernel is caught at next instantiate; a
      consolidation pass (single header generating both lists)
      is the right fix but doesn't unblock anything in flight.

- [ ] **#3** — `doc/implementation-plan/rewrite/wasi/experiments/exp1_re2/`
      contains a `wasi-sdk/` symlink + cached absl install used by
      the absl::ParseTime experiment.  Both are post-Phase C
      garbage once `abseil-cpp` lands as a proper `http_archive`.
      Surfaced: 2026-05-18 MVP review (P2).
      Files: `wasm_compilation_experiments/exp1_re2/`.
      Why P2: experimental directory; doesn't affect production
      build paths.

- [ ] **#4** — `compiler_v2/runtime/cel_memory.c` has an inline-asm
      opacity barrier (`asm volatile("" :: "r"(...))`) that
      prevents clang from miscompiling `*(base + off)` stores as
      no-ops when `base` is a zero pointer literal.  Verify the
      barrier still triggers under wasi-sdk clang-19 by inspecting
      the emitted `.wasm` for the expected store instructions
      (was historically clang-15-specific).
      Surfaced: 2026-05-18 MVP review (P2).
      Files: `compiler_v2/runtime/cel_memory.c`.
      Why P2: tests + e2e currently pass, so the barrier is
      working at runtime; the concern is that a future clang
      could regress.  Add a disassembly assertion in a follow-up.

- [ ] **#5** — `compiler_v2/runtime/cel_memory.c:1-15` comment block
      describes a "1-page memory" + the `cel_alloc` ABI that no
      longer exists.  Comment is stale post-MVP; the wasi-sdk
      runtime imports a 2-page memory and `cel_alloc` was deleted
      in B1.
      Surfaced: 2026-05-18 MVP review (P2).
      Files: `compiler_v2/runtime/cel_memory.c`.
      Why P2: comment-only; no runtime impact.

- [ ] **#6** — `cel_memory_size_()` on wasm returns a hard-coded
      64 KiB; with the wasi-sdk build the linear memory is 2 ×
      64 KiB = 128 KiB, and the accessor should reflect that.
      Either compute from `__heap_base` + arena capacity or update
      the constant.  Today's incorrect value is harmless because
      no kernel actually consults `cel_memory_size_()` on the
      wasm side (only the native build does).
      Surfaced: 2026-05-18 MVP review.
      Files: `compiler_v2/runtime/cel_memory.c`.
      Why P2: not yet observable; lands when the host_string_arena
      cleanup goes in (M7).

## Closed

- [x] **#7** — `CEL_LOG("enter")` in every public runtime helper does
      a wasm→host `fprintf(stderr)` trampoline on every invocation;
      under wasi-sdk's call-prologue convention the per-call cost
      roughly doubled vs the pre-WASI freestanding build, dominating
      per-Eval cost on aggregate-heavy expressions (`Eval_ListAt_Arena`
      took 3.6 µs / Eval where the WASI-free body would take 0.6 µs).
      Fix: gate `-DCEL_LOG_DISABLED` on `-c opt` via `config_setting`
      in `compiler_v2/runtime/BUILD.bazel`; CEL_LOG stays live for
      `dbg` / `fastbuild` so the dead-code audit per `cel_log.h`
      still works.  Measured improvement: 1.4×–5.7× faster on the
      `BM_Eval_*` rows of `//compiler_v2/bench:pipeline_bench`.  See
      `bench/README.md` updated baseline and
      `doc/implementation-plan/rewrite/wasi/POST_MIGRATION_BENCH.md`
      "Mitigation paths" item 1.
      Surfaced: 2026-05-18 post-Phase-C perf-run investigation.
      Closed: 2026-05-19.
      Files: `compiler_v2/runtime/BUILD.bazel`,
      `compiler_v2/bench/README.md`,
      `doc/implementation-plan/rewrite/wasi/POST_MIGRATION_BENCH.md`.
