# Work plan — WASI / `malloc` migration

Status: plan — drafted 2026-05-17.  Companion to
`HANDOFF.md` and `ANALYSIS.md`.  This is the slice-by-slice
breakdown the new branch follows.

**Goal: simplify codegen and host.**  The benchmark
(Phase 7) is the validation gate, not the goal.

---

## Phase 0 — Branch + tree setup (1 session)

### Deliverables
  - New branch `wasi-malloc-migration` off the current
    master commit.
  - `compiler_v3/` directory: `cp -r compiler_v2 compiler_v3`.
  - Top-level BUILD rules so `bazel test //compiler_v2/...`
    and `bazel test //compiler_v3/...` both work
    independently against the same dependency tree.
  - `conformance/` is shared (input fixtures don't fork);
    only the runner code in `compiler_v[23]/conformance/`
    diverges.

### Verification
  - `bazel build //compiler_v2/...` green (regression check
    on master state).
  - `bazel build //compiler_v3/...` green (the copy
    compiles without any changes yet — confirms the rename
    + symbol-namespace work is correct).
  - Both target trees produce binary-identical
    `cel_runtime.wasm` (sha256 match) — confirms the copy
    is a faithful starting point.

### Out of scope
  - No code changes to compiler_v3 yet.
  - No WASI infrastructure yet.

---

## Phase 1 — wasi-sdk build infrastructure (1 session)

### Deliverables
  - `MODULE.bazel` gets four `http_archive` blocks:
    - `wasi_sdk_darwin_arm64` (the dev box)
    - `wasi_sdk_darwin_x86_64`
    - `wasi_sdk_linux_x86_64`
    - `wasi_sdk_linux_arm64`
  - `third_party/wasi_sdk/BUILD.external.bazel` exporting
    `clang`, `clang++`, `ar`, `ranlib` as filegroups.
  - Platform-aware alias `@wasi_sdk` selecting the right
    archive based on `@platforms//os` × `@platforms//cpu`.
  - **No** runtime build change yet — the toolchain is
    available but unused.

### Verification
  - `bazel build @wasi_sdk//:clang` produces the right
    binary path on macOS arm64.
  - The wasi-sdk tarball download is reproducible (sha256
    pinned per the http_archive idiom).

### Out of scope
  - Native build still uses host clang (no change).
  - compiler_v2's runtime build still uses brew clang.

---

## Phase 2 — Standalone prototype runtime (2 sessions)

### Deliverables
  - New file `compiler_v3/runtime/cel_runtime_wasi.c` —
    a parallel runtime that uses `malloc` instead of
    `cel_alloc`.  Initially supports only:
    - cel_int_add_at_vv (one kernel for proof)
    - cel_bool_eq_at_vv
    - cel_string_concat_at_vv (because span allocation is
      the interesting case)
  - mspace lifecycle helpers:
    - `cel_eval_begin()` — opens an mspace; stores handle
      in a wasm global.
    - `cel_eval_end()` — destroys the mspace.
    - Verify in this phase that wasi-libc's
      `mspace_create_with_base` / `mspace_destroy` are
      actually available.  If not, fall back to plan B
      (global malloc + explicit free).
  - Unit tests: same shape as the existing kernel tests,
    just verifying the malloc-based runtime behaves
    identically.

### Verification
  - The three prototype kernels pass equivalent tests to
    their compiler_v2 counterparts.
  - mspace lifecycle works: open, allocate 100 times,
    destroy.  Memory doesn't leak across cycles (verified
    via `malloc_stats` or equivalent).
  - Binary size of the new runtime is in the expected
    range (~120-150 KB stripped).

### Open questions resolved in this phase
  - Does wasi-libc expose `mspace_*`?  Yes/no → branch.
  - What's the actual dlmalloc lazy-init cost per Instance?
  - Does `wasm32-wasi` (non-threads target) suffice for our
    kernels?

### Out of scope
  - The full kernel port (Phase 5).
  - Codegen changes (Phase 3).

---

## Phase 3 — Codegen prologue rewrite (3 sessions) — THE BIG ONE

This is where simplification gets cashed in.  Most of the
real engineering happens here.

### Deliverables

**3a — Eval prologue (1 session).**  Replace
`EmitCelResetCall(mod, layout.arena_base, opts.mem_size_bytes)`
in `expr_lower.cc` with:

```
(local.set $msp (call $cel_eval_begin))
(local.set $ws_base (call $malloc (i32.const ws_bytes)))
;; ... eval body ...
(call $cel_eval_end (local.get $msp))
```

Drop `arena_base` from `StaticLayout`.  Drop
`mem_size_bytes` from `LoweringOptions`.

**3b — Variable / constant addressing (1 session).**
Today, kIdent emits `(i32.const absolute_offset)` for
variable slots.  Tomorrow:
`(i32.add (local.get $ws_base) (i32.const var_offset))`.
Same for kConst rodata lookups.

