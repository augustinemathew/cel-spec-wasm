# m35 — execution DAG

Status: live tracking doc for executing `m35-plugin-ergonomics.md`
(created 2026-07-25).  Updated as nodes complete; deleted or frozen
at milestone closeout.  Node definitions live in the plan doc §12 —
this file adds only ordering, parallelism, and status.

Goal state: full e2e green (unit + e2e + manual-tagged + conformance
monotonic), doc site rewritten (wasm-plugin guide included), branch
pushed.

## DAG

```mermaid
graph TD
  P0["P0 plan fold-in<br/>(rename + slices R/S)"] --> R
  P0 --> A1
  subgraph wave1 [Wave 1 — parallel]
    R["R1–R4 repo-wide rename (code)"]
    R5["R5 doc-site terminology sweep"]
    A1["A1 //abi:wasm_binary + sha256<br/>+ walker migration"]
  end
  R --> R5
  R --> A0["A0 package-override removal"]
  R --> V1["V1 cel.abi field 8 + celfn_wire"]
  A1 --> A2["A2 cel embed-decls tool"]
  R --> A2
  A0 --> A3["A3 macro embed step + e2e pin"]
  A2 --> A3
  A1 --> A4["A4 export-lookup probe + //abi:plugin (Plugin::Load)"]
  R --> A4
  V1 --> V2["V2 emission (post-optimize attach)"]
  V2 --> V3["V3 required_fn_check @ Plan"]
  R --> V3
  V3 --> V4["V4 selective instantiation"]
  R --> B0["B0 AddLibrary → DeclareFunctions"]
  A4 --> B1["B1 Engine::Use"]
  A4 --> B2["B2 Compiler::Builder::Use"]
  B0 --> B2
  B1 --> B3["B3 examples/09 rewrite"]
  B2 --> B3
  A3 --> B3
  V4 --> N["N benchmarks (-c opt)"]
  B3 --> N
  B3 --> S["S1–S5 doc site rewrite<br/>(plugins guide = S3)"]
  R5 --> S
  N --> G["G gates: lint --branch, $PROJ,<br/>manual-tagged, conformance, push"]
  S --> G
  B3 --> B4["B4 closeout (checklists, plan docs)"]
  B4 --> G
```

## Constraints the schedule honours

  - **One checkout, one bazel output base.**  Bazel serializes on
    the workspace lock, so at most ~2 build-heavy agents run
    concurrently; docs agents are free.  Worktree isolation was
    rejected: a fresh worktree pays the ~10-min cold cel-cpp build.
  - **R lands first** among code slices so A/V/B are written under
    final names (no post-hoc rename of new code).  A1 is the one
    exception: file-disjoint from R (new `abi/` files +
    `abi_decode*`/`compile_test` edits R doesn't touch), so it runs
    in Wave 1.
  - **Commit per slice**, repo commit conventions.  **No per-slice
    lint**: lint (PCH build + clang-tidy) serializes behind bazel
    and contends with agent builds — `scripts/lint.sh --branch`
    runs exactly once, at the final gate (G).
  - No broad process kills; agents scope any cleanup to their own
    PIDs (shared machine).

## Waves & status

| Wave | Node | Agent | Status |
|---|---|---|---|
| 0 | P0 plan fold-in (rename + R/S slices + this DAG) | orchestrator | done |
| 1 | R1–R4 code rename | agent-R | pending |
| 1 | A1 wasm_binary + sha256 + migration | agent-A1 | done (eb2f0f7, 692e474, 5736b7a, bc1b485) |
| 1→2 | R5 doc terminology sweep (after R names verified) | agent-R5 | pending |
| 2 | A0 + A2 + A3 (tool + macro chain) | agent-A | pending |
| 2 | A4 probe + Plugin::Load (//abi:plugin) | agent-A4 | pending |
| 2 | V1 + V2 (wire + emission) | agent-V | pending |
| 3 | B0 + B2 (compile-side surface) | agent-Bc | pending |
| 3 | B1 (Engine::Use) | agent-Be | pending |
| 3 | V3 + V4 (check + selective instantiation) | agent-V34 | pending |
| 4 | B3 examples + demo e2e | agent-B3 | pending |
| 4 | N benchmarks | agent-N | pending |
| 5 | S1–S5 doc site (parallel per page group) | agents-S | pending |
| 6 | B4 closeout (testing-checklist, plan-doc status) | orchestrator | pending |
| 6 | G gates: lint --branch, bazel test $PROJ, manual-tagged suite, conformance monotonic, push | orchestrator | pending |
