# celwasm-tool — design

> **Status:** research + design only (drafted 2026-06-06).  No code in
> this pass; the purpose is to model the abstractions and the
> organisational structure of the cel-spec-wasm project so that a
> later workstream can build a navigation / project-management tool
> on top of the model.

---

## 0. Executive summary (one page)

**What this is.** cel-spec-wasm is a CEL→WASM AOT compiler with a
3-week velocity (~235 commits in May 2026 alone, single-author
trunk), a ~190 KB design document, ~108 `.md` planning docs, ~270
milestone-named slices already shipped, 44 numbered backlog items
(open + closed), 110 `_test.cc` files, ~110 Bazel `cc_test` targets,
67 `cc_library` targets, 9 first-party role directories, 17 `m21`-style
extension milestones, ~50 WAT-first design traces, a 2454-row
conformance corpus run as a manual target, a Property-Based-Testing
oracle, and a `cleanup-backlog.md` ledger numbered #1–#43 that
threads `cite` references through commit messages, reviews and code
review reports back to the surfacing event.

**The problem.** Reading any single change in this repo requires
holding eight simultaneous coordinates in your head:

  1. *Which milestone* (numbered `mNN-*`, slices `A..G`, with the
     three-layer state shape *declared / e2e / tested*).
  2. *Which physical role dir* (the spine `compiler/ → eval/ →
     shared/ → abi/ → runtime/`).
  3. *Which pipeline stage* (the ~13-stage spine listed in
     `feature-pipeline-checklist.md`).
  4. *Which abstraction* (`NodeAnnotation` / `Storage` / `Origin` /
     `OverloadTable` / `Repr` / `CelKind` / `kBuiltinSeeds` / the 7
     `kPendingRuntimeExports` / `AttributePattern` / `Activation` / …)
  5. *Which WAT trace* that abstraction has an executable spec in
     (`wat/01_literal_42.wat` .. `m14_*.wat`, ~50 files).
  6. *Which testing-checklist row* the change ticks (the grid
     under `testing-checklist.md`, ~3000 lines, ~16 H2 sections).
  7. *Which cleanup-backlog entry* the change cites or surfaces
     (`#1`..`#43`).
  8. *Which conformance rows* it flips (the corpus has ~2454 rows;
     per-fixture pass/skip/fail headlines are auto-generated into
     `conformance/README.md`).

Today these coordinates exist only in (a) developer head, (b) text
files connected by string-search, and (c) the user's bespoke memory
of "feedback notes" in `~/.claude`.  The repo's CLAUDE.md is itself
an attempt to mechanise some of these joins (the rules about "don't
skip a feature-pipeline-checklist row," "every WAT trace is
executable," "every milestone-doc must have a status header") —
but enforcement is per-reader, not per-tool.

**What the tool does.** It builds a typed graph of those eight
coordinate systems plus the **bridges between them**, indexes it
off the repo's own canonical artefacts (git, Bazel, the
`testing-checklist.md` and `cleanup-backlog.md` files,
`conformance/README.md`, the `.wat` directory, `compile_commands.json`),
and presents that graph through a handful of cross-referencing views.
*Click an abstraction → see every physical site that touches it.
Click a backlog item → see the commits that cite it, the docs it
was surfaced in, and the tests that pin its absence.  Click a
milestone doc → see which sections went green, which are still
open, and which test files exercise them.*

**Why it matters now.** Three forcing functions:

  - **The "M2 incident" template repeats.** `per-component-test-coverage.md`
    documents how M2 shipped with 29 silent `GTEST_SKIP`s because
    `bazel test //...` was green but the `manual`-tagged e2e wasn't
    run.  The model that catches this is "feature × manual-target
    coverage" — exactly what a tool can render.
  - **Cleanup-backlog citation is brittle.** Today a commit cites
    `cleanup-backlog #N` in its message; reverse-finding "what
    closed #20" requires `git log --grep '#20'`.  A tool can pin
    the bidirectional link.
  - **Milestone docs go stale.**  The CLAUDE.md "closing out a
    planning doc" rule is honored unevenly; some docs say
    `Status: shipped 2026-04-22` and some still say `drafted`
    after the work landed.  Cheap to render the truth from git.

The tool is **read-mostly**: it surfaces what's already there and
where it lives, doesn't author new state.  Writes are limited to
ticking checklist boxes and appending backlog entries — both of
which already happen by hand and benefit from a tighter loop.

---

## 1. Phase 1 — docs catalog

The doc tree is the primary abstraction layer the project advertises
to humans.  108 `.md` files total, distributed across six
**layers**:

