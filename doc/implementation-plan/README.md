# celwasmc implementation plan

This folder is the **source of truth** for the state of the CEL → WASM AOT
compiler.  The active design + per-milestone plans live under `rewrite/`;
this top-level directory keeps the transverse coverage docs and backlog
lists used across every milestone.

> **Navigating the whole `doc/` tree?** Start at
> [`../README.md`](../README.md) — the top-level index that maps every
> doc (upstream spec vs. project planning) with a status per file.

Companion documents (do not duplicate — link):
  - `rewrite/design.md` — the active "why / what" design for the
    `compiler_v2/` rewrite.  Supersedes the original
    `doc/wasm-compiler-design.md`, which has been removed.
  - `../langdef.md` — the CEL language spec we must honour.
  - `CLAUDE.md` at the repo root — rules that apply to every turn.

## Working rules

1. **Update as you go.**  When a milestone ticks off a checklist item, flip
   the `[ ]` to `[x]` in the same commit as the code change.  When the user
   adds a new requirement, add a new bullet with a one-line `(requested
   <date>)` tag.
2. **One checklist per milestone.**  Milestone files hold plan + coverage;
   `testing-checklist.md` is a transverse view over *all* CEL types and AST
   variants, so a single test can satisfy coverage bullets under multiple
   milestones.
3. **Never delete a checklist item silently.**  If it becomes obsolete, strike
   it through (`~~…~~`) and add a note explaining why.
4. **Every compiler feature must have both** a positive test (the feature
   does what it says) and a negative test (malformed or out-of-subset input
   is rejected with a good message).  See `testing-checklist.md` for the
   per-type / per-AST-variant matrix.

## Files

  - `rewrite/` — **active rewrite + design surface.**  All per-milestone
    plans live here:
     - `rewrite/design.md` — the master 12-slice design doc.
     - `rewrite/feature-pipeline-checklist.md` — per-feature-kind
       checklist of files + tests that MUST be touched.
     - `rewrite/m1-scalar-pipeline.md`, `rewrite/m2-ident-select-unknowns.md`,
       `rewrite/m3-map-literals.md`, `rewrite/m4-list-literals.md`,
       `rewrite/m5-*.md`, `rewrite/m7-*.md`, `rewrite/m8-wrapper-types.md`,
       `rewrite/m9-type-subsystem.md`, `rewrite/m10-conversions.md`,
       `rewrite/m-custom-fns.md` — per-milestone plans / shipped notes.
     - `rewrite/phase-c-{research,design,plan}.md` + `rewrite/phase-c-probes/`
       — the Phase C (compiled-expression + host runtime split) research
       and probe artefacts.
     - `rewrite/wasi/` — the WASI migration design (`DESIGN.md`),
       per-milestone notes under `wasi/milestones/`, post-migration bench
       (`POST_MIGRATION_BENCH.md`), and reviews.
     - `rewrite/wat/` + `rewrite/wat-traces.md` — WAT-first ABI traces
       (every new codegen arm / host import lives here before C++ codegen
       lands; see CLAUDE.md "WAT-first" section).
     - `rewrite/cel-host-surface.md` — the host-import surface; supersedes
       the original `doc/cel-host-design.md`.
     - `rewrite/archive/predecessor-*.md`, `rewrite/archive/m-custom-fns.md`
       — retired / superseded designs kept for historical context;
       superseded by `design.md` (predecessors) and `m13-custom-fns.md`
       (the old M6 draft).  One-line pointer stubs remain at the old
       `rewrite/*.md` paths.
  - `testing-checklist.md` — transverse CEL type × AST-variant coverage
    grid.  Has a top-of-file **Gap summary** that calls out what's open
    in the active milestone vs. deferred.
  - `per-component-test-coverage.md` — per-component required test
    scenarios (positive + negative + boundary), the catalog of
    `manual`-tagged targets that MUST run before a milestone closes,
    SKIP discipline, and the closeout gate to copy into every milestone
    PR description.
  - `lint-backlog.md` — known clang-tidy / function-size exceedances
    tracked off the main lint gate; clear them before adding new code in
    the same file.
  - `cleanup-backlog.md` — P2 cleanup items surfaced by code-review
    passes, tagged with the review date so the trail back to context is
    preserved.
  - `repo-restructure.md` — **repo reorganization design**: dissolve
    `compiler_v2/` into role-based top-level dirs (`compiler/`, `eval/`,
    `runtime/`, `abi/`, `common/`), shed the cel-spec Go heritage into
    `spec/`, drop the `api/` umbrella (Abseil/cel-cpp convention:
    `internal/` + `visibility`), and reserve `bindings/` for TS/Go.
    Carries the direction toward compiling the compiler itself to wasm
    (§9) so bindings get full compile + eval.
  - `repo-restructure-execution.md` — the **parallel execution plan** for
    the above: the frozen path/label/include mapping, copy-paste agent
    briefs, and the wave/merge procedure (peak 9 parallel agents in the
    verification fan-out).

## Closing out a planning doc

Planning docs under `rewrite/**/*.md` are living artifacts.  When the
work they describe ships, follow the steps in CLAUDE.md ("Closing out
a planning doc"): update the status header, mark plan-vs-execution
deltas in-line, append a "Future work" section, reconcile sibling
docs, and tick the matching rows in `testing-checklist.md`.