This is the work that ripples through every
`Lower*` function in `expr_lower.cc`.  Each emit point
needs to think "is this addressing a workspace slot or a
runtime-shared region?"  Workspace → relative; runtime
exports / rodata constants → absolute.

**3c — Codegen test fixture update (1 session).**  The
~50 fixtures in `codegen/expr_lower_test.cc`,
`codegen/layout_pass_test.cc`, and `codegen/module_test.cc`
that assert specific emitted instructions need rewriting.

Most are mechanical (`EXPECT_STREQ(target, "cel_reset")` →
`EXPECT_STREQ(target, "malloc")`).  Some assert specific
i32.const offsets that are now relative — those need to
check the addressing shape instead of exact offsets.

### Verification
  - All `codegen/*_test.cc` green.
  - One handwritten e2e: compile `"42"`, plan, eval —
    returns 42.  Proves the prologue works.
  - One handwritten e2e with a bound variable:
    `"x + 1"` with `x=10` — returns 11.  Proves relative
    addressing works for workspace slots.

### Out of scope
  - Production e2e suite (Phase 5/6).
  - Comprehension codegen (depends on M5 follow-on).

### Risk: this is the slice where merge conflict with M5
comprehensions follow-on will hurt the most.  Do not start
this until M5 lands or until we accept the rebase.

---

## Phase 4 — Host integration (2 sessions)

### Deliverables

**4a — Engine memory ownership flip (1 session).**
`compiler_v3/api/engine.cc`:
  - `InitStoreAndMemory` no longer allocates memory.
    Renamed to `InitStore`.  Memory is now owned by
    `runtime_instance` after instantiation.
  - Instantiation order changes: runtime_instance FIRST
    (creates memory[0]); pull its exported `memory` via
    `wasmtime_instance_export_get`; define on linker
    under `(cel, memory)`; THEN instantiate expr_instance.
  - `kRuntimeExports[]` array: remove `"cel_reset"` and
    `"cel_alloc"`, add `"malloc"` and `"free"`.
  - `host_env.cel_alloc_fn` → `host_env.malloc_fn` rename
    + signature change (i32 → i32, same shape).

**4b — Activation marshalling rewrite (1 session).**
`compiler_v3/api/instance.cc`:
  - DELETE `EnsureHostStringArenaCapacity` and helpers
    (~110 LoC).
  - DELETE `HostStringArena` struct + field.
  - DELETE `host_string_arena_floor` /
    `host_string_arena_capacity` from `InstanceImpl`.
  - `EncodeStringOrBytes` becomes a thin wrapper that
    calls `host_malloc(ctx, malloc_fn, str.size())` via
    wasm reentry and writes the bytes there.  ~30 LoC.

### Verification
  - Plan + eval of `"x"` with `x="hello"` returns
    `"hello"`.  Proves activation marshalling works end-
    to-end.
  - No memory leak across 1000 evals of the same
    expression (each eval's activation strings are
    explicitly freed at end-of-eval or via mspace
    destruction).

### Out of scope
  - The 8 Layer-3 trampolines that take an `Allocator&` —
    that's Phase 5.

---

## Phase 5 — Runtime kernel + test port (2 sessions)

### Deliverables

**5a — Kernel call site port (1 session).**  All 107
`cel_alloc` invocations across:
  - `cel_runtime.c`, `cel_3vl.c`, `cel_make.c`,
    `cel_string_ops.c`, `cel_convert.c`, `cel_type.c`.

Each becomes `malloc` (or `mspace_malloc(g_msp, ...)`
depending on §4.2 in HANDOFF.md decision).

**5b — Test fixture port (1 session).**  21 test files:
  - `cel_runtime_wasm_test.cc`, `cel_aggregate_arena_test.cc`,
    `cel_arith_test.cc`, `cel_3vl_test.cc`,
    `cel_compare_test.cc`, `cel_type_test.cc`,
    `cel_convert_test.cc`, `cel_string_ops_test.cc`,
    `cel_make_test.cc`, `cel_time_test.cc`,
    `cel_list_test.cc`, `cel_map_test.cc`, plus 9 others.

`SetUp()` becomes either no-op or `mspace_create`; per-test
`cel_alloc` calls become `malloc`/`mspace_malloc`.

Also: **DELETE `cel_arena_test.cc` entirely** (the module
it tests is gone).

### Verification
  - `bazel test //compiler_v3/runtime/...` all green.

### Out of scope
  - Codegen tests (Phase 3).
  - E2E tests (Phase 6).

---

## Phase 6 — Conformance + e2e debug (2 sessions)