### 1.1 Layer L0 — Spec / upstream contract

  - `doc/langdef.md` (2,234 lines) — the CEL language spec.  Cited
    throughout test comments ("per langdef §… map keys must be
    comparable").  Source of truth for 3VL / partial-eval / type
    coercion assertions.  **Vocabulary contributed:** `dyn`,
    `RejectDyn` static subset, `AttributePattern`, `?:` 3VL,
    map-key kinds, list ordering, comprehension macros (`all`,
    `exists`, `exists_one`, `map`, `filter`), the operator-precedence
    table, every standard built-in overload.

  - `spec/tests/simple/testdata/*.textproto` — the upstream
    conformance corpus (2,454 rows, fixture-grouped).  Cited by
    file:row in tests (`comparisons.eq_int_double_special`,
    `string.textproto::matches/00`).  Hot file in commit graph
    (top-10 most-edited at 26 edits).

### 1.2 Layer L1 — Architectural design

  - `doc/implementation-plan/rewrite/design.md` (4,018 lines, the
    single largest doc) — the rewrite's end-to-end architecture.
    **Vocabulary contributed:** `ResolvePass`, `LayoutPass`,
    `Storage` / `StorageKind` / `kStaticRodata` / `kWorkspaceSlot` /
    `kLocal`, `Origin` / `kArena` / `kHost` / `kDynamic`,
    `NodeAnnotation` (with 9 fields), `OverloadTable` /
    `OverloadImpl` / `ImportModule`, the `(out_slot, args…) -> void`
    uniform call ABI, the "Five questions" framework, the
    `cel.abi` custom-section schema, the two-phase Engine/Instance
    split, the Phase C shared-memory model, `kPendingRuntimeExports`
    (a 7-entry guard set), the slice graph S1..S12, the
    "plan-vs-execution delta" callout pattern.

    This doc is interleaved with `> Plan-vs-execution delta:`
    callouts that explicitly compare the as-written design with
    the as-shipped reality.  *The callouts are the primary
    structural signal that an abstraction has drifted — they are
    candidate "view items" by themselves.*

  - `doc/implementation-plan/rewrite/cel-host-surface.md` (62,748 B)
    — the authoritative C++ public API spec (`Cel::Compiler` /
    `Cel::Program` / `Cel::Engine` / `Cel::Instance` /
    `Cel::Activation` / `Cel::Value` / `Cel::RuntimeBindings`).
    Cited by design.md when the two touch ("`cel-host-surface.md`
    wins").

  - `doc/implementation-plan/rewrite/map-list-dispatch.md` —
    three-path origin dispatch (`kArena` / `kHost` / `kDynamic`)
    for both maps and lists.  *Supersedes* §4.7.2 of design.md.
    Pattern: a focused doc that retroactively overrides part of
    the main design.

  - `doc/implementation-plan/rewrite/memory-layout-design.md` /
    `wasi/DESIGN.md` — the post-Phase-C memory model
    (malloc-backed arena, runtime-owned shared memory,
    `--global-base=8192`).  Plays the same "supersedes a section
    of design.md" role for memory.

  - `doc/implementation-plan/rewrite/two-phase-runtime-isolation.md`
    — Engine/Instance role split.  Same pattern.

  - `doc/implementation-plan/rewrite/modules-and-ffi.md` —
    custom-function modules + the foreign-FFI ABI (the
    `@host.` vs `@component.` distinction).  Cited by m13/m22/m23.

### 1.3 Layer L2 — Per-milestone planning

41 `mNN-*.md` files under `doc/implementation-plan/rewrite/`.
Naming convention is mandated: `mNN-<kebab-name>.md`.  Each carries:

  - A `Status:` header line (one of: `plan — drafted YYYY-MM-DD,
    not yet started.` / `in flight` / `shipped YYYY-MM-DD`).
  - A scope statement.
  - A slice plan (typically letters: `A`, `B`, `C`, `D`, ..., with
    sub-slices like `M5.D step 2`).
  - "What landed" + "Future work" trailers when shipped.
  - "Plan-vs-execution delta" callouts when scope shifted mid-flight.

**Naming gaps tell a story.**  Numbers go `m1`, `m1b`, `m2`, `m3`,
`m4`, `m5`, `m5b`, `m7`, `m7a`, `m7b`, `m8`, `m9`, `m10`, `m11`,
`m12`, `m13`, `m14`, `m16`, `m17`, `m18`, `m20`, `m21`, `m22`, `m23`,
`m24`, `m26`, `m27`.  Missing numbers (m6, m15, m19, m25) are
either folded into siblings (m25 → m24 — see `docs: merge m25
(DX) into m24` in git log) or live on un-merged branches.  The
**milestone-graph view** wants to surface this.

Each milestone doc also typically has companion ASCII tables — the
M13 doc has the canonical example: three columns *L1 declare/check
/ L2 eval e2e / L3 tested* with rows-per-feature.  This pattern is
the prototype for what the project-management view should render.

**Companion docs:** `mNN-ast-probe-findings.md`, `mNN-probes.md`,
`mNN-reviews/`.  The probe docs are throwaway by design (CLAUDE.md
"probes are disposable") but several have been kept post-close as
reference; the milestone docs cite them by path.

### 1.4 Layer L3 — Process / quality

  - `doc/implementation-plan/testing-checklist.md` (2,716 lines, 16
    H2 sections, 61 edits — *second most-edited file in the repo*)
    — the running grid of "what coverage shipped per milestone."
    Two ground-truth tables:

      * **Per CEL type × pipeline stage** (rows: CEL types incl.
        scalars / list / map / message / wrapper / duration /
        timestamp / optional; columns: parser / checker /
        annotations / RejectDyn / codegen / e2e).
      * **Per `ExprKindCase` × pipeline stage** (rows: each AST
        node kind; columns: as above).

    Plus per-milestone unticked-rows-as-they-ship sections
    (`## Rewrite M8 — wrapper types`, `### Rewrite M16 — math_ext`, …).
    *This is the most directly tabular doc and is the obvious
    first source for the "coverage matrix" view.*

  - `doc/implementation-plan/per-component-test-coverage.md` — the
    "M2 incident" doc.  Defines: **the rule** (a feature is not
    done until every layer has a passing test, AND every
    manual-tagged target runs green, AND the testing-checklist row
    is ticked); **the manual-tagged target catalog** (10 targets);
    per-component required scenarios (sections 3.1 .. 3.13); SKIP
    discipline.  Has its own "closing-out gate" template for
    milestone PR descriptions.

  - `doc/implementation-plan/rewrite/feature-pipeline-checklist.md`
    — the "for every feature type, here are the files + tests"
    forcing function.  Defines the **pipeline spine** as 13
    stages: Frontend → Typed IR → Codegen passes → Top-level
    facade → Runtime → Host imports → ABI → Public API → CLI / e2e
    / conformance → WAT prototyping.  Section 2 enumerates 6
    feature-type templates (new AST kind / new declarable type /
    new host fn / partial-eval-plumbing / new expression lowering /
    new ABI field) with per-file checklists.

  - `doc/implementation-plan/cleanup-backlog.md` (1,479 lines, 14
    edits) — the numbered P2 work-queue ledger.  Open items
    (`#43`, `#41`, `#40`, …) carry a description, surfacing event,
    affected files, and "why P2."  Closed items strikethrough or
    move to a `## Closed` tail with closure notes.  44 numbered
    entries today.  *This is the work-queue source of truth.*

  - `doc/implementation-plan/lint-backlog.md` (19 edits) — clang-tidy
    findings, by check, with counts.  Companion to the
    `scripts/lint.sh` workflow.

  - `doc/implementation-plan/dev-loop-performance.md` — the analysis
    behind the CLAUDE.md "stay in one configuration" rule.

  - `doc/implementation-plan/known-issues-findings.md`, `design-pressure-test-prompt.md`,
    `runtime-catalogue-genrule.md`, `repo-restructure*.md` — single-topic
    investigations.

### 1.5 Layer L4 — Reviews / retros

  - `doc/implementation-plan/rewrite/reviews/` — generic review
    reports (4 files, all `YYYY-MM-DD-slug.md`).
  - `doc/implementation-plan/rewrite/m13-reviews/` — milestone-specific
    review subdir, same pattern.
  - `doc/implementation-plan/rewrite/wasi/reviews/`.

Pattern: CLAUDE.md describes "Periodic code review (every few
commits, every milestone closeout)" — the reviews dir is where
those land.  Review reports are timestamped, slugged, carry a
**verdict** (`clean / dirty / mixed`), and surface entries into
`cleanup-backlog.md`.  *They form a "trail" of audits over time
that no current view aggregates.*

### 1.6 Layer L5 — WAT executable specs

  - `doc/implementation-plan/rewrite/wat/` (~50 `.wat` files +
    `BUILD.bazel`).  CLAUDE.md mandates: *every new codegen arm or
    host-ABI surface starts as a `.wat` file, assembled with
    `wasm-as`, then run through `tools/wat_runner` with stub
    impls before any C++ is written.*

  - `doc/implementation-plan/rewrite/wat-traces.md` (2,592 lines,
    18 edits) — the walkthrough document.  One section per WAT
    file, explaining memory layout, the source expression, the
    expected emit shape, and invariants.

WAT files are named `NN_<feature>.wat` (e.g. `04_select_c_name.wat`,
`30_logical_and.wat`, `56_wrapper_kstruct_unwrap.wat`).  Numbering
groups by family (literals 01–09, indexing 12–15, control flow
30–33, struct 40–41, time 50–55, comprehensions 60–67, M-prefixed
files for late-arriving milestones).  *The numbering scheme itself
is a navigation aid the tool can surface.*

### 1.7 Cross-cutting

  - `CLAUDE.md` (858 lines, 27 edits, *third most-edited file*) —
    the project's self-description.  Not a doc consumed in
    isolation; consumed *before* every change.  Reads as a stack
    of accumulated rules, each with a "why" anecdote.  The
    abstractions it names by its own headers: *repo rules*,
    *authoritative docs*, *C++ style*, *lint & format*, *probe
    vendored cel-cpp*, *the oracle is the empirical tiebreaker*,
    *WAT-first*, *testing*, *reporting bugs via tests*, *build
    & run*, *visibility regime*, *bench config*, *dev-loop perf*,
    *periodic code review*, *what not to do*.

  - `README.md` (343 lines, 23 edits) — public face.  Carries the
    conformance headline, the benchmark table, the "language
    bindings" plan, and the `@host.` vs `@component.` table.

  - `~/.claude/projects/.../MEMORY.md` (user-private; lives outside
    the repo but is part of the operating context).  Tagged
    bullets pointing at deeper memory files.  Captures the "what
    the user finds non-obvious" view — e.g. the `force push to
    master skips hook` rule, the `Don't commit unless asked`
    rule, the `CEL type matrix — spell it out, exhaust it` rule.
    The tool *should not* surface this directly (it's private),
    but the rules in it are exactly the kind of "invariants
    nobody currently writes down" the abstractions view wants
    to model.

---

## 2. Phase 2 — git-history mining

### 2.1 Velocity

| Month | Commits |
|---|---|
| 2026-06 | 36 (partial, through the 6th) |
| 2026-05 | 235 |
| 2026-04 | 126 |
| pre-2026 | scattered upstream-cel-spec heritage |

The repo's *current* identity (CEL→WASM AOT compiler) began life as
a fork of cel-spec; the heritage commits are upstream maintenance
of the `.textproto` corpus.  Real velocity starts 2026-04
(`compiler_v2` parallel-tree begins) and continues at a sustained
~7/day for 2.5 months.

**Total commits since 2026-04:** ~400 by Augustine Mathew (391 of
738 total when including upstream heritage).

### 2.2 Commit-message taxonomy

Conventional commits with project-specific prefixes:

| Prefix family | Examples | Rough share |
|---|---|---|
| `docs:` | `docs: rewrite README + user-guide closeout` | 17 |
| `mNN:` (milestone-tagged) | `m26 D.3: rewire foreign_component_bench…`, `m24: §13 probe resolves…`, `m21: host-call adapter — typed…` | ~50 (M4, M3, M12, M26 lead) |
| `restructure(WN)` | `restructure(W3): …` | 6 (the role-dir reorganisation) |
| `conformance:` | `conformance: proto2 extension field reads (cleanup-backlog #40 partial)` | 4 |
| `tests:` | `tests: Gap 1 trampoline e2e for #36; defer Gap 3…` | 3 |
| `eval:`, `codegen:`, `runtime:`, `bench:`, `lint:`, `phase-c:`, `wasi:` | by physical role | scattered |
| Special sentinels | `session:` (multi-topic), `known-bugs:`, `compiler_v2/…:` | rare |

**Embedded references in messages** (this is the load-bearing
project-management signal):

  - `cleanup-backlog #NN` — bidirectional citation of P2 items.
  - `#15`-style PR references — for merged PRs.
  - Slice nomenclature: `M5.D step 2`, `Slice C delta 1`, `B.2.1`.
  - Conformance counts: `1899 → 1949`, `1058 → 1137 (+79 PASS)`.

A grep of the last 300 messages for `\bmilestone #?[0-9]+\b`,
`\bcleanup-backlog #\d+\b`, `\bSlice [A-Z]`, `\b[0-9]+ → [0-9]+\b`,
and `\b#\d+\b` would build the entire reference graph the tool
needs.

### 2.3 Refactor events

Identifiable by message patterns:

  - **`restructure(WN)`** — the role-dir reshape that moved
    `compiler_v2/` into top-level `compiler/`, `eval/`, etc.
    (May 2026; W1..W6).
  - **`docs: rewrite stale `compiler_v2/` path refs…`** — the
    follow-up doc-drift cleanup.
  - **`compiler_v2/` → top-level** — visible in the hot-file
    list: `compiler_v2/runtime/cel_runtime.c` (35 edits) +
    `runtime/cel_runtime.c` (18 edits) sum to 53 edits on what is
    architecturally one file.  *The tool needs path-rename
    awareness.*
  - **`Revert "..."`** — present in the log (`ec42a9f2 Revert
    "docs: consolidate foreign-fn design into one doc (m23); fold
    in m24"`).  Indicates plan-vs-execution disagreements
    visible in the message graph.
  - **`docs: m25 — foreign custom-fn developer experience` →
    `docs: merge m25 (DX) into m24 — one foreign-custom-fn design
    doc`** — milestone-numbering reshuffle: m25 was drafted, then
    folded into m24 four days later.  *This explains the gap in
    milestone numbering.*

### 2.4 Hot files (the load-bearing surfaces)

Top 10 by total commit count, excluding `third_party/cel-cpp`:

  1. `doc/langdef.md` (80) — upstream-heritage churn.
  2. `doc/implementation-plan/testing-checklist.md` (61).
  3. `compiler_v2/runtime/BUILD.bazel` + `runtime/BUILD.bazel` (47 + …).
  4. `compiler_v2/runtime/cel_runtime.c` + `runtime/cel_runtime.c` (35 + 18).
  5. `compiler_v2/conformance/README.md` + `conformance/README.md` (34 + …) — auto-generated headline.
  6. `compiler/codegen/expr_lower.cc` + `compiler_v2/codegen/expr_lower.cc` + `compiler/codegen/expr_lower.cc` (33 + 27 + 33).
  7. `compiler_v2/api/internal/cel_host.cc` (33) — now `eval/internal/cel_host.cc`.
  8. `compiler_v2/api/engine.cc` (33) — now `eval/engine.cc`.
  9. `CLAUDE.md` (27).
  10. `compiler_v2/compile.cc` + `compiler/internal/compile.cc` (25).

**Interpretation:** `expr_lower.cc` + `cel_host.cc` + `cel_runtime.c`
+ `compile.cc` + `engine.cc` are the five files where the *whole
pipeline meets*.  These are the natural focus targets for the
"what's the most-touched code" view.

### 2.5 Working pattern

  - **Trunk-based.**  Almost everything lands as a direct push to
    master.  PRs (`Merge pull request #NN`) appear ~20 times in
    the log — mostly for review-mandated changes (the
    `runtime-catalogue-genrule`, the README rewrite, the
    component-bench rewire).
  - **One author dominant** (391/738 commits since the project's
    refocus; the rest are upstream cel-spec heritage).
  - **Force-push to master is a known operation** for the
    pre-push-hook-timeout case (the user has a private memory
    note about it).
  - **Commits are small and squashable** — CLAUDE.md says "Each
    slice is one squashable commit," and the log corroborates:
    descriptive subject lines, focused scope.

---

## 3. Phase 3 — organisational structure

### 3.1 Top-level role dirs (the "role layout")

Mandated by CLAUDE.md:

| Dir | Role | Public API? |
|---|---|---|
| `compiler/` | compile-time: CEL source → `Program` (wasm bytes + `cel.abi`) | yes: `compiler.h`, `program.h` |
| `compiler/frontend/` | parse + check (wraps cel-cpp) | internal |
| `compiler/ir/` | typed AST + annotations | internal |
| `compiler/codegen/` | Binaryen lowering | internal |
| `compiler/celfn/` | function library + .celfn IDL parser | internal |
| `compiler/internal/` | private `compile.{h,cc}` pipeline facade | internal |
| `eval/` | eval-time: `Program` + `Activation` → `Value` | yes: `engine.h`, `instance.h`, `activation.h`, `value.h`, `error.h`, `attribute.h` |
| `eval/host/` | cel_log trampolines | internal |
| `eval/internal/` | wasmtime glue, `abi_decode`, `cel_host`, `cel_component` | internal |
| `shared/` | `CelType`, the type vocabulary | yes: `type.h` |
| `abi/` | the `cel.abi` wire contract (emit + parse) + runtime catalogue | yes: per-target |
| `runtime/` | `cel_runtime.c` → `cel_runtime.wasm` (language-agnostic kernel) — split into ~30 `.c` files by topic (cel_arena, cel_arith, cel_compare, cel_make, cel_log, cel_map, cel_list, cel_3vl, cel_matches, cel_math_ext, cel_string_*, cel_time, cel_optional, cel_net_ext, cel_base64_ext, cel_convert) | yes |
| `tools/` | `cel` CLI (`tools/cel/`), `wat_runner` (`tools/wat_runner/`) | binaries |
| `conformance/` | harness (runner + binding marshal) | binary |
| `e2e/` | per-milestone e2e tests (m2_test … m18_test, plus host_fn_*, foreign_component_*, fuzz/, known_bugs_test) | tests |
| `bench/` | kernel / pipeline / in-operator / foreign-component benches | binaries |
| `testdata/` | shared proto fixtures + `cel_cpp_oracle_test.cc` | data |
| `spec/tests/` | upstream conformance corpus (textproto) | data |
| `bindings/` | language bindings — currently `ts/` only (TypeScript shim, in-design) | future |
| `probes/` | per-milestone throwaway investigations (per CLAUDE.md) | discardable |
| `docker/` | reproducer Dockerfile | infra |
| `scripts/` | lint, compile-db refresh, conformance gates, suite runner | infra |

**Bazel scale (first-party only):**

  - `cc_library`: ~67
  - `cc_test`: ~110
  - `cc_binary`: ~19
  - `proto_library`: 6
  - 37 `BUILD.bazel` files total

`compiler/` and `eval/` **share `shared/`** but do not depend on
each other (per CLAUDE.md "neither depends on the other").  This is
load-bearing: it keeps `compiler.wasm` reachable as a future build
target.

### 3.2 Visibility regime (two-tier)

  - **Public** — explicit `//visibility:public` (curated, ~13
    targets).
  - **First-party internal** — `//:internal` package_group (every
    first-party package).

The `internal/` subdir naming and the Bazel visibility move
together.  CLAUDE.md treats widening to public as a *reviewable
event*.

### 3.3 Test layout

  - **Per-file unit** — every non-trivial `.h`/`.cc` has a paired
    `*_test.cc` next to it (mandated rule).
  - **Per-milestone e2e** — `e2e/m<N>_test.cc` (m2, m4, m5, m5b,
    m7, m7a, m7b, m8, m9, m10, m12, m14, m16, m17, m18); plus
    `e2e/m2_partial_eval_test.cc`.
  - **Cross-cutting e2e** — `e2e/host_fn_test.cc`,
    `e2e/host_fn_type_matrix_test.cc`,
    `e2e/foreign_component_dispatch_test.cc`,
    `e2e/foreign_fn_type_matrix_test.cc`,
    `e2e/known_bugs_test.cc`, `e2e/proto_arena_lazy_copy_test.cc`,
    `e2e/slot_aliasing_test.cc`, `e2e/wkt_field_set_test.cc`,
    `e2e/optimize_test.cc`, `e2e/program_roundtrip_test.cc`,
    `e2e/mvp_concat_test.cc`.
  - **Fuzz / PBT** — `e2e/fuzz/` (generator, grammar, oracle
    harness, mine_divergences, repro_pbt_bug).
  - **Conformance** — `conformance/runner_test.cc` (unit on the
    runner) + `conformance/run_conformance` (the corpus
    executable, `manual`-tagged).
  - **Oracle** — `testdata/cel_cpp_oracle_test.cc` — the empirical
    tiebreaker; CLAUDE.md mandates "add a case to the oracle test
    to settle a question."

110 `_test.cc` files total.

### 3.4 The 10 manual-tagged targets

Per `per-component-test-coverage.md §2`:

| Target | What it covers |
|---|---|
| `//eval:instance_test` | Full Compile→Plan→Eval |
| `//eval:engine_test` | Engine::Plan, runtime instantiation |
| `//eval:cel_host_test` | Layer 1 + 2 trampolines |
| `//bench:cel_pipeline_bench` | Regression catch |
| `//e2e:m<N>_test` | Per-milestone e2e |
| `//e2e:eval_test` | Cross-cutting (flagged stale; see doc) |
| `//runtime:cel_runtime_wasm_test` | wasm32 cross-compile under wasmtime |
| `//tools/wat_runner:wat_runner_test` | WAT trace regression |
| `//conformance:run_conformance` | Upstream corpus |
| `//tools/cel:cel_smoke_test` | CLI e2e |

`scripts/run_full_suite.sh` is the suite-runner script.

---

## 4. Phase 4 — data sources for the tool

For each candidate source, the shape of the data, freshness, and
cost.

| Source | Shape | Freshness | Cost |
|---|---|---|---|
| **Filesystem** (`find`, raw read) | tree of files + sizes + mtimes | sub-second | cheap |
| **Git log** (`git log --pretty=…`, `git log --name-only --pretty=…`, `git blame`) | rows of `(sha, author, date, subject, body, files[])` | sub-second on warm repo | cheap |
| **Bazel targets** (`bazel query 'kind("cc_*", //...)'`) | (target_label, kind, srcs, deps, visibility) | seconds; revalidates on BUILD edits | medium (~5–15s warm; longer cold) |
| **Bazel deps graph** (`bazel query 'deps(...)'`, `rdeps(...)`) | edges between targets | seconds | medium |
| **Conformance runner** (`bazel run //conformance:run_conformance`) | per-row `(fixture, row_name, outcome, skip_category?)` plus headline counts | minutes (~5 min cold, ~10s warm) | expensive |
| **PBT oracle miner** (`e2e/fuzz/mine_divergences`) | `(seed, target, depth, divergence_kind)` | minutes | expensive |
| **`compile_commands.json`** | per-TU compiler flags (already cached at repo root) | regenerated by `scripts/refresh_compile_db.sh` | cheap to read; expensive to refresh |
| **clangd / LSP** | per-symbol `(def, refs[])` once indexed | indexing is minutes-cold | expensive cold, cheap warm |
| **Markdown parse** (the planning docs) | extract Status headers, code-fenced snippets, `mNN-*` references, `cleanup-backlog #NN` cites, "Plan-vs-execution delta" callouts, `[x]` / `[ ]` checkbox grids | sub-second | cheap (regex / mistune) |
| **WAT-runner output** | per-WAT file: assembles? validates? runs through wasmtime? matches expected? | seconds per file | cheap (~50 files, parallelizable) |
| **`cel_cpp_oracle_test.cc`** | `(source_expression, container) -> OracleResult` | per-case ms | cheap incremental |
| **agent activity** (`.claude/projects/.../`) | running/finished agent transcripts, status files | live | n/a (read-only) |
| **gcov / llvm-cov coverage** | per-source per-line hit counts | minutes | expensive — *defer unless cheap* |
| **`scripts/run_full_suite.sh`** outputs | green/red per manual-target | minutes | expensive |
| **`conformance/README.md`** (auto-gen headline) | pass/skip/fail counts | committed | cheap |

**The "live primary" sources** for the tool's day-to-day loop are:
filesystem, git log, markdown parse, `conformance/README.md`'s
auto-gen block.  The "snapshot secondary" sources (bazel query,
conformance run, PBT mining, full-suite runs) are slow enough that
the tool caches their last output and labels it with the git SHA
it was produced against.

### 4.1 Freshness contract

  - **Live (< 1s, refresh per request):** filesystem, git, markdown
    parse, `conformance/README.md` auto-gen excerpt.
  - **Snapshot (cached, labelled with git SHA):** bazel query
    output, conformance corpus per-row results, PBT divergence
    corpus, full-suite green/red.
  - **On-demand (user-triggered):** `bazel test`, `lint.sh`,
    `wat_runner` run.

---

## 5. Phase 5 — abstractions ↔ structure ↔ bridges

This is the catalog the tool is built around.

### 5.A Logical abstractions

For each: **name** | **vocabulary site(s)** | **physical site(s)** |
**human navigation question**.

#### A.1 The pipeline spine
  - 13 stages from `feature-pipeline-checklist.md §1`.
  - Vocab: design.md / feature-pipeline-checklist.md / per-component-test-coverage.md.
  - Physical: maps one-to-one to role dirs and key files.
  - *"Walk this feature through every stage; what's not done?"*

#### A.2 Milestone
  - mNN slug + status.
  - Vocab: every `mNN-*.md`, commit message prefixes, `Status:`
    header line, design.md §11.4 slice graph.
  - Physical: `e2e/m<N>_test.cc`, plus the slice tags scattered in
    code comments (CLAUDE.md says no NEW comments may carry
    milestone refs; existing ones are grandfathered).
  - *"What's M5's status?  What test pins M14 Slice E?"*

#### A.3 Slice / Sub-slice
  - Letter or letter+number nomenclature: `M5.D step 2`, `Slice C
    delta 1`.  No fixed schema; lives inside milestone docs.
  - Vocab: milestone docs, commit subjects, design.md slice graph.
  - *"Which slices of M5 shipped vs are pending?"*

#### A.4 AST kind (`ExprKindCase`)
  - The cel-cpp enum: `kConstant`, `kIdentExpr`, `kSelectExpr`,
    `kCallExpr`, `kCreateMap`, `kCreateStruct`, `kCreateList`,
    `kComprehensionExpr`.
  - Vocab: design.md §4.4, testing-checklist.md per-`ExprKindCase`
    grid, codegen `switch` in `expr_lower.cc`.
  - Physical: every codegen `case k…:` arm + every IR
    `PostVisit…` visitor + tests that build the corresponding
    AST.
  - *"Where is `kSelect` handled at each stage?"*

#### A.5 CelKind / Repr
  - Wire-level kinds: `CEL_NULL`, `CEL_BOOL`, `CEL_INT`, `CEL_UINT`,
    `CEL_DOUBLE`, `CEL_STRING`, `CEL_BYTES`, `CEL_LIST_ARENA=7`,
    `CEL_MAP_ARENA=8`, `CEL_MAP_HOST=9`, `CEL_MESSAGE`, `CEL_LIST_HOST=17`,
    `CEL_DURATION`, `CEL_TIMESTAMP`, `CEL_UNKNOWN`, `CEL_ERROR`,
    `CEL_OPTIONAL` (+ wrapper-via-message).
  - `Repr::k…` enum on `NodeAnnotation`.
  - Vocab: `runtime/cel_data.h`, `compiler/ir/annotations.h`,
    design.md, testing-checklist.md per-CEL-type grid.
  - *"Every site that switches on `CelKind` — show me."*

#### A.6 NodeAnnotation
  - Per-expr-id side-table with 9 fields: `repr`, `field_number`,
    `overload_id`, `local_index`, `scope_id`, `attribute_id`,
    `map_origin`, `list_origin`, `storage`.
  - Vocab: design.md §4.1, mostly.
  - Physical: `compiler/ir/annotations.h`, populated by ResolvePass
    + LayoutPass + visitors; read by codegen.
  - *"Which pass writes each field?  Show every read site."*

#### A.7 Storage / StorageKind / Origin
  - `kStaticRodata` | `kWorkspaceSlot` | `kLocal`; `Origin::kArena`
    | `kHost` | `kDynamic`.
  - The three-path origin dispatch is the load-bearing recurring
    pattern for aggregates.
  - Vocab: design.md §4.1, map-list-dispatch.md.
  - *"Trace a single value's storage through the pipeline."*

#### A.8 OverloadTable / Seeds / Pending exports
  - `OverloadImpl{module, name}` rows keyed by
    `StandardOverloadIds::k…`.
  - `kBuiltinSeeds` array (size grew 80 → 108 → 156 → 158 → 177
    → 235 across milestones).
  - `kPendingRuntimeExports` — the 7-name guard.
  - `kExplicitlyUnimplementedIds`.
  - Vocab: design.md §4.3, milestone docs (e.g. M5.E, M12.F).
  - Physical: `compiler/codegen/overload_table.{h,cc}`,
    `compile.cc::InstallOverloadImports`, the runtime helper
    names in `runtime/cel_runtime.h` + topic headers.
  - *"For built-in `size`, show every overload, every helper, every
    test."*

#### A.9 Host import surface (`cel_host.*`)
  - `cel_get_field`, `cel_has_field`, `cel_set_field`,
    `cel_make_message`, `cel_message_eq`, `cel_map_lookup`,
    `cel_list_at`, `cel_timestamp_tz_accessor`, custom-fn
    `cel_fn.*`, ... etc.
  - Three-layer split: Layer 1 (pure semantics, `HostMessageBacking`
    virtual), Layer 2 (marshalling — `Cel*Impl` + `TrampolineContext`),
    Layer 3 (wasmtime registration).
  - Vocab: design.md §4.7.6.
  - Physical: `eval/internal/cel_host.{h,cc}` + `cel_host_wasmtime.{h,cc}`.
  - *"Add a new host import — show me every site that has to
    grow."*

#### A.10 Runtime helpers (`cel_runtime.*`)
  - `cel_int_add_at_vv`, `cel_string_concat_at_vv`, `cel_and_at_vv`,
    `cel_unknown_merge`, the arena primitives (`arena_init`,
    `arena_alloc`, `arena_reset`), `cel_matches_at_vv`,
    `cel_base64_encode_at_v`, …
  - Uniform `(out_slot, args…) -> void` ABI.
  - Topic-split across `runtime/cel_*.c` (~30 TUs).
  - Vocab: design.md §4.2 + §8.
  - Physical: `runtime/cel_*.{h,c}` + the wasm-export list.
  - *"For helper X, show the wasm export, the native callers, the
    overload-id that routes to it, and the tests."*

#### A.11 `cel.abi` custom section
  - The wire schema between compile-time and load-time.
  - Variables, attributes, fields, memory layout, types,
    `host_custom_imports[]`.
  - Vocab: design.md §7.4, abi-refactor.md.
  - Physical: `abi/cel_abi.proto`, `abi/cel_abi_emit.{h,cc}`,
    `eval/internal/abi_decode.{h,cc}`.
  - *"Tell me what a given expression's ABI section says."*

#### A.12 Activation / AttributePattern / partial eval
  - User-supplied variable bindings + unknown patterns.
  - Vocab: design.md §10.1, langdef.md.
  - Physical: `eval/activation.{h,cc}`, `eval/attribute.{h,cc}`,
    `eval/instance.cc::PartialEval`.
  - *"Trace an unknown attribute through the cel_host
    trampoline."*

#### A.13 WAT trace / WAT-runner
  - Each `wat/NN_*.wat` is the executable spec for one codegen arm
    or host-ABI surface.
  - Vocab: CLAUDE.md, wat-traces.md, every milestone doc that
    cites a WAT.
  - Physical: `doc/.../wat/`, `tools/wat_runner/`.
  - *"Which WAT corresponds to this codegen arm?"*

#### A.14 Conformance row
  - `(fixture, row_name)` from `spec/tests/simple/testdata/*.textproto`.
  - Each is a PASS / SKIP / FAIL.
  - Vocab: `conformance/README.md`, milestone docs ("M12 unlocked
    `string_ext.textproto` 0/216 → 94/216").
  - Physical: harness in `conformance/`.
  - *"Show me every conformance row that landed in M16."*

#### A.15 cleanup-backlog entry
  - Numbered `#1`–`#43`.
  - Status: open or closed.
  - Cites code paths, review dates, "why P2."
  - Vocab: `cleanup-backlog.md`, commit messages.
  - *"Show every open backlog item and the files they affect."*

#### A.16 Manual-tagged target
  - The 10 from `per-component-test-coverage.md §2`.
  - The M2 incident's structural lesson.
  - *"Did all manual targets run for this milestone?"*

#### A.17 cel-cpp citation
  - Pattern: `third_party/cel-cpp/<path>:<line>` references in
    milestone docs and code comments.
  - Vocab: any probe doc, oracle test cases.
  - *"What cel-cpp source does this assertion derive from?"*

#### A.18 The "Plan-vs-execution delta"
  - A doc-level callout pattern marking where as-shipped diverged
    from as-written.
  - Vocab: design.md and every milestone doc.
  - *"Show every delta callout; rank by recency."*

#### A.19 Custom function backend (`@host` vs `@component`)
  - The two custom-fn backends, with the `@native` (CEL-defined,
    inline) variant as a third.
  - Vocab: `modules-and-ffi.md`, m13, m22, m23, m24, m26.
  - Physical: `compiler/celfn/` + `eval/typed_function.{h,cc}` +
    `eval/internal/cel_component.{h,cc}` + `eval/engine.cc`'s
    `AddTypedFunction` / `AddComponent` / `AddForeignComponent`.
  - *"Walk a `@component` fn from `.celfn` IDL to wasm-component
    instance."*

### 5.B Organisational structure (physical units)

Already enumerated in §3.  Each unit maps to one or more
abstractions in §5.A.

### 5.C Bridges — where abstraction ↔ structure DON'T align

This is the most valuable insight; these are the items the tool
should make first-class.

#### B.1 Cross-stage abstractions
*One concept that spans the pipeline spine and lives in many files.*

  - **`NodeAnnotation::storage`** — written in LayoutPass
    (`layout_pass.cc`), read in codegen (`expr_lower*.cc`), audited
    by `expr_lower_test.cc`, asserted in WAT traces.  No single
    "Storage" file owns it.
  - **`overload_id`** — produced by cel-cpp's checker (vendored),
    stamped by `ResolvePass`, looked up in `OverloadTable`, emitted
    as a `call $import`, executed by a `runtime/cel_*.c` body,
    asserted in `e2e/m*_test.cc`.  9 hops, 0 single-file home.
  - **The comprehension accu_var** — touched by ResolvePass scope
    handling, LayoutPass `ComprehensionLocalsVisitor`,
    `expr_lower_comprehension.cc`, the runtime list/map kernels,
    and tests across multiple milestones.  Cleanup-backlog `#31`
    was specifically a missing `Storage` stamp on
    `kComprehensionExpr`'s annotation — *exactly* the kind of
    cross-stage gap a unified abstraction view would surface.
  - **`AttributePattern` / unknown-pattern check** — design.md §4.7.6.3
    locates the check at "step 4" of the trampoline, but the
    actual matching logic is in `eval/attribute.cc`, the
    `attribute_id` is stamped in `resolve_pass.cc`, and the
    activation surface is in `eval/activation.cc` and
    `eval/instance.cc::PartialEval`.

#### B.2 Files that host multiple abstractions
*One physical unit, many concepts.*

  - **`eval/internal/cel_host.{h,cc}`** (most-edited file at one
    point, 33 commits to `compiler_v2/api/internal/cel_host.cc`
    alone) — hosts Layer 1 backings (`ProtoBacking`, `ProtoMap`,
    `ProtoList`), Layer 2 trampoline bodies (`CelGetFieldImpl`,
    `CelHasFieldImpl`, `CelMapLookupImpl`, `CelListAtImpl`,
    `CelSetFieldImpl`, `CelMakeMessageImpl`, `CelMessageEqImpl`,
    plus the timestamp/duration accessors and Any pack/unpack
    helpers), and shared marshal helpers (`EncodeValue`,
    `WriteMessageOrPack`, `UnpackAnyToValue`).  Lint backlog
    flags this file repeatedly for function-size; the
    `m11-cel-host-refactor.md` doc tracks the carving plan.
    **This is the file with the highest abstraction density**;
    the tool needs to surface "which trampoline lives where in
    cel_host.cc?"
  - **`compiler/codegen/expr_lower.cc`** — every codegen arm.
    Split between `expr_lower.cc` and `expr_lower_comprehension.cc`
    today (was split further during m13 customs).
  - **`compiler/internal/compile.cc`** — the
    `Install{Select,Map,List,OverloadImports}` + `EmitCustomFnBodies`
    + ABI emit + module finalize.  The "top-level facade" stage.
  - **`eval/engine.cc`** — the two-phase Plan (linker setup,
    runtime instantiation, expr instantiation, ABI decode), plus
    every host-import binding site, plus `AddTypedFunction` /
    `AddComponent` / `AddForeignComponent`.

#### B.3 Abstractions with no clean physical home
*Live in nobody's head until something breaks.*

  - **The lazy-arena invariant** — surfaced by cleanup-backlog
    `#35`-area discussion.  Not in any header.
  - **Aliasing safety of `_at_vv` helpers** — design.md §8.4
    names it; the test is in `runtime/cel_runtime_wasm_test.cc`
    but the *invariant statement* lives only in a comment in
    `cel_runtime.h`.
  - **The "skip is a live TODO" convention** — CLAUDE.md
    enforces it; no compile-time check.  The M2 incident is
    direct evidence that informal enforcement fails.
  - **The "single source of truth for runtime exports"** — the
    ABI catalogue + `wasm_exports.txt` + `BindAllRuntimeExports`
    + `OverloadTable` seeds should be one closed loop, validated
    by `runtime_catalogue_consistency_test`.  But the
    relationship between the four sites is described in design.md
    §9.1 prose only — there is no diagram or graphviz of the
    actual dataflow.

#### B.4 Implicit ordering and dependency
  - Milestone slice graph (S1..S12 from design.md §11.4) — a doc
    diagram, not a structured artefact.
  - "M2 prereq for M5" — captured only in milestone-doc prose.
  - The Phase A/B/C wasi migration — captured in `wasi/DESIGN.md`
    and Phase-C-prefixed deltas in design.md.  The *as-shipped*
    Phase-C delta callouts in design.md are how a reader knows
    the doc is honest.

---

## 6. Phase 6 — design of the tool

### 6.1 Core navigation primitives

The smallest set of operations that compose into the most useful
navigation.  Six primitives:

  1. **`abstraction → sites`** — given any A.1..A.19, return every
     file:line where it's *defined*, *read*, *tested*, or
     *documented*, with type tags on each.
  2. **`site → abstractions`** — given any file:line, return the
     abstractions that touch it (with intensity weighting: an arm
     of a `switch` on `CelKind` "touches" CelKind heavily; a
     comment mentioning it touches it lightly).
  3. **`abstraction → cross-stage view`** — given an abstraction
     that crosses pipeline stages (B.1), render a 13-cell row
     (one per stage) showing where in each stage it lives.
  4. **`milestone → state`** — given an `mNN`, return: status,
     slices (with each slice's three-layer state), tests, e2e
     status, conformance unlock, and open backlog items it
     surfaced or closed.
  5. **`backlog # → trail`** — given `#N`, return: surfacing
     event (review doc + date), affected files, commits citing
     the entry, current open/closed state, related entries.
  6. **`conformance row → owner`** — given a row, return: the
     fixture, the current outcome, the SKIP category if any, the
     milestone that last touched the area, the related backlog
     items.

Compose these and you can answer almost every question the
project's current `grep`-based loop is used for.

### 6.2 Surfaces (views)

Six concrete surfaces, in build-priority order:

#### V1.  Milestone burndown board
  - **Who uses it:** the project owner during planning and review.
  - **Question it answers:** *"What's shipped?  What's in flight?
    What's drafted but stale?  Which sibling docs are stale?"*
  - **Data it consumes:** all `mNN-*.md` Status headers (live
    parse); git log dates; the slice graph in design.md §11.4;
    backlog open/closed split.
  - **Actions it enables:** click a milestone → milestone-detail
    view (V2); click the Status header → propose an updated header
    based on git evidence ("last commit citing `m26` was
    `08cad216` on 2026-06-04, doc says `in flight — drafted
    2026-05-21`; mark shipped?").

#### V2.  Milestone detail view
  - **Question:** *"What did M14 actually do?  Which tests pin
    it?  What gaps are still open?"*
  - **Data:** the milestone doc itself; the three-layer table if
    present; `e2e/mN_test.cc` row count + pass/skip/fail; commits
    matching `mN:` or with the milestone in their subject;
    backlog items surfaced + closed by the milestone (by date
    proximity and by direct citation).
  - **Actions:** open any cited file; open any cited backlog
    item; pivot to abstraction views for `NodeAnnotation` fields
    the milestone added; cross-link to the WAT traces it
    introduced.

#### V3.  Pipeline coverage matrix
  - **Question:** *"For CEL type X at pipeline stage Y, do we
    have coverage?  If yes, which test?  If no, why not (deferred
    / blocked / actually missing)?"*
  - **Data:** the two grids in `testing-checklist.md` (rendered
    live from the markdown — these are already ASCII tables), the
    13 pipeline stages, the test files that mention each cell,
    the manual-tagged target catalog.
  - **Actions:** click a cell → list of `_test.cc` files
    asserting it.  Click a "[ ]" → propose a test stub.  Filter
    by "what's covered today by manual-tagged targets only" —
    this is the M2-incident-prevention view.

#### V4.  Abstraction explorer
  - **Question:** *"Where does the `OverloadTable` live?  Show
    every seed, every site that reads it, every test that
    asserts something about it."*
  - **Data:** the abstraction catalog (§5.A); a static-analysis
    layer (clangd / ctags / regex) that pins each abstraction's
    symbol(s) and finds references via `compile_commands.json`.
  - **Actions:** open any reference; pivot to the milestone that
    most recently grew the abstraction (per git blame on the
    relevant file:lines); pivot to the WAT trace that locked the
    ABI (if there is one).

#### V5.  Backlog & review trail
  - **Question:** *"What's the trail behind cleanup-backlog #20?
    What review surfaced it, what commits closed it, what
    follow-ups did closing produce?"*
  - **Data:** `cleanup-backlog.md` (parse # N, status, file
    cites, dates); git log `--grep '#NN'`; review docs in the
    three `reviews/` dirs (parse for `#N` mentions); CLAUDE.md
    rule references.
  - **Actions:** open the surfacing review doc; jump to the
    closing commit; *propose* a new entry (template-driven) from
    the current review's findings.

#### V6.  Conformance burndown
  - **Question:** *"Which conformance rows changed last week?
    What FAILs remain, and what's blocking each?"*
  - **Data:** `conformance/README.md` auto-gen headline + the
    per-fixture summary; (more expensively) the per-row results
    from a fresh `run_conformance` invocation; commit messages
    with `pass=NNNN` deltas; SKIP-category catalog from
    `conformance/runner.h`.
  - **Actions:** drill into a fixture; for any FAIL row, locate
    the related backlog item if any (string-match `extension`,
    `wrapper`, `Any`, …); for any SKIP row, locate the gate
    (`disable_check`, `type_env`, `dyn`, `unimplemented_extension`).

#### V7 (stretch).  WAT trace gallery
  - **Question:** *"Show me every WAT, grouped by family.  Which
    codegen arm is each one's spec?  Which trace is stale?"*
  - **Data:** filesystem under `wat/`, the walkthrough text in
    `wat-traces.md`, the `wat_runner_test.cc` results, the
    "SUPERSEDED memory map" callout in wat-traces.md.

#### V8 (stretch).  Pipeline-spine flow view
  - One row per pipeline stage; one column per recent slice.
    Cells light up green/yellow/red based on whether the slice
    touched the stage and whether the stage's tests are green.
  - This is the "feature-pipeline-checklist live" view — the
    forcing function the doc currently asks humans to apply.

### 6.3 Data model (entity kinds)

12 entity kinds, sufficient for V1–V6:

  1. **Doc** — `(path, layer L0..L5, title, status_header?, last_commit_sha,
     last_mod_date, mentions[])`.
  2. **Milestone** — `(slug "mNN", numeric_id, name, status,
     drafted_date, shipped_date?, primary_doc, slices[],
     supersedes[])`.
  3. **Slice** — `(milestone, slug "M5.D.step2", status, scope,
     primary_commit_sha?)`.
  4. **Abstraction** — `(slug, kind A.1..A.19, vocab_sites[],
     symbol(s)?, description)`.
  5. **File** — `(path, role_dir, language, sloc, is_test,
     edit_count, last_commit, abstractions_present[])`.
  6. **Symbol** — `(file, line, name, kind class|fn|enum|var,
     visibility public|internal, sloc?)`.  Derived from
     compile_commands + ctags/clangd.
  7. **Commit** — `(sha, author, date, subject, body, files[],
     prefix, milestone_refs[], backlog_refs[], pr?)`.
  8. **BacklogEntry** — `(num, status open|closed, surfacing_event,
     description, affected_files[], cites[], closed_by_sha?,
     closed_date?)`.
  9. **TestingChecklistRow** — `(grid_name, row_label, col_label,
     state [ ]|[x]|n/a, test_file?, milestone_credit?)`.
  10. **ConformanceRow** — `(fixture, row_name, outcome
      pass|skip|fail, skip_category?, last_outcome_change_sha?,
      blocking_backlog?)`.
  11. **ManualTarget** — `(target_label, what_it_covers,
      last_run_outcome?, last_run_sha?)`.
  12. **WatTrace** — `(file, number, family, source_expr,
      walkthrough_section_in_wat_traces_md, runner_test_status,
      superseded_by?)`.

Edges between entities are the navigation primitives in §6.1.

### 6.4 Integration points (what the tool wraps vs. reimplements)

  - **Wraps, doesn't reimplement:**
    - **git** — shell out to `git log --porcelain`, `git show`,
      `git blame`.  Don't reimplement Git plumbing.
    - **Bazel** — `bazel query` JSON output; cache aggressively.
    - **clangd / ctags** — defer-to.  The tool indexes symbols
      *only* for cross-link rendering, not for refactoring.
    - **conformance runner** — invoke as-is; parse stdout.
    - **`scripts/run_full_suite.sh`** — invoke as-is.
    - **`tools/wat_runner`** — invoke as-is.
    - **`testdata/cel_cpp_oracle_test.cc`** — invoke as-is for
      "ask the oracle" actions.
  - **Reimplements:**
    - **Markdown structural parse** of the planning docs (Status
      headers, mNN refs, `cleanup-backlog #NN` cites, `[ ]`/`[x]`
      checkboxes, "Plan-vs-execution delta" callouts).  This is
      pure regex + a small AST and is the *uniquely valuable*
      capability.
    - **The cross-link graph** itself — there is no off-the-shelf
      tool that knows "checklist row R cites file F that
      implements abstraction A."

### 6.5 Tech-stack recommendation — motivated

The tool surfaces (V1–V6) are dominated by **tabular cross-reference
rendering** with **drill-down**.  V3 (the coverage matrix) is a
literal 2D grid; V1 (milestone board) is a sortable list with status
badges; V2 (milestone detail) is a stack of small tables; V5
(backlog & trail) is a master-detail; V6 (conformance) is the
same.  There is **no** heavy graphical workload (no timeline charts,
no rich code editor, no canvas drawing).  Most cells are short text
or a small status indicator.

That points to a **server-rendered local web app** with:

  - **A small Python ingest** (`mistune` / `marko` for markdown,
    `pygit2` for git, `subprocess` for bazel/conformance), running
    as a long-lived `uvicorn` server.  Python wins here because
    (a) every read-side action is I/O bound and (b) the *uniquely
    valuable* code is markdown parsing + cross-graph construction,
    which Python expresses concisely.  Per project policy, use
    `ruff` + type hints + `pytest`.
  - **SQLite as the cache** — entity tables keyed by git SHA,
    invalidated on `git pull`.  The 12 entity kinds → ~12 tables
    + a few link tables.  No ORM beyond `sqlalchemy core`.
  - **`htmx`-driven server-rendered HTML** with **Tailwind** for
    styling.  Reasons:
      * The data is server-side already; round-tripping JSON to a
        SPA buys nothing.
      * `htmx` collapses the master-detail / drill-down pattern
        into one server-side rendering function per panel.
      * Tailwind matches the "tabular dense" aesthetic the doc
        layer already cultivates (the testing-checklist grids,
        the M13 three-layer table).
      * Zero `node_modules` (the only one we'd otherwise need is
        for the existing `bindings/ts/` work, which is unrelated).
  - **No persistent server / no auth** — the tool runs `localhost`,
    bound to the user's checkout.  It is read-mostly; the only
    writes (tick a checkbox, append a backlog entry) compose into
    a `git diff` the user reviews and commits manually.
  - **A single CLI entry point** at `tools/celwasm-tool/celwasm-tool`
    (`python -m celwasm_tool`).  No daemon, no global install — the
    binary lives in the repo it's serving.

Defending the choice against alternatives the project's own
practice biases toward:

  - **C++ + Bazel target?** The project's C++ is reserved for code
    that runs *under wasm or under wasmtime* — anything else
    burns the lint/format/test cost CLAUDE.md tracks (61 edits to
    testing-checklist.md alone).  A read-mostly local tool gains
    nothing from being C++; it loses iteration speed.
  - **Go?** Tempting (`bindings/` is going to need it eventually),
    but Go's markdown ecosystem + structural parser story is
    weaker than Python's, and the tool's hot path is markdown.
  - **Rust?** Same as Go, plus the iteration cost is wrong for a
    project where the user explicitly favours fast read loops.
  - **Pure shell + `gum` / `fzf`?** Works for V5 alone (backlog
    trail), but the cross-grid render of V3 and the entity-graph
    walks of V4 are too structural for shell pipelines.

### 6.6 The "first interactive experiment"

**V5 — the backlog & review trail.**

Why this first:

  - **Smallest data surface.**  `cleanup-backlog.md` is one file;
    `git log --grep` against it is sub-second; the three
    `reviews/` dirs together are <20 files.
  - **Highest signal per unit work.**  44 numbered entries today,
    each is a question the user reaches for; today the answer is
    "grep, read, scroll."
  - **Validates the markdown-parse-and-cross-link engine in
    isolation** — if V5 works, V1/V2/V6 are the same machinery
    pointed at a different file set.
  - **Forces the schema choice early.**  V5 exercises
    `BacklogEntry` ↔ `Commit` ↔ `Doc` ↔ `File` — 4 of the 12
    entity kinds — and any schema wart shows up in the first
    week of use, not after the harder views ship.
  - **Bypasses the expensive sources.**  No `bazel query`, no
    conformance run, no clangd.  Just `git` + filesystem.

Concretely: a single page that loads in <200 ms showing all 44
entries, filterable by `open|closed`, sortable by surface-date.
Click an entry → a detail panel showing the surfacing review (if
any), the affected files (live-clickable), every commit that cites
the entry, and the "why P2" rationale.  *No* writes in V1 of V5;
writes ("propose a new backlog entry") are V2 of V5.

If V5 takes a week to ship and is in active daily use by the user
after the second week, V1–V4 are justified.  If V5 stalls or sits
unused, the abstraction model is wrong and we redesign.

---

## 7. Open questions for the user

  1. **Scope of "writes."**  Should the tool ever commit on the
     user's behalf?  The `MEMORY.md` `feedback_no_unprompted_commits`
     bullet says never; but V3 (checklist matrix) is much more
     useful if ticking a box composes into a one-line commit.
     Suggest: never commits.  Always emits a `git diff` the user
     reviews.
  2. **Snapshot vs. on-demand for conformance.**  V6 is most
     valuable with fresh per-row data, but `run_conformance` is
     a 5-minute job.  Acceptable defaults: snapshot at git SHA,
     button to refresh.  Or: hook into a pre-existing run, never
     trigger one.  Which is preferred?
  3. **Abstraction symbol indexing.**  V4 needs clangd-style
     symbol info.  Two options: (a) shell out to `clangd
     --background-index` and read the cdb; (b) regex+ctags for
     the dominant ~80% of symbols and fall back to grep.
     `compile_commands.json` is already in the repo, so (a) is
     feasible — but it's expensive.  Recommendation: regex+ctags
     for V1, upgrade only if V4 demands it.
  4. **Hosting.**  Local only?  Or eventually a deployment so
     reviewers can see V1/V5/V6 without checking out the repo?
     Affects whether SQLite or Postgres is right.  Recommendation:
     local only.  When it's not, the design is wrong.
  5. **Where do the planning-doc abstractions live in code?**
     §5.A names ~19 abstractions, but only ~10 have a clean
     symbol (`OverloadTable`, `Storage`, `Origin`, etc.).  The
     rest (the "pipeline spine," the "manual-tagged-target
     contract," the "Plan-vs-execution delta" notion) are
     doc-only.  Does the user want the tool to enforce a
     consistent vocabulary — e.g. canonical names in a small
     YAML file — or infer it from doc text?  Recommendation: a
     small `tools/celwasm-tool/abstractions.yaml` (canonical
     name, vocabulary sites, symbol(s) if any) that the tool
     reads at start.  Cheap to maintain by hand; reviewed like
     any other doc change.
  6. **Renaming history.**  `compiler_v2/` → role-dir migration
     means git history bifurcates per file.  The tool should
     `git log --follow` everywhere it shows "edit count" —
     confirm this is the desired behaviour, or treat
     `compiler_v2/` as a separate epoch?
  7. **Integration with the agent loop.**  `Skill`s like
     `code-review`, `verify`, `init`, `review`, `security-review`
     already exist.  Should the tool *expose* its data to them
     (e.g. an HTTP endpoint that lists "what backlog items are
     in scope for this PR" that the review skill consumes)?
     Recommendation: yes, but later — V1 of the tool stops at
     the read views.

---

*End of design.*
