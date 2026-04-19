# celwasmc implementation plan

This folder is the **source of truth** for the state of the CEL → WASM AOT
compiler.  It is structured as one document per milestone (M0–M8) plus a
running test-coverage checklist.  Any new instruction, scope change, or test
obligation from the user gets recorded here so future sessions can pick up
exactly where the last one left off.

Companion documents (do not duplicate — link):
  - `../wasm-compiler-design.md` — the "why / what" design.  Sections 1–14
    define the architecture; §15 tracks the milestone plan at a coarser level
    than this folder.
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

  - `m0-parser-cli.md`       — DONE.
  - `m1-type-checker.md`     — DONE.
  - `m2-codegen-mvp.md`      — IN PROGRESS.
  - `m3-comprehensions.md`   — planned.
  - `m4-string-ops.md`       — planned.
  - `m5-three-valued.md`     — planned.
  - `m6-custom-fns.md`       — planned.
  - `m7-partial-eval.md`     — planned.
  - `m8-conformance.md`      — planned.
  - `testing-checklist.md`   — CEL type × AST-variant coverage grid.
