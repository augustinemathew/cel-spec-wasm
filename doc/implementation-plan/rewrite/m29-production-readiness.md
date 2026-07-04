# m29 — Production readiness: the senior-staff audit burn-down

Status: plan — drafted 2026-06-11, executing via the autonomous loop
(one committed, tested slice per iteration; this doc is the queue).

## 0. Objective and definition of done

The bar: a senior staff engineer auditing this repo for production
adoption finds **no crash reachable from untrusted input, no
front-page claim that doesn't reproduce, and a release they can pin**.
Concretely, M29 is done when:

1. No expression source or activation binding can crash the embedder
   process — every hostile-input path returns a non-OK
   `absl::Status` (the contract `eval/error.h` already promises).
2. The README "Production readiness" section's first two bullets
   (const-aggregate rebuild cost, trap-instead-of-error aggregates)
   are fixed or re-scoped with a shipped mitigation.
3. There is a versioned, tagged release with a human-written
   changelog and a reproducible artifact.
4. The `e2e/known_bugs_test.cc` skip count is materially lower, with
   every remaining skip naming a deliberate scope decision rather
   than an unfixed bug.

What this is NOT: new language surface (no `dyn`, no new extensions),
no bindings beyond C++ (designed-not-built stays honest in the README).

## 1. Inputs (where the gaps are already tracked)

- `README.md` § Production readiness — the four public bullets.
- `doc/implementation-plan/cleanup-backlog.md` — open items #2–#47;
  #47 (parser stack overflow on hostile input) is the standout
  production risk.
- `e2e/known_bugs_test.cc` — ~45 pinned cases, each skip a live TODO
  with the un-skip recipe baked in.
- `e2e/fuzz/` — grammar generator + cel-cpp oracle harness +
  divergence mining already exist; what's missing is *continuous*
  coverage-guided fuzzing, not a from-scratch harness.
- `.github/workflows/ci.yml` — build/test + manual-tagged targets +
  dual-mode conformance gate already run on macOS + ubuntu; CI is
  not a gap for correctness, only for release/fuzz jobs.

## 2. Workstreams, in execution order

### A. Crash-class hardening (top priority — untrusted input)

- [ ] **A1 — backlog #47**: bracket-shape parser stack overflow.
      `ParseAndCheck` on ~2k-deep bracket nesting SIGSEGVs a default
      8 MiB thread inside cel-cpp's ANTLR parser before any gate
      fires. Fix per the backlog entry: measure worst-shape
      per-level cost, then lower `max_recursion_depth` + the depth
      gate to reject gracefully within 8 MiB, or parse on an
      explicit-stack thread. Check cel-cpp upstream parity as part
      of the fix. Exit: hostile bracket input returns
      `ResourceExhausted` on a default-stack thread; regression test
      runs WITHOUT `RunWithLargeStack`.
