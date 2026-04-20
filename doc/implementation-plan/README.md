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

  - `m0-parser-cli.md`       — DONE.  CLI can parse + check + emit ABI.
  - `m1-type-checker.md`     — DONE.  Runtime struct + constructors landed.
  - `m2-codegen-mvp.md`      — **IN PROGRESS.**  Scalar slice executes
                                end-to-end via wasmtime.  Remaining:
                                `cel_refs.wat`, wasm32 cross-compile,
                                `cel.abi` custom section, a handful of
                                test-grid gaps.  See the *Remaining for
                                M2* section in that file.
  - `m3-proto-and-strings.md` — **planned.**  Proto field reads
                                (`cel_host.get_field`, `has_field`,
                                `SelectExpr`, `has()`) + string equality
                                and length + string constants through
                                linear memory.  Note: the milestone
                                chart in `../wasm-compiler-design.md` §15
                                calls this "M3 Proto field reads /
                                string ops" — this doc is the canonical
                                scope.  Renamed locally from
                                `m3-comprehensions.md` after the design
                                doc was settled (comprehensions moved to
                                M5 alongside collections; see the
                                ordering note below).
  - `m4-three-valued.md`     — **planned.**  Overflow → ERROR,
                                divide-by-zero, NaN-unordered compares,
                                `UnknownSet`, `cel_status_either`, and
                                the commutativity of `unknown && false`.
                                Swapped with collections on 2026-04-19 —
                                the §8.2 host ABI returns UNKNOWN /
                                ERROR statuses that codegen needs a
                                story for before collections can build
                                on top.
  - `m5-collections-and-comprehensions.md` — **planned.**  List, map,
                                struct literals + the comprehension
                                macros (`all`, `exists`, `exists_one`,
                                `map`, `filter`, nested shadowing).
  - `m6-custom-fns.md`       — **planned.**  `.celfn` IDL, `celfnc`
                                stub generator, `cel_fn.*` emission.
  - `m7-stdlib.md`           — **planned.**  Timestamps, durations,
                                regex, bytes; format directives; extras
                                under `../extensions/`.
  - `m8-conformance.md`      — **planned.**  Run against
                                `tests/simple/testdata/` (the static
                                subset only) as a gate for declaring
                                the compiler production-ready.
  - `testing-checklist.md`   — CEL type × AST-variant coverage grid.
                                Has a top-of-file **Gap summary** that
                                calls out what's open in the active
                                milestone vs. deferred.  The end-of-
                                file **Bench harness** section (added
                                2026-04-20) documents the
                                `compiler/bench/...` Google Benchmark
                                suite and the first-run findings
                                (~420 ns CallEval floor; per-op cost;
                                per-call O(L) string materialisation
                                via `cel_reset`).