### Deliverables
  - `bazel test //compiler_v3/e2e:*` all green (12 test
    files, ~10,981 LoC).
  - `bazel run //compiler_v3/conformance:run_conformance`
    reports 1,144 PASS (same as compiler_v2).
  - Any divergence is debugged and either fixed or
    documented as a known-issue (with a follow-up TODO).

### Expected failures (and why)
  - Tests that explicitly check for `cel_reset` /
    `cel_alloc` in the emitted wasm bytes — caught in
    Phase 3 already, but the e2e suite has a few extras.
  - Tests that count specific memory offsets — addressing
    is now relative.
  - Tests that rely on cross-eval state in the bump arena
    — there shouldn't be any (cel_reset rewinds), but
    occasionally a test is implicitly assuming it.

### Risk
  - 1-2 days of "this row passes under v2, fails under v3,
    why?" debug work.  Budget it.  Lifetime corners are
    where divergence sneaks in (host reads a CelValue
    payload after eval's mspace is destroyed — classic
    use-after-free).

### Out of scope
  - The bench (Phase 7).
  - Doc updates (Phase 8).

---

## Phase 7 — Bench harness + v2-vs-v3 comparison (2 sessions)

### Deliverables
  - `compiler_v3/bench/v2_vs_v3_bench.cc` — links against
    both `cel::v2` and `cel::v3` namespaces.  Runs the
    13-expression workload (see `BENCHMARK_DESIGN.md`)
    through both.  Captures the metrics in §3.2 of
    HANDOFF.md.
  - Output: CSV + a `RESULTS.md` in this dir summarising
    deltas.
  - Run on a quiet machine (no other load), at least 1000
    iterations per metric, sample variance reported.

### Verification
  - Each workload row produces identical `cel::Value`
    output from v2 and v3 (auto-check; bench aborts on
    mismatch).
  - Bench is reproducible: two runs back-to-back produce
    deltas within ±10% on per-Eval (the noisy metric).

### Acceptance criteria (also in HANDOFF.md §3.4)
  1. e2e + conformance match v2.
  2. Per-Eval cost ratio ≤ 5×.
  3. Per-Instance memory ≤ 1.5× v2 (expected: drops).
  4. Compile cost within ±10%.
  5. RE2 smoke-test: a regex-using expression evaluates
     end-to-end under v3 (proves the architectural payoff).

### Out of scope
  - The decision itself (Phase 8).
  - Adoption work (post-merge).

---

## Phase 8 — Decision + closeout (1 session)

### Deliverables
  - `RESULTS.md` in this directory with the bench numbers,
    pass/fail against the acceptance criteria, and a
    go/no-go recommendation.
  - All affected docs in `doc/implementation-plan/` updated
    (see ANALYSIS.md §2.9 for the file list).
  - PR description summarising what changed, what stayed,
    and what the per-Eval delta means in practice.

### If go: post-Phase 8 follow-ups (not in this branch)
  - Rip out compiler_v2/.
  - Rename compiler_v3/ → compiler_v2/.
  - Update every `#include "compiler_v2/..."` path.
  - Rebase the M5 comprehensions follow-on against the new
    runtime (if it hadn't shipped before).
  - Tighten any lints / clean any TODOs introduced during
    the migration.

### If no-go: post-Phase 8 follow-ups
  - Archive the branch.  Keep RESULTS.md as record of why.
  - Lift any incidental cleanups back into compiler_v2 via
    cherry-pick (the inline-asm comment cleanup, etc.).
  - Document the perf budget that killed it for future
    reference.

---

## Total: 16 sessions, ~3-4 weeks

### Recap of the simplification dividend

After all 8 phases, the codegen + host code paths look like:

```
// Codegen (today):
EmitVariablePrelude(...)
EmitCelResetCall(arena_base, mem_size_bytes)  // gone
EmitExpressionBody(...)
return root_ref

// Codegen (after):
EmitVariablePrelude(...)
(local.set $msp (call $cel_eval_begin))
(local.set $ws (call $malloc ws_bytes))
EmitExpressionBody(...)
(call $cel_eval_end $msp)
return root_ref
```

```cpp
// Host (today):
InitStoreAndMemory(state, impl);  // host allocates memory
InitLinker(state, impl);          // binds cel.memory
InstantiateRuntime(state, impl);  // binds runtime exports
EnsureHostStringArenaCapacity(...);  // 110 LoC of cursor math
EncodeStringOrBytesToHostArena(...);  // 80 LoC of arena writes

// Host (after):
InitStore(state, impl);                                  // just the store
InstantiateRuntime(state, impl);                         // runtime owns memory
LinkRuntimeMemoryAsCelMemory(impl);                      // 5 LoC of plumbing
EncodeStringOrBytes(activation_msp, ...);                // 30 LoC, straight malloc
```

The amount of cognitive overhead removed is the real prize.
The bench validates we didn't pay for it in latency.