- [x] **A3 — component resource limits (SHIPPED 2026-07-04)**: an
      untrusted `@component` is arbitrary guest wasm and could hang or
      OOM the host — the wasmtime store had no fuel/epoch/memory limit,
      so the security-model "cannot starve the host / Untrusted OK"
      claim was unbacked. Fixed: a public `ResourceLimits`
      (`eval/resource_limits.h`) with a wall-clock eval deadline
      (default 1s, wasmtime epoch interruption + a per-Engine timer
      thread) and a per-memory cap (default 64 MiB, `wasmtime_store_
      limiter`), configured via `Engine::Builder::WithResourceLimits`
      and on by default; `Unlimited()` opts out. The deadline also
      bounds Plan-time component instantiation (a hang in a component
      ctor). A hit deadline maps to `ResourceExhausted`. Also bounded
      the attacker-controlled `len` in `RandomGetBytesStub`. Tests:
      `eval/resource_limits_test.cc` (value semantics) +
      `e2e/component_resource_limits_test.cc` (infinite-loop →
      ResourceExhausted, memory cap refuses oversized growth, Unlimited
      opt-out, default doesn't false-trip). Docs: security-model §2.5,
      component guide §7, README status bullet.
- [ ] **A2 — trap-instead-of-error aggregates**: the README bullet
      "oversized literal aggregates can trap the runtime". Pinned by
      the arena-cliff family in `known_bugs_test.cc`
      (`ExpressionIntermediatesArenaCliff`, `MapSizeArenaCliff`,
      `BoundStringListInScanArenaOomAt10K`). Exit: each trap becomes
      a graceful eval error; the GTEST_SKIP lines are deleted.

### B. Known-bugs burn-down (spec correctness)

- [ ] **B1 — small-conversion cluster**: `IntFromStringLeadingPlus`,
      `UintFromStringLeadingPlus`, `DoubleFromStringRejectsWhitespace`,
      `FormatFixedRejectsInt`, `FormatFixedAcceptsNanToken` — each a
      bounded kernel/celfn fix that deletes one skip. Oracle-confirm
      expected values per the CLAUDE.md oracle rule before fixing.
- [ ] **B2 — equality/indexing cluster**: `MapKeyLossyDoubleEquality`,
      `DynDoubleListIndexCoercion`, `DynUintListIndexCoercion` —
      heterogeneous-numeric semantics; one design note, one fix.
- [ ] **B3 — remaining skips triage**: each surviving skip either
      gets fixed or re-justified as deliberate scope (then moved out
      of "known bugs" framing). Exit: zero skips that say "bug".

### C. Front-page performance debt

- [ ] **C1 — re-measure the 1000-term arithmetic chain** post
      slot-reuse (`benchmark/eval/run.sh`, `-c opt`) and restore the
      README/FAQ row that currently says "pending re-measure".
      Cheap; do early — it's a front-page claim sitting empty.
- [ ] **C2 — const-aggregate folding**: the 44× map-literal loss row.
      Constant lists/maps build once (rodata or plan-time memory
      image) instead of per-Eval. This is the largest engineering
      item and may become its own mNN doc + WAT trace per the
      WAT-first rule; M29 scopes the design + the list-literal case,
      and re-scopes after measuring.

### D. Continuous fuzzing

- [ ] **D1 — nightly fuzz job**: wire the existing `e2e/fuzz`
      generator + oracle harness into a scheduled CI job (seeded
      corpus from `spec/tests/`), failing on crash or divergence.
      Coverage-guided libFuzzer is a follow-up; scheduled
      grammar-PBT closes most of the gap at near-zero new code.

### E. Release engineering (currently absent: no tags, no changelog)

- [ ] **E1 — `v0.x.0` tag + CHANGELOG.md** written for humans (fmt
      standard: grouped by user impact, migration notes), plus a
      release job attaching a built `cel` CLI artifact per platform.
- [ ] **E2 — version stamp**: surface a version string in the `cel`
      CLI and `cel.abi` so a shipped Program names the compiler that
      produced it.

### F. Doc honesty (continuous, riding each slice)

- [ ] **F1** — README/FAQ rows updated in the same commit as each
      fix above (the A2 and C rows are quoted on the front page).
- [ ] **F2** — design-doc drift: `doc/design/01-compiler.md:678-680,758`
      and `doc/design/05-custom-functions.md` §"dead surface" still
      cite the deleted `compiler/celfn/library_module.h`; re-anchor
      the way `doc/user-guide/index.md` was (commit 5d4e041).

## 3. Working rules for every slice

Per CLAUDE.md, non-negotiable: interface → tests → implementation;
every fix deletes (never orphans) its pinned skip in the same commit;
`scripts/lint.sh` in the loop and `--branch` at the gate;
`bazel test` on the touched package per edit; conformance gate before
any push; README/FAQ numbers reconciled in the same commit that
changes them.

## 4. Future work

(Collected as slices land; nothing pre-committed.)
