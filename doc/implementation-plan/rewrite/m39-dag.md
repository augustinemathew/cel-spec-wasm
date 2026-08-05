# m39 — execution DAG

Status: live tracking doc, created 2026-08-04.  Node definitions live
in `m39-component-removal.md` (§3 inventory, §4 commit plan, §5 docs,
§6 dead-API audit, §7 stale-feature inventory) — this file adds only
ordering, parallelism, and status.

Goal state: zero component/plugin code or docs outside the archive
branch and history annotations; host-callback custom functions fully
intact; stale-feature inventory delivered; full gates green
(`bazel build //...`, `$PROJ` + manual-tagged, `lint.sh --branch`,
conformance monotonic both modes, `bug_pins.py validate`); branch
pushed.

## DAG

```mermaid
graph TD
  P0["P0 plan + DAG docs<br/>(this commit)"] --> S1
  P0 --> D1
  P0 --> D2
  subgraph wave1 [Wave 1 — parallel]
    S1["S1 stale-feature inventory<br/>(read-only, free)"]
    D1["D1 e2e + examples + benches<br/>deletion (build-heavy)"]
    D2["D2 CLI: generate/embed-decls<br/>deletion (build-heavy)"]
  end
  D1 --> D3["D3 eval layer: Engine Use/AddPlugin,<br/>cel_plugin, engine-state, required_fn PLUGIN"]
  D2 --> D4
  D3 --> D4["D4 compiler + abi: Builder::Use,<br/>celfnc_emit, function_library kPlugin,<br/>celfn_wire arm, cel_abi.proto, abi/plugin*,<br/>bindings/c"]
  D1 --> D5
  D4 --> D5["D5 toolchain: macro, rng stub,<br/>wit_bindgen, wasm_tools, wasip2 variant,<br/>MODULE.bazel"]
  D4 --> A1["A1 dead-API audit<br/>(caller census post-removal)"]
  D5 --> A1
  D3 --> DOC1["DOC1 milestone-doc annotations<br/>(m13/22/23/24/26/35/36, reviews)<br/>(free, docs-only)"]
  D4 --> DOC2["DOC2 design-doc + user-guide rewrite<br/>(00/02/05/06/07/08, notes, user-guide,<br/>READMEs, PROPOSALS entry)"]
  D5 --> DOC2
  A1 --> DOC3["DOC3 diagrams regen + final doc pass<br/>(render.py, testing-checklist,<br/>feature-pipeline-checklist)"]
  DOC2 --> DOC3
  S1 --> G
  DOC1 --> G
  DOC3 --> G["G gates: lint --branch (once),<br/>bazel build //... + $PROJ + manual,<br/>conformance both modes,<br/>bug_pins validate, zero-hit grep, push"]
  A1 --> G
```

## Constraints the schedule honours

  - **One checkout (`rip-out-components`), one bazel output base.**
    Bazel serializes on the workspace lock: at most 2 build-heavy
    agents concurrent (D1+D2 is the only build-heavy pair; every
    later build node is serial).  Docs/read-only agents (S1, DOC1,
    DOC2) are free and overlap anything.
  - **Disjoint file sets per concurrent agent.**  D1 owns
    `e2e/ examples/ benchmark/`; D2 owns `tools/cel/`.  Each agent
    stages ONLY its own paths (`git add <explicit paths>`, never
    `-A`) and tolerates the other's in-flight edits in `git status`.
  - **No lint in agent loops** — lint (PCH + clang-tidy) contends
    with builds; `scripts/lint.sh --branch` runs exactly once, at G.
    Agents verify with `bazel build` / `bazel test` on touched
    packages only.
  - **No broad process kills** — agents scope cleanup to their own
    PIDs; never `pkill -f` (shared machine).
  - **Tree green after every node's commit** — each node ends with
    its packages building + testing, one commit (or a small series),
    message citing m39.
  - **Docs ship with the change** where the change is user-facing:
    D2 updates `tools/cel/README.md` in its own commit; the broad
    doc rewrite is DOC2/DOC3.

## Status

| Wave | Node | Owner | Status |
|---|---|---|---|
| 0 | P0 plan + DAG docs | orchestrator | done (this commit) |
| 1 | S1 stale-feature inventory | agent | done (478c3ec) |
| 1 | D1 e2e/examples/benches | agent | done (a3f5c62; foreign_fn_type_matrix was plugin-only → deleted whole; ArgkindSlug coverage handoff → D4) |
| 1 | D2 CLI | agent | done (fda6124; its staged deletions were swept into 478c3ec by an orchestrator commit — content correct, wrong message; orchestrator now commits with pathspecs while agents share the tree) |
| 2 | D3 eval layer | agent | running |
| 2 | DOC1 milestone annotations | agent | running |
| 3 | D4 compiler + abi | agent | pending |
| 4 | D5 toolchain | agent | pending |
| 4 | DOC2 design docs + user guide | agent | pending |
| 5 | F1 ofNonZeroValue(message) full-depth delete | agent | pending |
| 5 | F2 stale-skip un-skips | agent | pending |
| 5 | A1 dead-API audit | agent | pending |
| 5 | DOC3 diagrams + final pass | agent | pending |
| 6 | G gates + push | orchestrator | pending |
